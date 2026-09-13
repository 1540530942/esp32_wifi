"""生成 E1 用的粉红噪声：16kHz 单声道 16bit，40 秒。

用 Voss-McCartney 算法（多组不同更新率的白噪声叠加），比 FFT 整形更适合生成任意
长度且频谱稳定的粉红噪声。峰值留 -6 dBFS 余量，避免推到工作音量时削波——削波会
让 AEC 的线性假设不成立，测出来的 ERLE 会偏低且不可复现。
"""
import numpy as np, wave, sys

RATE, SECONDS, PEAK_DBFS = 16000, 40, -6.0
n = RATE * SECONDS
rng = np.random.default_rng(20260914)

# Voss-McCartney: 16 组，第 k 组每 2^k 个样本更新一次
rows = 16
arr = np.empty((rows, n))
for k in range(rows):
    step = 1 << k
    vals = rng.standard_normal(n // step + 2)
    arr[k] = np.repeat(vals, step)[:n]
pink = arr.sum(axis=0)

pink -= pink.mean()
peak = np.max(np.abs(pink))
target = 10 ** (PEAK_DBFS / 20.0) * 32767.0
pcm = (pink / peak * target).astype(np.int16)

out = sys.argv[1] if len(sys.argv) > 1 else "pink_noise_40s.wav"
with wave.open(out, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(pcm.tobytes())

# 自检：1/f 斜率应该接近 -3 dB/倍频程
spec = np.abs(np.fft.rfft(pcm.astype(np.float64))) ** 2
freqs = np.fft.rfftfreq(n, 1.0 / RATE)
def band(lo, hi):
    m = (freqs >= lo) & (freqs < hi)
    return 10 * np.log10(spec[m].sum() / m.sum())
print(f"{out}: {SECONDS}s @{RATE}Hz, peak={20*np.log10(np.max(np.abs(pcm))/32767.0):.1f} dBFS")
for lo, hi in ((125, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000)):
    print(f"  {lo:>5}-{hi:<5}Hz: {band(lo, hi):6.1f} dB   (相邻倍频程应约 -3 dB)")
