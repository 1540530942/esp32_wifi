#include "audio_codec.h"
#include <esp_log.h>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

static AudioCodec* s_audio_codec = NULL;

AudioCodec* AudioCodec_GetInstance(void) {
    return s_audio_codec;
}

void AudioCodec_SetInstance(AudioCodec* codec) {
    s_audio_codec = codec;
}

void AudioCodec_OutputData(AudioCodec* codec, const int16_t* data, size_t samples) {
    if (codec == NULL || codec->write == NULL || data == NULL || samples == 0) {
        return;
    }
    codec->write(codec, data, (int)samples);
}

bool AudioCodec_InputData(AudioCodec* codec, int16_t* data, size_t samples) {
    int read_samples = 0;
    if (codec == NULL || codec->read == NULL || data == NULL || samples == 0) {
        return false;
    }
    read_samples = codec->read(codec, data, (int)samples);
    return read_samples > 0;
}

void AudioCodec_Start(AudioCodec* codec) {
    if (codec == NULL) {
        return;
    }
    if (codec->output_volume <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is invalid, setting to default (80)", codec->output_volume);
        codec->output_volume = 80;
    }
    if (codec->tx_handle != NULL) {
        ESP_ERROR_CHECK(i2s_channel_enable(codec->tx_handle));
    }
    if (codec->rx_handle != NULL) {
        ESP_ERROR_CHECK(i2s_channel_enable(codec->rx_handle));
    }
    AudioCodec_EnableInput(codec, true);
    AudioCodec_EnableOutput(codec, true);
    ESP_LOGI(TAG, "Audio codec started, output volume=%d", codec->output_volume);
}

void AudioCodec_SetOutputVolume(AudioCodec* codec, int volume) {
    if (codec == NULL) {
        return;
    }
    codec->output_volume = volume;
    if (codec->set_output_volume_impl != NULL) {
        codec->set_output_volume_impl(codec, volume);
    }
    ESP_LOGI(TAG, "Set output volume to %d", codec->output_volume);
}

void AudioCodec_SetInputGain(AudioCodec* codec, float gain) {
    if (codec == NULL) {
        return;
    }
    codec->input_gain = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", codec->input_gain);
}

void AudioCodec_EnableInput(AudioCodec* codec, bool enable) {
    if (codec == NULL) {
        return;
    }
    if (codec->enable_input_impl != NULL) {
        codec->enable_input_impl(codec, enable);
    } else {
        codec->input_enabled = enable;
    }
}

void AudioCodec_EnableOutput(AudioCodec* codec, bool enable) {
    if (codec == NULL) {
        return;
    }
    if (codec->enable_output_impl != NULL) {
        codec->enable_output_impl(codec, enable);
    } else {
        codec->output_enabled = enable;
    }
}

bool AudioCodec_GetOutputEnabled(const AudioCodec* codec) {
    return codec != NULL && codec->output_enabled;
}

bool AudioCodec_GetInputEnabled(const AudioCodec* codec) {
    return codec != NULL && codec->input_enabled;
}

int AudioCodec_GetInputChannels(const AudioCodec* codec) {
    return codec != NULL ? codec->input_channels : 1;
}

int AudioCodec_GetInputSampleRate(const AudioCodec* codec) {
    return codec != NULL ? codec->input_sample_rate : 16000;
}

bool AudioCodec_GetInputReference(const AudioCodec* codec) {
    return codec != NULL && codec->input_reference;
}
