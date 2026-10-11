# 2026-10-11 · 同步复核与工程状态确认

## 代码同步

`git fetch origin` 后，本地 `feature/aec-fullduplex` 与
`origin/feature/aec-fullduplex` 完全一致（HEAD 均为 `9e22c40`），
无领先也无落后，`git diff --stat` 双向均为空。工作区除几个本地测试产生的
临时 wav 文件（未跟踪，非仓库内容）外干净。

（本次会话早些时候发现并修复过一次真实差异：之前的 spark 中转推送流程
静默丢过两个文件——`docs/USB_RECOVERY.md` 的 WSL/usbipd 备用链路章节、
以及整个 `tools/aec_bench/ab_session.py`——已补回并推送，此后两边一致
延续至今。）

## 工程目标

ESP32-S3 上实现全双工实时打断：机器人用 TTS 说话过程中，用户开口后
200ms 内停止播报。核心链路：模拟麦 → ES7210 ADC → I2S0 四槽 TDM
（ch0 麦克风 / ch1 硬件回采）→ AFE 回声消除 + 本地 VAD → 立即停播。

## 实现进度（直接核对源码，非仅凭提交记录）

逐项在当前 HEAD 的源码里核实，均存在且与此前记录一致：

| 项目 | 核实方式 | 结果 |
|---|---|---|
| T0.1 REG43/REG44 实测诊断 | `app_main.cpp` 的 `es7210_regs` 命令 | 存在 |
| T0.5 AFE 类型 | `afe_pipeline.c:703` `afe_config_init(..., AFE_TYPE_VC, ...)` | 确认为 VC |
| T1.1 独立增益配置 | `Kconfig.projbuild` 的 `AEC_MIC_PGA_GAIN_DB`/`AEC_REF_PGA_GAIN_DB` | 存在 |
| T1.2 codec 异常非致命化 | `box_audio_codec.c` 的 `CODEC_TRY`/`s_codec_failed` | 存在 |
| P3 VAD 判定/打断支持 | `afe_pipeline.h` 的 `afe_voice_silence_ms`/`afe_preroll_copy`/
  `afe_last_bargein_us`/`afe_bargein_mute` | 存在 |
| T3.3 播放位置回报 | `audio_player.h` 的 `spk_buffer_ms()`/`spk_played_ms()` | 存在 |
| alpha/beta 两种打断演示 | `app_main.cpp` 的 `keep_talking` 分支逻辑 | 存在 |
| P4 Spark 流式开关 | `Kconfig.projbuild` 的 `AEC_STREAM_TO_CLOUD`（默认关） | 存在，默认关闭 |

结合 9 月 19 日的实测记录（E1-E5 全部通过，打断延迟 118-161ms，
alpha/beta 4/4 通过），**P0-P3 完成且有源码与实测双重证据支持**。
P4（接入 Spark 流式 ASR/LLM/TTS）按设计未开始，开关默认关闭。

## 当前设备状态

云端心跳在线，`last_seen` 为检查时刻（2026-10-11 11:26），
`codec_failed=False`、`reset_reason=1`（正常上电）。

## 今天新发现并已修复/确认的两件事

1. **spark↔ESP32 的 USB 链路一度看起来断了**：15 天的 spark 内核日志里
   完全没有 USB 插拔记录，怀疑是充电线或接触问题。现场重新插拔后，
   `303a:1001`（Espressif USB JTAG/serial debug unit）正常枚举，
   `/dev/ttyACM0` 出现——**链路已恢复**，可以继续走 USB 读写/恢复流程。
2. **仓库同步缺口已补齐**（见上），避免了后续工作建立在不完整代码上的风险。
