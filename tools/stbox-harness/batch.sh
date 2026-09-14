#!/bin/bash
# batch.sh <tos> <secs> <variant...>  -> table: image | verdict per variant (+detail)
TOS=$1; SECS=$2; shift 2
G=${GAMES:-/mnt/user-data/uploads/games-share}
ROMS=${ROMS:-/mnt/user-data/uploads/roms}
printf "%-58s" "image"; for v in "$@"; do printf " | %-8s" "$v"; done; echo
for img in "$G"/*.st "$G"/*.msa; do
  [ -f "$img" ] || continue
  n=$(basename "$img" | cut -c1-56)
  printf "%-58s" "$n"
  for v in "$@"; do
    out=$(timeout 120 $(dirname "$0")/harness-$v ${HARNESS_STE:+--ste} --tos $ROMS/$TOS.rom --ram 4096 --disk "$img" --secs $SECS 2>&1 | grep VERDICT)
    verdict=$(echo "$out" | awk '{print $2}')
    fatal=$(echo "$out" | grep -o "last-fatal=vec[0-9]*@[0-9A-F]*" | sed 's/last-fatal=//')
    printf " | %-8s" "${verdict:-ERR}"
    [ -n "$fatal" ] && [ "$verdict" != "RUNNING" ] && printf "%s" " $fatal"
  done
  echo
done
