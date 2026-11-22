#!/usr/bin/env python3
"""mkgamesimg.py - build the regression game disk: an IDE hard disk image
holding the regression games (games.txt), bootable by TOS 2.06.

    python3 tools/regress/games-img/mkgamesimg.py [~/dkimages/regress-games.img]

Layout, as dkimages' Petari IDE image, which TOS 2.06 boots on the games
machine: its root sector (AHDI boot code), bad-sector list and C: (the
AHDI driver, 4 MB) are copied as they are; the partition table is
rewritten to C: plus one 32 MB GEMDOS partition, D:, holding one folder
per game, with the byte patches in patches.txt. Stored word-swapped like the source (psctrl.cfg ide_byteorder
native). Pure python3: no mtools, no root, the same bytes every run.
The image is read back and every file compared with its source before
the script says it is done.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ahdi  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser('~')
D_START = 8194              # C: is sectors 2..8193 in the Petari image
D_SECTORS = 65536           # 32 MB
BPS, SPC, NROOT = 1024, 2, 256
DATE = ((2026 - 1980) << 9) | (11 << 5) | 22


def spec():
    out = []
    for ln in open(os.path.join(HERE, 'games.txt')):
        ln = ln.split('#', 1)[0].strip()
        if ln:
            out.append([f.strip() for f in ln.split('|')])
    return out


def take(src, kind, letter, path):
    """{relative path: (attr, bytes or None for a folder)}"""
    img = ahdi.Img(os.path.join(HOME, src), kind == 'ide')
    if kind == 'floppy':
        fs = ahdi.Fat(img, 0)
    else:
        fs = ahdi.Fat(img, ahdi.allparts(img)['CDEFGHIJKLMNOP'.index(letter)][1])
    out = {}
    pre = '' if path == '\\' else path
    for p, a, c, s in fs.walk():
        if pre and not p.startswith(pre + '\\'):
            continue
        rel = p[len(pre) + 1:]
        out[rel] = (a, None) if a & 0x10 else (a, fs.read(c, s))
    if not out:
        sys.exit('%s: nothing at %s %s' % (src, letter, path))
    return out


def patches(tree):
    """patches.txt: fixed bytes, checked before they are replaced"""
    n = 0
    for ln in open(os.path.join(HERE, 'patches.txt')):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        f, off, old, new, _why = [x.strip() for x in ln.split('|')]
        off, old, new = int(off, 16), bytes.fromhex(old), bytes.fromhex(new)
        if f not in tree or tree[f][1] is None:
            sys.exit('patch: no file %s' % f)
        a, d = tree[f]
        if len(old) != len(new) or d[off:off + len(old)] != old:
            sys.exit('patch: %s at %X does not hold %s - a different version?'
                     % (f, off, old.hex().upper()))
        tree[f] = (a, d[:off] + new + d[off + len(new):])
        n += 1
    return n


def name83(n):
    b, _, x = n.partition('.')
    if len(b) > 8 or len(x) > 3 or not b:
        sys.exit('not an 8.3 name: %s' % n)
    return (b.ljust(8) + x.ljust(3)).encode('latin1')


def dirent(n, attr, clus, size):
    raw = n if isinstance(n, bytes) else name83(n)
    return raw + bytes([attr]) + bytes(10) + struct.pack('<HHHI', 0, DATE, clus, size)


def build_d(tree):
    """GEMDOS FAT16 partition, BPS-byte logical sectors: bytes"""
    ls = BPS // 512
    total = D_SECTORS // ls                        # logical sectors (16-bit)
    root_secs = NROOT * 32 // BPS
    spf = 1
    while True:
        clusters = (total - 1 - root_secs - 2 * spf) // SPC
        need = ((clusters + 2) * 2 + BPS - 1) // BPS
        if need <= spf:
            break
        spf = need
    csize = SPC * BPS
    data_start = 1 + 2 * spf + root_secs           # logical sector
    fat = [0xFFF8, 0xFFFF] + [0] * clusters
    data = {}                                      # cluster -> bytes
    nxt = [2]

    def alloc(nbytes):
        n = max(1, (nbytes + csize - 1) // csize)
        first = nxt[0]
        if first + n - 2 > clusters:
            sys.exit('D: is full')
        for i in range(n):
            fat[first + i] = first + i + 1 if i < n - 1 else 0xFFFF
        nxt[0] += n
        return first, n

    # children of each folder ('' = root)
    kids = {'': []}
    for rel, (a, d) in sorted(tree.items()):
        parent = rel.rpartition('\\')[0]
        kids.setdefault(parent, []).append(rel)
        if d is None:
            kids.setdefault(rel, [])

    def put(c, blob):
        for i in range(0, max(len(blob), 1), csize):
            data[c + i // csize] = blob[i:i + csize]

    def write_dir(rel, own, parent):
        ents = []
        if rel:
            ents.append(dirent(b'.          ', 0x10, own, 0))
            ents.append(dirent(b'..         ', 0x10, parent, 0))
        else:
            ents.append(dirent(b'REGRESS    ', 0x08, 0, 0))
        for k in kids[rel]:
            a, d = tree[k]
            n = k.rpartition('\\')[2]
            if d is None:
                c, _ = alloc((len(kids[k]) + 2) * 32)
                ents.append(dirent(n, 0x10, c, 0))
                write_dir(k, c, own)
            else:
                c = alloc(len(d))[0] if d else 0
                if d:
                    put(c, d)
                ents.append(dirent(n, a & 0x27, c, len(d)))
        blob = b''.join(ents)
        if rel:
            put(own, blob)
        else:
            if len(ents) > NROOT:
                sys.exit('root folder full')
            root[:len(blob)] = blob

    root = bytearray(root_secs * BPS)
    write_dir('', 0, 0)

    boot = bytearray(BPS)
    boot[0:2] = b'\x60\x38'                        # bra.s: not executable (sum != $1234)
    boot[2:8] = b'PISTRG'
    boot[8:11] = b'\x52\x47\x31'                   # serial, fixed: same bytes every run
    boot[11:30] = struct.pack('<HBHBHHBHHHH', BPS, SPC, 1, 2, NROOT, total, 0xF8,
                              spf, 32, 2, 0)
    if sum(struct.unpack('>%dH' % (BPS // 2), boot)) & 0xFFFF == 0x1234:
        boot[30] ^= 1
    fatb = bytearray(spf * BPS)
    for i, v in enumerate(fat):
        struct.pack_into('<H', fatb, i * 2, v)
    out = bytearray(total * BPS)
    out[0:BPS] = boot
    out[BPS:BPS + spf * BPS] = fatb
    out[BPS + spf * BPS:BPS + 2 * spf * BPS] = fatb
    out[(1 + 2 * spf) * BPS:(1 + 2 * spf) * BPS + len(root)] = root
    for c, blob in data.items():
        o = (data_start + (c - 2) * SPC) * BPS
        out[o:o + len(blob)] = blob
    return bytes(out), clusters, nxt[0] - 2


def main():
    dst = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/dkimages/regress-games.img')
    rows = spec()
    tree = {}
    for folder, src, kind, letter, path in rows:
        tree[folder] = (0x10, None)
        for rel, v in take(src, kind, letter, path).items():
            tree[folder + '\\' + rel] = v
    npatch = patches(tree)
    d, clusters, used = build_d(tree)

    head_src = None
    for folder, src, kind, letter, path in rows:
        if kind == 'ide':
            head_src = src
            break
    pet = ahdi.Img(os.path.join(HOME, head_src), True)
    head = bytearray(pet.rd(0, D_START))
    p = ahdi.parts(pet)
    if not p or p[0][1] != 2 or p[0][2] != D_START - 2:
        sys.exit('%s: C: is not sectors 2..%d' % (head_src, D_START - 1))
    rs = head[0:512]
    rs[0x1c6 + 12:0x1c6 + 48] = bytes(36)
    rs[0x1c6 + 12:0x1c6 + 24] = b'\x01BGM' + struct.pack('>II', D_START, D_SECTORS)
    rs[0x1c2:0x1c6] = struct.pack('>I', D_START + D_SECTORS)
    rs[0x1fe:0x200] = b'\0\0'
    s = sum(struct.unpack('>256H', bytes(rs))) & 0xFFFF
    rs[0x1fe:0x200] = struct.pack('>H', (0x1234 - s) & 0xFFFF)   # still bootable
    head[0:512] = rs

    img = bytes(head) + d
    sw = bytearray(len(img))
    sw[0::2], sw[1::2] = img[1::2], img[0::2]
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(sw)

    # read it back as the emulator will, and compare every file
    chk = ahdi.Img(tmp, True)
    r = chk.rd(0)
    if sum(struct.unpack('>256H', r)) & 0xFFFF != 0x1234:
        sys.exit('root sector checksum wrong')
    ps = ahdi.allparts(chk)
    if [x[1:] for x in ps] != [(2, D_START - 2), (D_START, D_SECTORS)]:
        sys.exit('partition table wrong: %s' % ps)
    fs = ahdi.Fat(chk, D_START)
    got = {}
    for p_, a, c, sz in fs.walk():
        got[p_[1:]] = (a, None) if a & 0x10 else (a, fs.read(c, sz))
    for k, (a, dd) in tree.items():
        if k not in got:
            sys.exit('missing on the new disk: ' + k)
        if dd is not None and got[k][1] != dd:
            sys.exit('differs on the new disk: ' + k)
    if len(got) != len(tree):
        sys.exit('extra files on the new disk: %s' % sorted(set(got) - set(tree)))
    os.replace(tmp, dst)
    nfiles = sum(1 for v in tree.values() if v[1] is not None)
    print('%s: %d MB, C: (AHDI driver) + D: %d games, %d files, %d patch(es) (patches.txt), '
          '%d of %d clusters used; read back and checked'
          % (dst, len(img) >> 20, len(rows), nfiles, npatch, used, clusters))
    for folder, *_ in rows:
        print('  D:\\' + folder)


if __name__ == '__main__':
    main()
