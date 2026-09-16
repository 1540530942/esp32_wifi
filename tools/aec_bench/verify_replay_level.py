#!/usr/bin/env python3
"""Prove the echo replay actually comes out of the speaker, and how loud.

The Pi does double duty: it plays the part of the person who interrupts, and it
records the room throughout. The room recording is the witness -- the device
reporting "replayed=1" only means it wrote samples to the codec, and the whole
reason this check exists is that a replay once reported success while producing
no sound at all.

Two things this gets right that the first version did not:

* The interruption is played over ssh with aplay, which blocks for exactly the
  clip so the window is known. (An earlier version of this file claimed the
  task queue took 8 seconds from cue to sound; measured properly it is 1.4s.
  The first run of this check did fail because the interruption landed after
  the robot had already been stopped by something else, but the cause was room
  noise stopping the robot early, not the transport.)
* The verdict compares what happens after the interruption against the robot's
  own turn in the SAME recording, which cancels speaker volume, distance and
  microphone gain.

Ordering in a good recording: robot speaks, Pi interrupts, robot falls silent,
replay comes back.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
PI_WAV = "/tmp/replay_check.wav"
PI_CLIP = "/home/pi/action_move/local_audio/turn05_user.wav"
OUT = "replay_check.wav"
TOTAL = 60
LEAD_S = 6.0          # how long the robot speaks before being interrupted


def post(body, timeout=25):
    p = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-X", "POST", f"{HUB}/command",
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
    # Clear the decks: a cycle started while the speaker is still busy fails at
    # speak_start, and the leftover playback looks exactly like the robot
    # taking its turn.
    post({"action": "stop_audio"})
    for _ in range(15):
        if not device().get("state", {}).get("audio_playing"):
            break
        time.sleep(2)
    time.sleep(2)

    # setsid + </dev/null + disown: a plain nohup'd arecord is killed when the
    # ssh channel closes, and the only symptom is a file that never appears.
    sh(f"rm -f {PI_WAV}; setsid arecord -D plughw:2,0 -f S16_LE -r 16000 -c 1 "
       f"-d {TOTAL} {PI_WAV} </dev/null >/dev/null 2>&1 & disown; sleep 1; echo ok")
    print(f"  🎙  树莓派录音 {TOTAL}s")
    t_rec = time.time()
    time.sleep(2)

    cid = post({"action": "echo_demo",
                "args": {"quiet_ms": 2000, "end_silence_ms": 800,
                         "max_record_s": 12, "wait_s": 40, "volume": 80}}
               ).get("command_id")
    t0 = time.time()
    print(f"  ▶  echo_demo {cid}")

    # A fixed lead rather than waiting on audio_playing: that field rides the
    # heartbeat and can be 5s stale, which is the same order as the window being
    # aimed at. The device's own "spoke=" figure afterwards says whether the aim
    # was good -- it should land near LEAD_S.
    time.sleep(LEAD_S)
    t_pi = time.time()
    print(f"  🧑 +{t_pi-t0:4.1f}s 树莓派插话（aplay 直发）")
    sh(f"aplay -D plughw:2,0 {PI_CLIP}", timeout=30)
    print(f"      插话结束 +{time.time()-t0:4.1f}s")

    while time.time() - t0 < 90:
        rec = find(cid)
        if rec.get("status") in ("done", "failed"):
            print(f"\n  设备：{rec.get('status')} | {rec.get('message')}")
            break
        time.sleep(2)

    remain = TOTAL - (time.time() - t_rec) + 5
    if remain > 0:
        print(f"  等录音自然结束 {remain:.0f}s ...")
        time.sleep(remain)
    subprocess.run(["scp", f"pi:{PI_WAV}", OUT], capture_output=True, timeout=300)

    pi_at = t_pi - t_rec
    print(f"\n  房间录音（插话约在 {pi_at:.0f}s 处，时长 4.1s）：")
    subprocess.run(["python3", "-c", f"""
import wave, audioop, math
w = wave.open({OUT!r}); sr = w.getframerate()
rows = []
for s in range(w.getnframes() // sr):
    d = w.readframes(sr)
    rows.append((s, 20*math.log10(max(audioop.rms(d,2),1)/32768)))
pi_at = {pi_at:.0f}
for s, db in rows:
    mark = ' <- 插话' if pi_at <= s <= pi_at + 4 else ''
    bar = '#' * min(40, max(0, int((db + 60) / 1.5)))
    print(f'    {{s:3d}}s  {{db:6.1f}} dBFS  {{bar}}{{mark}}')
# Thresholds relative to the room's own floor, not absolutes. A fixed
# -25 dBFS line missed both the robot's turn (-29.6) and the replay
# (-27.4) while catching only the Pi, whose speaker sits against its own
# microphone -- so the first run of this printed "no robot segment found"
# and pointed at the Pi's tail as the replay.
floor = sorted(db for _, db in rows)[len(rows) // 4]      # quiet quartile
gate = floor + 12
pi_end = pi_at + 4.1 + 1.0        # clip length, plus a second of slack
robot = [(s, db) for s, db in rows if db > gate and s < pi_at]
replay = [(s, db) for s, db in rows if db > gate and s > pi_end]
print(f'  房间底噪 {floor:.1f} dBFS，判定门限 {gate:.1f} dBFS')
if robot:
    print(f'  机器人说话 {[s for s,_ in robot]}s  最大 {max(db for _,db in robot):.1f} dBFS')
else:
    print('  没找到机器人说话段 —— 这一轮无效')
if replay:
    rmax = max(db for _, db in replay)
    print(f'  回放     {[s for s,_ in replay]}s  最大 {rmax:.1f} dBFS')
    if robot:
        print(f'  -> 回放出声了，比机器人自己的话 {rmax - max(db for _,db in robot):+.1f} dB')
    else:
        print('  -> 回放出声了')
else:
    print('  插话结束后没有任何声音 -> 回放没有出声')
"""])
    return 0


if __name__ == "__main__":
    sys.exit(main())
