#!/usr/bin/env python3
"""Wrap a flat, position-independent 68k text image as a TOS .PRG
(no data, no bss, no symbols, empty relocation table). Used to build
PSVIDEL.PRG with plain binutils when the m68k-atari-mint toolchain is
not at hand: m68k-linux-gnu-as -m68000 --register-prefix-optional,
objcopy -O binary, then this."""
import struct, sys
text = open(sys.argv[1], 'rb').read()
if len(text) & 1:
    text += b'\0'
hdr = struct.pack('>HIIIIIIH', 0x601A, len(text), 0, 0, 0, 0, 0, 0)
open(sys.argv[2], 'wb').write(hdr + text + b'\0\0\0\0')
