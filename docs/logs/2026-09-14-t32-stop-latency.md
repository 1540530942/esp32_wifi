# 2026-09-14 · T3.2 复查：停播不是"立即"，而是最多等一个 256ms 的写入周期

## 起因

树莓派仍不通，按上一轮说的继续逐条回读任务书。T3.2 写的是：

> T3.2 立即停播：STOP_PLAYBACK 必须立即生效，**不能等缓冲播完**，清空喇叭缓冲。

这一项一直被当成"已实现"，但**从来没有被测量过**。

## 读代码的怀疑

`play_wav_stream_raw_http()`（`play_lan_audio` 走的生产路径）：

```cpp
constexpr size_t kWavBufferBytes = 8192;     // 4096 样本 = 256ms 音频

while (!stop_requested_ && ... read_some(..., 8192) > 0) {
    AudioCodec_OutputData(codec_, buf, 4096);   // 阻塞到 4096 样本全部入队
}
```

`AudioCodec_OutputData` → `esp_codec_dev_write` → `i2s_channel_write(portMAX_DELAY)`，
**阻塞到全部数据交给 DMA 为止**。而 DMA 环只有 6×240=1440 帧 = 90ms，所以一次调用要
等约 166ms 以上才返回。

也就是说 **`stop_requested_` 每 ~256ms 才被检查一次**，这期间喇叭一直在响。

## 为什么 E4 抓不到

E4 按任务书的定义记两个时间戳：咔哒 → **"VAD 触发停播时"**。后者是
`playback_barge_in()` **被调用**的时刻，**不是声音停止的时刻**。

所以这个缺陷对 E4 完全隐形：延迟可以漂亮地落在 200ms 以内，而机器人还要再响四分之
一秒。**指标通过，打断实际失败。**

这和同一轮查出的 T3.1 是同一个模式：**任务书里写了验收标准，但那条标准量的不是它
想约束的东西。**

## 先量再修

v54 只加埋点不改行为——记录 `stop()` 到输出真正拆除之间的时长，上报到心跳
`stop_latency_ms`。这一步不能省：这一轮前面 v48/v49 两次"看起来对的修复其实无效"
已经说明，光靠读代码推理不足以下结论。

实测（播 turn10 @vol50，播到第 7s 发 `stop_audio`）：

```
trial 1: stop_latency_ms = -1     ← 心跳快照早于停播完成，无效样本
trial 2: stop_latency_ms = 143
trial 3: stop_latency_ms = 159
```

**143ms / 159ms**，落在预测的 150–260ms 区间。问题属实。

这 150ms 是**叠加在**打断检测延迟之上的：4 帧检测（64ms）+ AFE 处理 + 150ms 停播，
真实的"开口→静音"轻松超过 200ms 及格线。**这很可能就是 E4 本来会不及格的原因**，
而 E4 自己量不出来。

## 修法

新增 `write_interruptible()`，把写入切成 DMA 描述符粒度（240 样本 ≈ 15ms）的小块，
块间检查停播标志：

```cpp
constexpr int kChunk = AUDIO_CODEC_DMA_FRAME_NUM;   // 240
while (done < samples) {
    if (stop_requested_) break;
    const int n = std::min(kChunk, samples - done);
    const int w = AudioCodec_OutputData(codec_, data + done, n);
    ...
}
```

选 240 而不是更小：**写入本来就要阻塞到一个描述符腾出来**，所以按描述符切是不额外
付出代价的最小粒度。再切小只会增加调用次数，不会更快返回。

三条播放路径全部改走这个 helper。顺带把 `note_samples_written()` 收进 helper 内部，
避免以后新增路径时漏掉 T3.3 的计数——之前是三处各调一次，是个明显的遗漏面。

短写日志加了 `&& !stop_requested_` 条件：被打断时的短写是**预期行为**，不该打 warning。

## 状态

- v55 待发。**验收方式**：同样三次测量，`stop_latency_ms` 应从 ~150ms 降到 ~20ms
  量级（一个描述符 15ms + 拆除开销）。
- 这一项的验证**不需要树莓派**——`stop_audio` 命令就能触发，和真实打断走同一条
  `AudioPlayer::stop()` 路径。
