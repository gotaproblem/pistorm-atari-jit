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
ARM clock kHz, 1-minute load average x100, uptime seconds); 78-84 the
desktop's status icons and PSMON's browser row: 78 `PS_HOST_NET` (bit0 a
link is up with an IPv4 address, bit1 it is wireless, bits 8-15 Wi-Fi link
quality 0-100), 79 `PS_HOST_IPV4` (the address as one long), 80
`PS_HOST_INPUT` (bit0 the USB/Bluetooth input bridge is on, bit1 a keyboard
is attached, bit2 a mouse, bit3 the real IKBD is present, bits 8-15 seconds
since the last forwarded event), 81 `PS_WEB_STATE` (0 no psweb, 1 socket,
2 connected, 3 a view is live), 82 `PS_WEB_FPS_X10`, 83 `PS_WEB_KBPS`
(frames and KB per second the guest fetched), 84 `PS_WEB_RSS_MB` (resident
memory of psweb and its WebKit processes). TeraDesk hands 78 and 80 to
XaAES every tick (`appl_control` opcode 122), which draws the network and
USB icons left of the menu-bar clock from `apjglyphs.bin`.

Strictly read-only: no JIT state is mutated from the handler (see the JIT
invariant note above `atari_natfeat_handle_opcode`). Test tool:
`PSCHK.TTP` (source in `cdev/psctrl/`).

### PSIMG
Host-side image decoding (PNG / JPG via the vendored stb libraries in
`third_party/stb/`). The guest names a file and a target geometry; the Pi
decodes, scales (stretch or aspect-fit with black letterboxing) and converts
to the fVDI pixel format (32bpp `00 RR GG BB` or big-endian RGB565), writing
the result into a guest-supplied buffer. Used by the Bespoke Desktop for
PNG/JPG wallpapers; a 1920x1080 JPEG decodes in tens of milliseconds host-side
versus tens of seconds on the 68k.

| Sub-op | Name    | Arguments / result                                        |
|--------|---------|-----------------------------------------------------------|
| 0      | VERSION | returns PSIMG API version                                 |
| 1      | LOAD    | p0 path, p1 dest buffer, p2 width, p3 height, p4 bpp (16/32), p5 mode (0 stretch, 1 fit); 0 = OK, -1 = error |
| 2      | INFO    | p0 path, p1 which (0 = width, 1 = height); dimension or -1 |

Paths take the MP3PLAY forms (`S:\PIX\WALL.JPG` or `/s/pix/wall.jpg` on a
**HOSTFS drive**) or a plain host path on the Pi (`/home/pistorm/wall.png`).
Implementation: `platforms/atari/psimg/psimg.cpp` (decode/scale/convert) and
the `nf_call_psimg()` handler in `atari_natfeat.cpp`.

### PSPDF
Host-side PDF rendering (Poppler + cairo). The guest names a file, asks for a
page at a zoom, and copies the visible part of the drawn page into its own
buffer in the fVDI pixel format. Drawing happens on a **worker thread** (core 1
by default: core 0 takes the interrupts, core 2 is the 68k thread, core 3
the IPL poller), so no
sub-op below stalls the guest for more than a mutex trylock. Front end:
`PDFGEM.PRG`; test tool `PDFCHK.TTP` (source in `apj-os-tools/pdfchk/`).

| Sub-op | Name     | Arguments / result                                        |
|--------|----------|-----------------------------------------------------------|
| 0      | VERSION  | -> PSPDF API version                                       |
| 1      | OPEN     | p0 path -> handle > 0; -1 error, -2 needs a password       |
| 2      | OPENMEM  | p0 guest buffer, p1 length -> handle (for files not on HOSTFS) |
| 3      | CLOSE    | p0 handle                                                  |
| 4      | INFO     | p0 handle, p1 0=pages 1=outline entries 2=flags            |
| 5      | PAGESIZE | p0 handle, p1 page, p2 zoom -> (w<<16)\|h in pixels        |
| 6      | RENDER   | p0 handle, p1 page, p2 zoom, p3 rotation -> 0 queued       |
| 7      | STATUS   | p0 handle -> 0 ready, 1 busy, -1 failed                    |
| 8      | FETCH    | p0 handle, p1 dest, p2 x, p3 y, p4 w, p5 h, p6 bpp (16/32), p7 row bytes, p8 background RGB |
| 9      | FIND     | p0 handle, p1 needle, p2 page, p3 flags, p4 zoom, p5 result buf, p6 max -> hits on that page |
| 10     | TEXT     | p0 handle, p1 page, p2 zoom, p3 rect or 0, p4 buf, p5 len  |
| 11     | LINKS    | p0 handle, p1 page, p2 zoom, p3 buf, p4 max -> count       |
| 12     | LINKURI  | p0 handle, p1 page, p2 index, p3 buf, p4 len               |
| 13     | OUTLINE  | p0 handle, p1 index, p2 buf {depth, page, title[80]}       |
| 14     | META     | p0 handle, p1 0=title 1=author 2=subject 3=producer 4=creator, p2 buf, p3 len |
| 15     | HILITE   | p0 handle, p1 page, p2 rects (int32 x,y,w,h x n), p3 n (0 clears, max 64), p4 RGB: blended into the page by FETCH |
| 16     | PREFETCH | like RENDER, but only fills the cache; the current page and FETCH are untouched |
| 17     | CONTINUOUS | p0 handle, p1 on/off, p2 gap px: FETCH stitches the previous/next page above/below the current one (when cached), with a gap of background |

- **Zoom** is percent x 10, and 100% is 96 DPI (PDF points x 96/72). All
  geometry - page sizes, fetch rectangles, search hits, link boxes - is in
  device pixels at that zoom, so the guest never handles points.
- **Paths** take the MP3PLAY forms (`S:\DOCS\X.PDF` or `/s/docs/x.pdf` on a
  **HOSTFS drive**) or a plain host path. A PDF on an IDE image is loaded by
  the guest and handed over with OPENMEM instead.
- **The fetch buffer must be TT-RAM** (`Mxalloc(size, 1)`): the copy is a
  host-side memcpy through a direct pointer, and a write below 4 MB would go
  through the ST-RAM DMA mirror and the JIT's self-modifying-code check one
  byte at a time.
- Conversion to the fVDI format (32 bpp `00 RR GG BB`, 16 bpp big-endian
  RGB565) happens **during the fetch**, on the visible pixels only. Converting
  a whole 19 MP page costs ~110 ms on a Pi 4; a 1920x1080 viewport ~12 ms.
- Rendered pages are cached per document (`PISTORM_PDF_CACHE_MB`, default
  192). Rendering a page the guest already has costs nothing, so the front end
  can pre-draw the next page. A page bigger than `PISTORM_PDF_MAX_MPIX`
  (default 24 MP, i.e. roughly 300% zoom on A4) is drawn in bands around the
  viewport, and scrolling past the band queues the next one automatically.
- **Search is one page per call** - about 7 ms per page on a Pi 4, so a
  whole-document search is the guest's loop to drive (and to cancel).
- **Highlights are host-side.** The guest hands FETCH's page the search
  hits (or a selection) as rectangles with HILITE; the Pi blends them
  into the pixels it copies, translucent with a stronger edge. The 68k
  never draws or blends anything for them.
- **PREFETCH** draws the next page into the cache while the user reads
  this one, so the following RENDER answers from the cache at once.
- **CONTINUOUS** makes FETCH treat the document as one strip: rows above
  the current page come from the bottom of the previous one and rows below
  it from the top of the next (centred on the current page, a gap of
  background between), whenever the cache holds them. The guest scrolls
  through the seam and switches its notion of the current page when the
  seam passes the top of the viewport - the pixels are the same either
  side of the switch, so nothing visible happens.
- Page sizes are remembered once asked for or drawn, so PAGESIZE answers
  without the render lock for a page the worker has already seen.
- Implementation: `platforms/atari/pdf/pspdf.cpp` and `nf_call_pspdf()` in
  `atari_natfeat.cpp`. Build deps: `libpoppler-glib-dev libcairo2-dev`;
  install `fonts-urw-base35` as well for PDFs with no embedded fonts.
- Poppler parses untrusted files inside the emulator process, and the
  no-child-process rule means no sandbox: a Poppler crash takes the machine
  down. Use the distribution's patched Poppler, do not vendor an old one.

### PSWEB
The web browser's link to **psweb**, a separate process on the Pi that
renders pages with WPE WebKit off-screen and hands each frame over in the
guest's pixel format through shared memory. The emulator side is a connector
thread on core 1 (`platforms/atari/web/psweb_client.c`); the NatFeat handler
only pushes command records into a ring, reads state words, or copies the
frame into a TT-RAM buffer. One frame is ever in flight: psweb releases the
next to WebKit only after the guest has fetched the last, so the engine runs
at the guest's pace. Front end: `WEBGEM.PRG`; test tool `WEBCHK.PRG`
(`apj-os-tools/webchk/`). Design: `psweb-design.md`.

| Sub-op | Name       | Arguments / result                                   |
|--------|------------|------------------------------------------------------|
| 0      | VERSION    | -> PSWEB API version                                  |
| 1      | STATUS     | -> bit0 psweb socket exists, bit1 connected, bit2 view ready |
| 2      | VIEW_NEW   | p0 w, p1 h, p2 bpp (16/32), p3 flags -> 1 queued; poll STATUS bit2 |
| 3      | VIEW_FREE  |                                                       |
| 4      | VIEW_SIZE  | p0 view, p1 w, p2 h                                   |
| 5      | VIEW_STATE | p0 view, p1 bits (1 visible, 2 focused, 4 topped)    |
| 6      | LOAD       | p0 view, p1 text: URL, host name, words to search, or a GEMDOS path on a HOSTFS drive (-> file:// on the Pi; -1 for any other drive) |
| 7      | NAV        | p0 view, p1 0 back 1 forward 2 reload 3 reload-nocache 4 stop |
| 8      | POLL       | p0 view, p1 ptr to 8 longs: frame serial, damage count, progress (0..1000), flags, cursor (1 = over a link), title serial, URI serial, dialog |
| 9      | FETCH      | p0 view, p1 TT-RAM buffer, p2 bytes per row, p3 rows, p4 rect array (x y w h longs), p5 max -> rects copied; 0 = no new frame |
| 10     | POINTER    | p0 view, p1 0 move 1 press 2 release, p2 x, p3 y, p4 button (1 left 2 right 3 middle), p5 kstate |
| 11     | SCROLL     | p0 view, p1 dx, p2 dy (wheel notches x 120, or pixels when p5 & 1), p3 x, p4 y, p5 flags |
| 12     | KEY        | p0 view, p1 1 down / 0 up, p2 AES key word (scancode<<8 \| ascii), p3 kstate |
| 13     | TEXT       | p0 view, p1 string typed into the page                |
| 14     | GETSTR     | p0 view, p1 0 title 1 URI 2 hovered link 3 status 6 engine, p2 buf, p3 len -> bytes (Atari charset); -3 = try again |
| 19     | ZOOM       | p0 view, p1 percent                                   |
| 24     | SETTING    | p0 view, p1 key (0 JavaScript 1 images 2 UA preset 3 search URL 4 home 5 zoom 6 blocker), p2 value, p3 string or 0 |

Flags in POLL: 1 can go back, 2 can go forward, 4 loading, 8 https, 0x40 the
web process crashed, 0x100 JavaScript off, 0x200 content blocker active.
Results: -1 error, -3 busy (ask again next tick), -4 psweb not connected.

- psweb is socket-activated: `install-full.sh` (the "web" step, `WEB=1`)
  installs `psweb.socket` / `psweb.service`, and connecting to
  `/run/psweb/psweb.sock` starts it; it exits again after ten idle
  minutes (`PSWEB_IDLE_S`) so the 250-480 MB a page costs is only spent
  while browsing. The emulator tries that socket first, then
  `/tmp/psweb.sock` for a psweb run by hand; `PISTORM_WEB_SOCK` names one
  explicitly. The emulator reconnects by itself whenever psweb appears.
- Memory: the web process polices itself (`PSWEB_MEM_MB`, default 40% of
  RAM; killed at 1.5x, which WEBGEM shows as "crashed - reload"), because
  a Pi booted with `cgroup_disable=memory` ignores the unit's MemoryMax.
- Content blocker: `/etc/psweb/adblock.json`, EasyList + EasyPrivacy in
  WebKit's rule format from `psweb/mkblocker.py` (68k rules; `adblock-lite.json`
  is the hosts-only list for a 1 GB Pi). Compiled once into WebKit's DFA
  by `psweb --compile-filter` at install and cached; recompiled only when
  the JSON changes.
- Frames arrive whole in v1 (one damage rectangle covering the view); the
  guest blits the rectangles FETCH returns.

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
| `PISTORM_PDF_CPUS`             | Hex affinity mask for the PDF render thread     |
| `PISTORM_PDF_CACHE_MB`         | Rendered-page cache per document (default 192)  |
| `PISTORM_PDF_MAX_MPIX`         | Largest page drawn in one piece (default 24 MP) |
| `PISTORM_WEB_SOCK`             | psweb socket (default: /run/psweb/psweb.sock if present, else /tmp/psweb.sock) |
| `PISTORM_WEB_CPUS`             | Hex affinity mask for the psweb connector thread |
| `PISTORM_WEB_DEBUG=1`          | Trace the psweb link                             |
