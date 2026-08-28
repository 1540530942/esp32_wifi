#include "audio_player.h"

#include "audio/codecs/box_audio_codec.h"
#include "audio_board_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

static const char* TAG = "audio_player";

AudioPlayer::AudioPlayer() {
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    i2c_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus_));
    codec_ = BoxAudioCodec_Create(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
        AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
        AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
    if (!codec_) {
        ESP_LOGE(TAG, "ES7210/ES8311 codec initialization failed");
        return;
    }
    AudioCodec_SetOutputVolume(codec_, 20);
    AudioCodec_Start(codec_);
    AudioCodec_EnableOutput(codec_, false);
    ESP_LOGI(TAG, "ES7210/ES8311 audio path ready, output volume=20%%");
}

AudioPlayer::~AudioPlayer() {
    stop();
    if (codec_) {
        if (codec_->destroy_impl) codec_->destroy_impl(codec_);
        codec_ = nullptr;
    }
    if (i2c_bus_) i2c_del_master_bus(i2c_bus_);
}

esp_err_t AudioPlayer::play_wav_url(const std::string& url, uint8_t volume_percent) {
    if (!codec_) return ESP_ERR_INVALID_STATE;
    if (task_) return ESP_ERR_INVALID_STATE;
    if (url.rfind("https://", 0) != 0) return ESP_ERR_INVALID_ARG;
    pending_url_ = url;
    pending_volume_ = std::min<uint8_t>(volume_percent, 100);
    stop_requested_ = false;
    return xTaskCreate(&AudioPlayer::task_entry, "audio_play", 8192, this, 5, &task_) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t AudioPlayer::stop() {
    if (task_) stop_requested_ = true;
    return ESP_OK;
}

void AudioPlayer::task_entry(void* arg) {
    static_cast<AudioPlayer*>(arg)->play_task();
    vTaskDelete(nullptr);
}

void AudioPlayer::play_task() {
    const std::string url = pending_url_;
    const uint8_t volume = pending_volume_;
    ESP_LOGI(TAG, "starting WAV playback at %u%%", volume);
    esp_err_t err = play_wav_stream(url, volume);
    ESP_LOGI(TAG, "WAV playback %s", err == ESP_OK ? "finished" :
             (stop_requested_ ? "stopped" : "failed"));
    task_ = nullptr;
    stop_requested_ = false;
}

esp_err_t AudioPlayer::play_wav_stream(const std::string& url, uint8_t volume_percent) {
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
        esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
    }
    uint8_t header[44] = {};
    int got = esp_http_client_read(client, reinterpret_cast<char*>(header), sizeof(header));
    bool pcm16mono = got >= 44 && std::memcmp(header, "RIFF", 4) == 0 &&
                     std::memcmp(header + 8, "WAVE", 4) == 0 && header[20] == 1 &&
                     header[22] == 1 && header[34] == 16;
    if (!pcm16mono) {
        ESP_LOGW(TAG, "unsupported WAV; expected PCM 16-bit mono");
        esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t input[1024] = {};
    int read = 0;
    const int scale = volume_percent;
    AudioCodec_EnableOutput(codec_, true);
    AudioCodec_SetOutputVolume(codec_, scale);
    while (!stop_requested_ && (read = esp_http_client_read(client, reinterpret_cast<char*>(input), sizeof(input))) > 0) {
        int samples = read / 2;
        auto* pcm = reinterpret_cast<int16_t*>(input);
        for (int i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>((pcm[i] * scale) / 100);
        AudioCodec_OutputData(codec_, pcm, samples);
    }
    AudioCodec_EnableOutput(codec_, false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return stop_requested_ ? ESP_ERR_INVALID_STATE : (err == ESP_OK ? ESP_OK : err);
}
