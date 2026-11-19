#!/bin/bash
# tryboot-nohz3.sh - boot the 6.18.50-v8-nohz kernel ONCE (Pi 4 tryboot).
# Same source and config as the stock 6.18.50+rpt-rpi-v8, plus nohz_full.
#   - /boot/firmware/nohz/: kernel8.img (that name: the firmware decides
#     64-bit from it), the Pi 3 / Zero 2 W / Pi 4 device trees,
#     cmdline-nohz.txt, overlays/ (os_prefix=nohz/ in tryboot.txt)
#   - modules go to /lib/modules/6.18.50-v8-nohz (stock modules untouched)
#   - the screen shows everything (splash on, hotplug not ignored, no quiet)
#   - dtoverlay=ramoops + panic=5: a crash reboots into the STOCK kernel
#     with the log kept (systemd-pstore saves it to /var/lib/systemd/pstore)
# config.txt is not touched. If it hangs, power off and on: stock boots.
#
#   sudo ./tryboot-nohz3.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
B=/boot/firmware
REL=6.18.50-v8-nohz
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
if grep -qx "kernel=kernel8-nohz.img" $B/config.txt; then
    echo "config.txt still selects a nohz kernel - restore config.txt.before-nohz first"; exit 1
fi
[ -f "$HERE/nohz/kernel8.img" ] || { echo "$HERE/nohz/kernel8.img missing"; exit 1; }
rm -rf $B/nohz
cp -r "$HERE/nohz" $B/nohz
rm -rf "/lib/modules/$REL"
cp -a "$HERE/lib/modules/$REL" /lib/modules/
depmod "$REL"
rm -rf /var/lib/systemd/pstore/* 2>/dev/null || true
grep -vx -e "hdmi_ignore_hotplug=1" -e "disable_splash=1" $B/config.txt > $B/tryboot.txt
printf '\n[all]\nos_prefix=nohz/\nkernel=kernel8.img\narm_64bit=1\ncmdline=cmdline-nohz.txt\ndtoverlay=ramoops,total-size=0x40000,record-size=0x10000,console-size=0x10000\n' >> $B/tryboot.txt
sed -e 's/ quiet\b//' -e 's/ loglevel=3\b/ loglevel=7/' -e 's/ logo\.nologo\b//' \
    -e 's/ nosoftlockup\b//' -e 's/$/ panic=5/' $B/cmdline.txt > $B/nohz/cmdline-nohz.txt
echo "tryboot.txt (end):";  tail -6 $B/tryboot.txt
echo "cmdline-nohz.txt:";   cat $B/nohz/cmdline-nohz.txt
sync
echo "rebooting once into $REL ..."
reboot "0 tryboot"
