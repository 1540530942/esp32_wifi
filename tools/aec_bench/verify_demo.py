#!/usr/bin/env python3
"""Verify an alpha or beta demo session from all three evidence sources.

Nothing here trusts one source. Every measurement failure in this work has been
the device's account and what actually happened disagreeing with nobody
checking, so the three are collected and compared:

  device self-report  what it believed happened (spoke/rec/pre/gap/peak/gain)
  uploaded recording  what the AFE captured
  room recording      what actually came out of the speaker, from a microphone
                      that is not the device's own

For beta there is a fourth, and it is the decisive one. The recording is made
while the robot is speaking, so the raw microphone and the AFE output for the
same window are both uploaded. Transcribing the pair should read the ROBOT from
the microphone and the PERSON from the AFE output. A clean replay on its own
proves nothing there -- it is equally consistent with the robot being quiet.

    python3 verify_demo.py <room_recording.wav> [--beta]
"""
import json
import subprocess
import sys
import wave

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
AUDIO = "https://www.wangyutang.cn/devices/api/audio"
ASR = "tools/aec_bench/asr_check.py"


def sh(cmd, timeout=180):
    return subprocess.run(cmd, shell=True, capture_output=True,
                          text=True, timeout=timeout).stdout.strip()


def hub_files(prefix, limit):
    """Newest uploads whose stored name we cannot predict -- the hub renames
    them -- so they are listed by mtime from inside the container."""
    out = sh("ssh tang 'sudo docker exec device-hub sh -c \""
             f"ls -t /app/data/audio/*.wav | head -{limit}\"'")
    return [l.strip().split("/")[-1] for l in out.splitlines() if l.strip()]


def fetch(name, dest):
    sh(f"curl -s -m 60 -o {dest} {AUDIO}/{name}")
    try:
        with wave.open(dest) as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return 0.0


def transcribe(paths):
    if not paths:
        return {}
    out = sh(f"timeout 600 python3 {ASR} " + " ".join(paths), timeout=700)
    res = {}
    for line in out.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            res[k.strip()] = v.strip()
    return res


def main():
    room = sys.argv[1] if len(sys.argv) > 1 else "echo_session.wav"
    beta = "--beta" in sys.argv

    print("=== 1. 设备自述 ===")
    dev = json.loads(sh(f"curl -s -m 20 {HUB}") or "{}").get("device", {})
    st = dev.get("state", {})
    print(f"  轮次={st.get('echo_cycles')} 回声={st.get('echo_replies')} "
          f"循环中={st.get('echo_loop')}")
    print(f"  最近一轮: {st.get('echo_last')}")

    print("\n=== 2. 上传的录音（AFE 输出 = 它录到了什么）===")
    n = 6 if not beta else 8
    names = hub_files("echo", n)
    paths = []
    for nm in names:
        dest = f"/tmp/vd_{nm}"
        dur = fetch(nm, dest)
        if dur > 0:
            paths.append(dest)
            print(f"  {nm}  {dur:.2f}s")

    res = transcribe(paths)
    for k, v in res.items():
        print(f"  {k.split('/')[-1]}: {v}")

    if beta:
        print("\n  判据（beta）：clean 通道应转写出【人】的话；")
        print("              mic 通道应转写出【机器人】的话（turn06 的内容）。")
        print("              若 clean 里读出机器人 -> AEC 在持续双讲下不够。")
    else:
        print("\n  判据（alpha）：应转写出说话人的原话，且【开头完整】")
        print("               （开头缺字 -> pre-roll 不够长，看 pre= 的值）。")

    print("\n=== 3. 房间录音（喇叭实际发出了什么）===")
    try:
        with wave.open(room) as w:
            print(f"  {room}: {w.getnframes()/w.getframerate():.1f}s")
        print("  -> 用 gate_distribution 的方式逐秒看电平，回放段应与机器人自己")
        print("     的话在同一量级（同一段录音内比较，音量/距离/增益自动抵消）。")
    except Exception as exc:
        print(f"  无法读取 {room}: {exc}")
        print("  -> 没有房间录音就无法回答『到底出没出声』，这正是昨天"
              "『命令报成功却一声没出』被漏掉的原因。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
