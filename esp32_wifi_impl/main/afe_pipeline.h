// esp-sr AFE pipeline: reads the ES7210 TDM stream (mics + reference), runs
// acoustic echo cancellation + noise suppression + VAD, and emits clean 16 kHz
// mono PCM for uplink. Also drives on-device (level-1) barge-in.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Called (from the fetch task) with each clean 16 kHz mono PCM block.
typedef void (*afe_audio_cb_t)(const int16_t *pcm, size_t bytes);

esp_err_t afe_pipeline_init(afe_audio_cb_t on_clean_audio);

// Software-reference mode only (CONFIG_AEC_SOFTWARE_REF): playback pushes the
// samples it just played so the AFE has an echo reference. No-op in hardware-
// reference mode (ES7210 captures the loopback directly).
void afe_pipeline_push_ref(const int16_t *pcm, size_t samples);
