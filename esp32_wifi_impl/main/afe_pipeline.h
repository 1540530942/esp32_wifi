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

// --- ERLE measurement window -------------------------------------------------
// afe_metrics_begin() zeroes the accumulators and starts collecting; the feed
// task sums the raw mic + hardware-reference channels, the fetch task sums the
// AFE's clean output. afe_metrics_end() returns the RMS of each over the window,
// so ERLE = 20*log10(mic_rms / clean_rms) during a known echo burst.
void afe_metrics_begin(void);
void afe_metrics_end(float *mic_rms, float *ref_rms, float *clean_rms);

// --- raw/clean capture ------------------------------------------------------
// Records the two things you need to hear an AEC working: the raw microphone
// channel (echo included, pre-AFE) and the AFE's clean output (post-AEC), plus
// the hardware reference channel. Buffers are caller-owned int16 mono @16 kHz.
// --- live config readback ---------------------------------------------------
// Writes the AFE settings that actually took effect (after afe_config_check(),
// which silently rewrites conflicting fields) into buf as "k=v k=v ...".
// Reading these off the serial console needs USB; this lets the same facts come
// back over MQTT in a command reply, so a headless board can be verified.
void afe_config_summary(char *buf, size_t n);

void afe_capture_begin(int16_t *mic, int16_t *ref, int16_t *clean, size_t cap_samples);
void afe_capture_end(size_t *mic_n, size_t *ref_n, size_t *clean_n);
