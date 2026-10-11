#!/usr/bin/env python3
"""Alternate the alpha and beta demos, judging each run by ASR.

The Pi plays the part of the person. Every run is scored on content, not on the
device agreeing with itself: the replay is transcribed and compared against a
transcription of the source clip the Pi actually played, through the same ASR,
so the two are directly comparable.

Timing, which is the part that has to be right:

  1. clear the decks and wait for the player to go idle -- a cycle started
     while something is still coming out of the speaker fails at speak_start,
     and the leftover playback looks exactly like the robot taking its turn
  2. confirm the room is quiet BEFORE starting. The robot will not speak until
     it has heard 10s of silence, and a noisy room makes every later number
     meaningless. A run that starts noisy is abandoned, not "explained"
  3. start the Pi recording the room for the whole run
  4. send the cycle, then wait for playback to actually be observed
  5. wait past the onset grace plus the AEC's convergence (measured at 0.5-0.6s)
     before the Pi speaks, or the interruption lands while the filter is still
     settling
  6. the Pi speaks over ssh, blocking for exactly the clip, so the observation
     window is known rather than inferred
  7. read the device's report, fetch the uploads, transcribe, score

alpha: the robot stops when the person speaks; the recording is made in
silence. A clean replay proves the plumbing.

beta: the robot keeps talking; every sample of the recording is taken during
double-talk, and the raw microphone is uploaded alongside. The pair is the
proof -- microphone should read the ROBOT, AFE output should read the PERSON. A
clean replay alone is equally consistent with the robot having been quiet.
"""
import argparse
import datetime
import difflib
import json
import re
import subprocess
import sys
import time
import wave

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
AUDIO = "https://www.wangyutang.cn/devices/api/audio"
PI_DIR = "/home/pi/action_move/local_audio"
PI_WAV = "/tmp/ab_room.wav"
LOG = "docs/logs/ab_session_results.jsonl"
GATE = 150          # AEC_BARGEIN_MIN_RMS; a room above this is not quiet


def sh(cmd, timeout=240):
    return subprocess.run(cmd, shell=True, capture_output=True,
                          text=True, timeout=timeout).stdout.strip()


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


def ack(cid, wait=180):
    end = time.time() + wait
    while time.time() < end:
        for c in device().get("commands") or []:
            if c.get("id") == cid and c.get("status") in ("done", "failed", "unsupported"):
                return c.get("message") or ""
        time.sleep(2)
    return ""


def room_is_quiet(samples=3):
    """The robot waits for 10s of silence; if the room is not quiet the run
    cannot even start, and anything measured in a noisy room is worthless.
    Checked rather than assumed -- a phone call in the room once cost a day of
    chasing a design flaw that did not exist."""
    vals = []
    for _ in range(samples):
        m = ack(post({"action": "aec_erle"}).get("command_id", ""), wait=60)
        d = dict(kv.split("=", 1) for kv in m.split() if "=" in kv)
        try:
            vals.append(float(d.get("idle_clean", 0)))
        except ValueError:
            pass
        time.sleep(3)
    return (max(vals) if vals else 999), (bool(vals) and max(vals) < GATE)


def transcribe(paths):
    if not paths:
        return {}
    out = sh("timeout 700 python3 tools/aec_bench/asr_check.py " + " ".join(paths),
             timeout=800)
    res = {}
    for line in out.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            res[k.strip().split("/")[-1]] = v.strip()
    return res


def strip_punct(s):
    return re.sub(r"[，。、！？；：《》,.!?;:\s]", "", s or "")


def newest_uploads(k):
    out = sh("ssh tang 'sudo docker exec device-hub sh -c \""
             f"ls -t /app/data/audio/*.wav | head -{k}\"'")
    return [l.strip().split("/")[-1] for l in out.splitlines() if l.strip()]


def fetch(name, dest):
    sh(f"curl -s -m 60 -o {dest} {AUDIO}/{name}")
    try:
        with wave.open(dest) as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return 0.0


def one_run(mode, clip, reference, quiet_ms, room_secs):
    """-> dict with the report, the transcription and the verdict."""
    beta = mode == "beta"
    out = {"mode": mode, "clip": clip,
           "at": datetime.datetime.now().isoformat(timespec="seconds")}

    post({"action": "stop_audio"})
    for _ in range(15):
        if not device().get("state", {}).get("audio_playing"):
            break
        time.sleep(2)
    time.sleep(2)

    level, quiet = room_is_quiet()
    out["room_idle_clean"] = level
    if not quiet:
        out["verdict"] = "abandoned"
        out["why"] = f"房间不安静（idle_clean={level:.0f} ≥ {GATE}），本轮作废"
        return out

    # setsid + </dev/null + disown: a plain nohup'd arecord dies with the ssh
    # channel and the only symptom is a recording that never appears.
    sh(f"ssh pi 'rm -f {PI_WAV}; setsid arecord -D plughw:2,0 -f S16_LE -r 16000 "
       f"-c 1 -d {room_secs} {PI_WAV} </dev/null >/dev/null 2>&1 & disown; sleep 1'")
    t_rec = time.time()
    time.sleep(2)

    before = set(newest_uploads(12))
    cid = post({"action": "echo_demo",
                "args": {"quiet_ms": quiet_ms, "end_silence_ms": 800,
                         "max_record_s": 15, "wait_s": 120, "volume": 80,
                         "preroll_ms": 600, "keep_talking": beta}}
               ).get("command_id", "")

    t0 = time.time()
    while time.time() - t0 < quiet_ms / 1000 + 60:
        if device().get("state", {}).get("audio_playing"):
            break
        time.sleep(1)
    else:
        out["verdict"] = "abandoned"
        out["why"] = "机器人始终没开口"
        return out

    # Past the onset grace (1500ms) and the AEC's convergence (0.5-0.6s
    # measured cold), so the interruption does not land on an unsettled filter.
    time.sleep(3.0)
    out["pi_at"] = round(time.time() - t_rec, 1)
    sh(f"ssh pi 'aplay -D plughw:2,0 {PI_DIR}/{clip}'", timeout=60)

    out["report"] = ack(cid, wait=150)

    # Let arecord reach its own -d limit; killing it early leaves a truncated
    # header and loses the whole recording rather than its tail.
    remain = room_secs - (time.time() - t_rec) + 4
    if remain > 0:
        time.sleep(remain)
    sh(f"scp pi:{PI_WAV} ./ab_room_{mode}.wav", timeout=300)

    new = [n for n in newest_uploads(6) if n not in before]
    paths = []
    for n in new[:2]:
        dest = f"/tmp/ab_{mode}_{n}"
        if fetch(n, dest) > 0:
            paths.append(dest)
    out["uploads"] = paths
    tx = transcribe(paths)
    out["asr"] = tx

    # Score on content against the same ASR's reading of the source clip.
    best = 0.0
    for name, text in tx.items():
        r = difflib.SequenceMatcher(None, strip_punct(reference),
                                    strip_punct(text)).ratio()
        best = max(best, r)
    out["similarity"] = round(best, 3)

    if "not_interrupted" in (out.get("report") or ""):
        out["verdict"] = "fail"
        out["why"] = "没有被打断（树莓派的声音没让它触发）"
    elif not tx:
        out["verdict"] = "fail"
        out["why"] = "没有可转写的上传文件"
    elif best >= 0.6:
        out["verdict"] = "pass"
        out["why"] = f"回放内容与原话相似度 {best:.2f}"
    else:
        out["verdict"] = "fail"
        out["why"] = f"回放内容与原话相似度仅 {best:.2f}"

    if beta:
        # The decisive pair: one upload should read the person, the other the
        # robot. Reported rather than folded into the verdict, because deciding
        # which file is which from content alone is exactly the kind of
        # inference that should be looked at, not automated away.
        out["beta_note"] = ("beta：两份上传应分别读出【人】与【机器人】；"
                            "若两份都读出人，说明麦克风通道也没有回声，需查播放是否真在进行")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=4)
    ap.add_argument("--clip", default="turn05_user.wav")
    ap.add_argument("--quiet-ms", type=int, default=10000)
    ap.add_argument("--room-secs", type=int, default=90)
    args = ap.parse_args()

    # Reference transcription of the clip the Pi will actually play, through the
    # same ASR, so "did the replay say the same thing" is a like-for-like
    # comparison rather than a comparison against something I typed.
    sh(f"ssh pi 'cp {PI_DIR}/{args.clip} /tmp/ref.wav'")
    sh(f"scp pi:/tmp/ref.wav ./ab_reference.wav", timeout=120)
    ref = transcribe(["./ab_reference.wav"]).get("ab_reference.wav", "")
    print(f"素材原话（ASR 基准）：{ref}\n")
    if not ref:
        print("拿不到基准转写，无法评分。停止。")
        return 1

    results = []
    for i in range(args.runs):
        mode = "alpha" if i % 2 == 0 else "beta"
        print(f"=== 第 {i+1}/{args.runs} 轮 · {mode} ===")
        r = one_run(mode, args.clip, ref, args.quiet_ms, args.room_secs)
        results.append(r)
        print(f"  设备: {r.get('report','')}")
        for k, v in (r.get("asr") or {}).items():
            print(f"  ASR {k}: {v}")
        print(f"  -> {r['verdict']}  {r.get('why','')}")
        with open(LOG, "a") as f:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
        time.sleep(8)

    ok = sum(1 for r in results if r["verdict"] == "pass")
    ab = sum(1 for r in results if r["verdict"] == "abandoned")
    print(f"\n{len(results)} 轮：通过 {ok}，失败 {len(results)-ok-ab}，作废 {ab}")
    print(f"逐轮明细已追加到 {LOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
