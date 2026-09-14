# 2026-09-14 · E4 打断延迟：把缺的那一半时间戳补上

## 背景：状态文档里的一句话是错的

`2026-09-14-aec-bargein-status.md` 之前写：

> E4 还缺的东西：素材开头的 20ms 咔哒声还没做。**ESP32 侧的时间戳埋点已就位**——
> `afe_pipeline.c` 在打断触发时打印 `local barge-in -> duck+stop at t=<微秒>`。

这句话把"一半"说成了"已就位"。E4 要测的是两个时间戳之差：

```
延迟 = t_duck（停播） − t_click（人开口）
```

**`t_duck` 有埋点，`t_click` 完全没有实现。** 只有停播时刻、没有开口时刻，这个实验
根本跑不起来——不是"等树莓派回来就能跑"。这次把缺的一半补上了，而且这部分工作
**完全不需要树莓派**。

## t_click 为什么必须在 AFE 之前测

放在 AFE 输出上测会把 AEC/NS/VAD 的处理延迟从测量值里**减掉**——而那正是这个实验
要量的东西。所以检测放在 `feed_task` 里，直接扫 `board_mic_read()` 拿到的**原始麦克风
通道**（ch0），在喂给 AFE 之前。

## 阈值为什么必须是相对的

E4 进行时 ESP32 自己正在播放，原始麦克风里本来就有机器人的回声。固定绝对阈值两头
不讨好：定低了在高音量下会被回声触发，定高了在低音量下漏掉咔哒。

所以用**相对于慢速包络的倍数**：

```c
#define CLICK_ENV_ALPHA      0.05f   // ~20 帧（~320ms）时间常数
#define CLICK_TRIGGER_RATIO  4.0f    // 高出running包络 +12dB
#define CLICK_MIN_PEAK       2000    // 绝对下限，防止安静时自激
#define CLICK_SEED_FRAMES    16      // 包络收敛前不判定
```

包络跟着回声电平走，咔哒要高出它 12dB 才算数。`CLICK_SEED_FRAMES` 是防止刚 arm
时包络还是 0、任何声音都满足"4 倍"的问题。

## 帧内定位：不要被 16ms 量化

`s_feed_chunksize` 在 16kHz 下是 256 样本 ≈ 16ms。如果只记"哪一帧检测到"，测量值
就会有 16ms 的量化误差——占 200ms 预算的 8%，白白吃掉一格精度。

所以扫描帧内第一个越过阈值的样本下标，再换算回时间：

```c
s_click_us = frame_end_us - (int64_t)(chunk - first) * 1000000 / 16000;
```

`frame_end_us` 是整帧读完的时刻，所以第 `first` 个样本发生在它之前
`(chunk - first)` 个样本的时间。

## 一次性检测，不是持续检测

第一个合格的上升沿记下时间戳就 **disarm**。否则后面的语音会不断覆盖 `t_click`，
最后量到的是"最后一次大声"到停播的间隔，而不是"开口"到停播。

## 结果必须能远程读到

这次特意加了 `click_result` 命令，而不是只打 UART 日志：

```
click_arm     -> done|armed
click_result  -> done|latency=<N> ms click=<us> duck=<us>
```

理由是刚在 T3.3 上吃过一次亏（见 `t33-position` 末尾）：`dropped=` 只打在串口，
设备上报云端的日志缓冲里只有命令回执，结果那个数**远程根本验证不了**。E4 的延迟是
这次工作的最终验收指标，绝不能是一个只有插 USB 才能看到的数。

## 咔哒素材

`tools/aec_bench/make_click_fixtures.py`，已在 spark 上生成：

```
turn01_user_click.wav: 4.32s -> 4.34s
turn03_user_click.wav: 93.12s -> 93.14s
turn05_user_click.wav: 4.08s -> 4.10s
turn07_user_click.wav: 3.92s -> 3.94s
turn09_user_click.wav: 3.36s -> 3.38s
```

位置：`spark:~/workspace/data/aec/dialogue_wenyanwen/`（24kHz 单声道，树莓派 aplay 直读）。

实测 turn05 开头：**咔哒峰值 24588（约 -2.5 dBFS），紧随其后是静音**——咔哒确实是
开头最响的东西，检测器不会被原素材的起始音盖过。

两个设计细节：

1. **用宽带噪声突发而不是纯音**。纯音可能正好落在房间响应的零点里；而且咔哒要在
   混有回声的原始麦克风信号里被认出来。
2. **20ms 内衰减到 0**（`(1 - i/n)²`）。平顶突发在末尾会产生第二个瞬变，检测器同样
   会对它感兴趣，记录的起点就会有一个咔哒宽度的歧义。

## 咔哒本身不会触发打断（这是有意的）

打断需要 `CONFIG_AEC_BARGEIN_SPEECH_FRAMES`(4) 帧持续语音 ≈ 64ms，而咔哒只有 20ms。
所以**停播仍然是被咔哒之后的语音触发的**，量到的是真正的"开口→静音"，不是一个
咔哒回路的往返时间。

## 状态

- 固件：`click_arm` / `click_result` 两条命令 + `feed_task` 里的检测器，本地编译通过。
- 素材：5 个 `_click` 版本已在 spark 上生成并抽查过电平。
- **仍然阻塞在树莓派**：E4 需要它扮演开口的人。ESP32 侧现在真的准备好了。

## E4 恢复后的执行步骤

```bash
# 1. 把 click 素材推到树莓派（它掉线期间没同步过）
scp ~/workspace/data/aec/dialogue_wenyanwen/turn05_user_click.wav pi:<素材目录>/

# 2. ESP32 开始播一段长素材
curl -X POST 'https://www.wangyutang.cn/devices/api/device/esp32-s3-walle/play_lan_audio' \
  -H 'Content-Type: application/json' \
  -d '{"params":{"name":"turn04_assistant.wav"},"settings_override":{"voice_volume_percent":80}}'

# 3. 等过了 onset grace（500ms）再 arm，然后让树莓派播 click 素材
curl -X POST '.../command' -d '{"action":"click_arm"}'
#    树莓派 play_local_audio turn05_user_click.wav

# 4. 读结果
curl -X POST '.../command' -d '{"action":"click_result"}'
# 期望 latency < 200ms（好 < 120ms）
```

**注意 arm 的时机**：要在 ESP32 已经开始播、且过了 500ms onset grace 之后再 arm，
否则包络是在"没有回声"的状态下收敛的，等回声起来时会误判。
