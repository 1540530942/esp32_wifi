# Does the near end survive AEC? — cloud ASR on the captured audio

The device sits next to an external speaker looping song lyrics (the "near end").
Each capture records three channels simultaneously on-device; every file below was
then POSTed to the cloud ASR (`qwen3-asr-1.7b`) so the comparison is end-to-end,
not a level measurement.

## 1. Baseline — ESP32 speaker silent, only the external lyrics

| | RMS | ASR |
|---|---|---|
| `NEAR_1_mic_raw.wav` (pre-AEC) | 42.4 | 如果我就是绝对，如果清醒是种罪，就让爱去蔓延，成全。 |
| `NEAR_2_reference.wav` | 1.9 | — (our speaker is silent, as expected) |
| `NEAR_3_clean_aec.wav` (post-AEC) | 25.0 | 如果爱就是绝对，如果清醒是种罪，就让爱去蔓延，就全。 |

**Near-end passes through AEC essentially intact.** The two transcripts differ by two
characters. Per-second RMS tracks the raw mic (peaks and dips line up), so the AFE is
attenuating a few dB of low-level content (WebRTC NS) rather than gating anything out.

## 2. Double talk — ESP32 playing while the lyrics play

This is the question that matters: with the robot's own speaker running, can the cloud
still understand the room?

| far-end volume | `mic_raw` (pre-AEC) ASR | `clean_aec` (post-AEC) ASR |
|---|---|---|
| 25 | You're the one I want to keep. *(garbled)* | 以为会认定，就算明天就是一切。如果说就是绝对，如果真心是错，那就。 |
| 40–60 | **(empty)** | **如果你还没有睡，如果我还不停。** |
| 100 | (empty) | (empty) |

At moderate playback the raw microphone is unintelligible — the echo masks the lyrics and
ASR returns nothing — while **the AEC output transcribes correctly**. That is full duplex
working: the device can listen through its own voice.

## 3. Where it breaks

At volume 100 both fail. Levels explain it:

| | mic_raw RMS | clean_aec RMS | suppression |
|---|---|---|---|
| vol=100 | 2233.0 | 43.3 | 34.3 dB |
| vol=60 | 214.8 | 14.8 | 23.2 dB |

The external speaker only reaches the mic at RMS ~40. At volume 100 the echo is ~34 dB
above it, and `aec_nlp_level: AGGRESSIVE` cannot pull a signal that weak back out of a
residual that large — the near end is suppressed along with the echo.

**Operating envelope: the near end needs to stay within roughly 25–30 dB of the echo.**
In real use a person speaks close to the device and is far louder than this external
speaker, so there is comfortable margin — and the double-talk detector already backs
suppression off when it hears near-end energy (measured: ERLE drops 36 dB → 5 dB).

If preserving weak near-end during loud playback matters, `aec_nlp_level` can be moved
from `AGGRESSIVE` to `MODERATE`, trading echo suppression for near-end preservation.

## Files

`NEAR_*` baseline · `DT25_* DT40_* DT60_* DT100_*` double talk at each volume ·
`D_noise20s_*` the 20 s before/after demo · `L1_* L4off_*` the ERLE and AEC-on/off A/B sets.
`D_speech12s_* E_speech12s_*` are **invalid** — the speaker never played (reference RMS 0.6),
so they contain only room tone.
