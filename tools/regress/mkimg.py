#!/usr/bin/env python3
"""Disk images for the regression suite (no dependencies, any host).

  mkimg.py st  OUT.st  [--auto PRG ...] [--file NAME=SRC ...] [--pattern NAME=BYTES ...]
      720K double-sided FAT12 floppy (80 tracks, 9 sectors, 2 sides).
      --auto      copies a PRG into A:\\AUTO\\ (TOS runs them at boot, in
                  the order given - the order they sit in the directory)
      --file      copies a host file into the root
      --pattern   writes a root file of BYTES bytes whose byte at offset i
                  is pattern_byte(i) below - the guest checks it with the
                  same formula, so a wrong sector, a short read or a
                  swapped byte all show

  mkimg.py hdboot OUT.img [same options as st]
      The same FAT12 file system as one FAT12 partition (type $01) behind
      an MBR, for ACSI ID 0 (C:). EmuTOS boots from C: on a cold start
      whenever a hard disk exists (bios.c: DEFAULT_BOOTDEV) and only runs
      the boot drive's AUTO folder, EMUDESK.INF and accessories, so run.sh
      puts the same files on C: and in A:. The MBR is not executable.

  mkimg.py hd  OUT.img SECTORS
      Raw hard-disk image for the IDE and ACSI tests. Sector 0 is an MBR
      (55AA + one DOS partition entry) so the emulator's byte-order
      detection says "stored native, readable as is"; it is NOT a boot
      sector (word sum != $1234, TOS will not run it). Every other sector
      n holds n as a big-endian long in bytes 0-3, then (n + i) & $FF at
      byte offset i (4..511).
"""
import struct, sys

def pattern_byte(i):
    return (i ^ (i >> 8) ^ (i >> 16)) & 0xFF

def pattern(n):
    return bytes(pattern_byte(i) for i in range(n))

# ---------------------------------------------------------------- FAT12 floppy
SPT, HEADS, TRACKS, BPS = 9, 2, 80, 512
NSECT = SPT * HEADS * TRACKS            # 1440
SPC, RES, NFATS, SPF, NDIRS = 2, 1, 2, 3, 112
DIR0 = RES + NFATS * SPF                # 7
DATA0 = DIR0 + NDIRS * 32 // BPS        # 14
NCLUST = (NSECT - DATA0) // SPC         # 713

class Fat12:
    def __init__(self):
        self.img = bytearray(NSECT * BPS)
        self.fat = [0] * (NCLUST + 2)
        self.fat[0], self.fat[1] = 0xFF9, 0xFFF
        self.next = 2
        b = self.img
        b[0:2] = b'\xE9\x00'            # not a BRA: never executable
        b[2:8] = b'PSREGR'
        b[8:11] = b'\x12\x34\x56'       # serial
        struct.pack_into('<HBHBHHBHHHH', b, 11, BPS, SPC, RES, NFATS, NDIRS,
                         NSECT, 0xF9, SPF, SPT, HEADS, 0)
        s = sum(struct.unpack('>256H', bytes(b[0:512]))) & 0xFFFF
        if s == 0x1234:                 # never a valid boot checksum
            b[510] ^= 1

    def alloc(self, data):
        if not data:
            return 0
        n = (len(data) + SPC * BPS - 1) // (SPC * BPS)
        first = self.next
        for k in range(n):
            c = first + k
            self.fat[c] = c + 1 if k < n - 1 else 0xFFF
            off = (DATA0 + (c - 2) * SPC) * BPS
            chunk = data[k * SPC * BPS:(k + 1) * SPC * BPS]
            self.img[off:off + len(chunk)] = chunk
        self.next += n
        if self.next > NCLUST + 2:
            sys.exit('floppy full')
        return first

    @staticmethod
    def entry(name, attr, clus, size):
        base, _, ext = name.upper().partition('.')
        e = bytearray(32)
        e[0:8] = base.ljust(8).encode()[:8]
        e[8:11] = ext.ljust(3).encode()[:3]
        e[11] = attr
        struct.pack_into('<HH', e, 22, 0, (2026 - 1980) << 9 | 11 << 5 | 1)
        struct.pack_into('<HI', e, 26, clus, size)
        return bytes(e)

    def finish(self, root, auto):
        if auto is not None:            # AUTO directory: one cluster
            ac = self.alloc(b'\0' * SPC * BPS)
            ents = [self.entry('.', 0x10, ac, 0), self.entry('..', 0x10, 0, 0)]
            ents[0] = b'.' + b' ' * 10 + ents[0][11:]
            ents[1] = b'..' + b' ' * 9 + ents[1][11:]
            for name, data in auto:
                ents.append(self.entry(name, 0, self.alloc(data), len(data)))
            if len(ents) * 32 > SPC * BPS:
                sys.exit('too many AUTO programs')
            off = (DATA0 + (ac - 2) * SPC) * BPS
            blob = b''.join(ents)
            self.img[off:off + len(blob)] = blob
            root = [('AUTO', 0x10, ac, 0)] + root
        rd = b''.join(self.entry(n, a, c, s) for n, a, c, s in root)
        if len(rd) > NDIRS * 32:
            sys.exit('root directory full')
        self.img[DIR0 * BPS:DIR0 * BPS + len(rd)] = rd
        fat = bytearray(SPF * BPS)
        for c in range(0, len(self.fat), 2):
            a = self.fat[c]
            b = self.fat[c + 1] if c + 1 < len(self.fat) else 0
            i = c * 3 // 2
            fat[i:i + 3] = bytes([a & 0xFF, (a >> 8) | ((b & 0xF) << 4), b >> 4])
        for f in range(NFATS):
            o = (RES + f * SPF) * BPS
            self.img[o:o + len(fat)] = fat
        return bytes(self.img)

def make_st(out, args, hd=False):
    auto, root, fs = [], [], Fat12()
    i = 0
    while i < len(args):
        opt, val = args[i], args[i + 1]
        i += 2
        if opt == '--auto':
            name = val.replace('\\', '/').split('/')[-1].upper()
            auto.append((name, open(val, 'rb').read()))
        elif opt == '--file':
            name, src = val.split('=', 1)
            data = open(src, 'rb').read()
            root.append((name.upper(), 0, fs.alloc(data), len(data)))
        elif opt == '--pattern':
            name, n = val.split('=', 1)
            data = pattern(int(n))
            root.append((name.upper(), 0, fs.alloc(data), len(data)))
        else:
            sys.exit('unknown option ' + opt)
    fsimg = fs.finish(root, auto if auto else None)
    if hd:
        mbr = bytearray(512)
        struct.pack_into('<BBBBBBBBII', mbr, 0x1BE, 0x00, 0, 2, 0, 0x01, 0, 0, 0,
                         1, NSECT)
        mbr[510:512] = b'\x55\xAA'
        if sum(struct.unpack('>256H', bytes(mbr))) & 0xFFFF == 0x1234:
            mbr[2] = 1
        fsimg = bytes(mbr) + fsimg
    open(out, 'wb').write(fsimg)

# ---------------------------------------------------------------- hard disk
def hd_sector(n):
    return struct.pack('>I', n) + bytes(((n + i) & 0xFF) for i in range(4, 512))

def make_hd(out, nsect):
    with open(out, 'wb') as f:
        mbr = bytearray(512)
        mbr[0:2] = b'\x00\x00'
        struct.pack_into('<BBBBBBBBII', mbr, 0x1BE, 0x00, 0, 2, 0, 0x06, 0, 0, 0,
                         1, nsect - 1)
        mbr[510:512] = b'\x55\xAA'
        if sum(struct.unpack('>256H', bytes(mbr))) & 0xFFFF == 0x1234:
            mbr[2] = 1
        f.write(mbr)
        for n in range(1, nsect):
            f.write(hd_sector(n))

if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    kind, out = sys.argv[1], sys.argv[2]
    if kind == 'st':
        make_st(out, sys.argv[3:])
    elif kind == 'hdboot':
        make_st(out, sys.argv[3:], hd=True)
    elif kind == 'hd':
        make_hd(out, int(sys.argv[3]))
    else:
        sys.exit(__doc__)
