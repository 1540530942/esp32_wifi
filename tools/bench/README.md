# 全双工验收台架脚本

在 `tang` 的 device-hub 容器里运行（那里能直接 `import server` 拿到 MQTT 下发和音频目录）。
部署：`docker cp <file> device-hub:/app/`。

判据与门禁见 [`../../docs/aec/ACCEPTANCE.md`](../../docs/aec/ACCEPTANCE.md)，
实测结果见 [`../../docs/aec/RUN_2026-09-10.md`](../../docs/aec/RUN_2026-09-10.md)。

| 脚本 | 用途 |
|---|---|
| `scene.py <esp_vol> <pi_vol> <run_id>` | G4 场景：人声远端连播 60 s，树莓派插话 3 次，ESP32 采 3 段 |
| `scene_g5.py <esp_vol> <pi_vol> <run_id>` | G5 场景：**噪声**远端，树莓派插话 4 次，采一段 30 s |
| `ner2.py <mic>.wav <ref>.wav <clean>.wav [...]` | 打分：区别性词计数 + 近端字级 NER，可一次传多组 |
| `falseduck.py <ref>.wav` | 数 reference 通道上的 duck（持续跌到中位数 1/3 以下 ≥200 ms）|
| `duck.py <ref>.wav <clean>.wav` | 同一捕获内的打断延迟：clean 起点 → reference 塌陷 |

**文件顺序永远是 mic / ref / clean。**

## 两个容易踩的地方

**采样点必须跟捕获节奏对齐。** 一段是 12 s 录制加约 12 s 上传，共约 24 s；
排得比这密，实际落点就会漂，段里可能根本不含插话。`scene.py` 因此把插话设成 24 s
间隔（5/29/53），采样点 3/27/51 —— 实测稳定落在 3.0/27.0/51.1。
对齐之前 6 段里有 2 段 ASR 全空，对齐之后 6/6 满分。

**打分不能用纯字级编辑距离。** 近端脚本 S 和远端脚本 E 共享 `今天`/`我们`/`出去走走`，
字级距离会给只含 E 的转写打出可观的 S 分，反方向也会算出并不存在的泄漏。`ner2.py`
改用区别性词：S 专属 `下午`/`好吗`，E 专属 `天气`/`不错`/`一起`/`怎么样`/`相机`/`带上`。

## 远端用人声还是噪声

看判据：

- **G4 用人声 TTS** —— 判据是 ASR 转写，远端必须是真实语音才有意义。
- **G5 用宽带噪声** —— 判据是电平时序。人声句间自带停顿，reference 包络摆幅 36 dB，
  duck 藏在里面认不出；噪声只摆 0.8 dB，任何下跌都只能是 duck。
