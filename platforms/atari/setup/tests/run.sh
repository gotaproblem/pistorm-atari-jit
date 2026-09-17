#!/bin/sh
# Host test for the setup page's shifter code. Runs anywhere - no Pi, no
# Atari - including on the Pi itself.
#
# Sanitizers are OFF by default: ASan's fixed shadow mapping does not fit
# the Pi's 39-bit user address space and aborts before main() with
# "sanitizer_allocator_primary64.h ... CHECK failed". Run SAN=1 sh run.sh
# on a development box (x86-64) to get them.
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/setup_harness
[ "$SAN" = 1 ] && san="-fsanitize=address,undefined" || san=""
# shellcheck disable=SC2086
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 $san \
    -I"$here/.." -I"$here/../../../.." "$here/harness.c" "$here/../shifter_setup.c" "$here/../setup_input.c" "$here/../setup_cfg.c" \
    -o "$out"
"$out"
