# synthetic PISTORM_IPL_TIMELINE=2 log with known losses:
#  HBL: 2000 lines of 63.83 us, one loss of each kind and a 30-line
#       stuck episode (line reads 2 until a Timer C moves it)
#  Timer B: 5 event-count frames, the third missing lines 50 and 51
L = 63.83
out = ["# test"]
t0 = 1000.0
lose = {100: "other", 200: "cpu", 300: "notlatch", 400: "short", 500: "unseen",
        501: "unseen"}
stuck_from, stuck_to = 700, 730
for n in range(2000):
    t = t0 + n * L
    kind = lose.get(n)
    if stuck_from < n < stuck_to:
        continue
    if n == stuck_to:                         # Timer C unsticks it
        out.append("%.1f I 2->6 line 0 1" % (t - 20))
        out.append("%.1f I 0->6 LATCH 0 1" % (t - 20))
        out.append("%.1f X 30 45 1 00E000" % (t - 18))
        out.append("%.1f I 6->2 line 0 1" % (t - 3))
        out.append("%.1f I 0->2 LATCH 0 1" % (t - 2))
        out.append("%.1f X 26 1A 1 012345" % t)
        continue
    if kind == "unseen":
        continue
    if kind == "other":                       # VBL handler covers the HBL
        out.append("%.1f I 2->0 line 0 1" % (t - 40))
        out.append("%.1f I 0->4 line 0 1" % (t - 32))
        out.append("%.1f X 28 1C 1 00E000" % (t - 30))
        out.append("%.1f I 4->2 line 0 4" % (t - 8))
        out.append("%.1f I 2->0 line 0 4" % (t + 12))
        out.append("%.1f M 4->1 00E002" % (t + 20))
        continue
    if kind == "short":
        out.append("%.1f C 2 0->2 1.20" % (t - 30))
        continue
    if n != stuck_from + 1:
        out.append("%.1f I 2->0 line 0 1" % (t - 40))
    out.append("%.1f I 0->2 line 0 1" % (t - 8))
    if kind == "notlatch":
        out.append("%.1f B 2 av_held 0 1" % (t - 7))
        continue
    out.append("%.1f I 0->2 LATCH 0 1" % (t - 7))
    if kind == "cpu":
        continue
    out.append("%.1f X 26 1A 1 012345" % t)
    if n == stuck_from:
        out.append("%.1f B 2 av_held 0 1" % (t + 15))
# Timer B frames, after the HBL run
tb0 = t0 + 2000 * L + 50000
for f in range(6):
    fs = tb0 + f * 20000
    out.append("%.1f X 28 1C 3 00E000" % fs)
    if f == 5:
        break
    for n in range(200):
        t = fs + 3000 + n * L
        if f == 2 and n in (50, 51):
            continue
        out.append("%.1f I 0->6 line 0 3" % (t - 2))
        out.append("%.1f I 0->6 LATCH 0 3" % (t - 1.5))
        out.append("%.1f X 30 48 3 00E100" % t)
        out.append("%.1f I 6->0 line 0 6" % (t + 5))
        out.append("%.1f M 6->3 00E104" % (t + (90 if f == 2 and n == 49 else 6)))
print("\n".join(out))
