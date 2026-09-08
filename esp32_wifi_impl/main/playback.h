// TTS playback: a queue of WAV chunks (one per streamed sentence) played to the
// ES8311 speaker, with barge-in support (duck = lower volume, killable; kill =
// drop everything immediately). Chunks are parsed and resampled to 16 kHz mono.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Called (from the playback task) whenever the playing state flips, so the WS
// client can report tts_state to the cloud.
typedef void (*playback_state_cb_t)(bool playing);

esp_err_t playback_init(playback_state_cb_t on_state_change);

// Cloud signalled a new TTS stream: accept chunks again, mark the turn start
// (used for barge-in onset grace).
void playback_begin_turn(void);

// Copy and enqueue one TTS WAV chunk. Ignored if the current turn was killed
// (until the next playback_begin_turn). Returns ESP_ERR_NO_MEM if the queue is full.
esp_err_t playback_enqueue_wav(const uint8_t *wav, size_t len);

// Barge-in level 1: lower volume, recoverable.
void playback_duck(void);
void playback_unduck(void);

// Barge-in level 2: drop queued + current audio immediately, discard until the
// next playback_begin_turn().
void playback_kill(void);

// True while a chunk is playing or chunks are queued.
bool playback_is_playing(void);

// Monotonic ms since the current turn began (for onset grace).
int64_t playback_turn_age_ms(void);
