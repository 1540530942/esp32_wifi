#!/usr/bin/env python3
"""A multi-turn conversation demo: normal turn-taking, barge-in, and a control.

Orchestration rule: never sleep for a duration and hope. Each step waits for
the previous one to actually finish, read back from device state or the task
queue. The two machines are not clock-synced and do not need to be -- the only
timestamps that matter (click onset, barge-in) are both taken on the ESP32's
own clock.

Section 2 reports every attempt, including the failures, with the gate counters
that explain them. A demo that shows only the attempts that worked would
misrepresent a detector that currently fires about half the time.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"
TASKS = "https://www.wangyutang.cn/action/api/tasks"
VOL = 80


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


def ack(cmd_id, timeout_s=30):
    end = time.time() + timeout_s
    while time.time() < end:
        for c in device().get("commands") or []:
            if c.get("id") == cmd_id and c.get("status") not in (
                    None, "dispatching", "pending", "sent", "dispatched"):
                return c.get("message") or c.get("status") or ""
        time.sleep(1.5)
    return None


def robot_say(name, expect_ms, label):
    """ESP32 speaks one turn, to completion. Returns ms actually played.

    spk_played_ms rides the heartbeat, which lands about every 5s, so the value
    visible at the instant the command flips to done is up to one heartbeat
    stale. Reading it there reported a 7920ms clip as 17083ms (the previous
    clip's figure, not yet refreshed) and a completed 27280ms turn as 19396ms,
    which read as the robot being cut off when nothing had cut it. Wait for the
    command to finish AND for the reported figure to stop changing.
    """
    before = device().get("state", {}).get("spk_played_ms") or 0
    cid = post(f"{HUB}/play_lan_audio",
               {"params": {"name": name},
                "settings_override": {"voice_volume_percent": VOL}}).get("command_id")
    end = time.time() + expect_ms / 1000 + 60
    done = False
    while time.time() < end and not done:
        for c in device().get("commands") or []:
            if c.get("id") == cid and c.get("status") in ("done", "failed"):
                done = True
        if not done:
            time.sleep(2)
    # Let the heartbeat carry the final figure, and require it to be a NEW one:
    # an unchanged value is the stale reading, not this turn's result.
    played, stable = 0, 0
    settle = time.time() + 20
    while time.time() < settle and stable < 2:
        time.sleep(3)
        now = device().get("state", {}).get("spk_played_ms") or 0
        if now == played and now != before:
            stable += 1
        else:
            stable = 0
        played = now
    short = expect_ms - played
    mark = "完整" if short <= 250 else f"**短 {short}ms**"
    print(f"  🤖 {label:22} {played:>6}/{expect_ms}ms  {mark}")
    return played


def human_say(name, expect_s, label):
    """Pi speaks one turn, to completion."""
    r = post(TASKS, {"action": "play_local_audio", "params": {"name": name},
                     "settings_override": {"voice_volume_percent": VOL}})
    tid = (r.get("task") or {}).get("id")
    end = time.time() + expect_s + 45
    while time.time() < end:
        p = subprocess.run(["curl", "-s", "-m", "20", f"{TASKS}?limit=25"],
                           capture_output=True, text=True)
        try:
            items = json.loads(p.stdout).get("tasks") or []
        except Exception:
            items = []
        for t in items:
            if t.get("id") == tid and t.get("status") in ("complete", "failed"):
                print(f"  🧑 {label:22} {t.get('status')}  ({expect_s}s)")
                return True
        time.sleep(2)
    print(f"  🧑 {label:22} 超时")
    return False


def section1():
    print("\n" + "=" * 66)
    print("第 1 段 · 正常多轮对话（严格一问一答，不重叠）")
    print("=" * 66)
    turns = [
        ("turn01_user.wav", 4.32, "轮1 用户 turn01"),
        ("turn02_assistant.wav", 7920, "轮1 机器人 turn02"),
        ("turn05_user.wav", 4.08, "轮2 用户 turn05"),
        ("turn06_assistant.wav", 27280, "轮2 机器人 turn06"),
        ("turn07_user.wav", 3.92, "轮3 用户 turn07"),
        ("turn08_assistant.wav", 27200, "轮3 机器人 turn08"),
    ]
    cut = 0
    for name, dur, label in turns:
        if name.endswith("_user.wav"):
            human_say(name, dur, label)
        else:
            if robot_say(name, dur, label) < dur - 250:
                cut += 1
        time.sleep(2)
    print(f"\n  机器人被误停 {cut} 次（应为 0）")
    return cut


def section2(attempts=4):
    print("\n" + "=" * 66)
    print("第 2 段 · 打断演示（全部如实报，含失败）")
    print("=" * 66)
    fired, missed = [], 0
    for i in range(attempts):
        print(f"\n  第 {i+1}/{attempts} 次：机器人开始说 turn04（27.2s）")
        post(f"{HUB}/play_lan_audio",
             {"params": {"name": "turn04_assistant.wav"},
              "settings_override": {"voice_volume_percent": VOL}})
        end = time.time() + 40
        while time.time() < end and not device().get("state", {}).get("audio_playing"):
            time.sleep(2)
        # Past the 1500ms onset grace, and far enough in for the AEC to settle.
        time.sleep(4)
        ack(post(f"{HUB}/command", {"action": "click_arm"}).get("command_id", ""))
        print("        用户插话 turn05（4.08s）...")
        post(TASKS, {"action": "play_local_audio",
                     "params": {"name": "turn05_user_click.wav"},
                     "settings_override": {"voice_volume_percent": VOL}})
        # Watch playback directly: whether it stopped is a property of the
        # player, while click_result only says whether the stop got paired with
        # a click timestamp. A barge-in that fires before the click is seen
        # stops the audio but records no latency, and scoring that as a miss
        # would be wrong.
        stopped_at = None
        watch = time.time() + 12
        while time.time() < watch:
            if not device().get("state", {}).get("audio_playing"):
                stopped_at = time.time()
                break
            time.sleep(0.5)
        if stopped_at is None:
            time.sleep(2)
        msg = ack(post(f"{HUB}/command", {"action": "click_result"}).get("command_id", "")) or ""
        played = device().get("state", {}).get("spk_played_ms") or 0
        if stopped_at is None and "latency=" not in msg:
            missed += 1
            print(f"        ❌ 没停 —— 机器人继续说（播放仍在进行）")
            stats = " ".join(w for w in msg.split() if w.split("=")[0] in
                             ("frames", "loud", "speech", "both", "max_rms"))
            if stats:
                print(f"        门限归因: {stats}")
            post(f"{HUB}/command", {"action": "stop_audio"})
            time.sleep(6)
            continue
        if "latency=" in msg:
            ms = int(msg.split("latency=")[1].split()[0])
            fired.append(ms)
            verdict = "✅ 200ms 内停播" if ms < 200 else f"⚠️ 停了，但用了 {ms}ms"
            print(f"        {verdict}   已播 {played}ms/27200ms")
        else:
            # Playback did stop, but no latency was recorded.
            print(f"        ⚠️ 停了，但没配上咔哒时间戳（无法给出延迟）")
        stats = " ".join(w for w in msg.split() if w.split("=")[0] in
                         ("frames", "loud", "speech", "both", "max_rms"))
        if stats:
            print(f"        门限归因: {stats}")
        post(f"{HUB}/command", {"action": "stop_audio"})
        time.sleep(6)
    return fired, missed


def section3():
    print("\n" + "=" * 66)
    print("第 3 段 · 对照：无人插话，不该停就不能停")
    print("=" * 66)
    played = robot_say("turn10_assistant.wav", 37920, "机器人 turn10 独白")
    return played >= 37920 - 250


def main():
    print("多轮对话演示  固件 v79  音量 80")
    cut = section1()
    fired, missed = section2()
    ok3 = section3()

    print("\n" + "=" * 66)
    print("演示小结")
    print("=" * 66)
    print(f"  第1段 正常轮流：机器人被误停 {cut} 次 -> {'通过' if cut == 0 else '不通过'}")
    total = len(fired) + missed
    if total:
        print(f"  第2段 打断：{len(fired)}/{total} 次停播"
              f"（触发率 {len(fired)/total*100:.0f}%），漏 {missed} 次")
        if fired:
            fast = [m for m in fired if m < 200]
            print(f"        其中 {len(fast)}/{len(fired)} 次在 200ms 内；延迟 {sorted(fired)}")
        print(f"  -> 判据「开口 200ms 内停播」：{len(fast) if fired else 0}/{total} 次达成")
    print(f"  第3段 对照：{'通过（整段播完）' if ok3 else '不通过（被误停）'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
