"""
mkfat.py - build FAT12/FAT16 volumes for the atariclean tests, the way
macOS + TOS would leave them: 8.3 entries, VFAT long-name entries for
anything that is not 8.3, '.'/'..' in subdirectories, two FAT copies.
Independent of atariclean.py (nothing imported from it) so the tests
have something to disagree with.
"""
import struct

ATTR_DIR = 0x10
ATTR_LFN = 0x0F


def lfn_checksum(short11):
    s = 0
    for b in short11:
        s = ((s & 1) << 7) + (s >> 1) + b
        s &= 0xFF
    return s


def is_83(name):
    if name in (".", ".."):
        return True
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    ok = lambda s: s and all(c.isalnum() or c in "_-~!#$%&'()@^`{}" for c in s) and s == s.upper()
    return ok(base) and len(base) <= 8 and (ext == "" or (ok(ext) and len(ext) <= 3)) and "." not in base


class Volume:
    def __init__(self, bps=512, spc=2, totsec=1440, rootents=112, fatsz=None, bits=None, nfats=2, reserved=1):
        self.bps, self.spc, self.totsec, self.rootents, self.nfats, self.reserved = bps, spc, totsec, rootents, nfats, reserved
        rootsecs = (rootents * 32 + bps - 1) // bps
        # clusters, then the FAT size that carries them (iterate once)
        nclus = (totsec - reserved - rootsecs) // spc
        self.bits = bits or (12 if nclus < 4085 else 16)
        if fatsz is None:
            need = (nclus + 2) * (3 if self.bits == 12 else 2)
            if self.bits == 12:
                need = ((nclus + 2) * 3 + 1) // 2
            fatsz = (need + bps - 1) // bps
        self.fatsz = fatsz
        self.rootsecs = rootsecs
        self.data_start = reserved + nfats * fatsz + rootsecs
        self.nclus = (totsec - self.data_start) // spc
        self.img = bytearray(totsec * bps)
        self.eoc = 0xFFF if self.bits == 12 else 0xFFFF
        self.next_free = 2
        self.serial = 0
        b = self.img
        b[0:3] = b"\xe9\x00\x00"
        b[3:11] = b"ATARICLN"
        struct.pack_into("<HBHBHHBHHHII", b, 11, bps, spc, reserved, nfats, rootents,
                         totsec if totsec < 65536 else 0, 0xF9 if totsec < 4096 else 0xF8,
                         fatsz, 9, 2, 0, totsec if totsec >= 65536 else 0)
        for c in range(nfats):
            o = (reserved + c * fatsz) * bps
            self.put(0, 0xFF8 if self.bits == 12 else 0xFFF8)
            self.put(1, self.eoc)
        self.dirs = {"": ("root", None)}          # path -> (kind, first cluster)

    # FAT ---------------------------------------------------------------
    def _off(self, copy, n):
        return (self.reserved + copy * self.fatsz) * self.bps + (n + n // 2 if self.bits == 12 else n * 2)

    def get(self, n):
        v = struct.unpack_from("<H", self.img, self._off(0, n))[0]
        if self.bits == 12:
            return (v >> 4) if (n & 1) else (v & 0xFFF)
        return v

    def put(self, n, val):
        for c in range(self.nfats):
            o = self._off(c, n)
            if self.bits == 12:
                v = struct.unpack_from("<H", self.img, o)[0]
                v = (v & 0x000F) | ((val & 0xFFF) << 4) if (n & 1) else (v & 0xF000) | (val & 0xFFF)
                struct.pack_into("<H", self.img, o, v)
            else:
                struct.pack_into("<H", self.img, o, val)

    def alloc(self, nbytes):
        n = max(1, (nbytes + self.spc * self.bps - 1) // (self.spc * self.bps))
        chain = []
        while len(chain) < n:
            if self.next_free >= self.nclus + 2:
                raise RuntimeError("volume full")
            chain.append(self.next_free)
            self.next_free += 1
        for i, c in enumerate(chain):
            self.put(c, chain[i + 1] if i + 1 < len(chain) else self.eoc)
        return chain

    def coff(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.bps

    def write_chain(self, chain, data):
        cl = self.spc * self.bps
        for i, c in enumerate(chain):
            part = data[i * cl:(i + 1) * cl]
            self.img[self.coff(c):self.coff(c) + len(part)] = part

    # directories --------------------------------------------------------
    def _slots(self, dirpath):
        kind, first = self.dirs[dirpath]
        if kind == "root":
            base = (self.reserved + self.nfats * self.fatsz) * self.bps
            return [base + i * 32 for i in range(self.rootents)]
        out = []
        c = first
        while 2 <= c < 0xFF8 if self.bits == 12 else 2 <= c < 0xFFF8:
            base = self.coff(c)
            out += [base + i * 32 for i in range(self.spc * self.bps // 32)]
            c = self.get(c)
        return out

    def _free_slots(self, dirpath, n):
        slots = self._slots(dirpath)
        run = []
        for o in slots:
            if self.img[o] in (0x00, 0xE5):
                run.append(o)
                if len(run) == n:
                    return run
            else:
                run = []
        raise RuntimeError("directory full: " + dirpath)

    def _short(self, name, dirpath):
        if name in (".", ".."):
            return name.ljust(11).encode()
        if is_83(name):
            base, ext = (name.rsplit(".", 1) + [""])[:2] if "." in name else (name, "")
            return (base.ljust(8) + ext.ljust(3)).encode("cp437")
        # a Windows-style alias: strip the dots, upper-case, ~N
        stem = "".join(ch for ch in name.upper() if ch.isalnum() or ch in "_-")
        ext = ""
        if "." in name.strip("."):
            stem_part, ext = name.strip(".").rsplit(".", 1)
            stem = "".join(ch for ch in stem_part.upper() if ch.isalnum() or ch in "_-")
            ext = "".join(ch for ch in ext.upper() if ch.isalnum())[:3]
        stem = (stem or "_")[:6]
        self.serial += 1
        return ("%s~%d" % (stem, self.serial)).ljust(8)[:8].encode() + ext.ljust(3).encode()

    def add(self, path, data=None, is_dir=False):
        dirpath, _, name = path.rpartition("/")
        if dirpath not in self.dirs:
            self.add(dirpath, is_dir=True)
        short = self._short(name, dirpath)
        entries = []
        if not is_83(name):
            u = name.encode("utf-16-le") + b"\x00\x00"
            u += b"\xff" * ((26 - len(u) % 26) % 26)
            n = len(u) // 26
            cs = lfn_checksum(short)
            for i in range(n, 0, -1):
                chunk = u[(i - 1) * 26:i * 26]
                e = bytearray(32)
                e[0] = i | (0x40 if i == n else 0)
                e[1:11] = chunk[0:10]
                e[11] = ATTR_LFN
                e[13] = cs
                e[14:26] = chunk[10:22]
                e[28:32] = chunk[22:26]
                entries.append(bytes(e))
        e = bytearray(32)
        e[0:11] = short
        if is_dir:
            chain = self.alloc(self.spc * self.bps)
            first = chain[0]
            e[11] = ATTR_DIR
            struct.pack_into("<H", e, 26, first)
            entries.append(bytes(e))
            for o, ent in zip(self._free_slots(dirpath, len(entries)), entries):
                self.img[o:o + 32] = ent
            self.dirs[path] = ("dir", first)
            # . and ..
            pk, pfirst = self.dirs[dirpath]
            dot = bytearray(32); dot[0:11] = b".          "; dot[11] = ATTR_DIR; struct.pack_into("<H", dot, 26, first)
            dd = bytearray(32); dd[0:11] = b"..         "; dd[11] = ATTR_DIR; struct.pack_into("<H", dd, 26, pfirst or 0)
            self.img[self.coff(first):self.coff(first) + 32] = dot
            self.img[self.coff(first) + 32:self.coff(first) + 64] = dd
        else:
            data = data or b""
            if data:
                chain = self.alloc(len(data))
                self.write_chain(chain, data)
                struct.pack_into("<H", e, 26, chain[0])
            struct.pack_into("<I", e, 28, len(data))
            entries.append(bytes(e))
            for o, ent in zip(self._free_slots(dirpath, len(entries)), entries):
                self.img[o:o + 32] = ent
        return self

    def free_clusters(self):
        return sum(1 for c in range(2, self.nclus + 2) if self.get(c) == 0)
