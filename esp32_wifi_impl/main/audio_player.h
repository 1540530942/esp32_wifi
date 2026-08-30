#pragma once

#include "audio/audio_codec.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <atomic>
#include <string>

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    esp_err_t play_wav_url(const std::string& url, uint8_t volume_percent = 30);
    esp_err_t play_pcm_url(const std::string& url, uint8_t volume_percent = 30,
                           int pa_level = 1);
    esp_err_t stop();
    bool is_playing() const { return task_ != nullptr; }
    esp_err_t last_result() const { return last_result_; }
    const std::string& last_error() const { return last_error_; }
    size_t last_pcm_bytes() const { return last_pcm_bytes_.load(); }
    int64_t last_pcm_elapsed_ms() const { return last_pcm_elapsed_ms_.load(); }
    int last_pcm_pa_level() const { return last_pcm_pa_level_.load(); }

private:
    static void task_entry(void* arg);
    void play_task();
    esp_err_t play_wav_stream(const std::string& url, uint8_t volume_percent);
    esp_err_t play_wav_stream_raw_http(const std::string& url, uint8_t volume_percent);
    esp_err_t play_pcm_stream_websocket(const std::string& url, uint8_t volume_percent,
                                        int pa_level);
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* codec_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool stop_requested_ = false;
    volatile esp_err_t last_result_ = ESP_OK;
    std::string pending_url_;
    uint8_t pending_volume_ = 30;
    bool pending_pcm_ = false;
    int pending_pa_level_ = 1;
    std::atomic<size_t> last_pcm_bytes_{0};
    std::atomic<int64_t> last_pcm_elapsed_ms_{0};
    std::atomic<int> last_pcm_pa_level_{-1};
    std::string last_error_;
};
