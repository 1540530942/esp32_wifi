#!/usr/bin/env python3
"""把录音送 ASR，客观判定"残余里还有没有可辨的语音内容"。

E2/E2b/E3 的验收都落在"人耳能不能听出内容"上。跑自动化时没法戴耳机听，用同一个
ASR（qwen3-asr-1.7b）转写是可复现的替代判据：转写出内容 = 有可辨语音，转写为空
= 没有。历史上这套 AEC 台架用的就是这个模型，判据保持一致才好横向比。

2026-10-11：原来经 110.40.154.41 转发到 lv_server（39.156.151.204:8000）的路径
断了（那台机器连不上）。改走 spark 本地跑的同一个模型（容器 vllm-qwen3-asr，
OpenAI 兼容的 /v1/audio/transcriptions，端口 8001）——scp 文件过去，在 spark 上
本地 curl，没有额外的公网依赖。判据模型没变，只是换了一条不依赖 lv_server 的路。

用法：asr_check.py <wav> [<wav>...]
"""
import json, subprocess, sys
from pathlib import Path

SPARK_ASR_PORT = 8001
SPARK_MODEL = "qwen3-asr-1.7b"
REMOTE_TMP = "/tmp/asr_check_in.wav"


def transcribe(path: Path) -> str:
    scp = subprocess.run(["scp", "-q", str(path), f"spark:{REMOTE_TMP}"],
                         capture_output=True, text=True, timeout=60)
    if scp.returncode != 0:
        return f"<scp failed: {scp.stderr[:120]}>"
    r = subprocess.run(
        ["ssh", "spark",
         f"curl -s -m 120 -X POST http://localhost:{SPARK_ASR_PORT}/v1/audio/transcriptions "
         f"-F file=@{REMOTE_TMP} -F model={SPARK_MODEL}"],
        text=True, capture_output=True, timeout=150)
    if r.returncode != 0:
        return f"<ssh/curl failed: {r.stderr[:120]}>"
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
