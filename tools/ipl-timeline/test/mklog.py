# synthetic PISTORM_IPL_TIMELINE=2 log: 2000 lines of 63.83 us, with
# one loss of each kind at known lines
import sys
L = 63.83
out = ["# test"]
t0 = 1000.0
lose = {100: "other", 200: "cpu", 300: "notlatch", 400: "short", 500: "unseen", 501: "unseen"}
for n in range(2000):
    t = t0 + n * L
    kind = lose.get(n)
    if kind == "unseen":                      # line never seen low or back
        continue
    if kind == "short":
        out.append("%.1f C 2 0->2 1.20" % (t - 30))
        continue
    out.append("%.1f I 2->0 line 0 1" % (t - 40))
    out.append("%.1f I 0->2 line 0 1" % (t - 8))
    if kind == "notlatch":
        out.append("%.1f B 2 av_held 0 1" % (t - 7))
        continue
    out.append("%.1f I 0->2 LATCH 0 1" % (t - 7))
    if kind == "cpu":
        continue
    if kind == "other":
        out.append("%.1f X 28 1C 1 00E000" % (t - 5))
        continue
    out.append("%.1f X 26 1A 1 012345" % t)
print("\n".join(out))
