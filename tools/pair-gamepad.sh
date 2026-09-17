#!/bin/bash
# pair-gamepad.sh - pair a Bluetooth game controller with the Pi, unattended.
#
#   sudo tools/pair-gamepad.sh              # first "Xbox" pad it can find
#   sudo tools/pair-gamepad.sh 14:CB:65:E6:41:92
#   sudo tools/pair-gamepad.sh "8BitDo"     # any name substring
#   sudo tools/pair-gamepad.sh --fresh ...  # forget the old bond first
#
# Pairing is a one-off per pad: once paired and trusted it reconnects by
# itself whenever you press its power button, and the emulator picks it up
# by hotplug ("usb gamepad" in the cfg). Run this again only for a new pad
# or after the pad has been paired to something else in the meantime.
#
# Everything runs inside ONE bluetoothctl session with a NoInputNoOutput
# agent. That matters: an Xbox pad pairs with LE Secure Connections and
# the kernel raises a "user confirmation" that the agent must answer -
# separate one-shot bluetoothctl calls leave no agent alive for it and
# the pad reports "Numeric comparison failed" / AuthenticationFailed.
#
# Exit 0 = pad connected and an evdev node exists.
set -u

SCAN_SECS=${SCAN_SECS:-12}
TRIES=${TRIES:-3}
FRESH=0
if [ "${1:-}" = "--fresh" ]; then FRESH=1; shift; fi
WANT=${1:-Xbox}

say()  { printf '[pair] %s\n' "$*"; }
die()  { printf '[pair] ERROR: %s\n' "$*" >&2; cleanup; exit 1; }
is_mac(){ [[ "$1" =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]]; }

[ "$(id -u)" = 0 ] || die "run with sudo (bluetoothctl needs it here)"
command -v bluetoothctl >/dev/null || die "bluetoothctl not installed (apt install bluez)"

# ---- one persistent bluetoothctl session -----------------------------
LOG=$(mktemp /tmp/pair-gamepad.XXXXXX)
FIFO=$(mktemp -u /tmp/pair-gamepad.fifo.XXXXXX)
mkfifo "$FIFO"
cleanup() {
    if [ -n "${BTPID:-}" ]; then
        cmd quit 2>/dev/null; sleep 0.5
        kill "$BTPID" 2>/dev/null
        exec 3>&-
        BTPID=
    fi
    rm -f "$FIFO" "$LOG"
}
trap cleanup EXIT
bluetoothctl < "$FIFO" > "$LOG" 2>&1 &
BTPID=$!
exec 3> "$FIFO"
cmd()  { printf '%s\n' "$*" >&3; }
# wait up to $1 seconds for a line matching $2 (regex) to appear after
# log offset $3; prints the line, returns 1 on timeout
waitfor() {
    local secs=$1 pat=$2 from=${3:-0} i line
    for ((i = 0; i < secs * 4; i++)); do
        line=$(tail -c +$((from + 1)) "$LOG" | grep -m1 -E "$pat") && { printf '%s\n' "$line"; return 0; }
        sleep 0.25
    done
    return 1
}
mark() { stat -c %s "$LOG"; }
# After "remove" BlueZ has no object for the pad until the scan sees it
# advertise again; "pair" before that answers "Device ... not available".
seen() {
    local m
    m=$(mark); cmd info "$MAC"; sleep 0.7
    ! tail -c +$((m + 1)) "$LOG" | grep -q "not available"
}
wait_seen() {
    local secs=${1:-25} i
    for ((i = 0; i < secs; i++)); do
        seen && return 0
        [ $i = 3 ] && say "  waiting for the pad to advertise - hold the pair button until it flashes FAST"
        sleep 1
    done
    return 1
}

# ---- radio up ---------------------------------------------------------
rfkill unblock bluetooth 2>/dev/null
systemctl is-active --quiet bluetooth || systemctl start bluetooth
sleep 1
m=$(mark); cmd power on
waitfor 5 "Changing power on succeeded|Powered: yes|already" "$m" >/dev/null \
    || die "adapter will not power on (rfkill list; dmesg | grep -i bluetooth)"
cmd agent NoInputNoOutput          # Just Works: confirmations auto-accepted
cmd default-agent
cmd pairable on

# ---- find the pad -----------------------------------------------------
if is_mac "$WANT"; then
    MAC=$(echo "$WANT" | tr a-f A-F)
    cmd scan on                      # the pad must be advertising to pair
    sleep 2
else
    say "scanning ${SCAN_SECS}s for '$WANT' - put the pad in pairing mode now"
    say "(Xbox: power on, then hold the small pair button until the logo flashes fast)"
    m=$(mark); cmd scan on
    line=$(waitfor "$SCAN_SECS" "\[NEW\] Device .*$WANT|Device [0-9A-F:]{17} .*$WANT" "$m") \
        || { cmd devices; sleep 1; line=$(grep -i "Device [0-9A-F:]\{17\} .*$WANT" "$LOG" | tail -1); }
    MAC=$(echo "$line" | grep -oE '([0-9A-F]{2}:){5}[0-9A-F]{2}' | head -1)
    [ -n "$MAC" ] || die "no device matching '$WANT' seen (in pairing mode? bonded to the PC? - turn the PC's Bluetooth off)"
fi
say "pad: $MAC"

# ---- already good? ----------------------------------------------------
m=$(mark); cmd info "$MAC"; sleep 1
if [ $FRESH = 0 ] && tail -c +$((m + 1)) "$LOG" | grep -q "Paired: yes"; then
    cmd trust "$MAC"
    m=$(mark); cmd connect "$MAC"
    if waitfor 8 "Connection successful|Connected: yes" "$m" >/dev/null; then
        sleep 3
        m=$(mark); cmd info "$MAC"; sleep 1
        if tail -c +$((m + 1)) "$LOG" | grep -q "Connected: yes"; then
            say "already paired and connected"; FRESH=-1
        fi
    fi
    [ $FRESH = -1 ] || { say "paired but will not stay connected - re-pairing from scratch"; FRESH=1; }
fi
if [ $FRESH = 1 ]; then
    cmd remove "$MAC"; sleep 1
    wait_seen 30 || die "pad not seen after forgetting it - is it in pairing mode (fast flash)? PC Bluetooth off?"
fi

# ---- pair -------------------------------------------------------------
if [ $FRESH != -1 ]; then
    ok=0
    for try in $(seq 1 "$TRIES"); do
        say "pairing attempt $try/$TRIES - pad must be flashing FAST now (hold the pair button ~3 s)"
        wait_seen 30 || { say "  pad not advertising"; continue; }
        m=$(mark); cmd pair "$MAC"
        if line=$(waitfor 25 "Pairing successful|AlreadyExists|Failed to pair|AuthenticationFailed|AuthenticationCanceled|not available" "$m"); then
            case "$line" in
                *"Pairing successful"*|*AlreadyExists*) ok=1; break ;;
                *) say "  $line" ;;
            esac
        else
            say "  no answer from the pad"
        fi
        # a failed attempt can leave half a bond behind: forget it, then
        # the loop waits for the pad to be seen again before retrying
        cmd remove "$MAC"; sleep 2
    done
    [ $ok = 1 ] || die "pairing failed $TRIES times. If the PC is in range turn its Bluetooth off (the pad reconnects to its last bond instead of pairing); else update the pad firmware (Xbox Accessories app)"
    cmd trust "$MAC"
    m=$(mark); cmd connect "$MAC"
    waitfor 10 "Connection successful|Connected: yes" "$m" >/dev/null \
        || say "connect not confirmed yet, waiting for the pad to come back"
fi
cmd scan off

# ---- confirm it stays up and the kernel made a device ----------------
up=0
for i in $(seq 1 12); do
    sleep 1
    m=$(mark); cmd info "$MAC"; sleep 0.5
    if tail -c +$((m + 1)) "$LOG" | grep -q "Connected: yes"; then up=$((up + 1)); else up=0; fi
    [ $up -ge 4 ] && break
done
[ $up -ge 4 ] || die "pad connects but drops again (journalctl -u bluetooth -b | tail; sudo btmon)"

# the kernel adds the input device after HID service discovery, a few
# seconds after the link comes up - match it by the pad's Bluetooth
# address (uniq= in /proc/bus/input/devices), falling back to its name
NAME=$(grep -m1 -oE "Device $MAC .*" "$LOG" | cut -d' ' -f3- | tr -d '\r')
lmac=$(echo "$MAC" | tr A-F a-f)
node=
for i in $(seq 1 15); do
    node=$(awk -v mac="$lmac" -v name="${NAME:-Controller}" '
        /^I:/ { blk = ""; hit = 0 }
        { blk = blk $0 "\n" }
        /^U: Uniq=/ && index(tolower($0), mac) { hit = 1 }
        /^N: Name=/ && index($0, name)         { hit = 1 }
        /^H: Handlers=/ && hit { match($0, /event[0-9]+/); print substr($0, RSTART, RLENGTH); exit }
    ' /proc/bus/input/devices 2>/dev/null)
    [ -n "$node" ] && break
    sleep 1
done
if [ -n "$node" ]; then
    say "OK: $MAC connected, /dev/input/$node - the emulator will pick it up (usb gamepad)"
else
    say "connected, but no evdev node yet - check: lsmod | grep -E 'uhid|hid_microsoft'; dmesg | tail"
    exit 2
fi
