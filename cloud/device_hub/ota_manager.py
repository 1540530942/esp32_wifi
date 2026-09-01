from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
import threading
import time
from pathlib import Path
from typing import Any, Callable

from fastapi import FastAPI, File, Form, Header, HTTPException, UploadFile
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field


MAX_FIRMWARE_BYTES = 3 * 1024 * 1024
OTA_JOB_TIMEOUT_S = 10 * 60
TERMINAL_TARGET_STATES = {"verified", "failed", "cancelled"}


class OtaJobRequest(BaseModel):
    release_id: str = Field(..., min_length=1, max_length=80)
    device_ids: list[str] = Field(..., min_length=1, max_length=100)
    force: bool = False


def install_ota_routes(
    app: FastAPI,
    *,
    data_dir: Path,
    static_dir: Path,
    data_lock: threading.Lock,
    load_devices: Callable[[], dict[str, Any]],
    save_devices: Callable[[dict[str, Any]], None],
    is_online: Callable[[dict[str, Any]], bool],
    enqueue_mqtt_command: Callable[[str, str, str, dict[str, Any], str], bool],
    clear_mqtt_command: Callable[[str, str], None],
) -> None:
    firmware_dir = data_dir / "firmware"
    releases_file = data_dir / "ota_releases.json"
    jobs_file = data_dir / "ota_jobs.json"
    audit_file = data_dir / "ota_audit.jsonl"
    firmware_dir.mkdir(parents=True, exist_ok=True)
    publish_token = os.environ.get("OTA_PUBLISH_TOKEN", "")
    audit_lock = threading.Lock()
    release_lock = threading.Lock()

    def load_json(path: Path, default: Any) -> Any:
        if not path.exists():
            return default
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return default

    def save_json(path: Path, value: Any) -> None:
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(path)

    def append_audit(job_id: str, event: str, *, device_id: str = "", detail: Any = None) -> dict[str, Any]:
        entry = {
            "ts": time.time(),
            "job_id": job_id,
            "device_id": device_id,
            "event": event,
            "detail": detail if detail is not None else {},
        }
        with audit_lock:
            with audit_file.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
        return entry

    def read_audit(job_id: str, limit: int = 500) -> list[dict[str, Any]]:
        if not audit_file.exists():
            return []
        result: list[dict[str, Any]] = []
        try:
            for line in audit_file.read_text(encoding="utf-8").splitlines():
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if entry.get("job_id") == job_id:
                    result.append(entry)
            return result[-limit:]
        except OSError:
            return []

    def require_publish_token(authorization: str | None) -> None:
        if not publish_token:
            return
        supplied = authorization or ""
        if supplied.startswith("Bearer "):
            supplied = supplied[7:]
        if not secrets.compare_digest(supplied, publish_token):
            raise HTTPException(status_code=403, detail="invalid OTA publish token")

    def release_by_id(release_id: str) -> dict[str, Any] | None:
        releases = load_json(releases_file, [])
        return next((item for item in releases if item.get("id") == release_id), None)

    @app.post("/api/ota/releases")
    async def publish_release(
        firmware: UploadFile = File(...),
        version: str = Form(...),
        commit_sha: str = Form(""),
        branch: str = Form("main"),
        build_url: str = Form(""),
        notes: str = Form(""),
        authorization: str | None = Header(None),
    ) -> Any:
        require_publish_token(authorization)
        version = version.strip()
        if not re.fullmatch(r"[A-Za-z0-9._+-]{1,80}", version):
            raise HTTPException(status_code=422, detail="invalid version")
        blob = await firmware.read(MAX_FIRMWARE_BYTES + 1)
        if len(blob) > MAX_FIRMWARE_BYTES:
            raise HTTPException(status_code=413, detail="firmware exceeds 3 MiB OTA slot limit")
        if len(blob) < 1024 or blob[0] != 0xE9:
            raise HTTPException(status_code=422, detail="not an ESP application image")
        digest = hashlib.sha256(blob).hexdigest()
        with release_lock:
            releases = load_json(releases_file, [])
            existing = next((item for item in releases if item.get("version") == version), None)
            if existing:
                if existing.get("sha256") != digest:
                    raise HTTPException(status_code=409, detail="version already exists with another SHA256")
                return {"ok": True, "release": existing, "idempotent": True}

            release_id = "rel-" + secrets.token_hex(6)
            safe_version = re.sub(r"[^A-Za-z0-9._+-]", "-", version)
            filename = f"{safe_version}-{digest[:12]}.bin"
            (firmware_dir / filename).write_bytes(blob)
            now = time.time()
            release = {
                "id": release_id,
                "version": version,
                "filename": filename,
                "url": f"https://www.wangyutang.cn/devices/api/ota/firmware/{filename}",
                "sha256": digest,
                "size": len(blob),
                "commit_sha": commit_sha[:64],
                "branch": branch[:100],
                "build_url": build_url[:500],
                "notes": notes[:1000],
                "created_at": now,
            }
            releases.append(release)
            releases.sort(key=lambda item: float(item.get("created_at", 0)), reverse=True)
            save_json(releases_file, releases[:200])
        append_audit("release:" + release_id, "release_published", detail=release)
        return {"ok": True, "release": release, "idempotent": False}

    @app.get("/api/ota/releases")
    def list_releases() -> Any:
        return {"ok": True, "releases": load_json(releases_file, [])}

    @app.get("/api/ota/firmware/{filename}")
    def download_firmware(filename: str) -> FileResponse:
        if not re.fullmatch(r"[A-Za-z0-9._+-]+\.bin", filename):
            raise HTTPException(status_code=404, detail="not found")
        path = firmware_dir / filename
        if not path.exists() or path.parent != firmware_dir:
            raise HTTPException(status_code=404, detail="not found")
        return FileResponse(path, media_type="application/octet-stream", filename=filename)

    @app.post("/api/ota/jobs")
    def create_job(req: OtaJobRequest) -> Any:
        release = release_by_id(req.release_id)
        if release is None:
            raise HTTPException(status_code=404, detail="unknown release_id")
        requested_ids = list(dict.fromkeys(req.device_ids))
        now = time.time()
        job_id = "ota-" + secrets.token_hex(6)
        dispatches: list[tuple[str, str, dict[str, Any]]] = []
        with data_lock:
            devices = load_devices()
            targets: dict[str, Any] = {}
            for device_id in requested_ids:
                rec = devices.get(device_id)
                if rec is None:
                    raise HTTPException(status_code=404, detail=f"unknown device: {device_id}")
                if not is_online(rec):
                    raise HTTPException(status_code=409, detail=f"device offline: {device_id}")
                current_version = str(rec.get("state", {}).get("firmware") or "")
                if current_version == release["version"] and not req.force:
                    raise HTTPException(status_code=409, detail=f"device already on {release['version']}: {device_id}")
                command_id = "c-" + secrets.token_hex(3)
                args = {
                    "url": release["url"],
                    "version": release["version"],
                    "sha256": release["sha256"],
                    "release_id": release["id"],
                    "ota_job_id": job_id,
                }
                rec.setdefault("commands", []).append({
                    "id": command_id,
                    "action": "ota",
                    "args": args,
                    "text": f"OTA {current_version or 'unknown'} → {release['version']}",
                    "status": "pending",
                    "created_at": now,
                    "dispatched_at": 0.0,
                    "done_at": 0.0,
                    "message": "",
                    "ota_job_id": job_id,
                    "ota_release_id": release["id"],
                })
                targets[device_id] = {
                    "device_id": device_id,
                    "command_id": command_id,
                    "from_version": current_version,
                    "to_version": release["version"],
                    "state": "queued",
                    "created_at": now,
                    "updated_at": now,
                    "last_seen_before": float(rec.get("last_seen", 0)),
                    "uptime_before": int(rec.get("state", {}).get("uptime_s") or 0),
                    "message": "",
                }
                dispatches.append((device_id, command_id, args))
                devices[device_id] = rec
            save_devices(devices)
            jobs = load_json(jobs_file, [])
            job = {
                "id": job_id,
                "release_id": release["id"],
                "version": release["version"],
                "sha256": release["sha256"],
                "status": "running",
                "created_at": now,
                "updated_at": now,
                "targets": targets,
            }
            jobs.insert(0, job)
            save_json(jobs_file, jobs[:200])

        append_audit(job_id, "job_created", detail={"release": release, "device_ids": requested_ids})
        published: dict[str, bool] = {}
        for device_id, command_id, args in dispatches:
            ok = enqueue_mqtt_command(device_id, command_id, "ota", args, "")
            published[device_id] = ok
            append_audit(job_id, "command_published" if ok else "command_queued_for_heartbeat",
                         device_id=device_id, detail={"command_id": command_id})
            if ok:
                with data_lock:
                    devices = load_devices()
                    rec = devices.get(device_id)
                    if rec:
                        for command in rec.get("commands", []):
                            if command.get("id") == command_id and command.get("status") == "pending":
                                command["status"] = "dispatched"
                                command["dispatched_at"] = time.time()
                        save_devices(devices)
        return {"ok": True, "job_id": job_id, "published": published}

    @app.get("/api/ota/jobs")
    def list_jobs(limit: int = 50) -> Any:
        limit = max(1, min(limit, 200))
        return {"ok": True, "jobs": load_json(jobs_file, [])[:limit]}

    @app.get("/api/ota/jobs/{job_id}")
    def get_job(job_id: str) -> Any:
        job = next((item for item in load_json(jobs_file, []) if item.get("id") == job_id), None)
        if job is None:
            raise HTTPException(status_code=404, detail="unknown OTA job")
        return {"ok": True, "job": job, "audit": read_audit(job_id)}

    @app.get("/ota")
    def ota_page() -> FileResponse:
        return FileResponse(static_dir / "ota.html")

    def monitor_jobs() -> None:
        while True:
            time.sleep(2)
            try:
                with data_lock:
                    jobs = load_json(jobs_file, [])
                    devices = load_devices()
                    now = time.time()
                    changed = False
                    pending_audit: list[tuple[str, str, str, dict[str, Any]]] = []
                    commands_to_clear: list[tuple[str, str]] = []
                    for job in jobs:
                        if job.get("status") not in {"running", "partial"}:
                            continue
                        for device_id, target in job.get("targets", {}).items():
                            if target.get("state") in TERMINAL_TARGET_STATES:
                                continue
                            rec = devices.get(device_id)
                            if not rec:
                                continue
                            command = next((item for item in rec.get("commands", [])
                                            if item.get("id") == target.get("command_id")), None)
                            online = is_online(rec)
                            current_version = str(rec.get("state", {}).get("firmware") or "")
                            current_uptime = int(rec.get("state", {}).get("uptime_s") or 0)
                            previous_state = target.get("state")
                            next_state = previous_state
                            message = target.get("message", "")
                            if current_version == target.get("to_version") and (
                                current_uptime < int(target.get("uptime_before") or 0) or
                                float(rec.get("last_seen", 0)) > float(job.get("created_at", 0)) + 3
                            ):
                                next_state = "verified"
                                message = f"device online with firmware {current_version}"
                                target["verified_at"] = now
                                commands_to_clear.append((device_id, str(target.get("command_id"))))
                                if command:
                                    command["status"] = "done"
                                    command["done_at"] = now
                                    command["message"] = "OTA verified after reboot"
                            elif previous_state == "rebooting" and \
                                    current_version == target.get("from_version") and \
                                    current_uptime < int(target.get("uptime_before") or 0) and \
                                    float(rec.get("last_seen", 0)) > float(job.get("created_at", 0)) + 3:
                                next_state = "failed"
                                message = (f"OTA rollback detected: device rebooted on "
                                           f"{current_version or 'previous firmware'}")
                                target["rollback_detected_at"] = now
                                commands_to_clear.append((device_id, str(target.get("command_id"))))
                                if command:
                                    command["status"] = "failed"
                                    command["done_at"] = now
                                    command["message"] = message
                            elif command and command.get("status") == "failed":
                                next_state = "failed"
                                message = str(command.get("message") or "device reported OTA failure")
                            elif command and command.get("status") == "done":
                                next_state = "rebooting"
                                message = str(command.get("message") or "image applied; waiting for reboot")
                            elif command and int(command.get("ota_progress_bytes") or 0) > int(target.get("progress_bytes") or 0):
                                next_state = "downloading"
                                target["progress_bytes"] = int(command.get("ota_progress_bytes") or 0)
                                target["progress_total"] = int(command.get("ota_progress_total") or 0)
                                target["updated_at"] = now
                                message = str(command.get("message") or "downloading firmware")
                                changed = True
                                pending_audit.append((str(job["id"]), "download_progress", device_id, {
                                    "bytes": target["progress_bytes"],
                                    "total": target["progress_total"],
                                }))
                            elif command and command.get("accepted_at") and previous_state == "queued":
                                next_state = "downloading"
                                message = "device accepted OTA command"
                            elif not online and previous_state in {"queued", "downloading", "dispatched"}:
                                next_state = "rebooting"
                                message = "device offline during OTA/reboot"
                            elif now - float(job.get("created_at", now)) > OTA_JOB_TIMEOUT_S:
                                next_state = "failed"
                                message = "OTA verification timeout"
                                commands_to_clear.append((device_id, str(target.get("command_id"))))

                            if next_state != previous_state:
                                target["state"] = next_state
                                target["updated_at"] = now
                                target["message"] = message
                                pending_audit.append((str(job["id"]), next_state, device_id, {
                                    "message": message,
                                    "firmware": current_version,
                                    "uptime_s": current_uptime,
                                }))
                                changed = True

                        states = [target.get("state") for target in job.get("targets", {}).values()]
                        old_status = job.get("status")
                        if states and all(state == "verified" for state in states):
                            job["status"] = "verified"
                            job["completed_at"] = now
                        elif states and all(state in TERMINAL_TARGET_STATES for state in states):
                            job["status"] = "failed" if all(state == "failed" for state in states) else "partial"
                            job["completed_at"] = now
                        if job.get("status") != old_status:
                            job["updated_at"] = now
                            pending_audit.append((str(job["id"]), "job_" + str(job["status"]), "", {}))
                            changed = True
                    if changed:
                        save_devices(devices)
                        save_json(jobs_file, jobs)
                for job_id, event, device_id, detail in pending_audit:
                    append_audit(job_id, event, device_id=device_id, detail=detail)
                for device_id, command_id in commands_to_clear:
                    clear_mqtt_command(device_id, command_id)
            except Exception as exc:
                print(f"OTA monitor error: {exc}", flush=True)

    @app.on_event("startup")
    def start_ota_monitor() -> None:
        threading.Thread(target=monitor_jobs, name="ota-monitor", daemon=True).start()
