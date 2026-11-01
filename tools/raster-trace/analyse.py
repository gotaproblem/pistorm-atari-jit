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
            cur = {'t': t, 'base': int(p[3], 16) if len(p) > 3 else 0,
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
            cur['w'].append((t, int(p[2]), int(p[3], 16), int(p[4], 16)))
        elif p[1] == 'M':
            cur['m'].append((t, p[2], int(p[3], 16), int(p[4], 16)))
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

    white = collections.Counter(int(f['t'] / 1e6) for f in frames
                                for w in f['w'] if (w[2] & 0x777) == 0x777)
    if white:
        print("\nwhite (777) writes per second (s:count): " +
              ' '.join(f"{s}:{c}" for s, c in sorted(white.items())))
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
