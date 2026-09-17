#!/usr/bin/env python3
"""Catch a false barge-in in the act and read the gate counters.

E5 started failing at about one cut per 127s run after passing three runs in a
row on earlier firmware. Knowing that playback stopped says nothing about why,
and the room measured quiet immediately afterwards, so guessing "probably
noise" would be exactly the move that has cost this project a day already.

click_arm resets the gate counters, so arming at the start of each turn and
reading click_result after a cut gives the state at the moment it fired:

    frames    frames examined while playing and past the onset grace
    loud      of those, clean_rms >= AEC_BARGEIN_MIN_RMS
    speech    of those, the VAD called it speech
    both      both at once -- the barge-in fires on the first of these
    max_rms   the loudest AFE output frame in the window

A cut with max_rms far above the threshold means the AFE really did emit
something speech-like: either residual echo got through, or there was a sound
in the room. A cut with max_rms near the threshold means it only just crossed.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
TURNS = [("turn02_assistant.wav", 7920), ("turn04_assistant.wav", 27200),
         ("turn06_assistant.wav", 27280), ("turn08_assistant.wav", 27200),
         ("turn10_assistant.wav", 37920)]


def post(body, path="/command"):
    p = subprocess.run(
        ["curl", "-s", "-m", "25", "-X", "POST", HUB + path,
         "-H", "Content-Type: application/json", "-d", json.dumps(body)],
        capture_output=True, text=True)
    try:
        return json.loads(p.stdout)
    except Exception:
        return {}


def device():
    p = subprocess.run(["curl", "-s", "-m", "20", HUB], capture_output=True, text=True)
    try:
        return json.loads(p.stdout).get("device", {})
    except Exception:
        return {}


def ack(cid, wait=30):
    end = time.time() + wait
    while time.time() < end:
        for c in device().get("commands") or []:
            if c.get("id") == cid and c.get("status") in ("done", "failed", "unsupported"):
                return c.get("message") or ""
        time.sleep(1.5)
    return ""


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    # e5_full_run plays the turns back to back; this script stopped and waited
    # 4s between them and saw zero cuts in 10 clips while e5_full_run cut one
    # per run, twice. That gap is the one difference worth isolating, so it is
    # switchable rather than assumed irrelevant.
    back_to_back = "--back-to-back" in sys.argv
    cuts = 0
    played = 0
    maxima = []
    for r in range(rounds):
        print(f"\n=== 第 {r+1}/{rounds} 轮 ===")
        for name, expect in TURNS:
            if not back_to_back:
                post({"action": "stop_audio"})
                time.sleep(4)
            cid = post({"params": {"name": name},
                        "settings_override": {"voice_volume_percent": 80}},
                       "/play_lan_audio").get("command_id")
            # Wait for it to be speaking, then arm so the counters cover this
            # turn only.
            end = time.time() + 30
            while time.time() < end and not device().get("state", {}).get("audio_playing"):
                time.sleep(1)
            ack(post({"action": "click_arm"}).get("command_id", ""), wait=20)

            end = time.time() + expect / 1000 + 40
            status = None
            while time.time() < end:
                for c in device().get("commands") or []:
                    if c.get("id") == cid and c.get("status") in ("done", "failed"):
                        status = c
                        break
                if status:
                    break
                time.sleep(2)
            played += 1
            # Read the counters on EVERY turn, not just cut ones.
            #
            # The offline distribution from the archived captures tops out at a
            # per-frame RMS of 51 with the robot alone, while live cuts report
            # 151 and 1073. The captures are not wrong -- they total 83s and the
            # event happens about once per 1200s, so catching one was a 7%
            # proposition. The tail is the whole question and the captures have
            # none of it.
            #
            # These counters need no capture, no upload and have no 30s ceiling,
            # so every turn contributes its maximum. That is the cheap way to
            # get the tail.
            stats = ack(post({"action": "click_result"}).get("command_id", ""))
            keep = " ".join(w for w in stats.split() if w.split("=")[0] in
                            ("frames", "loud", "speech", "both", "max_rms"))
            mx = next((int(w.split("=")[1]) for w in stats.split()
                       if w.startswith("max_rms=")), None)
            if mx is not None:
                maxima.append(mx)
            if status and status.get("status") == "failed":
                cuts += 1
                if not keep:
                    # Older firmware dropped the counters on the no-click path,
                    # which is every E5 cut since the Pi is silent by
                    # definition. Show the raw reply so a missing figure is
                    # visibly missing rather than an empty line.
                    keep = f"(原始回执: {stats[:90]})"
                print(f"  {name}: **被切断**  {status.get('message','')[:40]}")
                print(f"      门限归因: {keep}")
            else:
                print(f"  {name}: 完整   max_rms={mx}")
    print(f"\n{played} 段里被切 {cuts} 段")
    # Accumulate across soaks. A single soak's largest draw is not the
    # distribution's upper bound -- one sample topped out at 94 and the next at
    # 140 against a gate of 150, and the 94 had been used to claim 1.6x of
    # margin. Only a tail built from many soaks can answer where the gate
    # belongs.
    import datetime
    with open("docs/logs/e5_maxima.jsonl", "a") as f:
        f.write(json.dumps({
            "at": datetime.datetime.now().isoformat(timespec="seconds"),
            "clips": played, "cuts": cuts, "maxima": maxima}) + "\n")
    if maxima:
        maxima.sort()
        n = len(maxima)
        def pc(q):
            return maxima[min(n - 1, int(q * n / 100))]
        print(f"每轮 max_rms 分布（n={n}）：最小 {maxima[0]}  中位 {pc(50)}  "
              f"p90 {pc(90)}  最大 {maxima[-1]}")
        over = sum(1 for m in maxima if m >= 40)
        print(f"  其中 {over}/{n} 轮的峰值越过了当前门限 40")
        print(f"  全部: {maxima}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
