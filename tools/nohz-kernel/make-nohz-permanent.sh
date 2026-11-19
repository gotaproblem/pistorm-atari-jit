#!/bin/bash
# make-nohz-permanent.sh - boot the 6.18.50-v8-nohz kernel every time, on
# a Pi 3 / 3+ / 3A+, Zero 2 W or Pi 4 / 400 / CM4, from /boot/firmware/nohz/
# as the tryboot test did (tryboot-nohz3.sh puts the files there - run it,
# and check the board boots right, before this).
#
#   sudo ./make-nohz-permanent.sh install   select it in config.txt
#   sudo ./make-nohz-permanent.sh revert    back to the stock kernel
#
# install:
#   - checks the running kernel is 6.18.50-v8-nohz (run it from the
#     working tryboot boot, or after one)
#   - /boot/firmware/nohz/kernel8.img (the known name: the firmware starts
#     an unknown name like kernel8-nohz.img wrongly - black screen)
#   - config.txt (backup: config.txt.before-nohz-permanent) gets at the end,
#     for each of [pi3] (3 / 3+ / 3A+), [pi02] (Zero 2 W) and [pi4]:
#         os_prefix=nohz/
#         kernel=kernel8.img
#         arm_64bit=1
#         cmdline=/cmdline.txt      <- your normal cmdline.txt, no prefix
#     then [all]. One card works on all of them. The firmware drops the
#     prefix on a board whose device tree is not in nohz/ (boots stock).
#   - removes what the failed attempts left: kernel8-nohz.img files,
#     tryboot.txt, nohz/cmdline-nohz.txt, /lib/modules/6.18.62-v8-nohz
# revert: puts config.txt.before-nohz-permanent back.
# If the Pi ever does not boot: on the Mac,
#   cp /Volumes/bootfs/config.txt.before-nohz-permanent /Volumes/bootfs/config.txt
set -euo pipefail
B=/boot/firmware
CFG=$B/config.txt
BAK=$B/config.txt.before-nohz-permanent
REL=6.18.50-v8-nohz
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

case "${1:-}" in
install)
    [ -f $B/nohz/kernel8.img ] && [ -f $B/nohz/overlays/README ] && ls $B/nohz/bcm27*.dtb > /dev/null 2>&1 \
        || { echo "$B/nohz/ is incomplete (kernel8.img, a bcm27*.dtb, overlays/README) - run tryboot-nohz3.sh"; exit 1; }
    [ -d /lib/modules/$REL ] || { echo "/lib/modules/$REL missing"; exit 1; }
    grep -aq "Linux version $REL " $B/nohz/kernel8.img \
        || { echo "$B/nohz/kernel8.img is not $REL"; exit 1; }
    if grep -qx "os_prefix=nohz/" $CFG; then echo "config.txt already selects it"; exit 0; fi
    cp $CFG $BAK
    for f in pi3 pi02 pi4; do
        printf '\n[%s]\nos_prefix=nohz/\nkernel=kernel8.img\narm_64bit=1\ncmdline=/cmdline.txt\n' $f >> $CFG
    done
    printf '[all]\n' >> $CFG
    rm -f $B/kernel8-nohz.img $B/nohz/kernel8-nohz.img $B/nohz/cmdline-nohz.txt $B/tryboot.txt
    rm -rf /lib/modules/6.18.62-v8-nohz
    sync
    echo "config.txt now ends with:"; tail -16 $CFG
    echo "backup: $BAK. Reboot; then  uname -r  -> $REL  and  cat /sys/devices/system/cpu/nohz_full  -> 2-3"
    ;;
revert)
    [ -f $BAK ] || { echo "no $BAK"; exit 1; }
    cp $BAK $CFG
    sync
    echo "stock kernel selected again - reboot (the nohz/ folder is left in place)"
    ;;
*)
    echo "usage: sudo $0 install | revert"; exit 1 ;;
esac
