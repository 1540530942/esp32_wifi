"""Count ducks in a capture where the near end never spoke.

With broadband noise as the far end the reference envelope is flat, so every
sustained drop is the duck gain firing. Any of them is a false trigger.
"""
import sys, os, wave, array, math
D = "/app/data/audio"
W = 0.05
w = wave.open(os.path.join(D, sys.argv[1])); a = array.array("h", w.readframes(w.getnframes())); w.close()
step = int(16000 * W)
env = [int(math.sqrt(sum(x * x for x in a[i:i + step]) / step)) for i in range(0, len(a) - step + 1, step)]
n = len(env)
base = sorted(env)[n // 2]
lo, hi = min(env), max(env)
print("windows %d  median %d  min %d  max %d  spread %.1f dB"
      % (n, base, lo, hi, 20 * math.log10(max(hi, 1) / max(lo, 1))))
ducks, i = [], 0
while i < n:
    if env[i] < base / 3:
        j = i
        while j < n and env[j] < base / 2:
            j += 1
        if (j - i) * W >= 0.2:
            ducks.append((i * W, (j - i) * W, min(env[i:j])))
        i = j
    else:
        i += 1
print("sustained drops below 1/3 median, >=200ms: %d" % len(ducks))
for t, d, m in ducks:
    print("  @%.2fs  for %.2fs  down to %d" % (t, d, m))
