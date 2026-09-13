# 2026-09-13 · 多轮对话测试音频生成 + ESP32 播放链路排查(未执行播放)

## 背景

用户想为后续的传输/AEC 验证准备一批多轮对话测试素材，围绕"能不能纯本地(不走云端)
验证 AEC 效果""ESP32 和 spark 之间传输 5 秒音频要多久""能不能让 ESP32 播这些测试
音频"这几个问题，边分析边做。整个过程**没有修改任何固件代码，没有重刷设备，
也没有改动 `device_hub` 服务端代码**——本篇只记录分析结论、已生成的产物、和下一步
需要用户拍板的决策点，方便下一个打开这个仓库的 Claude 或 Codex 会话不用从头重问一遍。

仓库里已经有一份 `multi-turn_dialog.txt`（提交 `47dbf78`，仓库所有者本人直接提交，
非 agent），跟这次会话里用户在聊天中重新贴的对话文本**内容几乎一样但不完全一致**
（差异见下方"发现4"），这一点也需要下一个会话留意，别当成两份独立素材。

## 用户提出的问题，按时间顺序

1. spark 和 ESP32 是否物理互联（USB）。
2. 设想一个纯端侧的 AEC MVP：安静环境 vs TTS播报环境两种场景，音频和ASR结果通过
   spark 获取，不影响现有云端链路。
3. 追问传输延迟：ESP32→spark 传 5 秒音频要多久，"只通过本地传输"——先问的是广义
   本地，后来明确问的是 USB 那条物理连接，最后问 USB 和 WiFi局域网 哪个延迟更低、
   哪个能最快验证 AEC 效果。
4. 给了一段 10 轮的文言文多轮问答对话（用户/assistant 交替：桃花源记 → 陶渊明
   《饮酒·其五》→ 辛弃疾《破阵子》→ 李白《望庐山瀑布》），要求在 spark 上把每一轮
   文本合成语音，"方便后面传输验证"。
5. 要求把生成的音频也存一份到 Windows 桌面路径
   `C:\Users\Administrator\Desktop\Data\audio`。
6. 问能不能让 ESP32 播放其中的 `*_assistant.wav`，以及"直接在 spark 上面获取数据
   是不是会更快"。

## 发现 1：spark ↔ ESP32 物理链路确认可用，但只是调试口，不能传音频

`/dev/ttyACM0` 存在，`lsusb` 显示 `303a:1001 Espressif USB JTAG/serial debug unit`，
串口能读到设备实时日志（心跳、AFE feed/fetch 帧率等），确认设备在线且固件在正常
运行。

但这条 USB 线走的是 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`——纯粹是固件的
日志打印通道。翻遍 `esp32_wifi_impl/main/*.c*`，**没有任何代码把 PCM 音频字节写向
这条串口**。所以"USB 传音频要多久"这个问题现在没有测量对象，要测得先从零写一套
"设备把音频灌进串口、spark 从串口读出来计时"的诊断协议——这是新增功能，不是"跑个
测试"。

## 发现 2：现有的 `aec_capture` 云端链路已经是能测 AEC 效果的最快路径，不需要再搭"本地"版本

`aec_capture {seconds, settle, volume, text}` 命令本来就会做三通道（`mic_raw` /
`reference` / `clean_aec`）同步录制、自动循环播 TTS 填满窗口、HTTP 上传到云端
`110.40.154.41`，再送 `qwen3-asr-1.7b` 转写——这条链路在 2026-09-10 的 15 轮实测里
已经跑通、并测出了 37.8dB 抑制、字准 1.000 这些硬指标（详见 `docs/aec/SUMMARY.md`）。

如果目标是"验证 AEC 效果"，直接复用这条现成链路就是成本最低、风险最低的路径——
零新代码、零刷机风险。搭一条"纯本地不经过云端"的平行链路（无论是走 WiFi 局域网还是
USB）本质上是在解决"排除公网延迟这个变量"的问题，跟"AEC 有没有把回声消掉"这个问题
是两件事，不应该混在一起决策。

## 发现 3：要测真正的"本地"传输，两条路线里 WiFi 局域网明显优于 USB，但都需要改固件

- **USB**：如发现1所述，协议从零搭，工程量最大，且 USB-Serial-JTAG 本身不是为批量
  数据传输设计的，实际吞吐大概率不如 WiFi。
- **WiFi 局域网**：`aec_upload_wav()`（`esp32_wifi_impl/main/app_main.cpp:446`）的
  HTTP POST 逻辑本来就是现成的，只需要把硬编码的目标地址从 `110.40.154.41` 改成
  spark 的局域网 IP（`192.168.1.16`），spark 端起一个几行的接收脚本即可。改动量是
  "改一个字符串常量"，不是"设计新协议"。

**但两条路线都需要真实刷机**——这台设备是在线生产设备，且此前在 spark 上重建
`sdkconfig` 时发生过一次误清空 WiFi 凭据、设备真掉线的事故（见
`2026-09-13-spark-flash-wiped-wifi-creds-incident.md`，另一仓库）。所以这类改动
**没有征得用户同意前不会执行**，本次会话仅做了分析，没有改代码、没有编译、没有刷机。

## 发现 4：本次合成用的对话文本，跟仓库里已有的 `multi-turn_dialog.txt` 有一处不一致

仓库根目录的 `multi-turn_dialog.txt`（提交 `47dbf78`，作者是仓库所有者本人，非
agent）里，第 2 轮 assistant 的回复是：

```
好的，你说。
```

而这次会话里用户在聊天中重新贴出的版本，第 2 轮是更完整的：

```
好的，请你说出片段，我来帮你判断文章名称、作者，并简要介绍相关背景。
```

**本次生成音频用的是聊天里贴的这个更完整版本**，不是仓库里已提交的那份。其余 9 轮
文本两者一致。如果这两份素材本来是想保持同一份，需要用户确认以哪份为准，否则以后
拿 `multi-turn_dialog.txt` 重新生成音频会跟这次的产物对不上。

## 已生成的产物

在 spark 上用本机的 `qwen3-tts-server`（`localhost:8002`，`POST /v1/audio/speech`，
`GET /v1/models` 列出的可用音色：`Vivian / Serena / Uncle_Fu / Dylan / Eric / Ryan /
Aiden / Ono_Anna / Sohee`）合成了全部 10 轮对话：

- **音色**：`user` 角色统一用 `Dylan`，`assistant` 角色统一用 `Vivian`，
  `language=Chinese`，`response_format=wav`。
- **存放位置**：`spark:~/workspace/data/aec/dialogue_wenyanwen/`，文件名
  `turn{01..10}_{user,assistant}.wav`，同目录下有 `manifest.json` 记录每轮的
  文本、音色、字节数、TTS合成耗时。
- **规模**：10 个文件共约 11MB，累计音频时长约 236 秒。

| 轮次 | 角色 | 音色 | 时长 | 大小 | 备注 |
|---|---|---|---|---|---|
| turn01 | user | Dylan | 4.32s | 202.5KB | |
| turn02 | assistant | Vivian | 7.92s | 371.3KB | |
| turn03 | user | Dylan | 93.12s | 4365.0KB | 桃花源记原文，最长的一段 |
| turn04 | assistant | Vivian | 27.20s | 1275.0KB | |
| turn05 | user | Dylan | 4.08s | 191.3KB | |
| turn06 | assistant | Vivian | 27.28s | 1278.8KB | 饮酒·其五 |
| turn07 | user | Dylan | 3.92s | 183.8KB | |
| turn08 | assistant | Vivian | 27.20s | 1275.0KB | 破阵子 |
| turn09 | user | Dylan | 3.36s | 157.5KB | |
| turn10 | assistant | Vivian | 37.92s | 1777.5KB | 望庐山瀑布 |

**踩的一个坑**：turn03（桃花源记原文，300+字）第一次合成在默认 60 秒 HTTP 超时下
失败（`timed out`）；把超时放宽到 180 秒后重试成功，实际耗时 80.81 秒。这跟
`2026-09-13-tts-latency-investigation-and-fixes.md`（另一仓库）里记录的"TTS 合成
耗时随字数线性增长、非流式接口在长文本下容易撞超时"的结论是一致的，算是又一次
交叉验证。

另外把这 10 个 WAV + `manifest.json` 通过 `ssh wsl` 挂载的 `/mnt/c/...` 路径，
复制了一份到用户 Windows 桌面的 `C:\Users\Administrator\Desktop\Data\audio`，
已核对文件大小与 spark 原始文件一致。

## 发现 5：ESP32 的 `play_audio` 命令有硬编码 URL 白名单，直接指向 spark 会静默失败

查了 `esp32_wifi_impl/main/app_main.cpp:503-532` 的 `play_audio` 处理逻辑：

```c
if (url 以 "https://www.wangyutang.cn" 开头)
    → 重写成 "http://110.40.154.41" + 剩余路径
else if (url 已经是 "http://110.40.154.41/devices/api/" 开头)
    → 原样使用
else if (没有传 name="test")
    → 返回 "unsupported"
// 否则（比如传了 name="test" 或者两个条件都不满足但也没触发上面的 else if）
    → 静默使用默认的 kTestAudioUrl
```

**关键坑**：如果传一个 spark 局域网地址（比如 `http://192.168.1.16:8080/xxx.wav`），
它既不匹配前两个分支，又因为没有传 `name` 字段所以第三个 `else if` 的判断条件
（`cJSON_IsString(name_item)`）也是 false——三个分支都不命中，**代码会往下继续执行
用默认测试音频播放，不会报任何错误**，很容易让人误以为播放成功了、播的却是别的
文件。这是这次分析里挖出来的、值得单独记一笔的隐患点。

不过同时也找到了一条**不用碰固件就能用**的现成通路：`device_hub/server.py:811`
的 `POST /api/device/{device_id}/upload_audio?play=1`——上传文件后存到
`AUDIO_DIR`，生成的下载地址是 `AUDIO_PUBLIC_BASE`（默认
`https://www.wangyutang.cn/devices/api/audio/<filename>`），这个地址**正好匹配**
`play_audio` 白名单里的第一条重写规则，然后自动通过 MQTT 下发 `play_audio` 命令。
也就是说，把这 4 段 `*_assistant.wav` 通过这个接口上传，就能让 ESP32 播放，
**完全不需要改固件、不需要重刷**。

**这一步截至本次会话结束尚未执行**——因为这会让机器人真实出声（4段合计约100秒），
需要用户明确确认后再触发，不属于"只读分析"范畴。

## 现状小结（截至本次会话结束）

- 固件代码、`device_hub` 服务端代码：**均未修改**。
- 测试音频：已在 spark 生成，已同步到 Windows 桌面，尚未上传到 `device_hub`，
  尚未在 ESP32 上播放过。
- USB 传输测速、WiFi局域网直连传输：均为分析结论，均未实现、未测量。

## 留给下一个会话的决策点

1. **要不要现在就用 `upload_audio?play=1` 把 4 段 assistant 音频推给 ESP32 播放**——
   现成通路已确认可用，只差用户点头。
2. **`multi-turn_dialog.txt` 和这次实际合成用的文本，turn2 不一致**——需要确认
   以哪份为准，避免以后重新生成音频时对不上。
3. **是否值得为了"纯本地"验证去改固件**——无论走 WiFi 局域网还是 USB，都需要真实
   刷机，且现有云端 `aec_capture` 链路已经能满足"验证AEC效果"这个目的。如果用户
   后续的真实诉求是"排除公网延迟这个变量"或者"为脱离云端依赖做准备"，才值得投入；
   单纯为了"测个传输时间"不建议为此改动生产设备。
