# PiSTorm Atari JIT — Installer (`install-full.sh`)

`install-full.sh` sets up a Raspberry Pi to run the PiSTorm Atari JIT emulator.
It installs dependencies, lays out the runtime file tree, safely merges the
required boot-firmware settings, and can optionally build the emulator, make it
auto-start on boot, and share its folders over the network.

The script is **idempotent** — running it twice does no harm — and ships with a
clean **`--uninstall`** that reverses every system change.

---

## Requirements

- **64-bit OS (aarch64).** The JIT backend is AArch64-only — a 32-bit OS can
  neither build nor run it. **The installer aborts if the OS isn't 64-bit.**
- **Raspberry Pi 4** recommended (Pi 3 and Pi Zero 2 W are currently not supported).
- **Raspberry Pi OS Lite (64-bit)** — a fresh install is fine and recommended.
- **No desktop environment.** PiSTorm JIT can't run with a desktop: the
  compositor holds the display/DRM master (so `native_hdmi`/KMSDRM can't get the
  console) and steals the isolated CPU cores. Use **Lite**, or let the installer
  switch you to console boot.
- **PiSTorm board fitted to a real Atari ST/STe.**
- Run as your **normal user** (the script calls `sudo` itself; do **not** run it
  as root).
- For the HDMI screen-mirror, a monitor on the **Pi's** HDMI, must be connected at boot.

### Pre-flight checks

Before touching anything, the installer verifies:

| Check              | If it fails                                                       |
|--------------------|------------------------------------------------------------------|
| 64-bit (`aarch64`) | **Aborts** — re-install with 64-bit Raspberry Pi OS.              |
| Pi model           | Warns on anything that is not a **Pi 4 / Pi 400 / CM4** — those are the only boards the JIT is validated on. It is a warning, not an abort. |
| RAM                | Warns if low, suggesting `make HEAVY_OPT=-O0` / swap for building. |
| Desktop present    | Offers to disable it (console boot); **aborts** if you decline (`KILLGUI=1` to auto-disable). |

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
| `KILLGUI=1`    | Disable the desktop and switch to console boot      | prompt (declining **aborts**) |
| `PISTORM_CFG=` | Config the auto-start service launches              | `master.cfg` |

Any of these set to `1`/`y`/`yes` means yes; anything else means no. Unset means
prompt on a TTY, or take the default when there is no TTY.

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
build-essential  g++  make  pkg-config  cmake  git
libsdl3-dev      audio backend (see note below)
libmpg123-dev    host MP3 decode — the MP3PLAY NatFeat
libavformat-dev  libavcodec-dev  libavutil-dev
libswscale-dev   libswresample-dev
                 host video decode — the VIDPLAY NatFeat. The Makefile
                 refuses to build without these
libdrm-dev       KMS/DRM display and the video overlay plane
libjpeg-dev      MJPEG frames in avrecord.c (screen capture)
zlib1g-dev       PNG screendumps, written in-process
libslirp-dev     user-mode networking (optional at build time)
libasound2-dev   ALSA backend, needed only if SDL3 is built from source
ffmpeg           the command-line tool, used only by capmux.sh to mux a
                 finished capture
cifs-utils       mounting a share from a NAS or PC — see below
```

`libsdl3-dev` comes from apt on trixie. On an older image the package does not
exist, so the installer builds SDL3 from source once (that is what `cmake` and
`libasound2-dev` are for).

`samba` and `samba-common-bin` are installed **only** if you choose the share
option.

> Nothing in the emulator shells out to `ffmpeg`. Video decode is in-process
> through `libav*`; screendumps are PNGs written directly. The `ffmpeg` binary
> is there purely for `capmux.sh`.

For **hardware HEVC** the distro `libav*` is not enough — see `VIDEO.md`.

### 2. Runtime file tree

Created next to the repo (won't overwrite anything that already exists):

```
<parent>/
├── roms/            EmuTOS is installed here; add your own TOS ROM here too
├── configs/         atari.cfg, master.cfg (your own .cfg files go here)
├── dkimages/
│   └── fdd/         720k.st blank floppy; put disk/game images here
├── atari-share/     point a HOSTFS drive here. The GEM programs
│                    (MP3GEM.PRG, VIDGEM.PRG, VIDPLAY.TTP) are installed
│                    into it, and it is the natural place to mount a
│                    media share from another machine — see below
└── screendumps/     screenshots
```

### 3. Boot firmware — **merged, not overwritten**

`config.txt` and `cmdline.txt` are backed up **once** to `*-bak.txt`, then:

- The PiStorm block is **appended once** to `config.txt` (guarded — re-runs skip
  it), so your existing settings are preserved.
- The CPU-isolation kernel args (`isolcpus=2,3 …`) are **appended to the single
  `cmdline.txt` line**, only if not already present.

Nothing you had in those files is discarded.

> **`gpu_mem=128` is a hard requirement on the Pi 4** (already set in the
> shipped `configs/config.txt`). The VideoCore H.264 decoder (VIDPLAY film
> playback) and encoder (screen recording) allocate their frame buffers from
> `gpu_mem`. With less (e.g. 32 MB) the decoder opens but never produces a
> frame: playback hangs silently, and killing it wedges the VPU decoder
> (`failed to create component ril.video_decode` in dmesg) until the next
> reboot. If films suddenly play green or not at all after "tuning" memory
> settings, check this first.

---

## Optional components

### Build the emulator

If you choose to build, the script detects your Pi model and runs
`make PIMODEL=…` — `PI4` for a Pi 4 / Pi 400 / CM4, `PI3` for a Pi 3, `PI02W`
for a Zero 2 W, and `PI4` for anything it does not recognise. Note that the
`PI3`/`PI02W` cases exist in the script but those boards are **not currently
supported**; the pre-flight check will already have warned you.

You can always build later by hand:

```bash
make -C pistorm-atari-jit          # PIMODEL is not actually needed for a Pi 4
```

The build stops early with a list of any missing `-dev` packages rather than
burying you in compiler errors.

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

## Mounting a share from another PC or NAS (for HOSTFS)

The Samba step above is the **server** side: it pushes the Pi's files out to
your desktop. This is the opposite direction — the Pi mounts a share **from**
another machine, so a library living on your PC or NAS shows up inside a HOSTFS
drive on the Atari.

That matters most for video. A handful of films is tens of gigabytes; nobody
wants a second copy of that on an SD card, and copying each one across before
watching it defeats the point. `cifs-utils` is installed for you.

### Where to mount it

A HOSTFS drive points at a real directory, so mount the remote share **inside**
`atari-share` as a subdirectory:

```
<parent>/atari-share/
├── VIDGEM.PRG          installed for you
├── VIDPLAY.TTP
└── media/              <- the mount point; the NAS appears here
```

The Atari then sees `MEDIA` as a folder on that drive, and nothing else needs
configuring.

**Mount the directory itself — do not symlink to a mount elsewhere.** HOSTFS
resolves paths on the guest's behalf, and a link pointing outside the drive's
root is exactly the sort of thing it is entitled to refuse.

### Setting it up

**1. Credentials, kept out of `/etc/fstab`.** fstab is world-readable; this
file must not be:

```
sudo install -m600 /dev/null /etc/samba/nas-credentials
sudo nano /etc/samba/nas-credentials
```

```
username=yourname
password=yourpassword
domain=WORKGROUP
```

**2. The mount point:**

```
mkdir -p ~/atari-share/media          # adjust to your <parent> path
```

**3. One line in `/etc/fstab`** — all on a single line:

```
//192.168.1.10/Media  /home/pi/atari-share/media  cifs  credentials=/etc/samba/nas-credentials,uid=1000,gid=1000,iocharset=utf8,file_mode=0664,dir_mode=0775,vers=3.0,nofail,_netdev,x-systemd.automount,x-systemd.idle-timeout=600  0  0
```

**4. Apply it:**

```
sudo systemctl daemon-reload
sudo mount -a
ls ~/atari-share/media
```

### What those options are actually for

| Option | Why it is there |
|---|---|
| `credentials=` | Keeps the password out of the world-readable fstab |
| `uid=` / `gid=` | CIFS has no shared user database, so ownership is decided at mount time. Use your own IDs (`id -u`, `id -g`) so files the Atari writes come back owned by you |
| `nofail` | **Do not skip this.** Without it, a NAS that is switched off stops the Pi booting — a miserable thing to debug on a headless machine |
| `_netdev` | Wait for the network before trying |
| `x-systemd.automount` | Mount on first access rather than at boot. If the NAS is off you get an empty directory instead of a hang |
| `x-systemd.idle-timeout` | Unmount after ten idle minutes, so a NAS that spins down does not leave a stale handle behind |
| `vers=3.0` | Modern SMB. Some elderly NAS boxes only speak `vers=1.0`; it works, but SMB1 is long deprecated and worth avoiding |
| `iocharset=utf8` | Sane handling of accented filenames |

### Checking it before blaming the emulator

```
findmnt ~/atari-share/media           # is it actually mounted?
dd if=~/atari-share/media/somefilm.mkv of=/dev/null bs=1M count=500
```

The second one is worth doing before concluding that video playback is broken.
It reports a throughput figure; compare that against the file's bitrate, which
`ffprobe` will tell you. 4K HEVC runs at roughly 50–80 Mbit/s — comfortable
over the Pi 4's gigabit ethernet, marginal over Wi-Fi. Playback that stutters
at a *steady* rate is usually decode; playback that is fine and then hitches in
bursts is usually the network.

### Filenames

HOSTFS passes names through to the guest, so what survives depends on the
Atari's filesystem layer. Under MiNT, long names are fine. Under plain TOS you
get 8.3, and a film called `Some.Very.Long.Release.Name.2160p.HDR.mkv` will not
make the trip intact — rename it, or keep those files in a subdirectory you
browse from a MiNT-aware program.

### NFS instead

If the far end is Linux, NFS skips the credentials file entirely:

```
sudo apt install nfs-common
```

```
192.168.1.10:/export/media  /home/pi/atari-share/media  nfs  nofail,_netdev,x-systemd.automount  0  0
```

---

## Add your own ROM and games

The installer ships **EmuTOS** (GPL, freely distributable) as the default ROM.
To use a real Atari TOS instead, drop it in and point your `.cfg` at it:

```
<parent>/roms/emutos-aranym.rom   # your own copy — NOT included/redistributable
<parent>/dkimages/…               # your own disk/game images
```

Then in your config (e.g. `configs/master.cfg`):

```
rom ../roms/emutos-aranym.rom
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
