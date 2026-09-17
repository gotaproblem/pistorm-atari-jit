#!/bin/sh
# Host tests for the pre-boot setup page. Run anywhere - no Pi, no Atari -
# including on the Pi itself.
#
# Sanitizers are OFF by default: ASan's fixed shadow mapping does not fit
# the Pi's 39-bit user address space and aborts before main() with
# "sanitizer_allocator_primary64.h ... CHECK failed". Run SAN=1 sh run.sh
# on a development box (x86-64) to get them.
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/setup_harness
[ "$SAN" = 1 ] && san="-fsanitize=address,undefined" || san=""

# setup_hdmi.c is linked but never called here: the page calls sh_present()
# on every redraw, and nothing opens DRM unless sh_open() is called.
# shellcheck disable=SC2086
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 $san \
    -I"$here/.." -I"$here/../../../.." -I/usr/include/libdrm \
    "$here/harness.c" \
    "$here/../shifter_setup.c" "$here/../setup_input.c" \
    "$here/../setup_cfg.c" "$here/../setup_enums.c" \
    "$here/../setup_hdmi.c" "$here/../setup_page.c" \
    -o "$out" -ldrm
"$out"

# the section-aware .cfg loader has its own harness: it builds the real
# config_file.c against stubs
sh "$here/cfg_section_run.sh"
