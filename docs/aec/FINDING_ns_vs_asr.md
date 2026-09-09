
## 2026-09-10 — Finding: WebRTC NS degrades uplink ASR accuracy (not the AEC)

### What was seen

A 20 s capture with the ESP32 speaker **silent** (`aec_capture` at `volume: 0`), while an
external speaker looped song lyrics next to the device. All three channels recorded, each
sent to the cloud ASR (`qwen3-asr-1.7b`):

```
mic_raw   RMS 194.0  (-44.6 dBFS)  ASR: 记住青春的滋味，记住流泪。
reference RMS   0.6  (-94.7 dBFS)  ASR: (empty)      <- digital silence, we played nothing
clean_aec RMS 106.9  (-49.7 dBFS)  ASR: 个月，记住青春的滋味，记住流。
```

The post-AFE transcript **gains a phantom "个月" at the head and loses the final "泪"**.

### It is not the AEC

The reference channel is at −94.7 dBFS — the device played nothing, so the AEC had no
echo to cancel and contributed no gain change. Everything that altered the audio happened
downstream of it:

```
[input] -> |AEC(no-op here)| -> |NS(WebRTC)| -> |VAD| -> [output]
```

### It is not a level problem, and not ASR nondeterminism

Two controls, both decisive:

```
mic_raw                        : 记住青春的滋味，记住流泪。
clean_aec                      : 个月，记住青春的滋味，记住流。
clean_aec amplified +5.2 dB    : 个月，记住青春的滋味，记住流。   <- same RMS as mic_raw (193.7 vs 194.0)
clean_aec re-run               : 个月，记住青春的滋味，记住流。   <- ASR is deterministic
```

Restoring the level character-for-character reproduces the same errors, so the missing
phoneme was **removed**, not merely made quieter. Re-running gives an identical string, so
the ASR is not hallucinating at random.

### Mechanism: WebRTC NS applies a time-varying, per-band gain mask

Per-second RMS shows the attenuation deepening across the capture as the noise estimator
learns the sustained music as "background":

| second | mic_raw | clean_aec | attenuation |
|---|---|---|---|
| 1 | 176 | 118 | −3.5 dB |
| 5 | 175 | 61 | −9.2 dB |
| 17 | 189 | 42 | −13.1 dB |
| 19 | 163 | 27 | −15.6 dB |

Full series —
`mic_raw:   176 208 207 206 175 179 178 274 171 158 216 198 193 183 213 216 189 168 163`
`clean_aec: 118 167 156 150  61  92  91 235 110  71 100  76  86  63  82  85  42  30  27`

Peaks and dips still track (sec 8: 274 → 235), so nothing is being gated wholesale — the
signal is being *reshaped*. The consequences for ASR:

1. The trailing weak unvoiced phoneme ("泪") falls inside a heavily masked band/region and
   is erased. Amplification cannot bring back spectral content that was removed.
2. Mask switching produces "musical noise" artifacts at the onset, which the model reads
   as a spurious token ("个月").

NS assumes anything sustained and stationary is noise. That is right for a room hum and
wrong for music, a TV, or any continuous audio you actually want transcribed.

### Why it matters

The AFE's clean output *is* the uplink to cloud ASR. NS is measurably costing recognition
accuracy on that path. This is separate from — and does not undermine — the AEC results
(L0 PASS, L1 35–36 dB, L4 A/B 37.8 dB, double-talk ASR recovery); it is a downstream
processing choice worth revisiting.

### Options (not yet tested)

| option | trade-off |
|---|---|
| `ns_init = false` | uplink carries room noise unfiltered, but maximum ASR fidelity; AEC unaffected |
| `afe_ns_mode`: `WEBRTC` → `NSNET` | neural NS, usually gentler on speech; costs CPU/RAM |
| `AFE_TYPE_VC` → `AFE_TYPE_SR` | SR mode excludes the nonlinear noise suppression by design |

The direct A/B is one build: disable NS, re-record the same 20 s against the same external
source, and check whether the transcript returns to "记住青春的滋味，记住流泪。".

Audio for this finding: `data/aec/EXT_v0_{1_mic_raw,2_reference,3_clean_aec}.wav`,
`data/aec2/{10_extspeaker_BEFORE,11_extspeaker_REFERENCE,12_extspeaker_AFTER}.wav`.
