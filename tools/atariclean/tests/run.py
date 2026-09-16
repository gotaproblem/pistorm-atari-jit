#!/usr/bin/env python3
"""
tests for atariclean: builds a 720K floppy, its MSA twin, an AHDI hard
disk image (GEM + BGM with 1024-byte sectors + an XGM extended chain) and
a directory tree, all littered the macOS way; runs the tool dry, then
for real, and checks: every litter entry found, nothing else touched
(surviving files byte-identical), clusters really freed, MSA round trip
exact, images left consistent. Writes the cleaned images to out/ so
fsck.vfat -n can be run on them where dosfstools exists.
"""
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import mkfat                     # noqa: E402
import atariclean as ac          # noqa: E402

import tempfile
# a fresh scratch directory each run (in the tree only with --keep, as out/)
OUT = os.path.join(HERE, "out") if "--keep" in sys.argv else tempfile.mkdtemp(prefix="atariclean-test-")
TOOL = os.path.join(HERE, "..", "atariclean.py")
fails = 0


def fail(msg):
    global fails
    fails += 1
    print("FAIL", msg)


def run(*args):
    p = subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout


def litter(v, prefix=""):
    """the macOS pattern: sidecars for everything, plus the folders"""
    v.add(prefix + "README.TXT", b"hello atari\r\n" * 50)
    v.add(prefix + "._README.TXT", b"\x00\x05\x16\x07" + b"\x00" * 4092)
    v.add(prefix + ".DS_Store", b"\x00\x00\x00\x01Bud1" + b"\x00" * 3000)
    v.add(prefix + "GAMES", is_dir=True)
    v.add(prefix + "GAMES/ELITE.PRG", bytes(range(256)) * 40)
    v.add(prefix + "GAMES/._ELITE.PRG", b"\x00\x05\x16\x07" + b"\x00" * 4092)
    v.add(prefix + "GAMES/.DS_Store", b"\x00" * 6148)
    v.add(prefix + ".fseventsd", is_dir=True)
    v.add(prefix + ".fseventsd/fseventsd-uuid", b"3c6f-4bd4" * 4)
    v.add(prefix + ".fseventsd/0000000000a1b2c3", b"\x1f\x8b" + b"\x00" * 600)
    v.add(prefix + ".Trashes", is_dir=True)
    v.add(prefix + ".Trashes/501", is_dir=True)
    v.add(prefix + ".Trashes/501/OLD.TXT", b"binned\r\n")
    v.add(prefix + ".smbdeleteAAA43473", b"")
    v.add(prefix + "DOCS", is_dir=True)
    v.add(prefix + "DOCS/notes.txt", b"lower case, long name ok\r\n" * 20)
    v.add(prefix + "DOCS/._notes.txt", b"\x00\x05\x16\x07" + b"\x00" * 300)
    v.add(prefix + "LONGER~1.TXT", b"x")
    return v


KEEP = {"README.TXT", "GAMES", "GAMES/ELITE.PRG", "DOCS", "DOCS/notes.txt", "LONGER~1.TXT"}
JUNK = {"._README.TXT", ".DS_Store", "GAMES/._ELITE.PRG", "GAMES/.DS_Store", ".fseventsd",
        ".Trashes", ".smbdeleteAAA43473", "DOCS/._notes.txt"}


def tree(fat, first=None, path=""):
    """{path: (is_dir, data)} of a volume via atariclean's own reader"""
    out = {}
    entries = fat.root_entries() if first is None else fat.dir_entries(first)
    for name, is_dir, fc, size, offs in list(fat.listing(entries)):
        if name in (".", ".."):
            continue
        p = (path + "/" + name) if path else name
        if is_dir:
            out[p] = (True, None)
            out.update(tree(fat, fc, p))
        else:
            data = b""
            if fc >= 2:
                for c in fat.chain(fc):
                    o = fat.cluster_off(c)
                    data += bytes(fat.img[o:o + fat.spc * fat.bps])
            out[p] = (False, data[:size])
    return out


def check_volume(before, after, label):
    """after cleaning: junk gone, keepers identical"""
    for p in JUNK:
        if p in after:
            fail("%s: %s survived" % (label, p))
    for p in KEEP:
        if p not in after:
            fail("%s: %s vanished" % (label, p))
        elif after[p] != before[p]:
            fail("%s: %s changed" % (label, p))
    extra = set(after) - KEEP - {".Trashes/501", ".Trashes/501/OLD.TXT", ".fseventsd/fseventsd-uuid",
                                 ".fseventsd/0000000000a1b2c3"}
    if extra - set(before):
        fail("%s: unexpected entries %s" % (label, extra))


def fat_consistent(fat, label):
    """every live chain valid; no cluster referenced twice; freed = free"""
    used = {}
    def walk(first, path):
        entries = fat.root_entries() if first is None else fat.dir_entries(first)
        for name, is_dir, fc, size, offs in list(fat.listing(entries)):
            if name in (".", ".."):
                continue
            if fc >= 2:
                for c in fat.chain(fc):
                    if c in used:
                        fail("%s: cluster %d shared by %s and %s" % (label, c, used[c], path + name))
                    used[c] = path + name
            if is_dir:
                walk(fc, path + name + "/")
    walk(None, "")
    orphans = [c for c in range(2, fat.nclusters + 2) if fat.get(c) != 0 and c not in used]
    if orphans:
        fail("%s: %d orphan clusters (first %s)" % (label, len(orphans), orphans[:5]))
    return len(used)


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT, exist_ok=True)

    # ---- 720K floppy ------------------------------------------------------
    v = litter(mkfat.Volume())
    st = os.path.join(OUT, "test.st")
    open(st, "wb").write(v.img)
    before = tree(ac.Fat(bytearray(v.img), 0, len(v.img)))
    rc, out = run(st)
    print(out.strip().splitlines()[-1])
    if rc != 0 or "8 items" not in out:
        fail("floppy dry run: rc %d, %s" % (rc, out.strip().splitlines()[-1]))
    if open(st, "rb").read() != bytes(v.img):
        fail("floppy dry run modified the image")
    rc, out = run("-d", "-b", st)
    if rc != 0 or "8 removed" not in out:
        fail("floppy delete: rc %d: %s" % (rc, out))
    if not os.path.exists(st + ".bak"):
        fail("no .bak written")
    img = bytearray(open(st, "rb").read())
    fat = ac.Fat(img, 0, len(img))
    after = tree(fat)
    check_volume(before, after, "floppy")
    fat_consistent(fat, "floppy")
    freed = sum(1 for c in range(2, fat.nclusters + 2) if fat.get(c) == 0)
    if freed <= v.free_clusters():
        fail("floppy: no clusters were freed (%d before, %d after)" % (v.free_clusters(), freed))
    rc, out = run(st)
    if "0 items" not in out:
        fail("floppy: second pass still finds litter")
    print("floppy ok: %d clusters free (%d before)" % (freed, v.free_clusters()))

    # ---- MSA twin ----------------------------------------------------------
    msa = os.path.join(OUT, "test.msa")
    hdr = (9, 1, 0, 79)
    enc = ac.msa_encode(bytearray(v.img), hdr)
    dec, h2 = ac.msa_decode(enc)
    if bytes(dec) != bytes(v.img) or h2 != hdr:
        fail("MSA round trip differs")
    open(msa, "wb").write(enc)
    rc, out = run("-d", msa)
    if rc != 0 or "8 removed" not in out or "MSA floppy" not in out:
        fail("MSA clean: rc %d: %s" % (rc, out))
    dec2, _ = ac.msa_decode(open(msa, "rb").read())
    if bytes(dec2) != bytes(img):
        fail("MSA cleaned image differs from the cleaned .st")
    print("msa ok: %d bytes encoded, %d bytes raw" % (len(enc), len(v.img)))
    # the raw-track path: a track of noise is stored raw (length == 4608)
    noisy = bytearray(v.img)
    noisy[0:4608] = bytes((i * 7919) & 0xFF for i in range(4608))
    e2 = ac.msa_encode(noisy, hdr)
    if struct.unpack(">H", e2[10:12])[0] != 4608:
        fail("MSA: an incompressible track was not stored raw")
    if bytes(ac.msa_decode(e2)[0]) != bytes(noisy):
        fail("MSA raw-track round trip differs")

    # ---- AHDI hard disk: GEM at 2, BGM (1024-byte sectors) and an XGM chain -
    # >= 4085 clusters each, so dosfstools reads them as FAT16 too (it goes
    # by cluster count; TOS goes by "it is a hard disk") - except p4, a
    # small TOS-style FAT16 partition that only this tool's rule gets right
    p1 = litter(mkfat.Volume(bps=512, spc=1, totsec=8192, rootents=256, bits=16))       # 4 MB GEM
    p2 = litter(mkfat.Volume(bps=1024, spc=1, totsec=6000, rootents=512, bits=16))      # ~6 MB BGM
    p3 = litter(mkfat.Volume(bps=512, spc=1, totsec=8192, rootents=128, bits=16))       # in the XGM chain
    p4 = litter(mkfat.Volume(bps=512, spc=2, totsec=2048, rootents=128, bits=16))       # small, TOS-style
    s1 = 2
    s2 = s1 + 8192
    x = s2 + 6000 * 2                        # XGM root sector
    s3 = x + 1                               # first extended partition, right after its root
    x2 = x + 1 + 8192                        # second extended root
    s4 = x2 + 1
    total = s4 + 2048
    disk = bytearray(total * 512)

    def entry(sec, off, flag, pid, start, size):
        struct.pack_into(">B3sII", disk, sec * 512 + off, flag, pid, start, size)
    entry(0, 0x1C6, 1, b"GEM", s1, 8192)
    entry(0, 0x1C6 + 12, 1, b"BGM", s2, 6000 * 2)
    entry(0, 0x1C6 + 24, 1, b"XGM", x, total - x)
    entry(x, 0x1C6, 1, b"GEM", 1, 8192)                     # relative to x
    entry(x, 0x1C6 + 12, 1, b"XGM", x2 - x, 2048 + 1)        # relative to the first XGM
    entry(x2, 0x1C6, 1, b"GEM", 1, 2048)
    for vol, sec in ((p1, s1), (p2, s2), (p3, s3), (p4, s4)):
        disk[sec * 512:sec * 512 + len(vol.img)] = vol.img
    hd = os.path.join(OUT, "test.img")
    open(hd, "wb").write(disk)
    parts = ac.ahdi_partitions(disk)
    if [p[0] // 512 for p in parts] != [s1, s2, s3, s4]:
        fail("AHDI parse: %s" % parts)
    befores = [tree(ac.Fat(bytearray(disk), st_ * 512, sz * 512, is_partition=True))
               for st_, sz in ((s1, 8192), (s2, 12000), (s3, 8192), (s4, 2048))]
    rc, out = run("-v", hd)
    if rc != 0 or "32 items" not in out or "4 partitions" not in out:
        fail("hard disk dry run: rc %d: %s" % (rc, out))
    rc, out = run("-d", hd)
    if rc != 0 or "32 removed" not in out:
        fail("hard disk delete: rc %d: %s" % (rc, out))
    disk2 = bytearray(open(hd, "rb").read())
    for i, (st_, sz) in enumerate(((s1, 8192), (s2, 12000), (s3, 8192), (s4, 2048))):
        f = ac.Fat(disk2, st_ * 512, sz * 512, is_partition=True)
        check_volume(befores[i], tree(f), "partition %d" % i)
        fat_consistent(f, "partition %d" % i)
        # the partition alone, for fsck.vfat -n elsewhere
        open(os.path.join(OUT, "part%d.fat" % i), "wb").write(disk2[st_ * 512:(st_ + sz) * 512])
        open(os.path.join(OUT, "part%d-before.fat" % i), "wb").write(disk[st_ * 512:(st_ + sz) * 512])
    # the partition table itself is untouched
    if disk2[:512] != disk[:512] or disk2[x * 512:(x + 1) * 512] != disk[x * 512:(x + 1) * 512]:
        fail("hard disk: a root sector changed")
    print("hard disk ok: 4 partitions (GEM, BGM 1024-byte sectors, XGM chain of two)")

    # ---- FAT12 vs FAT16 on a small partition: TOS says 16 ------------------
    small = litter(mkfat.Volume(bps=512, spc=2, totsec=2048, rootents=64, bits=16))
    f = ac.Fat(bytearray(small.img), 0, len(small.img), is_partition=True)
    if f.bits != 16:
        fail("a small partition should be read as FAT16 (TOS rule), got %d" % f.bits)
    f = ac.Fat(bytearray(small.img), 0, len(small.img), is_partition=False)
    if f.bits != 12:
        fail("a small floppy-style volume should be FAT12 by cluster count, got %d" % f.bits)

    # ---- a directory tree --------------------------------------------------
    d = os.path.join(OUT, "share")
    os.makedirs(os.path.join(d, "GAMES"))
    os.makedirs(os.path.join(d, ".Trashes", "501"))
    os.makedirs(os.path.join(d, ".fseventsd"))
    os.makedirs(os.path.join(d, ".git", "objects"))
    for p, data in (("README.TXT", b"x"), ("._README.TXT", b"y" * 4096), (".DS_Store", b"z"),
                    ("GAMES/ELITE.PRG", b"e"), ("GAMES/._ELITE.PRG", b"y"), (".smbdeleteAAA1", b""),
                    (".Trashes/501/OLD", b"o"), (".fseventsd/x", b"f"), (".git/objects/ab", b"g"),
                    (".git/._config", b"y"), (".hidden_ok", b"h"), ("_notjunk", b"n")):
        open(os.path.join(d, p), "wb").write(data)
    # an old .smbdelete is litter, a fresh one is a Mac still at work
    old_t = os.path.getmtime(os.path.join(d, "README.TXT")) - 3600
    os.utime(os.path.join(d, ".smbdeleteAAA1"), (old_t, old_t))
    open(os.path.join(d, ".smbdeleteAAA2"), "wb").write(b"")
    rc, out = run(d)
    if rc != 0 or "7 items" not in out:
        fail("dir dry run: %s" % out)
    rc, out = run("-d", d)
    if rc != 0 or "7 removed" not in out:
        fail("dir delete: %s" % out)
    left = sorted(os.path.relpath(os.path.join(dp, f), d) for dp, dn, fn in os.walk(d) for f in fn)
    if left != sorted([".git/objects/ab", ".hidden_ok", ".smbdeleteAAA2", "GAMES/ELITE.PRG", "README.TXT", "_notjunk"]):
        fail("dir: wrong survivors %s" % left)
    if os.path.exists(os.path.join(d, ".Trashes")) or os.path.exists(os.path.join(d, ".fseventsd")):
        fail("dir: litter directory survived")
    print("directory ok")

    # ---- odd input: not an image -------------------------------------------
    junkfile = os.path.join(OUT, "not-an-image.bin")
    open(junkfile, "wb").write(b"hello" * 200)
    rc, out = run(junkfile)
    if rc != 2:
        fail("a non-image should give rc 2, got %d" % rc)

    print("%d CHECK(S) FAILED" % fails if fails else "all checks passed")
    if "--keep" in sys.argv:
        print("images kept in %s (fsck.vfat -n them if you like)" % OUT)
    else:
        shutil.rmtree(OUT, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
