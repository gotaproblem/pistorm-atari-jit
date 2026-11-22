#!/bin/sh
# Run from the repo root. On the Pi: native gcc. Elsewhere: cross + qemu.
set -e
CC=gcc; RUN=
[ "$(uname -m)" = aarch64 ] || { CC=aarch64-linux-gnu-gcc; RUN=qemu-aarch64-static; }
$CC -O2 -w -static -I. -Igpio -o /tmp/wtest tools/raster-trace/test/wtest.c \
    platforms/atari/raster_trace.c -lpthread
rm -f /tmp/wtest.txt /tmp/wtest.txt.[0-9]*   # rt_dump never writes over a trace
PISTORM_RASTER_TRACE=/tmp/wtest.txt $RUN /tmp/wtest
python3 tools/raster-trace/analyse.py /tmp/wtest.txt | sed -n "/^timing/,\$p"
python3 tools/raster-trace/test/wcheck.py /tmp/wtest.txt /tmp/wtest.causes
