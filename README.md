# ESP32 XiaoZhi / wangyutang.cn

设备侧 ESP-IDF 工程，目标硬件为 ESP32-S3 `bread-compact-wifi-lcd`（N16R8）。

## 功能

- 连接 2.4 GHz Wi-Fi，凭据存入 NVS，断电重启自动恢复
- 向 wangyutang.cn device_hub 注册设备
- HTTP 心跳与 MQTT over WSS 控制面（MQTT 不可用时自动降级到 HTTP 心跳）
- HTTP WAV 与 WebSocket PCM 播报，支持远程音量控制
- `identify`、`reboot`、`set_volume`、`play_audio`、`stream_prepare`、`ota`
- 双 OTA 分区、下载进度、重启后版本确认和 Bootloader rollback
- GitHub Actions 构建并发布固件到 `/devices/ota`
- **1602A LCD 状态显示**（v11+）：上电显示 BOOTING，注册后显示 ONLINE + IP，心跳轮播 IP ↔ OTA 版本

## 硬件接线

### 音频（ES8311 DAC + ES7210 ADC）

| 信号 | GPIO |
|------|------|
| I2C SDA | 1 |
| I2C SCL | 2 |
| I2S MCLK | 15 |
| I2S WS (LRCK) | 13 |
| I2S BCLK | 14 |
| I2S DIN（麦克风） | 12 |
| I2S DOUT（喇叭） | 16 |
| PA Enable | 17 |

### 1602A LCD（HD44780 4-bit 并口）

| 1602A 引脚 | 名称 | 接到 |
|-----------|------|------|
| 1 (VSS) | GND | GND |
| 2 (VDD) | 电源 | **5V** |
| 3 (V0) | 对比度 | 1kΩ 电阻接 GND（或 10kΩ 电位器） |
| 4 (RS) | 寄存器选择 | **GPIO3** |
| 5 (RW) | 读写 | GND（固定写模式） |
| 6 (E) | 使能 | **GPIO4** |
| 7–10 (D0–D3) | — | 不接（4-bit 模式） |
| 11 (D4) | 数据 | **GPIO5** |
| 12 (D5) | 数据 | **GPIO6** |
| 13 (D6) | 数据 | **GPIO7** |
| 14 (D7) | 数据 | **GPIO8** |
| 15 (A/LED+) | 背光正极 | 5V 串 220Ω 电阻 |
| 16 (K/LED-) | 背光负极 | GND |

> ESP32-S3 输出 3.3V 逻辑，HD44780 TTL 兼容，可直接驱动无需电平转换。

### LCD 显示内容

| 状态 | Line 1 | Line 2 |
|------|--------|--------|
| 启动中 | `ESP32  BOOTING ` | `Connecting WiFi` |
| 在线（交替显示） | `ESP32  ONLINE  ` | IP 地址 |
| 在线（交替显示） | `ESP32  ONLINE  ` | `OTA:v11-lcd...` |
| 离线 | `ESP32  OFFLINE ` | （空白） |

## 构建与烧录

```bash
cd esp32_wifi_impl
idf.py set-target esp32s3
idf.py menuconfig      # 填写 Wi-Fi SSID/密码（仅首次）
idf.py build
idf.py -p COM4 flash monitor
```

Wi-Fi 密码只在首次 USB 配置时写入 NVS，后续 OTA 镜像从 NVS 读取，禁止提交到仓库。

完整固件工程位于 `esp32_wifi_impl/`，云侧控制面源码位于 `cloud/device_hub/`。
自动化发布和现场操作见 [`docs/OTA_RELEASE.md`](docs/OTA_RELEASE.md)。
