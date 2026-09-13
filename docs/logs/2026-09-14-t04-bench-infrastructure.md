# 2026-09-14 · T0.4 双通道调试录音基础设施

## 与原计划的偏离（先说清楚）

任务书写的是"固件加调试模式，把 ch0 原始信号和 AFE 输出打包成双声道，经**已有
voice_link 通道**推给树莓派"。实际情况：

1. **`voice_link` 在这个代码库里不存在**（`grep -rn voice_link esp32_wifi_impl/main/`
   零结果），所以"复用已有通道"这个前提不成立。
2. **但固件里已经有一套更完整的采集能力**：`aec_capture` 命令在**同一个采集窗口**
   里把 mic_raw(ch0) / reference(ch1) / clean_aec(AFE输出) 写进三个按样本索引并行
   推进的缓冲区（`afe_pipeline.c` 的 `s_cap_mic/s_cap_ref/s_cap_clean`），天然
   时间对齐，然后三路分别上传成 WAV。这比任务书要求的两路还多一路参考通道。

所以没有新造固件通道，也没有动固件。缺的只是**取回、打包、归档、分析**这一段，
用一个脚本补上。少改一处固件就少一次 OTA 和一次回归风险，而且多出来的 ch1 参考
通道对后面诊断"回采电平对不对"直接有用。

## 交付物

`tools/aec_bench/aec_bench.py` —— 一条命令跑完整个流程：

1. 下发 `aec_capture`（支持三种模式：内置噪声 / `url` 播局域网素材 / `no_play`
   纯录音）。
2. 从 tang 的 device_hub 数据卷里取回本次新增的三个 WAV。
3. 打包 `stereo_pre_post.wav`：**左声道 = AFE 前（原始麦克风），右声道 = AFE 后**。
4. numpy 离线分析：整体/稳态 ERLE、100ms 粒度的 ERLE 曲线、收敛时间、分频段
   （0-500 / 500-2k / 2k-8k）。
5. 归档到 `spark:~/workspace/data/aec/bench/<run_id>/`，原始三路 + 立体声 +
   `analysis.json` 全留，便于改参数后横向对比。

`tools/aec_bench/make_pink_noise.py` —— E1 用的粉红噪声生成器。

### 两个实现上的坑（记下来免得重踩）

- **文件归属不能靠 `ls -t` 取最近 N 个**：device_hub 上传时会把文件名随机化，而
  同一秒内上传的多个文件排序不保证稳定，`boot_announce`/`speak` 产生的文件也会混
  进来。改成**采集前后快照比对**，只取新增文件，再按 `ls --full-time` 的精确 mtime
  升序还原固件的上传顺序（mic_raw → reference → clean_aec）。
- **回执可能拿不到**：当时 v39（ACK 截断修复）还没刷上去，`aec_capture` 的长
  `done|...` 状态串会被服务端 20 字符上限拒绝，命令停在 `dispatched`。脚本因此加了
  一条回退：从设备自己上报的日志里按 command_id 捞结果。这条回退即使在 v39 之后
  也值得留着——它让台架不依赖回执链路本身是否健康。

## 验收

跑 `--tag t04verify --seconds 30 --settle 3 --volume 70`：

```
captured mic=480000 ref=480000 clean=480000 uploads=1/1/1
stereo packed: 30.00s (mic=480000 clean=480000 samples)
{"duration_s": 30.0, "erle_full_db": 29.09, "erle_steady_db": 28.98,
 "pre_rms": 268.1, "post_rms": 9.4, "convergence_s": 0.0,
 "erle_low_0_500_db": 13.38, "erle_mid_500_2k_db": 30.97, "erle_high_2k_8k_db": 49.19}
```

- 30.00 秒双声道 WAV，两个声道各 480000 样本、**样本数完全一致**（同一采集窗口、
  同一索引写入，对齐是构造保证的，不是事后对齐出来的）。
- numpy 能直接读出来算功率比。**T0.4 验收通过。**
- 归档在 `spark:~/workspace/data/aec/bench/t04verify_072048/`。

### 这次验收顺带看到的东西（不是 E1 正式结果）

用的是固件内置的均匀白噪声 burst，不是 E1 要求的粉红噪声，所以**不能当 E1 的
数据**。但分频段结果已经露出一个明显特征：

| 频段 | ERLE |
|---|---|
| 0–500 Hz | **13.4 dB** |
| 500–2k Hz | 31.0 dB |
| 2k–8k Hz | 49.2 dB |

低频比中高频差 17～36 dB。按任务书的判读规则，这是"**低频特别差 → 结构传声或腔体
共振**"的签名，而不是"全频段均匀地差 → 电平或失真问题"。E1 用粉红噪声正式复测时
重点看这一条是否复现。

另外 `convergence_s=0.0` 是因为这次 `settle=3` 的收敛窗口跑在采集窗口**之前**——
滤波器在采集开始时已经收敛完了。E1 要测收敛时间必须用 `settle=0`，让采集和播放
同时开始。

## 粉红噪声（E1 素材）

`pink_noise_40s.wav`：16 kHz 单声道 16 bit、40 秒、峰值 -6 dBFS（留余量防削波，
削波会破坏 AEC 的线性假设，测出的 ERLE 既偏低又不可复现）。用 Voss-McCartney
算法生成，频谱自检每倍频程精确 -3.0 dB：

```
125-250Hz 134.7 dB / 250-500Hz 131.7 / 500-1k 128.5 / 1k-2k 125.3 / 2k-4k 122.0
```

已部署到 `spark:~/workspace/data/aec/dialogue_wenyanwen/esp32/`，ESP32 可以通过
`url` 模式经局域网直接拉取播放。
