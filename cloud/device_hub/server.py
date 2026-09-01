"""device_hub — 设备终端注册/心跳/控制服务.

实现《设备 ↔ 平台 API 契约 v1》(见 API_CONTRACT.md)。设备侧仓库 1540530942/esp32_wifi
按契约上报;本服务是平台侧唯一实现。设计上与设备完全解耦:平台只认契约,用 device_type
区分设备种类,加新设备不改本文件。

路由说明:Caddy `handle_path /devices/*` 会剥掉 `/devices` 前缀,故本服务内部路由是
`/api/...` 与 `/`;对外即 `https://www.wangyutang.cn/devices/...`。

v1 从简:不做鉴权,所有接口开放。
"""

from __future__ import annotations

import asyncio
import json
import os
import secrets
import threading
import time
import urllib.request
import urllib.error
import io
import wave
import uuid
import re
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, UploadFile, File, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
import paho.mqtt.client as mqtt

from ota_manager import install_ota_routes


BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
DATA_DIR = BASE_DIR / "data"
DEVICES_FILE = DATA_DIR / "devices.json"
AUDIO_DIR = DATA_DIR / "audio"

DATA_DIR.mkdir(parents=True, exist_ok=True)
AUDIO_DIR.mkdir(parents=True, exist_ok=True)

TTS_URL = os.environ.get("TTS_URL", "http://audio-interact:8097/api/tts")
AUDIO_PUBLIC_BASE = os.environ.get("AUDIO_PUBLIC_BASE", "https://www.wangyutang.cn/devices/api/audio")
UPLOAD_MAX_BYTES = 20 * 1024 * 1024  # 20MB
TEST_AUDIO_TEXT = "早上好，我的公主"
TEST_AUDIO_PATH = AUDIO_DIR / "test-morning-princess.wav"

# 契约常量
HEARTBEAT_INTERVAL_S = 5           # 建议心跳周期,注册回执下发给设备
OFFLINE_AFTER_S = 15               # 3× 心跳无上报 -> offline(容忍偶发丢包)
MAX_LOGS = 200                     # 每设备环形日志上限
MAX_COMMAND_HISTORY = 50           # 每设备已完成指令保留上限
KNOWN_ACTIONS = {"reboot", "set_volume", "identify", "ota", "play_audio", "stop_audio", "stream_prepare"}
OFFLINE_ALERT_AFTER_S = 60    # 超过此时长无心跳 → 记录告警（4× OFFLINE_AFTER_S，过滤偶发断联）
ALERTS_FILE = DATA_DIR / "alerts.jsonl"
MAX_ALERTS = 500

DATA_LOCK = threading.Lock()
_alerted_offline: set[str] = set()   # 已告警的设备 id，防止重复；服务重启后重置

app = FastAPI(title="wangyutang device_hub", version="1.0.0")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


# MQTT control plane and in-memory PCM stream registry. Authentication is
# intentionally disabled for the current integration phase.
MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
PCM_STREAMS: dict[str, Path] = {}
PCM_STREAMS_LOCK = threading.Lock()


def _mqtt_publish_command(device_id: str, command: dict[str, Any]) -> bool:
    topic = f"devices/{device_id}/command/{command.get('command_id') or command.get('id') or secrets.token_hex(4)}"
    client = mqtt.Client(client_id=f"device-hub-{secrets.token_hex(4)}", protocol=mqtt.MQTTv311)
    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
        client.loop_start()
        info = client.publish(topic, json.dumps(command, ensure_ascii=False), qos=1, retain=True)
        info.wait_for_publish(timeout=5)
        return info.rc == mqtt.MQTT_ERR_SUCCESS
    except Exception as exc:
        print(f"mqtt publish failed topic={topic}: {exc}", flush=True)
        return False
    finally:
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass


def _mqtt_clear_retained_command(device_id: str, command_id: str) -> None:
    topic = f"devices/{device_id}/command/{command_id}"
    client = mqtt.Client(client_id=f"device-hub-clear-{secrets.token_hex(4)}", protocol=mqtt.MQTTv311)
    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
        client.loop_start()
        info = client.publish(topic, payload=b"", qos=1, retain=True)
        info.wait_for_publish(timeout=5)
    except Exception as exc:
        print(f"mqtt retained command clear failed topic={topic}: {exc}", flush=True)
    finally:
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass


def _record_mqtt_ack(device_id: str, payload: dict[str, Any]) -> bool:
    command_id = str(payload.get("command_id") or payload.get("id") or "")
    status = str(payload.get("status") or "")
    if not command_id or not status:
        return False
    with DATA_LOCK:
        data = _load()
        rec = data.get(device_id)
        if rec is None:
            return False
        for command in rec.get("commands", []):
            if command.get("id") != command_id:
                continue
            command.setdefault("transport", "mqtt")
            for key in ("action", "event", "stream_id"):
                if payload.get(key):
                    command[key] = payload[key]
            detail = str(payload.get("message") or "")
            for key, pattern in (("playback_bytes", r"\bbytes=(\d+)"), ("playback_elapsed_ms", r"\belapsed_ms=(\d+)"), ("pa_gpio17", r"\bpa_gpio17=(-?\d+)")):
                match = re.search(pattern, detail)
                if match:
                    command[key] = int(match.group(1))
            if payload.get("event") == "playback_done":
                command["playback_done_at"] = time.time()
            if status == "accepted":
                command["accepted_at"] = time.time()
                if command.get("status") == "pending":
                    command["status"] = "dispatched"
            elif status == "progress":
                command["message"] = detail
                bytes_match = re.search(r"\bbytes=(\d+)", detail)
                total_match = re.search(r"\btotal=(\d+)", detail)
                command["ota_progress_bytes"] = int(bytes_match.group(1)) if bytes_match else 0
                command["ota_progress_total"] = int(total_match.group(1)) if total_match else 0
                command["ota_progress_at"] = time.time()
            elif status in {"done", "failed", "unsupported"}:
                command["status"] = status
                command["message"] = str(payload.get("message") or "")
                command["done_at"] = time.time()
            _save(data)
            # OTA reboots immediately after writing the inactive slot. Clear
            # its retained command as soon as the device accepts it, otherwise
            # the freshly booted firmware could receive and execute it again.
            return status in {"done", "failed", "unsupported"} or (
                status == "accepted" and command.get("action") == "ota"
            )


def _mqtt_ack_worker() -> None:
    client = mqtt.Client(client_id=f"device-hub-ack-{secrets.token_hex(4)}", protocol=mqtt.MQTTv311)
    def on_connect(c, userdata, flags, rc):
        c.subscribe("devices/+/ack", qos=1)
    def on_message(c, userdata, msg):
        try:
            parts = msg.topic.split("/")
            if len(parts) == 3:
                device_id = parts[1]
                payload = json.loads(msg.payload.decode("utf-8"))
                if _record_mqtt_ack(device_id, payload):
                    _mqtt_clear_retained_command(device_id, str(payload.get("command_id") or payload.get("id") or ""))
        except Exception as exc:
            print(f"mqtt ack parse failed: {exc}", flush=True)
    client.on_connect = on_connect
    client.on_message = on_message
    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
            client.loop_forever()
        except Exception as exc:
            print(f"mqtt ack worker disconnected: {exc}", flush=True)
            time.sleep(3)


@app.on_event("startup")
def _start_mqtt_ack_worker() -> None:
    threading.Thread(target=_mqtt_ack_worker, name="mqtt-ack", daemon=True).start()


def _enqueue_mqtt_command(device_id: str, command_id: str, action: str,
                          args: dict[str, Any], text: str = "") -> bool:
    return _mqtt_publish_command(device_id, {
        "command_id": command_id, "id": command_id, "action": action,
        "args": args, "text": text, "published_at": time.time(),
    })


# ---------------------------------------------------------------------------
# 存储:单一 JSON 文件 { device_id: record }。对齐平台其余服务的 data/*.json 习惯。
# ---------------------------------------------------------------------------
def _load() -> dict[str, Any]:
    if not DEVICES_FILE.exists():
        return {}
    try:
        return json.loads(DEVICES_FILE.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}


def _save(data: dict[str, Any]) -> None:
    tmp = DEVICES_FILE.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(DEVICES_FILE)


def _new_device(device_id: str, device_type: str, name: str) -> dict[str, Any]:
    now = time.time()
    return {
        "device_id": device_id,
        "device_type": device_type or "unknown",
        "name": name or device_id,
        "first_seen": now,
        "last_seen": now,
        "state": {},
        "logs": [],       # [{level, message, ts}] 最新在末尾
        "commands": [],   # [{id, action, args, status, created_at, dispatched_at, done_at, message}]
    }


def _is_online(record: dict[str, Any]) -> bool:
    return (time.time() - float(record.get("last_seen", 0))) <= OFFLINE_AFTER_S


def _public_view(record: dict[str, Any], *, full: bool = False) -> dict[str, Any]:
    """转成前端友好的结构。full=True 附带日志与完整指令队列。"""
    view = {
        "device_id": record["device_id"],
        "device_type": record.get("device_type", "unknown"),
        "name": record.get("name") or record["device_id"],
        "online": _is_online(record),
        "first_seen": record.get("first_seen", 0),
        "last_seen": record.get("last_seen", 0),
        "state": record.get("state", {}),
    }
    if full:
        view["logs"] = record.get("logs", [])[-MAX_LOGS:]
        view["commands"] = record.get("commands", [])
    else:
        # 列表页只需要 state 里的常用键摘要,原样带过去由前端挑选
        view["pending_commands"] = sum(
            1 for c in record.get("commands", []) if c.get("status") == "pending"
        )
    return view


def _err(status: int, code: str, message: str = "") -> JSONResponse:
    return JSONResponse(
        status_code=status,
        content={"ok": False, "error": code, "message": message or code},
    )


# ---------------------------------------------------------------------------
# 离线告警基础设施
# ---------------------------------------------------------------------------
def _write_alert(alert: dict[str, Any]) -> None:
    try:
        line = json.dumps(alert, ensure_ascii=False) + "\n"
        with open(ALERTS_FILE, "a", encoding="utf-8") as f:
            f.write(line)
        if ALERTS_FILE.stat().st_size > 200_000:
            lines = ALERTS_FILE.read_text(encoding="utf-8").splitlines(keepends=True)
            ALERTS_FILE.write_text("".join(lines[-MAX_ALERTS:]), encoding="utf-8")
    except OSError:
        pass


def _check_offline_alerts() -> None:
    """后台线程：每 10s 扫一次，设备离线超 OFFLINE_ALERT_AFTER_S 时记录告警。"""
    while True:
        time.sleep(10)
        alerts_to_write: list[dict[str, Any]] = []
        with DATA_LOCK:
            data = _load()
            now = time.time()
            changed = False
            for device_id, rec in data.items():
                last_seen = float(rec.get("last_seen", 0))
                gone_for = now - last_seen
                online = gone_for <= OFFLINE_AFTER_S
                if online and device_id in _alerted_offline:
                    _alerted_offline.discard(device_id)
                elif not online and gone_for > OFFLINE_ALERT_AFTER_S and device_id not in _alerted_offline:
                    _alerted_offline.add(device_id)
                    alerts_to_write.append({
                        "ts": now,
                        "device_id": device_id,
                        "device_name": rec.get("name", device_id),
                        "event": "offline",
                        "gone_for_s": int(gone_for),
                    })
                    logs = rec.setdefault("logs", [])
                    logs.append({"level": "warn", "message": f"离线已 {int(gone_for)}s，告警已记录", "ts": now})
                    del logs[:-MAX_LOGS]
                    changed = True
            if changed:
                _save(data)
        for alert in alerts_to_write:
            _write_alert(alert)


# ---------------------------------------------------------------------------
# 请求模型
# ---------------------------------------------------------------------------
class RegisterReq(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=80)
    device_type: str = Field("unknown", max_length=40)
    name: str = Field("", max_length=80)
    state: dict[str, Any] = Field(default_factory=dict)


class HeartbeatReq(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=80)
    state: dict[str, Any] = Field(default_factory=dict)


class AckReq(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=80)
    command_id: str = Field(..., min_length=1, max_length=40)
    status: str = Field("done", max_length=20)   # done | failed | unsupported
    message: str = Field("", max_length=500)


class LogReq(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=80)
    level: str = Field("info", max_length=10)     # debug | info | warn | error
    message: str = Field("", max_length=500)
    ts: float = 0.0


class CommandReq(BaseModel):
    action: str = Field(..., min_length=1, max_length=40)
    args: dict[str, Any] = Field(default_factory=dict)


class BatchCommandReq(BaseModel):
    device_ids: list[str] | None = Field(None, description="None = 所有已注册设备")
    action: str = Field(..., min_length=1, max_length=40)
    args: dict[str, Any] = Field(default_factory=dict)


# ---------------------------------------------------------------------------
# 设备 -> 平台
# ---------------------------------------------------------------------------
@app.post("/api/register")
def register(req: RegisterReq) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(req.device_id)
        if rec is None:
            rec = _new_device(req.device_id, req.device_type, req.name)
        else:
            # 幂等:更新登记信息
            if req.device_type and req.device_type != "unknown":
                rec["device_type"] = req.device_type
            if req.name:
                rec["name"] = req.name
        rec["last_seen"] = time.time()
        if req.state:
            rec.setdefault("state", {}).update(req.state)
        data[req.device_id] = rec
        _save(data)
    return {
        "ok": True,
        "device_id": req.device_id,
        "server_time": int(time.time()),
        "heartbeat_interval_s": HEARTBEAT_INTERVAL_S,
        "config": {},
    }


@app.post("/api/heartbeat")
def heartbeat(req: HeartbeatReq) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(req.device_id)
        if rec is None:
            # 未注册也宽容接纳:自动建档(设备可能重启后直接心跳)
            rec = _new_device(req.device_id, "unknown", "")
        rec["last_seen"] = time.time()
        if req.state:
            rec.setdefault("state", {}).update(req.state)

        # 取出待下发指令(status==pending),标记为 dispatched 避免每次心跳重复下发
        dispatch: list[dict[str, Any]] = []
        for c in rec.get("commands", []):
            if c.get("status") == "pending":
                c["status"] = "dispatched"
                c["dispatched_at"] = time.time()
                dispatch.append({"id": c["id"], "action": c["action"], "args": c.get("args", {}), "text": c.get("text", "")})

        data[req.device_id] = rec
        _save(data)
    return {"ok": True, "server_time": int(time.time()), "commands": dispatch}


@app.post("/api/ack")
def ack(req: AckReq) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(req.device_id)
        if rec is None:
            return _err(404, "not_found", "unknown device_id")
        found = False
        for c in rec.get("commands", []):
            if c.get("id") == req.command_id:
                c["status"] = req.status if req.status in {"done", "failed", "unsupported"} else "done"
                c["message"] = req.message
                c["done_at"] = time.time()
                found = True
                break
        if not found:
            return _err(404, "not_found", "unknown command_id")
        # 修剪已完成指令历史
        done = [c for c in rec["commands"] if c.get("status") in {"done", "failed", "unsupported"}]
        active = [c for c in rec["commands"] if c.get("status") in {"pending", "dispatched"}]
        rec["commands"] = active + done[-MAX_COMMAND_HISTORY:]
        data[req.device_id] = rec
        _save(data)
    return {"ok": True}


@app.post("/api/log")
def log(req: LogReq) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(req.device_id)
        if rec is None:
            rec = _new_device(req.device_id, "unknown", "")
        entry = {
            "level": req.level if req.level in {"debug", "info", "warn", "error"} else "info",
            "message": req.message,
            "ts": req.ts or time.time(),
        }
        logs = rec.setdefault("logs", [])
        logs.append(entry)
        del logs[:-MAX_LOGS]  # 环形:仅保留最近 MAX_LOGS 条
        rec["last_seen"] = time.time()
        data[req.device_id] = rec
        _save(data)
    return {"ok": True}


# ---------------------------------------------------------------------------
# 前端 -> 平台
# ---------------------------------------------------------------------------
@app.get("/api/list")
def list_devices() -> Any:
    with DATA_LOCK:
        data = _load()
    devices = [_public_view(r) for r in data.values()]
    devices.sort(key=lambda d: (not d["online"], -d["last_seen"]))
    return {"ok": True, "devices": devices, "server_time": int(time.time())}


@app.get("/api/device/{device_id}")
def get_device(device_id: str) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(device_id)
    if rec is None:
        return _err(404, "not_found", "unknown device_id")
    return {"ok": True, "device": _public_view(rec, full=True), "server_time": int(time.time())}


@app.post("/api/device/{device_id}/command")
def enqueue_command(device_id: str, req: CommandReq) -> Any:
    with DATA_LOCK:
        data = _load()
        rec = data.get(device_id)
        if rec is None:
            return _err(404, "not_found", "unknown device_id")
        command_id = "c-" + secrets.token_hex(3)
        rec.setdefault("commands", []).append({
            "id": command_id,
            "action": req.action,
            "args": req.args,
            "status": "pending",
            "created_at": time.time(),
            "dispatched_at": 0.0,
            "done_at": 0.0,
            "message": "",
        })
        data[device_id] = rec
        _save(data)
    known = req.action in KNOWN_ACTIONS
    return {"ok": True, "command_id": command_id, "known_action": known}


class SpeakReq(BaseModel):
    text: str = Field(..., min_length=1, max_length=500)
    volume: int = Field(30, ge=0, le=100)


@app.post("/api/device/{device_id}/speak")
def speak(device_id: str, req: SpeakReq) -> Any:
    """文本 → TTS WAV → 保存 → 下发 play_audio 给设备。"""
    with DATA_LOCK:
        data = _load()
        if device_id not in data:
            return _err(404, "not_found", "unknown device_id")

    try:
        body = json.dumps({"text": req.text}).encode()
        tts_req = urllib.request.Request(
            TTS_URL, data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(tts_req, timeout=30) as resp:
            wav_bytes = resp.read()
    except urllib.error.URLError as e:
        return _err(502, "tts_error", f"TTS 服务不可达: {e}")

    filename = secrets.token_hex(8) + ".wav"
    (AUDIO_DIR / filename).write_bytes(wav_bytes)

    audio_url = f"{AUDIO_PUBLIC_BASE}/{filename}"
    with DATA_LOCK:
        data = _load()
        rec = data.get(device_id)
        if rec is None:
            return _err(404, "not_found", "unknown device_id")
        command_id = "c-" + secrets.token_hex(3)
        rec.setdefault("commands", []).append({
            "id": command_id,
            "action": "play_audio",
            "text": req.text,
            "args": {"url": audio_url, "volume": req.volume},
            # Reserve the command before MQTT publish so heartbeat cannot
            # race it and mark it dispatched without MQTT delivery.
            "status": "dispatching",
            "created_at": time.time(),
            "dispatched_at": 0.0,
            "done_at": 0.0,
            "message": "",
            "transport": "mqtt",
        })
        data[device_id] = rec
        _save(data)
    published = _enqueue_mqtt_command(
        device_id, command_id, "play_audio",
        {"url": audio_url, "volume": req.volume}, req.text,
    )
    with DATA_LOCK:
        data = _load()
        rec = data[device_id]
        for command in rec.get("commands", []):
            if command.get("id") == command_id:
                # A fast device can ACK while publish() is still returning.
                # Never overwrite a terminal ACK with "dispatched".
                if command.get("status") == "dispatching":
                    command["status"] = "dispatched" if published else "pending"
                    command["dispatched_at"] = time.time() if published else 0.0
                    command["transport"] = "mqtt" if published else "heartbeat"
                break
        _save(data)
    return {
        "ok": True,
        "command_id": command_id,
        "audio_url": audio_url,
        "transport": "mqtt" if published else "heartbeat",
    }


class TestAudioReq(BaseModel):
    volume: int = Field(30, ge=0, le=100)


@app.post("/api/device/{device_id}/test_audio")
def test_audio(device_id: str, req: TestAudioReq) -> Any:
    """快速测试固定音频；首次调用生成并缓存，后续直接下发。"""
    with DATA_LOCK:
        data = _load()
        if device_id not in data:
            return _err(404, "not_found", "unknown device_id")
    if not TEST_AUDIO_PATH.exists():
        try:
            body = json.dumps({"text": TEST_AUDIO_TEXT}).encode()
            tts_req = urllib.request.Request(
                TTS_URL, data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(tts_req, timeout=30) as resp:
                TEST_AUDIO_PATH.write_bytes(resp.read())
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            return _err(502, "tts_error", f"测试音频生成失败: {e}")
    audio_url = f"{AUDIO_PUBLIC_BASE}/{TEST_AUDIO_PATH.name}"
    with DATA_LOCK:
        data = _load()
        rec = data[device_id]
        command_id = "c-" + secrets.token_hex(3)
        rec.setdefault("commands", []).append({
            "id": command_id,
            "action": "play_audio",
            "text": TEST_AUDIO_TEXT,
            "args": {"url": audio_url, "volume": req.volume},
            "status": "dispatching",
            "created_at": time.time(),
            "dispatched_at": 0.0,
            "done_at": 0.0,
            "message": "",
        })
        data[device_id] = rec
        _save(data)
    command = {"id": command_id, "action": "play_audio", "args": {"url": audio_url, "volume": req.volume}, "text": TEST_AUDIO_TEXT}
    _mqtt_enqueue_ok = _enqueue_mqtt_command(device_id, command_id, command["action"], command["args"], TEST_AUDIO_TEXT)
    if _mqtt_enqueue_ok:
        with DATA_LOCK:
            data = _load()
            rec = data[device_id]
            for c in rec.get("commands", []):
                if c.get("id") == command_id:
                    if c.get("status") == "dispatching":
                        c["status"] = "dispatched"; c["dispatched_at"] = time.time(); c["transport"] = "mqtt"
            _save(data)
    else:
        with DATA_LOCK:
            data = _load()
            rec = data[device_id]
            for c in rec.get("commands", []):
                if c.get("id") == command_id and c.get("status") == "dispatching":
                    c["status"] = "pending"; c["transport"] = "heartbeat"
            _save(data)
    return {"ok": True, "command_id": command_id, "audio_url": audio_url, "text": TEST_AUDIO_TEXT, "transport": "mqtt" if _mqtt_enqueue_ok else "heartbeat"}


class SpeakPcmReq(BaseModel):
    text: str = Field(..., min_length=1, max_length=500)
    volume: int = Field(30, ge=0, le=100)
    # Optional PA output level for board-level validation. Omit to retain
    # active-high default; set 0 to test an active-low PA/MUTE circuit.
    pa_level: int | None = Field(None, ge=0, le=1)


@app.post("/api/device/{device_id}/speak_pcm")
def speak_pcm(device_id: str, req: SpeakPcmReq) -> Any:
    """TTS WAV -> MQTT stream_prepare -> WebSocket PCM stream."""
    with DATA_LOCK:
        if device_id not in _load():
            return _err(404, "not_found", "unknown device_id")
    try:
        body = json.dumps({"text": req.text}).encode()
        tts_req = urllib.request.Request(TTS_URL, data=body, headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(tts_req, timeout=30) as resp:
            wav_bytes = resp.read()
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            if wf.getnchannels() != 1 or wf.getsampwidth() != 2 or wf.getframerate() != 24000:
                return _err(502, "pcm_format", "TTS WAV must be 24kHz/16-bit/mono")
    except Exception as exc:
        return _err(502, "tts_error", f"TTS 服务不可达或格式错误: {exc}")
    stream_id = "s-" + secrets.token_hex(8)
    path = AUDIO_DIR / (stream_id + ".wav")
    path.write_bytes(wav_bytes)
    with PCM_STREAMS_LOCK:
        PCM_STREAMS[stream_id] = path
    command_id = "c-" + secrets.token_hex(3)
    args = {"stream_id": stream_id, "stream_url": f"wss://www.wangyutang.cn/devices/ws/pcm/{device_id}?stream_id={stream_id}", "audio": {"codec": "pcm_s16le", "sample_rate": 24000, "channels": 1, "frame_ms": 20}, "volume": req.volume}
    if req.pa_level is not None:
        args["pa_level"] = req.pa_level
    with DATA_LOCK:
        data = _load(); rec = data[device_id]
        # Reserve the command before publishing. The dispatching state is
        # intentionally invisible to heartbeat fallback, preventing the same
        # command from being delivered once by MQTT and once by heartbeat.
        rec.setdefault("commands", []).append({"id": command_id, "action": "stream_prepare", "text": req.text, "args": args, "status": "dispatching", "created_at": time.time(), "dispatched_at": 0.0, "done_at": 0.0, "message": "", "transport": "mqtt"})
        _save(data)
    published = _enqueue_mqtt_command(device_id, command_id, "stream_prepare", args, req.text)
    with DATA_LOCK:
        data = _load(); rec = data[device_id]
        for c in rec.get("commands", []):
            if c.get("id") == command_id and c.get("status") == "dispatching":
                c["status"] = "dispatched" if published else "pending"
                c["dispatched_at"] = time.time() if published else 0.0
                c["transport"] = "mqtt" if published else "heartbeat"
        _save(data)
    return {"ok": True, "command_id": command_id, "stream_id": stream_id, "transport": "mqtt" if published else "heartbeat", "stream_url": args["stream_url"]}


@app.websocket("/ws/pcm/{device_id}")
async def pcm_websocket(websocket: WebSocket, device_id: str, stream_id: str = "") -> None:
    await websocket.accept()
    with DATA_LOCK:
        known = device_id in _load()
    with PCM_STREAMS_LOCK:
        path = PCM_STREAMS.get(stream_id)
    # The registry is in memory, while the generated WAV is durable. Recover
    # the path after a container restart so MQTT delivery already accepted by
    # the device is not turned into an "unknown stream" 1008 error.
    if path is None and stream_id:
        candidate = AUDIO_DIR / (stream_id + ".wav")
        if candidate.exists():
            path = candidate
    if not known or path is None or not path.exists():
        await websocket.send_json({"event": "error", "message": "unknown stream"})
        await websocket.close(code=1008)
        return
    try:
        with wave.open(str(path), "rb") as wf:
            pcm = wf.readframes(wf.getnframes())
        stream_started = time.monotonic()
        frame_count = 0
        # 20 ms of 24 kHz / 16-bit / mono PCM. Small, even-sized frames avoid
        # large TLS/WS fragmentation. The device's blocking I2S writes and
        # the TCP receive window provide playback backpressure, so adding a
        # second real-time sleep here would nearly double playback duration.
        packet_bytes = 24000 * 2 * 20 // 1000
        await websocket.send_json({"event": "stream_start", "total_bytes": len(pcm), "audio": {"codec": "pcm_s16le", "sample_rate": 24000, "channels": 1, "frame_ms": 20, "packet_bytes": packet_bytes}})
        first_frame_at = time.monotonic()
        for offset in range(0, len(pcm), packet_bytes):
            await websocket.send_bytes(pcm[offset:offset + packet_bytes])
        frame_count = (len(pcm) + packet_bytes - 1) // packet_bytes if pcm else 0
        stream_elapsed_ms = int((time.monotonic() - stream_started) * 1000)
        first_frame_ms = int((first_frame_at - stream_started) * 1000)
        print(f"pcm_stream_done device={device_id} stream={stream_id} bytes={len(pcm)} frames={frame_count} first_frame_ms={first_frame_ms} send_ms={stream_elapsed_ms}", flush=True)
        await websocket.send_json({"event": "stream_end", "sent_bytes": len(pcm), "frames": frame_count, "send_ms": stream_elapsed_ms})
        # Keep the proxy/TLS path open until the device has drained the
        # buffered tail and closes its side after observing stream_end.
        try:
            await asyncio.wait_for(websocket.receive(), timeout=60)
        except (asyncio.TimeoutError, WebSocketDisconnect):
            pass
    except WebSocketDisconnect:
        return
    finally:
        with PCM_STREAMS_LOCK:
            PCM_STREAMS.pop(stream_id, None)


@app.post("/api/device/{device_id}/upload_audio")
async def upload_audio(device_id: str, file: UploadFile = File(...)) -> Any:
    """上传音频文件 → 保存 → 下发 play_audio 给设备。"""
    with DATA_LOCK:
        data = _load()
        if device_id not in data:
            return _err(404, "not_found", "unknown device_id")

    content = await file.read(UPLOAD_MAX_BYTES + 1)
    if len(content) > UPLOAD_MAX_BYTES:
        return _err(413, "too_large", f"文件超过 {UPLOAD_MAX_BYTES // 1024 // 1024}MB 限制")

    suffix = Path(file.filename or "audio.wav").suffix.lower() or ".wav"
    filename = secrets.token_hex(8) + suffix
    (AUDIO_DIR / filename).write_bytes(content)

    audio_url = f"{AUDIO_PUBLIC_BASE}/{filename}"
    with DATA_LOCK:
        data = _load()
        rec = data.get(device_id)
        if rec is None:
            return _err(404, "not_found", "unknown device_id")
        command_id = "c-" + secrets.token_hex(3)
        rec.setdefault("commands", []).append({
            "id": command_id,
            "action": "play_audio",
            "args": {"url": audio_url},
            "status": "pending",
            "created_at": time.time(),
            "dispatched_at": 0.0,
            "done_at": 0.0,
            "message": "",
        })
        data[device_id] = rec
        _save(data)
    return {"ok": True, "command_id": command_id, "audio_url": audio_url}


@app.get("/api/audio/{filename}")
def serve_audio(filename: str) -> FileResponse:
    """提供 TTS 生成或上传的音频文件（供 ESP32 下载）。"""
    path = AUDIO_DIR / filename
    if not path.exists() or path.parent != AUDIO_DIR:
        raise HTTPException(status_code=404, detail="not found")
    return FileResponse(path, media_type="audio/wav")


@app.post("/api/batch_command")
def batch_command(req: BatchCommandReq) -> Any:
    """向多台（或全部）设备同时下发同一指令。"""
    with DATA_LOCK:
        data = _load()
        targets = list(data.keys()) if req.device_ids is None else req.device_ids
        results: dict[str, Any] = {}
        now = time.time()
        for device_id in targets:
            rec = data.get(device_id)
            if rec is None:
                results[device_id] = {"ok": False, "error": "not_found"}
                continue
            command_id = "c-" + secrets.token_hex(3)
            rec.setdefault("commands", []).append({
                "id": command_id,
                "action": req.action,
                "args": req.args,
                "status": "pending",
                "created_at": now,
                "dispatched_at": 0.0,
                "done_at": 0.0,
                "message": "",
            })
            results[device_id] = {"ok": True, "command_id": command_id}
        _save(data)
    return {"ok": True, "known_action": req.action in KNOWN_ACTIONS,
            "total": len(targets), "results": results}


@app.get("/api/alerts")
def get_alerts(limit: int = 50) -> Any:
    """返回最近的离线告警，按时间倒序。前端每 5s 轮询。"""
    if not ALERTS_FILE.exists():
        return {"ok": True, "alerts": []}
    try:
        lines = ALERTS_FILE.read_text(encoding="utf-8").splitlines()
        alerts: list[dict[str, Any]] = []
        for line in reversed(lines):
            line = line.strip()
            if not line:
                continue
            try:
                alerts.append(json.loads(line))
            except json.JSONDecodeError:
                pass
            if len(alerts) >= limit:
                break
        return {"ok": True, "alerts": alerts}
    except OSError:
        return {"ok": True, "alerts": []}


@app.get("/api/health")
def health() -> dict[str, Any]:
    return {"status": "ok", "service": "device-hub"}


# ---------------------------------------------------------------------------
# 启动后台线程
# ---------------------------------------------------------------------------
@app.on_event("startup")
def _start_background() -> None:
    threading.Thread(target=_check_offline_alerts, daemon=True).start()


install_ota_routes(
    app,
    data_dir=DATA_DIR,
    static_dir=STATIC_DIR,
    data_lock=DATA_LOCK,
    load_devices=_load,
    save_devices=_save,
    is_online=_is_online,
    enqueue_mqtt_command=_enqueue_mqtt_command,
    clear_mqtt_command=_mqtt_clear_retained_command,
)


# ---------------------------------------------------------------------------
# 静态页面
# ---------------------------------------------------------------------------
@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/device")
def device_page() -> FileResponse:
    # 详情页,前端用 ?id=xxx 读取。放在 /device 而非 /device/ 便于相对静态资源。
    return FileResponse(STATIC_DIR / "device.html")
