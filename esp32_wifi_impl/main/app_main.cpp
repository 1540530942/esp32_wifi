#include "device_hub_client.h"
#include "ota_updater.h"
#include "audio_player.h"
#include "mqtt_control_client.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
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
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(arg);
        ESP_LOGI(TAG, "Wi-Fi got IP=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
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
    // Keep the radio awake while the device maintains the long-lived cloud
    // control link.  Some consumer APs/ISP gateways drop the first TCP
    // exchange when an ESP station enters modem-sleep immediately after
    // association.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static bool sync_clock_from_http_date() {
    // The site's HTTP redirect includes a Date header. Use it only to set the
    // initial clock; all registration/audio traffic remains HTTPS.
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
            // Convert the UTC date without relying on the optional timegm()
            // libc extension, which is not provided by ESP-IDF newlib.
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
    // HTTPS certificate validation requires a real wall clock. The ESP32
    // starts at epoch 0 after reset, so synchronize before the first request.
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
            // The ISP path currently blocks TCP/443.  The platform has a
            // direct-IP HTTP listener; preserve the cloud URL contract while
            // using that listener for this network.
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
        // Reuse the play_audio contract: rewrite the HTTPS cloud host to the
        // direct-IP HTTP listener, since this ISP blocks TCP/443.
        std::string ota_url = ota_url_item->valuestring;
        constexpr const char* kOtaHttpsHost = "https://www.wangyutang.cn";
        if (ota_url.rfind(kOtaHttpsHost, 0) == 0) {
            ota_url = std::string("http://110.40.154.41") + ota_url.substr(std::strlen(kOtaHttpsHost));
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
    return "unsupported";
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "reset_reason=%d", (int)esp_reset_reason());
    ESP_ERROR_CHECK(nvs_flash_init());
    load_persisted_settings();
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
    AudioPlayer audio_player;
    s_audio_player = &audio_player;
    DeviceHubClient hub(
        CONFIG_DEVICE_HUB_BASE_URL,
        CONFIG_DEVICE_ID,
        CONFIG_DEVICE_NAME,
        CONFIG_DEVICE_FIRMWARE_VERSION,
        device_state,
        [&audio_player](const HubCommand& cmd) { return handle_command(cmd, &audio_player); }
    );
    // Keep retrying registration so a transient WAN/DNS/TLS failure does not
    // leave the device permanently absent from the platform.
    int retry_seconds = 10;
    while (hub.register_device() != ESP_OK) {
        ESP_LOGW(TAG, "device registration failed; retrying in %d seconds", retry_seconds);
        vTaskDelay(pdMS_TO_TICKS(retry_seconds * 1000));
        retry_seconds = std::min(retry_seconds * 2, 120);
    }
    ESP_LOGI(TAG, "device hub client started (no token auth in v1)");
    // MQTT over WebSocket uses the already reachable public HTTP entrypoint.
    // The current integration intentionally has no authentication.
    MqttControlClient mqtt("wss://www.wangyutang.cn/mqtt", CONFIG_DEVICE_ID,
                           [&audio_player](const HubCommand& cmd) {
                               return handle_command(cmd, &audio_player);
                           });
    s_mqtt_control = &mqtt;
    if (mqtt.start() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT control start failed; HTTP heartbeat remains active");
    }
    while (true) {
        if (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) {
            hub.heartbeat();
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected; heartbeat skipped");
        }
        // MQTT is the primary control plane; keep HTTP heartbeat as a
        // low-rate presence/fallback channel so it does not compete with a
        // WSS PCM handshake for the ESP32 socket/TLS resources.
        vTaskDelay(pdMS_TO_TICKS(s_mqtt_control != nullptr &&
                                 s_mqtt_control->is_connected() ? 5000 : 2000));
    }
}
