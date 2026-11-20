#!/bin/bash
# pi4-tune.sh - trim Raspberry Pi OS (Lite) on a Pi 4 / Pi 3 / 3A+ / Zero 2 W
# for pistorm-atari-jit (install-full.sh runs it: TUNE=1)
#
# Linux keeps cores 0-1, cores 2-3 are left quiet for the emulator.
# Nothing the project uses is touched: SSH, Samba, Bluetooth (Xbox pads),
# USB, audio (DMA sound), HDMI/KMS + v3d (fVDI/PSVIDEL/PSGFX), the V4L2 codec.
#
#   sudo ./pi4-tune.sh            dry run - shows what would change
#   sudo ./pi4-tune.sh --apply    apply, then reboot
#   sudo ./pi4-tune.sh --status   check the result after reboot
#   sudo ./pi4-tune.sh --revert   put everything back, then reboot
#
# Options (environment):
#   KEEP_WIFI=auto|1|0     auto = keep Wi-Fi only if wlan0 has an address (default auto)
#   KEEP_AVAHI=1|0         mDNS - the Mac finds the Samba share by name with it (default 1)
#   KEEP_SWAP=1|0          dphys-swapfile (default 1)
#   KEEP_DOCKER=0|1        docker + containerd - nothing in PiStorm uses them,
#                          ~48 MB RSS on a 3A+ (default 0 = disable)
#   KEEP_WINBIND=0|1       winbind - only for joining a Windows/AD domain; the
#                          Samba share (smbd) works without it, ~20 MB
#                          (default 0 = disable)
#   KEEP_NMBD=0|1          nmbd (NetBIOS names): the Mac finds the share through
#                          avahi, Windows by IP; 3.5 s at boot (default 0 = disable)
#   KEEP_CLOUDINIT=auto|1|0  cloud-init: 4-5 s at every boot, needed only for the
#                          Imager's first-boot setup (user, Wi-Fi, ssh). auto =
#                          keep it in a chroot (an SD image still to be flashed)
#                          and until first boot has run, off after (default auto)
#   KEEP_SERIAL=0|1        the serial console (console=serial0,...) - slows the
#                          kernel's boot (default 0 = remove it)
#   MITIGATIONS_OFF=0|1    add mitigations=off to the kernel line (default 0)

set -euo pipefail

MODE="${1:---dry-run}"
LINUX_CPUS="0-1"
EMU_CPUS="2-3"
LINUX_MASK="3"
KEEP_WIFI="${KEEP_WIFI:-auto}"
KEEP_AVAHI="${KEEP_AVAHI:-1}"
KEEP_SWAP="${KEEP_SWAP:-1}"
KEEP_DOCKER="${KEEP_DOCKER:-0}"
KEEP_WINBIND="${KEEP_WINBIND:-0}"
KEEP_NMBD="${KEEP_NMBD:-0}"
KEEP_CLOUDINIT="${KEEP_CLOUDINIT:-auto}"
KEEP_SERIAL="${KEEP_SERIAL:-0}"
MITIGATIONS_OFF="${MITIGATIONS_OFF:-0}"

STATE_DIR=/var/backups/pi4-tune
ORIG="$STATE_DIR/orig"
SVC_STATE="$STATE_DIR/disabled-services"
RUNTIME_BIN=/usr/local/sbin/pi4-tune-runtime
RUNTIME_UNIT=/etc/systemd/system/pi4-tune-runtime.service
SYSCTL_FILE=/etc/sysctl.d/90-pi4-tune.conf
MARK_BEGIN="# >>> pi4-tune"
MARK_END="# <<< pi4-tune"

if [ -d /boot/firmware ]; then BOOT=/boot/firmware; else BOOT=/boot; fi
CMDLINE="$BOOT/cmdline.txt"
CONFIG="$BOOT/config.txt"
CLOUD_OFF=/etc/cloud/cloud-init.disabled

say()  { printf '%s\n' "$*"; }
act()  { if [ "$MODE" = "--apply" ]; then "$@"; else say "  would run: $*"; fi; }

[ "$(id -u)" -eq 0 ] || { say "run with sudo"; exit 1; }
grep -qE "Raspberry Pi (4|3|Zero 2)" /proc/device-tree/model 2>/dev/null || \
    say "warning: not a Pi 4 / Pi 3 / Zero 2 W ($(tr -d '\0' < /proc/device-tree/model 2>/dev/null))"

# ---------------------------------------------------------------- kernel config
kcfg() {   # kcfg CONFIG_FOO -> 0 if =y
    modprobe configs 2>/dev/null || true
    [ -r /proc/config.gz ] && zgrep -q "^$1=y" /proc/config.gz
}

# ---------------------------------------------------------------- services
SERVICES=(
    ModemManager.service
    triggerhappy.service triggerhappy.socket
    cups.service cups.socket cups-browsed.service
    packagekit.service udisks2.service
    rsyslog.service
    NetworkManager-wait-online.service
    rpi-eeprom-update.service
    apt-daily.timer apt-daily-upgrade.timer man-db.timer
    e2scrub_all.timer fstrim.timer
    # boot time, measured (Pi 4 10-11 Nov, Pi 3A+ 19 Nov 2026): neither is
    # used here - AD domain controller, LVM snapshot clean-up
    samba-ad-dc.service e2scrub_reap.service
)
[ "$KEEP_NMBD"  = "0" ] && SERVICES+=(nmbd.service)
[ "$KEEP_AVAHI" = "0" ] && SERVICES+=(avahi-daemon.service avahi-daemon.socket)
[ "$KEEP_SWAP"  = "0" ] && SERVICES+=(dphys-swapfile.service)
# Memory, not CPU: measured on the Pi 3A+ at the APJ-OS desktop (18 Oct 2026)
# these two freed ~65 MB - available went to 131 MB. smbd/nmbd stay.
[ "$KEEP_DOCKER"  = "0" ] && SERVICES+=(docker.service docker.socket containerd.service)
[ "$KEEP_WINBIND" = "0" ] && SERVICES+=(winbind.service)

wifi_in_use() { ip -4 addr show wlan0 2>/dev/null | grep -q 'inet '; }
want_wifi_off() {
    case "$KEEP_WIFI" in
        1) return 1 ;;
        0) return 0 ;;
        *) ! in_chroot && ! wifi_in_use ;;     # a chroot's wlan0 is the build host's
    esac
}

in_chroot() { systemd-detect-virt -q --chroot 2>/dev/null; }
want_cloudinit_off() {
    case "$KEEP_CLOUDINIT" in
        1) return 1 ;;
        0) return 0 ;;
        *) ! in_chroot && [ -e /var/lib/cloud/instance/boot-finished ] ;;
    esac
}

# ---------------------------------------------------------------- cmdline.txt
new_cmdline() {
    local line add=() p
    line="$(tr -d '\n' < "$CMDLINE")"
    has() { [[ " $line " == *" $1="* || " $line " == *" $1 "* ]]; }

    if has isolcpus; then
        say "  isolcpus already set: $(grep -o 'isolcpus=[^ ]*' <<<"$line") - left as is" >&2
    else
        add+=("isolcpus=domain,managed_irq,2,3")
    fi
    has irqaffinity || add+=("irqaffinity=$LINUX_CPUS")
    if kcfg CONFIG_RCU_NOCB_CPU; then
        has rcu_nocbs || add+=("rcu_nocbs=$EMU_CPUS")
    else
        say "  kernel lacks CONFIG_RCU_NOCB_CPU - rcu_nocbs skipped" >&2
    fi
    if kcfg CONFIG_NO_HZ_FULL; then
        has nohz_full || add+=("nohz_full=$EMU_CPUS")
    else
        say "  kernel lacks CONFIG_NO_HZ_FULL - nohz_full skipped (tick stays on cores 2-3)" >&2
    fi
    has nosoftlockup || add+=("nosoftlockup")
    has audit        || add+=("audit=0")
    if [ "$MITIGATIONS_OFF" = "1" ]; then has mitigations || add+=("mitigations=off"); fi

    for p in "${add[@]}"; do line="$line $p"; done
    if [ "$KEEP_SERIAL" = "0" ]; then
        line="$(sed -E 's/(^| )console=(serial0|ttyS0|ttyAMA0),[0-9]+//g' <<<"$line")"
        line="${line# }"
    fi
    printf '%s\n' "$line"
}

# ---------------------------------------------------------------- config.txt
config_block() {
    say "$MARK_BEGIN"
    # force_turbo only on the Pi 4: on BCM2837 (3A+/3B/Zero 2 W) it breaks
    # HDMI - the firmware refuses vc4-hdmi's pixel clock (doc/SMALL-BOARDS.md)
    say "[pi4]"
    say "force_turbo=1          # lock ARM + core clock at max: no DVFS ramp, steady GPIO bus timing"
    say "[all]"
    say "disable_splash=1"
    say "boot_delay=0"
    say "camera_auto_detect=0   # no camera stack"
    say "display_auto_detect=0  # no DSI panel probing (HDMI unaffected)"
    if want_wifi_off; then say "dtoverlay=disable-wifi"; fi
    say "$MARK_END"
}

# ---------------------------------------------------------------- runtime pieces
runtime_script() {
cat <<EOF
#!/bin/sh
# pi4-tune runtime: keep IRQs and unbound kernel work on cores $LINUX_CPUS
for d in /proc/irq/[0-9]*; do
    echo $LINUX_CPUS > "\$d/smp_affinity_list" 2>/dev/null || true
done
echo $LINUX_CPUS > /proc/irq/default_smp_affinity 2>/dev/null || \
    echo $LINUX_MASK > /proc/irq/default_smp_affinity 2>/dev/null || true
echo $LINUX_MASK > /sys/devices/virtual/workqueue/cpumask 2>/dev/null || true
for f in /sys/devices/virtual/workqueue/*/cpumask; do
    echo $LINUX_MASK > "\$f" 2>/dev/null || true
done
exit 0
EOF
}

runtime_unit() {
cat <<EOF
[Unit]
Description=pi4-tune: keep IRQs and kernel work off the emulator cores
After=sysinit.target

[Service]
Type=oneshot
ExecStart=$RUNTIME_BIN
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
}

sysctl_conf() {
cat <<EOF
# pi4-tune
vm.stat_interval = 10
vm.swappiness = 10
kernel.watchdog = 0
EOF
}

# ---------------------------------------------------------------- status
status() {
    say "model:      $(tr -d '\0' < /proc/device-tree/model)"
    say "kernel:     $(uname -r)"
    say "cmdline:    $(cat /proc/cmdline)"
    say "isolated:   $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '?')"
    say "nohz_full:  $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo 'n/a')"
    say "arm clock:  $(vcgencmd measure_clock arm 2>/dev/null)"
    say "core clock: $(vcgencmd measure_clock core 2>/dev/null)"
    say "temp:       $(vcgencmd measure_temp 2>/dev/null)"
    say "throttled:  $(vcgencmd get_throttled 2>/dev/null)   (0x0 = never throttled)"
    say "governor:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    say ""
    say "interrupts on cores 2-3 (should only be timer/IPI lines):"
    awk 'NR==1{print "            " $0; next} ($4+$5)>0 {printf "%s\n",$0}' /proc/interrupts
    say ""
    say "cloud-init: $([ -e "$CLOUD_OFF" ] && echo "off ($CLOUD_OFF)" || echo on)"
    say "services still enabled from the trim list:"
    for s in "${SERVICES[@]}"; do
        systemctl is-enabled "$s" 2>/dev/null | grep -qx enabled && say "  $s"
    done
    say "(end)"
}

# ---------------------------------------------------------------- revert
revert() {
    [ -d "$ORIG" ] || { say "nothing to revert ($ORIG missing)"; exit 1; }
    cp -a "$ORIG/cmdline.txt" "$CMDLINE"
    cp -a "$ORIG/config.txt"  "$CONFIG"
    if [ -f "$SVC_STATE" ]; then
        while read -r s; do [ -n "$s" ] && systemctl enable "$s" 2>/dev/null || true; done < "$SVC_STATE"
        rm -f "$SVC_STATE"
    fi
    [ -f "$STATE_DIR/cloud-init-off" ] && rm -f "$CLOUD_OFF" "$STATE_DIR/cloud-init-off"
    systemctl disable pi4-tune-runtime.service 2>/dev/null || true
    rm -f "$RUNTIME_UNIT" "$RUNTIME_BIN" "$SYSCTL_FILE"
    systemctl daemon-reload
    rm -rf "$ORIG"
    say "reverted - reboot to take effect"
}

# ---------------------------------------------------------------- main
case "$MODE" in
    --status) status; exit 0 ;;
    --revert) revert; exit 0 ;;
    --apply|--dry-run) ;;
    *) say "usage: $0 [--dry-run|--apply|--status|--revert]"; exit 1 ;;
esac

say "== boot files ($BOOT)"
NEW_CMD="$(new_cmdline)"
say "  cmdline now: $(cat "$CMDLINE")"
say "  cmdline new: $NEW_CMD"
if grep -q "$MARK_BEGIN" "$CONFIG"; then
    say "  config.txt already has a pi4-tune block - left as is"
    ADD_CONFIG=0
else
    say "  config.txt gets:"; config_block | sed 's/^/    /'
    ADD_CONFIG=1
fi
want_wifi_off && say "  Wi-Fi will be disabled (wlan0 has no address; KEEP_WIFI=1 to keep it)"

# A service an earlier --apply disabled that is now to be kept (KEEP_NMBD=1
# on a re-run, say) is enabled again and dropped from the record
TO_ENABLE=()
if [ -f "$SVC_STATE" ]; then
    while read -r s; do
        [ -n "$s" ] || continue
        case " ${SERVICES[*]} " in *" $s "*) ;; *) TO_ENABLE+=("$s") ;; esac
    done < <(sort -u "$SVC_STATE")
fi
if [ ${#TO_ENABLE[@]} -gt 0 ]; then
    say "== services to enable again (disabled by an earlier run, now kept)"
    for s in "${TO_ENABLE[@]}"; do say "  $s"; done
fi

say "== services to disable"
TO_DISABLE=()
for s in "${SERVICES[@]}"; do
    if systemctl is-enabled "$s" 2>/dev/null | grep -qx enabled; then
        TO_DISABLE+=("$s"); say "  $s"
    fi
done
[ ${#TO_DISABLE[@]} -eq 0 ] && say "  (none enabled)"
CLOUD=0
if [ ! -e "$CLOUD_OFF" ] && [ -d /etc/cloud ] && want_cloudinit_off; then
    CLOUD=1; say "  cloud-init (first boot done): $CLOUD_OFF"
fi

say "== runtime: IRQ + workqueue affinity -> cores $LINUX_CPUS, sysctl $SYSCTL_FILE"

if [ "$MODE" != "--apply" ]; then
    say ""
    say "dry run only. Apply with: sudo $0 --apply"
    exit 0
fi

mkdir -p "$ORIG"
[ -f "$ORIG/cmdline.txt" ] || cp -a "$CMDLINE" "$ORIG/cmdline.txt"
[ -f "$ORIG/config.txt"  ] || cp -a "$CONFIG"  "$ORIG/config.txt"

printf '%s\n' "$NEW_CMD" > "$CMDLINE"
if [ "$ADD_CONFIG" = "1" ]; then { echo; config_block; } >> "$CONFIG"; fi

for s in "${TO_ENABLE[@]}"; do
    if in_chroot; then systemctl enable "$s" 2>/dev/null || true
    else systemctl enable --now "$s" 2>/dev/null || true; fi
    grep -vx "$s" "$SVC_STATE" > "$SVC_STATE.new" || true
    mv "$SVC_STATE.new" "$SVC_STATE"
done

for s in "${TO_DISABLE[@]}"; do
    systemctl disable --now "$s" 2>/dev/null || true
    echo "$s" >> "$SVC_STATE"
done

if [ "$CLOUD" = "1" ]; then touch "$CLOUD_OFF" "$STATE_DIR/cloud-init-off"; fi

runtime_script > "$RUNTIME_BIN"; chmod 755 "$RUNTIME_BIN"
runtime_unit   > "$RUNTIME_UNIT"
sysctl_conf    > "$SYSCTL_FILE"
systemctl daemon-reload
if in_chroot; then
    systemctl enable pi4-tune-runtime.service       # takes effect on the image's first boot
else
    systemctl enable --now pi4-tune-runtime.service
    sysctl -q -p "$SYSCTL_FILE" || true
fi

say ""
say "applied. Originals saved in $ORIG. Reboot, then: sudo $0 --status"
