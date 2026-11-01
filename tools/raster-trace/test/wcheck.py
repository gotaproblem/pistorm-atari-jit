"""Check analyse.py's per-band blame against the delay wtest injected."""
import sys, collections
sys.path.insert(0, 'tools/raster-trace')
import analyse
names = {1: 'waiting for the bus (other thread)', 2: 'governor pauses',
         3: 'between bus accesses (CPU thread)'}
causes = [int(x) for x in open(sys.argv[2])]
bands, seen, tb, wrec = [], None, None, None
for ln in open(sys.argv[1]):
    if ln[0] == '#':
        continue
    p = ln.split(); t = float(p[0])
    if p[1] == 'L': seen = t
    elif p[1] == 'I' and p[2] == '6': tb = t
    elif p[1] == 'W': wrec = [int(x) for x in p[2:10]]
    elif p[1] == 'P': bands.append((t - seen, tb - seen, wrec))
bands, causes = bands[20:], causes[20:]        # the first bands: threads and qemu warming up
ok = [b for b, c in zip(bands, causes) if c == 0]
med = {n: analyse.q([analyse.band_parts(b)[n] for b in ok], .5)
       for n in analyse.band_parts(ok[0])}
bad = collections.Counter()
for b, c in zip(bands, causes):
    if c and analyse.band_blame(b, med) != names[c]:
        bad[(names[c], analyse.band_blame(b, med))] += 1
n = sum(1 for c in causes if c)
print(f"{n} injected delays, {n - sum(bad.values())} blamed on the right part")
for k, v in bad.items():
    print("  wrong:", k, v)
print("PASS" if not bad else "FAIL")
sys.exit(1 if bad else 0)
