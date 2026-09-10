"""Count ducks using a high percentile as the undisturbed level.

The median breaks as a baseline once ducking covers more than half the window --
it becomes the ducked level itself and every drop vanishes. Measured: scenes sit
at 44-52% ducked, so the median is right at the crossover.
"""
import sys, os, wave, array, math
import server
D = server.AUDIO_DIR
W = 0.05
for name in sys.argv[1:]:
    w = wave.open(os.path.join(D, name)); a = array.array("h", w.readframes(w.getnframes())); w.close()
    step = int(16000 * W)
    env = [int(math.sqrt(sum(x*x for x in a[i:i+step]) / step)) for i in range(0, len(a)-step+1, step)]
    n = len(env)
    base = sorted(env)[int(n * 0.9)]
    ducks, i = [], 0
    while i < n:
        if env[i] < base / 3:
            j = i
            while j < n and env[j] < base / 2:
                j += 1
            if (j - i) * W >= 0.2:
                ducks.append((round(i * W, 2), round((j - i) * W, 2)))
            i = j
        else:
            i += 1
    print("%s base=%d  ducks=%d  %s" % (name[:12], base, len(ducks),
          " ".join("@%.2f/%.2fs" % d for d in ducks)))
