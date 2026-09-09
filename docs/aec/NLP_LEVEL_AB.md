# `aec_nlp_level` AGGR vs NORMAL 对照

**状态：基线已测，NORMAL 侧待编译（WSL 不可达，见文末）。**

## 背景

`TTS_DOUBLETALK.md` 里暴露过一个边界：vol=60 连续播报 15 s 时，回声消掉了（24 dB）
但弱近端也被一起吃掉，`clean_aec` 的 ASR 返回空。怀疑是 AEC 的非线性处理（NLP）
压得太狠 —— 它的默认档位是最激进的一档。

先厘清一件事：**`aec_nlp_level` 此前从来没有被显式设置过**，一直用的是
`afe_config_init(..., AFE_TYPE_VC, ...)` 的默认值。查 esp-sr 2.5.3 的
`esp_aec_nlp.h`，取值只有三档：

```c
AEC_NLP_LEVEL_NORMAL   = 0,  // Normal level of NLP, suitable for most scenarios.
AEC_NLP_LEVEL_AGGR     = 1,  // Default: Aggressive level of NLP, stronger echo suppression.
AEC_NLP_LEVEL_VERYAGGR = 2,  // Very aggressive level, strongest echo suppression.
```

所以默认档 = `AGGR`，之前所有结果都是在 AGGR 下测的；文档里写的 "MODERATE"
其实对应 `AEC_NLP_LEVEL_NORMAL`。现在把它做成 `aec_config.h` 里的编译期开关
`AEC_NLP_LEVEL`（默认仍为 AGGR），并在 `afe_pipeline.c` 里显式赋值，这样
「某个 build 是在哪一档测的」直接从源码可见，而不是靠记默认值。

## AGGR 同期基线（2026-09-10，本轮重测）

条件：远端 = 人声 TTS 播报循环填满窗口，近端 = 房间里循环播放的歌词，
判据 = 云端 `qwen3-asr-1.7b` 转写。

| vol | 时长 | `mic_raw` RMS | `clean_aec` RMS | 抑制 | AEC 后 ASR |
|---|---|---|---|---|---|
| 25 | 12 s | 88.1 | 13.5 | 16.3 dB | 记住最初的梦。 |
| 40 | 12 s | 312.2 | 20.5 | 23.7 dB | 我总是觉得，我的情绪是种本能，不要抗拒。 |
| 60 | 15 s | 378.5 | 7.2 | **34.4 dB** | 心事，依然。人海里，千千万万，不相见。 |

`mic_raw` 三档全部只认出机器人自己那句话（循环 2–3 遍），近端歌词一个字都没有 ——
和之前一致。

## 关键发现：之前那个"边界"不可复现

一小时前同样是 AGGR、同样 vol=60 / 15 s，`clean_aec` 的 ASR 是**空**；本轮
**恢复出完整句子**「心事，依然。人海里，千千万万，不相见。」，而且是在 RMS 只有
7.2（−73.2 dBFS）的电平上转写出来的。

差别在输入条件，不在算法：

| | 当时 | 本轮 |
|---|---|---|
| `mic_raw` RMS | 575.6 | 378.5 |
| `reference` RMS | 520.8 | 402.5 |

本轮回声弱了约 4 dB，近端相对就强了，AGGR 就没把它压没。

**结论：`TTS_DOUBLETALK.md` 里"vol=60 连续 15 s 会丢近端"这条不是 AGGR 的固有
属性，而是当时回声/近端比偏差的结果。** 决定门槛的是**近端与回声的相对电平**，
不是播报的绝对音量或时长。原文里"操作区间：近端与回声差距 ~25–30 dB 以内"这个
表述方向是对的，但不该把它挂在某个音量档位上。

这也说明一件方法上的事：**跨时段的档位对比必须同期重测基线**，否则会把房间条件
的漂移误读成参数效果。本轮先测基线再改参数，就是为了这个。

## 待做

NORMAL 侧的对照没跑成 —— 改完代码准备编译时 WSL 掉线（`ssh wsl` 连续 5 次
`Connection timed out`，是 `docs/logs/2026-09-04-wsl-connection-drop.md` 记过的老问题）。
固件编译烧录只能在 WSL 上做，所以 NORMAL 这一半要等它回来。

代码改动（`AEC_NLP_LEVEL` 开关 + 显式赋值）已经落在 WSL 的工作区里，还没提交。
WSL 恢复后：把开关翻到 `AEC_NLP_LEVEL_NORMAL`、编译烧录、用**完全相同的**
三档（25/12s、40/12s、60/15s）和同一段 TTS 文本重跑，逐档对比上表。

看点不是"NORMAL 的抑制量是否更低"（一定更低），而是：

1. 在回声强的那一档（vol=60），NORMAL 的 `clean_aec` 里近端是否更完整 —— 转写
   的字数、有没有丢结尾。
2. 残留回声会不会强到让 ASR 把机器人自己的话也认出来 —— 那就是 NORMAL 过头了，
   上行会把设备自己说的话当成用户输入，这是全双工里最不能接受的失败模式。

第 2 点是真正的判据：**只要 `clean_aec` 的 ASR 里出现机器人自己的句子，这一档就
不可用**，无论近端保真度提升了多少。
