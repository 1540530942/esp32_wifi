#include "device_hub_client.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <cstring>

static const char* TAG = "wangyutang_app";
static EventGroupHandle_t wifi_events;
static constexpr int WIFI_CONNECTED_BIT = BIT0;

// Volume state tracked locally so heartbeat can report it
static int s_volume = 60;

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
}

static void init_wifi() {
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, nullptr));
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid),
                 CONFIG_DEVICE_WIFI_SSID, sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password),
                 CONFIG_DEVICE_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Wait for WiFi; returns false if timeout exceeded
static bool wait_wifi(uint32_t timeout_ms = 30000) {
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
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
    cJSON_AddStringToObject(state, "firmware",  CONFIG_DEVICE_FIRMWARE_VERSION);
    cJSON_AddStringToObject(state, "wifi_ssid", reinterpret_cast<char*>(ap.ssid));
    cJSON_AddNumberToObject(state, "wifi_rssi", ap.rssi);
    cJSON_AddStringToObject(state, "ip",        ip_text);
    cJSON_AddNumberToObject(state, "uptime_s",  (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(state, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(state, "volume",    s_volume);
    cJSON_AddBoolToObject(  state, "activated", false);  // XiaoZhi activation TBD
    char* text = cJSON_PrintUnformatted(state);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(state);
    return result;
}

// Command handler — returns "done" | "failed" | "unsupported"
static std::string handle_command(const HubCommand& cmd) {
    ESP_LOGI(TAG, "executing: %s args=%s", cmd.action.c_str(), cmd.args_json.c_str());

    if (cmd.action == "reboot") {
        ESP_LOGI(TAG, "rebooting in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return "done";  // unreachable
    }

    if (cmd.action == "identify") {
        // TODO: flash onboard LED or beep speaker when GPIO is wired
        ESP_LOGI(TAG, "identify: blink/beep placeholder");
        return "done";
    }

    if (cmd.action == "set_volume") {
        cJSON* args = cJSON_Parse(cmd.args_json.c_str());
        cJSON* val  = args ? cJSON_GetObjectItem(args, "value") : nullptr;
        if (cJSON_IsNumber(val)) {
            int v = (int)val->valuedouble;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            s_volume = v;
            ESP_LOGI(TAG, "volume set to %d", s_volume);
            // TODO: apply to audio codec when XiaoZhi audio engine integrated
            cJSON_Delete(args);
            return "done";
        }
        if (args) cJSON_Delete(args);
        return "failed";
    }

    // OTA and other future actions
    ESP_LOGW(TAG, "unsupported action: %s", cmd.action.c_str());
    return "unsupported";
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_wifi();

    ESP_LOGI(TAG, "waiting for Wi-Fi...");
    if (!wait_wifi(60000)) {
        ESP_LOGE(TAG, "Wi-Fi timeout, restarting");
        esp_restart();
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    DeviceHubClient hub(
        CONFIG_DEVICE_HUB_BASE_URL,
        CONFIG_DEVICE_ID,
        CONFIG_DEVICE_NAME,
        CONFIG_DEVICE_FIRMWARE_VERSION,
        device_state,
        handle_command
    );

    hub.register_device();

    while (true) {
        // Wait for Wi-Fi before each heartbeat (handles reconnects)
        if (!wait_wifi(10000)) {
            ESP_LOGW(TAG, "no Wi-Fi, skipping heartbeat");
        } else {
            hub.heartbeat();
        }
        // Contract: heartbeat_interval_s = 5
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
