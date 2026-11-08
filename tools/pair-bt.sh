#!/bin/bash
# pair-bt.sh - pair a Bluetooth keyboard and mouse to the Pi's BUILT-IN
# Bluetooth, so the USB receiver/dongle can come out.
#
#   sudo bash ~/pair-bt.sh          set up, then pair devices one at a time
#   sudo bash ~/pair-bt.sh --list   show what is paired / connected
#
# What it does:
#   1. checks the built-in controller is enabled (config.txt has no
#      disable-bt overlay; rfkill unblocked; hciuart + bluetooth services on)
#   2. low-latency settings in /etc/bluetooth/main.conf (backed up first):
#      LE connection interval 7.5-11.25 ms instead of BlueZ's default
#      30-50 ms (the 24/40 you saw), no slave latency, fast reconnect
#   3. pairs, trusts and connects each device you pick from a scan, so they
#      reconnect by themselves at every boot
# The emulator needs nothing: Bluetooth keyboards and mice appear as
# ordinary input devices and it picks them up when they connect.

set -u
[ "$(id -u)" -eq 0 ] || exec sudo bash "$0" "$@"

say()  { printf '\n== %s\n' "$*"; }
warn() { printf '!! %s\n' "$*"; }

if [ "${1:-}" = "--list" ]; then
    for mac in $(bluetoothctl devices | awk '{print $2}'); do
        bluetoothctl info "$mac" | awk -v m="$mac" '
            /Name:/{n=substr($0,index($0,$2))} /Paired:/{p=$2} /Connected:/{c=$2}
            END{printf "%s  %-30s paired=%s connected=%s\n", m, n, p, c}'
    done
    exit 0
fi

# ---------------------------------------------------------------- 1. controller
say "1/3 Built-in Bluetooth controller"
CFG=/boot/firmware/config.txt
[ -f "$CFG" ] || CFG=/boot/config.txt
if grep -Eq '^[[:space:]]*dtoverlay=(pi3-)?disable-bt' "$CFG" 2>/dev/null; then
    warn "$CFG disables Bluetooth:"
    grep -En '^[[:space:]]*dtoverlay=(pi3-)?disable-bt' "$CFG"
    read -r -p "Comment that line out (backup kept) and reboot afterwards? [y/N] " a
    if [ "${a,,}" = "y" ]; then
        cp -a "$CFG" "$CFG.bak-$(date +%Y%m%d-%H%M%S)"
        sed -i -E 's/^([[:space:]]*dtoverlay=(pi3-)?disable-bt.*)$/# \1   # pair-bt.sh/' "$CFG"
        echo "Done. Reboot (sudo reboot), then run this script again."
    fi
    exit 1
fi

rfkill unblock bluetooth 2>/dev/null
systemctl enable --now hciuart.service >/dev/null 2>&1   # Pi 3/Zero: UART-attached BT
systemctl enable --now bluetooth.service >/dev/null 2>&1

ok=0
for _ in $(seq 1 15); do
    if bluetoothctl show 2>/dev/null | grep -q '^Controller'; then ok=1; break; fi
    sleep 1
done
if [ $ok -ne 1 ]; then
    warn "No Bluetooth controller came up."
    echo "Look at:  sudo systemctl status hciuart bluetooth   and   dmesg | grep -i -E 'hci|bluetooth'"
    exit 1
fi
bluetoothctl show | grep -E '^Controller|Name:|Powered:'

# ---------------------------------------------------------------- 2. main.conf
say "2/3 Low-latency settings (/etc/bluetooth/main.conf)"
CONF=/etc/bluetooth/main.conf
[ -f "$CONF" ] || touch "$CONF"
cp -a "$CONF" "$CONF.bak-$(date +%Y%m%d-%H%M%S)"
python3 - "$CONF" <<'PY'
import re, sys
path = sys.argv[1]
want = {
    "General": {"FastConnectable": "true"},
    "Policy":  {"AutoEnable": "true", "ReconnectAttempts": "7",
                "ReconnectIntervals": "1,2,4,8,16,32,64"},
    # units of 1.25 ms: 6 = 7.5 ms, 9 = 11.25 ms
    "LE":      {"MinConnectionInterval": "6", "MaxConnectionInterval": "9",
                "ConnectionLatency": "0"},
}
lines = open(path).read().splitlines()
done = {s: set() for s in want}
out, sec = [], None
for ln in lines:
    m = re.match(r'^\s*\[(\w+)\]\s*$', ln)
    if m:
        if sec in want:                           # add missing keys at end of section
            for k, v in want[sec].items():
                if k not in done[sec]:
                    out.append(f"{k} = {v}"); done[sec].add(k)
        sec = m.group(1)
        out.append(ln); continue
    m = re.match(r'^\s*#?\s*(\w+)\s*=', ln)
    if sec in want and m and m.group(1) in want[sec] and m.group(1) not in done[sec]:
        k = m.group(1); out.append(f"{k} = {want[sec][k]}"); done[sec].add(k); continue
    out.append(ln)
if sec in want:
    for k, v in want[sec].items():
        if k not in done[sec]:
            out.append(f"{k} = {v}"); done[sec].add(k)
for s, kv in want.items():                         # sections not present at all
    missing = {k: v for k, v in kv.items() if k not in done[s]}
    if missing:
        out.append(""); out.append(f"[{s}]")
        out += [f"{k} = {v}" for k, v in missing.items()]
open(path, "w").write("\n".join(out) + "\n")
PY
systemctl restart bluetooth.service
sleep 2
grep -E '^(FastConnectable|AutoEnable|ReconnectAttempts|MinConnectionInterval|MaxConnectionInterval|ConnectionLatency)' "$CONF"

# ---------------------------------------------------------------- 3. pairing
say "3/3 Pairing"
bluetoothctl power on >/dev/null
bluetoothctl pairable on >/dev/null

pair_one() {
    local mac=$1
    echo "Pairing $mac ..."
    echo "If a 6-digit passkey appears below: type it ON THE BLUETOOTH KEYBOARD, then press Enter on it."
    {
        echo "agent KeyboardDisplay"
        echo "default-agent"
        echo "pair $mac"
        for _ in $(seq 1 40); do                  # up to 40 s for passkey entry
            sleep 1
            bluetoothctl info "$mac" 2>/dev/null | grep -q 'Paired: yes' && break
        done
        echo "trust $mac"
        echo "connect $mac"
        sleep 4
        echo "quit"
    } | bluetoothctl 2>&1 | grep -E --line-buffered -i 'passkey|pin|confirm|successful|failed|error|connected: yes'
    if bluetoothctl info "$mac" | grep -q 'Paired: yes'; then
        bluetoothctl info "$mac" | grep -E 'Name:|Paired:|Trusted:|Connected:'
        return 0
    fi
    warn "Pairing $mac failed - put the device back in pairing mode and try again."
    return 1
}

while :; do
    echo
    read -r -p "Put ONE keyboard or mouse in pairing mode, then press Enter (q to finish): " a
    [ "${a,,}" = "q" ] && break
    echo "Scanning for 20 seconds..."
    bluetoothctl --timeout 20 scan on >/dev/null 2>&1
    mapfile -t devs < <(bluetoothctl devices | while read -r _ mac name; do
        bluetoothctl info "$mac" | grep -q 'Paired: yes' || printf '%s %s\n' "$mac" "$name"
    done)
    if [ ${#devs[@]} -eq 0 ]; then
        warn "Nothing new found. Is it in pairing mode (usually a held button / blinking light)?"
        continue
    fi
    for i in "${!devs[@]}"; do printf '  %d) %s\n' $((i + 1)) "${devs[$i]}"; done
    read -r -p "Number to pair (Enter to rescan): " n
    [[ "$n" =~ ^[0-9]+$ ]] && [ "$n" -ge 1 ] && [ "$n" -le ${#devs[@]} ] || continue
    pair_one "$(awk '{print $1}' <<< "${devs[$((n - 1))]}")"
done

say "Paired devices"
bash "$0" --list
echo
echo "They reconnect by themselves at boot. Remove the USB receiver when both work."
