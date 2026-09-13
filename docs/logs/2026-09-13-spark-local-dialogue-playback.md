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

## 构建、OTA 与恢复结果

- 功能提交为 `22abd10`，tag 为 `ota-esp32-wangyutang-v34-local-audio`。GitHub
  Actions run `34760598090` 使用 ESP-IDF 5.5.4 构建成功，发布版本为
  `esp32-wangyutang-v34-local-audio`，release id `rel-04e2ee45f41e`。
- OTA job `ota-060ecc03d8f6` 能被设备接收且镜像完整写入，但 CI 通用镜像不带真实编译期
  Wi-Fi 凭据，设备重启后无法联网，最终 job 正确结束为 `failed`。这不是音频实现失败，
  而是当前设备在通用 OTA 镜像下没有预先可用的 `wifi_remote` 凭据。
- 为恢复设备，先通过 USB 仅重写 app 分区，保留 NVS、音量和设备身份。排查时一度把
  `otadata` 地址误读成 `0xe000`；本工程 `partitions.csv` 中真实地址是 `0xf000`，seq=3
  对应活动槽 `ota_0`（地址 `0x20000`）。写入非活动 `ota_1` 不会改变启动固件。
- 曾通过 `wifi_set_remote` 写入一组后来无法连接的凭据。恢复固件只删除
  `wifi_remote/ssid` 与 `wifi_remote/pass` 两个键，未擦除整个 NVS；恢复上线后立即刷回
  不含清理逻辑的正式 v34。
- Spark 本地 ESP-IDF 5.5.5 构建出的正式镜像大小为 2460224 bytes，写入活动
  `ota_0` 后 hash 校验通过。

## 实机验证与当前运行态

- Spark 已生成 5 个 16 kHz 副本，并持续在 `192.168.1.16:8080` 提供静态服务。
- Spark 本机请求 `turn02_assistant.wav`：HTTP 200，首字节约 1.9 ms，253518 bytes。
- 为排除云端控制链路影响，刷入一次性本地启动自测镜像。2026-09-13 22:18:02，Spark
  记录到来源 `192.168.1.15` 的
  `GET /esp32/turn02_assistant.wav HTTP/1.0`，HTTP 200；证明 ESP32 已直接从 Spark 获取并
  播放本地 WAV。自测后已移除启动播放逻辑并重新刷回正式镜像，重启没有再次自动 GET。
- 当前设备 `esp32-s3-walle` 在线，固件 `esp32-wangyutang-v34-local-audio`，IP
  `192.168.1.15`，音量 40。NVS 中的原音量得到保留。
- 两次通过网页 API 排队的临时 `play_audio` 测试命令停在 `dispatched`，设备没有访问
  Spark；这是当前 MQTT 下发链路的独立问题，不影响 BOOT 双击的纯本地路径。测试命令
  id 为 `c-22f1e2`、`c-083e26`，后续排查控制链路时可据此追溯。
- 自动化已经覆盖完整的本地取流/播放入口；BOOT 双击的物理按键手感与双击窗口仍需人在
  设备旁最终确认。每次成功双击依次轮播第 2、4、6、8、10 轮，随后循环。
