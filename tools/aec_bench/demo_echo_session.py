#!/usr/bin/env python3
"""Run the echo loop for a while and record the whole room on the Pi.

The device drives the interaction by itself; this starts it, watches progress
through the heartbeat, and stops it. The Pi records continuously from its USB
microphone for the whole session so the result can be listened to end to end --
the loop's own evidence (cycle counts, per-turn acknowledgements) says what the
device believed happened, and the room recording says what actually came out of
the speaker. Keeping both is the point: every measurement bug in this work so
far has been a case of the two disagreeing with nobody checking.

    python3 demo_echo_session.py [seconds]
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
PI_WAV = "/tmp/echo_session.wav"


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


def sh(host, cmd, timeout=120):
    return subprocess.run(["ssh", host, cmd], capture_output=True, text=True,
                          timeout=timeout).stdout.strip()


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    # 10s of silence before the robot speaks again, not 5.
    #
    # It is a stronger assertion than it looks. The robot must not be tripped
    # into starting by the tail of its own replay, and a person must have time
    # to finish a turn before it talks over them. A shorter gate lets both slip
    # through and the loop still "works", which is exactly the kind of pass
    # that means nothing.
    quiet_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
    print(f"回声循环会话 · {secs}s，静音门槛 {quiet_ms}ms，树莓派全程录音\n")

    before = {c.get("id") for c in (device().get("commands") or [])
              if c.get("action") == "play_audio"}

    # Pi records the room for the whole session, a little longer than the loop
    # so the last replay cannot fall off the end.
    # setsid + </dev/null + disown, not plain nohup: the backgrounded arecord is
    # killed when the ssh channel closes, and the only symptom is a recording
    # file that never appears.
    sh("pi", f"rm -f {PI_WAV}; setsid arecord -D plughw:2,0 -f S16_LE -r 16000 "
             f"-c 1 -d {secs + 20} {PI_WAV} </dev/null >/dev/null 2>&1 & disown; "
             f"sleep 1; pgrep -c arecord")
    print("  🎙  树莓派已开始录音")
    t_rec = time.time()
    time.sleep(2)

    r = post(f"{HUB}/command", {
        "action": "echo_demo",
        "args": {"loop": True, "quiet_ms": quiet_ms, "end_silence_ms": 800,
                 # wait_s is the per-cycle patience for the room to go quiet.
                 # With a 10s gate it must be well above 10s or a single cough
                 # ends the cycle as "never quiet".
                 "max_record_s": 15, "wait_s": 180, "volume": 80},
    })
    print(f"  ▶  循环已启动 ({r.get('command_id')})\n")

    t0 = time.time()
    last_seen = -1
    while time.time() - t0 < secs:
        st = device().get("state", {})
        cyc = st.get("echo_cycles")
        if cyc != last_seen:
            print(f"  +{time.time()-t0:5.0f}s  轮次={cyc} 回声={st.get('echo_replies')} "
                  f"运行中={st.get('echo_loop')}")
            if st.get("echo_last"):
                print(f"            {st.get('echo_last')}")
            last_seen = cyc
        time.sleep(5)

    print("\n  ⏹  停止循环")
    print("     ", post(f"{HUB}/command", {"action": "echo_stop"}).get("command_id"))
    time.sleep(8)
    st = device().get("state", {})
    print(f"     最终：轮次={st.get('echo_cycles')} 回声={st.get('echo_replies')}")

    # Let arecord reach its own -d limit and close the file cleanly. Killing it
    # early leaves a truncated header that wave cannot open at all, which loses
    # the entire recording rather than its last few seconds.
    remain = (secs + 20) - (time.time() - t_rec) + 5
    if remain > 0:
        print(f"\n  等录音自然结束 {remain:.0f}s ...")
        time.sleep(remain)
    size = sh("pi", f"stat -c %s {PI_WAV} 2>/dev/null || echo 0")
    print(f"  树莓派录音 {size} bytes")
    subprocess.run(["scp", f"pi:{PI_WAV}", "./echo_session.wav"],
                   capture_output=True, timeout=600)
    out = subprocess.run(
        ["python3", "-c",
         "import wave,audioop,math;w=wave.open('echo_session.wav');"
         "d=w.readframes(w.getnframes());"
         "print(f'  房间录音 {w.getnframes()/w.getframerate():.1f}s  "
         "峰值 {audioop.max(d,2)}  RMS {audioop.rms(d,2)}')"],
        capture_output=True, text=True)
    print(out.stdout.strip() or out.stderr.strip()[:200])

    news = [c for c in (device().get("commands") or [])
            if c.get("action") == "play_audio" and c.get("id") not in before]
    print(f"\n  本次产生的回放 {len(news)} 条：")
    for c in news:
        print(f"    {str((c.get('args') or {}).get('url','')).split('/')[-1]}  "
              f"{c.get('status')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
