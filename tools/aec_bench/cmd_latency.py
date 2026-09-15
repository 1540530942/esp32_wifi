#!/usr/bin/env python3
"""How long from POSTing a command to the device acknowledging it.

Recorded as 12-18s and flagged as blocking, but that measurement was taken
while tang's disk was full and device_hub was returning 500 to every write --
including the acknowledgement path. A 500 on the ack write looks exactly like a
slow device. So the number needs retaking now that the disk is bounded, before
anyone spends effort optimising a latency that was really an ENOSPC.

Uses get_afe_config because it is read-only: it cannot disturb playback, and
several earlier runs were ruined by test tooling whose own stop_audio commands
landed in the next measurement.
"""
import json
import subprocess
import sys
import time

HUB = "https://www.wangyutang.cn/devices/api/device/esp32-s3-walle"


def record_of(cmd_id):
    """Status of one command by id.

    Deliberately not "has the log grown". The device log is capped at 200
    entries, so once it is full its length never increases and every command
    looks unacknowledged forever -- which is what the first run of this script
    reported, six timeouts against six commands the device had in fact acked
    within seconds. Watching the id is the only reading that cannot be fooled by
    a ring buffer.
    """
    proc = subprocess.run(["curl", "-s", "-m", "20", HUB],
                          capture_output=True, text=True)
    try:
        cmds = json.loads(proc.stdout).get("device", {}).get("commands") or []
    except Exception:
        return None
    for c in cmds:
        if c.get("id") == cmd_id:
            return c
    return None


def main() -> int:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    samples, server, tr = [], [], []
    for i in range(n):
        t0 = time.time()
        proc = subprocess.run(
            ["curl", "-s", "-m", "25", "-X", "POST", f"{HUB}/command",
             "-H", "Content-Type: application/json",
             "-d", json.dumps({"action": "get_afe_config"})],
            capture_output=True, text=True)
        try:
            cmd_id = json.loads(proc.stdout).get("command_id")
        except Exception:
            cmd_id = None
        if not cmd_id:
            print(f"  {i+1}/{n}: POST 没返回 command id")
            continue
        dt = None
        deadline = time.time() + 60
        while time.time() < deadline:
            time.sleep(1)
            rec = record_of(cmd_id) or {}
            if rec.get("status") not in (None, "dispatching", "pending", "sent"):
                dt = time.time() - t0
                # The hub stamps created_at when it accepts the command and
                # done_at when the device's ack lands, both on its own clock.
                # That difference is the delivery latency; the wall figure this
                # script measures also carries a 1s poll interval and two curl
                # round trips to a host ~200ms away, which is most of it.
                ca, do = rec.get("created_at") or 0, rec.get("done_at") or 0
                if ca and do:
                    server.append(do - ca)
                    tr.append(rec.get("transport"))
                break
        if dt is None:
            print(f"  {i+1}/{n}: 60s 内无回执")
        else:
            samples.append(dt)
            print(f"  {i+1}/{n}: {dt:.1f}s")
        time.sleep(5)

    if not samples:
        print("\n没有样本")
        return 1
    samples.sort()
    mid = samples[len(samples) // 2]
    print(f"\n探针墙钟   中位 {mid:.1f}s  最大 {samples[-1]:.1f}s  "
          f"({len(samples)}/{n} 有回执)")
    if server:
        server.sort()
        print(f"服务端实测 中位 {server[len(server)//2]*1000:.0f}ms  "
              f"最大 {server[-1]*1000:.0f}ms   transport={sorted(set(tr))}")
        print("（服务端那行才是真正的下发延迟；墙钟那行含探针轮询开销）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
