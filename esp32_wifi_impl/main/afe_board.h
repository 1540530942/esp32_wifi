// Board shim: the AFE reference code (afe_pipeline.c / playback.c) is written
// against a small board_* API. Here it is backed by the existing BoxAudioCodec
// instance that AudioPlayer already brings up (ES7210 in [mic,ref], ES8311 out),
// instead of pulling in esp_codec_dev a second time.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once from app_main with the codec AudioPlayer created, before board_init().
void afe_board_bind_codec(AudioCodec *codec);

esp_err_t board_init(void);
esp_err_t board_mic_read(void *buf, size_t bytes, size_t *out_read);
esp_err_t board_spk_write(const void *buf, size_t bytes);
void board_set_volume(int pct);
void board_pa_enable(bool on);

#ifdef __cplusplus
}
#endif
