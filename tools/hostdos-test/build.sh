#!/bin/bash
# HOSTDOS host bench. From the repo root:  tools/hostdos-test/build.sh
# then:  tools/hostdos-test/run.sh     (68000 + 68030 frames, three GEMDOS versions)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
H=$R/tools/hostdos-test
O=$H/obj; mkdir -p "$O" "$O/68k"
AS="m68k-linux-gnu-as -m68000 --register-prefix-optional"
make -C $R/atari-tools/hostdos binutils >/dev/null
$AS -o $O/68k/test.o $H/test.S
m68k-linux-gnu-objcopy -O binary -j .text $O/68k/test.o $O/68k/test.bin
python3 $R/atari-tools/hostdos/mkprg.py $O/68k/test.bin $O/TEST.PRG
$AS -o $O/68k/child.o $H/child.S
m68k-linux-gnu-ld -q -Ttext=0 -o $O/68k/child.elf $O/68k/child.o
python3 $H/mkprg_rel.py $O/68k/child.elf $O/CHILD.PRG m68k-linux-gnu-objdump m68k-linux-gnu-objcopy
# a copy of the vendored Musashi with the 010-040 generations back on (the
# STBOX build compiles them out), so the 68030 long-frame path is tested too
M=$O/musashi; rm -rf "$M"; cp -r $R/third_party/musashi "$M"
sed -i '/STBOX is a plain-ST 68000/,/M68K_EMULATE_EC040 *M68K_OPT_OFF/s/M68K_OPT_OFF/M68K_OPT_ON/' "$M/m68kconf.h"
CF="-O1 -g -I$R -I$M -DM68K_ILLG_HAS_CALLBACK=M68K_OPT_SPECIFY_HANDLER -DM68K_ILLG_CALLBACK=bench_illg"
gcc $CF -c $M/m68kcpu.c -o $O/m68kcpu.o
gcc $CF -c $M/m68kops.c -o $O/m68kops.o
gcc $CF -c $M/m68kdasm.c -o $O/m68kdasm.o
gcc $CF -I$M/softfloat -c $M/softfloat/softfloat.c -o $O/softfloat.o
gcc $CF -c $H/harness.c -o $O/harness.o
g++ -O1 -g -std=gnu++17 -Wall -Wextra -c $R/platforms/atari/network/hostdos.cpp -o $O/hostdos.o
g++ -o $H/hostdos-bench $O/*.o -lm
echo built $H/hostdos-bench
