# 2026-09-15 · E5 的四次"失败"不是打断，是 MQTT retained 命令在重放

## 结论先说

E5 在完整 127 秒规模下连续四版不通过（v68 2/5、v69 5/5、v70 4/5、v71 5/5）。
我据此调了四个版本的 VAD 参数——`vad_min_speech_ms` 128→64→48、onset grace
900→1500、`vad_mute_playback` 开关。**全部是白做的。**

真正的原因是：**命令以 MQTT retained 方式发布，清除失败后会在每次设备重连时重放**，
而 `e4_latency.py` 每轮结束都发一条 `stop_audio`。积压的 stop 被反复重放，
把 E5 的每一轮播放都杀掉了。

排除干扰后重测：**5 轮里 4 轮完整播完**，剩下一轮短 397ms（37.92 秒的 1%，
在心跳采样粒度量级）。

## 怎么找到的

### 第一层：读回执，而不是看现象

切断点看起来像打断（1.4–6.9 秒，都在 onset grace 之后）。但设备的命令回执里写着
完全不同的故事：

```
status=failed|stage=wav_start     error=ESP_ERR_INVALID_STATE   ← 播放器忙，根本没播
status=failed|stage=wav_playback  error=ESP_ERR_INVALID_STATE   ← 播放中被 stop
text=今天是9月14日，星期一，固件版本e...                        ← 开机播报在抢播放器
```

`wav_start INVALID_STATE` 是 `play_wav_url` 在 `busy_` 已为真时的返回值——**命令被
拒绝，一个字节都没播**。打断不可能产生这个。

### 第二层：开机播报（解释了一部分）

`boot_announce` 用同一个 `AudioPlayer`。我的 E5 脚本紧跟 OTA 完成就启动，正好撞上
开机播报，两者互相 stop。时间戳集中在设备 uptime 39–44 秒。

加了"开机不足 150 秒就等"的防护后，失败从 5/5 降到 4/5——**有改善但不是全部**。

### 第三层：retained 重放（真正的根因）

再读一次回执，发现 **12 条 `stop_audio` 在极短时间内集中送达并执行**，而脚本只发
过 1 条。服务端 pending 队列却是 0。

`cloud/device_hub/server.py`：

```python
info = client.publish(topic, json.dumps(command), qos=1, retain=True)   # :91
```

命令是 **retained** 的，设备接受后由 `_mqtt_clear_retained_command` 清除。
**一旦清除失败或错过，命令就留在 broker 上，设备每次重连都重新执行一遍。**
`server.py:966` 的注释自己就写着「是 retained 的，下次启动还会重放」。

这台设备因为 WS 抖动和频繁 OTA，重连很密——于是 `e4_latency.py` 早先发的每一条
stop 都被反复重放。

等到 stop 回执计数连续 3 次采样不再增长才开测，E5 立刻从 4/5 失败变成 4/5 通过。

## 代价

**四个固件版本（v68–v71）的 VAD 调参完全是在追一个不存在的问题。** 期间做出的两个
改动需要重新评估：

- onset grace 900 → 1500ms：当时的依据（v68 的 2/5 被切）已被证明是污染数据
- `vad_min_speech_ms` 48 → 64：同样基于污染数据

这两项现在都缺乏有效依据，应在干净环境下重新测定。

## 教训

1. **现象相似不等于原因相同。** 播放提前结束看起来就是打断，而 `played=0`（一个字节
   没播）本该立刻排除打断——打断必须先让一些音频出去才能停它。这条线索在日志里
   躺了好几轮我才看见。

2. **先读设备自己的回执，再去调参数。** 回执里 `stage=wav_start` 和
   `stage=wav_playback` 的区别，直接区分了"没播成"和"播了被停"，而我在没有这个
   区分的情况下调了四版 VAD。

3. **我自己的测试工具是干扰源。** 先前为防机器人移动写了 `check_robot_still.py`，
   却被自己发的 stop 命令坑了。测量脚本必须把"自己造成的副作用"也算进去。

## 留下的防护

`tools/aec_bench/e5_full_run.py` 现在开测前做三件事：

1. 等设备 uptime ≥150 秒，避开开机播报
2. 轮询 stop 回执计数，连续 3 次不变才认为 retained 重放结束
3. 运行期间若有新的 stop 回执落地，整轮作废

## 待办

**retained 命令重放是生产问题，不只是测试干扰。** 一条误发的 stop 或 play 会在设备
每次重连时重新执行，用户侧表现为"机器人莫名其妙自己动/自己播/自己停"。
建议给 `_mqtt_clear_retained_command` 加重试与校验，或给命令加时效戳，
设备侧丢弃过期命令。这一项**尚未修复**。
