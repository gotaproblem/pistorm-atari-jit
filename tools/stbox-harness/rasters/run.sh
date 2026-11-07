#!/bin/sh
# run.sh [variant] - build the raster test ROMs, run each through
# harness-<variant> (default cur), check the frame against a real ST.
# From anywhere:  tools/stbox-harness/build.sh cur && tools/stbox-harness/rasters/run.sh
cd "$(dirname "$0")"
V=${1:-cur}
./build.sh >/dev/null || exit 1
mkdir -p out
fail=0
for t in tb_raster cnt_split borders rez_split midline ste_split; do
    opt=""; [ $t = ste_split ] && opt=--ste
    ../harness-$V $opt --tos out/$t.rom --ram 4096 --secs 2 --nokeys --dump out/$t > out/$t.log 2>&1
    python3 check.py $t out/$t.rows || fail=1
done
exit $fail
