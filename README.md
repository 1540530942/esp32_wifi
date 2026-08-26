# ESP32 XiaoZhi / wangyutang.cn

设备侧 ESP-IDF 工程，目标硬件为 ESP32-S3 `bread-compact-wifi-lcd`。

当前实现：

- 连接 2.4 GHz Wi-Fi
- 向 wangyutang.cn device_hub 注册设备
- 每 30 秒上报心跳和状态
- 接收平台命令并发送 ACK
- 支持 `identify`、`reboot` 基础命令
- 保留 XiaoZhi 音频播报、麦克风和屏幕能力的集成边界

完整工程位于 `esp32_wifi_impl/`。网站端 API 尚未部署完成，因此设备端代码先作为联调基线。

```bash
cd esp32_wifi_impl
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM4 flash monitor
```

Wi-Fi 密码和设备令牌只在本地 menuconfig/NVS 中配置，禁止提交到仓库。
