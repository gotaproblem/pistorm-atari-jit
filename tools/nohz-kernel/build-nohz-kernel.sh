#!/bin/bash
# build-nohz-kernel.sh - build the Pi 4 kernel your Pi runs (6.18.50,
# raspberrypi/linux tag stable_20260911) with ONE change: nohz_full
# (CONFIG_NO_HZ_FULL + CONFIG_RCU_NOCB_CPU). Native arm64 build in a
# Debian VM - same gcc 14 as Raspberry Pi's own build.
#
# Run it from the folder it lives in (tools/nohz-kernel/ in the repo, or a
# copy of that folder - e.g. on the Pi's share, seen from the VM):
#   bash build-nohz-kernel.sh
#   BUILD_DIR=/some/dir bash build-nohz-kernel.sh   (build somewhere else)
# Needs: pi-kernel.config (the Pi 4's stock 6.18.50+rpt-rpi-v8 config,
# /proc/config.gz), tryboot-nohz3.sh and make-nohz-permanent.sh next to it.
# Which CPUs run tickless is set on the kernel command line (cmdline.txt),
# not here - on the PiStorm Pi 4 it comes out as cores 2-3, the emulation
# cores (/sys/devices/system/cpu/nohz_full). The kernel only has to allow it.
# Result: nohz-kernel-6.18.50.tar.gz next to it (kernel, device tree,
# overlays, modules, tryboot-nohz3.sh).
set -euo pipefail
SHARE=$(cd "$(dirname "$0")" && pwd)
W=${BUILD_DIR:-$HOME/pi-kernel}       # BUILD_DIR=/path overrides
TAG=stable_20260911
REL=6.18.50-v8-nohz
step() { printf '\n\033[1;32m==== %s ====\033[0m\n' "$*"; }

[ -f "$SHARE/pi-kernel.config" ] || { echo "pi-kernel.config missing in $SHARE"; exit 1; }
for f in tryboot-nohz3.sh make-nohz-permanent.sh; do
    [ -f "$SHARE/$f" ] || { echo "$f missing in $SHARE"; exit 1; }
done
[ "$(uname -m)" = aarch64 ] || { echo "this is for an arm64 VM (uname -m: $(uname -m))"; exit 1; }
mkdir -p "$W"
# the kernel tree has files that differ only in case (xt_TCPMSS.c /
# xt_tcpmss.c ...): a case-insensitive folder (a default macOS volume
# shared into the VM) breaks the source
rm -f "$W/.case_test" "$W/.CASE_TEST"
touch "$W/.case_test"
if [ -e "$W/.CASE_TEST" ]; then
    rm -f "$W/.case_test"
    echo "$W is case-INSENSITIVE (a macOS volume?) - the kernel source cannot live there."
    echo "Use a folder on the VM's own disk, or a case-sensitive volume."
    exit 1
fi
rm -f "$W/.case_test"
avail=$(df --output=avail -k "$W" | tail -1)
[ "$avail" -gt 12000000 ] || { echo "need ~12 GB free in $W, have $((avail/1024/1024)) GB"; exit 1; }

step "1/6 build tools (sudo asks for your VM password)"
sudo apt-get update -q
sudo apt-get install -y -q git bc bison flex libssl-dev make gcc libc6-dev libncurses-dev kmod cpio rsync python3
gcc --version | head -1

step "2/6 kernel source $TAG"
if [ ! -d "$W/linux/.git" ]; then
    git clone --depth 1 -b "$TAG" https://github.com/raspberrypi/linux "$W/linux"
fi
cd "$W/linux"
git log --oneline -1
make -s kernelversion

step "3/6 config: your Pi's config + nohz_full only"
rm -rf "$W/build" && mkdir -p "$W/build"
cp "$SHARE/pi-kernel.config" "$W/build/.config"
./scripts/config --file "$W/build/.config" --set-str LOCALVERSION "-v8-nohz" \
    -d NO_HZ_IDLE -e NO_HZ_FULL -e RCU_NOCB_CPU
make -s O="$W/build" olddefconfig
echo "differences from your Pi's config (version lines left out):"
diff <(grep '^CONFIG_' "$SHARE/pi-kernel.config" | sort) <(grep '^CONFIG_' "$W/build/.config" | sort) \
    | grep -v -E 'VERSION|_TEXT=' || true
grep -E '^CONFIG_(NO_HZ_FULL|RCU_NOCB_CPU)=' "$W/build/.config"
r=$(LOCALVERSION= make -s O="$W/build" kernelrelease)
echo "kernel release: $r"
[ "$r" = "$REL" ] || { echo "expected $REL"; exit 1; }

step "4/6 build Image, modules, device trees ($(nproc) cores)"
time LOCALVERSION= make -j"$(nproc)" O="$W/build" Image modules dtbs

step "5/6 package"
P="$W/pkg/nohz-kernel-6.18.50"
rm -rf "$W/pkg" && mkdir -p "$P/nohz/overlays"
# kernel8.img: the firmware decides 64-bit from the name (an unknown name
# like kernel8-nohz.img gave a black screen on the Pi 4)
cp "$W/build/arch/arm64/boot/Image" "$P/nohz/kernel8.img"
# the device tree for every board this kernel (rpi-v8) runs on: Pi 3 / 3+ /
# 3A+ (bcm2710-rpi-3-b-plus), Zero 2 W, CM3, Pi 4 / 400 / CM4 - the firmware
# picks the one for the board it is on
cp "$W/build/arch/arm64/boot/dts/broadcom/"bcm2710-rpi-*.dtb \
   "$W/build/arch/arm64/boot/dts/broadcom/"bcm2711-rpi-*.dtb "$P/nohz/"
ls "$P/nohz/"*.dtb
cp "$W/build/arch/arm64/boot/dts/overlays/"*.dtbo "$P/nohz/overlays/"
cp "$W/build/arch/arm64/boot/dts/overlays/overlay_map.dtb" "$P/nohz/overlays/" 2>/dev/null || true
cp arch/arm/boot/dts/overlays/README "$P/nohz/overlays/"
LOCALVERSION= make -s O="$W/build" INSTALL_MOD_PATH="$P" INSTALL_MOD_STRIP=1 modules_install
rm -f "$P/lib/modules/$REL/build" "$P/lib/modules/$REL/source"
cp "$W/build/.config" "$P/config-$REL"
cp "$SHARE/tryboot-nohz3.sh" "$SHARE/make-nohz-permanent.sh" "$P/"
chmod +x "$P/tryboot-nohz3.sh" "$P/make-nohz-permanent.sh"
grep -a -m1 -o "Linux version $REL [^)]*)" "$P/nohz/kernel8.img" || true

step "6/6 archive next to this script"
tar czf "$SHARE/nohz-kernel-6.18.50.tar.gz" -C "$W/pkg" nohz-kernel-6.18.50
ls -la "$SHARE/nohz-kernel-6.18.50.tar.gz"
md5sum "$SHARE/nohz-kernel-6.18.50.tar.gz"
printf '\n\033[1;32mDONE.\033[0m On the Pi (any Pi 3 / 3A+ / Zero 2 W / Pi 4), in the folder holding the archive:\n  tar xzf nohz-kernel-6.18.50.tar.gz && sudo nohz-kernel-6.18.50/tryboot-nohz3.sh\n  then, if it boots right:  sudo nohz-kernel-6.18.50/make-nohz-permanent.sh install\n'
