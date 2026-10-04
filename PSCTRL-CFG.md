# psctrl.cfg - the configuration file, key by key

The PiSTorm has one configuration file, `configs/psctrl.cfg`, next to the
emulator's directory (`../configs/psctrl.cfg` from where the binary
runs). `install-full.sh` puts it there from `configs/psctrl.cfg.default`
and never overwrites yours. The pre-boot setup page reads and writes it,
and so does `PSCTRL.ACC` inside APJ-OS; every key below is also a tick box
or a field on the page, so you need not edit the file by hand - but it
is plain text, and this is what is in it.

`PSCTRL-DESIGN.md` describes the page itself. `master.cfg`, `atari.cfg`,
`apj-os.cfg` and `autoboot.cfg` were the old one-machine files; the keys
are the same, and such a file still loads with `--config file.cfg` (no
page). The annotated `master.cfg` is retired: this document replaces it.

## Shape of the file

```
[psctrl]              the page's own block - never a machine
countdown 5
boot apj-os
rom_path ../roms
...

[gem]                 a build: one complete machine
cpu 68030
rom emutos-aranym.rom
...

[apj-os]              another build
cpu 68040
...
```

* A `[section]` starts a build; everything up to the next `[` belongs to
  it. The emulator loads **one** build - the one the page picked, or the
  `boot` key, or the first section when there is neither. Nothing is
  inherited between builds: each one says everything it needs.
* A line is `key value`. Keys are case-insensitive. `#` starts a comment;
  a blank line is ignored. Order does not matter, except that repeated
  keys (`hdd`, `acsi`, `fdd`, `hostfs`) are taken in file order.
* A key that is absent is off, or at its default, as given below. The
  page writes only the keys it has ticked.
* Booleans: `key` on its own is on. So are `1`, `on`, `yes`, `true`,
  `enabled`, `enable`. Off is `0`, `off`, `no`, `false`, `disabled`,
  `disable`.
* Sizes: a plain number is KB; `M` and `G` scale it (`ttram 128M`,
  `jit_cache 16384`, `stram_size 2560` = 2.5 MB).
* File names: a bare name is looked for under the `[psctrl]` path for its
  kind (`rom_path`, `disk_path`, `fdd_path`). A name starting with `/`,
  `~` or `.` is used as it is, relative to the emulator's directory; `~`
  is the home of the user who ran `run-pistorm.sh`, not root's.
* A build whose name starts with `apj` gets the APJ-OS-only keys on the
  page (`stbox_*`); the others get the GEM-only ones (`monitor`,
  `shifter`, `cpu_compatible`). The emulator accepts any key in any build.
* Up to 8 builds are listed.

## `[psctrl]` - the page

| key | values | |
|---|---|---|
| `countdown` | seconds, default 5 | how long the page waits before booting `boot`. `0` boots at once with no page (so does `--no-setup`) |
| `boot` | a section name | the build booted last, and the one the countdown boots. Written by the page |
| `rom_path` | directory | where bare `rom` and `stbox_tos` names are looked for |
| `disk_path` | directory | where bare `hdd` and `acsi` images are looked for |
| `fdd_path` | directory | where bare `fdd` images are looked for |
| `clock` | `24` or `12` | the page's title-bar clock; `C` on the builds screen switches it |
| `date` | `dmy` or `mdy` | its date format; `M` switches it |

The three paths are relative to the emulator's directory (`../roms`,
`../dkimages`, `../dkimages/fdd` as installed) and are what the page's
image picker lists.

## Machine

| key | values | |
|---|---|---|
| `cpu` | `68000` `68010` `68020` `68030` `68040` `68060` | the CPU the guest sees. 68000/010 are 24-bit and cannot have TT-RAM or the JIT; 68030 is the Falcon/TT baseline; 68040 is the fastest with the JIT. No key: a 68000 |
| `fpu` | boolean | an FPU, where the CPU can have one (68020 and up) |
| `jit` | boolean, default on | the JIT compiler. Off means the interpreter: several times slower, but exact |
| `jit_cache` | size, default 8192 (KB) | the translation cache. 16384 for big applications (APJ-OS ships with it) |
| `mmu` | boolean | full MMU emulation, model from the `cpu` line (68030/040/060). Needed by guests that program real translation tables: Basilisk II, MagiCMac, MiNT memory protection. Interpreter only - forces the JIT off, expect half interpreter speed. A 68000/010/020 has no MMU and the key is ignored with a warning. Env: `PISTORM_MMU=1/0` overrides, `PISTORM_MMU_DEBUG=1` traces |
| `cpu_compatible` | boolean | the prefetch-accurate 68000 core, whose bus-error frames carry the exact faulting PC and access address. For software that virtualises hardware through bus-error traps (Spectre GCR's Mac world-switch). Only honoured with `cpu 68000` and the JIT off. Env: `PISTORM_CPU_COMPAT=1` |
| `machine` | `st` `ste` `megast` (also `mst`) `falcon` | what the guest is told it is (the `_MCH` cookie, patched in the jar by the host) and which hardware the emulator models around it: STE implies the STE shifter personality (below), Mega ST implies a blitter fitted. `falcon` is the exception: the Falcon030's `_MCH` on the STE personality, for software that refuses to run without a Falcon (with `psvidel` and `falcon_dsp` supplying the Falcon's video and DSP). The OS and every program then believe they are on a Falcon, which is an experiment per build rather than a default. No key: the emulator's ST default |
| `ttram` | boolean or size, default 128M when on | TT-RAM (Alt-RAM, FastRAM) at `$01000000`, up to 128 MB, in the Pi's memory - the fast RAM. Needs a 32-bit CPU (68020 and up). MiNT and the JIT both want it; PSVIDEL's video RAM needs it or `addr32` |
| `addr32` | boolean | a 32-bit address bus without TT-RAM. Rarely needed on its own |
| `stram_size` | `512K` `1M` `2M` `2560` `4M` | the ST-RAM the guest sees. When set, the emulator probes the board's real DRAM and configures the largest real ST setup fitting both - `4M` on a 1 MB board gives 1 MB (a 4 MB model on a small board puts the screen outside physical DRAM and scrambles the native display), `1M` on a 4 MB board deliberately presents a 1 MB machine. No key: the flat 4 MB model with no probing. MiNT gets its TT-RAM either way |
| `falcon_stram` | `14M` | `machine falcon` only: a 14 MB Falcon. `$400000`-`$DFFFFF` becomes ST-RAM backed by Pi memory (Mxalloc mode 0 finds it). Falcon TOS 4.0x sizes it from `$FF8006` and puts its screen at the top, so the display must be PSVIDEL/fVDI on HDMI - the real Shifter, and real FDC/ACSI DMA on the board, only reach the first 4 MB. Ignored with `vga` (the ET4000 windows are at `$A00000`-`$DFFFFF`) or `stram_size`. EmuTOS (ST builds) still sizes 4 MB. For programs built for 14 MB Falcons (BadMood 2.01 `base030.14m`) |
| `stram_cache` | boolean | keep a copy of ST-RAM in the Pi and satisfy the CPU's reads from it, writing through to the board only what the real hardware must see (low memory, the screen). Much faster. Off unless given |
| `stram_direct` | boolean | let the JIT read and write the ST-RAM copy inline. Faster again. Experimental: stupid fast for some software, breaks some |
| `cpu_clock_multiplier` | number | UAE's per-instruction cycle scaling. Interpreter only; with the JIT on it has no effect (`jit_glue` warns and forces `m68k_speed 0`) |
| `m68k_speed` | `-1` or `0` | UAE's CPU pacing: `-1` (default) as fast as possible, `0` paced against UAE's cycle counter. Interpreter only |

## Video

| key | values | |
|---|---|---|
| `vga` | `ET4000AX NOVA` `ET4000AX XVDI` `ET4000AX NVDI` `ET4000AX FVDI` | an ET4000AX graphics card on HDMI, and which Atari driver it is built for. `FVDI` is APJ-OS's. No key: no card, the ST screen is the display |
| `native_hdmi` | boolean | mirror the ST screen (low, medium, high) to HDMI, taken at each real VBL. Best-effort: beam-raced effects only look right on the real shifter output. Off: games and demos use the shifter, HDMI shows the logo |
| `hdmi_only` | boolean, default off | the HDMI mirror is the only display: screen writes and the shadow frame buffer copy on a screen flip stay off the bus, so a game that flips screens no longer stalls the 68k (late VBL / Timer B, palettes landing mid-frame). The ST's own RGB/mono output is then not kept up to date. Needs `native_hdmi` |
| `monitor` | `mono` (`sm124`, `high`) `colour` (`color`, `rgb`, `sc1224`) `auto` | what TOS is told is plugged in (MFP GPIP7). `mono` lets software that needs 640x400 run on a colour setup - Spectre GCR - with the real monitor then showing garbage; watch `native_hdmi` instead. Default `auto`: the real wire |
| `shifter` | `st` `ste` | whether the HDMI mirror honours the STE video registers (`$FF820D`, `$FF820F`, `$FF8265`). Set by `machine ste` unless given here. A plain ST ignores them like the real chip |
| `psvidel` | boolean | the Falcon's Videl and the SuperVidel on HDMI - Falcon and SV video modes, 112 MB of video RAM at `$A1000000`, the SuperBlitter, the Falcon video XBIOS. Armed by `PSVIDEL.PRG` in AUTO. Needs `ttram` or `addr32` for the video RAM. See `PSVIDEL.md` |
| `stbox_tos` | ROM image | TOS for STBOX, the sandboxed 68000 ST in a GEM window (APJ-OS): TOS 1.04/2.06 or a 192/256K EmuTOS. The Aranym EmuTOS of the main machine will not boot the box. The whole rest of the line is the path (spaces allowed). Env: `PISTORM_STBOX_TOS` |
| `stbox_machine` | `st` `ste` | the box's default machine; STBOX.PRG's own `st`/`ste` argument overrides per launch. An STE box wants an STE-aware TOS |
| `stbox_plane` | number | force a DRM overlay plane for the box's video; 0 or absent is auto. Env: `PISTORM_STBOX_PLANE` |

## Sound

All HDMI sound shares one audio device; any one of these brings it up.

| key | values | |
|---|---|---|
| `ym2149` | boolean | the PSG on HDMI: shadows the register writes the real chip still gets, so chip music, key clicks and effects play on HDMI too. Any machine |
| `dma_sound` | boolean | the STE DMA sound on HDMI. STE machines. Tuned for MOD and MP3 playback; system sounds may lag |
| `falcon_dsp` | boolean | the Falcon's DSP56001, host port, SSI, sound matrix, 16-bit sound DMA and CODEC, sound on HDMI. Armed by the same `PSVIDEL.PRG`. See `platforms/atari/falcon/README.md`. Env: `PISTORM_DSP_TURBO`, `PISTORM_FALCON_VBLSYNC`, `PISTORM_FALCON_STATS` |

## Input

| key | values | |
|---|---|---|
| `kbd` | `usb` [`nograb`] [`merge` or `standalone`] [`mousediv N`] | a USB or Bluetooth keyboard and mouse on the Pi, injected into the IKBD stream; the real ST keyboard and mouse keep working. Default mode merges the real IKBD when it is healthy and quarantines it when absent or noisy; `merge` always trusts it, `standalone` ignores it. Devices are grabbed from the Pi console unless `nograb` (F12 toggles the grab at run time). `mousediv N` (1-16) slows a fast USB mouse. E.g. `kbd usb nograb mousediv 2`. `kbd disabled` turns it off. See `KBD_USB_README.md` |
| `usb` | `gamepad` [`off`] | USB or Bluetooth game controllers as the ST's joysticks and STE joypads: pad 0 is joystick 1 and pad A, pad 1 is joystick 0 and pad B. Joystick only, never the GEM pointer. Works in STBOX too. See `GAMEPAD_README.md` |

## Blitter

| key | values | |
|---|---|---|
| `blitter` | on, `real`, or off | on: the emulated ST BLiTTER, a software blit over the RAM copy, the real chip untouched - works with or without one fitted. `real`: pass the registers through to the real chip (needs bus arbitration in the CPLD). Off: hide `$FF8A00`, TOS uses software rendering and a program that touches the blitter bus-errors. No key: follows `machine` - off for `st`, on for `ste`, `megast` and `falcon`, the machines that shipped with one |

## ROM

| key | values | |
|---|---|---|
| `rom` | image file | the TOS. 256K and larger images load at `$E00000` (TOS 2/3/4, EmuTOS 256K and up), 192K images at `$FC0000` (TOS 1.x, EmuTOS 192K). `emutos-aranym.rom` is installed under `rom_path` |

## Drives

| key | values | |
|---|---|---|
| `ide` | boolean | the IDE interface for the `hdd` images |
| `hdd` | `image` or `N:image` | an IDE hard disk image, up to 8. `N:` (0-7) pins the slot; a bare name takes the next free one. The first should be the bootable image. The page writes the pinned form so removing a drive never shifts the others |
| `acsi` | `enabled` / `disabled`, then `image` or `N:image` lines | emulated ACSI targets on the native DMA port, for AHDI/ICD/PP drivers and Spectre's Mac partitions; they work beside real ACSI devices, whose IDs pass through. `N:` (0-7) pins the ID, `N image` too. `.img` is a raw Atari ACSI disk; `.hfs` is a bare Macintosh HFS volume the emulator wraps in an AHDI root, so Spectre mounts it and Basilisk II opens the same file. See `ACSI-DESIGN.md` |
| `fdd` | `image`, `A:image`, `B:image` (also `0:`/`1:`) | floppy drives on the native bus (a WD1772 model). `.st` and raw images are read-write with the geometry from the BPB or size (360K, 720K, 1.44M); `.msa` is detected by content and mounted read-only. `.stx` is not supported. F11 on the USB keyboard ejects and re-inserts a disk |
| `hostfs` | `drive path [ro]` | a directory on the Pi as a GEMDOS drive through the ARAnyM HOSTFS NatFeat (`hostfs.xfs` under MiNT). The drive is a letter `A`-`Z` or `0`-`5`; `ro`/`readonly` at the end makes it read-only. `hostfs S /home/pistorm/atari-share` is the installed share; the GEM tools live there |

## Network

FreeMiNT networking through the ARAnyM ETHERNET NatFeat (`nfeth.xif`).
The Pi side (TAP and NAT) is set up after boot by `netstart.sh`.

| key | values | |
|---|---|---|
| `network` | boolean | the interface |
| `network_backend` | `tap` | the backend |
| `network_tap` | interface name, `tap0` | the Pi's TAP device |
| `network_base` | address, `0x00F10000` | where the interface's registers appear to the guest |
| `network_irq` | 1-7, `4` | its interrupt level |
| `network_host_ip` | `192.168.50.1` | the Pi's address on the link - leave as installed |
| `network_atari_ip` | `192.168.50.2` | the Atari's - leave as installed |
| `network_netmask` | `255.255.255.0` | |
| `network_mac` | `xx:xx:xx:xx:xx:xx` | the guest's MAC, if the built-in one must change |
| `network_debug` | boolean | log the packets |

## Example builds

The installed `[gem]` and `[apj-os]` are in `configs/psctrl.cfg.default`.
A Falcon-game build for an ST with a PiSTorm, playing Beats of Rage from
an IDE image with a USB pad:

```
[falcon]
cpu 68040
fpu
ttram 128M
psvidel true
falcon_dsp true
ym2149
kbd usb
usb gamepad
rom emutos-aranym.rom
ide
hdd 0:games.img
acsi disabled
network disabled
```

with `PSVIDEL.PRG` in the image's AUTO folder. Beats of Rage does not
check the machine; a game that does (Sonic Falcon, dino) also needs
`machine falcon`, which brings the blitter with it.

## Environment variables

Read by the emulator at start-up, for switches that do not belong in a
build. The launcher passes the environment through; `sudo` may not, so
set them in `run-pistorm.sh` or with `sudo env VAR=... sh run-pistorm.sh`.

| | |
|---|---|
| `PISTORM_JIT=0` (or `touch /tmp/pistorm_jit_off`) | JIT off without editing the cfg |
| `PISTORM_CPU_DIAG=1` | exception and bus-error traces, PC history at a reset |
| `PISTORM_MMU=1/0`, `PISTORM_MMU_DEBUG=1` | see `mmu` |
| `PISTORM_CPU_COMPAT=1` | see `cpu_compatible` |
| `PISTORM_VID_FOLLOW=1` | HDMI follows the guest's refresh rate (50 Hz for a 50 Hz screen) |
| `PISTORM_DSP_TURBO=1..8`, `PISTORM_FALCON_VBLSYNC=0` | see `falcon_dsp` |
| `PISTORM_FALCON_STATS=1` | the `[FALCON] 68k:` timing line every 10 s while the DSP runs |
| `PISTORM_FVDI_BLIT_TRACE=1` | fVDI: log the first 400 raster copies (path taken, MFDB headers, screen notes) and what each frame converts |
| `PISTORM_FVDI_FULLRENDER=1` | fVDI: convert the whole frame every time instead of the dirty rect (A/B for a stale-screen report) |
| `PISTORM_FVDI8_PLANAR=1` | fVDI 8-bit: take a memory MFDB in device format as interleaved planes (stock aranym.sys) instead of chunky (the patched driver) |
| `PISTORM_DRM_DIRTYBAND=0` | HDMI: copy the whole frame to the DRM buffer every present instead of the dirty band (A/B) |
| `PISTORM_ET4000_DIRTY=0` | HDMI: render and present every frame even when nothing changed |
| `PISTORM_DMASND_GPIP7=0/1` | STE DMA sound: withhold / force the GPIP7 frame pulses (default: only to a handler in RAM) |
| `PISTORM_STBOX_TOS`, `PISTORM_STBOX_PLANE` | see `stbox_*` |
| `PISTORM_DUMP_ADDR=addr[,addr]` | dump guest memory at these addresses when the guest dies |

`PERF-TUNING.md` has the performance-related ones.

## Migrating an old file

An `atari.cfg`, `master.cfg` or `apj-os.cfg` is a build without a
section header. Paste its lines under a `[name]` header in `psctrl.cfg`,
drop the comments if you like, and shorten paths that point under the
`[psctrl]` directories (`rom ../roms/emutos-aranym.rom` becomes
`rom emutos-aranym.rom`). The page's `N` key does the same from a copy of
an existing build.
