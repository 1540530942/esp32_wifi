#include "afe_pipeline.h"
#include "aec_config.h"
#include "afe_board.h"
#include "playback.h"

#include <stdio.h>
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

static char s_cfg_summary[160] = "(afe not initialised)";
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

// --- E4 click onset detection (see afe_pipeline.h) --------------------------
// Runs on the RAW mic channel inside the feed task, i.e. before the AFE, so the
// measured onset is not itself delayed by AEC/NS/VAD -- otherwise the latency
// under test would be partly subtracted from the measurement.
static volatile bool    s_click_armed = false;
static volatile int64_t s_click_us = 0;   // 0 = nothing detected since arming
static volatile int64_t s_click_duck_us = 0;
static volatile int     s_click_latency_ms = -1;
// The quantity tested is the RATIO ch0/ch1 against its own slow envelope --
// not either channel's level. Two hardware runs established why, and both
// rejected formulations are worth naming so they are not tried again:
//
//   v48: "ch0 clears its own envelope by +12 dB". Fired within 280 ms on the
//        robot's own speech. Speech is strongly non-stationary; a syllable
//        onset clears a 320 ms envelope exactly as easily as a click does.
//   v49: "...and reject the frame if ch1 also jumped". Also fired. During
//        playback ch1 is loud so its envelope is high, and a moderate ch1 rise
//        still sits within +6 dB of it -- the gate was true almost always.
//
// What works: the echo path has a roughly fixed gain G, so peak0 ~= G *
// peak_ref while the robot is the only source. Its onsets raise BOTH channels
// and leave the ratio flat; an external click raises only the numerator and the
// ratio jumps. ch1 can carry this because it is an electrical tap on the
// amplifier output rather than an acoustic pickup (P1 measured it at -84 dBFS
// with nothing playing while ch0 still saw -52 dBFS of room noise).
//
// It also degrades gracefully: with nothing playing peak_ref ~= 0, the
// denominator floors at 1 and the test becomes the plain ch0-level one, so
// there is no separate playing/idle code path.
// --- Early barge-in on the raw mic (pre-AFE) --------------------------------
// E4 measured 305 and 319 ms against a 200 ms budget. The breakdown: ~10 ms
// from click to speech onset, 64 ms for the four sustained VAD frames, 24 ms to
// stop the output (T3.2, measured) -- and roughly 210 ms inside the AFE before
// its VAD ever reports speech. The task list budgeted 50 ms for "AFE + VAD",
// so that single term is the whole overrun, and no amount of tuning the frame
// count recovers it.
//
// The click detector already answers the question barge-in needs, one frame
// after the sound arrives: ch0 rose without ch1 rising, so the source is not
// the robot. Requiring that for a few consecutive frames turns a transient
// detector into a speech-onset detector, and it runs in the feed task, before
// the AFE, so none of that 210 ms is in the path.
//
// The AFE VAD path stays as it was. This one only ever fires earlier, and a
// door slam still has to clear the same sustained requirement.
static uint32_t s_early_run = 0;
static bool s_early_fired = false;
static uint32_t s_early_frames = 0;
static float s_click_env_ratio = 0.0f;
static int16_t s_click_prev_ref = 0;   // previous frame's ch1 peak (see below)
static uint32_t s_click_frames = 0;
#define CLICK_ENV_ALPHA      0.05f   // ~20 frames (~320 ms) time constant
#define CLICK_TRIGGER_RATIO  (CONFIG_AEC_CLICK_TRIGGER_RATIO_X10 / 10.0f)
#define CLICK_MIN_PEAK       2000    // absolute floor, keeps silence from firing
#define CLICK_SEED_FRAMES    16      // let the envelope settle before arming fires

void afe_click_arm(void)
{
    s_click_us = 0;
    s_click_duck_us = 0;
    s_click_latency_ms = -1;
    s_click_env_ratio = 0.0f;
    s_click_prev_ref = 0;
    s_click_frames = 0;
    s_click_armed = true;
    ESP_LOGI(TAG, "click detector armed at t=%lld us", (long long)esp_timer_get_time());
}

bool afe_click_result(int64_t *click_us, int64_t *duck_us, int *latency_ms)
{
    if (click_us) *click_us = s_click_us;
    if (duck_us) *duck_us = s_click_duck_us;
    if (latency_ms) *latency_ms = s_click_latency_ms;
    return s_click_us != 0;
}

// Scans one feed frame for the first sample crossing the trigger, and converts
// its position inside the frame back to a timestamp. Without the sub-frame
// offset the measurement would quantise to the feed period (~16 ms at 16 kHz),
// which is a tenth of the 200 ms budget under test.
static void click_scan(const int16_t *buf, int chunk, int nch, int64_t frame_end_us)
{
    int16_t peak = 0, peak_ref = 0;
    for (int f = 0; f < chunk; f++) {
        int16_t v = buf[f * nch + 0];
        int16_t a = v < 0 ? (int16_t)-v : v;
        if (a > peak) peak = a;
        if (nch > 1) {
            int16_t r = buf[f * nch + 1];
            int16_t ra = r < 0 ? (int16_t)-r : r;
            if (ra > peak_ref) peak_ref = ra;
        }
    }

    // ch1 is an electrical tap and leads the acoustic path by the speaker->mic
    // flight time, so a robot onset near a frame boundary can land on ch1 in
    // frame N and on ch0 in frame N+1. Taking the larger of this frame's and the
    // previous frame's ch1 peak covers that skew; without it, every syllable
    // that straddles a boundary looks exactly like a click.
    const int16_t ref_win = peak_ref > s_click_prev_ref ? peak_ref : s_click_prev_ref;
    const float ratio = (float)peak / ((float)ref_win + 1.0f);

    bool fire = s_click_frames >= CLICK_SEED_FRAMES &&
                peak > CLICK_MIN_PEAK &&
                ratio > s_click_env_ratio * CLICK_TRIGGER_RATIO;

    if (fire && s_click_us == 0) {
        // Locate the crossing inside the frame. The per-sample threshold is the
        // ch0 level that the reference window would have had to produce, scaled
        // by the ratio envelope -- i.e. the level at which this sample stops
        // being explainable as echo.
        const float sample_thr = s_click_env_ratio * CLICK_TRIGGER_RATIO * ((float)ref_win + 1.0f);
        int first = 0;
        for (int f = 0; f < chunk; f++) {
            int16_t v = buf[f * nch + 0];
            int16_t a = v < 0 ? (int16_t)-v : v;
            if ((float)a > sample_thr && a > CLICK_MIN_PEAK) { first = f; break; }
        }
        // frame_end_us is when the whole frame had been read, so the sample at
        // index `first` happened (chunk - first) samples earlier.
        s_click_us = frame_end_us - (int64_t)(chunk - first) * 1000000 / 16000;
        s_click_armed = false;
        ESP_LOGI(TAG, "click onset at t=%lld us (peak=%d ref_win=%d ratio=%.2f env_ratio=%.2f)",
                 (long long)s_click_us, (int)peak, (int)ref_win,
                 (double)ratio, (double)s_click_env_ratio);
        return;
    }

    s_click_env_ratio = s_click_env_ratio * (1.0f - CLICK_ENV_ALPHA) + ratio * CLICK_ENV_ALPHA;
    s_click_prev_ref = peak_ref;
    s_click_frames++;
}

#if CONFIG_AEC_LOCAL_BARGEIN
// Same test as click_scan's, run on every frame rather than only while armed,
// and requiring several frames in a row so a single transient is not enough.
static void early_bargein_scan(const int16_t *buf, int chunk, int nch)
{
    if (nch < 2) return;              // no reference channel, no discriminator
    int16_t peak = 0, peak_ref = 0;
    for (int f = 0; f < chunk; f++) {
        int16_t v = buf[f * nch + 0];
        int16_t a = v < 0 ? (int16_t)-v : v;
        if (a > peak) peak = a;
        int16_t r = buf[f * nch + 1];
        int16_t ra = r < 0 ? (int16_t)-r : r;
        if (ra > peak_ref) peak_ref = ra;
    }
    const bool playing = playback_is_playing();
    const bool past_grace = playback_turn_age_ms() > CONFIG_AEC_BARGEIN_ONSET_GRACE_MS;
    if (!playing) {
        // Idle: ch1 is silent so the ratio is meaningless (peak0 over ~1).
        // Letting that into the envelope would poison the next utterance.
        s_early_run = 0;
        s_early_fired = false;
        s_early_frames = 0;
        return;
    }

    // Compare the ch0/ch1 RATIO against its own running value, not ch0 against
    // a fixed multiple of ch1. The first version did the latter and only fired
    // when the robot happened to be between syllables: while it is speaking
    // loudly peak_ref reaches several thousand, so "ch0 > 4 x ch1" demands a
    // ch0 beyond full scale and can never be true. Measured spread was 30 ms
    // when it caught a gap and 289 ms when it did not and the AFE path had to
    // carry it.
    //
    // The echo path has a roughly fixed gain, so that running ratio IS the
    // gain; an external talker is what makes the ratio jump above it. Same
    // reasoning as click_scan, which got this right -- this is the third time
    // in this work that substituting an absolute threshold for the relation
    // between the two channels has failed.
    // ch1 leads the acoustic path by the speaker-to-mic flight time, so a robot
    // syllable straddling a frame boundary shows on ch1 in frame N and on ch0
    // in frame N+1 -- and frame N+1 then looks exactly like an external talker.
    // click_scan widens the denominator across two frames for this reason;
    // omitting it here is what produced the false barge-in that failed E5 on
    // v62, with the robot cutting itself off 1131 ms in. Same omission as v50,
    // in a second copy of the same test.
    static float s_early_env = 0.0f;
    static int16_t s_early_prev_ref = 0;
    const int16_t ref_win = peak_ref > s_early_prev_ref ? peak_ref : s_early_prev_ref;
    s_early_prev_ref = peak_ref;
    const float ratio = (float)peak / ((float)ref_win + 1.0f);
    // The envelope has to track THIS utterance's echo path, and it can only do
    // that while the robot is actually speaking. The first version returned
    // early for the whole grace period, so when the grace expired the envelope
    // still held a stale value and the very first frames cleared it -- a false
    // barge-in at 1131 ms, every time, which is 900 ms of grace plus the three
    // sustained frames. Deterministic, not a stray echo. So: update always
    // while playing, fire only after the grace and once the envelope has had
    // time to settle.
    // Envelope bookkeeping has two requirements that pull against each other,
    // and each was satisfied alone before being satisfied together:
    //
    //   during the grace it MUST update, or it holds a stale value and the
    //   first eligible frame fires (v62/v63: a false barge-in at 1131 ms);
    //
    //   during a candidate run it MUST NOT update, or it climbs toward the
    //   interruption itself and the ratio stops clearing its own raised
    //   envelope before the frame count is reached. With a ~320 ms time
    //   constant the envelope moves about a third in eight frames, which was
    //   enough to stop the early path firing at all on v65 -- every sample
    //   came back ~340 ms, the AFE path's figure, in a suspiciously tight
    //   cluster.
    //
    // So: update while the robot alone is talking, freeze once something else
    // might be.
    const bool external = peak > CLICK_MIN_PEAK &&
                          ratio > s_early_env * (CONFIG_AEC_BARGEIN_EARLY_RATIO_X10 / 10.0f);
    const bool settling = !past_grace || ++s_early_frames < CLICK_SEED_FRAMES;
    if (!external || settling) {
        s_early_env = s_early_env * (1.0f - CLICK_ENV_ALPHA) + ratio * CLICK_ENV_ALPHA;
        s_early_run = 0;
        return;
    }

    if (++s_early_run >= CONFIG_AEC_BARGEIN_EARLY_FRAMES && !s_early_fired) {
        const int64_t duck_us = esp_timer_get_time();
        ESP_LOGI(TAG, "early barge-in (raw mic) -> duck+stop at t=%lld us", (long long)duck_us);
        if (s_click_us != 0 && s_click_duck_us == 0) {
            s_click_duck_us = duck_us;
            s_click_latency_ms = (int)((duck_us - s_click_us) / 1000);
            ESP_LOGI(TAG, "E4 barge-in latency = %d ms (click=%lld duck=%lld)",
                     s_click_latency_ms, (long long)s_click_us, (long long)duck_us);
        }
        playback_barge_in();
        s_early_fired = true;
    }
}
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

        // Reference-channel clipping watch. The task list suggests a boot
        // self-check that plays a tone and warns if ch1 sits out of range;
        // watching real playback covers the same risk without a startup noise
        // and under the conditions that actually matter.
        //
        // The risk is measured, not hypothetical: on v75 the reference peaks at
        // -3.9 dBFS at volume 100, i.e. 3.9 dB of headroom, and clipping there
        // breaks the linearity the AEC depends on. Speech is worse than the
        // pink noise those numbers came from -- same volume 80 reads -14.0 dBFS
        // for noise and -9.0 dBFS for speech, the crest factor difference -- so
        // the margin in real use is smaller still.
        if (s_feed_nch > 1) {
            static int64_t s_clip_log_us = 0;
            int16_t ref_peak = 0;
            for (int f = 0; f < s_feed_chunksize; f++) {
                const int16_t r = buf[f * s_feed_nch + 1];
                const int16_t a = r < 0 ? (int16_t)-r : r;
                if (a > ref_peak) ref_peak = a;
            }
            // -3 dBFS of full scale for int16.
            if (ref_peak > 23197 && now - s_clip_log_us > 10000000) {
                ESP_LOGW(TAG, "AEC reference near clipping: peak=%d (%.1f dBFS). "
                              "Lower the speaker volume; the AEC assumes a linear "
                              "reference.",
                         (int)ref_peak, 20.0 * log10((double)ref_peak / 32767.0));
                s_clip_log_us = now;
            }
        }

        if (s_click_armed) click_scan(buf, s_feed_chunksize, s_feed_nch, now);
#if CONFIG_AEC_LOCAL_BARGEIN
        early_bargein_scan(buf, s_feed_chunksize, s_feed_nch);
#endif
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
    int silence_run = 0;
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
        // Level gate on the AFE's own output, which is the discriminator the
        // duration knobs never had. Measured from the archives:
        //
        //   robot alone (E1 @vol80)   post_rms   8.5
        //   human voice (E2b)         post_rms 218.6
        //   double-talk (E3)          post_rms 181.2
        //
        // 21-26x apart, because the AEC removes the robot so thoroughly that
        // E3's ASR reads only the human. A threshold at 40 sits ~4.5x clear of
        // both. This is what vad_energy_threshold would do, except that field
        // needs a neural VAD model and therefore a model partition; computing
        // one frame's RMS here needs neither, and costs no latency because the
        // frame is already in hand.
        int32_t clean_rms = 0;
        if (res->data && res->data_size >= 2) {
            const int n = res->data_size / 2;
            int64_t acc = 0;
            for (int i = 0; i < n; i++) {
                const int32_t v = res->data[i];
                acc += (int64_t)v * v;
            }
            clean_rms = (int32_t)sqrt((double)acc / n);
        }
        const bool loud_enough = clean_rms >= CONFIG_AEC_BARGEIN_MIN_RMS;
        if (playing && past_grace && loud_enough && res->vad_state == VAD_SPEECH) {
            if (++speech_run >= CONFIG_AEC_BARGEIN_SPEECH_FRAMES && !ducked) {
                // Timestamp in the device's own clock so barge-in latency can be
                // measured without cross-device sync (E4).
                const int64_t duck_us = esp_timer_get_time();
                ESP_LOGI(TAG, "local barge-in -> duck+stop at t=%lld us", (long long)duck_us);
                // E4: pair this with the raw-mic click onset. Only meaningful
                // when a click was armed and seen for this turn.
                if (s_click_us != 0 && s_click_duck_us == 0) {
                    s_click_duck_us = duck_us;
                    s_click_latency_ms = (int)((duck_us - s_click_us) / 1000);
                    ESP_LOGI(TAG, "E4 barge-in latency = %d ms (click=%lld duck=%lld)",
                             s_click_latency_ms, (long long)s_click_us, (long long)duck_us);
                }
                playback_barge_in();
                ducked = true;
            }
            silence_run = 0;
        } else {
            speech_run = 0;
            if (ducked && !playing) {
                ducked = false;
                silence_run = 0;
            } else if (ducked && res->vad_state != VAD_SPEECH) {
                // T3.1: end the interruption only after sustained silence.
                // Un-ducking on the first non-speech frame (~16 ms) made the
                // robot bounce back to full volume inside the gaps between the
                // user's own syllables, then duck again -- audible flapping,
                // and the opposite of "the user is still talking".
                if (++silence_run >= CONFIG_AEC_BARGEIN_SILENCE_FRAMES) {
                    ESP_LOGI(TAG, "barge-in ended: %d silent frames -> unduck", silence_run);
                    playback_unduck();   // false alarm (cough/door) -> restore volume
                    ducked = false;
                    silence_run = 0;
                }
            } else {
                silence_run = 0;
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
    // VAD_MODE_4 is the most aggressive setting esp-sr offers at rejecting
    // non-speech. This is the half of "adjust the VAD threshold and the
    // minimum speech duration" that had not been tried: it changes the
    // decision, not the duration, so unlike vad_min_speech_ms it costs no
    // latency. Serial confirmed E5 genuinely fails at mode 3 with
    // vad_min_speech_ms=64 -- two real barge-ins, zero external stops -- while
    // E4 needs that duration low, so the duration knob alone has no setting
    // that satisfies both.
    cfg->vad_mode     = (vad_mode_t)CONFIG_AEC_VAD_MODE;
    // The AFE will not report speech until it has heard this much of it, and
    // the default is 128 ms. That is the bulk of the ~210 ms that E4 could not
    // account for: 128 ms here, plus the sustained-frame check above it, plus
    // pipeline delay and the 24 ms stop, lands at the ~330 ms measured.
    // T3.1 asks for roughly 48 ms to declare speech; 64 ms is the closest this
    // knob allows while staying above its 32 ms floor.
    cfg->vad_min_speech_ms = CONFIG_AEC_VAD_MIN_SPEECH_MS;
    // Also defaults to 128 ms and was never set. After accounting for
    // vad_min_speech_ms (64), the single sustained frame (16) and the measured
    // stop (24), E4's 232 ms leaves ~128 ms unexplained -- the same number.
    // Worth testing directly rather than assuming it is irreducible pipeline
    // cost, since the last unexplained 128 ms turned out to be exactly this
    // kind of unset default.
    cfg->vad_delay_ms = CONFIG_AEC_VAD_DELAY_MS;
    // "If true, the playback will be muted for vad detection" -- esp-sr's own
    // answer to the problem E5 keeps failing on, and it defaults to false.
    //
    // The residual after AEC is the robot's own speech attenuated by 33-51 dB
    // (E1), so it is still speech-SHAPED. A duration gate cannot separate it
    // from a person: vad_min_speech_ms at 48 ms cut 5 of 5 turns, at 64 ms cut
    // 4 of 5, and the cuts land 3-7 s in, nowhere near the onset grace. The
    // task list says to adjust the VAD threshold and the minimum speech
    // duration; only the duration half had been touched.
    //
    // vad_energy_threshold looks like the other half but is not available
    // here: it "is only applied when a vad model is used", and vad_model_name
    // is NULL, so this build runs the WebRTC VAD and that field is ignored.
    cfg->vad_mute_playback = true;
    cfg->pcm_config.sample_rate = AEC_SAMPLE_RATE_HZ;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#ifdef CONFIG_AEC_ENABLE
    cfg->aec_init = true;
#else
    cfg->aec_init = false;   // pass-through: raw mic upstream, for A/B echo measurement
#endif
    // Set explicitly rather than relying on the AFE_TYPE_VC default, so the
    // level a given build was measured at is visible in the source.
    cfg->aec_nlp_level = AEC_NLP_LEVEL;
    cfg->ns_init = (AEC_NS_ENABLE != 0);
    afe_config_check(cfg);
    // Snapshot post-check, so what is reported is what the AFE really runs with.
    snprintf(s_cfg_summary, sizeof(s_cfg_summary),
             "fmt=%s type=VC aec=%d nlp=%d filt=%d ns=%d vad=%d rate=%d",
             fmt, (int)cfg->aec_init, (int)cfg->aec_nlp_level,
             (int)cfg->aec_filter_length, (int)cfg->ns_init,
             (int)cfg->vad_init, (int)cfg->pcm_config.sample_rate);
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

void afe_config_summary(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    snprintf(buf, n, "%s", s_cfg_summary);
}
