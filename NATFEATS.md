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

### VIDPLAY
Host video playback (MP4 / MKV / AVI / MOV / WebM / TS - anything libavformat
reads). The Pi demuxes and decodes in-process with libav*, puts the picture on
its **own DRM overlay plane above the Atari screen** in NV12/YUV420 (so the vc4
HVS does colour conversion and scaling for free, and the guest display is never
touched), and mixes the soundtrack into the same SDL3 device as ST/STE sound
and MP3. H.264 uses the Pi 4's hardware decoder. Sub-ops 0-7 are identical to
MP3PLAY on purpose.

| Sub-op | Name   | Arguments / result                                          |
|--------|--------|-------------------------------------------------------------|
| 0      | PLAY   | ptr to path string; 0 = OK, -1 = error                       |
| 1      | STOP   | stop, hide the overlay, free everything                      |
| 2      | STATUS | 1 while playing/paused, else 0                               |
| 3      | PAUSE  | param0: 1 = pause, 0 = resume                                |
| 4      | SEEK   | param0: signed seconds relative to current position          |
| 5      | POS    | current position in seconds (-1 if n/a)                      |
| 6      | LEN    | duration in seconds (0 if unknown)                           |
| 7      | META   | param0: 0=title 1=author 2=codecs; param1: buf; param2: len  |
| 8      | RECT   | param0..3 = x,y,w,h in display pixels; all 0 = auto letterbox; negative w/h = hide the picture, keep the sound |
| 9      | VOLUME | param0: 0..200 percent                                       |
| 10     | INFO   | param0: 0=w 1=h 2=fps*100 3=audio 4=hwdec 5=volume 6=display width 7=display height 8=hidden |

- Paths take the same forms MP3PLAY accepts (`S:\FILM\X.MKV` or `/s/film/x.mkv`)
  and must be on a **HOSTFS drive** - the host has to open the real file.
- Implementation: `platforms/atari/video/vidplay.c` (demux/decode/sync) and
  `vidplane.c` (the second DRM plane). Build deps: `libavformat-dev
  libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`.
- Decode runs on two threads that demote themselves to SCHED_OTHER off CPU 2.
  Still no external processes - same rule as MP3, same reason.
- Front-ends: `VIDPLAY.TTP` (source in `cdev/vidplay/`) and `VIDGEM.PRG`
  (source in `cdev/vidgem/`), a GEM app that maps the picture onto its own
  window - RECT + INFO 6/7 exist so it can scale Atari screen coordinates to
  real display pixels.
- Full documentation, including tuning and limits: **VIDEO.md**.

### PSCTRL
Read-only PiStorm status (phase 1 of `PSCTRL-DESIGN.md` / `PSMON-DESIGN.md`).
A host-side sampler thread (started lazily on first use) snapshots JIT and
host statistics every 500 ms; guest reads are O(1) and side-effect free.

| Sub-op | Name    | Arguments / result                                  |
|--------|---------|-----------------------------------------------------|
| 0      | VERSION | returns PSCTRL API version                          |
| 1      | GETINT  | param0 = index; returns value, -1 = unknown index   |

GETINT index namespace (full list in `platforms/atari/psctrl/psctrl.h`):
0-31 configuration (JIT enabled, cache size KB, CPU/FPU model, TT-RAM);
32-63 sampled guest/JIT statistics (epoch, cache used/total bytes, blocks
compiled, hard flushes, `execute_normal()` calls and STOP-state iterations
per 500 ms window); 64+ host statistics (SoC temperature in millidegrees C,
ARM clock kHz, 1-minute load average x100, uptime seconds).

Strictly read-only: no JIT state is mutated from the handler (see the JIT
invariant note above `atari_natfeat_handle_opcode`). Test tool:
`PSCHK.TTP` (source in `cdev/psctrl/`).

## Audio architecture (context for MP3PLAY and VIDPLAY)

ST/STE DMA sound is captured by register snooping (`dmasnd_capture.c`) and
played through SDL3 (`dmasnd_hdmi.c`): one SDL audio device opened plainly,
with one stream for STE sound (S8 at the live STE rate), one for MP3 (S16 at
the track's native rate) and, while a film is playing, one for the video
soundtrack (S16 at its native rate). SDL3 performs all resampling and mixing. The display
does not use SDL (DRM/KMS direct); only the audio subsystem is initialised.

## Environment variables

| Variable                       | Effect                                          |
|--------------------------------|--------------------------------------------------|
| `PISTORM_NET`, `PISTORM_NET_*` | Enable/configure ETHERNET backend               |
| `PISTORM_NET_IRQ_LEVEL`        | Interrupt level for network RX                  |
| `PISTORM_HOSTFS_DEBUG=1`       | Trace HOSTFS calls                              |
| `PISTORM_HOSTFS_DEDUP=0`       | Disable HOSTFS node dedup (debug escape hatch)  |
| `PISTORM_FVDI_OFFSCREEN_PIXELS`| fVDI offscreen buffer size                      |
| `PISTORM_VID_HWDEC=0`          | Force software video decode                     |
| `PISTORM_VID_THREADS`          | Software video decoder threads (default 3)      |
| `PISTORM_VID_CPUS`             | Hex affinity mask for the video threads         |
| `PISTORM_VID_DEBUG=1`          | Per-second video decode/present statistics      |
