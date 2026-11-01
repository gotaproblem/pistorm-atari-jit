#!/usr/bin/env python3
"""
analyse.py - read a PISTORM_RASTER_TRACE file and say where the colour
changes landed on the real screen, frame by frame.

    python3 tools/raster-trace/analyse.py /home/pistorm/raster.txt

Frames are split at each VBL (level 4 interrupt). A write's line is
(video counter - screen base) / 160 (low resolution, 160 bytes a line).
Games double-buffer, and the base read at a VBL is not always the screen
on show, so each frame's base is the one of all the bases seen that puts
its palette writes on the screen.

The frames are grouped by their palette-write pattern; for the biggest
group every write is compared across frames. A raster that holds has one
line per write; a chain that starts on a different line moves every
write by the same amount - a SHIFT - and the frames that shift are
compared with the ones that do not: Timer B's state at the VBL and the
MFP writes between the VBL and the first band.
"""
import collections
import statistics
import sys

LINE, LINES = 160, 200


def load(path):
    frames, cur = [], None
    for ln in open(path):
        if ln.startswith('#') or not ln.strip():
            continue
        p = ln.split()
        t = float(p[0])
        if p[1] == 'I' and p[2] == '4':
            cur = {'t': t, 'base': int(p[3], 16) if len(p) > 3 else None,
                   'tb': (int(p[4], 16), int(p[5], 16)) if len(p) > 5 else None,
                   'w': [], 'i6': [], 'v6': [], 'm': []}
            frames.append(cur)
        elif cur is None:
            continue
        elif p[1] == 'I':
            if p[2] == '6':
                cur['i6'].append(t)
                cur['v6'].append((t, int(p[3], 16) if len(p) > 3 else -1))
        elif p[1] == 'P':
            cur['w'].append((t, int(p[2]), int(p[3], 16),
                             None if p[4] == '-' else int(p[4], 16)))
        elif p[1] == 'M':
            cur['m'].append((t, p[2], int(p[3], 16),
                             None if p[4] == '-' else int(p[4], 16)))
        # 'L' (IPL seen) and 'G' (governor) are read by timing()
    return frames


def pick_base(f, bases):
    vs = [w[3] for w in f['w']]
    if not vs:
        return f['base']
    def score(b):
        inside = sum(0 <= v - b < LINE * (LINES + 5) for v in vs)
        first = min([v - b for v in vs if v >= b] or [1 << 30])
        return (inside, -first)
    return max(bases, key=score)


def line(f, vc):
    off = vc - f['b']
    return off // LINE if off >= 0 else -1


def q(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))]


def main(path):
    frames = load(path)
    if len(frames) < 4:
        print("fewer than 4 VBLs in the trace - nothing to compare")
        return 1
    for a, b in zip(frames, frames[1:]):
        b['dt'] = b['t'] - a['t']
    frames = frames[1:-1]
    light = all(w[3] is None for f in frames for w in f['w'])
    if light:
        # PISTORM_RASTER_VCOUNT off: no screen positions, only timing
        gaps = [f['dt'] for f in frames]
        print(f"frames {len(frames)}, VBL interval us min {min(gaps):.0f} median "
              f"{statistics.median(gaps):.0f} max {max(gaps):.0f} (50 Hz = 20032); "
              f"no video counter in this trace (PISTORM_RASTER_VCOUNT off)")
        vecs = collections.Counter(v for f in frames for _, v in f['v6'])
        print("level-6 vectors (MFP: $48 Timer B, $4D Timer A, $44 Timer D, $45 "
              "Timer C, $46 ACIA): " + ' '.join(f"${v:02X}:{c}" for v, c in vecs.most_common(8)))
        timing(path, frames, [])
        return 0
    bases = sorted(set(f['base'] for f in frames))
    for f in frames:
        f['b'] = pick_base(f, bases)

    gaps = [f['dt'] for f in frames]
    print(f"frames {len(frames)}, VBL interval us min {min(gaps):.0f} median "
          f"{statistics.median(gaps):.0f} max {max(gaps):.0f} (50 Hz = 20032); "
          f"screen bases " + ' '.join('%06X' % b for b in bases))

    lat = []
    for f in frames:
        ws = [w[0] for w in f['w']]
        for t6 in f['i6']:
            nxt = [t for t in ws if t >= t6]
            if nxt and nxt[0] - t6 < 200:
                lat.append(nxt[0] - t6)
    if lat:
        print(f"level-6 interrupt -> next palette write, us: median {q(lat,.5):.1f} "
              f"95% {q(lat,.95):.1f} max {max(lat):.1f} ({len(lat)} pairs)")

    # the biggest group of frames with the same palette pattern
    for f in frames:
        f['sig'] = tuple((w[1], w[2]) for w in f['w'][:24])
    grp = collections.Counter(f['sig'] for f in frames if f['w'])
    sig, n = grp.most_common(1)[0]
    g = [f for f in frames if f['sig'] == sig]
    print(f"\nbiggest pattern: {n} frames, {len(sig)}+ writes each "
          f"(first: " + ' '.join(f"r{r}={v:03X}" for r, v in sig[:6]) + " ...)")

    k = min(len(f['w']) for f in g)
    cols = [[line(f, f['w'][i][3]) for f in g] for i in range(k)]
    mode = [collections.Counter(c).most_common(1)[0][0] for c in cols]
    print(" write  reg val   usual line   frames on it   off by (lines:frames)")
    for i in range(k):
        c = collections.Counter(x - mode[i] for x in cols[i])
        off = ' '.join(f"{d:+d}:{m}" for d, m in sorted(c.items()) if d)
        r, v = g[0]['w'][i][1], g[0]['w'][i][2]
        print(f"  {i:3d}  r{r:<2d} {v:03X}   {mode[i]:4d}        "
              f"{c[0]:5d}          {off}")

    # a shift moves the whole chain: take the first write below the top
    # border (the VBL handler's writes all read line 0)
    ref = next((i for i in range(k) if mode[i] > 0), 0)
    for f in g:
        f['shift'] = line(f, f['w'][ref][3]) - mode[ref]
    sh = collections.Counter(f['shift'] for f in g)
    print("\nwhole-chain shift (lines:frames): " +
          ' '.join(f"{d:+d}:{m}" for d, m in sorted(sh.items())))

    vecs = collections.Counter(v for f in frames for _, v in f['v6'])
    if vecs and -1 not in vecs:
        print("\nlevel-6 vectors (MFP: $48 Timer B, $4D Timer A, $44 Timer D, $45 "
              "Timer C, $46 ACIA): " + ' '.join(f"${v:02X}:{c}" for v, c in vecs.most_common(8)))

    def irqs_before(f):
        first = f['w'][ref][0]
        return collections.Counter(v for t, v in f['v6'] if t < first)

    def tb_story(f):
        first = f['w'][ref][0]
        return tuple((reg, val, line(f, vc)) for t, reg, val, vc in f['m'] if t < first)

    for name, sel in (("held", [f for f in g if f['shift'] == 0]),
                      ("shifted", [f for f in g if f['shift'] != 0])):
        if not sel:
            continue
        tb = collections.Counter(f['tb'] for f in sel if f['tb'])
        story = collections.Counter(tb_story(f) for f in sel)
        print(f"\n{name} frames ({len(sel)}): Timer B at the VBL (TBCR,count:frames) "
              + (' '.join(f"{a:02X},{b}:{c}" for (a, b), c in tb.most_common(6)) or "n/a"))
        ib = collections.Counter()
        for f in sel:
            ib.update(irqs_before(f))
        if ib:
            print("  level-6 interrupts from the VBL to the first band, per frame: " +
                  ' '.join(f"${v:02X}:{c / len(sel):.1f}" for v, c in ib.most_common(6)))
        print("  MFP writes from the VBL to the first band (reg=val@line), most common:")
        for s, c in story.most_common(3):
            print(f"   {c:5d}x  " + (' '.join(f"{r}={v:02X}@{l}" for r, v, l in s) or "(none)"))

    timing(path, frames, bases)

    white = collections.Counter(int(f['t'] / 1e6) for f in frames
                                for w in f['w'] if (w[2] & 0x777) == 0x777)
    if white:
        print("\nwhite (777) writes per second (s:count): " +
              ' '.join(f"{s}:{c}" for s, c in sorted(white.items())))
    return 0


def timing(path, frames, bases):
    """Delivery and handler timing, from the raw events (IPL seen by
    ipl_task 'L', interrupt taken 'I', palette 'P', governor 'G')."""
    ev = []
    for ln in open(path):
        if ln.startswith('#') or not ln.strip():
            continue
        p = ln.split()
        ev.append((float(p[0]), p))
    if not any(p[1] in ('L', 'G') for _, p in ev):
        return
    # 1. IPL 6 seen -> level 6 taken
    seen, deliv = None, []
    # 2. Timer B taken -> next palette write; governor waits inside it
    tb, tb_lat, tb_x, waits_in, skips_in = None, [], [], 0, 0
    # 3. the line ended (IPL 6 seen) -> colour written, for Timer B
    tb_seen, tb_seen_seen, end2col = None, None, []
    gw = gs = 0
    # 4. W records (0037): what the CPU thread did between the two
    wrec, bands = None, []
    nested = 0
    for t, p in ev:
        k = p[1]
        if k == 'L':
            seen = t if p[2] == '6' else None
        elif k == 'W':
            wrec = (t, [int(x) for x in p[2:10]])
        elif k == 'I' and tb is not None:
            nested += 1                    # taken inside a Timer B window
            if p[2] == '6':
                is_tb = len(p) > 3 and p[3] == '48'
                tb_seen_seen = seen if is_tb else None
                if seen is not None:
                    deliv.append(t - seen)
                    seen = None
                tb = t if is_tb else None
                tb_i = t
        elif k == 'I' and p[2] == '6':
            is_tb = len(p) > 3 and p[3] == '48'
            tb_seen_seen = seen if is_tb else None
            tb_l = seen
            tb_i = t
            wrec = None
            if seen is not None:
                deliv.append(t - seen)
                seen = None
            tb = t if is_tb else None
        elif k == 'G':
            if p[2] == 'wait':
                gw += 1
                if tb is not None:
                    waits_in += 1
            else:
                gs += 1
                if tb is not None:
                    skips_in += 1
        elif k == 'P' and tb is not None:
            tb_lat.append(t - tb)
            if tb_seen_seen is not None:
                end2col.append(t - tb_seen_seen)
                if wrec is not None:
                    bands.append((t - tb_seen_seen, tb - tb_seen_seen, wrec[1]))
            wrec = None
            if p[4] != '-':
                vc = int(p[4], 16)
                inside = [b for b in bases if 0 <= vc - b < LINE * LINES]
                if inside:
                    tb_x.append((vc - max(inside)) % LINE)
            tb = None
            tb_seen_seen = None
    print("\ntiming")
    if deliv:
        print(f"  IPL 6 seen by ipl_task -> CPU takes it, us: median {q(deliv,.5):.1f} "
              f"95% {q(deliv,.95):.1f} 99% {q(deliv,.99):.1f} max {max(deliv):.1f} ({len(deliv)})")
    if tb_lat:
        print(f"  Timer B taken -> colour written, us: median {q(tb_lat,.5):.1f} "
              f"95% {q(tb_lat,.95):.1f} 99% {q(tb_lat,.99):.1f} max {max(tb_lat):.1f}")
    if end2col:
        n = len(end2col)
        ok = sum(x <= 12 for x in end2col)
        mid = sum(12 < x <= 24 for x in end2col)
        late = n - ok - mid
        print(f"  line ended (IPL 6 seen) -> colour written, us: median {q(end2col,.5):.1f} "
              f"95% {q(end2col,.95):.1f} 99% {q(end2col,.99):.1f} max {max(end2col):.1f}\n"
              f"    within 12 us (a real ST): {100 * ok / n:.1f}%   12-24 us (still in the "
              f"border): {100 * mid / n:.1f}%   over 24 us (into the picture, the wobble): "
              f"{100 * late / n:.1f}%")
    if tb_x:
        h = collections.Counter(min(9, x // 16) for x in tb_x)
        print("  where along the line the colour changed (0% = left edge of the picture):\n    "
              + ' '.join(f"{i * 10}%:{100 * h[i] / len(tb_x):.0f}%" for i in range(10)))
    if bands:
        breakdown(bands)
    grid(ev)
    print(f"  governor pauses within 100 us of a Timer B interrupt: {gw} taken, {gs} skipped "
          f"(handler running); between a Timer B interrupt and its colour write: "
          f"{waits_in} taken, {skips_in} skipped")


# a PAL ST line: 512 clocks of 32.084988 MHz / 4 = 8.021247 MHz (Hatari's
# figure). 8.0107 (first version, 0039) is 0.13% off - over 160 lines that
# is 14 us, which showed up as interrupts "seen late" growing down the frame.
ST_LINE_US = 512 / 8.021247


def grid(ev):
    """Timer B fires on an exact line grid: within a frame the true
    interrupts are 63.9 us apart. The earliest 'IPL seen' of the frame
    stands for the real interrupt; everything else is measured from that
    grid, so a late sampler cannot hide in its own reference point.
    On a real ST (Hatari, Shadow of the Beast II) the band colour write
    comes ~19 us after the interrupt and the picture starts ~20.5 us
    after it."""
    frames, cur, seen, tb = [], None, None, None
    for t, p in ev:
        k = p[1]
        if k == 'I' and p[2] == '4':
            cur = []
            frames.append(cur)
            seen = None
        elif cur is None:
            continue
        elif k == 'L':
            seen = t
        elif k == 'I' and p[2] == '6':
            tb = (seen, t) if len(p) > 3 and p[3] == '48' else None
            seen = None
        elif k == 'P' and tb is not None:
            if tb[0] is not None:
                cur.append((tb[0], t))
            tb = None
    late_l, pos = [], []
    for fr in frames:
        if len(fr) < 8:
            continue
        t0 = fr[0][0]
        rs = []
        for l, p in fr:
            k = round((l - t0) / ST_LINE_US)
            rs.append((l - t0 - k * ST_LINE_US, p - t0 - k * ST_LINE_US))
        a = min(r[0] for r in rs)
        late_l += [r[0] - a for r in rs]
        pos += [r[1] - a for r in rs]
    if not pos:
        return
    n = len(pos)
    inpic = [x for x in pos if x > 20.5]
    print(f"\n  against the line grid ({n} Timer B colour writes):\n"
          f"    IPL seen after the interrupt, us: median {q(late_l,.5):.1f} 95% {q(late_l,.95):.1f} "
          f"max {max(late_l):.1f}  (sampler + confirm; a real CPU: 0)\n"
          f"    colour written after the interrupt, us: median {q(pos,.5):.1f} 5% {q(pos,.05):.1f} "
          f"95% {q(pos,.95):.1f}  (a real ST: ~19)\n"
          f"    landed in the picture (after 20.5 us): {100 * len(inpic) / n:.1f}%, "
          f"median {q(inpic,.5) - 20.5 if inpic else 0:.1f} us = ~{8 * (q(inpic,.5) - 20.5) if inpic else 0:.0f} px in")


def band_parts(b):
    """One Timer B write's delay split into its parts, us. b = (line ended
    -> colour written, line ended -> taken, W record fields)."""
    l2p, l2i, w = b
    tot, clk, bn, bns, bmax, yld, gap, gov = w
    return {'IPL seen -> taken': l2i,
            'taken -> handler (IACK, frame)': max(0.0, l2p - l2i - tot / 1000),
            'bus accesses': bns / 1000,
            'waiting for the bus (other thread)': yld / 1000,
            'governor pauses': gov / 1000,
            'between bus accesses (CPU thread)': max(0.0, (tot - bns - yld - gov) / 1000),
            }


def band_blame(b, med):
    """the part of b most over its on-time median"""
    pb = band_parts(b)
    return max(pb, key=lambda n: pb[n] - med[n])


def breakdown(bands):
    """Where the time went for each Timer B colour write (W records):
    on-time bands against the late ones (over 24 us after the line ended:
    into the picture). Times in us."""
    parts = band_parts
    late = [b for b in bands if b[0] > 24]
    ok = [b for b in bands if b[0] <= 24]
    print(f"\n  where the time goes, line ended -> colour written ({len(bands)} Timer B "
          f"writes, {len(late)} late = over 24 us)")
    names = list(parts(bands[0]).keys())
    print(f"    {'':38s} {'on time: median':>16s} {'95%':>6s}   {'late: median':>13s} {'95%':>6s}")
    for n in names:
        a = [parts(b)[n] for b in ok]
        z = [parts(b)[n] for b in late]
        print(f"    {n:38s} {q(a,.5) if a else 0:16.1f} {q(a,.95) if a else 0:6.1f}   "
              f"{q(z,.5) if z else 0:13.1f} {q(z,.95) if z else 0:6.1f}")
    for n, ix, div in (('guest 68000 clocks run', 1, 1), ('bus accesses (count)', 2, 1),
                       ('longest bus access, us', 4, 1000),
                       ('longest stretch without one, us', 6, 1000)):
        a = [b[2][ix] / div for b in ok]
        z = [b[2][ix] / div for b in late]
        print(f"    {n:38s} {q(a,.5) if a else 0:16.1f} {q(a,.95) if a else 0:6.1f}   "
              f"{q(z,.5) if z else 0:13.1f} {q(z,.95) if z else 0:6.1f}")
    if late and ok:
        med = {n: q([parts(b)[n] for b in ok], .5) for n in names}
        blame = collections.Counter(band_blame(b, med) for b in late)
        print("    late writes, the part most over its on-time median:")
        for n, c in blame.most_common():
            print(f"      {n:38s} {c:6d}  ({100 * c / len(late):.0f}%)")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
