#!/usr/bin/env python3
"""量已归档录音里 ch1（硬件回采）的电平，用于 T1.3 回采电平校准。

判据取峰值不是 RMS：余量是留给突发响度的，RMS 合格而峰值削顶的话 AEC 的线性假设
就不成立了，测出来的 ERLE 会偏低且不可复现。目标 -12 ~ -6 dBFS。

用法：ref_level.py <bench目录>...
"""
import sys, wave
from pathlib import Path

import numpy as np


def level(path: Path):
    with wave.open(str(path), "rb") as w:
        d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    if d.size == 0:
        return None
    peak = np.max(np.abs(d))
    rms = np.sqrt(np.mean(d * d))
    to_db = lambda v: -99.0 if v <= 0 else 20 * np.log10(v / 32767.0)
    return to_db(peak), to_db(rms), int(peak)


if __name__ == "__main__":
    print(f"{'run':34s} {'ch1峰值':>9s} {'ch1 RMS':>9s} {'ch0峰值':>9s}  判定")
    for d in sys.argv[1:]:
        run = Path(d)
        ref, mic = run / "reference.wav", run / "mic_raw.wav"
        if not ref.is_file():
            continue
        rp, rr, _ = level(ref)
        mp, _, _ = level(mic) if mic.is_file() else (float("nan"),) * 3
        if rp < -20:
            verdict = "偏低，应抬一档"
        elif rp > -3:
            verdict = "过高，必须降"
        elif -12 <= rp <= -6:
            verdict = "在目标区间"
        else:
            verdict = "可接受但不在目标区间"
        print(f"{run.name:34s} {rp:8.1f}dB {rr:8.1f}dB {mp:8.1f}dB  {verdict}")
