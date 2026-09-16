#!/usr/bin/env python3
"""Per-frame RMS distributions of the AFE output, for choosing the barge-in gate.

The gate is `clean_rms >= AEC_BARGEIN_MIN_RMS && vad == VAD_SPEECH`, evaluated
once per AFE fetch chunk -- 512 samples, 32 ms, as the device reports.

The current threshold of 40 was justified in a source comment by comparing mean
RMS over a window: 8.5 with the robot alone against 218.6 for a human, "4.5x
clear of both". That is the wrong statistic. The gate tests every frame, so
what matters is the upper tail of the robot-alone distribution against the body
of the human distribution -- and measured per frame the robot-alone residual
reaches 151 and 1073, one to two orders above that supposed clearance.

This computes both distributions from the archived three-channel captures and
reports, for a range of thresholds, how often each class crosses. No parameter
is changed from here; the point is to see the overlap before touching anything.

Note on scope: only the level half of the gate is modelled. The VAD is the
other half and is not recorded in the captures, so the robot-alone crossing
rate here is an upper bound on false triggers, not the rate itself. That is the
useful direction -- it bounds how much work the level gate is doing.
"""
import glob
import math
import os
import sys
import wave

import numpy as np

FRAME = 512          # AFE fetch chunk, 32 ms at 16 kHz (device-reported)
ARCHIVE = "/home/ubuntu/workspace/data/aec/bench"

# Which captures hold which class, and for double-talk, the window in which the
# person is actually speaking (located from the level profile, not assumed).
ROBOT_ONLY = ["e2_turn04_assistant_143233", "e2_turn10_assistant_143353",
              "e2_turn04_073043"]
HUMAN_ONLY = ["e2b_noecho_103228"]
DOUBLE_TALK = [("e3_doubletalk_091414", 3.5, 9.0),
               ("e3_long_103507", 0.0, 30.0)]


def frames_rms(path, start_s=None, end_s=None):
    with wave.open(path, "rb") as w:
        rate, n = w.getframerate(), w.getnframes()
        d = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    if start_s is not None:
        d = d[int(start_s * rate):int(end_s * rate)]
    usable = (len(d) // FRAME) * FRAME
    if usable == 0:
        return np.array([])
    blocks = d[:usable].reshape(-1, FRAME)
    return np.sqrt((blocks ** 2).mean(axis=1))


def collect(names_or_windows, windowed=False):
    out = []
    for item in names_or_windows:
        if windowed:
            name, a, b = item
        else:
            name, a, b = item, None, None
        path = os.path.join(ARCHIVE, name, "clean_aec.wav")
        if not os.path.exists(path):
            continue
        r = frames_rms(path, a, b)
        if r.size:
            out.append(r)
            print(f"    {name}: {r.size} 帧")
    return np.concatenate(out) if out else np.array([])


def describe(label, x):
    if not x.size:
        print(f"  {label}: 无数据")
        return
    qs = [50, 90, 99, 99.9, 100]
    vals = " ".join(f"p{q}={np.percentile(x, q):.0f}" for q in qs)
    print(f"  {label}: n={x.size}  mean={x.mean():.1f}  {vals}")


def main():
    print("=== 机器人独播（AFE 输出应接近静音；越过门限就是误打断的燃料）===")
    robot = collect(ROBOT_ONLY)
    print("=== 人声独讲（无回声，E2b）===")
    human = collect(HUMAN_ONLY)
    print("=== 双讲重叠段（人声必须能触发）===")
    both = collect(DOUBLE_TALK, windowed=True)

    print("\n逐帧 RMS 分布（帧长 512 样本 = 32ms）：")
    describe("机器人独播", robot)
    describe("人声独讲  ", human)
    describe("双讲重叠  ", both)

    print("\n门限扫描：")
    print(f"  {'门限':>6} | {'机器人越过':>10} | {'人声独讲越过':>12} | {'双讲越过':>10}")
    print("  " + "-" * 50)
    for t in (40, 60, 80, 100, 150, 200, 300, 400, 600, 800, 1200):
        rr = (robot >= t).mean() * 100 if robot.size else float("nan")
        hh = (human >= t).mean() * 100 if human.size else float("nan")
        bb = (both >= t).mean() * 100 if both.size else float("nan")
        mark = "  ← 当前" if t == 40 else ""
        print(f"  {t:>6} | {rr:9.2f}% | {hh:11.2f}% | {bb:9.2f}%{mark}")

    print("\n读法：")
    print("  「机器人越过」是误打断的燃料——它必须和 VAD 同时成立才会真的触发，")
    print("  所以这一列是误触发率的上界，不是误触发率本身。")
    print("  「双讲越过」是真打断的弹药——太低就会漏触发。")
    print("  两条曲线的间隔就是电平门限能提供的全部余量。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
