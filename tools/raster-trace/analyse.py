#!/usr/bin/env python3
"""
analyse.py - read a PISTORM_RASTER_TRACE file and say where the colour
changes landed on the real screen, frame by frame.

    python3 tools/raster-trace/analyse.py /home/pistorm/raster.txt

Frames are split at each VBL (level 4 interrupt). A palette write's line
is (video counter - screen base) / 160 - low resolution, 160 bytes a line;
the remainder is how far along the line it landed (bytes, 0-159). The
counter does not move in the borders, so a write there shows as line 0
(top) or 200 (bottom).

For each palette register the k-th write of every frame is compared across
frames: a band that stays put has one line; a band that jumps has a spread.
"""
import collections
import statistics
import sys

LINE = 160
LINES = 200


def load(path):
    frames, cur, irq6, vbls = [], None, [], []
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip():
                continue
            p = ln.split()
            t = float(p[0])
            if p[1] == 'I':
                lvl = int(p[2])
                if lvl == 4:
                    base = int(p[3], 16) if len(p) > 3 else 0
                    cur = {'t': t, 'base': base, 'w': [], 'i6': []}
                    frames.append(cur)
                    vbls.append(t)
                elif lvl == 6:
                    irq6.append(t)
                    if cur:
                        cur['i6'].append(t)
            elif p[1] == 'P' and cur:
                cur['w'].append((t, int(p[2]), int(p[3], 16), int(p[4], 16)))
    return frames, irq6, vbls


def where(base, vc):
    off = vc - base
    if off < 0:
        return 0, 0
    if off >= LINE * LINES:
        return LINES, 0
    return off // LINE, off % LINE


def main(path):
    frames, irq6, vbls = load(path)
    if len(frames) < 3:
        print("fewer than 3 VBLs in the trace - nothing to compare")
        return 1
    frames = frames[1:-1]               # first/last may be partial

    gaps = [b - a for a, b in zip(vbls, vbls[1:])]
    print(f"frames: {len(frames)}   VBL interval us: min {min(gaps):.0f} "
          f"median {statistics.median(gaps):.0f} max {max(gaps):.0f}  (50 Hz = 20032)")

    n6 = [len(f['i6']) for f in frames]
    nw = [len(f['w']) for f in frames]
    print(f"level-6 interrupts per frame: min {min(n6)} median {statistics.median(n6)} max {max(n6)}")
    print(f"palette writes per frame:     min {min(nw)} median {statistics.median(nw)} max {max(nw)}")

    # latency: each level-6 interrupt to the first palette write after it
    lat = []
    for f in frames:
        ws = [w[0] for w in f['w']]
        for t6 in f['i6']:
            nxt = [t for t in ws if t >= t6]
            if nxt and nxt[0] - t6 < 200:
                lat.append(nxt[0] - t6)
    if lat:
        lat.sort()
        q = lambda x: lat[min(len(lat) - 1, int(len(lat) * x))]
        print(f"level-6 interrupt -> next palette write, us: min {lat[0]:.1f} "
              f"median {q(.5):.1f} 95% {q(.95):.1f} max {lat[-1]:.1f}  ({len(lat)} pairs)")

    # bands: k-th write of each register, its line across frames
    by = collections.defaultdict(list)          # (idx, k) -> [(line, x, val)]
    for f in frames:
        k = collections.Counter()
        for t, idx, val, vc in f['w']:
            ln, x = where(f['base'], vc)
            by[(idx, k[idx])].append((ln, x, val))
            k[idx] += 1

    print("\nper band (register, k-th write in the frame): line min..max, spread, "
          "frames seen, values")
    worst = []
    for (idx, k), v in sorted(by.items()):
        if len(v) < len(frames) // 2:
            continue                             # not a per-frame write
        lines = [x[0] for x in v]
        spread = max(lines) - min(lines)
        vals = collections.Counter(x[2] for x in v)
        worst.append(spread)
        top = ' '.join(f"{val:03X}x{c}" for val, c in vals.most_common(3))
        print(f"  reg {idx:2d} #{k:2d}: line {min(lines):3d}..{max(lines):3d} "
              f"spread {spread:3d}  seen {len(v):4d}  {top}")
    if worst:
        print(f"\nbands: {len(worst)}, median spread {statistics.median(worst)} lines, "
              f"worst {max(worst)} lines  (a steady raster is 0-1)")

    # white: writes of 777 (ST) / FFF (STE) and where they landed
    white = [(where(f['base'], w[3])[0], w[1]) for f in frames for w in f['w']
             if (w[2] & 0x777) == 0x777]
    if white:
        regs = collections.Counter(r for _, r in white)
        print(f"white (777) writes: {len(white)} in {len(frames)} frames, registers "
              + ', '.join(f"{r}x{c}" for r, c in regs.most_common()))
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
