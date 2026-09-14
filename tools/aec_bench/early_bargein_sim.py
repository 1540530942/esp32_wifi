#!/usr/bin/env python3
"""Run the early barge-in detector offline against a recorded capture.

Seven firmware versions went into tuning this by flashing and re-measuring,
about fifteen minutes a round. The task list said to do it the other way from
the start (T0.4: "分析全部在树莓派离线做——同一份录音可反复分析，改参数后能横向
对比"), and it is right: both input channels are already archived, the detector
is twenty lines of arithmetic, and a parameter sweep that takes seconds here
answers what a flash cycle answers one point at a time.

What it reports, for each (ratio, frames) pair:
  - whether the detector fires during the robot-only stretch  -> E5 false trigger
  - whether it fires once the interrupting voice starts       -> E4 detection
  - how long after that voice starts                          -> E4 latency

Usage:
    python3 early_bargein_sim.py <capture_dir> --voice-start 10.0
"""
import argparse
import contextlib
import struct
import wave
from pathlib import Path

FRAME = 256          # samples per feed frame at 16 kHz -> 16 ms
RATE = 16000
ENV_ALPHA = 0.05
MIN_PEAK = 2000
SEED_FRAMES = 16


def read_mono(path: Path) -> list[int]:
    with contextlib.closing(wave.open(str(path), "rb")) as handle:
        frames = handle.readframes(handle.getnframes())
    return list(struct.unpack(f"<{len(frames) // 2}h", frames))


def frame_peaks(samples: list[int]) -> list[int]:
    peaks = []
    for start in range(0, len(samples) - FRAME, FRAME):
        chunk = samples[start:start + FRAME]
        peaks.append(max(abs(v) for v in chunk))
    return peaks


def simulate(mic_peaks, ref_peaks, ratio_x, need_frames, grace_frames):
    """Returns the frame index where it fires, or None.

    Mirrors the firmware: two-frame reference window, envelope updated while the
    robot alone is talking and frozen during a candidate run.
    """
    env = 0.0
    prev_ref = 0
    run = 0
    seen = 0
    for i, (peak, peak_ref) in enumerate(zip(mic_peaks, ref_peaks)):
        ref_win = max(peak_ref, prev_ref)
        prev_ref = peak_ref
        ratio = peak / (ref_win + 1.0)
        external = peak > MIN_PEAK and ratio > env * ratio_x
        settling = i < grace_frames or seen < SEED_FRAMES
        seen += 1
        if not external or settling:
            env = env * (1.0 - ENV_ALPHA) + ratio * ENV_ALPHA
            run = 0
            continue
        run += 1
        if run >= need_frames:
            return i
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("capture_dir")
    ap.add_argument("--voice-start", type=float, required=True,
                    help="seconds into the capture where the interrupting voice begins")
    ap.add_argument("--grace-ms", type=int, default=900)
    args = ap.parse_args()

    base = Path(args.capture_dir)
    mic = frame_peaks(read_mono(base / "mic_raw.wav"))
    ref = frame_peaks(read_mono(base / "reference.wav"))
    n = min(len(mic), len(ref))
    mic, ref = mic[:n], ref[:n]
    voice_frame = int(args.voice_start * RATE / FRAME)
    grace_frames = int(args.grace_ms / 1000 * RATE / FRAME)

    print(f"capture: {n} frames ({n * FRAME / RATE:.1f}s), "
          f"voice starts at frame {voice_frame} ({args.voice_start}s)")
    print(f"{'ratio':>6} {'frames':>7} {'fires at':>9} {'verdict':>34}")

    for ratio_x in (1.3, 1.5, 1.8, 2.2, 2.6, 3.0, 4.0):
        for need in (3, 5, 8, 12):
            fired = simulate(mic, ref, ratio_x, need, grace_frames)
            if fired is None:
                verdict, when = "never fires (E4 fails)", "-"
            elif fired < voice_frame:
                verdict = "fires on the robot (E5 fails)"
                when = f"{fired * FRAME / RATE:.2f}s"
            else:
                latency = (fired - voice_frame) * FRAME / RATE * 1000
                verdict = f"detects voice, +{latency:.0f}ms"
                when = f"{fired * FRAME / RATE:.2f}s"
            print(f"{ratio_x:>6} {need:>7} {when:>9} {verdict:>34}")


if __name__ == "__main__":
    main()
