#include "afe_pipeline.h"
#include "aec_config.h"
#include "afe_board.h"
#include "playback.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

// esp-sr 1.9 AFE API: use AFE_CONFIG_DEFAULT() + field overrides, then
// esp_afe_vc_v1 (voice-communication interface = AEC+NS+VAD for full-duplex).
// No afe_config_init / esp_afe_handle_from_config — those were a pre-release API.
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"

static const char *TAG = "afe";

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t  *s_afe_data;
static afe_audio_cb_t      s_audio_cb;
static int s_feed_chunksize;      // samples per channel per feed
static int s_feed_nch;            // total channels fed (== strlen(input_format))
static int s_ref_index = -1;      // index of the 'R' channel, for software ref

// ERLE measurement accumulators (sum of squares + sample counts).
static volatile bool   s_metrics_on = false;
static volatile double s_acc_mic = 0, s_acc_ref = 0, s_acc_clean = 0;
static volatile uint32_t s_n_in = 0, s_n_out = 0;

// Raw / clean capture (see afe_capture_begin).
static int16_t *s_cap_mic, *s_cap_ref, *s_cap_clean;
static volatile size_t s_cap_cap, s_cap_i_in, s_cap_i_out;
static volatile bool s_cap_on = false;

#if CONFIG_AEC_SOFTWARE_REF
static StreamBufferHandle_t s_ref_ring;   // mono int16 reference samples
#endif

// ---------------------------------------------------------------------------
// Feed task: ES7210 TDM -> AFE feed
// ---------------------------------------------------------------------------
static void feed_task(void *arg)
{
    (void)arg;
    const size_t frame_bytes = (size_t)s_feed_chunksize * s_feed_nch * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(frame_bytes);
    assert(buf);

    uint32_t fed = 0;
    int64_t last_log_us = 0;
    for (;;) {
        size_t got = 0;
        if (board_mic_read(buf, frame_bytes, &got) != ESP_OK || got == 0) {
            if ((fed & 0x3f) == 0) ESP_LOGW(TAG, "feed: mic_read returned nothing (got=%u)", (unsigned)got);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (fed == 0) {
            int32_t acc = 0;
            for (int i = 0; i < s_feed_chunksize * s_feed_nch; i++) acc += buf[i] < 0 ? -buf[i] : buf[i];
            ESP_LOGI(TAG, "feed: first frame ok bytes=%u mean_abs=%ld", (unsigned)got, (long)(acc / (s_feed_chunksize * s_feed_nch)));
        }
        fed++;
        if (s_cap_on && s_cap_i_in < s_cap_cap) {
            for (int f = 0; f < s_feed_chunksize && s_cap_i_in < s_cap_cap; f++) {
                s_cap_mic[s_cap_i_in] = buf[f * s_feed_nch + 0];
                s_cap_ref[s_cap_i_in] = s_feed_nch > 1 ? buf[f * s_feed_nch + 1] : 0;
                s_cap_i_in++;
            }
        }
        if (s_metrics_on) {
            double am = 0, ar = 0;
            for (int f = 0; f < s_feed_chunksize; f++) {
                double m = buf[f * s_feed_nch + 0];
                double r = s_feed_nch > 1 ? buf[f * s_feed_nch + 1] : 0.0;
                am += m * m; ar += r * r;
            }
            s_acc_mic += am; s_acc_ref += ar; s_n_in += s_feed_chunksize;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 3000000) {
            int32_t a0 = 0, a1 = 0;
            for (int f = 0; f < s_feed_chunksize; f++) {
                int16_t m = buf[f * s_feed_nch + 0];
                int16_t r = s_feed_nch > 1 ? buf[f * s_feed_nch + 1] : 0;
                a0 += m < 0 ? -m : m;
                a1 += r < 0 ? -r : r;
            }
            ESP_LOGI(TAG, "feed: %lu frames, last mic|abs=%ld ref|abs=%ld", (unsigned long)fed,
                     (long)(a0 / s_feed_chunksize), (long)(a1 / s_feed_chunksize));
            last_log_us = now;
        }
#if CONFIG_AEC_SOFTWARE_REF
        // Overwrite the reference channel with what we just played, so the AFE
        // AEC has a far-end signal even without a hardware loopback. Best-effort
        // alignment; a hardware reference (ES7210 loopback) is more accurate.
        if (s_ref_index >= 0 && s_ref_ring) {
            for (int f = 0; f < s_feed_chunksize; f++) {
                int16_t r = 0;
                xStreamBufferReceive(s_ref_ring, &r, sizeof(r), 0);
                buf[f * s_feed_nch + s_ref_index] = r;
            }
        }
#endif
        s_afe->feed(s_afe_data, buf);
    }
}

// ---------------------------------------------------------------------------
// Fetch task: AFE fetch -> clean audio uplink + local barge-in
// ---------------------------------------------------------------------------
static void fetch_task(void *arg)
{
    (void)arg;
    int speech_run = 0;
    bool ducked = false;

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // 1) Uplink the clean (echo-cancelled) audio — never gated by playback.
        {
            static uint32_t s_fetched = 0;
            static int64_t s_fetch_log_us = 0;
            s_fetched++;
            if (s_cap_on && res->data && res->data_size > 0 && s_cap_i_out < s_cap_cap) {
                int n = res->data_size / 2;
                for (int i = 0; i < n && s_cap_i_out < s_cap_cap; i++)
                    s_cap_clean[s_cap_i_out++] = res->data[i];
            }
            if (s_metrics_on && res->data && res->data_size > 0) {
                double ac = 0; int n = res->data_size / 2;
                for (int i = 0; i < n; i++) { double v = res->data[i]; ac += v * v; }
                s_acc_clean += ac; s_n_out += n;
            }
            if (s_fetched == 1) {
                ESP_LOGI(TAG, "fetch: first result data=%p size=%d vad=%d",
                         (void *)res->data, res->data_size, (int)res->vad_state);
            }
            int64_t now = esp_timer_get_time();
            if (now - s_fetch_log_us > 3000000) {
                int32_t acc = 0;
                int n = res->data_size / 2;
                for (int i = 0; i < n; i++) acc += res->data[i] < 0 ? -res->data[i] : res->data[i];
                ESP_LOGI(TAG, "fetch: %lu results size=%d clean|abs=%ld vad=%d",
                         (unsigned long)s_fetched, res->data_size,
                         (long)(n ? acc / n : 0), (int)res->vad_state);
                s_fetch_log_us = now;
            }
        }
        if (s_audio_cb && res->data && res->data_size > 0) {
            s_audio_cb(res->data, (size_t)res->data_size);
        }

        // 2) Local (level-1) barge-in: while TTS plays and past the onset grace,
        //    a sustained run of AFE speech frames means the user is talking over
        //    the robot -> duck immediately. The cloud VAD then confirms and sends
        //    tts_cancel for the hard kill (handled in ws_client). Recover if the
        //    speech run breaks before a kill arrives.
#if CONFIG_AEC_LOCAL_BARGEIN
        bool playing = playback_is_playing();
        bool past_grace = playback_turn_age_ms() > CONFIG_AEC_BARGEIN_ONSET_GRACE_MS;
        if (playing && past_grace && res->vad_state == VAD_SPEECH) {
            if (++speech_run >= CONFIG_AEC_BARGEIN_SPEECH_FRAMES && !ducked) {
                ESP_LOGI(TAG, "local barge-in -> duck");
                playback_duck();
                ducked = true;
            }
        } else {
            speech_run = 0;
            if (ducked && !playing) { ducked = false; }
            else if (ducked && res->vad_state != VAD_SPEECH) {
                playback_unduck();   // false alarm (cough/door) -> restore volume
                ducked = false;
            }
        }
#endif
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t afe_pipeline_init(afe_audio_cb_t on_clean_audio)
{
    s_audio_cb = on_clean_audio;

    const char *fmt = CONFIG_AEC_AFE_INPUT_FORMAT;   // e.g. "MMNR"
    int total_ch = (int)strlen(fmt);
    int mic_num = 0, ref_num = 0;
    for (int i = 0; fmt[i]; i++) {
        if      (fmt[i] == 'M' || fmt[i] == 'm') mic_num++;
        else if (fmt[i] == 'R' || fmt[i] == 'r') { ref_num++; s_ref_index = i; }
    }

    (void)total_ch; (void)mic_num; (void)ref_num;

    // esp-sr 2.x: afe_config_init(input_format, models, type, mode). AFE_TYPE_VC
    // = voice-communication front end (AEC + nonlinear NS + VAD, 16 kHz, no wake
    // word). No model partition needed for this path -> models = NULL.
    afe_config_t *cfg = afe_config_init(fmt, NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) { ESP_LOGE(TAG, "afe_config_init failed"); return ESP_FAIL; }
    cfg->wakenet_init = false;
    cfg->agc_init     = false;
    cfg->vad_init     = true;
    cfg->vad_mode     = VAD_MODE_3;
    cfg->pcm_config.sample_rate = AEC_SAMPLE_RATE_HZ;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#ifdef CONFIG_AEC_ENABLE
    cfg->aec_init = true;
#else
    cfg->aec_init = false;   // pass-through: raw mic upstream, for A/B echo measurement
#endif
    afe_config_check(cfg);
    // Dump what the AFE actually ended up with -- afe_config_check() silently
    // rewrites conflicting fields, and the AEC mode / filter length it picks
    // decides how much echo we can cancel.
    afe_config_print(cfg);

    s_afe = esp_afe_handle_from_config(cfg);
    if (!s_afe) { ESP_LOGE(TAG, "esp_afe_handle_from_config failed"); afe_config_free(cfg); return ESP_FAIL; }
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) { ESP_LOGE(TAG, "AFE create failed"); return ESP_FAIL; }

    s_feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_nch       = s_afe->get_feed_channel_num(s_afe_data);
    ESP_LOGI(TAG, "AFE ready: fmt=\"%s\" feed_chunk=%d nch=%d ref_ch=%d aec=on",
             fmt, s_feed_chunksize, s_feed_nch, s_ref_index);

#if CONFIG_AEC_SOFTWARE_REF
    s_ref_ring = xStreamBufferCreate(s_feed_chunksize * 8 * sizeof(int16_t), sizeof(int16_t));
    ESP_LOGW(TAG, "software AEC reference enabled (hardware loopback preferred)");
#endif

    // Feed on core 1, fetch on core 0 alongside WiFi/WS — balances the load.
    xTaskCreatePinnedToCore(feed_task,  "afe_feed",  6144, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 8192, NULL, 5, NULL, 0);
    return ESP_OK;
}

void afe_pipeline_push_ref(const int16_t *pcm, size_t samples)
{
#if CONFIG_AEC_SOFTWARE_REF
    if (s_ref_ring) xStreamBufferSend(s_ref_ring, pcm, samples * sizeof(int16_t), 0);
#else
    (void)pcm; (void)samples;
#endif
}

void afe_metrics_begin(void)
{
    s_acc_mic = s_acc_ref = s_acc_clean = 0;
    s_n_in = s_n_out = 0;
    s_metrics_on = true;
}

void afe_metrics_end(float *mic_rms, float *ref_rms, float *clean_rms)
{
    s_metrics_on = false;
    uint32_t ni = s_n_in, no = s_n_out;
    if (mic_rms)   *mic_rms   = ni ? (float)sqrt(s_acc_mic / ni) : 0.0f;
    if (ref_rms)   *ref_rms   = ni ? (float)sqrt(s_acc_ref / ni) : 0.0f;
    if (clean_rms) *clean_rms = no ? (float)sqrt(s_acc_clean / no) : 0.0f;
}

void afe_capture_begin(int16_t *mic, int16_t *ref, int16_t *clean, size_t cap_samples)
{
    s_cap_mic = mic; s_cap_ref = ref; s_cap_clean = clean;
    s_cap_cap = cap_samples;
    s_cap_i_in = s_cap_i_out = 0;
    s_cap_on = true;
}

void afe_capture_end(size_t *mic_n, size_t *ref_n, size_t *clean_n)
{
    s_cap_on = false;
    if (mic_n)   *mic_n   = s_cap_i_in;
    if (ref_n)   *ref_n   = s_cap_i_in;
    if (clean_n) *clean_n = s_cap_i_out;
}
