# `aec_nlp_level` AGGR vs NORMAL 对照

**结论：两档抑制量逐档相差不到 1 dB，NORMAL 没有可测收益，默认保持 AGGR。**
真正压住近端的是下游的 WebRTC NS，不是 AEC 的 NLP —— 详见文末。

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

## NORMAL 侧（同日稍后，重新编译烧录）

先确认参数真的生效了 —— `afe_config_check()` 会静默改写冲突字段，所以不能只看源码。
读设备启动串口，`afe_config_print()` 的输出：

```
aec_init: true
aec mode: VOIP_HIGH_PERF
aec_nlp_level: NORMAL          <- 改上了
aec_filter_length: 4
ns_init: true
AFE Pipeline: [input] -> |AEC(VOIP_HIGH_PERF, NLP_ON)| -> |NS(WebRTC)| -> |VAD(WebRTC)| -> [output]
```

同样三档、同一段 TTS 文本：

| vol | 时长 | `mic_raw` RMS | `clean_aec` RMS | 抑制 | AEC 后 ASR |
|---|---|---|---|---|---|
| 25 | 12 s | 130.0 | 22.2 | 15.3 dB | 曾经的伤心，如今想起始终刺痛。曾经的你，曾经的你。 |
| 40 | 12 s | 255.6 | 15.4 | 24.4 dB | **(空)** |
| 60 | 15 s | 655.4 | 12.7 | 34.3 dB | **(空)** |

## 结论：NORMAL 没有可测的收益，保持 AGGR

**抑制量逐档几乎完全一致，三档全部在 1 dB 以内：**

| vol | AGGR 抑制 | NORMAL 抑制 | 差 |
|---|---|---|---|
| 25 | 16.3 dB | 15.3 dB | 1.0 dB |
| 40 | 23.7 dB | 24.4 dB | 0.7 dB |
| 60 | 34.4 dB | 34.3 dB | 0.1 dB |

这不是"没改上"—— 串口已经确认 `aec_nlp_level: NORMAL` 生效了。就是说**在这条信号
路径上，NLP 档位对回声抑制量没有可测影响**。原来的假设（AGGR 压得太狠、退一档能保住
近端）不成立。

原因大概率是：真正决定 `clean_aec` 电平的不是 AEC 的 NLP，而是它下游的 **WebRTC NS**。
`FINDING_ns_vs_asr.md` 已经单独证过 NS 会把持续稳定的声音越学越当背景压掉；NS 在这里
是共同的下游，NLP 档位怎么调都要过它这一关，所以差异被抹平了。这也把优先级指向了
队列里的下一项 —— **NS 的 A/B 才是有希望的那条路**。

近端 ASR 恢复率 AGGR 3/3、NORMAL 1/3，但两组的输入条件有漂移（v60 那档 NORMAL 的
`mic_raw` 是 655.4，AGGR 是 378.5，回声强了 4.8 dB），所以这个差距**不能**归因于参数，
只能说 NORMAL 没有表现出任何优势。

**安全性上两档都合格**：六次捕获里 `clean_aec` 的 ASR **从没出现过机器人自己的句子**，
没有触发"把设备自己说的话当成用户输入"这个失败模式。

据此把 `AEC_NLP_LEVEL` 的默认值保持在 `AEC_NLP_LEVEL_AGGR`，开关留着备用。
`VERYAGGR` 没测 —— 既然 NORMAL→AGGR 之间毫无差别，再往更激进的一档走没有理由。

## 音频

`~/workspace/data/aec/`：`AGGR_v25_*` / `AGGR_v40_*` / `AGGR_v60_15s_*`、
`NORM_v25_*` / `NORM_v40_*` / `NORM_v60_15s_*`，每组三通道。
