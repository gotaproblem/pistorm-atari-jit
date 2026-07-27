# Native Features (NatFeats)

The emulator implements the ARAnyM-compatible Native Features interface, letting
guest software (TOS/FreeMiNT programs and drivers) call host-side services
directly. This is how the Atari side reaches the Pi for file sharing,
networking, accelerated graphics, and host audio playback.

Implementation: `platforms/atari/network/atari_natfeat.cpp`.

## Calling convention

Two reserved opcodes are intercepted by the CPU core:

| Opcode   | Function   | Stack                                     |
|----------|------------|-------------------------------------------|
| `0x7300` | NF_GET_ID  | `4(sp)` = pointer to feature name string   |
| `0x7301` | NF_CALL    | `4(sp)` = feature ID `\|` sub-op, args after |

`NF_GET_ID("NAME")` returns a feature base ID in `d0` (0 = not present).
`NF_CALL(id | subop, ...)` invokes the operation; result in `d0`. On real
hardware both opcodes are harmless, so probing is safe everywhere.

Feature IDs are `(index+1) << 20`; the low 20 bits carry the sub-operation.

## Implemented features

### NF_NAME
Returns the emulator name into a caller-supplied buffer. Standard ARAnyM probe
used by drivers to identify the host.

### NF_VERSION
Returns the NatFeat API version.

### NF_STDERR
Writes a guest string to the emulator console (prefixed `[NF_STDERR]`).
FreeMiNT and fVDI use this for boot/debug messages.

### ETHERNET
ARAnyM-compatible backend for FreeMiNT's `nfeth.xif` network driver:
`GET_VERSION`, `XIF_INTLEVEL`, `XIF_IRQ`, `XIF_START/STOP`,
`XIF_READLENGTH/READBLOCK/WRITEBLOCK`, `XIF_GET_MAC/IPHOST/IPATARI/NETMASK`.
Backends: slirp (user-mode NAT) or TAP. Enabled/configured via the emulator
config and `PISTORM_NET*` environment variables. Details in
`platforms/atari/network/README.md`.

### HOSTFS
ARAnyM-compatible host file system, used with FreeMiNT's `hostfs.xfs` driver.
Exposes directories on the Pi as GEMDOS drive letters (e.g.
`hostfs S /home/pistorm/atari-share` in the config → `S:` on the Atari, also
reachable as `/s/` or `u:\s\` under MiNT). Implements the full XFS/DEV protocol:
lookup, getxattr/stat64, open/read/lseek/ioctl/datime, opendir/readdir,
pathconf, dfree, readlink, dupcookie/release, dskchng — file *writes* are
currently refused (`EROFS`), so the share is effectively read-only from the
Atari side.

Notes:
- Requires FreeMiNT with NatFeats enabled and `hostfs.xfs` installed; plain TOS
  cannot see HOSTFS drives.
- Host Unix permissions are honoured (a file needs `x` on the Pi to be
  `Pexec`'d from the share).
- Cookie nodes are deduplicated by path so the node table is bounded by
  distinct paths, not operation count. `PISTORM_HOSTFS_DEDUP=0` restores the
  old (leaky) per-lookup allocation as an escape hatch.
- `PISTORM_HOSTFS_DEBUG=1` traces every HOSTFS call on the console.

### fVDI
Host-accelerated graphics backend for the fVDI driver (`aranym.sys`), ARAnyM
`fVDI` NFAPI (version `0x14000960`). Blits, fills, lines, mono expansion,
mouse, palette and resolution handling run host-side into the framebuffer the
DRM/ET4000 display path presents. 8/16/32-bit modes; the 16bpp path is
NEON-optimised. `PISTORM_FVDI_OFFSCREEN_PIXELS` tunes offscreen buffer size.

### MP3PLAY
Host MP3 playback: the Pi decodes the file and mixes it into the HDMI audio
alongside ST/STE DMA sound, taking the whole decode load off the 68k.

| Sub-op | Name   | Arguments / result                                         |
|--------|--------|------------------------------------------------------------| 
| 0      | PLAY   | ptr to path string; 0 = OK, -1 = error                     |
| 1      | STOP   | stops playback                                             |
| 2      | STATUS | returns 1 while playing/buffered, else 0                   |
| 3      | P.   AUSE  | param0: 1 = pause, 0 = resume                          |
| 4      | SEEK   | param0: signed seconds relative to current position        |
| 5      | POS    | returns current position in seconds (-1 if n/a)            |
| 6      | LEN    | returns track length in seconds (0 if unknown)             |
| 7      | META   | param0: 0=title 1=artist 2=album; param1: buf; param2: len |

- Paths are accepted in GEMDOS form (`S:\MUSIC\SONG.MP3`) or MiNT unix form
  (`/s/music/song.mp3`) and must point at a **HOSTFS drive** — the letter is
  mapped to the share's host directory. Files on IDE/floppy images cannot be
  played (they live inside disk images the host cannot open).
- Decoding is in-process via **libmpg123** (build dependency:
  `libmpg123-dev`); output is a second SDL3 audio stream bound to the same
  device as the STE sound stream, so SDL3 mixes both. No external processes.
- Front-ends:
  - `MP3PLAY.TTP` (source in `cdev/mp3play/`): command line / drag & drop.
    `MP3PLAY S:\MUSIC\SONG.MP3`, `MP3PLAY STOP`, `MP3PLAY STATUS`. Quoted
    paths and spaces in filenames are handled.
  - `MP3GEM.PRG` (source in `cdev/mp3gem/`): GEM player with playlist
    (directory of the opened file), play/pause/stop, prev/next, rewind/FF
    (+/-10 s), scrolling ID3 metadata and position readout, auto-advance at
    track end. Keys: space = pause, N/P = next/prev, Q = quit. Needs AES
    (FreeMiNT + XaAES); playback continues after quitting.

## Audio architecture (context for MP3PLAY)

ST/STE DMA sound is captured by register snooping (`dmasnd_capture.c`) and
played through SDL3 (`dmasnd_hdmi.c`): one SDL audio device opened plainly,
with one stream for STE sound (S8 at the live STE rate) and one for MP3 (S16 at
the track's native rate). SDL3 performs all resampling and mixing. The display
does not use SDL (DRM/KMS direct); only the audio subsystem is initialised.

## MP4 / other formats — planned

Not yet implemented. The intended path follows the MP3 design:

- **MP4/AAC audio**: swap in an AAC-capable in-process decoder (libmpg123 is
  MP3-only); same NatFeat API and SDL3 mixing. External-process decoders
  (ffmpeg) were tried and rejected: children spawned from the emulator's
  real-time, core-pinned CPU thread inherit its affinity/scheduling and starve.
- **MP4 video**: host decode + scale into the ET4000 framebuffer via the
  existing DRM present path, guest controlling via PLAY/STOP/SEEK sub-ops.

## Environment variables

| Variable                       | Effect                                          |
|--------------------------------|--------------------------------------------------|
| `PISTORM_NET`, `PISTORM_NET_*` | Enable/configure ETHERNET backend               |
| `PISTORM_NET_IRQ_LEVEL`        | Interrupt level for network RX                  |
| `PISTORM_HOSTFS_DEBUG=1`       | Trace HOSTFS calls                              |
| `PISTORM_HOSTFS_DEDUP=0`       | Disable HOSTFS node dedup (debug escape hatch)  |
| `PISTORM_FVDI_OFFSCREEN_PIXELS`| fVDI offscreen buffer size                      |
