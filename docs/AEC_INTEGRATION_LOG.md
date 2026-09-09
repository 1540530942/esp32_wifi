# AEC Full-Duplex Integration — Running Log

Branch: `feature/aec-fullduplex` (off `main` @ `752dbdf` / tag `ota-esp32-wangyutang-v20-mic`)
Goal: integrate esp-sr AFE (AEC + NS + VAD) into `esp32_wifi_impl` so `esp32-s3-walle`
does real full-duplex voice — mic keeps capturing during TTS playback, board-level
echo cancellation, barge-in — talking to the existing `audio_interact` `/ws/audio`.

Approach chosen (方案2): keep the `esp32_wifi_impl` firmware (LCD / OTA / device-hub /
MQTT shell), graft in the AFE pipeline + `/ws/audio` client + playback engine from
`esp32_mic_asr_test` (== the `AEC/` snapshot). Do NOT swap to a standalone firmware.

---

## Key facts established (from reading the code)

### Target: `esp32_wifi_impl` (repo `git_space/esp32_wifi`, branch `main`)
- IDF 5.5.5, ESP32-S3 N16R8 (16MB flash / 8MB Octal PSRAM — PSRAM enabled in v20-mic).
- Partitions: dual-OTA, `ota_0`/`ota_1` = 3MB each. **No `model` partition.**
- Codec: vendored `BoxAudioCodec` (xiaozhi-style, `main/audio/codecs/`), ES8311 out +
  ES7210 in, full-duplex I2S. `input_reference=true` → `AudioCodec_InputData()` returns
  **2ch interleaved `[mic, echo-ref]`** (confirmed: `run_mic_asr_test` keeps `raw[i*ch+0]`).
- Was **24 kHz** in/out. AFE needs 16 kHz → changed `audio_board_config.h` to 16000/16000.
- Audio was **half-duplex**: `play_task` OR button-triggered `run_mic_asr_test`, never
  a continuous concurrent loop.
- Command actions (firmware `handle_command`): reboot / set_volume / play_audio /
  stream_prepare / stop_audio / identify / ota  (+ `aec_probe` / `mic_asr_test` added
  on this branch, see below).
- Pins (audio_board_config.h): MCLK15 BCLK14 WS13 DOUT16 DIN12, PA GPIO17,
  I2C SDA1 SCL2, ES7210 addr 0x82(8-bit)/0x41(7-bit), ES8311 default. BOOT btn GPIO0.
  LCD GPIO3-8.
- Console: sdkconfig.defaults says USB-Serial-JTAG console disabled, but the **running
  v20 image prints ESP_LOGI over `/dev/ttyACM0` anyway** — serial log IS available.

### Reference: `esp32_mic_asr_test` (== `AEC/`)
- `afe_pipeline.c` — feed/fetch tasks, local barge-in. Written for **esp-sr 1.9** API.
- `playback.c` — WAV parse + linear resample-to-16k-mono + duck/kill + queue. Reusable
  as-is (only depends on `board.h` API + config constants).
- `ws_client.c` — `esp_websocket_client` to `/ws/audio`, proto 2, start_stream /
  stream_ready / bin PCM up / tts_begin+bin WAV down / tts_cancel+speech_start → kill.
- `board.c` — uses `esp_codec_dev` (NOT reused — shimmed to BoxAudioCodec instead).

### esp-sr version (RESOLVED at build): **2.5.3**, not 1.9
- New AFE v2.0 API, **not compatible** with the reference's 1.9 code. R1 hit.
- Ported `afe_pipeline.c` init:
  - `afe_config_init("MR", NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF)` (was `AFE_CONFIG_DEFAULT()`)
  - `AFE_TYPE_VC` = voice-comm front end: AEC + nonlinear NS + VAD, 16kHz, no wakenet →
    **no model partition needed** (models = NULL). Partition table untouched → OTA safe.
  - `cfg->wakenet_init=false; cfg->agc_init=false; cfg->aec_init = CONFIG_AEC_ENABLE`
  - `afe_config_check(cfg)` then `esp_afe_handle_from_config(cfg)` → `create_from_config(cfg)`
  - iface: `get_total_channel_num` → `get_feed_channel_num`
  - fetch result: `AFE_VAD_SPEECH` → `VAD_SPEECH` (vad_state_t)
  - fetch result has `data_volume` (dBFS, pre-AGC) + `raw_data`/`raw_data_channels` —
    useful for on-device ERLE without tapping the feed path.

### Git baseline cleanup (done, on `main`)
- The deployed `esp32-wangyutang-v20-boot` was **not in git** — v16..v20 built locally
  from 6 uncommitted files on top of `b9d4c38` (v15), never pushed.
- Landed as 3 commits `a09bb3d` / `ad14c36` / `752dbdf`, pushed to `origin/main`,
  tagged `ota-esp32-wangyutang-v20-mic`. Deleted dead `origin/feature/wifi-ota`
  (was 15 behind main / 0 ahead).

### Remote-trigger commands (done, on `feature/aec-fullduplex` @ `6480380`)
- `aec_probe`   → `run_aec_reference_probe()`, ACK: `done|silent_ch0=.. silent_ch1=..
  play_ch0=.. play_ch1=.. ch1_delta=.. reference=REAL|FLOATING`
- `mic_asr_test` → `run_mic_asr_test(args.seconds|4)`, ACK: `done|asr_text=..`
- `run_aec_reference_probe()` now also stashes RMS + verdict in
  `AudioPlayer::AecProbeResult` (`last_probe_result()`).
- Removes the physical-BOOT-button dependency for L0 verification.

### Hardware access (confirmed)
- ESP32-S3 is on the Windows host, `usbipd` busid `2-5` (`303a:1001`), was `Shared`.
- WSL→Windows interop works via `/mnt/c/Program Files/usbipd-win/usbipd.exe`.
- `usbipd attach --wsl --busid 2-5` → `/dev/ttyACM0` in WSL (archer in `dialout`).
- Live app console readable on `/dev/ttyACM0`.
- `raspberrypi` (`ssh pi@raspberrypi`, 192.168.1.46) has USB mic+speaker, same
  wifi/subnet as the ESP32 (192.168.1.55), can ping it. User confirmed the Pi speaker
  is within acoustic range of the ESP32 mic → L2/L3 double-talk/barge-in tests are viable.

---

## Files changed on `feature/aec-fullduplex` (WIP, uncommitted)

New in `esp32_wifi_impl/main/`: `afe_pipeline.{c,h}`, `playback.{c,h}`, `ws_client.{c,h}`
(ported), `afe_board.h` + `afe_board_shim.c` (BoxAudioCodec bridge), `aec_config.h`,
`idf_component.yml`.

Modified: `CMakeLists.txt` (+SRCS, +`esp-sr esp_websocket_client` REQUIRES),
`Kconfig.projbuild` (+"AEC Full-Duplex Voice" menu), `sdkconfig.defaults`
(+`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`, +`CONFIG_FREERTOS_HZ=1000`),
`audio_board_config.h` (24k→16k), `audio_player.h` (+`codec()` getter),
`app_main.cpp` (+includes, +AFE/playback/ws start after WiFi, +`aec_on_*` callbacks).

Shim: `board_mic_read` → `AudioCodec_InputData`, `board_spk_write` → `AudioCodec_OutputData`,
`board_pa_enable` → `AudioCodec_EnableOutput` + PA GPIO, `board_init` = no-op
(codec already up in AudioPlayer ctor) + set AEC-path volume.

---

## Verification plan (autonomy noted)

| L | what | how (autonomous path) | pass | autonomous? |
|---|---|---|---|---|
| L0 | ES7210 ref channel real? | `aec_probe` cmd → read ACK `reference=REAL|FLOATING` | REAL → HW ref | ✅ |
| L1 | single-talk ERLE | play 30s known speech → read heartbeat `erle_db` (instrumentation TODO) + `mock_cloud.py` cross-check | HW ≥25dB / SW ≥15dB, converge <500ms | ✅ (needs erle_db埋点) |
| L4 | AEC on/off A/B | build `CONFIG_AEC_ENABLE=y/n` twice, OTA each, compare | off − on ≥ 20dB | ✅ (2 OTA) |
| soak | 24h full-duplex | poll heartbeat: free_heap / feed_starved / reconnect / erle_db | stable | ✅ |
| L2 | double-talk | ESP32 plays TTS + `ssh pi aplay` known near-end phrase → mock_cloud captures uplink / cloud `result` ASR | near-end ASR ≥90% base, TTS residual <−20dB | ✅ (Pi co-located, confirmed) |
| L3 | barge-in latency | Pi aplay at T0; heartbeat `local_duck_ts_ms` / `cloud_kill_ts_ms` − T0 | duck <250ms, kill <600ms | ✅ (needs ts埋点) |

Order: L0 → L1 (mock_cloud then real cloud) → L4 → soak → L2 → L3.

Delivery preference: **first AEC flash wired (`idf.py flash` + `idf.py monitor` to watch
AFE init); all later iteration via OTA.**

---

## TODO / open

- [ ] First full build pass (esp-sr 2.5.3 compile — in progress)
- [ ] §5 instrumentation: `erle_db`, `feed_starved`, `afe_init_ok`, `ws_ready`,
      `local_duck_ts_ms` / `cloud_kill_ts_ms` into heartbeat state; coredump-to-flash.
- [ ] Route legacy `play_audio`/`stop_audio` through `playback_enqueue_wav`/`playback_kill`.
- [ ] BOOT short-press → toggle `/ws/audio` session (was `run_mic_asr_test`, conflicts with AFE feed).
- [ ] Confirm concurrent `AudioCodec_InputData` + `AudioCodec_OutputData` on BoxAudioCodec
      full-duplex I2S is thread-safe under load (soak).
- [ ] `idf.py size` — confirm binary < ~2.9MB (fits 3MB OTA slot) with esp-sr.
- [ ] `enqueue_command` in `device_hub/server.py` doesn't publish MQTT — decide: patch
      server to `_enqueue_mqtt_command`, or `docker exec` MQTT publish for test commands.

---

## Timeline

- 2026-09-09: baseline cleanup + push/tag v20-mic; deleted feature/wifi-ota;
  added aec_probe/mic_asr_test remote commands (`6480380`); usbipd-attached the
  ESP32 to WSL, confirmed serial; scaffolded AFE integration; ported afe_pipeline.c
  to esp-sr 2.5.3 API; kicked off first full build.

## 2026-09-09 — first wired flash

- `idf.py build` clean (BUILD_EXIT=0). Binary 0x254240 = 2.33 MB, 22% free in the 3 MB
  OTA slot. **R2 (flash budget) resolved — no partition changes.**
- Checkpoint committed `fb5e6e2`, pushed. (Snag: `git add -A` first pulled in the whole
  `managed_components/` tree, 2117 files pushed as `f9cc968`; fixed with
  `.gitignore += managed_components/`, re-commit, `--force-with-lease`.)
- `idf.py -p /dev/ttyACM0 flash` → FLASH_EXIT=0, all hashes verified, app written to
  ota_0 @ 0x20000.
- **USB-Serial-JTAG has no RTS pin → esptool's `--after hard_reset` is a no-op.** The
  chip stayed in the download stub after flash (silent serial, device-hub showed the
  pre-flash last-known-good state). Reset it with a pyserial DTR/RTS toggle
  (DTR=False, pulse RTS) → clean boot.
- Boot log: **AEC firmware boots fine** — `Loaded app from partition at offset 0x20000`,
  `App version: ota-esp32-wangyutang-v20-mic-2-`, PSRAM 8MB OK, cpu 240 MHz,
  `Calling app_main()`, `reset_reason=11` (ESP_RST_USB — our reset, benign).
- **BLOCKER: WiFi credentials not in tracked config.**
  `W (1335) wifi: Password length is zero, ... impossible to connect to an AP with authmode OPEN`
  `sdkconfig.defaults` and `sdkconfig.old` both have `CONFIG_DEVICE_WIFI_SSID/PASSWORD=""`.
  The creds that let v20-boot join `ChinaNet-dsge` were set via menuconfig into the
  gitignored `sdkconfig` and lost when it was regenerated. `app_main` blocks forever at
  `xEventGroupWaitBits(WIFI_CONNECTED_BIT)` → never reaches board_init / afe_pipeline_init.
  Need: the `ChinaNet-dsge` password. Fix = set the two CONFIG lines in the generated
  `sdkconfig` (gitignored), rebuild, reflash. Do NOT put real creds in `sdkconfig.defaults`.
- Serial access notes: `/dev/ttyACM0` present via `usbipd attach --wsl --busid 2-5`.
  `idf.py monitor` fails headless (needs a TTY). Use a pyserial reader + DTR/RTS toggle
  to reset+capture. esptool talks to the ROM/stub fine (read MAC 58:e6:c5:6b:9b:84).

## 2026-09-09 (cont.) — AFE pipeline confirmed live on hardware

WiFi creds set in gitignored `sdkconfig` (`ChinaNet-dsge`). Rebuild + wired reflash.
Boot log (device `esp32-s3-walle`, 192.168.1.55):

```
wifi: connected with ChinaNet-dsge, got IP=192.168.1.55
i2s_common: rx channel on I2S0 switched master->slave for full-duplex mode
afe_board: board ready (BoxAudioCodec: 2ch in, ES8311 out, 16000 Hz)      <- shim OK
AFE: AFE Version: (1MIC_V251128)
AFE: Input PCM Config: total 2 channels(1 microphone, 1 playback), 16000   <- "MR" parsed OK
AFE: AFE Pipeline: [input] -> |AEC(VOIP_HIGH_PERF, NLP_ON)| -> |NS(WebRTC)| -> |VAD(WebRTC)| -> [output]
wangyutang_app: AEC full-duplex pipeline started                           <- init chain all ESP_OK
device_hub: registered OK
```

**esp-sr 2.5.3 AFE (AEC VOIP-high-perf + NLP + WebRTC NS + WebRTC VAD) is initialized
and running on the real device.** The port (afe_config_init / esp_afe_handle_from_config
/ AFE_TYPE_VC / "MR" 2ch / BoxAudioCodec shim / 16 kHz) all works. `board_init` no-op +
`afe_pipeline_init` succeed.

### Blocker: TLS to :443 is broken on this ISP path (not a bug in our code)
- `ws_client.c` first failed with "No server verification option" (ported config had
  `skip_cert_common_name_check` + no CA). Added `crt_bundle_attach`.
- Then: `esp-x509-crt-bundle: Certificate matched but signature verification failed`
  (`PK verify failed 0x4290`), `mbedtls_ssl_handshake returned -0x3000`.
- **The whole firmware already works around this**: `device_hub_client` and `ota` fall
  back to plain `http://110.40.154.41` ("fallback when the ISP path to port 443 is
  temporarily unavailable" — tag `v10-network-fallback`). Looks like ChinaNet MITM /
  TLS interception on `www.wangyutang.cn:443`.
- Secondary damage: the 2s WS reconnect storm exhausted the TLS heap
  (`mbedtls_ssl_setup returned -0x7F00` = ALLOC_FAILED) and started breaking the
  device_hub HTTPS heartbeat too.

### Fix: `/ws/audio` over plain `ws://`
- `CONFIG_AEC_SERVER_URI` default → `ws://110.40.154.41/ws/audio` (Kconfig + sdkconfig).
  Caddy on :80 serves `/devices/api` fine (proven by the heartbeat fallback), should
  serve `/ws/audio` (WS upgrade over plain HTTP is fine).
- Removed `crt_bundle_attach` / `esp_crt_bundle.h` from `ws_client.c` — no TLS now.
- Rebuild + reflash (bf3) → expecting `start_stream` → `stream_ready`.

### Serial capture recipe (headless, no idf.py monitor)
`idf.py monitor` needs a TTY → unusable over SSH. Use pyserial:
`Serial('/dev/ttyACM0',115200)`, `setDTR(False); setRTS(True); sleep .1; setRTS(False)`
to reset-into-app, then read. USB-JTAG re-enumerates on reset so re-open with retries;
`usbipd attach --wsl --busid 2-5` if `/dev/ttyACM0` vanishes.

## 2026-09-09 (cont.) — L0 PASSED, and the full cloud loop works

### L0: hardware echo reference is REAL
Ran `aec_probe` remotely (device-hub `_enqueue_mqtt_command` → retained MQTT topic
`devices/esp32-s3-walle/command/<id>`; the plain `/api/device/{id}/command` HTTP route
stores but does NOT publish MQTT, so it never reaches the device). ACK:

```
aec_probe done | silent_ch0=13.5 silent_ch1=0.6 play_ch0=1516.7 play_ch1=1720.7
                 ch1_delta=1720.1 reference=REAL
```

Idle ref-channel RMS 0.6; with a 1 kHz tone playing it jumps to 1720. **ES7210 ch1 is a
real hardware speaker loopback** → AEC uses the hardware reference (`CONFIG_AEC_SOFTWARE_REF=n`,
the default). `"MR"` channel order confirmed: ch0 = mic, ch1 = reference.

Note: `mic_asr_test` fails with `command task allocation failed` — no internal RAM left
for another 8 KB task once the AFE is up. Its ASR call would also hit the :443 TLS wall.
Not a blocker (it is the legacy half-duplex path), but it flags how tight internal RAM is.

### Gateway: `/ws/audio` was never exposed
`https://www.wangyutang.cn/ws/audio` → 404. The live Caddyfile (bind-mounted from
`/root/wangyutang_platform/robot_gateway/Caddyfile`, NOT the stale
`/root/wangyutang_platform/Caddyfile`) routes `/audio_interact/*`, `/devices/*`, `/common/*`
… but had no `/ws/audio`. The audio-interact container itself answers a WS handshake on
`ws://127.0.0.1:8097/ws/audio` fine (`stream_ready`, vad silero, proto 2).

Added (backup `Caddyfile.bak-1788914715`, `caddy validate` + graceful `caddy reload`):
```
handle /ws/audio* {
    reverse_proxy {$AUDIO_INTERACT_UPSTREAM:audio-interact:8097}
}
```
`handle` (not `handle_path`) so the path passes through unchanged. Plain HTTP on
`http://110.40.154.41` is an explicit site address in that block, so no HTTPS redirect —
which is what the firmware needs since :443 is MITM-broken here.

### Full loop confirmed end-to-end
With fetch/uplink instrumentation (bf6), serial shows:

```
ws: rx text: {"type":"stream_ready","session_id":"esp32-...","device_id":"esp32-s3-walle",
              "vad":"silero","sample_rate":16000,"proto":2}
ws: uplink gated: ready=0 connected=0 sent=397 dropped=564     <- 397 PCM frames really went up
afe: fetch: 1091 results size=1024 clean|abs=21 vad=1          <- AFE produces clean audio
afe: feed:  2172 frames, last mic|abs=92 ref|abs=0
cloud -> {"type":"vad","probability":0.03..0.82,"speaking":false->true}
cloud -> {"type":"speech_start","probability":0.7059,"offset_seconds":4.608,...}
cloud -> {"type":"result","text":"星星。","wake_status":"sleeping",...}
```

**A looping audio source next to the device → ES7210 mic → on-board esp-sr AFE
(AEC/NS/VAD) → WS uplink → audio_interact Silero VAD → cloud ASR transcribed "星星。"**
The whole full-duplex path works.

### WS session instability → the cloud's VAD firehose
The socket kept dropping and reconnecting. Cause: audio_interact streams a per-frame
`{"type":"vad","probability":...}` message — 20–140/second. Every one of them cost the
firmware a `malloc` for frame reassembly + a `cJSON_ParseWithLength` (+ the debug log)
in the WS task, on top of already-tight internal RAM with the AFE running.

Fix: `handle_text` drops `"type":"vad"` frames with a `memmem` check before any
allocation, parse or log. Nothing in this firmware acts on them (barge-in uses the
local AFE VAD plus the cloud's `speech_start`/`tts_cancel`).

### Serial/tooling notes
- `idf.py monitor` needs a TTY → unusable over SSH. Use pyserial: reset with
  `setDTR(False); setRTS(True); sleep .1; setRTS(False)`, close, wait ~2.5 s for the
  USB-CDC to re-enumerate, reopen with retries, then read.
- `esptool --after hard_reset` is a no-op on USB-Serial-JTAG (no RTS pin) — after
  `idf.py flash` the chip stays in the stub until you toggle the lines yourself.
- The Tailscale link to `wsl` flaps; wrap SSH calls in retries.

## 2026-09-10 — L1 PASSED: 36 dB ERLE

### Result

Quiet room, single talk, on-device `aec_erle` command (no cloud, no serial):

| excitation | ERLE (converged) | ERLE (settle) | mic | ref | clean | idle_clean |
|---|---|---|---|---|---|---|
| noise vol=100 | **36.2 dB** | 28.3 | 1659.5 | 3654.2 | 25.7 | 94.4 |
| noise vol=100 (repeat) | **35.1 dB** | 21.8 | 1655.3 | 3654.4 | 29.0 | 25.5 |
| tone 1 kHz vol=100 | **26.9 dB** | 21.6 | 6847.4 | 8889.4 | 309.1 | 65.1 |
| volume=0 (control) | 0.6 dB | 0.5 | 91.5 | 0.6 | 85.1 | 122.3 |

- Reproducible at ~35–36 dB with broadband excitation, comfortably past the
  ≥25 dB bar for a hardware reference.
- Converged output (clean 25.7) is **below** the idle floor (94.4) — the echo is
  gone, not merely attenuated.
- Convergence goes the right way (settle < measure).
- A pure 1 kHz sine reads ~9 dB lower: a single tone is poor excitation for an
  adaptive filter. Broadband is the right test signal.
- The `volume=0` control reads ~0 dB, proving the measurement plumbing itself is
  not manufacturing the number.

### The 5 dB red herring: double-talk protection

Earlier runs with a looping audio source next to the device read only 4.7–5.9 dB,
and got *worse* from settle to measure. That is not a defect — with near-end
audio present the AEC's double-talk detector deliberately backs off suppression
so it does not eat the user's speech. Removing the near-end source took the same
test from 5 dB to 36 dB. **Always measure single-talk ERLE in a quiet room.**

The naive `20*log10(mic/clean)` formula is also dominated by near-end energy,
because the AEC correctly leaves near-end alone. Compensating with the idle floor
(`echo_in = sqrt(mic^2 - near^2)` etc.) recovers a sane number, but a quiet room
is simpler and unambiguous.

### AFE config actually in effect (`afe_config_print` after `afe_config_check`)

```
pcm_config.total_ch_num: 2   mic_num: 1 [ch0]   ref_num: 1 [ch1]   sample_rate: 16000
afe_type: VC        afe_mode: HIGH PERF        memory_alloc_mode: 3 (more PSRAM)
aec_init: true      aec mode: VOIP_HIGH_PERF   aec_nlp_level: AGGRESSIVE
aec_filter_length: 4
se_init: false (BSS)   ns_init: true (WEBRTC)   vad_init: true (mode 3, WebRTC)
agc_init: false        wakenet_init: false      afe_linear_gain: 1.0
```
Channel order matches the physical measurement from `aec_probe` (ch0 = mic,
ch1 = hardware loopback), so `"MR"` is right. No model partition needed.

### Control plane: everything off TLS

`:443` is MITM-broken on this ISP path, so the WS audio uplink, MQTT control and
device-hub HTTP all had to go plain:

- MQTT `wss://www.wangyutang.cn/mqtt` → `ws://110.40.154.41/mqtt` (Caddy already
  has `handle /mqtt*`). This restored commands **and** ACKs.
- The device-hub heartbeat cannot carry commands: HTTPS fails, and the plain-HTTP
  fallback returns an empty body (`heartbeat response bytes=0 body=`), so the
  `{"commands":[...]}` payload never reaches the firmware. Commands go
  `pending → dispatched` on the server and then nothing. Use MQTT.

### Sizing lessons (internal RAM, not PSRAM)

`free_heap` counts PSRAM and hid the real constraint. Added `free_internal` /
`largest_internal` to the heartbeat: ~49 KB free, ~17 KB largest block with the
AFE up.

- ws_client `buffer_size`: 4 K saturated (uplink is 1 KB every ~32 ms; the socket
  dropped after ~16 frames), 16 K fixed the drops but starved internal RAM until
  `xTaskCreate` for the command worker failed. **8 K holds both.**
- `mqtt_cmd` worker stack: 8192 → 4096 was too aggressive; the heavier `aec_erle`
  handler overflowed it (`Backtrace: … 0xa5a5a5a5 |<-CORRUPTED`, `rst:0xc`).
  Now 12288.
- **Retained MQTT command + crashing handler = reboot loop.** The command replays
  on every reconnect. Clear the retained topic (`_mqtt_clear_retained_command`)
  to break it; the runner script now always clears after reading the ACK.

### Status

L0 (hardware reference REAL) and L1 (36 dB ERLE) both pass. Remaining from the
verification plan: L4 (AEC on/off A/B via `CONFIG_AEC_ENABLE`), soak, and
L2/L3 (double-talk word accuracy + barge-in latency) which need a controlled
near-end source — the raspberrypi, once it is reachable again.

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
