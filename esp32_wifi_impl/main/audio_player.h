#pragma once

#include "audio/audio_codec.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <string>

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    esp_err_t play_wav_url(const std::string& url, uint8_t volume_percent = 20);
    esp_err_t stop();
    bool is_playing() const { return task_ != nullptr; }

private:
    static void task_entry(void* arg);
    void play_task();
    esp_err_t play_wav_stream(const std::string& url, uint8_t volume_percent);
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* codec_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool stop_requested_ = false;
    std::string pending_url_;
    uint8_t pending_volume_ = 20;
};
