#!/usr/bin/env python3
"""Where do HBL and Timer B interrupts go missing?  Reads a
PISTORM_IPL_TIMELINE=2 log (plain or .gz).

    tools/ipl-timeline/hbl.py ipl-timeline.log[.gz]

HBL. The CPU records every HBL it takes ('X 26'). While the regression
suite's irq_hbl check runs (SR $2100 for a second) consecutive HBLs are
one line apart; a gap of k lines means k-1 never reached the CPU. Runs
are split only at gaps over 8 ms, so one run is the whole check. Each gap
is put in the first category that fits:

  stuck        after the HBL before the gap was taken, ipl_task did not see
               level 2 leave for more than 1.5 lines, and nothing else was
               taken meanwhile: the line read 2 the whole time, and av_held
               (one delivery per assertion) refused it
  other-irq    a VBL or MFP interrupt was taken in the gap; the HBL rose
               and fell while the CPU's mask was above 2
  cpu          level 2 latched at least once per missing line, but the CPU
               took fewer
  not-latched  seen going back to 2 once per missing line, not latched
               (the B records say why)
  drop-short   not seen leaving 2: it left, but came back before the
               confirm time (C records)
  drop-unseen  a whole assertion was not seen at all

TIMER B. In the tmr_b_event check Timer B (MFP vector $48) is in event
count mode, one interrupt per display line, 200 per frame (frames are
between VBLs, 'X 28'). Frames with fewer are broken down the same way,
for level 6.
"""
import gzip
import sys
from collections import Counter, defaultdict
from statistics import median

STUCK_LINES = 1.5


def parse(fn):
    ev = []
    op = gzip.open if fn.endswith(".gz") else open
    with op(fn, "rt", errors="replace") as f:
        for ln in f:
            if ln.startswith("#"):
                if "overran" in ln:
                    print("WARNING:", ln.strip())
                continue
            p = ln.split()
            if len(p) < 3:
                continue
            try:
                t = float(p[0])
            except ValueError:
                continue
            k = p[1]
            raw = ln.rstrip()
            try:
                if k == "I":
                    a, b = (int(x) for x in p[2].split("->"))
                    ev.append((t, "I", a, b, p[3], raw))
                elif k == "C":
                    c, n = (int(x) for x in p[3].split("->"))
                    ev.append((t, "C", int(p[2]), c, n, float(p[4]), raw))
                elif k == "B":
                    ev.append((t, "B", int(p[2]), p[3], raw))
                elif k == "X":
                    ev.append((t, "X", int(p[2]), p[3].upper(), int(p[5], 16), raw))
            except (ValueError, IndexError):
                pass
    return ev


def dist(name, v):
    if v:
        v = sorted(v)
        print("  %-40s n=%d min %.1f  median %.1f  p99 %.1f  max %.1f us"
              % (name, len(v), v[0], v[len(v) // 2],
                 v[min(len(v) - 1, len(v) * 99 // 100)], v[-1]))


def runs_of(ev, idx, split_us, min_n):
    if not idx:
        return []
    runs, cur = [], [idx[0]]
    for i in idx[1:]:
        if ev[i][0] - ev[cur[-1]][0] > split_us:
            runs.append(cur)
            cur = []
        cur.append(i)
    runs.append(cur)
    return [r for r in runs if len(r) >= min_n]


def gap_counts(seg, lvl):
    """what ipl_task saw of level lvl between two deliveries"""
    g = dict(drops=0, rises=0, latches=0, cshort=0, xs=[], breason=Counter())
    for e in seg:
        if e[1] == "I" and e[4] == "line":
            if e[2] == lvl and e[3] != lvl:
                g["drops"] += 1
            elif e[3] == lvl:
                g["rises"] += 1
        elif e[1] == "I" and e[4].startswith("LATCH") and e[3] == lvl:
            g["latches"] += 1
        elif e[1] == "C" and e[2] == lvl and e[3] != lvl:
            g["cshort"] += 1
        elif e[1] == "B" and e[2] == lvl:
            g["breason"][e[3]] += 1
        elif e[1] == "X":
            g["xs"].append(e)
    return g


def classify(g, k, is_delivery):
    others = [x for x in g["xs"] if not is_delivery(x)]
    if others:
        return "other-irq", others
    if g["latches"] >= k:
        return "cpu", others
    if g["drops"] >= k and g["rises"] >= k:
        return "not-latched", others
    if g["cshort"]:
        return "drop-short", others
    return "drop-unseen", others


def show_examples(ev, examples, line):
    for cat, exs in examples.items():
        print("  example gaps, %s:" % cat)
        for i0, i1, k in exs:
            t0 = ev[i0][0]
            print("    %d lines:" % k)
            if i1 - i0 > 30:                       # long: first and last events
                sel = list(range(i0, i0 + 12)) + [None] + list(range(i1 - 11, i1 + 1))
            else:
                sel = range(i0, i1 + 1)
            for i in sel:
                if i is None:
                    print("      ...")
                    continue
                print("      +%8.1f  %s" % (ev[i][0] - t0, ev[i][-1]))


# ------------------------------------------------------------------ HBL
def hbl(ev):
    idx = [i for i, e in enumerate(ev) if e[1] == "X" and e[2] == 26]
    runs = runs_of(ev, idx, 8000, 1000)
    if not runs:
        print("HBL: no run of 1000+ HBLs taken")
        return False
    for run in runs:
        hbl_run(ev, run)
    return True


def last_latch(ev, i0, lvl):
    """time from the latch of the delivery at i0 to the delivery"""
    for b in range(i0 - 1, max(0, i0 - 40), -1):
        e = ev[b]
        if e[1] == "I" and e[4].startswith("LATCH") and e[3] == lvl:
            return ev[i0][0] - e[0]
        if e[1] == "X":
            return None
    return None


def hbl_run(ev, run):
    t = [ev[i][0] for i in run]
    d = [b - a for a, b in zip(t, t[1:])]
    m = median(d)
    ks = [max(1, round(x / m)) for x in d]
    line = (t[-1] - t[0]) / sum(ks)
    nlines = sum(ks) + 1
    missing = nlines - len(run)
    print("HBL run at %.0f us: %d taken over %.1f ms, line %.2f us -> %d lines,"
          " %d missing (%.2f%%)" % (t[0], len(run), (t[-1] - t[0]) / 1000,
                                     line, nlines, missing, 100.0 * missing / nlines))
    lost, gaps, other, breason = Counter(), Counter(), Counter(), Counter()
    examples = defaultdict(list)
    low, rise2x, latch2x, ok_lat = [], [], [], []
    st_len, st_end, st_c, st_lat, st_pc = [], Counter(), 0, [], Counter()
    for j, k in enumerate(ks):
        i0, i1 = run[j], run[j + 1]
        seg = ev[i0 + 1:i1]
        lat = last_latch(ev, i0, 2)
        if k <= 1:
            if lat is not None:
                ok_lat.append(lat)
            td = tr = tl = None
            for e in seg:
                if e[1] == "I" and e[4] == "line":
                    if e[2] == 2 and e[3] != 2:
                        td = e[0]
                    elif e[3] == 2:
                        tr = e[0]
                elif e[1] == "I" and e[4].startswith("LATCH") and e[3] == 2:
                    tl = e[0]
            if td is not None and tr is not None and tr > td:
                low.append(tr - td)
            if tr is not None:
                rise2x.append(ev[i1][0] - tr)
            if tl is not None:
                latch2x.append(ev[i1][0] - tl)
            continue
        gaps[k] += 1
        # stuck: level 2 not seen leaving for STUCK_LINES, nothing else taken
        first_leave = None
        taken_first = False
        for e in seg:
            if e[1] == "I" and e[4] == "line" and e[2] == 2 and e[3] != 2:
                first_leave = e
                break
            if e[1] == "X":
                taken_first = True
                break
        end = first_leave[0] if first_leave else ev[i1][0]
        stuck = not taken_first and end - ev[i0][0] > STUCK_LINES * line
        g = gap_counts(seg, 2)
        for r, n in g["breason"].items():
            breason[r] += n
        if stuck:
            cat = "stuck"
            st_len.append(end - ev[i0][0])
            st_end["2->%d" % first_leave[3] if first_leave else "never"] += 1
            if any(e[1] == "C" and e[2] == 2 and e[0] < end for e in seg):
                st_c += 1
            if lat is not None:
                st_lat.append(lat)
            st_pc[ev[i0][4]] += 1
        else:
            cat, others = classify(g, k, lambda x: x[2] == 26)
            for x in others:
                other["%d/%s" % (x[2], x[3])] += 1
        lost[cat] += k - 1
        if len(examples[cat]) < 3:
            examples[cat].append((i0, i1, k))
    total = sum(lost.values()) or 1
    print("  gaps (lines between HBLs taken: count): %s"
          % ", ".join("%d:%d" % kv for kv in sorted(gaps.items())))
    print("  missing HBLs by cause:")
    for cat in ("stuck", "other-irq", "cpu", "not-latched", "drop-short", "drop-unseen"):
        if lost[cat]:
            print("    %-12s %6d  %5.1f%%" % (cat, lost[cat], 100.0 * lost[cat] / total))
    if st_len:
        print("  stuck episodes: %d, %d with a short drop (C record) first"
              % (len(st_len), st_c))
        dist("stuck: HBL taken -> level 2 seen leaving", st_len)
        print("  stuck: ended by %s" % ", ".join("%s:%d" % kv for kv in st_end.most_common()))
        dist("stuck: latch -> take of the HBL before", st_lat)
        dist("normal: latch -> take", ok_lat)
        print("  stuck: pc of the HBL before: %s"
              % ", ".join("%06X:%d" % kv for kv in st_pc.most_common(6)))
    if other:
        print("  other interrupts in other-irq gaps (nr/vector: count): %s"
              % ", ".join("%s:%d" % kv for kv in sorted(other.items())))
    if breason:
        print("  level 2 not latched in gaps because: %s"
              % ", ".join("%s:%d" % kv for kv in breason.most_common()))
    print("  normal lines (one line apart):")
    dist("level 2 seen low (2->0 .. 0->2)", low)
    dist("seen back at 2 -> CPU took it", rise2x)
    dist("latched -> CPU took it", latch2x)
    show_examples(ev, examples, line)


# ------------------------------------------------------------------ Timer B
def timer_b(ev):
    vbl = [i for i, e in enumerate(ev) if e[1] == "X" and e[2] == 28]
    frames = []
    for a, b in zip(vbl, vbl[1:]):
        tb = [i for i in range(a + 1, b) if ev[i][1] == "X" and ev[i][2] == 30
              and ev[i][3] == "48"]
        frames.append((a, b, tb))
    ev_frames = [f for f in frames if 150 <= len(f[2]) <= 260]
    if not ev_frames:
        print("TIMER B: no event-count frames (150..260 Timer B per VBL)")
        return
    counts = Counter(len(f[2]) for f in ev_frames)
    print("TIMER B event-count frames: %d; Timer B per frame (count: frames): %s"
          % (len(ev_frames), ", ".join("%d:%d" % kv for kv in sorted(counts.items()))))
    d = []
    for _, _, tb in ev_frames:
        d += [ev[b][0] - ev[a][0] for a, b in zip(tb, tb[1:])]
    line = median(d)
    lost, other, where = Counter(), Counter(), Counter()
    examples = defaultdict(list)
    lat_ok, lat_gap = [], []
    for a, b, tb in ev_frames:
        for i in tb:
            l_ = last_latch(ev, i, 6)
            if l_ is not None:
                lat_ok.append(l_)
        if len(tb) >= 200:
            continue
        for j, (i0, i1) in enumerate(zip(tb, tb[1:])):
            k = round((ev[i1][0] - ev[i0][0]) / line)
            if k < 2:
                continue
            g = gap_counts(ev[i0 + 1:i1], 6)
            cat, others = classify(g, k, lambda x: x[2] == 30 and x[3] == "48")
            for x in others:
                other["%d/%s" % (x[2], x[3])] += 1
            lost[cat] += k - 1
            where[j + 1] += 1
            if len(examples[cat]) < 3:
                examples[cat].append((i0, i1, k))
    print("  line %.2f us" % line)
    dist("Timer B latched -> CPU took it", lat_ok)
    if lost:
        total = sum(lost.values())
        print("  missing Timer B by cause:")
        for cat in ("other-irq", "cpu", "not-latched", "drop-short", "drop-unseen"):
            if lost[cat]:
                print("    %-12s %6d  %5.1f%%" % (cat, lost[cat], 100.0 * lost[cat] / total))
        print("  gap after the n-th Timer B of the frame (n: count): %s"
              % ", ".join("%d:%d" % kv for kv in sorted(where.items())))
        if other:
            print("  other interrupts in gaps (nr/vector: count): %s"
                  % ", ".join("%s:%d" % kv for kv in sorted(other.items())))
        show_examples(ev, examples, line)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ev = parse(sys.argv[1])
    ok = hbl(ev)
    timer_b(ev)
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
