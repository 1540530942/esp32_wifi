#!/usr/bin/env python3
"""把录音送云端 ASR，客观判定"残余里还有没有可辨的语音内容"。

E2/E2b/E3 的验收都落在"人耳能不能听出内容"上。跑自动化时没法戴耳机听，用同一个
ASR（qwen3-asr-1.7b）转写是可复现的替代判据：转写出内容 = 有可辨语音，转写为空
= 没有。历史上这套 AEC 台架用的就是这个模型，判据保持一致才好横向比。

用法：asr_check.py <wav> [<wav>...]
"""
import json, subprocess, sys
from pathlib import Path

ASR_URL = "http://110.40.154.41/common/api/asr/transcribe"


def transcribe(path: Path) -> str:
    r = subprocess.run(
        ["curl", "-s", "-m", "120", "-X", "POST", ASR_URL, "-F", f"file=@{path}"],
        text=True, capture_output=True, timeout=150)
    if r.returncode != 0:
        return f"<curl failed: {r.stderr[:120]}>"
    try:
        d = json.loads(r.stdout)
    except json.JSONDecodeError:
        return f"<non-json: {r.stdout[:160]}>"
    for k in ("text", "transcript", "result"):
        if isinstance(d.get(k), str):
            return d[k].strip()
    return f"<unexpected shape: {json.dumps(d, ensure_ascii=False)[:160]}>"


if __name__ == "__main__":
    for p in sys.argv[1:]:
        path = Path(p)
        text = transcribe(path)
        print(f"{path.name}: {text!r}" if text else f"{path.name}: <空>")
