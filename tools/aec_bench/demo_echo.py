#!/usr/bin/env python3
"""Drive the echo_demo turn and narrate what the device reports.

The device runs the whole interaction itself (echo_demo). This script only
plays the part of the person who interrupts, and watches.

Timing: the Pi must start speaking while the robot is mid-sentence and past the
1500ms onset grace, so it waits for playback to actually be observed rather
than sleeping a fixed amount after issuing the command.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
TASKS = "https://www.wangyutang.cn/action/api/tasks"


def post(url, body, timeout=25):
    p = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-X", "POST", url,
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


def find(cmd_id):
    for c in device().get("commands") or []:
        if c.get("id") == cmd_id:
            return c
    return {}


def main():
    # --live: a person in the room is the interrupter, so nothing is cued to
    # the Pi. They hear the robot start talking, which is the only cue needed.
    live = "--live" in sys.argv
    args_rest = [a for a in sys.argv[1:] if not a.startswith("--")]
    interrupter = args_rest[0] if args_rest else "turn05_user.wav"
    print("回声演示 · 设备自己跑完整个回合\n")
    print("  ① 等房间安静 5s")
    print("  ② 机器人播 turn06（27.28s）")
    print("  ③ 有人插话 -> 立即停播")
    print("  ④ 录下插话者，直到他说完")
    print("  ⑤ 把录到的原话播出来\n")

    before_ids = {c.get("id") for c in (device().get("commands") or [])
                  if c.get("action") == "play_audio"}
    cid = post(f"{HUB}/command", {
        "action": "echo_demo",
        "args": {"quiet_ms": 5000, "end_silence_ms": 800,
                 "max_record_s": 15, "wait_s": 90, "volume": 80},
    }).get("command_id")
    if not cid:
        print("echo_demo 未被接受")
        return 1
    print(f"  已下发 echo_demo ({cid})，设备开始等待安静 ...")

    # Wait for the robot to actually be speaking before cueing the interrupter.
    t0 = time.time()
    speaking = False
    while time.time() - t0 < 120:
        if device().get("state", {}).get("audio_playing"):
            speaking = True
            print(f"  +{time.time()-t0:5.1f}s  🤖 机器人开口了")
            break
        if find(cid).get("status") in ("done", "failed"):
            break
        time.sleep(1)
    if not speaking:
        print(f"  机器人始终没开口：{find(cid).get('message')}")
        return 1

    if live:
        print(f"  +{time.time()-t0:5.1f}s  🧑 等你开口（听到机器人说话后停一两秒再说）")
    else:
        # Past the onset grace, then interrupt.
        time.sleep(4)
        print(f"  +{time.time()-t0:5.1f}s  🧑 插话：{interrupter}")
        post(TASKS, {"action": "play_local_audio",
                     "params": {"name": interrupter},
                     "settings_override": {"voice_volume_percent": 80}})

    # The device reports the whole turn in one acknowledgement when it is done.
    while time.time() - t0 < 180:
        rec = find(cid)
        if rec.get("status") in ("done", "failed"):
            print(f"\n  设备报告：{rec.get('status')} | {rec.get('message')}")
            break
        time.sleep(2)
    else:
        print("\n  超时，设备未给出结果")
        return 1

    # The upload asks the hub to play the recording back, so a play_audio for
    # the echo_*.wav should follow on its own. Watch for it.
    print("\n  等待回放（hub 应自动下发 play_audio）...")
    # Twice this reported a working demo as broken, so it is worth stating what
    # the replay command actually looks like. device_hub stores the upload under
    # a fresh random filename (same as the TTS path), so the echo_*.wav name the
    # device chose never appears anywhere -- matching on it found nothing. And
    # the upload happens INSIDE echo_demo, so the replay is created before that
    # command finishes; matching on "created after done_at" also found nothing.
    #
    # The durable identity is simply "a play_audio that was not there when this
    # run started", so the ids present beforehand are recorded and anything new
    # is the replay.
    t1 = time.time()
    while time.time() - t1 < 90:
        for c in reversed(device().get("commands") or []):
            if c.get("action") == "play_audio" and c.get("id") not in before_ids:
                url = str((c.get("args") or {}).get("url", ""))
                print(f"  🔊 回放：{url.split('/')[-1]}  status={c.get('status')}")
                return 0
        time.sleep(3)
    print("  没等到回放命令")
    return 1


if __name__ == "__main__":
    sys.exit(main())
