#!/bin/bash
H=$(cd "$(dirname "$0")" && pwd)
R=$H/../..
rc=0
for cfg in "68000 0D" "68000 15" "68000 19" "68030 19" "68030 15"; do
  set -- $cfg
  echo "=== CPU $1, GEMDOS $2"
  $H/hostdos-bench $R/atari-tools/hostdos/PSHOSTFS.PRG $H/obj/TEST.PRG $H/obj/CHILD.PRG $1 $2 | grep -v '^listing' || rc=1
done
exit $rc
