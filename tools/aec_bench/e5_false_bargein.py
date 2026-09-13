#!/usr/bin/env python3
"""E5 — 误打断率：ESP32 连播 5 个 assistant 轮次，房间安静，统计误触发。

判据：被自己的残余回声触发停播 0 次 / 约 127 秒。

v40 之前这个实验跑不了：打断检测器门控在 playback_is_playing() 上，而它只反映
playback.c 的 TTS 队列，play_lan_audio 走的 AudioPlayer 从不注册，检测器压根不
评估——那时跑出来的"0 次"是假通过。v40 把生产播放路径接进去之后才有意义。

同时检查每段是否被提前切断：v40 起，误触发的后果是**停播**而不只是压低音量，
所以一次误触发会让该段的实际播放时长明显短于音频本身。
"""
import json, re, subprocess, sys, time
from pathlib import Path

HUB = "http://110.40.154.41/devices/api"
DEVICE = "esp32-s3-walle"
BASE = "http://192.168.1.16:8080/esp32"
CLIPS = [("turn02_assistant.wav", 7.92), ("turn04_assistant.wav", 27.20),
         ("turn06_assistant.wav", 27.28), ("turn08_assistant.wav", 27.20),
         ("turn10_assistant.wav", 37.92)]
SERIAL_LOG = "/tmp/e5_serial.log"


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, text=True, capture_output=True,
                          timeout=timeout).stdout


def play(name, volume):
    body = json.dumps({"action": "play_audio",
                       "args": {"url": f"{BASE}/{name}", "volume": volume}})
    out = sh(f"curl -s -m 15 -X POST '{HUB}/device/{DEVICE}/command' "
             f"-H 'Content-Type: application/json' -d '{body}'")
    return json.loads(out)["command_id"]


def wait_done(cmd_id, limit_s):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        time.sleep(3)
        d = json.loads(sh(f"curl -s -m 10 '{HUB}/device/{DEVICE}'"))
        for c in d["device"].get("commands", []):
            if c.get("id") == cmd_id and c.get("status") in ("done", "failed"):
                span = (c.get("done_at", 0) or 0) - (c.get("dispatched_at", 0) or 0)
                return c["status"], span, (c.get("message") or "")
    return "timeout", 0.0, ""


def main():
    volume = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    # 后台抓串口：打断触发时固件会打印 "local barge-in -> duck+stop at t=..."
    sh(f"ssh -o ConnectTimeout=10 spark \"pkill -f 'e5_serial_capture' ; true\"")
    cap = subprocess.Popen(
        ["ssh", "-o", "ConnectTimeout=10", "spark",
         f"python3 -u -c \"import serial,time,sys\n"
         f"ser=serial.Serial('/dev/ttyACM0',115200,timeout=1)\n"
         f"t0=time.time()\n"
         f"f=open('{SERIAL_LOG}','w')\n"
         f"while time.time()-t0<400:\n"
         f"    d=ser.read(4000)\n"
         f"    if d: f.write(d.decode('utf-8',errors='replace')); f.flush()\n"
         f"ser.close(); f.close()\" # e5_serial_capture"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)

    print(f"== E5 误打断率 (volume={volume}, 房间应保持安静) ==")
    results = []
    for name, dur in CLIPS:
        cid = play(name, volume)
        st, span, msg = wait_done(cid, dur + 90)
        cut = span > 0 and span < dur * 0.8
        print(f"  {name:26s} 音频{dur:6.2f}s 实播{span:6.2f}s {st:8s}"
              f"{'  ← 疑似被切断' if cut else ''}")
        results.append((name, dur, span, st, cut))
        time.sleep(2)

    time.sleep(3)
    cap.terminate()
    log = sh(f"ssh -o ConnectTimeout=10 spark \"cat {SERIAL_LOG}\"", timeout=60)
    triggers = re.findall(r"local barge-in -> duck\+stop at t=(\d+)", log)
    total_audio = sum(d for _, d in CLIPS)

    print()
    print(f"总音频时长 : {total_audio:.2f}s")
    print(f"误打断次数 : {len(triggers)}")
    for t in triggers:
        print(f"    trigger at device t={int(t)/1e6:.3f}s")
    cuts = [r for r in results if r[4]]
    print(f"被切断的片段: {len(cuts)}")
    print()
    print("E5 " + ("通过（0 次误打断）" if len(triggers) == 0 and not cuts
                   else f"不通过（{len(triggers)} 次误触发 / {len(cuts)} 段被切断）"))


if __name__ == "__main__":
    main()
