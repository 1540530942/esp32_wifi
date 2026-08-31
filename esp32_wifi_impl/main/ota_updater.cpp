#include "ota_updater.h"

#include <cstring>

#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"

static const char* TAG = "ota";

esp_err_t ota_run_from_url(const char* url, const OtaProgressCallback& progress) {
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

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t last_reported = 0;
    do {
        ret = esp_https_ota_perform(handle);
        const int bytes_read = esp_https_ota_get_image_len_read(handle);
        const int image_size = esp_https_ota_get_image_size(handle);
        if (progress && bytes_read >= 0 &&
            (static_cast<size_t>(bytes_read) - last_reported >= 64 * 1024 || ret == ESP_OK)) {
            last_reported = static_cast<size_t>(bytes_read);
            progress(last_reported, image_size > 0 ? static_cast<size_t>(image_size) : 0);
        }
    } while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (ret == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
        ret = ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (ret == ESP_OK) {
        ret = esp_https_ota_finish(handle);
        handle = nullptr;
    } else {
        esp_https_ota_abort(handle);
        handle = nullptr;
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA image written to inactive slot; ready to reboot");
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
