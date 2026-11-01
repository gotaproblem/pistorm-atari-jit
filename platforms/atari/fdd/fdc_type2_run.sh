#!/bin/sh
# Host test for the WD1772 Type II commands (fdc_type2_test.c).
#   sh platforms/atari/fdd/fdc_type2_run.sh
set -e
cd "$(dirname "$0")/../../.."
INC="-I. -Iinclude -Igpio -Ithreaddep -Isoftfloat -Ijit -Iplatforms/atari/fdd"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
${CC:-cc} -std=gnu11 -O1 -w $INC -o "$TMP/fdc2" \
    platforms/atari/fdd/atari_fdd.c platforms/atari/fdd/fdc_type2_test.c -lpthread
"$TMP/fdc2" 2>&1 | grep -v '^\[FDD\]'
