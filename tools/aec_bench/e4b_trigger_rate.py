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
PI_CLIP_DIR = "/home/pi/action_move/local_audio"


def pi_play(name, timeout=40):
    """Play the interruption on the Pi over ssh, and block until it finishes.

    Uses ssh rather than /action/api/tasks, but NOT because that queue is slow:
    measured end to end it is 1.4s from POST to sound, with the Pi claiming the
    task in 7ms. An earlier version of this comment blamed an "8 second" queue
    lag for contaminated measurements, and that was wrong -- the figure came
    from misreading one timeline, and switching transport actually made the
    trigger rate look WORSE (50-60% -> 42%), which should have settled it at
    the time. The scoring was the whole problem.

    ssh is still preferable here: it blocks for exactly the clip, so the
    observation window is known rather than inferred.
    """
    subprocess.run(["ssh", "pi", f"aplay -D plughw:2,0 {PI_CLIP_DIR}/{name}"],
                   capture_output=True, timeout=timeout)


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
    play_id = post(f"{HUB}/play_lan_audio",
                   {"params": {"name": assistant},
                    "settings_override": {"voice_volume_percent": volume}}
                   ).get("command_id", "")

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

    # Blocks for the duration of the clip, so when this returns the interrupter
    # has definitely spoken -- there is no "did it even play" ambiguity left.
    if user:
        pi_play(user)
    else:
        time.sleep(4.1)      # same window, no sound

    # A little past the clip, so a slow stop still counts as a stop.
    time.sleep(3)

    # Judged on whether the PLAY COMMAND ended early, not on the click pairing
    # and not on audio_playing.
    #
    # click_result only records a latency when the click was seen before the
    # barge-in fired (it pairs s_click_us with s_click_duck_us). The barge-in
    # fires on the first qualifying speech frame, while the click detector has
    # to satisfy an envelope ratio first, so the barge-in can easily win the
    # race -- and then the acknowledgement says "no barge-in yet" for a turn
    # that was in fact cut off. Scoring those as misses is what produced the
    # 42% figure; several of them had stopped the robot exactly as intended.
    #
    # audio_playing is no better: it rides the heartbeat and can be 5s stale.
    #
    # The play command is the durable evidence. turn04 is 27.2s and the
    # interruption lands about 7s in, so a command that has already reached a
    # terminal state by now was ended by something -- and nothing else in this
    # run sends a stop until after this check.
    # Only a command that ended as "failed|stage=wav_playback" counts. A clip
    # that simply ran out reports "done", and turn04 is 27.2s against roughly
    # 18s of run, so a natural end would mean the run overran -- which must not
    # be scored as a successful interruption.
    stopped = False
    for c in device().get("commands") or []:
        if c.get("id") == play_id:
            if c.get("status") == "failed" and "wav_playback" in str(c.get("message")):
                stopped = True
            elif c.get("status") == "done":
                return "invalid", None, "整段播完了，插话没赶上（本轮超时）"
    ack = command_ack(post(f"{HUB}/command",
                           {"action": "click_result"}).get("command_id", "")) or ""
    stats = " ".join(w for w in ack.split()
                     if w.split("=")[0] in ("frames", "loud", "speech", "both", "max_rms"))
    match = re.search(r"latency=(\d+) ms", ack)
    if match:
        return "fired", int(match.group(1)), stats
    if stopped:
        return "fired", None, "停播（无咔哒，不报延迟） " + stats
    return "miss", None, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=15)
    ap.add_argument("--assistant", default="turn04_assistant.wav")
    ap.add_argument("--user", default="turn05_user.wav")
    ap.add_argument("--volume", type=int, default=80)
    ap.add_argument("--pi-volume", type=int, default=80)
    # Negative control: run the identical sequence but never play the
    # interruption. A criterion that reports "fired" here is measuring
    # something other than barge-in, and a 100% rate would mean nothing.
    ap.add_argument("--silent-control", action="store_true")
    args = ap.parse_args()

    fired, missed, invalid, fired_n = [], 0, [], 0
    print(f"打断触发率：{args.runs} 次，机器人播 {args.assistant} @vol{args.volume}，"
          f"树莓派播 {args.user} @vol{args.pi_volume}\n")
    for i in range(args.runs):
        outcome, latency, detail = one_run(
            args.assistant, "" if args.silent_control else args.user,
            args.volume, args.pi_volume)
        if outcome == "fired":
            if latency is not None:
                fired.append(latency)
            fired_n += 1
            print(f"  {i+1:2}/{args.runs}  触发   "
                  f"{str(latency) + ' ms' if latency is not None else '(停播)'}")
        elif outcome == "miss":
            missed += 1
            print(f"  {i+1:2}/{args.runs}  **漏触发**  {detail}")
        else:
            invalid.append(detail)
            print(f"  {i+1:2}/{args.runs}  作废   {detail}")
        post(f"{HUB}/command", {"action": "stop_audio"})
        time.sleep(6)

    scored = fired_n + missed
    print()
    if not scored:
        print("没有有效样本，无法判定")
        return 2
    rate = fired_n / scored
    print(f"有效样本 {scored}（作废 {len(invalid)}）")
    print(f"触发 {fired_n}，漏触发 {missed}  ->  **触发率 {rate*100:.0f}%**")
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
