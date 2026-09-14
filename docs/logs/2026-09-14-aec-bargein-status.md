# 2026-09-14 · ESP32 实时打断：整体进度与剩余工作

单篇总览，供下一轮（或两小时一次的定时检查）快速接手。分项细节见各篇日志。

**最终目标**：机器人说话时用户开口，200ms 内停播。

## 已完成并验收

| 项 | 判据 | 实测 | 详见 |
|---|---|---|---|
| T0.1 寄存器实测 | 打印 REG43/44 | `0x1A`(30dB) / `0x10`(0dB) | p0-prereqs |
| T0.2 播放超时 | turn03(93.12s) 完整播完 | 通过，8s→116.74s | p0-prereqs |
| T0.3 异常上报 | 制造失败→收到 failed | 单元+端到端双验 | p0-prereqs |
| T0.4 双通道录音 | 30s 双声道可 numpy 分析 | 480000 样本严格对齐 | t04-bench |
| T0.5 AFE 类型 | VC | 本来就是 VC，无需改 | p0-prereqs |
| T1.1/T1.2 显式增益 | 改配置→寄存器变化 | `0x10→0x12→0x10` | p1-gain |
| T1.3 回采电平 | 峰值 -12~-6 dBFS | -9.0 dBFS（语音@vol80） | p1-gain |
| **E1** 稳态 ERLE | ≥20 dB（好 ≥30） | **33.5 / 51.3 dB** | e1-e2 |
| **E1** 收敛时间 | <1s | **0.4–0.5 s** | e1-e2 |
| **E2** 单讲残余 | 无可辨语音 | **AEC 后 ASR 为空** | e1-e2 |
| **E5** 误打断率 | 0 次 / 127s | **0 次，0 段被切断** | e5-shared-state |
| **P3** 打断接入生产路径 | —— | 已实现，经两轮回归修复 | p3-bargein |
| **T3.3** 播放位置回报 | 心跳上报 spk_buffer_ms | buf 0–75ms（上限 90）；播完 37920ms 与素材逐毫秒吻合；打断后冻结在 10632ms | t33-position |

## 未完成（硬件阻塞）

**E2b / E3 / E4 全部需要树莓派扮演人声**，而它自 07:10 起三条路径全不通（见
`WORKING.md` 顶部的阻塞说明）。

这三项是最终目标的**直接验收环节**：目前已证明"机器人不会自己打断自己"（E5），
还缺"**人能不能真的打断它**"（E3/E4）和"AEC 会不会误伤人声"（E2b）。

恢复后的执行顺序（每一级失败会污染下一级，不要跳）：

```bash
cd tools/aec_bench
# E2b 反向对照：ESP32 静音，树莓派说话，看 AFE 会不会误伤目标人声
python3 aec_bench.py --tag e2b --seconds 25 --settle 2 --no-play \
    --pi-play turn03_user.wav --pi-volume 100 --pi-delay 3
python3 asr_check.py <归档目录>/mic_raw.wav <归档目录>/clean_aec.wav

# E3 双讲：ESP32 播 turn04，第 10s 时树莓派插话 turn05
python3 aec_bench.py --tag e3 --seconds 25 --settle 1 --volume 80 \
    --url http://192.168.1.16:8080/esp32/turn04_assistant.wav \
    --pi-play turn05_user.wav --pi-volume 100 --pi-delay 10
# 验收：clean_aec 的 ASR 里能不能读出 turn05 的内容

# E4 打断延迟：需要先在树莓派素材开头插 20ms 咔哒声
```

**E2b 的前置坑**：第一次尝试时树莓派的声音根本没到达 ESP32 麦克风（`pre_rms` 24.0
对底噪 17.5，只高 2.7 dB，两路 ASR 都是空）。恢复后先确认**音量和摆位**能让人声
明显高出底噪，否则 E2b/E3 测的都是噪声。

**E4 的前置工作已全部完成**（见 `e4-click-instrumentation`）。此处原先写的"ESP32 侧
时间戳埋点已就位"是错的：`t_duck` 有埋点，但**"人开口"那一半 `t_click` 当时根本没有
实现**，E4 并不是"等树莓派回来就能跑"。现已补上：

- 原始麦克风通道（AFE 之前）的咔哒检测 + 帧内定位，一次性触发
- `click_arm` / `click_result` 两条命令，延迟可远程读出，不是只打串口
- 5 个 `_click` 素材已在 spark 生成（`make_click_fixtures.py`）

两个时间戳都取自设备自己的时钟，不需要跨设备对时。

## 这次工作里三个不在原清单、但直接卡着目标的发现

1. **打断从来没有接进生产播放路径**（`p3-bargein`）。检测器门控在
   `playback_is_playing()` 上，而它只反映 TTS 分句队列；机器人真正说话走的
   `AudioPlayer` 从不注册，所以**用户根本打不断它**。若不先修这个就跑 E5，会得到
   "0 次误打断"的**假通过**——检测器压根没启动。

2. **接入时暴露的两层共享状态回归**（`e5-shared-state`）。`playback_kill()` 被
   "真打断"和"连接清理"两个语义混用；功放 GPIO 被 `playback.c` 单方面持有。两层
   都不是靠误触发计数发现的（三轮都是 0，看起来一直正常），是靠"每段是否播满
   时长"这个独立校验。

3. **P1 的预设方向被实测推翻**（`p1-gain`）。ch1 的 0 dB 不是漏设 bug，而是电气
   抽头的合理值。顺带发现真正的电平风险在 vol100——两路都在削顶边缘 4 dB 以内。

另外修掉两个基础设施问题：命令回执被 20 字符上限截断导致所有诊断结果静默丢失
（`p0-prereqs`）；`play_local_audio` 技能因"已部署未提交"被一次常规重部署抹掉
（`p3-bargein` 末尾）。

4. **每段话结尾被吃掉 90ms**（`t33-position`）。`i2s_channel_disable()` 是丢弃 DMA
   环而非排空，三条播放路径都在最后一次写入返回后立刻关输出。是为了给
   `spk_buffer_ms` 定量级才去翻 DMA 配置撞上的，播放结果永远返回 `done`、时长也对
   得上，单看播放本身发现不了。已修（只在正常播完时排空，被打断时照旧丢弃），
   但**这个修复本身还没有实测**。

## 一条方法上的教训

反复出现、值得单独记：**单一指标"看起来正常"不等于系统正常**。

- E5 的误触发计数三轮都是 0 → 实际藏着两层回归
- 保持原值不变去读寄存器 → 无法区分"配置生效"和"代码根本没跑"

两次都是靠**加一个独立的、与主指标不相关的校验**才看见问题。

## 产物位置

- 录音归档：`spark:~/workspace/data/aec/bench/<run_id>/`
  （原始三路 + 立体声 + analysis.json，改了参数可横向重算）
- 台架脚本：`tools/aec_bench/`（`aec_bench.py` / `asr_check.py` /
  `e5_false_bargein.py` / `ref_level.py` / `make_pink_noise.py`）
- 素材：`spark:~/workspace/data/aec/dialogue_wenyanwen/`（10 段对话原始 24kHz）
  和其 `esp32/` 子目录（5 段 assistant 的 16kHz 转码版 + 粉红噪声）
- 当前固件：`esp32-wangyutang-v47-drain-tail`
