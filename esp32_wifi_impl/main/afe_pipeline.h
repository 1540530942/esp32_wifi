// esp-sr AFE pipeline: reads the ES7210 TDM stream (mics + reference), runs
// acoustic echo cancellation + noise suppression + VAD, and emits clean 16 kHz
// mono PCM for uplink. Also drives on-device (level-1) barge-in.
#pragma once

#include <stdbool.h>
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

// --- reference-channel level ------------------------------------------------
// Peak seen on ch1 since the last read, in dBFS, plus how many frames have
// crossed the clipping warning threshold since boot. Exposed because a value
// that only reaches the UART cannot be checked on a headless board -- the same
// limitation already hit with T3.2's dropped-ms figure and E4's latency, and
// this is the third time, so it goes in the heartbeat rather than a log line.
// Returns false if no playback has been observed yet.
bool afe_ref_level(float *peak_dbfs, uint32_t *clip_frames);

// --- barge-in miss attribution ----------------------------------------------
// Counts, since the last afe_click_arm(), how many frames each half of the
// barge-in gate admitted while the robot was speaking and past the onset
// grace, plus the loudest AFE output frame seen. When a barge-in does not
// fire, these separate "the AEC also removed the user's voice" (max_rms below
// the threshold) from "the VAD never called it speech" -- two causes with
// opposite fixes that the ack alone cannot distinguish.
void afe_gate_stats(uint32_t *frames, uint32_t *loud, uint32_t *speech,
                    uint32_t *both, int32_t *max_rms);

// --- E4 barge-in latency ----------------------------------------------------
// E4 measures "user opens their mouth -> playback stops". The stop side was
// already timestamped in the fetch task; this is the other half: a sharp onset
// (the 20 ms click prepended to the Raspberry Pi's fixture) detected in the RAW
// microphone channel, before the AFE, so it is not delayed by AEC/VAD.
//
// Both timestamps come from this device's own esp_timer clock, so no
// cross-device sync is needed -- that is the whole reason the click exists
// rather than timing the Pi's aplay start.
//
// Arm immediately before telling the Pi to play. Detection is one-shot: the
// first qualifying onset records the timestamp and disarms, so the rest of the
// utterance cannot overwrite it.
void afe_click_arm(void);

// Reads back the last armed measurement. Returns false when no click has been
// detected since the last arm. `latency_ms` is -1 until the barge-in fires.
// Exposed as a command reply rather than only a UART log, because a value that
// only reaches the serial console cannot be verified on a headless board.
bool afe_click_result(int64_t *click_us, int64_t *duck_us, int *latency_ms);
