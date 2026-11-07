#!/usr/bin/env python3
"""check.py TEST ROWSFILE - compare a harness --dump row summary with what a
real ST shows for that raster test ROM. Prints PASS/FAIL and the first
mismatching rows."""
import sys

LV = [0, 36, 73, 109, 146, 182, 219, 255]
PAL16 = [0x000, 0x700, 0x070, 0x007, 0x770, 0x707, 0x077, 0x777,
         0x300, 0x030, 0x003, 0x330, 0x303, 0x033, 0x333, 0x531]

def st(p):
    return (LV[(p >> 8) & 7] << 16) | (LV[(p >> 4) & 7] << 8) | LV[p & 7]

def ste(p):
    f = lambda n: (((n & 7) << 1) | ((n >> 3) & 1)) * 17
    return (f(p >> 8) << 16) | (f(p >> 4) << 8) | f(p)

def load(fn):
    lines = open(fn).read().splitlines()
    w, h = [int(x) for x in lines[0].split()[1].split('x')]
    rows = {}
    for ln in lines[1:]:
        a, b = ln.split(':', 1)
        r0, r1 = [int(x) for x in a.replace('rows', '').replace(' ', '').split('-')]
        runs = []
        t = b.split()
        for i in range(0, len(t), 2):
            x0, x1 = [int(x) for x in t[i + 1].split('-')]
            runs.append((int(t[i], 16), x0, x1))
        for y in range(r0, r1 + 1):
            rows[y] = runs
    return w, h, rows

def solid(c, w):
    return [(c, 0, w - 1)]

def main():
    test, fn = sys.argv[1], sys.argv[2]
    w, h, rows = load(fn)
    want_h = 200
    exp = {}
    if test == 'tb_raster':
        for y in range(200): exp[y] = solid(st(y & 0x777), w)
    elif test == 'cnt_split':
        for y in range(200): exp[y] = solid(st(0x007 if y < 100 else 0x700), w)
    elif test == 'borders':
        want_h = 276
        for y in range(276): exp[y] = solid(st(PAL16[y & 15]), w)
    elif test == 'rez_split':
        for y in range(200):
            c = y & 15
            if y < 150:
                exp[y] = solid(st(PAL16[c]), w)
            else:
                a, b = st(PAL16[c & 3]), st(PAL16[(c >> 2) & 3])
                if a == b:
                    exp[y] = solid(a, w)
                else:
                    exp[y] = [((a if (x // 16) % 2 == 0 else b), x, x + 15) for x in range(0, w, 16)]
    elif test == 'midline':
        # every row 1..198 the same: blue, then red from one fixed x
        ref = rows.get(100)
        ok = ref and any(r[0] == st(0x007) for r in ref) and ref[-1][0] == st(0x700)
        bad = [y for y in range(1, 199) if rows.get(y) != ref]
        if h != 200 or not ok or bad:
            print('FAIL midline: h=%d ref=%s differing rows=%s' % (h, ref, bad[:5])); return 1
        print('PASS midline'); return 0
    elif test == 'ste_split':
        for y in range(100, 200): exp[y] = solid(ste(PAL16[15]), w)
    else:
        print('unknown test'); return 2
    bad = []
    if h != want_h:
        print('FAIL %s: %d rows, want %d' % (test, h, want_h)); return 1
    for y, e in exp.items():
        if rows.get(y) != e:
            bad.append(y)
    if bad:
        print('FAIL %s: %d rows differ, first %s' % (test, len(bad), bad[:5]))
        for y in bad[:3]:
            print('   row %d got %s want %s' % (y, rows.get(y), exp[y][:4]))
        return 1
    print('PASS %s' % test)
    return 0

sys.exit(main())
