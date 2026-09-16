#!/usr/bin/env python3
"""E4: barge-in latency, measured end to end and verified at every step.

Why this exists rather than a handful of curl calls: command delivery to the
ESP32 is intermittent (MQTT publishes return status=failed often enough to
matter). A run where the robot never started playing looks almost identical to
a run where barge-in failed -- both end with "click detected, no barge-in yet"
-- and reading the second as a latency failure would be wrong. Every step is
confirmed before the next one starts, and a run that cannot be confirmed is
discarded rather than reported.

The tell for an unconfirmed run is in the click log itself: ref_win is the
reference-channel peak at the moment the click landed, so ref_win ~= 1 means
the speaker was silent and there was nothing to interrupt.

Usage:
    python3 e4_latency.py [--runs 5] [--assistant turn04_assistant.wav]
                          [--user turn05_user_click.wav]
"""
import argparse
import json
import re
import subprocess
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
PI_CLIP_DIR = "/home/pi/action_move/local_audio"


def pi_play(name, timeout=40):
    """Play the interruption on the Pi over ssh, blocking until it finishes.

    Not /action/api/tasks: that queue takes about 8 seconds from cue to sound.
    E4 arms the click detector and then expects the interruption inside the
    clip that is playing, so an 8 second lag put the sound outside the window
    in some runs and at its very edge in others -- which is why runs were being
    discarded as "clip ended before the interruption" and why the latency
    distribution looked bimodal. aplay starts inside a second.
    """
    subprocess.run(["ssh", "pi", f"aplay -D plughw:2,0 {PI_CLIP_DIR}/{name}"],
                   capture_output=True, timeout=timeout)
LAN = "http://192.168.1.16:8080/esp32"


def post(url: str, body: dict, timeout: int = 25) -> dict:
    proc = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-X", "POST", url,
         "-H", "Content-Type: application/json", "-d", json.dumps(body)],
        capture_output=True, text=True)
    try:
        return json.loads(proc.stdout)
    except Exception:
        return {}


def device_state() -> dict:
    proc = subprocess.run(
        ["curl", "-s", "-m", "20", "https://www.wangyutang.cn/devices/api/list"],
        capture_output=True, text=True)
    try:
        payload = json.loads(proc.stdout)
    except Exception:
        return {}
    for dev in (payload.get("devices") or payload):
        if dev.get("device_id") == "esp32-s3-walle":
            return dev.get("state", {})
    return {}


def command_ack(command_id: str, timeout_s: float = 45) -> str | None:
    """Wait for the device's own acknowledgement of a command.

    Reads the command record, not the log list. Two reasons the log was the
    wrong place to look:

    * Only commands the firmware rejects produce a log line ("status=unsupported
      ack=ESP_OK"). A command that succeeds puts its answer in the record's
      `message` field -- click_arm replies "armed", click_result replies with
      the latency -- and never appears in the log at all. Seven E4 runs were
      discarded as "click_arm never acknowledged" while every one of them had in
      fact completed in about 150ms.
    * The log list is capped at 200 entries, so a chatty run pushes older acks
      out from under a reader that is still waiting for one.

    The record carries status, message and timestamps, and is keyed by id, so it
    answers the question directly instead of by substring search.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        proc = subprocess.run(["curl", "-s", "-m", "20", HUB],
                              capture_output=True, text=True)
        try:
            device = json.loads(proc.stdout).get("device", {})
        except Exception:
            device = {}
        for cmd in device.get("commands") or []:
            if cmd.get("id") == command_id and \
                    cmd.get("status") not in (None, "dispatching", "pending",
                                              "sent", "dispatched"):
                # Return the message when there is one; some acks carry only a
                # status, and callers regex the string for "latency=N ms".
                return cmd.get("message") or cmd.get("status") or ""
        # Fall back to the log for anything that only surfaces there.
        for entry in device.get("logs") or []:
            msg = entry.get("message", "")
            if command_id in msg:
                return msg
        time.sleep(2)
    return None


def wait_until_playing(timeout_s: float = 40) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if device_state().get("audio_playing"):
            return True
        time.sleep(3)
    return False


def one_run(assistant: str, user: str, volume: int, pi_volume: int) -> dict:
    result = {"ok": False, "reason": "", "latency_ms": None}

    play = post(f"{HUB}/play_lan_audio",
                {"params": {"name": assistant},
                 "settings_override": {"voice_volume_percent": volume}})
    play_id = play.get("command_id", "")
    if not play.get("ok"):
        result["reason"] = "play command rejected"
        return result

    # The robot must actually be speaking, or there is nothing to interrupt and
    # the run says nothing about latency.
    if not wait_until_playing():
        result["reason"] = "robot never started playing (command not delivered)"
        return result

    # Past the onset grace and far enough in that the AEC has converged.
    time.sleep(4)

    arm = post(f"{HUB}/command", {"action": "click_arm"})
    arm_id = arm.get("command_id", "")
    if not command_ack(arm_id, timeout_s=20):
        result["reason"] = "click_arm never acknowledged"
        return result

    # Waiting for that acknowledgement can take tens of seconds, and the clip
    # is finite: six runs were thrown away because the robot had already
    # finished speaking by the time the Pi opened its mouth, which reads
    # identically to a barge-in failure. Confirm the robot is still talking
    # immediately before the interruption, or the run proves nothing.
    if not device_state().get("audio_playing"):
        result["reason"] = "clip ended before the interruption (arm ack was slow)"
        return result

    pi_play(user)

    time.sleep(12)
    res = post(f"{HUB}/command", {"action": "click_result"})
    ack = command_ack(res.get("command_id", ""))
    if not ack:
        result["reason"] = "click_result never acknowledged"
        return result

    match = re.search(r"latency=(\d+) ms", ack)
    if match:
        result["ok"] = True
        result["latency_ms"] = int(match.group(1))
        return result
    if "no click detected" in ack:
        result["reason"] = "click never detected (Pi too quiet, or it never played)"
        return result

    # No latency in the acknowledgement does NOT mean the barge-in failed.
    # click_result pairs s_click_us with s_click_duck_us, so it can only report
    # a latency when the click was recognised BEFORE the barge-in fired. The
    # barge-in goes on the first qualifying speech frame while the click
    # detector must first satisfy an envelope ratio, so the barge-in routinely
    # wins and the turn is cut off with no timestamp to pair.
    #
    # Reporting those as failures is what produced a 42% "trigger rate" for a
    # detector that actually fires every time. Ask the player instead: a turn
    # that ended early as failed|stage=wav_playback was stopped.
    for c in (json.loads(subprocess.run(
            ["curl", "-s", "-m", "20", HUB], capture_output=True,
            text=True).stdout or "{}").get("device", {}).get("commands") or []):
        if c.get("id") == play_id:
            if c.get("status") == "failed" and "wav_playback" in str(c.get("message")):
                result["fired_untimed"] = True
                result["reason"] = "触发了但咔哒没赶上，无法计时"
                return result
    result["reason"] = "真·漏触发（播放未被打断）: " + ack[-50:]
    return result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--assistant", default="turn10_assistant.wav")  # 37.92s, the longest
    ap.add_argument("--user", default="turn05_user_click.wav")
    ap.add_argument("--volume", type=int, default=80)
    ap.add_argument("--pi-volume", type=int, default=100)
    args = ap.parse_args()

    good, bad, untimed = [], [], 0
    for i in range(1, args.runs + 1):
        out = one_run(args.assistant, args.user, args.volume, args.pi_volume)
        if out["ok"]:
            good.append(out["latency_ms"])
            print(f"run {i}: latency = {out['latency_ms']} ms")
        elif out.get("fired_untimed"):
            untimed += 1
            print(f"run {i}: 触发（未计时） — {out['reason']}")
        else:
            bad.append(out["reason"])
            print(f"run {i}: 作废 — {out['reason']}")
        # End the clip, and WAIT for the device to acknowledge it. Commands are
        # published retained (server.py publishes with retain=True and clears
        # the flag on acceptance); firing one and walking away leaves it on the
        # broker to replay at every reconnect. A backlog built that way killed
        # four consecutive full-scope E5 runs before anyone read the acks --
        # twelve stale stops arriving in one burst. Waiting for the ack is what
        # gets the retained flag cleared.
        stop_id = post(f"{HUB}/command", {"action": "stop_audio"}).get("command_id", "")
        if stop_id:
            command_ack(stop_id, timeout_s=30)
        time.sleep(6)

    print()
    fired = len(good) + untimed
    print(f"打断触发 {fired}/{args.runs}（其中 {len(good)} 次配上了咔哒可计时，"
          f"{untimed} 次打断跑赢咔哒、无法计时）")
    if bad:
        print(f"真·漏触发 {len(bad)} 次：")
        for reason in bad:
            print("  -", reason)
    if good:
        good.sort()
        print(f"\n可计时样本 {len(good)}: {good}")
        print(f"中位数 {good[len(good)//2]} ms  最小 {good[0]} ms  最大 {good[-1]} ms")
        print(f"判据 <200ms 及格 / <120ms 好 -> "
              f"{'及格' if good[len(good)//2] < 200 else '不及格'}")
        # The untimed runs are not a hidden slow tail: they are the runs where
        # the barge-in was FASTER than the click detector, so if anything they
        # sit below this median rather than above it.
        print("（未计时的那些是打断更快、咔哒没跟上，不是被藏起来的慢样本）")
    else:
        print("没有可计时样本")


if __name__ == "__main__":
    main()
