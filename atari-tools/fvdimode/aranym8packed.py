#!/usr/bin/env python3
"""Patch fVDI's aranym.sys so its 8-bit mode is PACKED (chunky) pixels.

Stock aranym.sys declares 8 bpp as interleaved bitplanes (format 0), so
vq_scrninfo() tells programs the screen is not 8-bit packed. The PiStorm
host framebuffer is chunky, one byte per pixel, so declaring it packed is
the truth. Changes exactly two words in the driver's mode table:

    mode[3] (8 bpp):  flags  CHECK_PREVIOUS -> CHECK_PREVIOUS|CHUNKY  (4 -> 5)
                      format 0 (interleaved) -> 2 (packed pixels)

The table (drivers/aranym/spec.c) is 36-byte Mode records:
    short bpp, flags; 6 x long MBits pointers; short code, format, clut, org
and starts with 1, 2, 4, 8 bpp entries of the form {bpp, 4, ..., 0, 0, 1, 1}
followed by {16, 7, ..., 0, 2, 2, 1}. The script insists on finding that
exact shape exactly once and refuses otherwise.

usage: aranym8packed.py ARANYM.SYS [OUT.SYS]   (default: writes ARANYM8P.SYS)
"""
import struct, sys

REC = 36

def rec(b, o):
    bpp, flags = struct.unpack_from('>hh', b, o)
    code, fmt, clut, org = struct.unpack_from('>hhhh', b, o + 28)
    return bpp, flags, code, fmt, clut, org

def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else 'ARANYM8P.SYS'
    b = bytearray(open(src, 'rb').read())
    hits = []
    for o in range(0, len(b) - REC * 5, 2):
        r = [rec(b, o + i * REC) for i in range(5)]
        if [x[0] for x in r] != [1, 2, 4, 8, 16]:
            continue
        if any(x[2:] != (0, 0, 1, 1) for x in r[:3]):
            continue
        if r[4][3] != 2:            # 16 bpp entry must be packed
            continue
        hits.append((o, r[3]))
    if len(hits) != 1:
        sys.exit('mode table not found exactly once (%d hits) - not patching'
                 % len(hits))
    o, r8 = hits[0]
    o8 = o + 3 * REC
    bpp, flags, code, fmt, clut, org = r8
    if (flags & 1) and fmt == 2:
        print('already patched (flags %d format %d)' % (flags, fmt))
        return
    if fmt != 0 or clut != 1:
        sys.exit('unexpected 8bpp entry %r - not patching' % (r8,))
    struct.pack_into('>h', b, o8 + 2, flags | 1)
    struct.pack_into('>h', b, o8 + 30, 2)
    open(dst, 'wb').write(b)
    print('mode table at file offset 0x%X; 8bpp entry: flags %d->%d, '
          'format 0->2; wrote %s' % (o, flags, flags | 1, dst))

main()
