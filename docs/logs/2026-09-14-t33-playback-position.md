# 2026-09-14 · T3.3 播放位置回报（spk_buffer_ms / spk_played_ms）

## 背景

任务书 P3 的最后一项：

> T3.3 播放位置回报：ESP32 心跳上报 `spk_buffer_ms`。已播时长 = 已发送时长 − 缓冲剩余。
> 接 LLM 后用于对话历史截断——这是打断里最容易漏的一步。TTS 念到第 20 字被打断，
> 若把完整 80 字写进历史，模型以为自己全说完了，后面会接着往下讲。

树莓派仍然掉线（见 `WORKING.md` 顶部），E2b/E3/E4 全部卡住。T3.3 是**唯一不需要
树莓派**的未完成项，所以这一轮做它。

## 怎么算出"缓冲里还剩多少"

没有去问 I2S 驱动要缓冲水位（ESP-IDF 没有稳定的公开接口），而是利用一个现成的
事实：**`AudioCodec_OutputData()` 在 DMA 环满的时候会阻塞**。

所以写入方能跑在喇叭前面多远，完全由 DMA 环的深度决定：

```
spk_buffer_ms = 已写入音频时长 − 首次写入至今的墙钟时间
```

这个差值就是压在缓冲里、还没出喇叭的音频——也正是一次打断会丢掉的部分。

### 一个容易搞错的起点

计时**从第一次写入开始，不是从 `play_task()` 进入开始**。两者之间还隔着 HTTP 连接
建立和 WAV 头解析（几百毫秒量级）。如果把这段算进"已流逝"，头一秒的缓冲估计会
系统性偏小。

```cpp
void AudioPlayer::note_samples_written(int samples) {
    if (samples <= 0) return;
    int64_t expected = 0;
    first_write_us_.compare_exchange_strong(expected, esp_timer_get_time());
    samples_written_.fetch_add(static_cast<uint64_t>(samples));
}
```

只有播放任务这一个调用方，所以 `compare_exchange` 在这里只是一次便宜的"是不是第一次"
判断，不是在处理竞争。

三条播放路径（PCM WebSocket / WAV 流 / raw-HTTP WAV）各有一处
`AudioCodec_OutputData()`，三处都挂上了这个钩子——只挂一处的话，`play_lan_audio`
走的 raw-HTTP 路径就统计不到。

### 结束时要"冻结"，而且要区分正常放完和被打断

```cpp
last_played_ms_ = stop_requested_ ? written_ms - buffered_ms : written_ms;
```

在 `busy_` 还是 true 的时候算，否则 `spk_buffer_ms()` 已经返回 0 了。

- **正常播完**：缓冲最终会排空，听众听到了全部写入的内容 → played = written。
- **被打断**：缓冲被丢弃 → played = written − buffer，差值就是用户**没听到**的那段。

对话历史要截断到的是后者。这个区分是 T3.3 的全部意义所在——如果不分，被打断和
正常播完上报的数字一样，那这个字段对历史截断没有任何用处。

同时在播放结束时打一行日志，便于串口侧核对：

```
playback position: written=27200ms played=12480ms dropped=14720ms
```

## 心跳新增字段

`app_main.cpp` 的 state 构造里加了两个数字字段：

```json
{"spk_buffer_ms": 0, "spk_played_ms": 0}
```

空闲时都是 0；播放中 `spk_played_ms` 单调增长。

`spk_buffer_ms` 的量级由 DMA 环深度决定，`audio/audio_codec.h`：

```c
#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240
```

6 × 240 = 1440 帧 @ 16 kHz = **90 ms 上限**。所以播放中这个字段应该在 0–90ms 之间，
**不是几百毫秒**；读到远大于 90 的值说明估计逻辑有问题（比如时钟起点选错）。

## 顺带查出来的一个真实缺陷：每段话的结尾被吃掉 90ms

量 DMA 环深度的时候发现的。`AudioCodec_EnableOutput(codec_, false)` 走到
`esp_codec_dev_close()` → `i2s_channel_disable()`，而 **`i2s_channel_disable()` 是
直接丢弃环里剩余内容，不是排空**。

三条播放路径都是"最后一次写入返回后立刻关输出"，所以**每一段话的最后 90ms 都被
丢掉了**——16kHz 下大约是最后一个汉字。安静时不明显（末尾往往是尾音），但它一直
在发生。

修法是关输出前等一个环的时间，且**只在正常播完时等**——被打断时我们本来就要丢：

```cpp
if (!stop_requested_) drain_output();   // 90ms + 20ms 余量
AudioCodec_EnableOutput(codec_, false);
```

这同时让 T3.3 上报的数字诚实了：正常播完时确实排空了，`played == written` 才成立；
在加这个修复之前，"正常播完 = 全部听到"这个假设本身是错的。

**这是"单一指标看起来正常不等于系统正常"的又一例**：播放返回 `done`、时长也对得上
（90ms 在 27 秒里看不出来），单看播放结果永远发现不了。是为了给 `spk_buffer_ms`
定一个合理区间才去翻 DMA 配置，才撞上的。

## 本地构建的一个坑（不是本次改动引入的）

本机 `~/esp/esp-idf` 是 **v5.3.3**，而 CI（`release-ota.yml`）用的是
`espressif/idf:v5.5.4`。`device_hub_client.cpp:143` 用的
`esp_http_client_config_t::addr_type` 是较新 IDF 才有的成员，所以**本机 `idf.py build`
必然在这个文件上失败**，和改了什么无关。

本次改动的两个文件（`audio_player.cpp` / `app_main.cpp`）在本机是编译通过的，
链接失败发生在它们之后。**发布仍然只走 CI**，不要试图靠升级本机 IDF 去"修"这个
报错，也不要因为本机构建红了就以为改动有问题。

## 硬件实测（v47,2026-09-14 08:53）

固件 `esp32-wangyutang-v47-drain-tail`,OTA job `ota-fb72ce4692ce` 状态 `verified`。

### 正常播完:`play_lan_audio` 播 turn10(37.92s,vol60)

```
08:53:29 playing=True  buf=16  played=2186
08:53:34 playing=True  buf=46  played=7376
08:53:40 playing=True  buf=74  played=12568
08:53:46 playing=True  buf=0   played=17682
08:53:52 playing=True  buf=75  played=23007
08:53:57 playing=True  buf=0   played=28122
08:54:02 playing=True  buf=0   played=33342
08:54:07 playing=False buf=0   played=37920   ← 冻结
```

三条都成立:

1. **`buf` 落在 0–75ms,从未超过 90ms 的理论上限**。这是独立于实现的校验——上限由
   DMA 环深度决定,如果估计逻辑的时钟起点选错了,这个数会跑飞。
2. **`played` 单调增长,每 ~5.2s 采样增加 ~5.1–5.3s**,与真实时间同步。
3. **结束时正好 37920ms,与 turn10 的 37.92s 逐毫秒吻合**。这一条最有说服力:它不是
   "看起来合理",是一个有独立正确答案的数,对上了。

### 被打断:播到 ~10.6s 时发 `stop_audio`

```
播放中     playing=True   played=5076
stop 之后  playing=False  played=10632   ← 冻结在打断点,不是 37920
```

**这就是 T3.3 的核心行为**:被打断的一段回报"听到了多少",不是"发出去了多少"。
如果冻结成 37920,接 LLM 后模型就会以为自己把 37.92 秒的内容全说完了。

## 没能验证的部分(说清楚,不要当成已验收)

1. **`dropped` 的具体数值没看到**。它只打在 UART(`playback position: written=...
   played=... dropped=...`),而设备上报到云端的日志缓冲里只有命令回执,没有 ESP_LOGI。
   远程能证明的是"played 冻结在打断点",不是"丢掉的正好是 ≤90ms 的环内容"。
2. **90ms 排空修复本身没有在硬件上测到**。想了一下,这次的两个实验都区分不了它:
   正常播完时 `played == written` 无论有没有排空都成立;而 90ms 的时长差在心跳
   采样粒度(5s)下根本看不见。这个修复目前**只有代码层面的依据**
   (`i2s_channel_disable()` 丢弃而非排空),没有实测。

想把这两条补上,最小的办法是心跳再加一个 `spk_written_ms`——那样
`dropped = written − played` 就可以远程直接读出来。没有现在就做,因为它纯属验证用
途(真正消费 T3.3 的历史截断只需要 `played`),留作决策点。

## 现状

- 代码：`ee759bb`，tag `ota-esp32-wangyutang-v47-drain-tail`，设备已在跑 v47。
  （v46 只含 T3.3，CI 也构建成功了，但没有下发——直接合成 v47 一次 OTA。）
- **T3.3 验收通过**（上面三条实测）；90ms 排空修复**未实测**，只有代码依据。
- **P3 三项（T3.1/T3.2/T3.3）全部实现完毕**，ESP32 侧不依赖树莓派的工作就全部做完了。
  剩下 E2b/E3/E4 纯粹等硬件。

## 留给下一个会话的决策点

`spk_played_ms` 目前是**毫秒**粒度的音频位置，要真正用于对话历史截断，还需要把
"播了多少毫秒"映射回"念到第几个字"。任务书里已经点到：**选 TTS 模型时留意是否
支持字级或词级时间戳**。这件事属于 P4（接 Spark 流式 TTS），现在没有动——当前
播的是预录 WAV，没有文本对齐信息可用。
