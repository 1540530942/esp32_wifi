// WebSocket client to the cloud (audio_interact /ws/audio). Sends clean PCM
// upstream and TTS/control down, matching the existing edge protocol (proto 2).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ws_client_start(void);

// Send one clean 16 kHz mono PCM block upstream. Safe to call before the socket
// is up (drops until stream_ready). Called from the AFE fetch task.
void ws_client_send_audio(const int16_t *pcm, size_t bytes);

// Report playback state to the cloud (drives its VAD double-profile / barge-in).
// Wire this as the playback state callback.
void ws_client_report_tts_state(bool playing);
