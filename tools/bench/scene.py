"""One 60 s full-duplex scene.

Far end (ESP32) plays E on loop for the whole minute; the near end (Pi) speaks S
four times inside it, and the ESP32 samples three 12 s segments spread across the
minute. The AFE is never reset, so the segments are slices of one continuous
scene rather than three independent runs.
"""
import json, os, sys, threading, time, urllib.request, secrets
import server

DID = "esp32-s3-walle"
ACTION = "https://www.wangyutang.cn/action/api/tasks"
E = "今天天气不错，我们一起出去走走吧，你觉得怎么样呢，要不要带上相机。"
S_TEXT = "今天下午我们出去走走好吗。"

esp_vol = int(sys.argv[1]) if len(sys.argv) > 1 else 40
pi_vol = int(sys.argv[2]) if len(sys.argv) > 2 else 70
run_id = sys.argv[3] if len(sys.argv) > 3 else time.strftime("%H%M%S")


def cmd(action, args, wait=120):
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


def pi_task(payload):
    req = urllib.request.Request(ACTION, data=json.dumps(payload).encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.loads(r.read())["task"]["id"]


def pi_wait(tid, wait=200):
    dl = time.time() + wait
    while time.time() < dl:
        time.sleep(5)
        with urllib.request.urlopen("https://www.wangyutang.cn/action/api/tasks?limit=8", timeout=30) as r:
            for t in json.loads(r.read())["tasks"]:
                if t["id"] == tid and t["status"] in ("complete", "failed", "expired"):
                    return t["status"], (t.get("output") or t.get("error") or "")
    return "timeout", ""


print(f"[scene {run_id}] esp_vol={esp_vol} pi_vol={pi_vol}")

# 1. Start the far end. This fetches the TTS first (~12 s), so it must lead.
st, msg = cmd("far_end", {"text": E, "volume": esp_vol, "seconds": 78}, wait=120)
print(f"  far_end: {st} | {msg}")
if st != "done":
    sys.exit(1)
t0 = time.time()

# 2. Near end: four utterances inside one 60 s recording, on its own thread so
#    the captures below run concurrently.
pi_out = {}
def run_pi():
    tid = pi_task({"action": "speak_listen",
                   # 20 s apart, not 14: a segment is 12 s of capture plus ~8 s
                   # of upload, so consecutive captures cannot be closer than
                   # that. At 14 s the second and third segments caught only the
                   # start or tail of an utterance -- one segment transcribed to
                   # nothing at all because it held just the last 2 s of speech.
                   "params": {"text": S_TEXT, "seconds": 60, "speak_at_ms": 5000,
                              "repeat": 3, "gap_ms": 24000},   # 24 s, matching the observed
                   # capture cadence: a segment is 12 s of recording plus about
                   # 12 s of upload, so the scheduled 3/23/43 offsets actually
                   # land at 3.0/27.1/51.2. Fighting that drift left the third
                   # segment missing its utterance entirely; spacing the
                   # utterances to match it instead puts one inside each window.
                   "source": f"scene_{run_id}", "ttl_seconds": 120,
                   "settings_override": {"voice_volume_percent": pi_vol}})
    pi_out["status"], pi_out["text"] = pi_wait(tid)
th = threading.Thread(target=run_pi); th.start()

# 3. Three segments spread across the minute.
segs = []
# Each capture opens 2 s before an utterance (5/25/45 s) so the whole utterance
# falls inside its 12 s window. At the old 14 s spacing the second and third
# segments caught only the start or tail of one, and a segment holding just the
# last 2 s of speech transcribed to nothing at all.
for k, at in enumerate([3, 27, 51], start=1):
    wait_for = at - (time.time() - t0)
    if wait_for > 0:
        time.sleep(wait_for)
    started = time.time() - t0
    st, msg = cmd("aec_capture",
                  {"seconds": 12, "settle": 0, "no_play": True, "tag": f"{run_id}s{k}"},
                  wait=150)
    segs.append({"k": k, "t_start": round(started, 1), "status": st, "msg": msg})
    print(f"  seg{k} @t={started:5.1f}s: {st} | {msg}")

th.join()
cmd("far_end_stop", {}, wait=30)
print(f"  pi: {pi_out.get('status')}")
for line in (pi_out.get("text") or "").splitlines():
    print("   ", line[:400])
json.dump({"run_id": run_id, "esp_vol": esp_vol, "pi_vol": pi_vol,
           "segments": segs, "pi": pi_out},
          open(f"/app/data/scene_{run_id}.json", "w"), ensure_ascii=False, indent=2)
