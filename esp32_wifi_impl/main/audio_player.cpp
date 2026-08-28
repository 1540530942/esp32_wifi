#include "audio_player.h"

#include "driver/i2s.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

static const char* TAG = "audio_player";
static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
static constexpr int SAMPLE_RATE = 16000;
static constexpr int BCLK_GPIO = 2;
static constexpr int LRCK_GPIO = 1;
static constexpr int DOUT_GPIO = 48;

AudioPlayer::AudioPlayer() {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 8;
    config.dma_buf_len = 256;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 0;
    if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK) return;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = BCLK_GPIO;
    pins.ws_io_num = LRCK_GPIO;
    pins.data_out_num = DOUT_GPIO;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    initialized_ = i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
    ESP_LOGI(TAG, "I2S audio output %s, volume=%d%%", initialized_ ? "ready" : "failed", 20);
}

esp_err_t AudioPlayer::play_wav_url(const std::string& url, uint8_t volume_percent) {
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
    int length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300 || length < 44) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t header[44] = {};
    int got = esp_http_client_read(client, reinterpret_cast<char*>(header), sizeof(header));
    bool pcm16mono = got >= 44 && std::memcmp(header, "RIFF", 4) == 0 &&
                     std::memcmp(header + 8, "WAVE", 4) == 0 &&
                     header[20] == 1 && header[22] == 1 && header[34] == 16;
    if (!pcm16mono) {
        ESP_LOGW(TAG, "unsupported WAV; expected PCM 16-bit mono");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t input[1024] = {};
    int read = 0;
    const int scale = std::min<int>(volume_percent, 100);
    while ((read = esp_http_client_read(client, reinterpret_cast<char*>(input), sizeof(input))) > 0) {
        int samples = read / 2;
        auto* pcm = reinterpret_cast<int16_t*>(input);
        for (int i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>((pcm[i] * scale) / 100);
        size_t written = 0;
        err = i2s_write(I2S_PORT, input, samples * 2, &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) break;
    }
    i2s_zero_dma_buffer(I2S_PORT);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "finished WAV playback at %d%% volume", scale);
    return err == ESP_OK ? ESP_OK : err;
}
