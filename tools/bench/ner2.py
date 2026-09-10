"""Score a transcript against the near-end script S and the far-end script E.

Plain character NER is wrong here: S and E share "今天", "我们", "出去走走", so a
transcript containing only E scores partial credit against S, and a short
transcript containing only S scores partial credit against E simply because the
distance is capped by E's length. Both directions produced misleading numbers.

What separates them is their distinctive words, so those are what get counted --
plus a character NER on S for the fine-grained accuracy G4.1 asks for.
"""
import os, json, uuid, urllib.request, sys
import server
D = server.AUDIO_DIR
S = "今天下午我们出去走走好吗"
S_ONLY = ["下午", "好吗"]                                   # in S, not in E
E_ONLY = ["天气", "不错", "一起", "怎么样", "相机", "带上"]   # in E, not in S
PUNC = "，。？！、,.?! \n"


def lev(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def s_ner(hyp):
    hyp = "".join(c for c in hyp if c not in PUNC)
    if not hyp: return 0.0
    best = min((lev(S, hyp[i:i + len(S)]) for i in range(max(1, len(hyp) - len(S) + 1))),
               default=len(S))
    return max(0.0, 1 - best / len(S))


def asr(name):
    b = open(os.path.join(D, name), "rb").read()
    bd = "----b" + uuid.uuid4().hex
    body = (b"--" + bd.encode() + b"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
            b"Content-Type: audio/wav\r\n\r\n" + b + b"\r\n--" + bd.encode() + b"--\r\n")
    req = urllib.request.Request("http://110.40.154.41/common/api/asr/transcribe", data=body,
                                 headers={"Content-Type": "multipart/form-data; boundary=" + bd})
    return str(json.load(urllib.request.urlopen(req, timeout=150)).get("text") or "")


groups = [sys.argv[i:i + 3] for i in range(1, len(sys.argv), 3)]
cs, cm, leaks = [], [], []
print("%-4s %-28s %-8s %-8s" % ("ch", "transcript", "S-words", "E-words"))
for g in groups:
    for label, name in (("mic", g[0]), ("cln", g[2])):
        t = asr(name)
        sw = sum(w in t for w in S_ONLY)
        ew = sum(w in t for w in E_ONLY)
        print("%-4s %-28s %d/%d      %d/%d   ner=%.2f"
              % (label, t[:26] or "(空)", sw, len(S_ONLY), ew, len(E_ONLY), s_ner(t)))
        if label == "cln":
            cs.append(s_ner(t)); leaks.append(ew)
        else:
            cm.append(s_ner(t) if sw > 0 else 0.0)
    print()
n = len(cs)
print("clean S_ner 均值 %.3f  (门槛 0.75)" % (sum(cs) / n))
print("mic  S_ner 均值 %.3f  (门槛 ≤0.30，只在出现 S 专属词时计分)" % (sum(cm) / n))
print("clean 出现 E 专属词的段数 %d/%d  (红线 = 0)" % (sum(1 for x in leaks if x), n))
