# ESP32 全双工自动验证台 —— 实现规划

**核心思路：树莓派扮演"房间里的那个人"。**

| 真实场景 | 台架里的对应物 |
|---|---|
| 人开口说话 | 树莓派**扬声器**播报已知文本 S |
| 人的耳朵听机器人说了什么 | 树莓派**麦克风**录音 |
| 机器人自己在播报（回声源） | ESP32 **扬声器**播报已知文本 E |
| 机器人的耳朵（被测对象） | ESP32 **麦克风** + ES7210 回采 → AFE |

两台设备各有一对收发，但角色完全不同：**ESP32 那对是被测对象，树莓派那对是测量仪器**。

这个分工带来两条独立的证据链，缺一不可：

- **上行链**（ESP32 麦 → AFE → 云端 ASR）：机器人**听懂了人吗**？判据是
  `clean_aec` 的转写与 S 的字准率。
- **下行链**（树莓派麦录音）：**人感受到机器人被打断了吗**？判据是这段录音里
  「S 开始」到「E 的能量塌下去」的时间差。

第二条以前没法测，因为要跨设备对时钟。现在整个打断过程落在**同一个文件**里 ——
树莓派的一次录音同时看得见自己嘴上说的和耳朵里听到的机器人 —— 时间差直接读出来，
一点同步都不需要。

之前的近端是人手动外放歌词：内容只能猜、电平不可控、人睡了就断，而且完全没有"耳朵"。
换成树莓派后这些一起解决，判据从"看着像"变成可计算的数。

---

## 1. 现状（2026-09-10）

### 已就绪

| 组件 | 状态 | 说明 |
|---|---|---|
| ESP32 `aec_capture {seconds,settle,volume,text}` | ✅ | TTS 片段循环填满窗口，三通道 WAV 自动上传 device_hub |
| ESP32 `aec_config` 命令 | ✅ 已 OTA 上线 | 回读 `afe_config_check()` 之后真正生效的配置，替代读串口 |
| ESP32 `AEC_NS_ENABLE` 编译开关 | ✅ 已 OTA 上线 | 同一份固件 `v21-aeccfg`，默认 ns=1 |
| 固件托管 `http://110.40.154.41/fw/` | ✅ | `esp32_wangyutang-v21-aeccfg.bin` 已上传，实测 HTTP 200 |
| 设备侧 `ota {url,version}` | ✅ | 双分区，版本号相同时跳过（防 retained 回放重启循环） |
| 树莓派 `speak` 技能（嘴） | ✅ | `POST /action/api/tasks {action:"speak"}`，aplay `plughw:2,0`，音色 vivian |
| 树莓派 `listen` 技能（耳） | ✅ 已推送 | arecord 16k 单声道，回报逐 100ms RMS 包络 + 云端 ASR 转写，不搬运音频文件 |
| 树莓派 TTS 拉取超时 | ✅ 已推送 | 原为固定 30 s（143 字脚本必失败），改为 15 s + 0.5 s/字，上限 150 s |
| 树莓派按任务音量 | ✅ 已部署 | `settings_override.voice_volume_percent`（默认 0 = 静音，必须显式传） |
| 树莓派 speak 认领超时 180 s | ✅ 已部署 | 其他动作仍 35 s |
| 树莓派本地语音缓存 | ✅ 已推送 | 按 文本+模型+音色+语言+instructions 哈希存 `speak_cache/`，重复播报读盘、字节一致 |
| 云端 ASR | ✅ | `http://110.40.154.41/common/api/asr/transcribe`，qwen3-asr-1.7b，确定性输出 |
| 分析脚本 | ✅ | device-hub 容器 `/app/an.py`（三通道 RMS+ASR）、`/app/seg.py`（逐秒 RMS） |
| 命令下发 | ✅ | `/app/cmd_run.py`，MQTT 直发、等 ACK、清 retained |

### 阻塞

- ~~ESP32 物理掉线~~ —— 已恢复，v21 固件已 OTA 上线并回读确认。
- **`listen` / `speak` 队列串行**：两者都走 action_move 任务队列，需实测能否重叠（见 §2.2）。
- device_hub 的 OTA 管理 API（`install_ota_routes`）从未被挂载 —— **不修它**，直接走
  `cmd_run.py ota`。

### 已知边界

- 内部 RAM 常驻后只剩 ~34 KB；20 s 捕获要 3 × 640 KB PSRAM。别再加长。
- WebRTC NS 会把持续声源越学越当背景（`FINDING_ns_vs_asr.md`），`clean_aec` 的近端
  逐秒衰减。这是待测变量之一，不是台架要绕开的东西。
- 晚 22:00–08:30 不跑出声的用例（树莓派音箱会吵人），台架默认拒绝，`--allow-night` 覆盖。

---

## 2. 台架设计

### 2.1 角色

```
   ┌─────────── 树莓派 = 人 ───────────┐        ┌──── ESP32 = 被测机器人 ────┐
   │                                   │        │                            │
   │  speak  ──嘴──▶ 说出 S ───────────┼─声波──▶│ ES7210 ch0  mic_raw ──┐    │
   │                                   │        │ ES7210 ch1  reference ┼─▶AFE│──▶ clean_aec
   │  listen ──耳──◀ 听到 S+E ◀────────┼─声波───│ ES8311 喇叭 ◀── 播 E ─┘    │      │
   │           ↓                       │        │                            │      ▼
   │   [ENV100] 逐100ms RMS            │        └────────────────────────────┘   上传三通道
   │   [ASR]    转写                   │                                             │
   └───────────────────────────────────┘                                    云端 ASR ×3
                    │                                                                │
                    ▼                                                                ▼
            下行链：打断延迟                                            上行链：近端字准 + 回声泄漏
```

- **S（近端脚本）**：一段 ~180 字的中文，切成 8–10 句，每句 ≤ 20 字。第一次播报走云端 TTS
  （≤ 90 s，在 180 s 超时内），之后全走缓存 —— 一次任务连续播完整段，**没有句间空档**，
  也不用再排多个任务（多任务会被 30 s 的 `ttl_seconds` 过期掉，今早就是这么丢的两句）。
- **E（远端脚本）**：固定用 `今天天气不错，我们一起出去走走吧，你觉得怎么样呢，要不要带上相机。`
  —— 已有三组基线用的就是它，循环填满窗口。

### 2.2 时序对齐

不追求精确对齐，用**长窗 + 脚本级打分**吸收抖动：

1. 先发树莓派 `listen`（耳朵先开，窗 25 s，覆盖后面所有事件）。
2. 发 ESP32 `aec_capture`（TTS 拉取 ~5 s + settle 2 s 后开录，窗 20 s）。
3. 2 s 后发树莓派 `speak`（缓存命中：poll ≤ 5 s + 读盘，约 3–7 s 后出声）。
4. 三者都落在 25 s 窗内；S 长 ~40 s 完全覆盖 ESP32 的 20 s 录制窗。

注意 `listen` 和 `speak` 都要经 action_move 的任务队列，而**同一时刻只有一个任务在跑** ——
`listen` 会占住 poller。所以 `listen` 必须由**编排器和 `speak` 并发发出前先入队**，或者
把两者合成一个技能。第一版先用「先 listen 后 speak」两条任务实测队列行为，如果串行化了
就加 `speak_while_listening` 复合动作。

捕获窗的绝对时间 = device_hub 里该命令的 `done_at − seconds − 0.3`。树莓派任务的
`claimed_at / completed_at` 也在 action_move 里。两者对得上就知道窗内播了 S 的哪一段。

### 2.3 打分

对每个通道的转写 T：

| 指标 | 定义 | 用途 |
|---|---|---|
| `S_recall` | 对 S 的每一句 s_i 算 LCS(s_i, T)/len(s_i)，≥ 0.6 记为"认出"；认出句数 / 窗内实际播过的句数 | **近端可懂度** |
| `S_char` | LCS(窗内 S 片段, T) / len(片段) | 字级召回，细粒度 |
| `E_leak` | LCS(E, T) / len(E) | **红线**：clean_aec 上 > 0.3 即判死 |
| `near_rms / echo_rms` | 近端-only 与回声-only 控制组各测一次 | 把每个格子换算成 近端/回声比（dB） |

`mic_raw` 通道是对照：它的 `E_leak` 应该高、`S_recall` 应该低 —— 证明近端在原始信号里
确实被盖住了，AEC 后认出来才算 AEC 的功劳。

### 2.3b 下行链打分（树莓派耳朵）

`listen` 返回逐 100 ms 的 RMS 包络 `[ENV100]` 和转写 `[ASR]`。在同一条包络上找两个点：

- **t_speak**：包络从底噪抬起并持续 ≥ 300 ms 的首个窗 —— "人"开口。
  （树莓派自己的嘴离自己的耳最近，这一跳最陡，最好找。）
- **t_duck**：t_speak 之后，包络跌到 t_speak 前平台值的 **1/3** 以下并保持 ≥ 200 ms 的首个窗
  —— 机器人的播报被压下去了。

`duck_latency = t_duck − t_speak`，分辨率 100 ms。要更细就把 `LISTEN_ENVELOPE_MS` 调到 20。

| 指标 | 定义 | 对应门禁 |
|---|---|---|
| `duck_latency` | 上式，取 20 次的 p95 | G5.1 |
| `false_duck` | 安静房间只让 ESP32 播报，包络出现上述跌落的次数 | G5.3 |
| `recover` | 虚警后包络是否回到原平台（不卡在低音量） | G5.4 |
| `heard_E` | `[ASR]` 对 E 的 LCS —— "人"听到机器人说了什么 | 辅助：确认 E 真的在放 |

**这条链完全不依赖 ESP32 侧的任何时间戳**，也不需要两台设备对时。云端 `tts_cancel`
那一段（G5.2）仍需设备侧日志，排在后面。

### 2.4 测试矩阵

| | ESP32 vol 0（近端 only） | 25 | 40 | 60 |
|---|---|---|---|---|
| **Pi vol 55** | 标定 near_rms | · | · | · |
| **Pi vol 70** | 标定 near_rms | · | · | · |
| **Pi vol 85** | 标定 near_rms | · | · | · |
| **Pi 静音**（回声 only） | 空跑对照 | 标定 echo_rms | 标定 echo_rms | 标定 echo_rms |

16 格 × (~20 s 录 + ~15 s 上传 + ~30 s 三路 ASR) ≈ 18 min 一整轮。每格结果一行 JSON，
一轮一个 run 目录。

### 2.5 判定（对应 `DESIGN` 的 G4）

- **G4.1**：近端/回声比 ≥ −25 dB 的格子，`clean_aec.S_recall ≥ 0.8`
- **G4.2**：同格 `mic_raw.S_recall ≤ 0.3`（近端确实被盖住）
- **G4.3 红线**：所有格子 `clean_aec.E_leak = 0`（任何一格 > 0.3 整轮 FAIL）
- **G4.4**：报告 S_recall 跌破 0.8 的最低比值 —— 这就是产品的工作边界

---

## 3. 实现步骤

### 第 0 步 —— 设备回来后（现场）

插好 ESP32 → 等心跳 → `cmd_run.py aec_probe '{}' 60` 看到 `reference=REAL`。

### 第 1 步 —— OTA 上 v21，验证不碰 USB  ✅ 已完成

```
done | fmt=MR type=VC aec=1 nlp=1 filt=4 ns=1 vad=1 rate=16000
```
心跳 `firmware=esp32-wangyutang-v21-aeccfg`。**G1 通过：全程未碰 USB。**

<details><summary>原步骤</summary>

```
cmd_run.py ota '{"url":"http://110.40.154.41/fw/esp32_wangyutang-v21-aeccfg.bin","version":"esp32-wangyutang-v21-aeccfg"}' 240
cmd_run.py aec_config '{}' 30      # 期望 done|fmt=MR type=VC aec=1 nlp=1 filt=4 ns=1 vad=1 rate=16000
```

心跳里 `firmware` 字段应变为 `esp32-wangyutang-v21-aeccfg`。
</details>

以后所有固件变更（NS 开关、`aec_capture` 加参数）全走这条路。

### 第 2 步 —— 树莓派预热与标定（不需要 ESP32 出声）

1. 用 vol 70 播一次完整 S（首次走 TTS，落缓存）。ACK 里应有 `source=tts`；再播一次应为
   `source=cache`，端到端 < 2 s。
2. ESP32 `aec_capture {volume:0, seconds:20}` 同时 Pi 播 S，三档 Pi 音量各一次 →
   `mic_raw` RMS 就是 `near_rms(Pi_vol)`。同时验证 `mic_raw` 的 ASR 能认出 S（近端本身
   到麦是清晰的，否则后面全无意义）。
3. Pi 静音，ESP32 三档各录一次 → `echo_rms(ESP_vol)`。已有数据：v25 88 / v40 312 / v60 378。
4. **耳朵标定**：只让 ESP32 播 E，树莓派 `listen 15`，确认 `[ASR]` 认得出 E、包络平台明显
   高于底噪 —— 否则 §2.3b 的 t_duck 无从判起。

### 第 3 步 —— 编排脚本 `aec_bench.py`（放 device-hub 容器 `/app/`）

```
aec_bench.py --pi-vols 55,70,85 --esp-vols 0,25,40,60 --seconds 20 [--allow-night] [--ns off]
```

每格：检查 ESP32 心跳 < 30 s、Pi idle → 发 listen → 发 capture → 2 s 后发 speak → 等三者结束 →
找到本次上传的 3 个文件 → 三路 ASR → 算 §2.3 四个指标 + §2.3b 打断延迟 → 写
`/app/data/aec_bench/<run_id>/<cell>.json`。结束打印 §2.4 的表 + §2.5 判定。

跑完的 JSON 和汇总表由循环那边（我）拷到 `docs/aec/bench/<run_id>/` 提交；WAV 归档到
WSL 的 `~/workspace/data/aec/bench/<run_id>/`，不进 git。

### 第 4 步 —— 用台架做剩下的参数对照

每个对照 = 改一个编译开关 → 改版本号 → build → 上传 `/fw/` → OTA → `aec_config` 确认 →
`aec_bench.py` 整轮 → 和基线整轮逐格比。顺序：

1. `AEC_NS_ENABLE=0`（NS 关）—— 直接针对 `FINDING_ns_vs_asr.md`
2. `afe_ns_mode = AFE_NS_MODE_NET`（NSNET，需确认 model 分区）
3. `AFE_TYPE_SR`

每个对照都要**先跑一轮当前固件的基线**再换固件 —— 跨时段比较已经骗过一次
（`NLP_LEVEL_AB.md`）。

---

## 4. 固件侧可选小改（不阻塞，第 3 步跑顺后再做）

- `aec_capture` 加 `delay_ms`：开播前等待，让编排器把两边起点对得更准，缩短窗长省 PSRAM。
- ACK 里带 `t_capture_start_us`（`esp_timer_get_time()`），对齐不再靠 `done_at` 倒推。
- `aec_capture` 加 `no_play: true`：纯录制模式，标定 near_rms 时不必拉一次 TTS。

---

## 5. 风险

| 风险 | 缓解 |
|---|---|
| 树莓派 poller 空闲 poll 5 s，`speak` 起点抖动 | 长窗 + 脚本级打分吸收；必要时 §4 的 `delay_ms` |
| `listen` 占住 poller 导致 `speak` 排队 | §2.2 先实测；必要时做 `speak_while_listening` 复合动作 |
| 树莓派自己的嘴声压过耳朵，掩盖 E 的塌陷 | 耳朵标定（第 2 步第 4 项）确认 E 平台可辨；必要时拉开嘴/耳与 ESP32 的相对距离 |
| 20 s 捕获 OOM（3 × 640 KB） | 已跑通多次；G6.4 盯着；不再加长 |
| Pi 与 ESP32 相对位置变了，near_rms 漂移 | 每轮开头重做第 2 步标定（3 格，~2 min），结果里带 near_rms 而非 Pi 音量 |
| ASR 对短片段幻觉（曾在近静音上吐出英文句子） | 打分用 LCS 对已知文本，幻觉句对不上 S 也对不上 E，自然得 0 |
| `settings_override` 忘传音量 → 静音成功 | 台架硬编码传；ACK 校验 `voice_volume_percent` 非 0 |
| 夜间跑出声 | 台架默认 22:00–08:30 拒绝 |
