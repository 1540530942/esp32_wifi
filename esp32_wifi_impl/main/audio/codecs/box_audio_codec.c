#include "box_audio_codec.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <stdlib.h>

#define TAG "BoxAudioCodec"

typedef struct {
    AudioCodec base;
    const audio_codec_data_if_t* data_if;
    const audio_codec_ctrl_if_t* out_ctrl_if;
    const audio_codec_if_t* out_codec_if;
    const audio_codec_ctrl_if_t* in_ctrl_if;
    const audio_codec_if_t* in_codec_if;
    const audio_codec_gpio_if_t* gpio_if;
    esp_codec_dev_handle_t output_dev;
    esp_codec_dev_handle_t input_dev;
} BoxAudioCodec;

static BoxAudioCodec* box_cast(AudioCodec* codec) {
    return (BoxAudioCodec*)codec;
}

static void box_create_duplex_channels(AudioCodec* codec, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)codec->output_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}
        }
    };
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)codec->input_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}
        }
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &codec->tx_handle, &codec->rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(codec->rx_handle, &tdm_cfg));
}

static int box_read(AudioCodec* codec, int16_t* dest, int samples) {
    BoxAudioCodec* box = box_cast(codec);
    if (codec->input_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(box->input_dev, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int box_write(AudioCodec* codec, const int16_t* data, int samples) {
    BoxAudioCodec* box = box_cast(codec);
    if (codec->output_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(box->output_dev, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}

static void box_set_output_volume(AudioCodec* codec, int volume) {
    BoxAudioCodec* box = box_cast(codec);
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(box->output_dev, volume));
}

static void box_enable_input(AudioCodec* codec, bool enable) {
    BoxAudioCodec* box = box_cast(codec);
    if (enable == codec->input_enabled) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)codec->output_sample_rate,
            .mclk_multiple = 0,
        };
        if (codec->input_reference) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(box->input_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(box->input_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), codec->input_gain));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(box->input_dev));
    }
    codec->input_enabled = enable;
}

static void box_enable_output(AudioCodec* codec, bool enable) {
    BoxAudioCodec* box = box_cast(codec);
    if (enable == codec->output_enabled) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)codec->output_sample_rate,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(box->output_dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(box->output_dev, codec->output_volume));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(box->output_dev));
    }
    codec->output_enabled = enable;
}

static void box_destroy(AudioCodec* codec) {
    BoxAudioCodec* box = box_cast(codec);
    ESP_ERROR_CHECK(esp_codec_dev_close(box->output_dev));
    esp_codec_dev_delete(box->output_dev);
    ESP_ERROR_CHECK(esp_codec_dev_close(box->input_dev));
    esp_codec_dev_delete(box->input_dev);
    audio_codec_delete_codec_if(box->in_codec_if);
    audio_codec_delete_ctrl_if(box->in_ctrl_if);
    audio_codec_delete_codec_if(box->out_codec_if);
    audio_codec_delete_ctrl_if(box->out_ctrl_if);
    audio_codec_delete_gpio_if(box->gpio_if);
    audio_codec_delete_data_if(box->data_if);
    free(box);
}

AudioCodec* BoxAudioCodec_Create(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
                                 gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                 gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    BoxAudioCodec* box = (BoxAudioCodec*)calloc(1, sizeof(BoxAudioCodec));
    audio_codec_i2s_cfg_t i2s_cfg = {0};
    audio_codec_i2c_cfg_t i2c_cfg = {0};
    es8311_codec_cfg_t es8311_cfg = {0};
    es7210_codec_cfg_t es7210_cfg = {0};
    esp_codec_dev_cfg_t dev_cfg = {0};
    if (box == NULL) {
        return NULL;
    }

    box->base.duplex = true;
    box->base.input_reference = input_reference;
    box->base.input_channels = input_reference ? 2 : 1;
    box->base.output_channels = 1;
    box->base.input_sample_rate = input_sample_rate;
    box->base.output_sample_rate = output_sample_rate;
    box->base.input_gain = 30.0f;
    box->base.output_volume = 90;
    box->base.read = box_read;
    box->base.write = box_write;
    box->base.enable_input_impl = box_enable_input;
    box->base.enable_output_impl = box_enable_output;
    box->base.set_output_volume_impl = box_set_output_volume;
    box->base.destroy_impl = box_destroy;

    box_create_duplex_channels(&box->base, mclk, bclk, ws, dout, din);

    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = box->base.rx_handle;
    i2s_cfg.tx_handle = box->base.tx_handle;
    box->data_if = audio_codec_new_i2s_data(&i2s_cfg);

    i2c_cfg.port = (i2c_port_t)1;
    i2c_cfg.addr = es8311_addr;
    i2c_cfg.bus_handle = i2c_master_handle;
    box->out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    box->gpio_if = audio_codec_new_gpio();
    es8311_cfg.ctrl_if = box->out_ctrl_if;
    es8311_cfg.gpio_if = box->gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    box->out_codec_if = es8311_codec_new(&es8311_cfg);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = box->out_codec_if;
    dev_cfg.data_if = box->data_if;
    box->output_dev = esp_codec_dev_new(&dev_cfg);

    i2c_cfg.addr = es7210_addr;
    box->in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_cfg.ctrl_if = box->in_ctrl_if;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    box->in_codec_if = es7210_codec_new(&es7210_cfg);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = box->in_codec_if;
    box->input_dev = esp_codec_dev_new(&dev_cfg);
    ESP_LOGI(TAG, "BoxAudioCodec initialized");
    return &box->base;
}
