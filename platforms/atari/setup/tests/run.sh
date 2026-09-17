#!/bin/sh
# Host test for the setup page's shifter code. Runs anywhere - no Pi.
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/setup_harness
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -fsanitize=address,undefined \
    -I"$here/.." -I"$here/../../../.." "$here/harness.c" "$here/../shifter_setup.c" -o "$out"
"$out"
