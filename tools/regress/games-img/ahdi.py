"""ahdi.py - read Atari hard disk images (AHDI root sector, BGM/GEM/XGM
partitions) and GEMDOS FAT12/FAT16 file systems, including the
word-swapped IDE images (ide_byteorder native). Read-only."""
import struct


class Img:
    def __init__(s, path, swap):
        s.f = open(path, 'rb')
        s.swap = swap

    def rd(s, lba, n=1):
        s.f.seek(lba * 512)
        b = s.f.read(512 * n)
        if s.swap:
            a = bytearray(b)
            a[0::2], a[1::2] = b[1::2], b[0::2]
            b = bytes(a)
        return b


def parts(img):
    b = img.rd(0)
    out = []
    for i in range(4):
        o = 0x1c6 + i * 12
        fl = b[o]
        pid = b[o + 1:o + 4]
        st, sz = struct.unpack('>II', b[o + 4:o + 12])
        if fl & 1:
            out.append((pid.decode('latin1'), st, sz))
    return out


def allparts(img):
    """every data partition, XGM chains followed, in drive-letter order"""
    out = []
    for pid, st, sz in parts(img):
        if pid != 'XGM':
            out.append((pid, st, sz))
            continue
        xbase = cur = st
        while True:
            b = img.rd(cur)
            nxt = None
            for i in range(4):
                o = 0x1c6 + i * 12
                fl = b[o]
                p2 = b[o + 1:o + 4].decode('latin1')
                s2, z2 = struct.unpack('>II', b[o + 4:o + 12])
                if not fl & 1:
                    continue
                if p2 == 'XGM':
                    nxt = xbase + s2
                else:
                    out.append((p2, cur + s2, z2))
            if nxt is None:
                break
            cur = nxt
    return out


class Fat:
    def __init__(s, img, start):
        s.img = img
        bs = img.rd(start)
        s.bps = struct.unpack('<H', bs[11:13])[0]
        s.spc = bs[13]
        s.res = struct.unpack('<H', bs[14:16])[0]
        s.nf = bs[16]
        s.nroot = struct.unpack('<H', bs[17:19])[0]
        ts = struct.unpack('<H', bs[19:21])[0]
        s.spf = struct.unpack('<H', bs[22:24])[0]
        s.ls = s.bps // 512                    # 512-byte sectors per logical sector
        s.fat_lba = start + s.res * s.ls
        s.root_lba = s.fat_lba + s.nf * s.spf * s.ls
        s.data_lba = s.root_lba + (s.nroot * 32 + s.bps - 1) // s.bps * s.ls
        s.fat = img.rd(s.fat_lba, s.spf * s.ls)
        tot = ts if ts else struct.unpack('<I', bs[32:36])[0]
        s.fat12 = (tot * s.ls - (s.data_lba - start)) // (s.spc * s.ls) < 4085

    def chain(s, c):
        end = 0xFF0 if s.fat12 else 0xFFF0
        while 2 <= c < end:
            yield c
            if s.fat12:
                o = c + c // 2
                w = s.fat[o] | s.fat[o + 1] << 8
                c = (w >> 4) if c & 1 else (w & 0xFFF)
            else:
                c = struct.unpack('<H', s.fat[c * 2:c * 2 + 2])[0]

    def clus(s, c):
        return s.img.rd(s.data_lba + (c - 2) * s.spc * s.ls, s.spc * s.ls)

    def entries(s, data):
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 8:
                continue
            n = e[0:8].decode('latin1').rstrip()
            x = e[8:11].decode('latin1').rstrip()
            name = n + ('.' + x if x else '')
            if name in ('.', '..'):
                continue
            yield name, e[11], struct.unpack('<H', e[26:28])[0], struct.unpack('<I', e[28:32])[0]

    def root(s):
        return s.img.rd(s.root_lba, (s.nroot * 32 + 511) // 512)

    def dirdata(s, c):
        return b''.join(s.clus(x) for x in s.chain(c))

    def read(s, c, size):
        if not size:
            return b''
        return b''.join(s.clus(x) for x in s.chain(c))[:size]

    def walk(s, data=None, path='', depth=0, maxd=8):
        """(path, attr, first cluster, size) for everything below"""
        if data is None:
            data = s.root()
        for name, attr, c, size in s.entries(data):
            p = path + '\\' + name
            yield p, attr, c, size
            if attr & 0x10 and depth < maxd:
                yield from s.walk(s.dirdata(c), p, depth + 1, maxd)
