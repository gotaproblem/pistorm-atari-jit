#!/bin/sh
# diag.sh TOS.ROM IMAGE [secs] - headless STBOX capture (no window, no
# emulator): runs the game in the harness, saves frames + per-line GLUE
# decisions, packs stbox-diag-<image>.tar.gz in the current folder.
# HARNESS_KEYS passes through (e.g. "10:31" taps 'n' at 10 s).
TOS=$1; IMG=$2; SECS=${3:-20}
H=$(cd "$(dirname "$0")" && pwd)
[ -f "$TOS" ] || { echo "diag: TOS ROM not found: $TOS"; exit 1; }
[ -f "$IMG" ] || { echo "diag: disk image not found: $IMG"; exit 1; }
if [ ! -x "$H/harness-diag" ]; then
  echo "diag: building the harness (about a minute)..."
  "$H/build.sh" diag -DSTBOX_LINE_LOG > /tmp/stbox-diag-build.log 2>&1 ||
    { echo "diag: build FAILED - last lines of /tmp/stbox-diag-build.log:"; tail -20 /tmp/stbox-diag-build.log; exit 1; }
fi
N=$(basename "$IMG" | tr -c 'A-Za-z0-9.\n' '_' | cut -c1-40)
O=stbox-diag-$N; rm -rf "$O"; mkdir -p "$O"
AT=2,4,6,8,10,12,14,16,18,20,22,25,28,30,35,40,45,50,55,60
for w in 1 0; do
  echo "diag: running $SECS guest seconds (pass $((2 - w)) of 2)..."
  PISTORM_STBOX_WAIT4=$w HARNESS_VIEW=monitor "$H/harness-diag" --tos "$TOS" --ram 4096 --disk "$IMG" \
      --secs "$SECS" --dump "$O/w$w" --dumpat $AT -v > "$O/w$w.log" 2>&1
  grep VERDICT "$O/w$w.log" | cut -c1-100
done
tar czf "$O.tar.gz" "$O"
echo "diag: done - send me $(pwd)/$O.tar.gz"
