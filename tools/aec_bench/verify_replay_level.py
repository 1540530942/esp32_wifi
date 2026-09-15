#!/usr/bin/env python3
"""Prove whether the echo replay is actually audible in the room.

The user reported hearing nothing on replay. The device said the command
completed and the uploaded file had content, so "it played" and "it could be
heard" had to be separated -- the only thing that settles it is a microphone in
the room that is not the device's own.

The Pi records the whole cycle while also playing the part of the interrupter.
Afterwards the recording contains, in order: the robot's turn, the Pi's
interruption, then the replay. Comparing the replay's level against the robot's
own turn in the SAME recording removes every variable that a level measured on
the file alone leaves open -- speaker volume, distance, mic gain.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
TASKS = "https://www.wangyutang.cn/action/api/tasks"
PI_WAV = "/tmp/replay_check.wav"
OUT = "replay_check.wav"


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


def find(cid):
    for c in device().get("commands") or []:
        if c.get("id") == cid:
            return c
    return {}


def sh(cmd, timeout=180):
    return subprocess.run(["ssh", "pi", cmd], capture_output=True,
                          text=True, timeout=timeout).stdout.strip()


def main():
    total = 70
    before = {c.get("id") for c in (device().get("commands") or [])
              if c.get("action") == "play_audio"}

    # Clear the decks first. A cycle that starts while something is still
    # coming out of the speaker fails at speak_start (the player is busy), and
    # the leftover playback looks exactly like the robot taking its turn.
    post(f"{HUB}/command", {"action": "stop_audio"})
    for _ in range(20):
        if not device().get("state", {}).get("audio_playing"):
            break
        time.sleep(2)
    else:
        print("  ⚠ 播放器一直忙，继续可能失败")
    time.sleep(3)

    # setsid + </dev/null + disown, not plain nohup: the backgrounded arecord
    # was being killed when the ssh channel closed, and the only symptom was a
    # recording file that never appeared.
    sh(f"rm -f {PI_WAV}; setsid arecord -D plughw:2,0 -f S16_LE -r 16000 -c 1 "
       f"-d {total} {PI_WAV} </dev/null >/dev/null 2>&1 & disown; sleep 1; "
       f"pgrep -c arecord")
    print(f"  🎙  树莓派录音 {total}s")
    time.sleep(2)

    cid = post(f"{HUB}/command", {
        "action": "echo_demo",
        "args": {"quiet_ms": 3000, "end_silence_ms": 800, "max_record_s": 12,
                 "wait_s": 40, "volume": 80},
    }).get("command_id")
    print(f"  ▶  echo_demo {cid}")

    t0 = time.time()
    while time.time() - t0 < 45:
        if device().get("state", {}).get("audio_playing"):
            print(f"  +{time.time()-t0:4.1f}s 机器人开口")
            break
        time.sleep(1)
    time.sleep(4)
    print(f"  +{time.time()-t0:4.1f}s 树莓派插话")
    post(TASKS, {"action": "play_local_audio",
                 "params": {"name": "turn05_user.wav"},
                 "settings_override": {"voice_volume_percent": 80}})

    while time.time() - t0 < 90:
        rec = find(cid)
        if rec.get("status") in ("done", "failed"):
            print(f"\n  设备：{rec.get('message')}")
            break
        time.sleep(2)

    # Watch the replay actually drive the speaker.
    print("\n  等回放 ...")
    t1 = time.time()
    replay = None
    while time.time() - t1 < 60:
        for c in device().get("commands") or []:
            if c.get("action") == "play_audio" and c.get("id") not in before:
                replay = c
                break
        if replay and replay.get("status") in ("done", "failed"):
            break
        time.sleep(2)
    if replay:
        print(f"  回放命令 status={replay.get('status')} "
              f"msg={replay.get('message')} "
              f"file={str((replay.get('args') or {}).get('url','')).split('/')[-1]}")
    else:
        print("  没有回放命令")

    # Let arecord reach its own -d limit and close the file cleanly; killing it
    # early leaves a truncated header that wave cannot open.
    remain = total - (time.time() - t0) + 6
    if remain > 0:
        print(f"  等录音自然结束 {remain:.0f}s ...")
        time.sleep(remain)
    size = sh(f"stat -c %s {PI_WAV} 2>/dev/null || echo 0")
    print(f"  树莓派录音文件 {size} bytes")
    if size == "0":
        print("  录音文件不存在，无法判读")
        return 1
    subprocess.run(["scp", f"pi:{PI_WAV}", OUT], capture_output=True, timeout=300)

    print("\n  房间录音逐秒电平（树莓派麦克风所听到的）：")
    subprocess.run(["python3", "-c", f"""
import wave, audioop, math
w = wave.open({OUT!r})
sr = w.getframerate()
for s in range(w.getnframes() // sr):
    d = w.readframes(sr)
    r = audioop.rms(d, 2)
    db = 20*math.log10(max(r,1)/32768)
    bar = '#' * min(40, max(0, int((db + 60) / 1.5)))
    print(f'    {{s:3d}}s  {{db:6.1f}} dBFS  {{bar}}')
"""])
    return 0


if __name__ == "__main__":
    sys.exit(main())
