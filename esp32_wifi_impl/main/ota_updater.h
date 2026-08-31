#pragma once
#include "esp_err.h"
#include <functional>
#include <cstddef>

using OtaProgressCallback = std::function<void(size_t bytes_read, size_t image_size)>;

// Self-contained WiFi OTA helper.
//
// Decoupled by design: depends only on esp_https_ota / esp_http_client, and has
// no knowledge of device_hub, audio, or MQTT. Downloads a firmware image over
// HTTP(S), verifies it, and writes it to the currently-inactive OTA slot,
// leaving the running image untouched. On ESP_OK the caller should reboot; the
// bootloader then starts the new slot (with automatic rollback if it fails to
// validate). Existing device functionality is unaffected until that reboot.
esp_err_t ota_run_from_url(const char* url, const OtaProgressCallback& progress = {});
