#include "afe_board.h"
#include "aec_config.h"
#include "audio_board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "afe_board";
static AudioCodec *s_codec;

void afe_board_bind_codec(AudioCodec *codec) { s_codec = codec; }

esp_err_t board_init(void)
{
    if (!s_codec) { ESP_LOGE(TAG, "codec not bound"); return ESP_ERR_INVALID_STATE; }
    // Codec + I2S already started by AudioPlayer's constructor; just set the
    // AEC-path speaker volume and make sure input is enabled.
    AudioCodec_SetOutputVolume(s_codec, CONFIG_AEC_SPEAKER_VOLUME);
    AudioCodec_EnableInput(s_codec, true);
    // Full-duplex on one I2S controller: RX is the clock slave, so it only
    // gets BCLK/WS while the TX path is enabled. Keep output open (playback.c
    // gates the actual amp via board_pa_enable) or the AFE feed starves.
    AudioCodec_EnableOutput(s_codec, true);
    ESP_LOGI(TAG, "board ready (BoxAudioCodec: %dch in, ES8311 out, %d Hz)",
             AudioCodec_GetInputChannels(s_codec), AudioCodec_GetInputSampleRate(s_codec));
    return ESP_OK;
}

esp_err_t board_mic_read(void *buf, size_t bytes, size_t *out_read)
{
    size_t samples = bytes / sizeof(int16_t);
    bool ok = AudioCodec_InputData(s_codec, (int16_t *)buf, samples);
    if (out_read) *out_read = ok ? bytes : 0;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t board_spk_write(const void *buf, size_t bytes)
{
    int n = AudioCodec_OutputData(s_codec, (const int16_t *)buf, bytes / sizeof(int16_t));
    return n >= 0 ? ESP_OK : ESP_FAIL;
}

void board_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    AudioCodec_SetOutputVolume(s_codec, pct);
}

void board_pa_enable(bool on)
{
    AudioCodec_EnableOutput(s_codec, on);
#ifdef AUDIO_CODEC_PA_PIN
    gpio_set_level(AUDIO_CODEC_PA_PIN, on ? 1 : 0);
#endif
}
