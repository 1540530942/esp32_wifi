#!/usr/bin/env python3
"""Prepend a 20 ms click to the Raspberry Pi's user fixtures, for E4.

E4 measures "user opens their mouth -> playback stops". Both timestamps have to
come from the ESP32's own clock, otherwise the number is really a measurement of
clock skew between two machines. The click is what makes that possible: the
ESP32 sees it in the RAW microphone channel (before the AFE, so it is not itself
delayed by AEC/VAD) and timestamps it there; the barge-in duck is timestamped in
the same clock a moment later.

The click has to survive being mixed with the robot's own echo in the raw mic,
so it is a full-scale broadband burst rather than a tone: a tone can land in a
null of the room response, and a quiet click would have to be separated from the
echo by a threshold that also has to not fire ON the echo.

It deliberately does NOT trigger the barge-in by itself. The detector needs
CONFIG_AEC_BARGEIN_SPEECH_FRAMES (4) sustained speech frames, ~64 ms, and the
click is 20 ms -- so the duck is still caused by the speech that follows, and the
measured interval is a real "opened mouth -> silence", not a click round-trip.
"""
import argparse
import contextlib
import random
import struct
import wave
from pathlib import Path

CLICK_MS = 20


def click_samples(frame_rate: int, amplitude: float = 0.9) -> bytes:
    """A 20 ms burst that decays across its length.

    The decay matters: a flat burst ending abruptly puts a second transient at
    its far end, which the detector would be equally happy to fire on, making
    the recorded onset ambiguous by the width of the click.
    """
    n = int(frame_rate * CLICK_MS / 1000)
    rng = random.Random(20260914)
    out = bytearray()
    for i in range(n):
        decay = (1.0 - i / n) ** 2
        v = int(32767 * amplitude * decay * rng.uniform(-1.0, 1.0))
        out += struct.pack("<h", max(-32768, min(32767, v)))
    return bytes(out)


def prepend(src: Path, dst: Path) -> tuple[float, float]:
    with contextlib.closing(wave.open(str(src), "rb")) as r:
        if r.getnchannels() != 1 or r.getsampwidth() != 2:
            raise SystemExit(f"{src.name}: expected mono 16-bit, got "
                             f"{r.getnchannels()}ch/{r.getsampwidth() * 8}bit")
        rate = r.getframerate()
        frames = r.readframes(r.getnframes())
        original_s = r.getnframes() / rate
    head = click_samples(rate)
    with contextlib.closing(wave.open(str(dst), "wb")) as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(head + frames)
    return original_s, original_s + CLICK_MS / 1000


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=str(Path.home() / "workspace/data/aec/dialogue_wenyanwen"))
    ap.add_argument("--pattern", default="*_user.wav")
    args = ap.parse_args()

    base = Path(args.dir)
    sources = sorted(p for p in base.glob(args.pattern) if "_click" not in p.name)
    if not sources:
        raise SystemExit(f"no fixtures matching {args.pattern} under {base}")
    for src in sources:
        dst = src.with_name(src.stem + "_click.wav")
        before, after = prepend(src, dst)
        print(f"{dst.name}: {before:.2f}s -> {after:.2f}s")


if __name__ == "__main__":
    main()
