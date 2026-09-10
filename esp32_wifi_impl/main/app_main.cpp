#include "device_hub_client.h"
#include "ota_updater.h"
#include "audio_player.h"
#include "mqtt_control_client.h"
#include "hd44780.h"
#include "audio_board_config.h"

extern "C" {
#include "afe_board.h"
#include "playback.h"
#include "ws_client.h"
#include "afe_pipeline.h"
}

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <ctime>
#include <sys/time.h>

static const char* TAG = "wangyutang_app";
static EventGroupHandle_t wifi_events;
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr char kTestAudioUrl[] =
    "https://raw.githubusercontent.com/1540530942/esp32_wifi/main/%E4%BD%A0%E4%BB%8A%E5%A4%A9%E5%A5%BD%E5%90%97.wav";
static int s_volume = 30;
static AudioPlayer* s_audio_player = nullptr;
static MqttControlClient* s_mqtt_control = nullptr;

// LCD 状态
static char s_lcd_ip[16] = "---";
static int  s_lcd_tick   = 0;

// 固件版本缩短：去掉 "esp32-wangyutang-" 前缀，剩余部分适合放 LCD
static const char* lcd_firmware_label() {
    const char* ver = CONFIG_DEVICE_FIRMWARE_VERSION;
    static const char prefix[] = "esp32-wangyutang-";
    if (std::strncmp(ver, prefix, sizeof(prefix) - 1) == 0) ver += sizeof(prefix) - 1;
    return ver;
}

static void load_persisted_settings() {
    nvs_handle_t handle = 0;
    if (nvs_open("settings", NVS_READONLY, &handle) != ESP_OK) return;
    int32_t volume = 30;
    if (nvs_get_i32(handle, "volume", &volume) == ESP_OK) {
        s_volume = std::max(0, std::min(100, static_cast<int>(volume)));
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "restored volume=%d%%", s_volume);
}

static void persist_volume() {
    nvs_handle_t handle = 0;
    if (nvs_open("settings", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "failed to open settings NVS for volume");
        return;
    }
    const esp_err_t err = nvs_set_i32(handle, "volume", s_volume);
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "failed to persist volume=%d: %s", s_volume,
                                esp_err_to_name(err));
}

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* arg) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(arg);
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d", event ? event->reason : -1);
        lcd_print_line(0, "ESP32  OFFLINE  ");
        lcd_print_line(1, "                ");
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(arg);
        ESP_LOGI(TAG, "Wi-Fi got IP=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
        esp_ip4addr_ntoa(&event->ip_info.ip, s_lcd_ip, sizeof(s_lcd_ip));
        lcd_print_line(1, s_lcd_ip);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wait_for_default_gateway() {    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");    if (!netif) return false;    esp_netif_ip_info_t ip = {};    for (int i = 0; i < 20; ++i) {        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.gw.addr != 0) {            ESP_LOGI(TAG, "default gateway ready: " IPSTR, IP2STR(&ip.gw));            return true;        }        ESP_LOGW(TAG, "waiting for DHCP gateway");        vTaskDelay(pdMS_TO_TICKS(500));    }    ESP_LOGE(TAG, "no default gateway after DHCP wait");    return false;}
static void init_wifi() {
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), CONFIG_DEVICE_WIFI_SSID, sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), CONFIG_DEVICE_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static bool sync_clock_from_http_date() {
    esp_http_client_config_t config = {};
    config.url = "http://www.wangyutang.cn/devices/api/health";
    config.timeout_ms = 8000;
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    char* date_header = nullptr;
    if (err == ESP_OK) esp_http_client_get_header(client, "Date", &date_header);
    bool synced = false;
    if (date_header) {
        struct tm parsed = {};
        if (strptime(date_header, "%a, %d %b %Y %H:%M:%S GMT", &parsed)) {
            int year = parsed.tm_year + 1900;
            unsigned month = static_cast<unsigned>(parsed.tm_mon + 1);
            year -= month <= 2;
            const int era = (year >= 0 ? year : year - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(year - era * 400);
            const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
                                 + static_cast<unsigned>(parsed.tm_mday) - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            const time_t days = static_cast<time_t>(era * 146097 + static_cast<int>(doe) - 719468);
            struct timeval tv = {.tv_sec = days * 86400 + parsed.tm_hour * 3600
                                           + parsed.tm_min * 60 + parsed.tm_sec,
                                 .tv_usec = 0};
            synced = settimeofday(&tv, nullptr) == 0;
        }
    }
    esp_http_client_cleanup(client);
    return synced;
}

static void sync_clock() {
    if (sync_clock_from_http_date()) {
        ESP_LOGI(TAG, "clock synchronized from wangyutang HTTP Date header");
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, const_cast<char*>("ntp.aliyun.com"));
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, const_cast<char*>("ntp.tencent.com"));
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, const_cast<char*>("time.cloudflare.com"));
#endif
    esp_sntp_init();
    time_t now = 0;
    for (int i = 0; i < 40 && now < 1700000000; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
    }
    if (now >= 1700000000) {
        ESP_LOGI(TAG, "SNTP time synchronized");
    } else {
        ESP_LOGW(TAG, "SNTP time synchronization timed out; HTTPS may fail");
    }
}

static std::string device_state() {
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    esp_netif_ip_info_t ip = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip);
    char ip_text[16] = {};
    esp_ip4addr_ntoa(&ip.ip, ip_text, sizeof(ip_text));
    cJSON* state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "firmware", CONFIG_DEVICE_FIRMWARE_VERSION);
    cJSON_AddStringToObject(state, "wifi_ssid", reinterpret_cast<char*>(ap.ssid));
    cJSON_AddNumberToObject(state, "wifi_rssi", ap.rssi);
    cJSON_AddStringToObject(state, "ip", ip_text);
    cJSON_AddNumberToObject(state, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(state, "free_heap", esp_get_free_heap_size());
    // free_heap counts PSRAM too; task stacks and driver buffers come out of
    // internal RAM, so report that separately - it is what actually runs out.
    cJSON_AddNumberToObject(state, "free_internal",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(state, "largest_internal",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(state, "volume", s_volume);
    cJSON_AddNumberToObject(state, "reset_reason", (int)esp_reset_reason());
    cJSON_AddBoolToObject(state, "audio_playing",
                          s_audio_player != nullptr && s_audio_player->is_playing());
    cJSON_AddBoolToObject(state, "mqtt_connected",
                          s_mqtt_control != nullptr && s_mqtt_control->is_connected());
    cJSON_AddBoolToObject(state, "activated", s_audio_player != nullptr);
    cJSON_AddStringToObject(state, "audio_capability", "play_audio,stream_prepare,stop_audio");
    char* text = cJSON_PrintUnformatted(state);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(state);
    return result;
}

// Mic + ASR + broadcast validation: hold the physical BOOT button (GPIO0,
// unused elsewhere in this firmware) to record 4s, send to cloud ASR, and
// speak the recognized text back. Polled (not interrupt-driven) since a
// button-press response time of ~100ms is plenty for a manual hardware test.
static void mic_asr_button_task(void* /*arg*/) {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    bool was_pressed = false;
    int64_t press_started_ms = 0;
    for (;;) {
        const bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;  // active-low
        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(30));  // debounce
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                press_started_ms = esp_timer_get_time() / 1000;
            }
        } else if (!pressed && was_pressed && press_started_ms > 0) {
            const int64_t held_ms = esp_timer_get_time() / 1000 - press_started_ms;
            press_started_ms = 0;
            if (s_audio_player != nullptr && !s_audio_player->is_playing()) {
                if (held_ms >= 800) {
                    // Long press: hardware-AEC-reference diagnostic (see
                    // docs/logs/ for what this measures and why).
                    ESP_LOGI(TAG, "BOOT button long-press (%lldms) -> aec_reference_probe", held_ms);
                    esp_err_t err = s_audio_player->run_aec_reference_probe();
                    ESP_LOGI(TAG, "aec_reference_probe result=%s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "BOOT button short-press (%lldms) -> mic_asr_test", held_ms);
                    esp_err_t err = s_audio_player->run_mic_asr_test(4, s_volume);
                    ESP_LOGI(TAG, "mic_asr_test result=%s asr_text=\"%s\"",
                             esp_err_to_name(err), s_audio_player->last_asr_text().c_str());
                }
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void aec_on_clean_audio(const int16_t* pcm, size_t bytes) {
    ws_client_send_audio(pcm, bytes);
}
static void aec_on_tts_state(bool playing) {
    ws_client_report_tts_state(playing);
}

// --- continuous far end ----------------------------------------------------
// aec_capture drives playback itself, so the echo only exists while a capture
// runs. That is fine for a single window, but it cannot produce the case the
// acceptance criteria care about: a minute of unbroken robot speech, sampled at
// several points. The AFE's NS noise estimate adapts to a *sustained* source and
// relaxes when it stops, so segments separated by silence are not slices of one
// scene -- each one restarts the adaptation.
//
// far_end plays a TTS clip on loop in its own task and returns immediately, so
// captures with no_play can sample inside it.
static TaskHandle_t s_farend_task = nullptr;
static volatile bool s_farend_playing = false;
static volatile bool s_farend_stop = false;
static int16_t* s_farend_pcm = nullptr;      // cached clip, kept between runs
static size_t s_farend_nsamp = 0;
static std::string s_farend_text;            // which text the cached clip is
static uint8_t s_farend_vol = 60;
static int s_farend_seconds = 60;

// One long-lived task, created on first use and never deleted. Creating it per
// run failed with no_task: FreeRTOS reclaims a self-deleted task's stack only
// when the idle task next runs, so starting the next one immediately could not
// find a contiguous 4 KB in the ~38 KB of internal RAM left once the AFE is up.
// Same failure mode, and same fix, as the per-command MQTT task earlier.
static void farend_task(void*) {
    for (;;) {
        if (!s_farend_playing) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        board_set_volume(s_farend_vol);
        board_pa_enable(true);
        const double step = 24000.0 / 16000.0;
        static int16_t out[256];
        size_t oi = 0; double pos = 0.0;
        const int64_t t0 = esp_timer_get_time();
        const int64_t limit = (int64_t)s_farend_seconds * 1000000;
        while (!s_farend_stop && (esp_timer_get_time() - t0) < limit) {
            if (pos >= (double)s_farend_nsamp - 1.0) pos = 0.0;
            size_t i0 = (size_t)pos;
            double fr = pos - i0;
            // Honour barge-in. playback_duck() only lowers the gain inside
            // playback.c, and this writes to the codec directly, so without
            // this the far end played straight through an interruption -- and
            // the reference channel showed no duck at all, which made the
            // barge-in latency unmeasurable.
            int32_t v = (int32_t)(s_farend_pcm[i0] + (s_farend_pcm[i0 + 1] - s_farend_pcm[i0]) * fr);
            out[oi++] = (int16_t)(v * playback_gain_pct() / 100);
            if (oi == 256) { board_spk_write(out, sizeof(out)); oi = 0; }
            pos += step;
        }
        board_pa_enable(false);
        s_farend_playing = false;
    }
}

// --- capture upload ---------------------------------------------------------
// Wrap mono 16-bit PCM as a WAV and multipart-POST it to device-hub, which
// stores it under its audio dir and hands back a URL. Plain HTTP on the gateway
// IP: :443 is MITM-broken on this ISP path (same reason everything else here
// avoids TLS).
static void aec_wav_header(uint8_t* h, uint32_t data_bytes, uint32_t rate) {
    const uint32_t br = rate * 2, riff = 36 + data_bytes;
    std::memcpy(h, "RIFF", 4);
    h[4]=riff&0xff; h[5]=(riff>>8)&0xff; h[6]=(riff>>16)&0xff; h[7]=(riff>>24)&0xff;
    std::memcpy(h+8, "WAVEfmt ", 8);
    h[16]=16; h[17]=h[18]=h[19]=0; h[20]=1; h[21]=0; h[22]=1; h[23]=0;
    h[24]=rate&0xff; h[25]=(rate>>8)&0xff; h[26]=(rate>>16)&0xff; h[27]=(rate>>24)&0xff;
    h[28]=br&0xff; h[29]=(br>>8)&0xff; h[30]=(br>>16)&0xff; h[31]=(br>>24)&0xff;
    h[32]=2; h[33]=0; h[34]=16; h[35]=0;
    std::memcpy(h+36, "data", 4);
    h[40]=data_bytes&0xff; h[41]=(data_bytes>>8)&0xff; h[42]=(data_bytes>>16)&0xff; h[43]=(data_bytes>>24)&0xff;
}

static bool aec_upload_wav(const char* name, const int16_t* pcm, size_t samples) {
    if (!pcm || samples == 0) return false;
    const size_t data_bytes = samples * sizeof(int16_t);
    uint8_t hdr[44];
    aec_wav_header(hdr, (uint32_t)data_bytes, 16000);

    const char* boundary = "----ESP32AecCapture7MA4YWxk";
    std::string pre = std::string("--") + boundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
        "\"\r\nContent-Type: audio/wav\r\n\r\n";
    std::string post = std::string("\r\n--") + boundary + "--\r\n";
    const size_t total = pre.size() + sizeof(hdr) + data_bytes + post.size();

    esp_http_client_config_t cfg = {};
    cfg.url = "http://110.40.154.41/devices/api/device/" CONFIG_DEVICE_ID "/upload_audio?play=0";
    cfg.method = HTTP_METHOD_POST;
    // A 20 s capture is 640 KB and uploads inside 30 s; a 30 s one is 960 KB
    // and does not, which showed up as uploads=0/0/0 with the capture itself
    // reporting success. device_hub accepts up to 20 MB, so the ceiling was
    // only ever this timeout.
    cfg.timeout_ms = 120000;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    std::string ct = std::string("multipart/form-data; boundary=") + boundary;
    esp_http_client_set_header(c, "Content-Type", ct.c_str());
    bool ok = false;
    if (esp_http_client_open(c, (int)total) == ESP_OK) {
        ok = esp_http_client_write(c, pre.data(), pre.size()) >= 0 &&
             esp_http_client_write(c, (const char*)hdr, sizeof(hdr)) >= 0;
        // stream the PCM in chunks so we never need a second full-size buffer
        const uint8_t* p = (const uint8_t*)pcm;
        size_t left = data_bytes;
        while (ok && left) {
            size_t n = left > 4096 ? 4096 : left;
            ok = esp_http_client_write(c, (const char*)p, n) >= 0;
            p += n; left -= n;
        }
        if (ok) ok = esp_http_client_write(c, post.data(), post.size()) >= 0;
        if (ok) { esp_http_client_fetch_headers(c); ok = esp_http_client_get_status_code(c) / 100 == 2; }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "aec_capture upload %s samples=%u ok=%d", name, (unsigned)samples, (int)ok);
    return ok;
}

static std::string handle_command(const HubCommand& cmd, AudioPlayer* player) {
    ESP_LOGI(TAG, "executing command=%s args=%s", cmd.action.c_str(), cmd.args_json.c_str());
    if (cmd.action == "reboot") {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return "done";
    }
    if (cmd.action == "set_volume") {
        cJSON* args = cJSON_Parse(cmd.args_json.c_str());
        cJSON* value = args ? cJSON_GetObjectItem(args, "value") : nullptr;
        if (!cJSON_IsNumber(value)) {
            if (args) cJSON_Delete(args);
            return "failed";
        }
        s_volume = std::max(0, std::min(100, (int)value->valuedouble));
        persist_volume();
        if (args) cJSON_Delete(args);
        ESP_LOGI(TAG, "volume set to %d", s_volume);
        return "done";
    }
    if (cmd.action == "play_audio") {
        cJSON* args = cJSON_Parse(cmd.args_json.c_str());
        const char* url = kTestAudioUrl;
        std::string rewritten_url;
        cJSON* url_item = args ? cJSON_GetObjectItem(args, "url") : nullptr;
        cJSON* name_item = args ? cJSON_GetObjectItem(args, "name") : nullptr;
        cJSON* volume_item = args ? cJSON_GetObjectItem(args, "volume") : nullptr;
        if (cJSON_IsNumber(volume_item)) {
            s_volume = std::max(0, std::min(100, (int)volume_item->valuedouble));
            persist_volume();
            ESP_LOGI(TAG, "play_audio volume=%d", s_volume);
        }
        if (cJSON_IsString(url_item) && std::strncmp(url_item->valuestring, "https://", 8) == 0) {
            constexpr const char* kHttpsPrefix = "https://www.wangyutang.cn";
            if (std::strncmp(url_item->valuestring, kHttpsPrefix,
                             std::strlen(kHttpsPrefix)) == 0) {
                rewritten_url = std::string("http://110.40.154.41") +
                                (url_item->valuestring + std::strlen(kHttpsPrefix));
                url = rewritten_url.c_str();
            } else {
                url = url_item->valuestring;
            }
        } else if (cJSON_IsString(url_item) &&
                   std::strncmp(url_item->valuestring,
                                "http://110.40.154.41/devices/api/", 33) == 0) {
            url = url_item->valuestring;
        } else if (cJSON_IsString(name_item) && std::strcmp(name_item->valuestring, "test") != 0) {
            if (args) cJSON_Delete(args);
            return "unsupported";
        }
        esp_err_t err = player->play_wav_url(url, (uint8_t)s_volume);
        if (args) cJSON_Delete(args);
        if (err != ESP_OK) return "failed|stage=wav_start error=" + std::string(esp_err_to_name(err));
        while (player->is_playing()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return player->last_result() == ESP_OK ? "done" :
               "failed|stage=wav_playback error=" + player->last_error();
    }
    if (cmd.action == "stream_prepare") {
        cJSON* args = cJSON_Parse(cmd.args_json.c_str());
        cJSON* stream_url = args ? cJSON_GetObjectItem(args, "stream_url") : nullptr;
        cJSON* volume_item = args ? cJSON_GetObjectItem(args, "volume") : nullptr;
        cJSON* pa_level_item = args ? cJSON_GetObjectItem(args, "pa_level") : nullptr;
        if (!cJSON_IsString(stream_url)) {
            if (args) cJSON_Delete(args);
            return "failed";
        }
        if (cJSON_IsNumber(volume_item)) {
            s_volume = std::max(0, std::min(100, (int)volume_item->valuedouble));
            persist_volume();
        }
        const int pa_level = cJSON_IsNumber(pa_level_item) && pa_level_item->valuedouble == 0 ? 0 : 1;
        esp_err_t err = player->play_pcm_url(stream_url->valuestring, (uint8_t)s_volume, pa_level);
        if (args) cJSON_Delete(args);
        if (err != ESP_OK) return "failed|stage=pcm_start error=" + std::string(esp_err_to_name(err));
        while (player->is_playing()) vTaskDelay(pdMS_TO_TICKS(20));
        if (player->last_result() != ESP_OK) return "failed|stage=pcm_playback error=" + player->last_error();
        return "done|event=playback_done bytes=" +
               std::to_string(player->last_pcm_bytes()) +
               " elapsed_ms=" + std::to_string(player->last_pcm_elapsed_ms()) +
               " pa_gpio17=" + std::to_string(player->last_pcm_pa_level());
    }
    if (cmd.action == "stop_audio" || cmd.action == "stop") {
        return player->stop() == ESP_OK ? "done" : "failed";
    }
    if (cmd.action == "identify") {
        ESP_LOGI(TAG, "identify: audio device online");
        return "done";
    }
    if (cmd.action == "ota") {
        cJSON* ota_args = cJSON_Parse(cmd.args_json.c_str());
        cJSON* ota_url_item = ota_args ? cJSON_GetObjectItem(ota_args, "url") : nullptr;
        if (!cJSON_IsString(ota_url_item)) {
            if (ota_args) cJSON_Delete(ota_args);
            return "failed|stage=ota error=missing_url";
        }
        std::string ota_url = ota_url_item->valuestring;
        constexpr const char* kOtaHttpsHost = "https://www.wangyutang.cn";
        if (ota_url.rfind(kOtaHttpsHost, 0) == 0) {
            ota_url = std::string("http://110.40.154.41") + ota_url.substr(std::strlen(kOtaHttpsHost));
        }
        // Skip OTA if already on target version to prevent retained-command reboot loop
        cJSON* ver_item = cJSON_GetObjectItem(ota_args, "version");
        if (cJSON_IsString(ver_item) &&
            std::string(ver_item->valuestring) == CONFIG_DEVICE_FIRMWARE_VERSION) {
            cJSON_Delete(ota_args);
            ESP_LOGI(TAG, "OTA: already on target version %s, skipping", CONFIG_DEVICE_FIRMWARE_VERSION);
            return std::string("done|ota_skipped=already_on_") + CONFIG_DEVICE_FIRMWARE_VERSION;
        }
        if (ota_args) cJSON_Delete(ota_args);
        esp_err_t ota_err = ota_run_from_url(ota_url.c_str());
        if (ota_err != ESP_OK) {
            return "failed|stage=ota error=" + std::string(esp_err_to_name(ota_err));
        }
        ESP_LOGI(TAG, "OTA applied, rebooting into new image");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
        return "done";  // not reached
    }
    if (cmd.action == "far_end_stop") {
        s_farend_stop = true;
        for (int i = 0; i < 50 && s_farend_playing; ++i) vTaskDelay(pdMS_TO_TICKS(100));
        return s_farend_playing ? "failed|stage=farend_stop" : "done|far_end stopped";
    }
    if (cmd.action == "far_end") {
        if (s_farend_playing) return "failed|stage=farend error=already_running";
        cJSON* a = cJSON_Parse(cmd.args_json.c_str());
        cJSON* t_it = a ? cJSON_GetObjectItem(a, "text") : nullptr;
        cJSON* v_it = a ? cJSON_GetObjectItem(a, "volume") : nullptr;
        cJSON* s_it = a ? cJSON_GetObjectItem(a, "seconds") : nullptr;
        std::string ftext = cJSON_IsString(t_it) ? t_it->valuestring : "";
        s_farend_vol = cJSON_IsNumber(v_it) ? (uint8_t)v_it->valueint : 60;
        s_farend_seconds = cJSON_IsNumber(s_it) ? s_it->valueint : 60;
        if (a) cJSON_Delete(a);
        if (ftext.empty()) return "failed|stage=farend error=missing_text";
        if (s_farend_seconds < 1 || s_farend_seconds > 300) return "failed|stage=farend error=bad_seconds";

        // Synthesize only when the text changed. Re-fetching cost ~12 s for the
        // 33-character script and failed intermittently; keeping the clip also
        // makes the far end byte-identical from run to run, which is what lets
        // two scenes be compared as the same stimulus.
        bool cached = (s_farend_pcm && ftext == s_farend_text);
        if (!cached) {
            uint8_t* wav = nullptr; size_t wlen = 0;
            if (player->fetch_tts(ftext, &wav, &wlen) != ESP_OK || wlen < 44) {
                return "failed|stage=farend_tts";
            }
            size_t nsamp = (wlen - 44) / sizeof(int16_t);
            int16_t* pcm = (int16_t*)heap_caps_malloc(nsamp * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!pcm) { heap_caps_free(wav); return "failed|stage=farend error=no_mem"; }
            memcpy(pcm, wav + 44, nsamp * sizeof(int16_t));
            heap_caps_free(wav);
            if (s_farend_pcm) heap_caps_free(s_farend_pcm);
            s_farend_pcm = pcm; s_farend_nsamp = nsamp; s_farend_text = ftext;
        }

        if (!s_farend_task &&
            xTaskCreate(farend_task, "farend", 4096, nullptr, 4, &s_farend_task) != pdPASS) {
            return "failed|stage=farend error=no_task";
        }
        s_farend_stop = false;
        s_farend_playing = true;
        char msg[160];
        snprintf(msg, sizeof(msg), "done|far_end started secs=%d vol=%d clip_ms=%u src=%s",
                 s_farend_seconds, (int)s_farend_vol,
                 (unsigned)(s_farend_nsamp * 1000 / 24000), cached ? "cache" : "tts");
        return std::string(msg);
    }
    if (cmd.action == "aec_config") {
        char sum[160];
        afe_config_summary(sum, sizeof(sum));
        return std::string("done|") + sum;
    }
    if (cmd.action == "aec_probe") {
        if (player->is_playing()) return "failed|busy";
        esp_err_t err = player->run_aec_reference_probe();
        if (err != ESP_OK) {
            return "failed|stage=aec_probe error=" + std::string(esp_err_to_name(err));
        }
        const auto& r = player->last_probe_result();
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "done|silent_ch0=%.1f silent_ch1=%.1f play_ch0=%.1f play_ch1=%.1f "
                 "ch1_delta=%.1f reference=%s",
                 r.silent_rms0, r.silent_rms1, r.play_rms0, r.play_rms1,
                 r.play_rms1 - r.silent_rms1, r.reference_is_real ? "REAL" : "FLOATING");
        return std::string(buf);
    }
    if (cmd.action == "aec_capture") {
        // Record raw mic (pre-AEC), the hardware reference, and the AFE clean
        // output (post-AEC) while a burst plays, then upload all three as WAVs.
        cJSON* a = cJSON_Parse(cmd.args_json.c_str());
        auto num = [&](const char* k, int dflt, int lo, int hi) {
            cJSON* it = a ? cJSON_GetObjectItem(a, k) : nullptr;
            return cJSON_IsNumber(it) ? std::max(lo, std::min(hi, (int)it->valuedouble)) : dflt;
        };
        const int secs   = num("seconds", 15, 1, 30);
        const int settle = num("settle", 3, 0, 10);
        const int vol    = num("volume", 100, 0, 100);
        cJSON* tone_it = a ? cJSON_GetObjectItem(a, "tone") : nullptr;
        const bool use_tone = cJSON_IsTrue(tone_it);
        // Optional: play a real WAV (TTS speech) through the speaker instead of a
        // synthetic burst. Speech is far easier to judge by ear than noise.
        cJSON* url_it = a ? cJSON_GetObjectItem(a, "url") : nullptr;
        std::string play_url = cJSON_IsString(url_it) ? url_it->valuestring : "";
        // `text` plays real TTS speech as the far end -- the actual product case
        // (the robot talking while someone speaks to it).
        cJSON* text_it = a ? cJSON_GetObjectItem(a, "text") : nullptr;
        std::string tts_text = cJSON_IsString(text_it) ? text_it->valuestring : "";
        // Record without driving playback: used to sample inside a far_end run.
        cJSON* np_it = a ? cJSON_GetObjectItem(a, "no_play") : nullptr;
        const bool no_play = cJSON_IsTrue(np_it);
        const char* tag = a ? [&]{ cJSON* t = cJSON_GetObjectItem(a, "tag");
                                   return cJSON_IsString(t) ? t->valuestring : "run"; }() : "run";
        std::string tagname = tag;
        if (a) cJSON_Delete(a);

        const size_t cap = (size_t)secs * 16000;
        int16_t* bmic   = (int16_t*)heap_caps_malloc(cap * 2, MALLOC_CAP_SPIRAM);
        int16_t* bref   = (int16_t*)heap_caps_malloc(cap * 2, MALLOC_CAP_SPIRAM);
        int16_t* bclean = (int16_t*)heap_caps_malloc(cap * 2, MALLOC_CAP_SPIRAM);
        if (!bmic || !bref || !bclean) {
            heap_caps_free(bmic); heap_caps_free(bref); heap_caps_free(bclean);
            return "failed|stage=alloc";
        }

        static int16_t blk[256];
        uint32_t rng = 0x12345678; double phase = 0.0;
        const double inc = 2.0 * M_PI * 1000.0 / 16000.0;
        auto burst = [&](int seconds) {
            const int blocks = seconds * 16000 / 256;
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < 256; ++i) {
                    if (use_tone) { blk[i] = (int16_t)(9000.0 * sin(phase)); phase += inc; }
                    else { rng = rng * 1664525u + 1013904223u; blk[i] = (int16_t)((int32_t)(rng >> 16) % 9000); }
                }
                board_spk_write(blk, sizeof(blk));
            }
        };

        size_t nm = 0, nr = 0, nc = 0;
        if (no_play) {
            if (settle > 0) vTaskDelay(pdMS_TO_TICKS(settle * 1000));
            afe_capture_begin(bmic, bref, bclean, cap);
            vTaskDelay(pdMS_TO_TICKS(secs * 1000));
            afe_capture_end(&nm, &nr, &nc);
        } else if (!tts_text.empty()) {
            uint8_t* twav = nullptr; size_t tlen = 0;
            if (player->fetch_tts(tts_text, &twav, &tlen) != ESP_OK || tlen < 44) {
                heap_caps_free(bmic); heap_caps_free(bref); heap_caps_free(bclean);
                return "failed|stage=tts_fetch";
            }
            // TTS comes back at the cloud's rate (24 kHz); the codec runs at 16 kHz,
            // so resample on the fly while writing, exactly like playback.c does.
            const int16_t* pcm = reinterpret_cast<const int16_t*>(twav + 44);
            const size_t nsamp = (tlen - 44) / sizeof(int16_t);
            const double step = 24000.0 / 16000.0;
            board_set_volume(vol);
            board_pa_enable(true);
            static int16_t out[256];
            size_t oi = 0; double pos = 0.0; bool capturing = false;
            const int64_t t0 = esp_timer_get_time();
            // Replay the clip until the window is full. Synthesis length is not
            // under our control (and the body can come back short), so looping
            // is what makes the capture a fixed, requested duration -- and a
            // robot that keeps talking is the case we actually care about.
            while (true) {
                if (pos >= (double)nsamp - 1.0) pos = 0.0;
                if (!capturing && (esp_timer_get_time() - t0) > (int64_t)settle * 1000000) {
                    afe_capture_begin(bmic, bref, bclean, cap);
                    capturing = true;
                }
                size_t i0 = (size_t)pos;
                double fr = pos - i0;
                out[oi++] = (int16_t)(pcm[i0] + (pcm[i0 + 1] - pcm[i0]) * fr);
                if (oi == 256) { board_spk_write(out, sizeof(out)); oi = 0; }
                pos += step;
                if (capturing && (esp_timer_get_time() - t0) > (int64_t)(settle + secs) * 1000000) break;
            }
            if (oi) board_spk_write(out, oi * sizeof(int16_t));
            if (!capturing) afe_capture_begin(bmic, bref, bclean, cap);
            // let the tail of the burst reach the mic before closing the window
            vTaskDelay(pdMS_TO_TICKS(300));
            afe_capture_end(&nm, &nr, &nc);
            board_pa_enable(false);
            heap_caps_free(twav);
        } else if (!play_url.empty()) {
            // Real audio through the normal playback path; capture runs alongside.
            // The amp must be ungated here too -- a previous burst leaves it off.
            board_set_volume(vol);
            board_pa_enable(true);
            s_volume = vol;
            if (player->play_wav_url(play_url, (uint8_t)vol) != ESP_OK) {
                heap_caps_free(bmic); heap_caps_free(bref); heap_caps_free(bclean);
                return "failed|stage=play_url";
            }
            if (settle > 0) vTaskDelay(pdMS_TO_TICKS(settle * 1000));
            afe_capture_begin(bmic, bref, bclean, cap);
            vTaskDelay(pdMS_TO_TICKS(secs * 1000));
            afe_capture_end(&nm, &nr, &nc);
            player->stop();
            for (int i = 0; i < 100 && player->is_playing(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
            board_pa_enable(false);
        } else {
            board_set_volume(vol);
            board_pa_enable(true);
            if (settle > 0) burst(settle);        // let the adaptive filter converge
            afe_capture_begin(bmic, bref, bclean, cap);
            burst(secs);
            afe_capture_end(&nm, &nr, &nc);
            board_pa_enable(false);
        }

        const std::string base = tagname + "_" + (no_play ? "sample" : !tts_text.empty() ? "tts" : (!play_url.empty() ? "speech" : (use_tone ? "tone" : "noise"))) + "_v" + std::to_string(vol);
        const bool o1 = aec_upload_wav((base + "_1_mic_raw.wav").c_str(),   bmic,   nm);
        const bool o2 = aec_upload_wav((base + "_2_reference.wav").c_str(), bref,   nr);
        const bool o3 = aec_upload_wav((base + "_3_clean_aec.wav").c_str(), bclean, nc);
        heap_caps_free(bmic); heap_caps_free(bref); heap_caps_free(bclean);

        char buf[220];
        snprintf(buf, sizeof(buf),
                 "done|captured mic=%u ref=%u clean=%u uploads=%d/%d/%d base=%s secs=%d settle=%d vol=%d",
                 (unsigned)nm, (unsigned)nr, (unsigned)nc, (int)o1, (int)o2, (int)o3,
                 base.c_str(), secs, settle, vol);
        return std::string(buf);
    }
    if (cmd.action == "aec_erle") {
        // On-device ERLE. Plays a broadband burst through the real ES8311 ->
        // speaker path while the AFE runs, then compares raw-mic RMS against the
        // AFE's clean output. Two windows: a settle window (the adaptive filter
        // is still converging) and a measure window after it. A pure sine is a
        // poor excitation for an adaptive filter, so the default is white noise.
        cJSON* a = cJSON_Parse(cmd.args_json.c_str());
        auto num = [&](const char* k, int dflt, int lo, int hi) {
            cJSON* it = a ? cJSON_GetObjectItem(a, k) : nullptr;
            return cJSON_IsNumber(it) ? std::max(lo, std::min(hi, (int)it->valuedouble)) : dflt;
        };
        const int secs   = num("seconds", 6, 2, 15);
        const int settle = num("settle", 3, 1, 10);
        const int vol    = num("volume", 70, 0, 100);
        cJSON* tone_it = a ? cJSON_GetObjectItem(a, "tone") : nullptr;
        const bool use_tone = cJSON_IsTrue(tone_it);
        if (a) cJSON_Delete(a);
        const int meas = std::max(1, secs - settle);

        float idle_mic = 0, idle_ref = 0, idle_clean = 0;
        afe_metrics_begin();
        vTaskDelay(pdMS_TO_TICKS(1000));
        afe_metrics_end(&idle_mic, &idle_ref, &idle_clean);

        board_set_volume(vol);
        board_pa_enable(true);

        static int16_t blk[256];
        uint32_t rng = 0x12345678;
        double phase = 0.0;
        const double inc = 2.0 * M_PI * 1000.0 / 16000.0;
        auto fill = [&]() {
            for (int i = 0; i < 256; ++i) {
                if (use_tone) { blk[i] = (int16_t)(9000.0 * sin(phase)); phase += inc; }
                else { rng = rng * 1664525u + 1013904223u; blk[i] = (int16_t)((int32_t)(rng >> 16) % 9000); }
            }
        };
        auto burst = [&](int seconds) {
            const int blocks = seconds * 16000 / 256;
            for (int b = 0; b < blocks; ++b) { fill(); board_spk_write(blk, sizeof(blk)); }
        };

        // window 1: converging
        float s_mic = 0, s_ref = 0, s_clean = 0;
        afe_metrics_begin();
        burst(settle);
        afe_metrics_end(&s_mic, &s_ref, &s_clean);

        // window 2: converged
        float m_mic = 0, m_ref = 0, m_clean = 0;
        afe_metrics_begin();
        burst(meas);
        afe_metrics_end(&m_mic, &m_ref, &m_clean);
        board_pa_enable(false);

        auto erle = [](float in, float out) {
            return (in > 0.5f && out > 0.5f) ? 20.0f * log10f(in / out) : 0.0f;
        };
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "done|erle_db=%.1f settle_erle_db=%.1f "
                 "mic=%.1f ref=%.1f clean=%.1f settle_mic=%.1f settle_clean=%.1f "
                 "idle_mic=%.1f idle_ref=%.1f idle_clean=%.1f src=%s secs=%d settle=%d vol=%d",
                 erle(m_mic, m_clean), erle(s_mic, s_clean),
                 m_mic, m_ref, m_clean, s_mic, s_clean,
                 idle_mic, idle_ref, idle_clean,
                 use_tone ? "tone" : "noise", secs, settle, vol);
        return std::string(buf);
    }
    if (cmd.action == "mic_asr_test") {
        if (player->is_playing()) return "failed|busy";
        cJSON* args = cJSON_Parse(cmd.args_json.c_str());
        cJSON* secs = args ? cJSON_GetObjectItem(args, "seconds") : nullptr;
        int seconds = cJSON_IsNumber(secs) ? std::max(1, std::min(10, (int)secs->valuedouble)) : 4;
        if (args) cJSON_Delete(args);
        esp_err_t err = player->run_mic_asr_test(seconds, s_volume);
        if (err != ESP_OK) return "failed|stage=mic_asr error=" + player->last_error();
        return "done|asr_text=" + player->last_asr_text();
    }
    return "unsupported";
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "reset_reason=%d", (int)esp_reset_reason());
    ESP_ERROR_CHECK(nvs_flash_init());
    load_persisted_settings();

    // LCD enabled in v15 - JTAG disabled in sdkconfig to free GPIO 5-8
    lcd_init();
    lcd_print_line(0, "ESP32  BOOTING  ");
    lcd_print_line(1, "Connecting WiFi ");

    init_wifi();
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected");
    if (!wait_for_default_gateway()) {
        ESP_LOGW(TAG, "network route is not ready; waiting for a fresh IP event");
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        wait_for_default_gateway();
    }
    sync_clock();
    // Mark OTA partition as valid so new OTA can be started
    esp_ota_mark_app_valid_cancel_rollback();
    AudioPlayer audio_player;
    s_audio_player = &audio_player;
    xTaskCreate(mic_asr_button_task, "mic_asr_btn", 8192, nullptr, 5, nullptr);

    // esp-sr AFE full-duplex voice path (mic -> AEC -> /ws/audio, TTS back).
    afe_board_bind_codec(audio_player.codec());
    if (board_init() == ESP_OK &&
        playback_init(aec_on_tts_state) == ESP_OK &&
        ws_client_start() == ESP_OK &&
        afe_pipeline_init(aec_on_clean_audio) == ESP_OK) {
        ESP_LOGI(TAG, "AEC full-duplex pipeline started");
    } else {
        ESP_LOGE(TAG, "AEC pipeline init failed; continuing half-duplex");
    }
    DeviceHubClient hub(
        CONFIG_DEVICE_HUB_BASE_URL,
        CONFIG_DEVICE_ID,
        CONFIG_DEVICE_NAME,
        CONFIG_DEVICE_FIRMWARE_VERSION,
        device_state,
        [&audio_player](const HubCommand& cmd) { return handle_command(cmd, &audio_player); }
    );
    int retry_seconds = 10;
    while (hub.register_device() != ESP_OK) {
        ESP_LOGW(TAG, "device registration failed; retrying in %d seconds", retry_seconds);
        vTaskDelay(pdMS_TO_TICKS(retry_seconds * 1000));
        retry_seconds = std::min(retry_seconds * 2, 120);
    }
    // 注册成功后切换为 ONLINE 状态
    lcd_print_line(0, "ESP32  ONLINE   ");
    lcd_print_line(1, s_lcd_ip);

    ESP_LOGI(TAG, "device hub client started (no token auth in v1)");
    MqttControlClient mqtt("ws://110.40.154.41/mqtt", CONFIG_DEVICE_ID,
                           [&audio_player](const HubCommand& cmd) {
                               return handle_command(cmd, &audio_player);
                           });
    s_mqtt_control = &mqtt;
    if (mqtt.start() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT control start failed; HTTP heartbeat remains active");
    }
    hub.boot_announce();
    while (true) {
        if (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) {
            hub.heartbeat();
            // 每次心跳轮播 Line2：IP ↔ OTA版本（约每 5s 切换一次）
            char line2[17];
            if (s_lcd_tick % 2 == 0) {
                snprintf(line2, sizeof(line2), "%-16s", s_lcd_ip);
            } else {
                snprintf(line2, sizeof(line2), "OTA:%-12s", lcd_firmware_label());
            }
            lcd_print_line(1, line2);
            s_lcd_tick++;
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected; heartbeat skipped");
        }
        vTaskDelay(pdMS_TO_TICKS(s_mqtt_control != nullptr &&
                                 s_mqtt_control->is_connected() ? 5000 : 2000));
    }
}
