#include "device_hub_client.h"
#include "audio_player.h"

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
#include <cstring>\n#include <cstdio>

static const char* TAG = "wangyutang_app";
static EventGroupHandle_t wifi_events;
static constexpr int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
}

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
    cJSON_AddStringToObject(state, "firmware", "esp32-wangyutang-v1");
    cJSON_AddStringToObject(state, "wifi_ssid", reinterpret_cast<char*>(ap.ssid));
    cJSON_AddNumberToObject(state, "wifi_rssi", ap.rssi);
    cJSON_AddStringToObject(state, "ip", ip_text);
    cJSON_AddNumberToObject(state, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(state, "free_heap", esp_get_free_heap_size());
    char* text = cJSON_PrintUnformatted(state);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(state);
    return result;
}

static void local_console_task(void* arg) {
    auto* player = static_cast<AudioPlayer*>(arg);
    std::printf("\r\nLocal audio console ready. Type play + Enter.\r\n");
    char line[64] = {};
    while (true) {
        if (std::fgets(line, sizeof(line), stdin)) {
            if (std::strncmp(line, "play", 4) == 0) {
                std::printf("Playing at 20%% volume...\r\n");
                esp_err_t err = player->play_wav_url(
                    "https://raw.githubusercontent.com/1540530942/esp32_wifi/main/%E4%BD%A0%E4%BB%8A%E5%A4%A9%E5%A5%BD%E5%90%97.wav",
                    20);
                std::printf("Playback %s\r\n", err == ESP_OK ? "done" : "failed");
            } else {
                std::printf("Command: play\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_wifi();
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected");
    AudioPlayer audio_player;

    xTaskCreate(local_console_task, "local_console", 4096, &audio_player, 5, nullptr);
    ESP_LOGI(TAG, "Local-only mode: platform connection disabled");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
