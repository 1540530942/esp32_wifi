# Device Hub API Contract v1

Base URL: `https://www.wangyutang.cn/devices/api`

All requests use `Content-Type: application/json` and:

```http
Authorization: Bearer <DEVICE_TOKEN>
```

## Register

`POST /register`

```json
{
  "device_id": "esp32-s3-walle",
  "device_type": "esp32_xiaozhi",
  "firmware": "esp32-wangyutang-v1",
  "capabilities": "audio_playback,microphone,display"
}
```

## Heartbeat

`POST /heartbeat`

```json
{
  "device_id": "esp32-s3-walle",
  "state": {
    "wifi_ssid": "ChinaNet-dsge",
    "wifi_rssi": -55,
    "ip": "192.168.1.20",
    "uptime_s": 120,
    "free_heap": 120000
  }
}
```

The response may contain:

```json
{
  "commands": [
    {"command_id": "cmd-1", "action": "identify", "payload": {}}
  ]
}
```

## Acknowledge

`POST /ack`

```json
{
  "device_id": "esp32-s3-walle",
  "command_id": "cmd-1",
  "status": "done"
}
```

Initial actions: `identify`, `reboot`, `play_test_audio`. `play_test_audio` downloads the repository's `你今天好吗.wav` over HTTPS and plays it through the board speaker at 20% software-scaled volume. The production platform should expose this only to authenticated operators.
