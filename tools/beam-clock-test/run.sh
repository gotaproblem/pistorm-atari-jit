#!/bin/sh
# Host test for platforms/atari/video/beam_clock.c (any host, no Pi).
# From the repo root:  sh tools/beam-clock-test/run.sh
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${TMPDIR:-/tmp}/beam-clock-test.$$
cc -std=gnu11 -O2 -Wall -Wextra -DBC_TEST -I"$R" \
   "$R/tools/beam-clock-test/test.c" "$R/platforms/atari/video/beam_clock.c" -lm -o "$O"
rc=0
for s in 50 60 dbuf150 dbuf312 ste border noread; do "$O" $s || rc=1; done
BC_TEST_PPM=150 "$O" 50 || rc=1
BC_TEST_PPM=-150 "$O" 60 || rc=1
rm -f "$O"
[ $rc = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit $rc
