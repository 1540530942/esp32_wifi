#pragma once

#include "esp_err.h"
#include <string>

class AudioPlayer {
public:
    AudioPlayer();
    esp_err_t play_wav_url(const std::string& url, uint8_t volume_percent = 20);

private:
    bool initialized_ = false;
};
