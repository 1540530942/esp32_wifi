# ESP32 XiaoZhi / wangyutang.cn

设备侧 ESP-IDF 工程，目标硬件为 ESP32-S3 `bread-compact-wifi-lcd`。

当前实现：

- 连接 2.4 GHz Wi-Fi
- 向 wangyutang.cn device_hub 注册设备
- HTTP 心跳与 MQTT over WSS 控制面
- HTTP WAV 与 WebSocket PCM 播报
- `identify`、`reboot`、`set_volume`、`play_audio`、`stream_prepare`、`ota`
- 双 OTA 分区、下载进度、重启后版本确认和 Bootloader rollback
- GitHub Actions 构建并发布固件到 `/devices/ota`

完整固件工程位于 `esp32_wifi_impl/`，云侧控制面源码位于 `cloud/device_hub/`。
自动化发布和现场操作见 [`docs/OTA_RELEASE.md`](docs/OTA_RELEASE.md)。

```bash
cd esp32_wifi_impl
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM4 flash monitor
```

Wi-Fi 密码只在首次 USB 配置时写入 NVS，后续 OTA 镜像从 NVS 读取，禁止提交到仓库。
