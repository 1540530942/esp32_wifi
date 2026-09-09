# ESP32-S3 AEC — test record and captured audio

Device `esp32-s3-walle`: ESP32-S3 + ES7210 (mic + hardware echo reference) + ES8311
(speaker). Firmware branch `feature/aec-fullduplex`, esp-sr **2.5.3** AFE in
`AFE_TYPE_VC` mode — AEC (VOIP_HIGH_PERF, NLP AGGRESSIVE) → WebRTC NS → WebRTC VAD,
16 kHz mono, hardware reference on ES7210 ch1.

Every WAV here is 16 kHz / 16-bit / mono, recorded **on the device** with all three
channels captured simultaneously during the same burst:

- `*_1_mic_raw.wav` — **before AEC**: the raw microphone (echo included)
- `*_2_reference.wav` — the ES7210 hardware reference (what the speaker played)
- `*_3_clean_aec.wav` — **after AEC**: the AFE's clean output

Narrative + every pitfall hit along the way: `esp32_wifi/docs/AEC_INTEGRATION_LOG.md`.
Detail files here: `RESULTS.md` (L0/L1/L4 numbers), `ASR_RESULTS.md` (near-end / double talk).

---

## Stage L0 — is the reference channel a real hardware loopback?

On-device `aec_probe`: play a 1 kHz tone, compare per-channel RMS.

| | ch0 (mic) | ch1 (reference) |
|---|---|---|
| silence | 13.5 | **0.6** |
| tone playing | 1516.7 | **1720.7** |

**PASS — `reference=REAL`.** ch1 carries the played signal, so AEC uses a hardware
reference (no software-reference fallback). Channel order `"MR"` confirmed.

## Stage L1 — single-talk ERLE (quiet room)

On-device `aec_erle`: 1 s idle floor, then a burst split into a settle window
(filter converging) and a converged measure window.

| excitation | vol | ERLE converged | settle | mic | ref | clean | idle_clean |
|---|---|---|---|---|---|---|---|
| noise | 100 | **36.2 dB** | 28.3 | 1659.5 | 3654.2 | 25.7 | 94.4 |
| noise (repeat) | 100 | **35.1 dB** | 21.8 | 1655.3 | 3654.4 | 29.0 | 25.5 |
| tone 1 kHz | 100 | **26.9 dB** | 21.6 | 6847.4 | 8889.4 | 309.1 | 65.1 |
| control: silent | 0 | 0.6 dB | 0.5 | 91.5 | 0.6 | 85.1 | 122.3 |

**PASS.** Reproducible 35–36 dB on broadband excitation. Converged output (25.7) sits
*below* the idle floor (94.4) — the echo is removed, not attenuated. Convergence improves
settle → measure. A pure sine reads ~9 dB lower (single tone is poor excitation for an
adaptive filter). The `volume=0` control reads ~0 dB, so the measurement chain is honest.

Measured independently off the waveforms:
`L1_noise` 1667.2 → 21.2 = **37.9 dB** · `L1_tone` 7305.5 → 411.2 = **25.0 dB**.

## Stage L4 — AEC on/off A/B

Same firmware, `CONFIG_AEC_ENABLE` flipped, identical input conditions.

| | mic_raw RMS | reference RMS | clean output RMS | peak |
|---|---|---|---|---|
| AEC **off** | 1668.8 | 3653.8 | **1641.8** | 7389 |
| AEC **on** | 1667.2 | 3654.0 | **21.2** | 149 |

**PASS — 37.8 dB attributable to AEC.** With AEC off the output is the input
(pass-through, 0.1 dB in the settle window); the only thing that changed is AEC.

## Stage — does the near end survive? (cloud ASR)

An external speaker loops song lyrics next to the device. Every file was POSTed to the
cloud ASR (`qwen3-asr-1.7b`), so this is end-to-end, not a level measurement.

**Baseline, ESP32 speaker silent:**

| | RMS | ASR |
|---|---|---|
| `NEAR_1_mic_raw` (pre) | 42.4 | 如果我就是绝对，如果清醒是种罪，就让爱去蔓延，成全。 |
| `NEAR_3_clean_aec` (post) | 25.0 | 如果爱就是绝对，如果清醒是种罪，就让爱去蔓延，就全。 |

Two characters apart — **near-end passes through AEC intact.**

**Double talk — ESP32 playing while the lyrics play:**

| far-end vol | pre-AEC ASR | post-AEC ASR |
|---|---|---|
| 25 | You're the one I want to keep. *(garbled)* | 以为会认定，就算明天就是一切。如果说就是绝对，如果真心是错，那就。 |
| 40–60 | **(empty)** | **如果你还没有睡，如果我还不停。** |
| 100 | (empty) | (empty) |

**This is full duplex working:** at moderate playback the raw mic is unintelligible (the
echo masks the lyrics, ASR returns nothing) while the AEC output transcribes correctly.

**Where it breaks:** the external speaker only reaches the mic at RMS ~40; at volume 100
the echo is RMS 2233, i.e. ~34 dB above it, and AGGRESSIVE NLP suppresses the near end
along with the echo. **Operating envelope: near end within ~25–30 dB of the echo.** Real
speech at conversational distance is far louder than this, and the double-talk detector
already backs off when it hears near-end energy (ERLE drops 36 dB → 5 dB).

To favour weak near-end over echo suppression, move `aec_nlp_level` from `AGGRESSIVE`
to `MODERATE`.

---

## File index

| files | stage | what to listen for |
|---|---|---|
| `D_noise20s_*` | 20 s before/after demo | mic_raw RMS 1758.8 → clean_aec 80.1 (**26.8 dB**). Longest clip — easiest to hear. |
| `L4off_noise_v100_*` + `L1_noise_v100_3_clean_aec` | AEC on/off A/B | play `L4off_*_3_clean_NOAEC` (1641.8) against `L1_*_3_clean_aec` (21.2). Most convincing pair. |
| `L1_noise_v100_*` | L1 ERLE, broadband | 37.9 dB |
| `L1_tone_v100_*` | L1 ERLE, 1 kHz tone | 25.0 dB |
| `NEAR_*` | near-end only, ESP32 silent | lyrics audible before **and** after |
| `DT25_* DT40_* DT60_* DT100_*` | double talk per volume | `_1_mic_raw` vs `_3_clean_aec` |
| `D_speech12s_* E_speech12s_*` | **INVALID** | the speaker never played (reference RMS 0.6) — room tone only. Ignore. |

## Still open

- **Soak** — 24 h stability run.
- **L3** barge-in latency (duck / cloud-kill timestamps).
- A clean `aec_nlp_level: MODERATE` vs `AGGRESSIVE` comparison.
- Speech-as-far-end capture: `play_wav_url` does not produce audio on this device; the
  proven path is `tts_synthesize()` → direct codec write (as `run_mic_asr_test` uses).
