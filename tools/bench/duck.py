"""Barge-in latency from one ESP32 capture.

Both events are in the same file pair, so no clock has to be synchronised:
clean_aec carries the near end (the AEC removed the echo, so its onset is the
person starting to talk), and reference carries what the speaker is actually
emitting (so a duck is a drop in its envelope).
"""
import sys, os, wave, array, math
D = "/app/data/audio"
W = 0.05  # 50 ms windows


def env(name):
    w = wave.open(os.path.join(D, name)); n = w.getnframes()
    a = array.array("h", w.readframes(n)); w.close()
    step = int(16000 * W)
    return [int(math.sqrt(sum(x * x for x in a[i:i + step]) / step))
            for i in range(0, len(a) - step + 1, step)]


clean, ref = env(sys.argv[2]), env(sys.argv[1])
n = min(len(clean), len(ref))
base_ref = sorted(ref[:n])[n // 2]          # median reference = undisturbed level
noise = sorted(clean[:n])[n // 10]          # clean floor

print("ref median %d   clean floor %d   windows %d (%.0f ms each)" % (base_ref, noise, n, W * 1000))
print("t(s)   ref  clean")
onsets = []
for i in range(n):
    if clean[i] > max(noise * 6, 150) and all(clean[j] <= max(noise * 6, 150) for j in range(max(0, i - 6), i)):
        onsets.append(i)
for o in onsets:
    duck = None
    for j in range(o, min(n, o + 40)):       # look 2 s ahead
        if ref[j] < base_ref / 3 and all(ref[k] < base_ref / 2 for k in range(j, min(n, j + 4))):
            duck = j
            break
    lat = "%d ms" % ((duck - o) * W * 1000) if duck is not None else "no duck"
    print("onset @%.2fs -> %s" % (o * W, lat))
    for j in range(max(0, o - 2), min(n, o + 14)):
        mark = " <-onset" if j == o else (" <-DUCK" if j == duck else "")
        print("  %.2f  %6d %6d%s" % (j * W, ref[j], clean[j], mark))
    print()
