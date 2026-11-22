#!/usr/bin/env python3
"""jitter.py - score a raster trace (PISTORM_RASTER_TRACE with
PISTORM_RASTER_VCOUNT=1) for the regression jitter check.

    python3 tools/regress/jitter.py TRACE [WINDOW_S]

Takes the game's commonest palette chain (the most frames with the same
first 24 palette writes - analyse.py's "biggest pattern"), finds each
write's usual line, and calls a frame HELD when every write of the chain
landed on its usual line. Prints

    frames N held H pct P
    window W s: worst M% at T s ... (one line), then the per-window list

where the windows are consecutive WINDOW_S seconds (default 5) from the
trace's first frame; a partial last window is left out of the worst.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raster-trace'))
import analyse  # noqa: E402
from analyse import line, pick_base  # noqa: E402


def main():
    path = sys.argv[1]
    win = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    frames = analyse.load(path)[1:-1]
    frames = [f for f in frames if f['w'] and f['w'][0][3] is not None]
    if len(frames) < 10:
        print('frames 0 held 0 pct 0 - no palette writes with video counter (PISTORM_RASTER_VCOUNT?)')
        return 1
    bases = sorted(set(f['base'] for f in frames))
    for f in frames:
        f['b'] = pick_base(f, bases)
        f['sig'] = tuple((w[1], w[2]) for w in f['w'][:24])
    sig = collections.Counter(f['sig'] for f in frames).most_common(1)[0][0]
    g = [f for f in frames if f['sig'] == sig]
    k = min(len(f['w']) for f in g)
    mode = [collections.Counter(line(f, f['w'][i][3]) for f in g).most_common(1)[0][0]
            for i in range(k)]
    t0 = frames[0]['t']
    held = 0
    bins = collections.defaultdict(lambda: [0, 0])
    for f in g:
        ok = all(line(f, f['w'][i][3]) == mode[i] for i in range(k))
        held += ok
        b = bins[int((f['t'] - t0) / 1e6 // win)]
        b[0] += ok
        b[1] += 1
    last = int((frames[-1]['t'] - t0) / 1e6 // win)
    full = [(i, h, n) for i, (h, n) in sorted(bins.items()) if i < last and n]
    print('frames %d held %d pct %.1f (chain of %d writes, %d of %d frames have it)'
          % (len(g), held, 100.0 * held / len(g), k, len(g), len(frames)))
    if full:
        i, h, n = min(full, key=lambda x: x[1] / x[2])
        print('window %g s: worst %.1f%% at %g s (%d of %d)' % (win, 100.0 * h / n, i * win, h, n))
        print('windows: ' + ' '.join('%g:%.0f' % (i * win, 100.0 * h / n) for i, h, n in full))
    else:
        print('window %g s: none complete' % win)
    return 0


if __name__ == '__main__':
    sys.exit(main())
