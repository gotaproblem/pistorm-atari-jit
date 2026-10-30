#!/bin/sh
# Host-side unit test for platforms/atari/mfp_hub.c (any host, no Pi).
# From the repo root:  sh tools/mfp-hub-test/run.sh
# Covers the GPIP4 edge source (one interrupt per injected IKBD byte, not
# re-delivered when a handler EOIs before reading - Risky Woods), the
# lost-edge safety net, IER gating and channel priority.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${TMPDIR:-/tmp}/mfp-hub-test.$$
cc -std=gnu11 -O1 -D_GNU_SOURCE -I"$R" -I"$R/platforms/atari" \
   "$R/tools/mfp-hub-test/test.c" "$R/platforms/atari/mfp_hub.c" -o "$O"
"$O"; rc=$?
rm -f "$O"
exit $rc
