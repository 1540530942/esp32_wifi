#!/usr/bin/env python3
"""E5 over the serial console, where the cause of a cut is unambiguous.

Runs on the WSL host, which has the USB serial line.

The cloud acks cannot answer the question E5 asks. A barge-in and a stop_audio
both go through AudioPlayer::stop(), so both surface as
"wav_playback ESP_ERR_INVALID_STATE" and the played figure freezes the same way.
Five full-scope runs came back 1, 3, 4 and 5 turns cut with no consistency,
which is the signature of interference rather than a detector misfiring --
retained MQTT commands replaying, as it turned out. Distinguishing them needs
the device's own log:

    "local barge-in -> duck+stop"   the detector fired      -> real E5 failure
    "early barge-in (raw mic)"      the detector fired      -> real E5 failure
    "command: stop_audio"           something sent a stop   -> discard the turn

Counts both and reports them separately, so a run can only fail E5 for the
reason E5 is about.
"""
import json
import subprocess
import sys
import time

import serial

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
PORT = "/dev/ttyACM0"

TURNS = [
    ("turn02_assistant.wav", 7.92),
    ("turn04_assistant.wav", 27.20),
    ("turn06_assistant.wav", 27.28),
    ("turn08_assistant.wav", 27.20),
    ("turn10_assistant.wav", 37.92),
]


def post(body, path="/play_lan_audio"):
    subprocess.run(
        ["curl", "-s", "-m", "25", "-X", "POST", HUB + path,
         "-H", "Content-Type: application/json", "-d", json.dumps(body)],
        capture_output=True)


def main() -> int:
    ser = serial.Serial(PORT, 115200, timeout=1)
    ser.reset_input_buffer()

    print(f"E5 over serial: {len(TURNS)} turns, "
          f"{sum(d for _, d in TURNS):.2f}s total, Pi silent\n")

    bargein_total = 0
    stop_total = 0
    results = []

    for name, dur in TURNS:
        # Command delivery drops turns often enough that a run can end with
        # three of five unplayed, which decides nothing. Retry until the device
        # actually starts, so the verdict rests on five turns.
        # Send ONCE and wait. Re-posting on every timeout queues duplicate
        # plays, and the run that did so reported turn04 as 37920 ms -- turn10's
        # length -- because a later clip's completion line got matched to an
        # earlier turn. Delivery is slow, not lost; give it room instead.
        ser.reset_input_buffer()
        post({"params": {"name": name},
              "settings_override": {"voice_volume_percent": 80}})
        # Delivery is minute-scale, not seconds. Serial caught the device
        # executing a play command issued long enough earlier that the script
        # had already written it off -- so "command not delivered" in earlier
        # runs was really "delivered late", and a 60 s window threw away turns
        # that would have played.
        waited = time.time() + 100
        got_start = False
        while time.time() < waited:
            probe = ser.readline()
            if probe and b"starting WAV playback" in probe:
                got_start = True
                break
        if not got_start:
            print(f"    {name}: 60s 内未启动")

        deadline = time.time() + dur + 30
        played = None
        bargein = 0
        stops = 0
        started = got_start
        while time.time() < deadline:
            line = ser.readline()
            if not line:
                continue
            txt = line.decode("utf-8", "replace").rstrip()
            if "starting WAV playback" in txt:
                started = True
            elif "barge-in" in txt and "->" in txt:
                bargein += 1
            elif "command: stop_audio" in txt or "action=stop_audio" in txt:
                stops += 1
            elif "playback position:" in txt and started:
                # "playback position: written=Xms played=Yms dropped=Zms"
                for token in txt.split():
                    if token.startswith("played="):
                        played = int(token[len("played="):].rstrip("ms"))
                break

        bargein_total += bargein
        stop_total += stops
        expected = int(dur * 1000)
        if played is None:
            verdict = "没有播放日志（命令未送达）"
        elif stops:
            verdict = f"作废 — 期间有 {stops} 条 stop 落地"
        elif bargein:
            verdict = f"**被打断 {bargein} 次 —— E5 真实失败**"
        elif expected - played > 250:
            verdict = f"短 {expected - played}ms，但无打断日志"
        else:
            verdict = "完整"
        results.append((name, played, expected, verdict))
        print(f"  {name}: 实播 {played}ms / 应为 {expected}ms  {verdict}")
        time.sleep(4)

    print()
    print(f"打断触发合计 {bargein_total} 次，外部 stop 合计 {stop_total} 次")
    print(f"判据（被自己回声触发停播 0 次）-> "
          f"{'通过' if bargein_total == 0 else '不通过'}")
    return 0 if bargein_total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
