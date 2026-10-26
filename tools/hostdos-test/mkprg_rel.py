#!/usr/bin/env python3
"""ELF (ld -Ttext=0 -q) -> TOS .PRG with a relocation table (text only,
bss from .bss). For the bench's CHILD.PRG."""
import struct, subprocess, sys
elf, out, objdump, objcopy = sys.argv[1:5]
subprocess.check_call([objcopy, '-O', 'binary', '-j', '.text', elf, out + '.text'])
text = open(out + '.text', 'rb').read()
if len(text) & 1: text += b'\0'
bss = 0
for line in subprocess.check_output([objdump, '-h', elf], text=True).splitlines():
    f = line.split()
    if len(f) > 2 and f[1] == '.bss': bss = int(f[2], 16)
rel = []
sec = None
for line in subprocess.check_output([objdump, '-r', elf], text=True).splitlines():
    if line.startswith('RELOCATION RECORDS FOR'): sec = line.split('[')[1].split(']')[0]; continue
    f = line.split()
    if sec == '.text' and len(f) >= 3 and f[1] == 'R_68K_32': rel.append(int(f[0], 16))
rel.sort()
tab = b''
if rel:
    tab = struct.pack('>I', rel[0]); prev = rel[0]
    for r in rel[1:]:
        d = r - prev
        while d > 254: tab += b'\1'; d -= 254
        tab += bytes([d]); prev = r
    tab += b'\0'
else:
    tab = b'\0\0\0\0'
hdr = struct.pack('>HIIIIIIH', 0x601A, len(text), 0, bss, 0, 0, 0, 0)
open(out, 'wb').write(hdr + text + tab)
