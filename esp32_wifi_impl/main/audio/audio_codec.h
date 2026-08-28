#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <driver/i2s_std.h>

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

typedef struct AudioCodec AudioCodec;

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*audio_codec_read_fn)(AudioCodec* codec, int16_t* dest, int samples);
typedef int (*audio_codec_write_fn)(AudioCodec* codec, const int16_t* data, int samples);
typedef void (*audio_codec_enable_fn)(AudioCodec* codec, bool enable);
typedef void (*audio_codec_set_output_volume_fn)(AudioCodec* codec, int volume);
typedef void (*audio_codec_destroy_fn)(AudioCodec* codec);

struct AudioCodec {
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    bool duplex;
    bool input_reference;
    bool input_enabled;
    bool output_enabled;
    int input_sample_rate;
    int output_sample_rate;
    int input_channels;
    int output_channels;
    int output_volume;
    float input_gain;

    audio_codec_read_fn read;
    audio_codec_write_fn write;
    audio_codec_enable_fn enable_input_impl;
    audio_codec_enable_fn enable_output_impl;
    audio_codec_set_output_volume_fn set_output_volume_impl;
    audio_codec_destroy_fn destroy_impl;
};

AudioCodec* AudioCodec_GetInstance(void);
void AudioCodec_SetInstance(AudioCodec* codec);

void AudioCodec_Start(AudioCodec* codec);
void AudioCodec_SetOutputVolume(AudioCodec* codec, int volume);
void AudioCodec_SetInputGain(AudioCodec* codec, float gain);
void AudioCodec_EnableInput(AudioCodec* codec, bool enable);
void AudioCodec_EnableOutput(AudioCodec* codec, bool enable);
void AudioCodec_OutputData(AudioCodec* codec, const int16_t* data, size_t samples);
bool AudioCodec_InputData(AudioCodec* codec, int16_t* data, size_t samples);

bool AudioCodec_GetOutputEnabled(const AudioCodec* codec);
bool AudioCodec_GetInputEnabled(const AudioCodec* codec);
int AudioCodec_GetInputChannels(const AudioCodec* codec);
int AudioCodec_GetInputSampleRate(const AudioCodec* codec);
bool AudioCodec_GetInputReference(const AudioCodec* codec);

#ifdef __cplusplus
}
#endif

#endif
