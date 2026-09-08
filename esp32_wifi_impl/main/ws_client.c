#include "ws_client.h"
#include "aec_config.h"
#include "playback.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "ws";

static esp_websocket_client_handle_t s_client;
static volatile bool s_ready;         // stream_ready received -> may send audio
static char s_session_id[40];

// Reassembly for (possibly fragmented) inbound WS frames.
static uint8_t *s_rx;
static size_t   s_rx_len;
static size_t   s_rx_fill;
static int      s_rx_op;

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------
static void send_start_stream(void)
{
    uint32_t r = esp_random();
    snprintf(s_session_id, sizeof(s_session_id), "esp32-%08" PRIx32 "%08" PRIx32,
             (uint32_t)(esp_timer_get_time() & 0xffffffff), r);
    char msg[256];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"start_stream\",\"session_id\":\"%s\",\"device_id\":\"%s\","
        "\"sample_rate\":%d,\"route\":true,\"proto\":%d}",
        s_session_id, AEC_DEVICE_ID, AEC_SAMPLE_RATE_HZ, AEC_WS_PROTO);
    esp_websocket_client_send_text(s_client, msg, n, portMAX_DELAY);
    ESP_LOGI(TAG, "start_stream session=%s", s_session_id);
}

void ws_client_send_audio(const int16_t *pcm, size_t bytes)
{
    if (!s_ready || !s_client || !esp_websocket_client_is_connected(s_client)) return;
    // Non-blocking-ish: short timeout, drop on backpressure rather than stall AFE.
    esp_websocket_client_send_bin(s_client, (const char *)pcm, bytes, pdMS_TO_TICKS(50));
}

void ws_client_report_tts_state(bool playing)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return;
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"tts_state\",\"playing\":%s,\"ts_ms\":%lld}",
        playing ? "true" : "false", (long long)(esp_timer_get_time() / 1000));
    esp_websocket_client_send_text(s_client, msg, n, pdMS_TO_TICKS(50));
}

// ---------------------------------------------------------------------------
// Inbound frame handling
// ---------------------------------------------------------------------------
static void handle_text(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "stream_ready") == 0) {
        s_ready = true;
        ESP_LOGI(TAG, "stream_ready — streaming clean mic");
    } else if (strcmp(t, "tts_begin") == 0) {
        playback_begin_turn();
    } else if (strcmp(t, "tts_cancel") == 0 || strcmp(t, "speech_start") == 0) {
        // Hard barge-in confirmation from the cloud.
        ESP_LOGI(TAG, "%s -> kill playback", t);
        playback_kill();
    } else if (strcmp(t, "result") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        const cJSON *tts  = cJSON_GetObjectItem(root, "tts_text");
        ESP_LOGI(TAG, "result: asr=\"%s\" tts=\"%s\"",
                 cJSON_IsString(text) ? text->valuestring : "",
                 cJSON_IsString(tts) ? tts->valuestring : "");
    } else if (strcmp(t, "stream_stopped") == 0) {
        s_ready = false;
    }
    cJSON_Delete(root);
}

static void handle_binary(const uint8_t *data, size_t len)
{
    if (len == 0) return;                 // TTS stream-end sentinel (empty frame)
    playback_enqueue_wav(data, len);      // one streamed TTS sentence (WAV)
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_ready = false;
        send_start_stream();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        s_ready = false;
        playback_kill();
        break;
    case WEBSOCKET_EVENT_DATA: {
        // Ignore control frames (ping/pong/close: op 0x8/0x9/0xA).
        if (ev->op_code == 0x8 || ev->op_code == 0x9 || ev->op_code == 0xA) break;

        // Reassemble fragmented frames using payload_offset/payload_len.
        if (ev->payload_offset == 0) {
            free(s_rx);
            s_rx_len = ev->payload_len;
            s_rx = s_rx_len ? malloc(s_rx_len) : NULL;
            s_rx_fill = 0;
            s_rx_op = ev->op_code ? ev->op_code : s_rx_op;  // continuation keeps prev op
        }
        if (s_rx && ev->data_len && s_rx_fill + ev->data_len <= s_rx_len) {
            memcpy(s_rx + s_rx_fill, ev->data_ptr, ev->data_len);
            s_rx_fill += ev->data_len;
        }
        // Frame complete?
        if (s_rx_fill >= s_rx_len) {
            if (s_rx_op == 0x01)      handle_text((const char *)s_rx, s_rx_len);
            else if (s_rx_op == 0x02) handle_binary(s_rx, s_rx_len);
            free(s_rx); s_rx = NULL; s_rx_len = s_rx_fill = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
esp_err_t ws_client_start(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = AEC_SERVER_URI,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 8000,
        .buffer_size = 4096,
        .ping_interval_sec = 20,
        // Public gateway uses a real cert; we skip verification for portability.
        // Provide .cert_pem and remove this for pinned-cert production.
        .skip_cert_common_name_check = true,
        .disable_auto_reconnect = false,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    if (strlen(AEC_AUDIO_TOKEN) > 0) {
        esp_websocket_client_append_header(s_client, "X-Audio-Token", AEC_AUDIO_TOKEN);
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    return esp_websocket_client_start(s_client);
}
