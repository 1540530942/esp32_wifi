#include "ota_updater.h"

#include <cstring>

#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char* TAG = "ota";

esp_err_t ota_run_from_url(const char* url) {
    if (!url || !*url) {
        ESP_LOGE(TAG, "empty OTA url");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "starting OTA from %s", url);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.timeout_ms = 60000;
    http_cfg.keep_alive_enable = true;
    // Attach the certificate bundle only for TLS URLs. The direct-IP HTTP path
    // (used on this ISP because TCP/443 is blocked) needs no certificate.
    if (std::strncmp(url, "https://", 8) == 0) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA image written to inactive slot; ready to reboot");
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
