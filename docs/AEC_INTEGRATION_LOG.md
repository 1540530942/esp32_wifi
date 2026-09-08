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
