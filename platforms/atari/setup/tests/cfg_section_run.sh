#!/bin/sh
# Host test for the section-aware .cfg loader. No Pi, no Atari.
set -e
here=$(dirname "$0")
root=$here/../../../..
out=${TMPDIR:-/tmp}/cfg_section_test
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -I"$root" -I"$root/gpio" \
    -I"$root/include" -I"$root/threaddep" \
    "$here/cfg_section_test.c" "$root/config_file/config_file.c" -o "$out"
"$out"
