// Global compile-time constants for the esp-sr AFE full-duplex path.
#pragma once
#include "sdkconfig.h"

// Everything downstream of the codec runs at 16 kHz / 16-bit / mono — the rate
// esp-sr AFE operates at and the rate audio_interact /ws/audio expects.
#define AEC_SAMPLE_RATE_HZ   16000
#define AEC_BITS_PER_SAMPLE  16
// BoxAudioCodec with input_reference=true delivers 2 interleaved channels:
// [mic, echo-reference]. AFE input format is CONFIG_AEC_AFE_INPUT_FORMAT ("MR").
#define AEC_MIC_CHANNELS     2

// Echo-suppression aggressiveness of the AEC's nonlinear processor
// (aec_nlp_level_t in esp_aec_nlp.h: NORMAL=0, AGGR=1, VERYAGGR=2).
// AGGR is the AFE default and is what every result up to and including
// docs/aec/TTS_DOUBLETALK.md was measured with. It suppresses hard enough that
// a weak near end gets taken along with the echo; NORMAL backs that off,
// trading residual echo for near-end fidelity.
#ifndef AEC_NLP_LEVEL
#define AEC_NLP_LEVEL AEC_NLP_LEVEL_AGGR
#endif

#define AEC_WS_PROTO         2
#define AEC_PLAYBACK_QUEUE_LEN   12
#define AEC_PLAYBACK_CHUNK_MAX   (64 * 1024)

#define AEC_SERVER_URI       CONFIG_AEC_SERVER_URI
#define AEC_DEVICE_ID        CONFIG_DEVICE_ID
#define AEC_AUDIO_TOKEN      CONFIG_AEC_AUDIO_TOKEN
