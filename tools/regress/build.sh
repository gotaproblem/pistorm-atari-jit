#!/bin/sh
# Build the regression guest programs and the disk images they run from.
# Needs m68k binutils (Debian/Raspberry Pi OS: sudo apt install
# binutils-m68k-linux-gnu) and python3. The built .PRG files are committed
# too, so the Pi only needs this when a guest program changes.
#   tools/regress/build.sh          from the repo root (or anywhere)
set -e
H=$(cd "$(dirname "$0")" && pwd)
G=$H/guest
O=$H/build; mkdir -p "$O/obj"
for t in m68k-linux-gnu-as m68k-linux-gnu-ld m68k-linux-gnu-objdump m68k-linux-gnu-objcopy; do
    command -v $t >/dev/null || { echo "missing $t (sudo apt install binutils-m68k-linux-gnu)"; exit 2; }
done
for src in $G/*.S; do
    n=$(basename "$src" .S)
    [ "$n" = lib ] && continue
    N=$(echo "$n" | tr a-z A-Z)
    m68k-linux-gnu-as -m68000 --register-prefix-optional -I"$G" -o "$O/obj/$n.o" "$src"
    m68k-linux-gnu-ld -q -Ttext=0 -o "$O/obj/$n.elf" "$O/obj/$n.o"
    python3 "$H/mkprg_rel.py" "$O/obj/$n.elf" "$G/$N.PRG" m68k-linux-gnu-objdump m68k-linux-gnu-objcopy
    rm -f "$G/$N.PRG.text"
    echo "built $N.PRG ($(stat -c %s "$G/$N.PRG") bytes)"
done
