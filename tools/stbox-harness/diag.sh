#!/bin/sh
# diag.sh TOS.ROM IMAGE [secs] - one run, everything needed to see why a game
# draws wrongly in the box: frames (PPM + row summary) and the GLUE's per-line
# decisions at 2..secs seconds, with and without ST bus-slot padding, plus
# the exception/verdict log. Result: stbox-diag-<image>.tar.gz in the cwd.
set -e
TOS=$1; IMG=$2; SECS=${3:-20}
H=$(cd "$(dirname "$0")" && pwd)
[ -x "$H/harness-diag" ] || "$H/build.sh" diag -DSTBOX_LINE_LOG >/dev/null
N=$(basename "$IMG" | tr -c 'A-Za-z0-9.\n' '_' | cut -c1-40)
O=stbox-diag-$N; rm -rf "$O"; mkdir -p "$O"
AT=2,3,4,5,6,8,10,12,15,20,25,30
for w in 1 0; do
  PISTORM_STBOX_WAIT4=$w "$H/harness-diag" --tos "$TOS" --ram 4096 --disk "$IMG" \
      --secs "$SECS" --dump "$O/w$w" --dumpat $AT -v > "$O/w$w.log" 2>&1 || true
done
for f in "$O"/*.ppm; do command -v convert >/dev/null && convert "$f" "${f%.ppm}.png" && rm "$f" || true; done
tar czf "$O.tar.gz" "$O"
echo "wrote $O.tar.gz"
