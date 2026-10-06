#!/usr/bin/env python3
"""Compare a PiStorm host-port trace (PISTORM_DSP_TRACE dump) with Hatari's
(HATARI_DSPTRACE, see README.md) and report where the words first differ.

Both files have one event per line:  seq ms frame type value x dsp-pc [68k-pc]
The order in which the two sides interleave is a matter of timing, so the
words are compared stream by stream:
  W  68k -> DSP words          S  DSP -> 68k words (as the DSP wrote them)
  R  words the 68k read        C  host commands asked (CVR writes with HC)
  H  host commands taken
The PiStorm dump is a ring (it starts mid-run), so each stream is aligned
on a run of words that occurs only once in Hatari's trace. Hatari's trace
may be tens of millions of lines: only the word values are kept in memory.

usage: dsptrace_cmp.py pistorm.txt hatari.txt [--window 24] [--context 6]
"""
import sys, argparse, collections
from array import array

KINDS = 'WSRCH'
NAMES = {'W': '68k -> DSP words', 'S': 'DSP -> 68k words', 'R': 'words the 68k read',
         'C': 'host commands asked', 'H': 'host commands taken'}

def load(path):
    """per stream: (values, line numbers); and event counts by type"""
    vals = {k: array('I') for k in KINDS}
    lines = {k: array('I') for k in KINDS}
    count = collections.Counter()
    with open(path, 'rb') as f:
        for n, ln in enumerate(f):
            if ln[:1] == b'#':
                continue
            p = ln.split()
            if len(p) < 7:
                continue
            t = p[3].decode()
            count[t] += 1
            try:
                v = int(p[4], 16)
            except ValueError:
                continue
            if t == 'C':
                if not v & 0x80:
                    continue
                v = (v & 0x1F) * 2
            elif t not in 'WSRH':
                continue
            vals[t].append(v)
            lines[t].append(n)
    return vals, lines, count

def fetch(path, want):
    """the text of the given line numbers"""
    want = set(want); out = {}
    with open(path, 'rb') as f:
        for n, ln in enumerate(f):
            if n in want:
                out[n] = ln.decode(errors='replace').rstrip()
                if len(out) == len(want):
                    break
    return out

def occurrences(hay, needle, limit=2):
    """positions (in words) of needle in hay, up to `limit`"""
    hb, nb, pos, out = hay.tobytes(), needle.tobytes(), 0, []
    while len(out) < limit:
        k = hb.find(nb, pos)
        if k < 0:
            break
        if k % 4 == 0:
            out.append(k // 4)
        pos = k + 1
    return out

def align(o, h, win):
    """(i, j): a run of `win` words at o[i:] that occurs once in h, at j"""
    step = max(1, win // 2)
    for i in range(0, max(0, len(o) - win + 1), step):
        js = occurrences(h, o[i:i + win])
        if len(js) == 1:
            return i, js[0]
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pistorm'); ap.add_argument('hatari')
    ap.add_argument('--window', type=int, default=24)
    ap.add_argument('--context', type=int, default=6)
    a = ap.parse_args()
    ov, ol, oc = load(a.pistorm)
    hv, hl, hc = load(a.hatari)
    print('events by type (pistorm / hatari): ' + ', '.join(
        '%s %d/%d' % (t, oc[t], hc[t]) for t in sorted(set(oc) | set(hc))))
    report = []
    for k in KINDS:
        o, h = ov[k], hv[k]
        print('\n== %s: pistorm %d, hatari %d' % (NAMES[k], len(o), len(h)))
        if not o or not h:
            continue
        win, at = min(a.window, len(o)), None
        while win >= 4 and not at:
            at = align(o, h, win)
            if not at:
                win //= 2
        if not at:
            print('   no run of words that occurs once in hatari - cannot align')
            continue
        i, j = at
        n = 0
        lim = min(len(o) - i, len(h) - j)
        while n < lim and o[i + n] == h[j + n]:
            n += 1
        report.append((k, i, j, n, win))
    # one pass over each file for the lines to show
    c = a.context
    wo = [ol[k][x] for k, i, j, n, w in report for x in range(max(0, i + n - c), min(len(ol[k]), i + n + c + 1))]
    wo += [ol[k][i] for k, i, j, n, w in report]
    wh = [hl[k][y] for k, i, j, n, w in report for y in range(max(0, j + n - c), min(len(hl[k]), j + n + c + 1))]
    wh += [hl[k][j] for k, i, j, n, w in report]
    to, th = fetch(a.pistorm, wo), fetch(a.hatari, wh)
    for k, i, j, n, w in report:
        print('\n== %s' % NAMES[k])
        print('   aligned on a run of %d:\n     pistorm %s\n     hatari  %s' % (w, to[ol[k][i]], th[hl[k][j]]))
        if i + n >= len(ov[k]):
            print('   %d words agree to the end of the pistorm trace' % n)
            continue
        if j + n >= len(hv[k]):
            print('   %d words agree to the end of the hatari trace' % n)
            continue
        print('   FIRST DIFFERENCE after %d equal words (pistorm above, hatari below):' % n)
        for d in range(-c, c + 1):
            x, y = i + n + d, j + n + d
            mark = '>>' if d == 0 else '  '
            if 0 <= x < len(ov[k]):
                print('   %s P %s' % (mark, to[ol[k][x]]))
            if 0 <= y < len(hv[k]):
                print('   %s H %s' % (mark, th[hl[k][y]]))

if __name__ == '__main__':
    main()
