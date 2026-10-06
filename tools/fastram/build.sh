#!/bin/sh
# FASTRAM.PRG from fastram.s - needs binutils for m68k (apt: binutils-m68k-linux-gnu).
# Position independent: the PRG has no relocation table (absflag set).
set -e
cd "$(dirname "$0")"
m68k-linux-gnu-as -m68030 --register-prefix-optional -o fastram.o fastram.s
m68k-linux-gnu-objcopy -O binary -j .text fastram.o fastram.bin
python3 - <<'PY'
import struct
t = open('fastram.bin', 'rb').read()
if len(t) & 1: t += b'\0'
bss = 32 + 512 + 64 + 32 + 65536          # vars, jar, INF buffer, digits, _FRB buffer
hdr = struct.pack('>HIIIIIIH', 0x601A, len(t), 0, bss, 0, 0, 0, 1)
open('FASTRAM.PRG', 'wb').write(hdr + t)
PY
rm -f fastram.o fastram.bin
echo "FASTRAM.PRG built"
