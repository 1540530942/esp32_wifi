# 2026-09-14 · 当前 ESP32 外设与连接关系核对

## 核对目的

确认当前 `esp32-s3-walle` 固件所使用的外设、连接方式和用途，并区分源码配置、实机测量
以及仍需现场目视确认的项目。

## 当前运行态

- 设备在线，固件 `esp32-wangyutang-v36-heartbeat-fix`，IP `192.168.1.15`，Wi-Fi
  `1-1-1204`，MQTT 已连接。
- Spark 上存在 `/dev/ttyACM0`，是当前首选 USB 调试/恢复链路；USB 用于供电、烧录和
  调试，不承担本地 WAV 传输。WSL usbipd 是 Spark 不可用时的备用链路。

## 音频外设

- `ES7210` ADC：通过 I2C（SDA GPIO1、SCL GPIO2）配置，通过 I2S0/TDM DIN GPIO12
  输入。当前 AFE 格式为 `MR`：ch0 是原始麦克风，ch1 是功放硬件回采参考。
- 硬件参考已实测为真实信号：静音 RMS 约 0.6，播放 1 kHz 音时 RMS 约 1720.7；当前
  `CONFIG_AEC_SOFTWARE_REF=n`，AEC 使用该硬件参考。
- `ES8311` DAC：通过同一 I2C 总线配置，通过 I2S0 DOUT GPIO16 输出到扬声器链路；
  GPIO17 控制 PA enable。
- 输入、参考和输出都配置为 16 kHz。RX/TX 使用同一个 I2S0，并共享 MCLK GPIO15、
  BCLK GPIO14、WS/LRCLK GPIO13，因此麦克风与硬件参考位于同一个采样时钟域。

## 其他外设

- BOOT 按键：GPIO0；当前用于单击、双击本地 Spark WAV 和长按 AEC 诊断。
- 1602A LCD：源码和接线文档声明 GPIO3–8，显示启动、在线、IP 和 OTA 版本。历史记录中
  固件驱动已运行，但当前是否仍实际接线且显示内容正常，远程无法目视确认。

## 非直连设备

- Spark 的 `192.168.1.16:8080` 是局域网音频源，不是 I2S 外设；ESP32 通过 Wi-Fi HTTP
  直接拉取 WAV。
- 树莓派的麦克风/扬声器用于 AEC 台架测试，通过网络协同，不直接接在 ESP32 GPIO/I2S。
