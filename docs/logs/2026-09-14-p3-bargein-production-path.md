# 2026-09-14 · P3：打断从来没有接进生产播放路径

## 怎么发现的

本来是要跑 E5（误打断率：ESP32 连播 5 个 assistant 轮次、树莓派全程静音、统计被
自己残余回声误触发停播的次数）。跑之前核对了一下"停播"到底由谁触发，结果发现这个
实验在当前代码下**不可能失败**，因为检测器压根不会启动。

`afe_pipeline.c` 的本地打断判定是这样门控的：

```c
bool playing = playback_is_playing();
if (playing && past_grace && res->vad_state == VAD_SPEECH) { ... playback_duck(); }
```

而 `playback_is_playing()` 只反映 `playback.c` 自己那个 TTS 分句队列的状态：

```c
bool playback_is_playing(void) {
    return s_busy || s_external_active || uxQueueMessagesWaiting(s_queue) > 0;
}
```

`s_external_active` 由 `playback_set_external_active()` 设置，全代码库只有一个调用
方——`far_end` **测试夹具**（`app_main.cpp:485`）。

**机器人真正说话走的是 `AudioPlayer`**（`play_audio` / `play_lan_audio` /
`speak_pcm` 都是它），它直接往 codec 写数据，从不注册。于是：

> 机器人通过生产路径说话时，`playback_is_playing()` 是 false，打断检测器一次都
> 不会评估。**用户根本打不断它。**

更讽刺的是，`playback.h` 里那段注释把这个道理写得很清楚——只是写给 far_end 夹具的：

> "Barge-in is gated on playback_is_playing(), so without this the detector never
> even evaluates while the fixture is playing, and no interruption can be measured."

同样的道理适用于生产播放器，但没人给它接上。

## 第二个缺口：就算检测器跑起来了，也停不下来

检测器触发后调的是 `playback_duck()`，而 duck 只改 `playback.c` 自己的输出增益
（`s = s * s_gain_pct / 100`）。`AudioPlayer` 有自己的写循环，**没有任何增益钩子**，
duck 对它完全无效。

所以即使补上第一个缺口，打断也只会让一个空队列变安静，机器人照样说完。

## 修法

1. **`AudioPlayer::play_task()` 在播放期间注册为 external-active**，检测器因此会
   评估。顺带一个好处：`playback_set_external_active(true)` 会重置 turn 计时，
   起播宽限（onset grace）从每次发声开头重新计算，避免 AEC 滤波器尚未重新收敛时
   自己触发自己。
2. **新增外部停止回调** `playback_set_external_stop_cb()`，`app_main` 注册成
   `AudioPlayer::stop()`。`playback_kill()` 和新的 `playback_barge_in()` 都会调它。
3. 检测器改调 `playback_barge_in()`——一次动作同时覆盖两条播放路径：duck 自己的
   队列 + 停掉外部播放器。

### 一个无法回避的不对称

外部播放器**只能停、不能 duck**（没有增益钩子）。所以：

- 误触发打到 TTS 队列 → 音量压低一秒再恢复，代价小
- 误触发打到 `play_audio` → **片段被直接切断，无法恢复**

这让起播宽限和"连续语音帧数"两个参数在生产路径上变成承重件。任务书说误打断要调
VAD 阈值和最短语音时长、不要动 AEC，正是针对这个。

**E1 顺带给了宽限参数一个实测依据**：滤波器收敛时间实测 0.4–0.5 秒，所以现有的
`CONFIG_AEC_BARGEIN_ONSET_GRACE_MS = 500` 大致是对的——而不是早前文档推测的
"收敛需数秒、应提到 3000ms"。那个推测来自 L1 实验的间接观察，这次是直接测的。

## 顺带为 E4 埋点

检测器触发时现在会打印 `esp_timer_get_time()`：

```
local barge-in -> duck+stop at t=<微秒>
```

E4 要测"人开口 → 停播"的延迟，两个时间戳都取自 ESP32 自己的时钟，不需要跨设备
对时。这条日志就是其中的"停播"时刻。

## 状态

代码已提交，发布为 `ota-esp32-wangyutang-v40-bargein-production`。

**尚未验证**：这套改动能不能真的打断，以及误触发率是多少。两者都要等树莓派恢复
（E3/E4 需要它扮演人说话；E5 虽然只要求它静音，但要先确认检测器在生产路径上确实
会评估，否则 0 次误触发依然是假通过）。

## 阻塞：树莓派掉线

下午 07:10 之后树莓派失联，两条独立路径都不通：

- 反向 SSH 隧道 `pi-reverse`（127.0.0.1:10024）→ Connection refused
- Tailscale `pi-tailnet`（100.118.92.117:22）→ Connection timed out
- `action_move` 服务端也报 `robot edge device is offline`

三处证据一致，是设备本身不可达，不是某一条访问路径的问题。远程无法恢复，需要现场
处理。掉线前最后的活动是 `codex-loop-camera-check` 在 07:09–07:10 驱动机器人做
前后左右移动。

### 期间还踩到并修复的一件事

树莓派掉线之前，`action-move` 服务在 07:07 被重新部署，线上技能从 25 个掉到 23 个
——`play_audio` 和 `play_local_audio` **双双消失**，API 开始回
`unknown action: play_local_audio`。根因是这两个技能的服务端注册和校验代码一直
是"已部署但未提交"状态（早先已在 device_hub 那次分叉里记录过同类问题），一次常规
重新部署就把它抹掉了。

已把那份工作提交进 `wangyutang` 仓库（在 commit message 里注明了不是我写的、是抢救
既有工作），并重新部署，线上技能恢复到 25 个。这样下次再部署不会重演。
