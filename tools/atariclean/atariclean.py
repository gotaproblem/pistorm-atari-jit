#!/usr/bin/env python3
"""
atariclean - take the macOS and SMB litter out of Atari drives and images.

    atariclean [-d] [-b] [-v] PATH...

PATH is a directory (a HOSTFS folder such as atari-share, a git tree on
the share, the whole SD card) or an image: a raw floppy (.st), a Magic
Shadow Archiver floppy (.msa, told by its magic), a hard-disk image with
an AHDI/ICD partition table (the emulator's .img), or a bare FAT volume.
Everything is a dry run until -d/--delete is given.

What counts as litter (case-insensitive, at any depth):
    ._*             AppleDouble sidecars (Finder metadata, resource forks)
    .DS_Store       Finder folder settings
    .smbdelete*     macOS SMB "delete when closed" leftovers (in a directory,
                    only when older than --min-age, default 10 minutes:
                    a young one is a Mac still deleting an open file)
    .fseventsd/  .Spotlight-V100/  .Trashes/  .TemporaryItems/
    .AppleDouble/  .AppleDB/  .AppleDesktop/  .apdisk  __MACOSX/
Nothing else is touched: no other dotfile, nothing the Atari made.

Images are edited in place: matching directory entries (with their long
name entries) are marked deleted and their cluster chains freed in every
FAT copy, like a DEL on the Atari would. -b/--backup writes PATH.bak
first. MSA archives are decoded, cleaned, and re-encoded (RLE) in place.
Hard-disk partitions are treated as 16-bit FAT, as TOS does, whatever
their size; floppies by cluster count. --fat12 / --fat16 override.
Nothing in an image is written unless every chain to be freed walks
cleanly first; run fsck.vfat -n on a partition afterwards if you like
(the extracted-partition offset is printed with -v).

Exit status: 0 nothing found or all removed, 1 something could not be
removed, 2 bad arguments or an image that could not be read.
"""
import argparse
import fnmatch
import os
import shutil
import stat
import struct
import sys
import time

JUNK_FILES = ["._*", ".DS_Store", ".smbdelete*", ".apdisk"]
JUNK_DIRS = [".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems",
             ".AppleDouble", ".AppleDB", ".AppleDesktop", "__MACOSX"]


def is_junk(name, is_dir):
    n = name.lower()
    pats = JUNK_DIRS if is_dir else JUNK_FILES
    for p in pats:
        if fnmatch.fnmatchcase(n, p.lower()):
            return True
    # a directory named like a file pattern (a stray ".DS_Store" folder)
    # or an AppleDouble that is a directory: still litter
    if is_dir and (n.startswith("._") or n == ".ds_store"):
        return True
    return False


class Report:
    def __init__(self, verbose):
        self.found = 0
        self.removed = 0
        self.failed = 0
        self.bytes = 0
        self.verbose = verbose

    def hit(self, what, size, done, err=None):
        self.found += 1
        self.bytes += size
        if err:
            self.failed += 1
            print("  FAILED  %s  (%s)" % (what, err))
        else:
            if done:
                self.removed += 1
            print("  %s  %s%s" % ("removed" if done else "would remove", what,
                                  ("  [%d bytes]" % size) if self.verbose else ""))


# ----------------------------------------------------------------- dirs --

def clean_dir(root, delete, rep, min_age=600):
    """walk root; litter directories are removed whole, never descended"""
    for dirpath, dirnames, filenames in os.walk(root, topdown=True):
        keep = []
        for d in sorted(dirnames):
            p = os.path.join(dirpath, d)
            if is_junk(d, True):
                size = 0
                for dp, dn, fn in os.walk(p):
                    for f in fn:
                        try:
                            size += os.lstat(os.path.join(dp, f)).st_size
                        except OSError:
                            pass
                err = None
                if delete:
                    try:
                        shutil.rmtree(p)
                    except OSError as e:
                        err = e.strerror
                rep.hit(p, size, delete, err)
            else:
                keep.append(d)
        dirnames[:] = keep
        for f in sorted(filenames):
            if is_junk(f, False):
                p = os.path.join(dirpath, f)
                try:
                    st_ = os.lstat(p)
                    size = st_.st_size
                except OSError:
                    st_ = None
                    size = 0
                # a .smbdelete file is a Mac mid-way through deleting an
                # open file; only the ones it forgot are litter
                if f.lower().startswith(".smbdelete") and st_ and \
                   time.time() - st_.st_mtime < min_age:
                    continue
                err = None
                if delete:
                    try:
                        os.unlink(p)
                    except OSError as e:
                        err = e.strerror
                rep.hit(p, size, delete, err)


# ------------------------------------------------------------------ FAT --

class FatError(Exception):
    pass


class Fat:
    """
    One FAT12/FAT16 volume inside a bytearray (the whole image), starting at
    byte offset `base`. Atari BPBs are little-endian like a PC's; TOS gives
    hard-disk partitions logical sectors of 512..8192 bytes and a 16-bit
    FAT regardless of size, floppies 512 and 12-bit.
    """

    def __init__(self, img, base, size, force=None, is_partition=False):
        self.img = img
        self.base = base
        self.size = size
        b = img[base:base + 512]
        if len(b) < 512:
            raise FatError("volume too small for a boot sector")
        self.bps = struct.unpack_from("<H", b, 11)[0]
        self.spc = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.rootents = struct.unpack_from("<H", b, 17)[0]
        totsec16 = struct.unpack_from("<H", b, 19)[0]
        self.fatsz = struct.unpack_from("<H", b, 22)[0]
        totsec32 = struct.unpack_from("<I", b, 32)[0]
        self.totsec = totsec16 if totsec16 else totsec32
        if self.bps not in (512, 1024, 2048, 4096, 8192) or self.spc == 0 or \
           self.nfats not in (1, 2) or self.fatsz == 0 or self.totsec == 0 or \
           self.rootents == 0 or self.reserved == 0:
            raise FatError("no FAT boot sector (bps %d spc %d fats %d fatsz %d sectors %d root %d)"
                           % (self.bps, self.spc, self.nfats, self.fatsz, self.totsec, self.rootents))
        self.rootsecs = (self.rootents * 32 + self.bps - 1) // self.bps
        self.fat_start = self.reserved                     # in sectors
        self.root_start = self.fat_start + self.nfats * self.fatsz
        self.data_start = self.root_start + self.rootsecs
        self.nclusters = (self.totsec - self.data_start) // self.spc
        if self.nclusters <= 0:
            raise FatError("no data clusters")
        if force:
            self.bits = force
        elif is_partition:
            self.bits = 16                                  # TOS: always on a hard disk
        else:
            self.bits = 12 if self.nclusters < 4085 else 16
        if self.totsec * self.bps > size + 0:
            # TOS sometimes rounds; only refuse when the FAT itself is outside
            if (self.data_start * self.bps) > size:
                raise FatError("BPB claims %d sectors of %d bytes, volume is %d bytes"
                               % (self.totsec, self.bps, size))
        self.eoc = 0xFF8 if self.bits == 12 else 0xFFF8
        self.dirty = set()                                  # nothing: we write straight through

    # -- FAT entries -------------------------------------------------------
    def _fat_off(self, copy, n):
        return self.base + (self.fat_start + copy * self.fatsz) * self.bps + \
               (n + n // 2 if self.bits == 12 else n * 2)

    def get(self, n):
        o = self._fat_off(0, n)
        if self.bits == 12:
            v = struct.unpack_from("<H", self.img, o)[0]
            return (v >> 4) if (n & 1) else (v & 0xFFF)
        return struct.unpack_from("<H", self.img, o)[0]

    def put(self, n, val):
        for c in range(self.nfats):
            o = self._fat_off(c, n)
            if self.bits == 12:
                v = struct.unpack_from("<H", self.img, o)[0]
                if n & 1:
                    v = (v & 0x000F) | ((val & 0xFFF) << 4)
                else:
                    v = (v & 0xF000) | (val & 0xFFF)
                struct.pack_into("<H", self.img, o, v)
            else:
                struct.pack_into("<H", self.img, o, val & 0xFFFF)

    def chain(self, first):
        """the clusters of a chain; FatError on a loop or a bad link"""
        out = []
        seen = set()
        c = first
        while 2 <= c < self.eoc:
            if c in seen or c >= self.nclusters + 2:
                raise FatError("bad cluster chain at %d" % c)
            seen.add(c)
            out.append(c)
            c = self.get(c)
            if len(out) > self.nclusters:
                raise FatError("cluster chain never ends")
        return out

    def cluster_off(self, c):
        return self.base + (self.data_start + (c - 2) * self.spc) * self.bps

    # -- directories -------------------------------------------------------
    def root_entries(self):
        """(offset, 32-byte entry) for every slot of the root directory"""
        off = self.base + self.root_start * self.bps
        for i in range(self.rootents):
            o = off + i * 32
            yield o, self.img[o:o + 32]

    def dir_entries(self, first):
        for c in self.chain(first):
            off = self.cluster_off(c)
            n = self.spc * self.bps // 32
            for i in range(n):
                o = off + i * 32
                yield o, self.img[o:o + 32]

    @staticmethod
    def short_name(e):
        base = bytes(e[0:8]).rstrip(b" ")
        ext = bytes(e[8:11]).rstrip(b" ")
        if base[:1] == b"\x05":
            base = b"\xe5" + base[1:]
        s = base.decode("cp437", "replace")
        if ext:
            s += "." + ext.decode("cp437", "replace")
        return s

    def listing(self, entries):
        """
        walk one directory's slots: yields (name, is_dir, first_cluster,
        size, [offsets of the entry and its LFN entries]) for each live
        entry, long name when there is one
        """
        lfn = {}
        lfn_offs = []
        for o, e in entries:
            first = e[0]
            if first == 0x00:
                break
            if first == 0xE5:
                lfn, lfn_offs = {}, []
                continue
            attr = e[11]
            if attr == 0x0F:
                seq = first & 0x1F
                chunk = bytes(e[1:11]) + bytes(e[14:26]) + bytes(e[28:32])
                lfn[seq] = chunk
                lfn_offs.append(o)
                continue
            if attr & 0x08:                                  # volume label
                lfn, lfn_offs = {}, []
                continue
            name = None
            if lfn:
                raw = b"".join(lfn[k] for k in sorted(lfn))
                s = raw.decode("utf-16-le", "replace")
                cut = s.find("\x00")
                if cut >= 0:
                    s = s[:cut]
                name = s.rstrip("￿")
            if not name:
                name = self.short_name(e)
            is_dir = bool(attr & 0x10)
            first_cl = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            yield name, is_dir, first_cl, size, lfn_offs + [o]
            lfn, lfn_offs = {}, []

    # -- deletion ----------------------------------------------------------
    def dir_bytes(self, first):
        """bytes of the files under a directory cluster, for the report"""
        total = 0
        for name, is_dir, fc, size, offs in self.listing(self.dir_entries(first)):
            if name in (".", ".."):
                continue
            total += self.dir_bytes(fc) if is_dir else size
        return total

    def remove(self, is_dir, first_cl, offs):
        """free the chain (recursing into a directory) and mark the slots"""
        if is_dir and first_cl >= 2:
            for name, sub_dir, fc, size, sub_offs in list(self.listing(self.dir_entries(first_cl))):
                if name in (".", ".."):
                    continue
                self.remove(sub_dir, fc, sub_offs)
        if first_cl >= 2:
            for c in self.chain(first_cl):
                self.put(c, 0)
        for o in offs:
            self.img[o] = 0xE5

    def check_removable(self, is_dir, first_cl):
        """walk everything a removal would touch; FatError if anything is off"""
        if first_cl >= 2:
            self.chain(first_cl)
        if is_dir and first_cl >= 2:
            for name, sub_dir, fc, size, offs in list(self.listing(self.dir_entries(first_cl))):
                if name in (".", ".."):
                    continue
                self.check_removable(sub_dir, fc)


def clean_volume(fat, label, delete, rep, path=""):
    """walk a volume, litter first; returns True if something was modified"""
    modified = False
    stack = [("", None)]                           # (path, first cluster or None=root)
    while stack:
        dpath, first = stack.pop()
        entries = fat.root_entries() if first is None else fat.dir_entries(first)
        for name, is_dir, fc, size, offs in list(fat.listing(entries)):
            if name in (".", ".."):
                continue
            full = "%s:%s/%s" % (label, dpath, name)
            if is_junk(name, is_dir):
                total = size
                err = None
                try:
                    fat.check_removable(is_dir, fc)
                    if is_dir:
                        total = fat.dir_bytes(fc)
                    if delete:
                        fat.remove(is_dir, fc, offs)
                        modified = True
                except FatError as e:
                    err = str(e)
                rep.hit(full, total, delete, err)
            elif is_dir and fc >= 2:
                stack.append((dpath + "/" + name, fc))
    return modified


# ----------------------------------------------------------------- AHDI --

def ahdi_partitions(img):
    """[(start_byte, size_byte, id)] from an Atari root sector, [] if none"""
    if len(img) < 512:
        return []
    total = len(img) // 512

    def entry(sec, off):
        b = img[sec * 512 + off: sec * 512 + off + 12]
        if len(b) < 12:
            return None
        flag = b[0]
        pid = bytes(b[1:4])
        start, size = struct.unpack(">II", b[4:12])
        return flag, pid, start, size

    def valid(e, lo=1):
        if not e:
            return False
        flag, pid, start, size = e
        return (flag & 1) and pid.isalnum() and pid.isupper() and \
            start >= lo and size > 0 and start + size <= total

    parts = []
    # ICD's extension: up to eight more entries before the standard four
    slots = [0x156 + 12 * i for i in range(8)] + [0x1C6 + 12 * i for i in range(4)]
    for off in slots:
        e = entry(0, off)
        if not valid(e):
            continue
        flag, pid, start, size = e
        if pid == b"XGM":
            # extended: a chain of root sectors, each with one partition
            # (relative to itself) and maybe another XGM (relative to the
            # first XGM sector)
            root = start
            cur = start
            hops = 0
            while hops < 64:
                hops += 1
                p = entry(cur, 0x1C6)
                if valid(p, 0):
                    parts.append(((cur + p[2]) * 512, p[3] * 512, p[1].decode()))
                nxt = entry(cur, 0x1C6 + 12)
                if valid(nxt, 0) and nxt[1] == b"XGM":
                    cur = root + nxt[2]
                else:
                    break
        else:
            parts.append((start * 512, size * 512, pid.decode()))
    # only believe it when the standard slots produced something sane
    return parts


# ------------------------------------------------------------------ MSA --

def msa_decode(raw):
    if len(raw) < 10 or raw[0] != 0x0E or raw[1] != 0x0F:
        return None
    spt, sides0, tstart, tend = struct.unpack(">HHHH", raw[2:10])
    sides = sides0 + 1
    tlen = 512 * spt
    ntracks = (tend - tstart + 1) * sides
    if not spt or spt > 12 or sides > 2 or tend < tstart:
        raise FatError("MSA header out of range")
    img = bytearray(tlen * ntracks)
    src = 10
    dst = 0
    for t in range(ntracks):
        dlen = struct.unpack(">H", raw[src:src + 2])[0]
        src += 2
        if src + dlen > len(raw):
            raise FatError("MSA truncated")
        if dlen == tlen:
            img[dst:dst + tlen] = raw[src:src + tlen]
        else:
            i, e, o = src, src + dlen, dst
            while i < e and o < dst + tlen:
                b = raw[i]
                i += 1
                if b != 0xE5:
                    img[o] = b
                    o += 1
                    continue
                v = raw[i]
                n = struct.unpack(">H", raw[i + 1:i + 3])[0]
                i += 3
                n = min(n, dst + tlen - o)
                img[o:o + n] = bytes([v]) * n
                o += n
        src += dlen
        dst += tlen
    return img, (spt, sides0, tstart, tend)


def msa_rle(track):
    out = bytearray()
    i = 0
    n = len(track)
    while i < n:
        b = track[i]
        j = i + 1
        while j < n and track[j] == b and j - i < 0xFFFF:
            j += 1
        run = j - i
        if b == 0xE5 or run >= 4:
            out += bytes([0xE5, b]) + struct.pack(">H", run)
        else:
            out += bytes([b]) * run
        i = j
    return out


def msa_encode(img, hdr):
    spt, sides0, tstart, tend = hdr
    tlen = 512 * spt
    out = bytearray(b"\x0e\x0f" + struct.pack(">HHHH", spt, sides0, tstart, tend))
    for t in range(len(img) // tlen):
        track = bytes(img[t * tlen:(t + 1) * tlen])
        comp = msa_rle(track)
        if len(comp) < tlen:
            out += struct.pack(">H", len(comp)) + comp
        else:
            out += struct.pack(">H", tlen) + track
    return bytes(out)


# ---------------------------------------------------------------- images --

def clean_image(path, delete, backup, force, verbose, rep):
    with open(path, "rb") as f:
        raw = f.read()
    hdr = None
    try:
        dec = msa_decode(raw)
    except FatError as e:
        print("%s: %s" % (path, e))
        return 2
    if dec:
        img, hdr = dec
        kind = "MSA floppy"
    else:
        img = bytearray(raw)
        kind = None

    parts = [] if hdr else ahdi_partitions(img)
    if parts:
        kind = "hard disk, %d partition%s" % (len(parts), "" if len(parts) == 1 else "s")
    elif kind is None:
        kind = "raw volume"
    print("%s: %s" % (path, kind))

    modified = False
    if parts:
        for i, (start, size, pid) in enumerate(parts):
            label = "%s[%d]" % (os.path.basename(path), i)
            try:
                fat = Fat(img, start, size, force, is_partition=True)
            except FatError as e:
                print("  %s (%s at %d): %s - skipped" % (label, pid, start, e))
                continue
            if verbose:
                print("  %s: %s at byte %d, %d bytes, %d-byte sectors, FAT%d, %d clusters"
                      % (label, pid, start, size, fat.bps, fat.bits, fat.nclusters))
            modified |= clean_volume(fat, label, delete, rep)
    else:
        try:
            fat = Fat(img, 0, len(img), force, is_partition=False)
        except FatError as e:
            print("  %s - not a FAT image, skipped" % e)
            return 2
        if verbose:
            print("  FAT%d, %d-byte sectors, %d clusters" % (fat.bits, fat.bps, fat.nclusters))
        modified |= clean_volume(fat, os.path.basename(path), delete, rep)

    if delete and modified:
        if backup:
            shutil.copy2(path, path + ".bak")
        out = msa_encode(img, hdr) if hdr else bytes(img)
        tmp = path + ".atariclean.tmp"
        with open(tmp, "wb") as f:
            f.write(out)
        try:
            st = os.stat(path)
            os.chmod(tmp, stat.S_IMODE(st.st_mode))
        except OSError:
            pass
        os.replace(tmp, path)
    return 0


def main():
    ap = argparse.ArgumentParser(description="remove macOS / SMB litter from Atari folders and disk images")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("-d", "--delete", action="store_true", help="actually remove (default: report only)")
    ap.add_argument("-b", "--backup", action="store_true", help="images: write PATH.bak before changing PATH")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--fat12", action="store_const", const=12, dest="force")
    ap.add_argument("--fat16", action="store_const", const=16, dest="force")
    ap.add_argument("--min-age", type=int, default=600, metavar="SECONDS",
                    help="directories: leave .smbdelete* files younger than this (default 600)")
    a = ap.parse_args()

    rep = Report(a.verbose)
    rc = 0
    for p in a.paths:
        if os.path.isdir(p):
            print("%s: directory" % p)
            clean_dir(p, a.delete, rep, a.min_age)
        elif os.path.isfile(p):
            r = clean_image(p, a.delete, a.backup, a.force, a.verbose, rep)
            rc = max(rc, r)
        else:
            print("%s: no such file or directory" % p)
            rc = 2
    print("%d item%s of litter, %d bytes%s" % (
        rep.found, "" if rep.found == 1 else "s", rep.bytes,
        (", %d removed" % rep.removed) if a.delete else " (dry run: add -d to remove)"))
    if rep.failed:
        print("%d could not be removed" % rep.failed)
        rc = max(rc, 1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
