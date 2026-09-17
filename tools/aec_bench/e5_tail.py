#!/usr/bin/env python3
"""Pool the per-turn AFE-output peaks from every soak and describe the tail.

The barge-in gate has twice been argued from a single soak's largest value: 94
"1.6x clear" of a gate at 150, then a later soak reaching 140. A sample maximum
is not an upper bound, and using it as one is the same error as justifying a
per-frame gate with a windowed mean -- asking one statistic a question it
cannot answer.

This pools every soak's per-turn maxima and reports percentiles, which is what
an operating point can actually be argued from.
"""
import json
import sys

SRC = "docs/logs/e5_maxima.jsonl"


def main():
    vals, clips, cuts, soaks = [], 0, 0, 0
    try:
        for line in open(SRC):
            r = json.loads(line)
            vals += r.get("maxima") or []
            clips += r.get("clips") or 0
            cuts += r.get("cuts") or 0
            soaks += 1
    except FileNotFoundError:
        print(f"还没有累积数据（{SRC} 不存在）")
        return 1
    if not vals:
        print("没有样本")
        return 1
    vals.sort()
    n = len(vals)
    def pc(q):
        return vals[min(n - 1, int(round(q * (n - 1) / 100)))]
    print(f"{soaks} 次浸泡，{clips} 段播放，被切 {cuts} 段")
    print(f"每轮 AFE 输出峰值（n={n}）：")
    print(f"  中位 {pc(50)}   p90 {pc(90)}   p95 {pc(95)}   p99 {pc(99)}   最大 {vals[-1]}")
    print()
    print("  门限 | 本样本中越过的轮次")
    for t in (100, 120, 150, 200, 250, 300):
        over = sum(1 for v in vals if v >= t)
        print(f"  {t:>5} | {over:>3}/{n}  ({over*100.0/n:.1f}%)")
    print()
    print("  人声侧每 4s 发声窗口峰值（E2b/E3 实测）：357 462 498 634 722 744")
    print("                                        795 841 911 932 1234 1248 1300 1583 1662")
    print("  -> 门限要同时高过机器人尾巴、低过人声最小值 357。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
