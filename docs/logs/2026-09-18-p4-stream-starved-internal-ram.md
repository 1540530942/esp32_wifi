# 2026-09-18 · 用户不在家时自主排查：P4 流式连接把内部 RAM 榨干到 1152 字节

## 起点：反向隧道断了，不是主机断了

上次汇报"WSL 主机联系不上"是不完整的判断。`~/.ssh/config` 里 `wsl` 走的是
反向隧道 `127.0.0.1:10022`,而 `wsl-tailnet`(同一台机器的 Tailscale 直连
100.88.133.9)完全正常。**只测了一条路径就下结论,是这次自查的第一个教训。**

## 串口现场：不是崩溃，是静默挂起

挂回串口后先监听 25 秒——**一行输出都没有**,和之前"每 6 秒一轮崩溃重启"的
签名完全不同。用 DTR/RTS 主动触发一次硬复位后才看到启动日志。

**必须说清楚**:这次复位是我主动触发的,`reset_reason=1 (POWERON)` 是这个动作
本身的产物,不能当作"设备之前断过电"的证据——ESP32-S3 上 EN 脚复位常常也报
POWERON,这是我这次操作造成的假象,不是独立证据。

## 复位后：I2C 故障依旧，但这次完整跑通了开机流程

ES8311/ES7210 仍然全部无应答,和之前一致——硬件没有自愈。但 v101 的修复生效:
codec 打不开不再 abort,WiFi 连上、注册成功、MQTT 也连上了。

## 新发现：心跳能到但从不走 HTTPS，语音流从未稳定过

90 秒完整日志：

```
W POST /heartbeat short write -1/590        ← HTTPS 每次都失败
W public HTTPS unavailable; retrying http://...
I raw POST /heartbeat HTTP 200               ← 靠明文回落才成功
```

同时 `/ws/audio`(P4 流式 ASR/TTS 通道)持续 `connected → error → disconnected
→ 等 2000ms → 重连`。查服务端(`audio-interact`)日志印证：连接每 20~50 秒
重新 accept 一次,从未稳定。

## 决定性证据：内部 RAM 被挤到 1152 字节

连续 6 次采样,跨 3.5 分钟：

```
free_internal = 2515~2539 字节（几乎不变）
largest_internal = 1152 字节（完全不变）
```

**这不是缓慢泄漏,是长期卡在一个危险的低水位。** 1152 字节这么小,
解释了两件事：
1. **HTTPS 心跳为什么每次都失败**——mbedTLS 握手需要的连续内部内存块
   拿不到,只能靠明文回落
2. **很可能是那次 5 小时静默挂起的根因**——某个时刻某处代码需要比
   1152 字节更大的内部分配而拿不到,如果没有妥善处理，就会不崩溃、
   不重启、不打印任何东西、永久挂起，直到我用 DTR/RTS 强制复位

服务端代码(`/root/wangyutang_platform/audio_interact/server.py`)里
`/ws/audio` 的 `device_id` 写死成 `"turbopi-01"`——这是给另一台设备用的
调试端点，而且是共享生产代码，不在这次任务范围内，没有碰。

## 我在职责范围内做的修复

任务清单原文：P4（流式 ASR/TTS）明确是**"本阶段之后"**。而 `ws_client_start()`
现在无条件在开机时启动，P0–P3 的 AEC/打断验证完全不需要它——回声、打断、
所有测试都在设备本地闭环，不出局域网。

新增 `CONFIG_AEC_STREAM_TO_CLOUD`（默认 **关闭**），把这条连接的启动从
`app_main.cpp` 里摘掉，等真正进入 P4 时再打开——并且到时候要为它单独预算
内部 RAM，而不是让它和 AFE/MQTT/mbedTLS 抢同一小块池子。

`ws_client_send_audio` / `ws_client_report_tts_state` 在客户端从未启动时
本来就会安全地判空跳过（`if (!s_client) return`），不需要额外改动。

## 待验证

- v104 部署后，`free_internal`/`largest_internal` 应显著回升
- HTTPS 心跳应能直接成功，不再每次回落明文
- 观察是否还会出现无征兆的长时间静默——如果内部 RAM 假说成立，
  这类挂起应该消失
