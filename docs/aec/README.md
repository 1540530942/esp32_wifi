# AEC test records

Verification of the esp-sr AFE integration on `esp32-s3-walle` (ESP32-S3 + ES7210
mic/hardware-reference + ES8311 speaker). Firmware: this branch.

| file | what it holds |
|---|---|
| `RESULTS.md` | L0 (hardware reference real), L1 (single-talk ERLE), L4 (AEC on/off A/B) — the numbers |
| `TTS_DOUBLETALK.md` | Double talk with **real TTS speech** as the far end (the product case): before AEC the cloud hears only the robot, after AEC it transcribes the room |
| `NLP_LEVEL_AB.md` | `aec_nlp_level` AGGR vs NORMAL. Same-session AGGR baseline, and why the "loses the near end at volume 60" boundary in `TTS_DOUBLETALK.md` did not reproduce |
| `ASR_RESULTS.md` | Does the near end survive? Cloud-ASR transcripts before vs after AEC, and the double-talk sweep across playback volumes |
| `FINDING_ns_vs_asr.md` | WebRTC NS — not the AEC — measurably degrades uplink ASR; controls that prove it, and the untested options |
| `AUDIO_INDEX.md` | Every captured WAV, what stage it belongs to, and its levels |
| `LISTENING_GUIDE.md` | A curated 12-file subset in listening order, with what to listen for |

Running narrative and every pitfall hit during bring-up: `../AEC_INTEGRATION_LOG.md`.

## Headline results

| stage | result |
|---|---|
| **L0** ES7210 reference channel | PASS — `reference=REAL` (silent 0.6 → tone 1720.7) |
| **L1** single-talk ERLE, quiet room | **35–36 dB** broadband, reproducible; 26.9 dB on a pure tone; 0.6 dB with the speaker off (control) |
| **L4** AEC on/off A/B | **37.8 dB** attributable to AEC (identical inputs, only `CONFIG_AEC_ENABLE` flipped) |
| double-talk ASR | Pre-AEC ASR returns empty, post-AEC transcribes correctly — full duplex works |
| near-end preservation | Transcripts two characters apart with the speaker silent |

## Audio

The WAV files are **not** committed — they are ~9.4 MB of binary. They live on the bench
machine:

- `~/workspace/data/aec/` — full archive, 29 WAVs, three channels per capture
- `~/workspace/data/aec2/` — curated 12-file listening set, numbered in order

Naming is consistent everywhere:

- `*_1_mic_raw.wav` — **before AEC**: raw microphone (ES7210 ch0), echo included
- `*_2_reference.wav` — the hardware echo reference (ES7210 ch1, amp loopback)
- `*_3_clean_aec.wav` — **after AEC**: the AFE clean output, i.e. what is uplinked

## Still open

- Soak (24 h stability)
- L3 barge-in latency (duck / cloud-kill timestamps)
- `aec_nlp_level`: AGGR baseline measured (`NLP_LEVEL_AB.md`); the NORMAL half still needs a build
- The NS A/B from `FINDING_ns_vs_asr.md` (`ns_init=false`, or `NSNET`, or `AFE_TYPE_SR`)
