# ESP32-S3 AEC — test results

Device `esp32-s3-walle` (ESP32-S3 + ES7210 mic/reference + ES8311 speaker),
firmware branch `feature/aec-fullduplex`, esp-sr 2.5.3 AFE
(`AFE_TYPE_VC`: AEC VOIP_HIGH_PERF + NLP AGGRESSIVE, WebRTC NS, WebRTC VAD),
16 kHz mono, hardware echo reference on ES7210 ch1.

Full narrative + every pitfall: `esp32_wifi/docs/AEC_INTEGRATION_LOG.md`.

---

## L0 — is the ES7210 reference channel a real hardware loopback?

`aec_probe` (on-device, plays a 1 kHz tone and compares per-channel RMS):

| | ch0 (mic) | ch1 (reference) |
|---|---|---|
| silence | 13.5 | **0.6** |
| tone playing | 1516.7 | **1720.7** |

`reference=REAL` — ch1 carries the played signal, so AEC runs with a hardware
reference (`CONFIG_AEC_SOFTWARE_REF=n`). Channel order `"MR"` confirmed.

## L1 — single-talk ERLE

`aec_erle` (on-device: 1 s idle floor, then a burst split into a settle window
and a converged measure window). Quiet room, no near-end source.

| excitation | vol | ERLE converged | ERLE settle | mic | ref | clean | idle_clean |
|---|---|---|---|---|---|---|---|
| noise | 100 | **36.2 dB** | 28.3 | 1659.5 | 3654.2 | 25.7 | 94.4 |
| noise (repeat) | 100 | **35.1 dB** | 21.8 | 1655.3 | 3654.4 | 29.0 | 25.5 |
| tone 1 kHz | 100 | **26.9 dB** | 21.6 | 6847.4 | 8889.4 | 309.1 | 65.1 |
| control: silent | 0 | 0.6 dB | 0.5 | 91.5 | 0.6 | 85.1 | 122.3 |

- Reproducible ~35–36 dB on broadband excitation; well past the ≥25 dB bar.
- Converged `clean` (25.7) sits **below** the idle floor (94.4): echo removed, not attenuated.
- Convergence improves settle → measure, as it should.
- A pure sine reads ~9 dB lower — single-tone is poor excitation for an adaptive filter.
- The `volume=0` control reads ~0 dB, so the measurement chain is not inventing the number.

### With a near-end source present (double-talk)

The same test with a looping audio source next to the device read **4.7–5.9 dB**,
and got *worse* settle → measure. That is the double-talk detector correctly
refusing to suppress while the near end is active — not a defect. Removing the
near-end source took the identical test from 5 dB to 36 dB.

## Captured audio (`*.wav`, 16 kHz / 16-bit / mono, 5 s each)

Recorded simultaneously on-device during a converged burst (3 s settle first):

| file | what it is | RMS | peak |
|---|---|---|---|
| `L1_noise_v100_1_mic_raw.wav` | **pre-AEC** raw microphone (echo included) | 1667.2 | 7046 |
| `L1_noise_v100_2_reference.wav` | ES7210 hardware reference channel | 3654.0 | 13276 |
| `L1_noise_v100_3_clean_aec.wav` | **post-AEC** AFE clean output | **21.2** | **149** |
| `L1_tone_v100_1_mic_raw.wav` | pre-AEC raw microphone | 7305.5 | 12585 |
| `L1_tone_v100_2_reference.wav` | hardware reference | 8888.9 | 12458 |
| `L1_tone_v100_3_clean_aec.wav` | post-AEC clean output | **411.2** | 2339 |

Measured straight off the waveforms:

- noise: 1667.2 → 21.2 = **37.9 dB**
- tone: 7305.5 → 411.2 = **25.0 dB**

which independently confirms the on-device `aec_erle` numbers.

## Full-loop check (cloud)

Mic → on-board AFE (AEC/NS/VAD) → WS uplink → `audio_interact` Silero VAD →
cloud ASR transcribed a looping audio source next to the device as "星星。",
with `speech_start` barge-in signals coming back and the firmware acting on them.
WS uplink stable at 1 KB / ~32 ms, zero disconnects after the buffer fix.

## Still open

- **L4** AEC on/off A/B (`CONFIG_AEC_ENABLE` compile switch is already wired).
- **Soak** 24 h stability.
- **L2/L3** double-talk word accuracy and barge-in latency — need a controlled
  near-end source (the raspberrypi), which was unreachable during this session.
