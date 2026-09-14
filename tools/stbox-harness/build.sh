#!/bin/bash
# Headless STBOX harness (any host). From the repo root:
#   tools/stbox-harness/build.sh <name> [-DSTBOX_NO_TRACE] [-DSTBOX_OLD_VIDCOUNTER] [-DSTBOX_OLD_FDC_INTRQ] [-DSTBOX_FDC_TRACE]
# -> tools/stbox-harness/harness-<name>
set -e
NAME=$1; shift
R=$(cd "$(dirname "$0")/../.." && pwd)
H=$R/tools/stbox-harness
O=$H/obj-$NAME; mkdir -p "$O"
# disk loaders come straight from stbox_host.c so they never drift
awk '/^static FILE \*fopen_ci/,/^int stbox_disk_insert_path/' $R/platforms/atari/stbox/stbox_host.c | sed '$d' > "$O/diskload.inc"
CF="-O2 -D_GNU_SOURCE -DSTBOX_WATCH_RING=65536 -I$R -I$R/include -I$R/platforms/atari -I$R/platforms/atari/stbox -I$R/third_party/musashi -I$O $*"
gcc $CF -c $R/platforms/atari/stbox/stbox.c -o $O/stbox.o
gcc $CF -c $R/platforms/atari/stbox/stbox_blit.c -o $O/stbox_blit.o
gcc $CF -c $R/third_party/musashi/m68kcpu.c -o $O/m68kcpu.o
gcc $CF -c $R/third_party/musashi/m68kops.c -o $O/m68kops.o
gcc $CF -c $R/third_party/musashi/m68kdasm.c -o $O/m68kdasm.o
gcc $CF -I$R/third_party/musashi/softfloat -c $R/third_party/musashi/softfloat/softfloat.c -o $O/softfloat.o
gcc $CF -c $H/harness.c -o $O/harness.o
gcc -o $H/harness-$NAME $O/*.o -lm
echo built $H/harness-$NAME
