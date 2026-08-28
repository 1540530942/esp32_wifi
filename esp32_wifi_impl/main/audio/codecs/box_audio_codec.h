#ifndef BOX_AUDIO_CODEC_H
#define BOX_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

AudioCodec* BoxAudioCodec_Create(void* i2c_master_handle,
                                 int input_sample_rate,
                                 int output_sample_rate,
                                 gpio_num_t mclk,
                                 gpio_num_t bclk,
                                 gpio_num_t ws,
                                 gpio_num_t dout,
                                 gpio_num_t din,
                                 gpio_num_t pa_pin,
                                 uint8_t es8311_addr,
                                 uint8_t es7210_addr,
                                 bool input_reference);

#ifdef __cplusplus
}
#endif

#endif
