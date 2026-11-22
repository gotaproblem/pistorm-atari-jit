#!/bin/sh
# Host test for the native hotkeys on an unread keyboard (idle_test.c).
#   sh tools/hotkey-idle/run.sh
set -e
cd "$(dirname "$0")/../.."
INC="-I. -Igpio -Ithreaddep -Iinclude -Isoftfloat -Ijit -Iplatforms/atari/setup -Ipcem"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for ovr in "" -DOVR_ACCEPTS; do
    echo "== 6850 model ${ovr:-as the data sheet reads first}"
    ${CC:-cc} -std=gnu11 -O1 -w -D_GNU_SOURCE $ovr $INC -Wl,--wrap=clock_gettime -o "$TMP/t" \
        tools/hotkey-idle/idle_test.c tools/hotkey-idle/stubs.c platforms/atari/kbd_usb.c -lpthread
    "$TMP/t" | grep -v '^\[KBD\]'
done
