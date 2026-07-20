# PiSTorm Atari JIT — Installer (`install-full.sh`)

`install-full.sh` sets up a Raspberry Pi to run the PiSTorm Atari JIT emulator.
It installs dependencies, lays out the runtime file tree, safely merges the
required boot-firmware settings, and can optionally build the emulator, make it
auto-start on boot, and share its folders over the network.

The script is **idempotent** — running it twice does no harm — and ships with a
clean **`--uninstall`** that reverses every system change.

---

## Requirements

- **Raspberry Pi 4** (Pi 3 and Pi Zero 2 W are also detected). 64-bit.
- **Raspberry Pi OS Lite (64-bit)** — a fresh install is fine and recommended.
- **PiSTorm board fitted to a real Atari ST/STe.**
- Run as your **normal user** (the script calls `sudo` itself; do **not** run it
  as root).
- For the HDMI screen-mirror, a monitor on the **Pi's** HDMI, connected at boot.

---

## Quick start

```bash
cd pistorm-atari-jit
chmod +x install-full.sh
./install-full.sh
```

You'll be asked whether to build the emulator, install the auto-start service,
and create a Samba share. Then:

```bash
sudo reboot
```

### Non-interactive install

Answer the optional prompts up front with environment variables:

```bash
BUILD=1 SERVICE=1 SAMBA=0 PISTORM_CFG=games.cfg ./install-full.sh
```

| Variable       | Effect                                              | Default |
|----------------|-----------------------------------------------------|---------|
| `BUILD=1`      | Run `make` (auto-detects Pi model)                  | prompt  |
| `SERVICE=1`    | Install the systemd auto-start unit                 | prompt  |
| `SAMBA=1`      | Install + configure the Samba share                 | prompt  |
| `PISTORM_CFG=` | Config the auto-start service launches              | `master.cfg` |

### Other commands

```bash
./install-full.sh --help        # usage
./install-full.sh --uninstall   # reverse all system changes
```

---

## What it does

### 1. Dependencies

Installs the build **and** runtime libraries (the `-dev` packages pull in the
runtime libs, so a prebuilt binary works too):

```
build-essential  g++  make  pkg-config
libsdl2-dev  libdrm-dev  libslirp-dev  libasound2-dev  zlib1g-dev
```

`samba` is only installed if you choose the share option.

### 2. Runtime file tree

Created next to the repo (won't overwrite anything that already exists):

```
<parent>/
├── roms/            EmuTOS is installed here; add your own TOS ROM here too
├── configs/         atari.cfg, master.cfg (your own .cfg files go here)
├── dkimages/
│   └── fdd/         720k.st blank floppy; put disk/game images here
├── atari-share/     scratch folder (handy target for the Samba share)
└── screendumps/     screenshots
```

### 3. Boot firmware — **merged, not overwritten**

`config.txt` and `cmdline.txt` are backed up **once** to `*-bak.txt`, then:

- The PiStorm block is **appended once** to `config.txt` (guarded — re-runs skip
  it), so your existing settings are preserved.
- The CPU-isolation kernel args (`isolcpus=2,3 …`) are **appended to the single
  `cmdline.txt` line**, only if not already present.

Nothing you had in those files is discarded.

---

## Optional components

### Build the emulator

If you choose to build, the script detects your Pi model and runs
`make PIMODEL=PI4|PI3|PI02W`. You can always build later by hand:

```bash
make -C pistorm-atari-jit          # add PIMODEL=PI3 etc. if needed
```

### Auto-start on boot (systemd)

Installs `/etc/systemd/system/pistorm.service`, which launches the emulator on
**tty1 as root** (the GPIO/DMA bus needs root; KMSDRM/`native_hdmi` needs the
console) and takes tty1 from the login prompt, appliance-style. It restarts on
failure. Change the config it runs by editing that unit file, or set
`PISTORM_CFG=` at install time.

```bash
sudo systemctl start pistorm     # test now
journalctl -u pistorm -f         # watch its output
```

### Samba share

Adds a guest-writable `[pistorm]` share pointing at the runtime tree so you can
drop games/images onto the Pi from another machine. **This is a home-LAN
convenience** — lock it down (real users / `smbpasswd`) if the Pi is on an
untrusted network.

---

## Add your own ROM and games

The installer ships **EmuTOS** (GPL, freely distributable) as the default ROM.
To use a real Atari TOS instead, drop it in and point your `.cfg` at it:

```
<parent>/roms/tos206uk.rom        # your own copy — NOT included/redistributable
<parent>/dkimages/…               # your own disk/game images
```

Then in your config (e.g. `configs/master.cfg`):

```
rom ../roms/tos206uk.rom
hdd ../dkimages/yourdisk.img
fdd ../dkimages/fdd/yourfloppy.st
```

---

## Display notes (`native_hdmi`)

- The Pi-side HDMI mirror needs `native_hdmi enabled` in your `.cfg`. It renders
  from the console via KMSDRM, so it must run from a **text console, not a
  desktop** (the auto-start service already handles this).
- Mirror smoothness is set by the `fps` config value (clamped **10–60**,
  default **25**). If the mouse looks sluggish on the HDMI copy, raise it:
  ```
  fps 60
  ```
  Higher `fps` costs more upload bandwidth; drop to `fps 50` if you see
  `[DISPLAY] render overrun` in the log. The real Atari monitor is native and
  unaffected.

---

## Uninstalling

```bash
./install-full.sh --uninstall
sudo reboot
```

This:

- stops, disables and removes `pistorm.service`, and restores the tty1 login;
- removes just the `[pistorm]` stanza from `smb.conf` and restarts Samba;
- restores `config.txt` and `cmdline.txt` from the `*-bak.txt` backups.

Your `roms/`, `dkimages/`, `configs/` and the built `emulator` are **left in
place**. For a full wipe, remove the runtime tree yourself.

> **Note:** uninstall restores the boot files from the backups the *installer*
> made. If you hand-edited `config.txt`/`cmdline.txt` after installing, those
> edits are reverted — the safe, predictable behaviour for a clean back-out.

---

## Distributing to others

If you package this for other people:

- **Do** include EmuTOS (GPL).
- **Do not** bundle Atari TOS ROMs or any games/disk images — they're
  copyrighted. Ship the empty `roms/`/`dkimages/` folders and let users add
  their own.
