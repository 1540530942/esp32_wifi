#!/usr/bin/env python3
"""E5 at the scope the task list actually specifies: five assistant turns, ~127 s.

E5 was last run at full length on v40-v42. Since then the barge-in path changed
substantially -- T3.1's silence hangover, a raw-mic early path added and then
disabled, and vad_min_speech_ms taken from 128 ms to 48 ms, which makes the VAD
2.7x quicker to call something speech. Passing a single 37.92 s clip does not
stand in for 127 s under those settings; a false trigger has 3.3x as many
chances and the VAD is more willing.

Criterion, from the task list: zero playbacks stopped by the robot's own
residual echo. Measured here as "did each clip play for its full duration",
which is equivalent -- a barge-in stops playback, and spk_played_ms freezes at
the point it was cut. Serial would give the barge-in count directly, but both
serial links were down for this run.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
LIST = "https://www.wangyutang.cn/devices/api/list"

TURNS = [
    ("turn02_assistant.wav", 7920),
    ("turn04_assistant.wav", 27200),
    ("turn06_assistant.wav", 27280),
    ("turn08_assistant.wav", 27200),
    ("turn10_assistant.wav", 37920),
]
TOLERANCE_MS = 250     # heartbeat granularity, not a real shortfall


def post(url, body, timeout=25):
    proc = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-X", "POST", url,
         "-H", "Content-Type: application/json", "-d", json.dumps(body)],
        capture_output=True, text=True)
    try:
        return json.loads(proc.stdout)
    except Exception:
        return {}


def state():
    proc = subprocess.run(["curl", "-s", "-m", "20", LIST],
                          capture_output=True, text=True)
    try:
        payload = json.loads(proc.stdout)
    except Exception:
        return {}
    for dev in (payload.get("devices") or payload):
        if dev.get("device_id") == "esp32-s3-walle":
            return dev.get("state", {})
    return {}


def play_one(name, expected_ms):
    # No stop_audio here. Command delivery lags by seconds, so a stop issued at
    # the top of one turn lands during the next one and kills it: the first
    # full run showed cuts at 101, 735 and 758 ms -- all before the 900 ms onset
    # grace, where a barge-in cannot fire by construction. That timing is what
    # gave it away. Reported as-is it would have been a false E5 failure.
    # The loop already waits for each clip to end, so nothing needs stopping.
    before = state().get("spk_played_ms") or 0
    post(f"{HUB}/play_lan_audio",
         {"params": {"name": name},
          "settings_override": {"voice_volume_percent": 80}})

    # Do not decide "it started" from audio_playing alone. The heartbeat lands
    # roughly every 8 s and turn02 is 7.92 s, so a whole clip can begin and end
    # between two of them with audio_playing never observed true -- reported as
    # a delivery failure when the clip in fact played perfectly. A change in
    # spk_played_ms is the durable evidence that something ran.
    deadline = time.time() + 45
    started = False
    while time.time() < deadline:
        snap = state()
        if snap.get("audio_playing") or (snap.get("spk_played_ms") or 0) != before:
            started = True
            break
        time.sleep(3)
    if not started:
        return None, "never started (command not delivered)"

    deadline = time.time() + expected_ms / 1000 + 45
    last = 0
    while time.time() < deadline:
        snap = state()
        played = snap.get("spk_played_ms") or 0
        if played:
            last = played
        if not snap.get("audio_playing") and last:
            break
        time.sleep(4)
    return last, None


def main() -> int:
    print(f"E5 full scope: {len(TURNS)} turns, "
          f"{sum(d for _, d in TURNS) / 1000:.2f}s total, Pi silent\n")
    post(f"{HUB}/command", {"action": "stop_audio"})   # clear the decks once
    time.sleep(12)
    cut = 0
    total = 0
    for name, expected in TURNS:
        played, err = play_one(name, expected)
        if err:
            print(f"  {name}: 作废 — {err}")
            continue
        total += 1
        short = expected - played
        if short > TOLERANCE_MS:
            cut += 1
            print(f"  {name}: 实播 {played}ms / 应为 {expected}ms  "
                  f"**短 {short}ms —— 被切断**")
        else:
            print(f"  {name}: 实播 {played}ms / 应为 {expected}ms  完整")

    print()
    if total == 0:
        print("没有有效轮次，无法判定")
        return 2
    print(f"有效轮次 {total}/{len(TURNS)}，被切断 {cut} 次")
    # A verdict needs the duration the criterion names, not whatever fraction of
    # it happened to run. One turn out of five is 27s, not 127s, and reporting
    # that as a pass would be the same mistake this work has already made three
    # times: reading a partial measurement as a whole one.
    if total < len(TURNS):
        covered = sum(d for (n, d), _ in zip(TURNS, range(total))) / 1000
        print(f"判定无效 —— 只覆盖了约 {covered:.0f}s，判据要求 127s。重跑。")
        return 2
    print(f"判据 0 次 -> {'通过' if cut == 0 else '不通过'}")
    return 0 if cut == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
