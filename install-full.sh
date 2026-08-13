#!/usr/bin/env bash
#
# PiSTorm ATARI JIT — installation helper (hardened / idempotent)
#
#   - installs build + runtime dependencies
#   - lays out the runtime file tree (won't overwrite existing files)
#   - MERGES the required /boot/firmware settings instead of clobbering them
#   - optionally builds the emulator and sets it to auto-start on boot
#   - optionally creates a Samba share for dropping games/images onto the Pi
#
# Safe to run more than once. Non-interactive use:
#   BUILD=1 SERVICE=1 SAMBA=0 ./install-full.sh
#
# cryptodad / hardened rewrite — 2026
#
set -euo pipefail

# --------------------------------------------------------------------------
# Locate ourselves. HERE = the repo (pistorm-atari-jit); ROOT = its parent,
# which holds roms/ configs/ dkimages/ ... so the emulator's "../" paths work.
# --------------------------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot                 # older Raspberry Pi OS layout

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n'  "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n'  "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] && die "Run as your normal user (the script uses sudo where needed), not as root."
command -v sudo >/dev/null || die "sudo is required but not installed."
[ -d "$BOOT" ]          || die "No $BOOT — is this Raspberry Pi OS?"

# ask VAR "Prompt" DEFAULT(y|n)
#   honours env var $VAR (1/y/yes = yes); else prompts on a TTY; else uses default.
ask() {
  local var="$1" prompt="$2" def="${3:-n}" ans
  if [ -n "${!var:-}" ]; then
    case "${!var}" in 1|y|Y|yes) return 0 ;; *) return 1 ;; esac
  fi
  if [ -t 0 ]; then
    read -r -p "$prompt [$([ "$def" = y ] && echo 'Y/n' || echo 'y/N')] " ans || true
    case "${ans:-$def}" in y|Y|yes) return 0 ;; *) return 1 ;; esac
  fi
  [ "$def" = y ]
}

copy_once() {                                # src dst  (never overwrites dst)
  [ -e "$1" ] || { warn "missing $1 — skipped"; return 0; }
  [ -e "$2" ] || cp "$1" "$2"
}

# copy_newer src dst
#   For things we SHIP and expect to improve - the GEM programs. copy_once is
#   wrong for these, because a re-run after a git pull would leave the old
#   binary in place and the bug you just fixed still on the Atari. Plain cp is
#   also wrong: it would flatten someone's own build of the same name. `cp -u`
#   splits the difference by only copying when ours is newer.
copy_newer() {
  [ -e "$1" ] || { warn "missing $1 — skipped"; return 0; }
  cp -u "$1" "$2"
}

# --------------------------------------------------------------------------
# Uninstall: undo the *system* integration only. Your games/ROMs/configs and
# the built emulator are left in place (delete $ROOT yourself for a full wipe).
# --------------------------------------------------------------------------
uninstall() {
  say "Uninstalling PiSTorm system integration (user files are left untouched)"

  # systemd service
  if [ -e /etc/systemd/system/pistorm.service ]; then
    say "Removing systemd service + returning tty1 to the login prompt"
    sudo systemctl disable --now pistorm.service 2>/dev/null || true
    sudo rm -f /etc/systemd/system/pistorm.service
    sudo systemctl daemon-reload
    sudo systemctl start getty@tty1.service 2>/dev/null || true
  else
    warn "No pistorm.service — skipped"
  fi

  # Samba share
  if [ -e /etc/samba/smb.conf ] && grep -q '^\[pistorm\]' /etc/samba/smb.conf; then
    say "Removing Samba [pistorm] share"
    tmp="$(mktemp)"
    sudo awk '/^\[pistorm\]/{skip=1;next} skip&&/^\[/{skip=0} !skip{print}' \
        /etc/samba/smb.conf > "$tmp"
    sudo cp "$tmp" /etc/samba/smb.conf
    rm -f "$tmp"
    sudo systemctl restart smbd 2>/dev/null || true
  else
    warn "No [pistorm] Samba share — skipped"
  fi

  # Boot files — restore the pre-install backups (exact inverse of the merge)
  if [ -e "$BOOT/config-bak.txt" ]; then
    say "Restoring config.txt from backup"
    sudo cp "$BOOT/config-bak.txt" "$BOOT/config.txt"
  else
    warn "No config-bak.txt — leaving config.txt as-is"
  fi
  if [ -e "$BOOT/cmdline-bak.txt" ]; then
    say "Restoring cmdline.txt from backup"
    sudo cp "$BOOT/cmdline-bak.txt" "$BOOT/cmdline.txt"
  else
    warn "No cmdline-bak.txt — leaving cmdline.txt as-is"
  fi

  say "Uninstall done."
  echo "  Left in place: $ROOT (roms/ dkimages/ configs/ ...) and $HERE/emulator"
  echo "  Full wipe (careful — deletes your games/images):  rm -rf \"$ROOT\""
  warn "Reboot to apply the restored boot files:  sudo reboot"
}

# --------------------------------------------------------------------------
# Argument handling
# --------------------------------------------------------------------------
case "${1:-}" in
  -u|--uninstall) uninstall; exit 0 ;;
  -h|--help)
    cat <<EOF
PiSTorm Atari installer (idempotent).
  ./install-full.sh               install: deps + files + boot merges,
                                      then prompts for build / service / samba
  ./install-full.sh --uninstall   restore boot files, remove service + share
                                      (leaves your games/ROMs/configs alone)

Non-interactive env overrides:
  BUILD=1  SERVICE=1  SAMBA=1  PISTORM_CFG=games.cfg  ./install-full.sh
EOF
    exit 0 ;;
  "") ;;
  *) die "Unknown argument '$1' (try --help)" ;;
esac

# --------------------------------------------------------------------------
# 0. Pre-flight (install path only): JIT requirements + no desktop environment
# --------------------------------------------------------------------------
require_jit() {
  say "Checking JIT requirements"
  # The Amiberry JIT backend is AArch64-only: 32-bit OS can neither build nor run it.
  local arch; arch="$(uname -m)"
  case "$arch" in
    aarch64|arm64) ;;
    *) die "PiSTorm JIT requires a 64-bit (aarch64) OS — detected '$arch'. Reflash with 64-bit Raspberry Pi OS." ;;
  esac
  # Board model
  local model; model="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
  case "$model" in
    *"Pi 4"*|*"Pi 400"*|*"Compute Module 4"*) ;;
    *) warn "Unrecognised/older board '$model' — JIT is validated on Pi 4 only" ;;
  esac
  # RAM — compiling the CPU/JIT units is memory-hungry.
  local memmb; memmb=$(( $(awk '/MemTotal/{print $2}' /proc/meminfo) / 1024 ))
  if [ "$memmb" -lt 1800 ]; then
    warn "Only ${memmb} MB RAM — if the build is OOM-killed, use 'make HEAVY_OPT=-O0' and/or add swap."
  fi
  say "OK: $arch | $model | ${memmb} MB RAM"
}

no_desktop() {
  local dm p found=""
  [ "$(systemctl get-default 2>/dev/null)" = graphical.target ] && found="graphical boot target"
  for dm in lightdm gdm3 gdm sddm xdm; do
    if systemctl is-active --quiet "$dm" 2>/dev/null || systemctl is-enabled --quiet "$dm" 2>/dev/null; then
      found="${found:+$found, }$dm"
    fi
  done
  for p in Xorg Xwayland labwc wayfire weston mutter lxsession; do
    if pgrep -x "$p" >/dev/null 2>&1; then found="${found:+$found, }$p"; break; fi
  done
  [ -z "$found" ] && return 0

  warn "Desktop environment detected: $found"
  warn "PiSTorm JIT can not run with desktop environment enabled."
  if ask KILLGUI "Disable the desktop and switch to console boot?" n; then
    say "Switching to console boot + disabling the display manager"
    sudo systemctl set-default multi-user.target
    for dm in lightdm gdm3 gdm sddm xdm; do
      if systemctl list-unit-files "$dm.service" >/dev/null 2>&1; then
        sudo systemctl disable "$dm" 2>/dev/null || true
      fi
    done
    warn "Desktop disabled — takes full effect after reboot (your current session is left running)."
  else
    die "PiSTorm JIT can not run with desktop environment enabled."
  fi
}

require_jit
no_desktop

# --------------------------------------------------------------------------
# 1. Dependencies (build + runtime). The -dev packages pull in the runtime
#    libs, so this covers both "build from source" and "run a prebuilt binary".
# --------------------------------------------------------------------------
say "Installing dependencies"
sudo apt-get update
# build + runtime libs. What each of the non-obvious ones is actually for:
#   libmpg123-dev   host MP3 decode (the MP3PLAY NatFeat)
#   libav*/libsw*   host VIDEO decode (the VIDPLAY NatFeat). The Makefile
#                   hard-errors without these, so a BUILD=1 run used to die at
#                   the dependency preflight - they were missing here while
#                   being required there.
#   libjpeg-dev     MJPEG frames in avrecord.c (screen capture)
#   zlib1g-dev      PNG screendumps, which are written in-process - NOT by
#                   ffmpeg, despite what this comment used to imply
#   ffmpeg          the command-line tool, used only by capmux.sh to mux a
#                   finished capture. Nothing in the emulator shells out to it
#   libasound2-dev  ALSA backend, needed only for an SDL3 source build
#
# cifs-utils is easy to confuse with the optional Samba step further down, so:
# Samba is the SERVER side, sharing the Pi's files OUT to your desktop.
# cifs-utils is the CLIENT side, letting the Pi mount a share IN from a NAS or
# PC - which is how a media library on another machine appears inside
# atari-share, and therefore inside a HOSTFS drive. Video files are much too
# large to want a second copy of on the SD card. Installed unconditionally
# because it is tiny, and discovering it is missing usually happens when the Pi
# is already sitting headless next to the Atari.
sudo apt-get install -y \
  build-essential g++ make pkg-config cmake git \
  libmpg123-dev libjpeg-dev libdrm-dev libslirp-dev zlib1g-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev \
  libasound2-dev ffmpeg cifs-utils

# ---- SDL3 -----------------------------------------------------------------
# Debian ships libsdl3-dev from trixie (Raspberry Pi OS 13) onwards. On a
# bookworm image the package does not exist in apt, so build it from source
# once (audio is all we use; the build takes a few minutes on a Pi 4).
if apt-cache show libsdl3-dev >/dev/null 2>&1; then
  say "Installing SDL3 from apt"
  sudo apt-get install -y libsdl3-dev
elif pkg-config --exists sdl3; then
  say "SDL3 already present ($(pkg-config --modversion sdl3)) — skipping"
else
  SDL3_TAG=release-3.2.16
  say "libsdl3-dev not in apt (pre-trixie OS) — building SDL3 $SDL3_TAG from source"
  SDL3_TMP="$(mktemp -d)"
  git clone --depth 1 --branch "$SDL3_TAG" https://github.com/libsdl-org/SDL.git "$SDL3_TMP/SDL"
  cmake -S "$SDL3_TMP/SDL" -B "$SDL3_TMP/build" \
        -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
  cmake --build "$SDL3_TMP/build" -j"$(nproc)"
  sudo cmake --install "$SDL3_TMP/build"
  sudo ldconfig
  rm -rf "$SDL3_TMP"
  pkg-config --exists sdl3 \
    || die "SDL3 installed to /usr/local but pkg-config can't see it — check PKG_CONFIG_PATH includes /usr/local/lib/pkgconfig"
  say "SDL3 $(pkg-config --modversion sdl3) installed from source"
fi

# --------------------------------------------------------------------------
# 2. Runtime file tree (idempotent)
# --------------------------------------------------------------------------
say "Creating file structure under $ROOT"
mkdir -p "$ROOT"/roms "$ROOT"/configs "$ROOT"/atari-share \
         "$ROOT"/dkimages/fdd "$ROOT"/screendumps

say "Installing default configs / EmuTOS / blank floppy (existing files kept)"
copy_once "$HERE/configs/atari.cfg"         "$ROOT/configs/atari.cfg"
copy_once "$HERE/configs/master.cfg"        "$ROOT/configs/master.cfg"
copy_once "$HERE/configs/emutos-aranym.rom" "$ROOT/roms/emutos-aranym.rom"
copy_once "$HERE/configs/720k.st"           "$ROOT/dkimages/fdd/720k.st"

# capture muxer: shipped in configs/, run from the emulator directory
copy_once "$HERE/configs/capmux.sh"         "$HERE/capmux.sh"
chmod +x "$HERE/capmux.sh" 2>/dev/null || true

# ---- GEM programs ---------------------------------------------------------
# The host-side NatFeats (MP3PLAY, VIDPLAY) are useless without something on
# the Atari to drive them, and atari-share is the one directory the guest can
# actually reach: it is what a HOSTFS drive points at. Anywhere else and you
# are back to writing them onto a floppy image to get them across.
if [ -d "$HERE/configs/gem-binaries" ]; then
  say "Installing GEM programs into $ROOT/atari-share"
  for prg in "$HERE"/configs/gem-binaries/*.PRG "$HERE"/configs/gem-binaries/*.TTP; do
    [ -e "$prg" ] || continue                 # nullglob is not set; skip misses
    copy_newer "$prg" "$ROOT/atari-share/$(basename "$prg")"
  done
  ls "$ROOT"/atari-share/*.PRG "$ROOT"/atari-share/*.TTP 2>/dev/null \
    | sed 's|.*/|    |' || true
else
  warn "no configs/gem-binaries — GEM programs not installed"
fi

if [ -f "$HERE/configs/pistormbg.jpg" ]; then
  say "Installing desktop wallpaper into $ROOT/atari-share/bg"
  mkdir -p "$ROOT/atari-share/bg"
  copy_newer "$HERE/configs/pistormbg.jpg" "$ROOT/atari-share/bg/pistormbg.jpg"
fi

# APJ-OS Wi-Fi onboarding: after flashing the SD image, a user on any OS
# can edit wifi.txt on the FAT boot partition; this service applies it
# at boot and then renames the file so credentials leave the FAT.
if [ -f "$HERE/configs/apj-wifi" ]; then
  say "Installing APJ Wi-Fi onboarding (wifi.txt on the boot partition)"
  sudo install -m 755 "$HERE/configs/apj-wifi" /usr/local/sbin/apj-wifi
  sudo install -m 644 "$HERE/configs/apj-wifi.service" /etc/systemd/system/apj-wifi.service
  sudo systemctl enable apj-wifi.service
  if [ -d /boot/firmware ] && [ ! -f /boot/firmware/wifi.txt ] \
       && [ ! -f /boot/firmware/wifi.txt.applied ]; then
    sudo tee /boot/firmware/wifi.txt >/dev/null <<'WEOF'
# APJ-OS Wi-Fi setup
# Edit the three lines below with your network details, save, and boot.
# The file is renamed to wifi.txt.applied once the settings are taken.
SSID=YourNetworkName
PASSWORD=YourWifiPassword
COUNTRY=GB
WEOF
  fi
fi

# APJ-OS ssh host-key generation: RPi OS trixie's regenerate service did
# not run in the field (sshd refused to start with no host keys after a
# sanitized image's first boot). This oneshot runs ssh-keygen -A only
# when the ed25519 host key is absent - inert on an installed system,
# decisive on a freshly flashed card.
if [ -f "$HERE/configs/apj-sshkeys.service" ]; then
  say "Installing APJ ssh host-key first-boot generation"
  sudo install -m 644 "$HERE/configs/apj-sshkeys.service" /etc/systemd/system/apj-sshkeys.service
  sudo systemctl enable apj-sshkeys.service
fi


# --------------------------------------------------------------------------
# 3. Boot configuration — MERGE, don't clobber the user's settings.
# --------------------------------------------------------------------------
say "Backing up boot files (once)"
[ -e "$BOOT/config-bak.txt" ]  || sudo cp "$BOOT/config.txt"  "$BOOT/config-bak.txt"
[ -e "$BOOT/cmdline-bak.txt" ] || sudo cp "$BOOT/cmdline.txt" "$BOOT/cmdline-bak.txt"

if [ -e "$HERE/configs/config.txt" ]; then
  if sudo grep -q 'PiStorm-Atari' "$BOOT/config.txt"; then
    warn "config.txt already contains the PiStorm block — left as-is."
  else
    say "Appending PiStorm block to config.txt"
    printf '\n' | sudo tee -a "$BOOT/config.txt" >/dev/null
    sudo tee -a "$BOOT/config.txt" < "$HERE/configs/config.txt" >/dev/null
  fi
fi

if [ -e "$HERE/configs/cmdline.txt" ]; then
  # cmdline.txt MUST stay a single line. Strip newlines from the fragment and
  # append it inline to the existing line, only if not already present.
  CMDADD="$(tr -d '\r\n' < "$HERE/configs/cmdline.txt")"
  KEY="$(printf '%s' "$CMDADD" | awk '{print $1}')"     # e.g. isolcpus=2,3
  if [ -n "$KEY" ] && sudo grep -qF "$KEY" "$BOOT/cmdline.txt"; then
    warn "cmdline.txt already has '$KEY' — left as-is."
  elif [ -n "$CMDADD" ]; then
    say "Appending kernel args to cmdline.txt (kept on one line)"
    sudo sed -i "s|[[:space:]]*\$|${CMDADD}|" "$BOOT/cmdline.txt"
  fi
fi

# --------------------------------------------------------------------------
# 4. Optional: build the emulator
# --------------------------------------------------------------------------
if ask BUILD "Build the emulator now (make)?" y; then
  MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
  case "$MODEL" in
    *"Pi 4"*|*"Pi 400"*|*"Compute Module 4"*) PIMODEL=PI4 ;;
    *"Pi 3"*)                                 PIMODEL=PI3 ;;
    *"Zero 2"*)                               PIMODEL=PI02W ;;
    *) PIMODEL=PI4; warn "Unrecognised model '$MODEL' — defaulting to PI4" ;;
  esac
  say "Building for $PIMODEL ($MODEL)"
  make -C "$HERE" PIMODEL="$PIMODEL"
  [ -x "$HERE/emulator" ] || die "Build finished but ./emulator is missing."
  say "Built $HERE/emulator"
fi

# --------------------------------------------------------------------------
# 5. Optional: auto-start on boot (systemd). KMSDRM/native_hdmi needs the
#    console, and the GPIO/DMA bus needs root — so we run on tty1 as root and
#    take tty1 away from getty (appliance style).
# --------------------------------------------------------------------------
if ask SERVICE "Auto-start the emulator on boot (systemd)?" n; then
  CFG="${PISTORM_CFG:-master.cfg}"
  say "Installing pistorm.service (config: ../configs/$CFG)"
  sudo tee /etc/systemd/system/pistorm.service >/dev/null <<UNIT
[Unit]
Description=PiSTorm Atari emulator
After=multi-user.target
Conflicts=getty@tty1.service

[Service]
Type=simple
User=root
WorkingDirectory=$HERE
ExecStart=$HERE/emulator --config ../configs/$CFG
Restart=on-failure
RestartSec=2
StandardInput=tty
StandardOutput=journal
StandardError=journal
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes

[Install]
WantedBy=multi-user.target
UNIT
  sudo systemctl daemon-reload
  sudo systemctl enable pistorm.service
  warn "Enabled: starts on next boot. Test now with:  sudo systemctl start pistorm"
  warn "Change the config file by editing /etc/systemd/system/pistorm.service"
fi

# --------------------------------------------------------------------------
# 6. Optional: Samba share (drop games/images onto the Pi from another machine)
# --------------------------------------------------------------------------
if ask SAMBA "Create a Samba share for the PiSTorm files?" n; then
  say "Installing + configuring Samba share 'pistorm' -> $ROOT"
  sudo apt-get install -y samba samba-common-bin
  if grep -q '^\[pistorm\]' /etc/samba/smb.conf 2>/dev/null; then
    warn "smb.conf already has a [pistorm] share — left as-is."
  else
    sudo tee -a /etc/samba/smb.conf >/dev/null <<SMB

[pistorm]
   comment = PiSTorm Atari files
   path = $ROOT
   browseable = yes
   read only = no
   guest ok = yes
   create mask = 0664
   directory mask = 0775
   force user = $USER
SMB
    sudo systemctl restart smbd
    warn "Guest-writable share created for home-LAN convenience."
    warn "If this Pi is on an untrusted network, lock it down (valid users / smbpasswd)."
  fi
fi

# --------------------------------------------------------------------------
say "Done."
echo
echo "  Runtime tree : $ROOT"
if [ -x "$HERE/emulator" ]; then
  echo "  Emulator     : $HERE/emulator  (built)"
else
  echo "  Emulator     : not built yet  ->  make -C \"$HERE\""
fi
echo "  Bring your own : TOS ROM -> $ROOT/roms/   (or use the bundled EmuTOS)"
echo "                   games/images -> $ROOT/dkimages/"
echo "  GEM programs : $ROOT/atari-share/   (point a HOSTFS drive here)"
echo
echo "  Media on another PC or NAS? cifs-utils is installed. Mount the share"
echo "  as a subdirectory of atari-share and it appears on the HOSTFS drive:"
echo "      mkdir -p $ROOT/atari-share/media"
echo "  then one line in /etc/fstab - INSTALL-README.md has the full recipe,"
echo "  including why 'nofail' is not optional on a headless Pi."
echo
warn "Reboot to apply the boot-file changes:  sudo reboot"
