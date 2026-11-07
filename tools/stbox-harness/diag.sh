#!/bin/sh
# diag.sh TOS.ROM IMAGE [secs] - (HARNESS_KEYS passes through, e.g.
#   HARNESS_KEYS="10:31,11:31,12:31" taps 'n' at 10-12 s) one run, everything needed to see why a game
# draws wrongly in the box: frames (PPM + row summary) and the GLUE's per-line
# decisions at 2..secs seconds, with and without ST bus-slot padding, plus
# the exception/verdict log. Result: stbox-diag-<image>.tar.gz in the cwd.
set -e
TOS=$1; IMG=$2; SECS=${3:-20}
H=$(cd "$(dirname "$0")" && pwd)
[ -x "$H/harness-diag" ] || "$H/build.sh" diag -DSTBOX_LINE_LOG >/dev/null 2>&1
N=$(basename "$IMG" | tr -c 'A-Za-z0-9.\n' '_' | cut -c1-40)
O=stbox-diag-$N; rm -rf "$O"; mkdir -p "$O"
AT=2,4,6,8,10,12,14,16,18,20,22,25,28,30,35,40,45,50,55,60
for w in 1 0; do
  PISTORM_STBOX_WAIT4=$w HARNESS_VIEW=monitor "$H/harness-diag" --tos "$TOS" --ram 4096 --disk "$IMG" \
      --secs "$SECS" --dump "$O/w$w" --dumpat $AT -v > "$O/w$w.log" 2>&1 || true
done
for f in "$O"/*.ppm; do command -v convert >/dev/null && convert "$f" "${f%.ppm}.png" && rm "$f" || true; done
tar czf "$O.tar.gz" "$O"
echo "wrote $O.tar.gz"
