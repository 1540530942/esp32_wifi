# 2026-09-13 · Spark 局域网多轮音频播报

## 用户诉求

让 ESP32 播报已在 Spark 上生成的多轮对话音频。音频获取与播放不能经过
`device_hub`、MQTT 或公网音频接口，也不能改变现有线上控制链路；设备音量仍允许通过
`https://www.wangyutang.cn/devices/` 调节。部署优先使用 OTA，USB 只作回退。

## 方案与边界

- Spark 在局域网地址 `192.168.1.16:8080` 提供静态 WAV。
- ESP32 双击 BOOT 后直接 HTTP GET，轮流播报 `turn02/04/06/08/10_assistant.wav`。
- 单击 BOOT 仍运行原来的 `mic_asr_test`，长按仍运行 `aec_reference_probe`。
- 播放音量使用现有 `s_volume`，因此网页下发的 `set_volume` 仍然生效并写入 NVS。
- `play_audio` 只额外接受 `CONFIG_LOCAL_AUDIO_BASE_URL` 下的 URL，没有开放任意局域网
  URL，避免把线上命令入口变成通用 SSRF 客户端。

## 实现

- `CONFIG_LOCAL_AUDIO_BASE_URL` 默认值为
  `http://192.168.1.16:8080/esp32`。
- `AudioPlayer` 的裸 HTTP 流支持显式端口，并按 RIFF chunk 遍历 `fmt`、`LIST`、`data`
  等块，不再假定 PCM 一定从第 44 字节开始。
- 本地裸 HTTP 播放严格要求 PCM s16le、单声道、16 kHz。
- `tools/serve_local_dialogue_audio.sh` 在 Spark 上把原始 24 kHz assistant 素材转换为
  `esp32/` 下的 16 kHz 副本，然后启动仅绑定 Spark 局域网地址的静态服务器。

## 排查中发现的坑

1. 设备当前固件没有双击逻辑；未部署前双击当然不会有响应。
2. 原始素材为 24 kHz，而 codec 输出为 16 kHz，直接写会以错误速度播放。
3. FFmpeg 输出包含位于 `data` 前的 `LIST/ISFT` 块。旧裸 HTTP 播放器固定读取 44 字节
   头，会把元数据当 PCM 且把 `LIST` 标识误当数据长度；必须使用 chunk 解析。
4. 当前工作机只有 ESP-IDF 5.3，而工程要求 >=5.4；Spark 的 IDF 5.5 源码存在，但对应
   Python 虚拟环境缺失。因此正式编译使用仓库既有 GitHub Actions IDF 5.5.4 环境。
5. 曾误启动一次 `esp32` 目标构建，已立即停止，并把构建自动产生的
   `dependencies.lock` 改动恢复；该错误产物没有烧录或发布。

## 当前运行态（发布前）

- 设备 `esp32-s3-walle` 在线，固件 `esp32-wangyutang-v33-remotewifi`，局域网地址
  `192.168.1.15`，Spark 为 `192.168.1.16`，RSSI -56 dBm，音量 40。
- Spark 已生成 5 个 16 kHz 副本，并已在 `192.168.1.16:8080` 启动静态服务。
- Spark 本机请求 `turn02_assistant.wav`：HTTP 200，首字节约 1.9 ms，253518 bytes。
- 尚待：GitHub Actions 构建、OTA 登记与下发、设备端双击实测和首声延迟记录。
