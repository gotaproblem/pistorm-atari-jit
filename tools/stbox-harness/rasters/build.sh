#!/bin/sh
# build.sh - assemble the raster test ROMs (needs binutils-m68k-linux-gnu)
set -e
cd "$(dirname "$0")"
mkdir -p out
for s in tb_raster cnt_split borders rez_split midline ste_split; do
    m68k-linux-gnu-as -m68000 -o out/$s.o $s.s
    m68k-linux-gnu-ld -e 0xfc0000 -Ttext=0xfc0000 --oformat=binary -o out/$s.bin out/$s.o
    # pad to a 192 KB ROM
    dd if=/dev/zero bs=1024 count=192 2>/dev/null | tr '\0' '\377' > out/$s.rom
    dd if=out/$s.bin of=out/$s.rom conv=notrunc 2>/dev/null
done
echo built: $(ls out/*.rom)
