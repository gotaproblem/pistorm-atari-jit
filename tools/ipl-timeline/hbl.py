#!/usr/bin/env python3
"""Where do HBL interrupts go missing?  Reads a PISTORM_IPL_TIMELINE=2 log.

    tools/ipl-timeline/hbl.py ipl-timeline.log

The CPU records every HBL it takes ('X 26'). Inside a run of HBLs (the
regression suite's irq_hbl check: SR $2100 for a second) consecutive ones
are one line apart. A gap of k lines means k-1 HBLs never reached the CPU.
For each gap this counts what ipl_task saw between the two HBLs, and puts
the loss in the first category that fits:

  other-irq    a VBL or MFP interrupt was taken in the gap - the HBL is held
               behind it and merges with the next one (a real ST does this
               too; Hatari: ~1% of lines)
  cpu          ipl_task latched level 2 at least once per missing line, but
               the CPU took fewer
  not-latched  the line was seen going back to 2 for each missing line, but
               not latched (the B records say why)
  drop-short   the line was not seen leaving 2: it did leave, but came back
               before the confirm time (C records: candidate 0 dropped)
  drop-unseen  the line was not seen leaving 2 at all between the two HBLs
"""
import sys
from collections import Counter, defaultdict
from statistics import median


def parse(fn):
    ev = []
    with open(fn, errors="replace") as f:
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
                    ev.append((t, "X", int(p[2]), p[3], int(p[4]), raw))
            except (ValueError, IndexError):
                pass
    return ev


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ev = parse(sys.argv[1])
    hbl = [i for i, e in enumerate(ev) if e[1] == "X" and e[2] == 26]
    if len(hbl) < 100:
        sys.exit("only %d HBLs taken in the log - was it PISTORM_IPL_TIMELINE=2,"
                 " and did the irq test run?" % len(hbl))
    # runs: HBLs no more than 2 ms apart
    runs, cur = [], [hbl[0]]
    for i in hbl[1:]:
        if ev[i][0] - ev[cur[-1]][0] > 2000:
            runs.append(cur)
            cur = []
        cur.append(i)
    runs.append(cur)
    runs = [r for r in runs if len(r) >= 1000]
    if not runs:
        sys.exit("no run of 1000+ HBLs")
    for r in runs:
        report(ev, r)


def report(ev, run):
    t = [ev[i][0] for i in run]
    d = [b - a for a, b in zip(t, t[1:])]
    span = t[-1] - t[0]
    lines = sum(max(1, round(x / median(d))) for x in d)
    line = span / lines
    lines += 1                                # counting the first
    print("run at %.0f us: %d HBLs taken over %.1f ms, line %.2f us"
          " -> %d lines, %d missing (%.2f%%)"
          % (t[0], len(run), span / 1000, line, lines, lines - len(run),
             100.0 * (lines - len(run)) / lines))
    lost = Counter()
    gaps = Counter()
    other = Counter()
    breason = Counter()
    examples = defaultdict(list)
    low, rise2x, latch2x = [], [], []
    for j in range(len(run) - 1):
        i0, i1 = run[j], run[j + 1]
        k = round((ev[i1][0] - ev[i0][0]) / line)
        seg = ev[i0 + 1:i1]
        drops = rises = latches = cshort = 0
        xs = []
        t_drop = t_rise = t_latch = None
        for e in seg:
            if e[1] == "I" and e[4] == "line":
                if e[2] == 2 and e[3] != 2:
                    drops += 1
                    t_drop = e[0]
                elif e[3] == 2:
                    rises += 1
                    t_rise = e[0]
            elif e[1] == "I" and e[4].startswith("LATCH") and e[3] == 2:
                latches += 1
                t_latch = e[0]
            elif e[1] == "C" and e[2] == 2 and e[3] < 2:
                cshort += 1
            elif e[1] == "B" and e[2] == 2:
                if k >= 2:
                    breason[e[3]] += 1
            elif e[1] == "X":
                xs.append(e[2])
        if k <= 1:
            if t_drop is not None and t_rise is not None and t_rise > t_drop:
                low.append(t_rise - t_drop)
            if t_rise is not None:
                rise2x.append(ev[i1][0] - t_rise)
            if t_latch is not None:
                latch2x.append(ev[i1][0] - t_latch)
            continue
        miss = k - 1
        gaps[k] += 1
        if xs:
            cat = "other-irq"
            for x in xs:
                other[x] += 1
        elif latches >= k:
            cat = "cpu"
        elif drops >= k and rises >= k:
            cat = "not-latched"
        elif cshort:
            cat = "drop-short"
        else:
            cat = "drop-unseen"
        lost[cat] += miss
        if len(examples[cat]) < 4:
            examples[cat].append((i0, i1, k, drops, rises, latches, cshort))
    total = sum(lost.values())
    print("  gaps (lines between HBLs taken: count): %s"
          % ", ".join("%d:%d" % kv for kv in sorted(gaps.items())))
    print("  missing HBLs by cause:")
    for cat in ("other-irq", "cpu", "not-latched", "drop-short", "drop-unseen"):
        if lost[cat]:
            print("    %-12s %6d  %5.1f%%" % (cat, lost[cat], 100.0 * lost[cat] / total))
    if other:
        print("  other interrupts in gaps (vector nr: count): %s"
              % ", ".join("%d:%d" % kv for kv in sorted(other.items())))
    if breason:
        print("  level 2 not latched in gaps because: %s"
              % ", ".join("%s:%d" % kv for kv in breason.most_common()))

    def dist(name, v):
        if v:
            v = sorted(v)
            print("  %-34s n=%d min %.1f  median %.1f  p99 %.1f  max %.1f us"
                  % (name, len(v), v[0], v[len(v) // 2],
                     v[min(len(v) - 1, len(v) * 99 // 100)], v[-1]))
    print("  normal lines (one line apart):")
    dist("level 2 seen low (2->0 .. 0->2)", low)
    dist("seen back at 2 -> CPU took it", rise2x)
    dist("latched -> CPU took it", latch2x)
    for cat, exs in examples.items():
        print("  example gaps, %s:" % cat)
        for i0, i1, k, dr, ri, la, cs in exs:
            print("    %d lines: drops %d rises %d latches %d short-drops %d"
                  % (k, dr, ri, la, cs))
            t0 = ev[i0][0]
            for e in ev[i0:i1 + 1]:
                print("      +%8.1f  %s" % (e[0] - t0, e[-1]))


if __name__ == "__main__":
    main()
