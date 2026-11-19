#!/bin/bash
# install-nohz-kernel.sh - install a built tickless kernel package
# (nohz-kernel-6.18.50.tar.gz from build-nohz-kernel.sh) as the kernel the
# Pi boots, with no tryboot step. install-full.sh calls it (NOHZ=1); it can
# also be run by hand:
#
#   sudo tools/nohz-kernel/install-nohz-kernel.sh nohz-kernel-6.18.50.tar.gz
#
#   - /boot/firmware/nohz/: kernel8.img, the device trees in the package,
#     overlays/ (the stock /boot/firmware files are not touched)
#   - /lib/modules/6.18.50-v8-nohz (stock modules untouched)
#   - config.txt (backup once: config.txt.before-nohz-permanent) gets, at
#     the end, a block for each board the package has a device tree for -
#     [pi4] (bcm2711-rpi-4-b), [pi3] (bcm2710-rpi-3-b*), [pi02]
#     (bcm2710-rpi-zero-2*):
#         os_prefix=nohz/
#         kernel=kernel8.img      (the known name: an unknown name such as
#         arm_64bit=1              kernel8-nohz.img gave a black screen)
#         cmdline=/cmdline.txt    (the normal cmdline.txt)
#     then [all]. A board with no block boots the stock kernel.
#   - which cores run tickless is cmdline.txt's nohz_full= (install-full.sh
#     adds nohz_full=2,3: the two emulation cores)
#
# Works in a chroot (the APJ-OS SD-image build): nothing here needs the
# running kernel. Safe to re-run: an existing nohz block is left as it is.
# Undo: sudo tools/nohz-kernel/make-nohz-permanent.sh revert  (or
# install-full.sh --uninstall, which restores the pre-install config.txt).
set -euo pipefail
PKG=${1:-}
B=/boot/firmware
[ -d $B ] || B=/boot
CFG=$B/config.txt
BAK=$B/config.txt.before-nohz-permanent
REL=6.18.50-v8-nohz
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -n "$PKG" ] && [ -f "$PKG" ] || { echo "usage: sudo $0 nohz-kernel-6.18.50.tar.gz"; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
tar xzf "$PKG" -C "$T"
P="$T/nohz-kernel-6.18.50"
[ -f "$P/nohz/kernel8.img" ] && [ -d "$P/lib/modules/$REL" ] && [ -f "$P/nohz/overlays/README" ] \
    || { echo "$PKG is not a $REL package (nohz/kernel8.img, nohz/overlays, lib/modules/$REL)"; exit 1; }
grep -aq "Linux version $REL " "$P/nohz/kernel8.img" \
    || { echo "$PKG: nohz/kernel8.img is not $REL"; exit 1; }

# a block only for the boards this package can boot
boards=""
ls "$P"/nohz/bcm2711-rpi-4-b.dtb     > /dev/null 2>&1 && boards="$boards pi4"
ls "$P"/nohz/bcm2710-rpi-3-b*.dtb    > /dev/null 2>&1 && boards="$boards pi3"
ls "$P"/nohz/bcm2710-rpi-zero-2*.dtb > /dev/null 2>&1 && boards="$boards pi02"
[ -n "$boards" ] || { echo "$PKG has no Pi 3 / Zero 2 W / Pi 4 device tree"; exit 1; }

if [ "$(uname -r)" = "$REL" ] && grep -qx "os_prefix=nohz/" "$CFG" && [ "${NOHZ_FORCE:-0}" != 1 ]; then
    echo "$REL is already installed and running - nothing to do (NOHZ_FORCE=1 replaces its files)"
    exit 0
fi
echo "installing $REL for:$boards"
rm -rf "$B/nohz"
cp -r "$P/nohz" "$B/nohz"
rm -rf "/lib/modules/$REL"
cp -a "$P/lib/modules/$REL" /lib/modules/
depmod "$REL"

if grep -qx "os_prefix=nohz/" "$CFG"; then
    echo "config.txt already selects the nohz kernel - left as it is"
else
    [ -e "$BAK" ] || cp "$CFG" "$BAK"
    n=0
    for f in $boards; do
        printf '\n[%s]\nos_prefix=nohz/\nkernel=kernel8.img\narm_64bit=1\ncmdline=/cmdline.txt\n' "$f" >> "$CFG"
        n=$((n + 1))
    done
    printf '[all]\n' >> "$CFG"
    echo "config.txt now ends with:"; tail -n $((6 * n + 1)) "$CFG"
fi
grep -q "nohz_full=" "$B/cmdline.txt" \
    || echo "NOTE: cmdline.txt has no nohz_full= - the kernel allows it, but no core runs tickless until it does"
sync
echo "done - reboot; then  uname -r  -> $REL  and  cat /sys/devices/system/cpu/nohz_full"
