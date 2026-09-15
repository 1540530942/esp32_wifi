#!/usr/bin/env python3
"""How often the barge-in actually fires when someone speaks.

Neither E4 nor E5 measures this. E4 times the stop on runs where the barge-in
fired and discards the rest; E5 counts stops that should not have happened.
A miss -- the user speaks and the robot keeps talking -- falls in the gap
between them, and it is the failure a user notices most.

Discarding non-firing runs also biases E4's median optimistic, because the runs
thrown away are exactly the hard ones.

The distinction this script rests on:

    robot not playing / click never detected -> invalid setup, discard
    robot playing + click detected + no stop -> MISS, count it

So a run is only discarded for reasons that are independent of whether the
detector works. Everything else is a result.

    python3 e4b_trigger_rate.py [--runs 15] [--user turn05_user_click.wav]
"""
import argparse
import json
import re
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
TASKS = "https://www.wangyutang.cn/action/api/tasks"


def post(url, body, timeout=25):
    proc = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-X", "POST", url,
         "-H", "Content-Type: application/json", "-d", json.dumps(body)],
        capture_output=True, text=True)
    try:
        return json.loads(proc.stdout)
    except Exception:
        return {}


def device():
    proc = subprocess.run(["curl", "-s", "-m", "20", HUB],
                          capture_output=True, text=True)
    try:
        return json.loads(proc.stdout).get("device", {})
    except Exception:
        return {}


def state():
    return device().get("state", {})


def command_ack(command_id, timeout_s=25):
    """Read the acknowledgement out of the command record (see e4_latency)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        dev = device()
        for cmd in dev.get("commands") or []:
            if cmd.get("id") == command_id and cmd.get("status") not in (
                    None, "dispatching", "pending", "sent", "dispatched"):
                return cmd.get("message") or cmd.get("status") or ""
        time.sleep(1.5)
    return None


def one_run(assistant, user, volume, pi_volume):
    """-> (outcome, latency_ms, detail)

    outcome is "fired", "miss", or "invalid".
    """
    post(f"{HUB}/play_lan_audio",
         {"params": {"name": assistant},
          "settings_override": {"voice_volume_percent": volume}})

    deadline = time.time() + 40
    while time.time() < deadline:
        if state().get("audio_playing"):
            break
        time.sleep(2)
    else:
        return "invalid", None, "robot never started"

    # Past the 1500ms onset grace, and far enough in that the AEC has settled.
    time.sleep(4)

    arm_id = post(f"{HUB}/command", {"action": "click_arm"}).get("command_id", "")
    if not command_ack(arm_id):
        return "invalid", None, "click_arm not acknowledged"
    if not state().get("audio_playing"):
        return "invalid", None, "clip ended before the interruption"

    post(TASKS, {"action": "play_local_audio", "params": {"name": user},
                 "settings_override": {"voice_volume_percent": pi_volume}})

    # Long enough for the whole user clip plus the 200ms budget several times
    # over. A stop that has not happened by now is a miss, not a slow stop.
    time.sleep(12)

    ack = command_ack(post(f"{HUB}/command",
                           {"action": "click_result"}).get("command_id", ""))
    if not ack:
        return "invalid", None, "click_result not acknowledged"

    # No click means the Pi's sound never reached the microphone at all, which
    # says nothing about the detector -- that is a broken setup, not a miss.
    if "click=0" in ack or "no click" in ack:
        return "invalid", None, f"click never detected ({ack[:60]})"

    match = re.search(r"latency=(\d+) ms", ack)
    if match:
        return "fired", int(match.group(1)), ack[:80]
    return "miss", None, ack[:80]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=15)
    ap.add_argument("--assistant", default="turn04_assistant.wav")
    ap.add_argument("--user", default="turn05_user_click.wav")
    ap.add_argument("--volume", type=int, default=80)
    ap.add_argument("--pi-volume", type=int, default=80)
    args = ap.parse_args()

    fired, missed, invalid = [], 0, []
    print(f"打断触发率：{args.runs} 次，机器人播 {args.assistant} @vol{args.volume}，"
          f"树莓派播 {args.user} @vol{args.pi_volume}\n")
    for i in range(args.runs):
        outcome, latency, detail = one_run(
            args.assistant, args.user, args.volume, args.pi_volume)
        if outcome == "fired":
            fired.append(latency)
            print(f"  {i+1:2}/{args.runs}  触发   {latency} ms")
        elif outcome == "miss":
            missed += 1
            print(f"  {i+1:2}/{args.runs}  **漏触发**  {detail}")
        else:
            invalid.append(detail)
            print(f"  {i+1:2}/{args.runs}  作废   {detail}")
        post(f"{HUB}/command", {"action": "stop_audio"})
        time.sleep(6)

    scored = len(fired) + missed
    print()
    if not scored:
        print("没有有效样本，无法判定")
        return 2
    rate = len(fired) / scored
    print(f"有效样本 {scored}（作废 {len(invalid)}）")
    print(f"触发 {len(fired)}，漏触发 {missed}  ->  **触发率 {rate*100:.0f}%**")
    if fired:
        fired.sort()
        print(f"触发时的延迟：中位 {fired[len(fired)//2]} ms，最大 {fired[-1]} ms")
    # No pass mark is asserted here: the task list never set one, and inventing
    # a threshold after seeing the number is how a measurement becomes a
    # rationalisation. The figure is reported for a threshold to be chosen
    # against.
    print("\n（任务清单未给漏触发判据，此处只报数，不自拟及格线）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
