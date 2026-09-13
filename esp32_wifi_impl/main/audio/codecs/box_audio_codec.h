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

// Read one ES7210 register over the input codec's existing I2C control
// interface. Exists because the per-channel PGA gain that the AEC reference
// path depends on (REG43 = MIC1/ch0, REG44 = MIC2/ch1) is never written
// explicitly for ch1 -- esp_codec_dev_open() sets every channel to the
// handle's default gain and box_enable_input() then raises only ch0, so the
// reference channel's real gain has only ever been inferred from the code,
// never measured. Returns 0 on success and writes the byte to *value.
int BoxAudioCodec_ReadInputReg(AudioCodec* codec, int reg, uint8_t* value);

#ifdef __cplusplus
}
#endif

#endif
