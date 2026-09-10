#include "playback.h"
#include "aec_config.h"
#include "afe_board.h"
#include "afe_pipeline.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

static const char *TAG = "playback";

typedef struct {
    uint8_t *data;
    size_t   len;
} wav_chunk_t;

static QueueHandle_t s_queue;                 // wav_chunk_t
static volatile bool s_killed;                // discard until next begin_turn
static volatile bool s_busy;                  // a chunk is being written
static volatile int  s_gain_pct = 100;        // duck applies here
static volatile int64_t s_turn_start_us;
static volatile bool s_external_active;
static playback_state_cb_t s_state_cb;
static volatile bool s_last_reported_playing;

// ---------------------------------------------------------------------------
// WAV parsing + linear resample to 16 kHz mono int16
// ---------------------------------------------------------------------------
static uint32_t rd_u32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
static uint16_t rd_u16(const uint8_t *p) { return p[0] | (p[1]<<8); }

// Locate the PCM payload inside a canonical WAV chunk. Returns pointer/len of
// int16 samples and fills rate/channels. Returns false if not 16-bit PCM WAV.
static bool wav_parse(const uint8_t *w, size_t len, const int16_t **pcm, size_t *pcm_samples,
                      uint32_t *rate, uint16_t *channels)
{
    if (len < 44 || memcmp(w, "RIFF", 4) != 0 || memcmp(w + 8, "WAVE", 4) != 0) return false;
    size_t off = 12;
    uint16_t bits = 0, ch = 1;
    uint32_t sr = AEC_SAMPLE_RATE_HZ;
    const uint8_t *data = NULL; size_t data_len = 0;
    while (off + 8 <= len) {
        const uint8_t *id = w + off;
        uint32_t sz = rd_u32(w + off + 4);
        const uint8_t *body = w + off + 8;
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            ch   = rd_u16(body + 2);
            sr   = rd_u32(body + 4);
            bits = rd_u16(body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            data = body;
            data_len = (off + 8 + sz <= len) ? sz : (len - off - 8);
            break;
        }
        off += 8 + sz + (sz & 1);   // chunks are word-aligned
    }
    if (!data || bits != 16) return false;
    *pcm = (const int16_t *)data;
    *pcm_samples = data_len / 2;      // total int16 samples (all channels)
    *rate = sr;
    *channels = ch ? ch : 1;
    return true;
}

// Write PCM to the speaker as 16 kHz mono, downmixing channels and linearly
// resampling on the fly, applying the current duck gain. Aborts mid-chunk if
// killed (barge-in) so playback stops within one write block.
static void play_pcm(const int16_t *pcm, size_t samples, uint32_t rate, uint16_t ch)
{
    const size_t frames = ch ? samples / ch : 0;
    if (frames == 0) return;

    // Output block buffer (mono 16k).
    enum { OUT_FRAMES = 512 };
    static int16_t out[OUT_FRAMES];
    const double step = (double)rate / AEC_SAMPLE_RATE_HZ;  // input frames per output frame

    double pos = 0.0;
    size_t oi = 0;
    while (pos < frames - 1) {
        if (s_killed) return;
        size_t i0 = (size_t)pos;
        double frac = pos - i0;
        // Downmix channel 0..ch-1 at i0 and i0+1, then interpolate.
        int32_t a = 0, b = 0;
        for (uint16_t c = 0; c < ch; c++) {
            a += pcm[i0 * ch + c];
            b += pcm[(i0 + 1) * ch + c];
        }
        a /= ch; b /= ch;
        int32_t s = (int32_t)(a + (b - a) * frac);
        s = s * s_gain_pct / 100;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[oi++] = (int16_t)s;
        if (oi == OUT_FRAMES) {
            afe_pipeline_push_ref(out, oi);   // software-AEC reference (no-op in HW-ref mode)
            board_spk_write(out, oi * sizeof(int16_t));
            oi = 0;
        }
        pos += step;
    }
    if (oi > 0 && !s_killed) {
        afe_pipeline_push_ref(out, oi);
        board_spk_write(out, oi * sizeof(int16_t));
    }
}

// ---------------------------------------------------------------------------
// Playback task
// ---------------------------------------------------------------------------
static void report_state(bool playing)
{
    if (playing != s_last_reported_playing) {
        s_last_reported_playing = playing;
        if (s_state_cb) s_state_cb(playing);
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    wav_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_queue, &chunk, portMAX_DELAY) != pdTRUE) continue;

        if (s_killed) {                 // belongs to a cancelled turn
            free(chunk.data);
            continue;
        }
        s_busy = true;
        report_state(true);
        board_pa_enable(true);

        const int16_t *pcm; size_t nsamp; uint32_t rate; uint16_t ch;
        if (wav_parse(chunk.data, chunk.len, &pcm, &nsamp, &rate, &ch)) {
            play_pcm(pcm, nsamp, rate, ch);
        } else {
            ESP_LOGW(TAG, "unparseable TTS chunk (%u bytes) dropped", (unsigned)chunk.len);
        }
        free(chunk.data);
        s_busy = false;

        // Only report "stopped" once the backlog is drained, so tts_state doesn't
        // flap between streamed sentences.
        if (uxQueueMessagesWaiting(s_queue) == 0) {
            report_state(false);
            board_pa_enable(false);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t playback_init(playback_state_cb_t on_state_change)
{
    s_state_cb = on_state_change;
    s_queue = xQueueCreate(AEC_PLAYBACK_QUEUE_LEN, sizeof(wav_chunk_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    s_turn_start_us = esp_timer_get_time();
    // Pin off core 0 (WiFi/AFE heavy) to keep playback smooth.
    xTaskCreatePinnedToCore(playback_task, "playback", 4096, NULL, 6, NULL, 1);
    return ESP_OK;
}

void playback_begin_turn(void)
{
    s_killed = false;
    s_gain_pct = 100;
    s_turn_start_us = esp_timer_get_time();
}

esp_err_t playback_enqueue_wav(const uint8_t *wav, size_t len)
{
    if (s_killed || len == 0 || len > AEC_PLAYBACK_CHUNK_MAX) return ESP_ERR_INVALID_STATE;
    wav_chunk_t c = { .data = malloc(len), .len = len };
    if (!c.data) return ESP_ERR_NO_MEM;
    memcpy(c.data, wav, len);
    if (xQueueSend(s_queue, &c, 0) != pdTRUE) {
        free(c.data);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int  playback_gain_pct(void) { return s_gain_pct; }
void playback_duck(void)   { s_gain_pct = CONFIG_AEC_DUCK_VOLUME_PCT; }
void playback_unduck(void) { s_gain_pct = 100; }

void playback_kill(void)
{
    s_killed = true;                 // stops play_pcm mid-block and drops future chunks
    wav_chunk_t c;
    while (xQueueReceive(s_queue, &c, 0) == pdTRUE) free(c.data);
    report_state(false);
    board_pa_enable(false);
}

bool playback_is_playing(void)
{
    return s_busy || s_external_active || uxQueueMessagesWaiting(s_queue) > 0;
}

void playback_set_external_active(bool active)
{
    if (active) s_turn_start_us = esp_timer_get_time();
    s_external_active = active;
    if (!active) playback_unduck();
}

int64_t playback_turn_age_ms(void)
{
    return (esp_timer_get_time() - s_turn_start_us) / 1000;
}
