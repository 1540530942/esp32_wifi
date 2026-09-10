"""Barge-in latency scene: steady noise far end, four near-end utterances.

Noise rather than speech as the far end -- the criterion here is level over time,
and speech pauses on its own, which is indistinguishable from a duck.
"""
import json, sys, threading, time, urllib.request, secrets
import server

DID = "esp32-s3-walle"
ACTION = "https://www.wangyutang.cn/action/api/tasks"
S_TEXT = "今天下午我们出去走走好吗。"
esp_vol = int(sys.argv[1]); pi_vol = int(sys.argv[2]); run_id = sys.argv[3]


def cmd(action, args, wait=200):
    cid = "c-" + secrets.token_hex(3)
    with server.DATA_LOCK:
        d = server._load(); rec = d.get(DID)
        rec.setdefault("commands", []).append({
            "id": cid, "action": action, "args": args, "status": "dispatched",
            "created_at": time.time(), "dispatched_at": time.time(), "done_at": 0.0,
            "message": "", "transport": "mqtt"})
        d[DID] = rec; server._save(d)
    server._enqueue_mqtt_command(DID, cid, action, args)
    dl = time.time() + wait
    while time.time() < dl:
        time.sleep(2)
        with server.DATA_LOCK:
            rec = server._load().get(DID, {})
        for c in rec.get("commands", []):
            if c.get("id") == cid and c.get("status") in ("done", "complete", "failed"):
                try: server._mqtt_clear_retained_command(DID, cid)
                except Exception: pass
                return c.get("status"), c.get("message", "")
    try: server._mqtt_clear_retained_command(DID, cid)
    except Exception: pass
    return "timeout", ""


def pi_run(out):
    payload = {"action": "speak_listen",
               "params": {"text": S_TEXT, "seconds": 32, "speak_at_ms": 4000,
                          "repeat": 4, "gap_ms": 7000},
               "source": f"g5_{run_id}", "ttl_seconds": 120,
               "settings_override": {"voice_volume_percent": pi_vol}}
    req = urllib.request.Request(ACTION, data=json.dumps(payload).encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=40) as r:
        tid = json.loads(r.read())["task"]["id"]
    dl = time.time() + 200
    while time.time() < dl:
        time.sleep(4)
        with urllib.request.urlopen("https://www.wangyutang.cn/action/api/tasks?limit=8", timeout=30) as r:
            for t in json.loads(r.read())["tasks"]:
                if t["id"] == tid and t["status"] in ("complete", "failed", "expired"):
                    out["status"], out["text"] = t["status"], (t.get("output") or t.get("error") or "")
                    return
    out["status"] = "timeout"


cmd("far_end_stop", {}, wait=30)
st, msg = cmd("far_end", {"mode": "noise", "volume": esp_vol, "seconds": 45}, wait=90)
print(f"[g5 {run_id}] far_end: {st} | {msg}")
if st != "done":
    sys.exit(1)
out = {}
th = threading.Thread(target=pi_run, args=(out,)); th.start()
time.sleep(1)
st, msg = cmd("aec_capture", {"seconds": 30, "settle": 0, "no_play": True, "tag": run_id}, wait=200)
print(f"  capture: {st} | {msg}")
th.join()
cmd("far_end_stop", {}, wait=30)
print(f"  pi: {out.get('status')}")
for line in (out.get("text") or "").splitlines():
    print("   ", line[:200])
