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
// Current output gain in percent, 100 unless barge-in has ducked. Exposed so
// anything that writes to the speaker outside this module -- the far_end test
// fixture -- can honour the duck too, instead of playing straight through it.
int playback_gain_pct(void);

// Mark the speaker as busy with audio this module did not queue -- the far_end
// test fixture writes to the codec directly. Barge-in is gated on
// playback_is_playing(), so without this the detector never even evaluates while
// the fixture is playing, and no interruption can be measured. Also restarts the
// turn clock that the onset grace is measured against.
void playback_set_external_active(bool active);

// Barge-in must be able to stop audio this module did not queue. Ducking only
// scales this module's own output gain, which does nothing to a player that
// writes to the codec itself -- AudioPlayer (every play_audio / play_lan_audio
// / speak_pcm the robot performs) and the far_end fixture both do. Register a
// stop callback and the barge-in paths will invoke it alongside their own
// duck/kill, so an interruption reaches whatever is actually making sound.
typedef void (*playback_external_stop_cb_t)(void);
void playback_set_external_stop_cb(playback_external_stop_cb_t cb);

void playback_duck(void);
void playback_unduck(void);

// What the local (level-1) barge-in detector should call: ducks this module's
// own queue and stops any registered external player in one go.
void playback_barge_in(void);

// Barge-in level 2: drop queued + current audio immediately, discard until the
// next playback_begin_turn(), and stop any registered external player. Use this
// only for an actual interruption.
void playback_kill(void);

// Connection teardown: same queue cleanup, but leaves external playback alone.
// A dropped WebSocket means the TTS stream is gone, not that the user
// interrupted -- truncating an unrelated local play_audio on a network blip is
// a real failure mode, not a hypothetical one.
void playback_discard_stream(void);

// True while a chunk is playing or chunks are queued.
bool playback_is_playing(void);

// Monotonic ms since the current turn began (for onset grace).
int64_t playback_turn_age_ms(void);
