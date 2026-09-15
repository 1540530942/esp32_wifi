# 2026-09-15 · 部署 retain=False 到生产，以及我造成的一次 502

## 做了什么

1. 清除 broker 上 60 个 retained command topic（其中 14 条未完成，含 **3 条 ota
   和 1 条 reboot**）
2. 生产 `server.py` 第 91 行 `retain=True` → `False`，重建容器
3. 命令往返从**分钟级**降到 **12–18 秒**

第 111 行的 `retain=True` **保持不变**——清除一条 retained 消息本身就要向同一
topic 发一条空载荷的 retained 消息，改掉它清除功能就废了。

## 我造成了一次 502，原因是漏抄容器参数

新容器起来后公网返回 **502**，而容器内 `curl localhost:8102/api/health` 是 **200**。

定位过程：

```
直连容器      200        → 应用没问题
docker port   正常       → 端口绑定没问题
→ 问题在 Caddy 侧
```

```
旧容器网络: wangyutang_platform_default
新容器网络: bridge          ← 我建容器时没加 --network
Caddy 网络: infra_default, wangyutang_platform_default
```

**Caddy 和新容器不在同一个网络上，解析不到 `device-hub`。**
`docker network connect wangyutang_platform_default device-hub` 后立即恢复 200。

### 这是同一个教训的第二次

`2026-09-14-device-hub-production-deploy.md` 里记着：

> **教训**：`docker inspect` 抓运行参数时，`.Config.*` 和 `.HostConfig.*` 都要抓全

那次漏的是 `--add-host`，这次漏的是 `--network`。而且我这次确实跑了 `docker inspect`
去抓参数——**抓了 Binds、PortBindings、ExtraHosts、RestartPolicy、Image、Env，
唯独没抓 NetworkSettings.Networks**。

上一篇的教训写成了"要抓全"，而"全"是什么并没有列出来。这次补上清单：

| 必抓项 | 命令 |
|---|---|
| 挂载 | `.HostConfig.Binds` |
| 端口 | `.HostConfig.PortBindings` |
| 额外 hosts | `.HostConfig.ExtraHosts` |
| 重启策略 | `.HostConfig.RestartPolicy.Name` |
| 环境变量 | `.Config.Env` |
| **网络** | **`.NetworkSettings.Networks`** ← 两次事故里漏的都在这类 |
| 镜像 | `.Config.Image` |

更稳妥的做法是不要手工重建：把这台机器的手工 `docker run` 改成 compose 或一个
脚本，让参数集合本身成为可复现的产物，而不是每次靠人从 inspect 里挑。
**这一项没做，记为待办。**

## 影响范围

- 公网 502 持续约 3 分钟（从重建容器到 `network connect`）
- 期间设备心跳走的是 `http://110.40.154.41` 明文兜底，**设备未掉线**
- 旧容器保留为 `device-hub-old-1789475603`，回滚路径可用

## 验证

```
health                200 ×3
设备 online           True，fw v77
ref_peak_dbfs         -84.3（静默，符合预期）
命令往返              15s / 18s / 12s
```

命令往返仍有十几秒，其中包含约 8 秒的心跳周期和我 4 秒的轮询粒度，**实际下发更快**。
与修复前的分钟级相比是数量级的改善。

---

## 后续：磁盘再次写满，以及削顶门限终于验成

### 命令"未送达"其实是服务端写不进去

部署后 `set_volume` 和 `play_lan_audio` 连续失败，我一度以为是下发延迟没修好。
查容器日志才看到真相：

```
OSError: [Errno 28] No space left on device: '/app/data/devices.json.tmp'
POST /api/heartbeat HTTP/1.1  500 Internal Server Error
```

**磁盘满 → hub 无法持久化状态 → 心跳和命令全部 500。** 表现是"命令没到设备"，
成因却在服务端写盘。这和之前几次"命令未送达"的成因都不同——
retained 重放、下发延迟、现在是磁盘满。**同一个症状，三个不同的根因。**

### 磁盘是我自己填满的

`audio_interact` 卷从我上次清理后的 17GB 涨回 22GB：

| 日期 | sessions | recordings |
|---|---|---|
| 2026-09-14 | 2.4G | 1.2G |
| 2026-09-15 | 2.4G | 1.2G |

**这两天各 3.6GB，几乎全是我自己的测试产生的**——整天在跑 `asr_check.py`、
`aec_capture`、E1–E5，每次云端 ASR 调用都会在那里落一份会话录音。

删掉今天的 `recordings/2026-09-15`（1221MB）后服务立即恢复。选这个目录是因为它
**明确是我今天测试的副产品**，而真正有价值的采集（`aec_bench` 的原始三路 + 立体声）
是独立归档到 spark 的，不在这个卷里。

**这一项是待办**：测试会持续产生录音，而磁盘没有保留策略。要么给 audio-interact
加定期清理，要么测试脚本用完即删。现在只是又腾了一次，**根因没解决**。

### 削顶门限双向验证通过

磁盘恢复后立刻补验了之前被挡住的那一半：

| 音量 | `ref_peak_dbfs` | `ref_clip_frames` |
|---|---|---|
| 40 | -29 ~ -33 dBFS | **0（不触发）** |
| **100** | **-1.9 dBFS** | **0 → 5（触发）** |

**只验"不触发"区分不了"正确地不触发"和"探测器是死的"**——这次两边都有了。

顺带修正一个数字：vol100 下真实语音的回采峰值是 **-1.9 dBFS**，比粉红噪声测出的
-3.9 dBFS **更糟 2 dB**。这正是之前那个判断的实证：**真实语音峰值更高，用测试信号
测会低估风险**。vol100 下回采实际已在削顶边缘，告警是应该响的。

### 一个用错接口的小插曲

`set_volume` 第一次失败是我把参数名写成了 `args.volume`，而设备读的是 `args.value`
（`app_main.cpp:641`）。回执写着 `status=failed`，但我先入为主地归因成下发延迟，
多花了一轮才去读回执。**回执里就有答案，我又一次先猜后读。**
