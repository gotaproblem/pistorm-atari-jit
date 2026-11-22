#!/bin/sh
# Host test for the regression input player (input_script.c).
set -e
cd "$(dirname "$0")/../../.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=gnu11 -O1 -Wall -D_GNU_SOURCE -I. -o "$T/t" tools/regress/input-test/test.c \
    platforms/atari/input_script.c -lpthread
"$T/t" | grep -v '^\[INPUT\]'
