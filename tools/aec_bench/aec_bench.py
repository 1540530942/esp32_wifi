#!/usr/bin/env python3
"""AEC 实验台：触发 ESP32 采集 → 取回三通道录音 → 打包立体声 → 归档 → 分析。

T0.4 要的是"ch0 原始信号 + AFE 输出打包成双声道，落地成立体声 WAV，能用 numpy
离线反复分析"。固件侧 aec_capture 已经在同一个采集窗口里把 mic_raw / reference /
clean_aec 写进三个按样本索引对齐的缓冲区，所以不需要新造固件通道（用户提到的
voice_link 在这个代码库里并不存在）——缺的只是取回和打包这一段，这个脚本补上。

录音归档在 spark:~/workspace/data/aec/bench/<run_id>/，原始三路 + 立体声版本都留，
方便以后改了参数横向对比。
"""
import argparse, json, subprocess, sys, time, wave
from pathlib import Path

DEVICE = "esp32-s3-walle"
HUB = "http://110.40.154.41/devices/api"
TANG = "tang"
ARCHIVE_ROOT = Path.home() / "workspace/data/aec/bench"


def sh(cmd, timeout=120):
    r = subprocess.run(cmd, shell=True, text=True, capture_output=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stderr[:500]}")
    return r.stdout


def send_command(action, args, wait_s=240):
    """下发命令并等到终态，返回 (status, message)。"""
    body = json.dumps({"action": action, "args": args})
    out = sh(f"curl -s -m 15 -X POST '{HUB}/device/{DEVICE}/command' "
             f"-H 'Content-Type: application/json' -d '{body}'")
    cmd_id = json.loads(out)["command_id"]
    print(f"  [{action}] command_id={cmd_id} args={args}")
    deadline = time.time() + wait_s
    while time.time() < deadline:
        time.sleep(5)
        dev = json.loads(sh(f"curl -s -m 10 '{HUB}/device/{DEVICE}'"))
        for c in dev["device"].get("commands", []):
            if c.get("id") == cmd_id:
                st = c.get("status")
                if st in ("done", "failed", "unsupported"):
                    return st, (c.get("message") or "")
        # 回执可能因为历史上的 20 字符 status 截断问题丢失，此时从设备日志里捞
        for l in reversed(dev["device"].get("logs", [])):
            if cmd_id in (l.get("message") or ""):
                return "done(from-log)", l["message"]
    return "timeout", ""


ACTION_API = "https://www.wangyutang.cn/action/api"


PI_CLIP_DIR = "/home/pi/action_move/local_audio"


def pi_play(name, volume=70):
    """让树莓派播一个已预置的素材（扮演人说话）。阻塞到播完。

    这套对话素材是严格一问一答的，天然没有重叠，所以双讲和打断必须由这里人为
    制造：先让 ESP32 开始播，再在指定时刻触发树莓派开口。

    走 ssh aplay，但**不是因为任务队列慢**：实测下发到出声 1.4s，认领只要 7ms。
    这段注释早先写的"队列约 8 秒"是错的——那个数来自一次时间线误读，而且换成
    ssh 之后触发率反而更差（50-60% -> 42%），当时就该据此排除传输层。真正的
    成因是判据（只认咔哒）。

    ssh 仍然更合适：它阻塞到整段播完，观察窗口是已知的而不是推断的。
    """
    print(f"  [pi_play] {name} vol={volume} (ssh aplay)")
    sh(f"ssh pi 'aplay -D plughw:2,0 {PI_CLIP_DIR}/{name}'")
    return name


def snapshot_uploads():
    """采集前的文件名快照，用来识别本次新增的上传。"""
    out = sh(f"ssh -o ConnectTimeout=15 {TANG} \"docker exec device-hub ls /app/data/audio/\"")
    return set(out.split())


def pull_new_uploads(before: set, dest: Path):
    """取回本次新增的上传文件，按精确 mtime 升序 = 固件的上传顺序。

    固件里 aec_upload_wav 依次传 mic_raw / reference / clean_aec，所以升序就是这个
    顺序。不能只用 `ls -t` 取最近 N 个：同一秒内上传的文件排序不保证稳定，而且会把
    别的来源（boot_announce、speak）的文件混进来。
    """
    dest.mkdir(parents=True, exist_ok=True)
    listing = sh(f"ssh -o ConnectTimeout=15 {TANG} "
                 f"\"docker exec device-hub ls --full-time /app/data/audio/\"")
    entries = []
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) < 9:
            continue
        name = parts[-1]
        if name in before:
            continue
        entries.append((parts[5] + " " + parts[6], name))   # date + time.nnnnnnnnn
    entries.sort()
    pulled = []
    for _, n in entries:
        sh(f"ssh -o ConnectTimeout=20 {TANG} \"docker exec device-hub cat /app/data/audio/{n}\" "
           f"> {dest / n}", timeout=180)
        pulled.append(dest / n)
    return pulled


def read_wav(path: Path):
    import numpy as np
    with wave.open(str(path), "rb") as w:
        rate, n = w.getframerate(), w.getnframes()
        data = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    return data, rate


def pack_stereo(mic: Path, clean: Path, out: Path):
    """把 ch0 原始 和 AFE 输出 交织成立体声 —— T0.4 的交付物。"""
    import numpy as np
    a, rate = read_wav(mic)
    b, rate_b = read_wav(clean)
    assert rate == rate_b, f"采样率不一致: {rate} vs {rate_b}"
    n = min(len(a), len(b))
    inter = np.empty(n * 2, dtype=np.int16)
    inter[0::2] = a[:n].astype(np.int16)   # 左 = AFE 前（原始麦克风）
    inter[1::2] = b[:n].astype(np.int16)   # 右 = AFE 后（AEC 输出）
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(inter.tobytes())
    return n / rate, len(a), len(b)


def analyse(stereo: Path, settle_s=1.0):
    """ERLE、收敛时间、分频段 —— 全部离线算，同一份录音可反复分析。"""
    import numpy as np
    with wave.open(str(stereo), "rb") as w:
        rate, n = w.getframerate(), w.getnframes()
        d = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    pre, post = d[0::2], d[1::2]

    def erle(x, y):
        px, py = np.mean(x * x), np.mean(y * y)
        return 10 * np.log10(px / py) if py > 0 and px > 0 else float("nan")

    skip = int(settle_s * rate)
    res = {
        "duration_s": round(n / rate, 2),
        "erle_full_db": round(erle(pre, post), 2),
        "erle_steady_db": round(erle(pre[skip:], post[skip:]), 2),
        "pre_rms": round(float(np.sqrt(np.mean(pre ** 2))), 1),
        "post_rms": round(float(np.sqrt(np.mean(post ** 2))), 1),
    }

    # 逐 100ms 的 ERLE 曲线 -> 收敛时间（首次达到稳态值 -3dB 并保持）
    win = rate // 10
    curve = []
    for i in range(0, n - win, win):
        curve.append(erle(pre[i:i + win], post[i:i + win]))
    res["erle_curve_100ms"] = [None if np.isnan(v) else round(float(v), 1) for v in curve]
    steady = res["erle_steady_db"]
    conv = None
    if curve and not np.isnan(steady):
        for i, v in enumerate(curve):
            if not np.isnan(v) and v >= steady - 3:
                if all((not np.isnan(u)) and u >= steady - 6 for u in curve[i:i + 5]):
                    conv = round(i * 0.1, 1); break
    res["convergence_s"] = conv

    # 分频段（低/中/高），用 FFT 功率比
    spec_pre = np.abs(np.fft.rfft(pre[skip:])) ** 2
    spec_post = np.abs(np.fft.rfft(post[skip:])) ** 2
    freqs = np.fft.rfftfreq(len(pre[skip:]), 1.0 / rate)
    for label, lo, hi in (("low_0_500", 0, 500), ("mid_500_2k", 500, 2000), ("high_2k_8k", 2000, 8000)):
        m = (freqs >= lo) & (freqs < hi)
        a, b = spec_pre[m].sum(), spec_post[m].sum()
        res[f"erle_{label}_db"] = round(float(10 * np.log10(a / b)), 2) if b > 0 and a > 0 else None
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True, help="本次实验标识，用于归档目录名")
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--settle", type=int, default=3)
    ap.add_argument("--volume", type=int, default=70)
    ap.add_argument("--url", default="", help="播放局域网素材（E2/E3），留空则播内置噪声")
    ap.add_argument("--no-play", action="store_true", help="纯录音不播放（E2b）")
    ap.add_argument("--pi-play", default="", help="同时让树莓派播这个素材（扮演人）")
    ap.add_argument("--pi-volume", type=int, default=70)
    ap.add_argument("--mute-bargein", action="store_true",
                    help="采集期间只统计打断、不执行（E3 需要双讲持续下去）")
    ap.add_argument("--pi-delay", type=float, default=0.0,
                    help="树莓派开口相对采集开始的延迟秒数（E3 靠这个制造双讲重叠）")
    args = ap.parse_args()

    run_id = f"{args.tag}_{time.strftime('%H%M%S')}"
    dest = ARCHIVE_ROOT / run_id
    print(f"== run {run_id} ==")

    cap_args = {"seconds": args.seconds, "settle": args.settle,
                "volume": args.volume, "tag": args.tag}
    if args.no_play:
        cap_args["no_play"] = True
    elif args.url:
        cap_args["url"] = args.url
    if args.mute_bargein:
        cap_args["mute_bargein"] = True

    before = snapshot_uploads()

    # 树莓派的开口时刻由一个后台线程定时触发。这里不追求毫秒级对齐：命令经心跳
    # 下发有 0~5s 的排队抖动，精确调度本来就做不到。做法是把采集窗口开得足够长，
    # 事后从录音里定位人声实际落在哪儿，用内容（ASR）而不是时间戳来判读。
    if args.pi_play:
        import threading
        def fire():
            time.sleep(args.pi_delay)
            try:
                pi_play(args.pi_play, args.pi_volume)
            except Exception as exc:                      # noqa: BLE001
                print(f"  !! pi_play 失败: {exc}")
        threading.Thread(target=fire, daemon=True).start()

    st, msg = send_command("aec_capture", cap_args,
                           wait_s=args.seconds + args.settle + 180)
    print(f"  capture -> {st}: {msg[:200]}")
    if st.startswith("failed") or st == "timeout":
        sys.exit(1)

    files = pull_new_uploads(before, dest)
    print(f"  pulled {len(files)} new files -> {dest}")
    for f in files:
        d, _ = read_wav(f)
        print(f"    {f.name}: {len(d)} samples")
    if len(files) != 3:
        print(f"  !! 预期 3 个新文件（mic/ref/clean），实际 {len(files)}，不做分析")
        sys.exit(2)

    mic, ref, clean = files          # mtime 升序 = 固件上传顺序
    (dest / "mic_raw.wav").write_bytes(mic.read_bytes())
    (dest / "reference.wav").write_bytes(ref.read_bytes())
    (dest / "clean_aec.wav").write_bytes(clean.read_bytes())
    dur, nm, nc = pack_stereo(dest / "mic_raw.wav", dest / "clean_aec.wav",
                              dest / "stereo_pre_post.wav")
    print(f"  stereo packed: {dur:.2f}s (mic={nm} clean={nc} samples)")
    res = analyse(dest / "stereo_pre_post.wav", settle_s=1.0)
    res["_meta"] = {"run_id": run_id, "capture_args": cap_args, "device_msg": msg[:300]}
    (dest / "analysis.json").write_text(json.dumps(res, ensure_ascii=False, indent=2))
    print("  " + json.dumps({k: v for k, v in res.items()
                             if k not in ("erle_curve_100ms", "_meta")}, ensure_ascii=False))

    # 归档到 spark，跟既有的 AEC 数据放在一起（本机存储不保证持久）
    sh(f"ssh -o ConnectTimeout=15 spark 'mkdir -p ~/workspace/data/aec/bench/{run_id}'")
    sh(f"scp -o ConnectTimeout=20 -q {dest}/*.wav {dest}/analysis.json "
       f"spark:~/workspace/data/aec/bench/{run_id}/", timeout=300)
    print(f"  archived -> spark:~/workspace/data/aec/bench/{run_id}/")


if __name__ == "__main__":
    main()
