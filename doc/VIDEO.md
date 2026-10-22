# Host video playback (MP4 / MKV / ...) — the VIDPLAY NatFeat

The MP3PLAY idea one level up. The 68k asks the Pi to play a file; the Pi
demuxes, decodes, scales and mixes it. The Atari's share is a handful of
NatFeat traps per second, so a film plays at full speed on an 8 MHz STfm while
the guest keeps running underneath.

`NATFEATS.md` listed this as *planned*, with the intended design. This is that
design, implemented — and then corrected by the hardware several times over.

## What actually works, on a Pi 4

| Source | Path | Result |
|---|---|---|
| H.264 ≤1080p | `h264_v4l2m2m`, stateful block | hardware, effortless |
| **H.265 any size incl. 4K** | `v4l2request` + rpivid, **zero copy** | hardware, ~15% of one core |
| H.264 **4K** | software only | ~9 fps — see below |
| MPEG-2 / MPEG-4 / VP8/9 | software | fine at DVD-ish sizes |
| Audio | AAC / AC3 / MP3 / Vorbis / Opus / FLAC / PCM | mixed with ST & STE sound |

**The one real gap: 4K H.264.** The Pi 4 has two decoder blocks —
`bcm2835-codec` (`/dev/video10`, stateful, H.264, tops out at 1920×1088) and
`rpivid` (`/dev/video19`, stateless, HEVC, good to 4Kp60). There is no stateless
H.264 device, so `h264_v4l2request` has nothing to bind to, and a 4K H.264 file
falls entirely on the CPU. The same content as H.265 uses the hardware and plays
perfectly. Check any machine with:

```
for d in /dev/video*; do echo -n "$d: "; \
  v4l2-ctl -d $d --list-formats-out 2>/dev/null | grep -oE "'S26[45]'|'H264'"; echo; done
```

## Hardware HEVC — the zero-copy path

Requires an FFmpeg with the V4L2 Request API hwaccels; stock Debian has none.
`tools/build-rpi-ffmpeg.sh` builds one into `/opt/rpi-ffmpeg`, isolated from the
system (not in the linker cache; the emulator finds it via `PKG_CONFIG_PATH` and
a baked-in rpath). Also needs `dtoverlay=rpivid-v4l2` so `/dev/video19` exists.

`attach_v4l2request()` looks the hwaccel up **by name at runtime**, so this is
not a build dependency: with a distro FFmpeg it simply isn't found and HEVC
decodes in software as before.

Frames come back as dmabuf handles in Broadcom **SAND** tiled layout. libavutil
deliberately refuses to download those to linear memory, so there is no CPU
fallback — and no need for one. `AVDRMFrameDescriptor` → `drmPrimeFDToHandle` →
`drmModeAddFB2WithModifiers` (SAND modifier and all) → `drmModeSetPlane`. The
decoder's own memory is scanned out directly: no decode copy, no colour
conversion, no scaling, nothing touching the CPU between decoder and display.
Imports are cached on the dmabuf fd, since the decoder recycles a small pool.

Measured: 53 s of 4K H.265 decoded in 34.6 s wall with **5 s of CPU**.

## The scaling limit that is not obvious

`drivers/gpu/drm/vc4/vc4_plane.c`:

```c
vscale_factor = DIV_ROUND_UP(src_h, crtc_h);
membus_load  += src_w * src_h * vscale_factor * cpp;
membus_load  *= vrefresh;
```

and `vc4_kms.c` rejects above 1.5 GB/s ("the absolute limit is 2Gbyte/sec, but
let's take a margin"). Downscaling forces the HVS to read every source line
within one output line's time, so **halving the destination height doubles the
read bandwidth**. On this kernel the load tracker is not enforcing, so exceeding
it returns no error at all: the HVS underruns and the plane comes out **black**.
It has to be computed up front, not detected.

The factor is an integer ceiling, so legal sizes come in steps. For 3840×2160
NV12 at 60 Hz:

| destination height | factor | load | |
|---|---|---|---|
| 1080 | 2× | 1.24 GB/s | works |
| 720–1079 | 3× | 1.99 GB/s | works (measured) |
| ≤540 | 4×+ | 2.49 GB/s+ | black |

So a 4K film gets a **1280×720 window** and no smaller. A 1080p source starts
four times lower and shrinks to an eighth of the screen height quite happily,
which is why ordinary files behave normally. `PISTORM_VID_BUDGET` (GB/s, default
2.0) tunes it; `PISTORM_VID_MAXDOWN` overrides the ratio directly.

Software frames never hit this — they are pre-scaled to the display size before
reaching the plane. Only hardware frames, which cannot be shrunk on the CPU, are
constrained.

## Scheduling inheritance — the rule that bit three times

**Anything that creates a thread must not be called from the 68k CPU thread.**
That thread is SCHED_FIFO, pinned to CPU 2, in a JIT loop that never yields.
Anything born onto it is never scheduled.

1. **The media threads.** A thread created with default attributes inherits its
   creator's policy and affinity, so it never runs — and cannot even reach the
   code that would demote it. Fixed with `PTHREAD_EXPLICIT_SCHED` +
   `SCHED_OTHER` + an affinity mask on the `pthread_attr_t`, set by the creator.
2. **`avcodec_open2()`.** A multi-threaded software decoder spawns its *own*
   workers, which inherit whoever opened it. Opened from the NatFeat handler
   they land on CPU 2, and the first `avcodec_send_packet()` blocks forever: no
   frames, no error. So the codecs are opened **on the decode thread**.
   libavformat already knows about this — `avformat_find_stream_info()` forces
   `threads=1` on its probe decoders with the comment *"Ensure non-blocking
   operation"*.
3. **Child processes**, which is why there are none. This was the original
   ffmpeg-subprocess failure during the MP3 work.

The threads run on CPUs 1 and 3: CPU 2 is the 68k, CPU 0 is the et4000 render
thread (SCHED_IDLE, and already tight). `PISTORM_VID_CPUS=b` adds CPU 0 back.

## Buffering and the clock

Three stages, deliberately decoupled:

* **video packet queue** (96 packets, ~4 s) — the demuxer must never block on
  the video path, because the same loop feeds audio. Without it, audio could
  only run as far ahead as the scanout ring was deep (~0.12 s); any hiccup
  drained it and playback spiralled down.
* **scanout ring** (5 buffers, **2 reserved**). Two are in use at any instant:
  the one the decoder is writing, *and the one the CRTC is scanning*. A
  committed frame keeps being scanned until the NEXT commit replaces it, not
  merely until the commit latches — reserve only one and the decoder paints the
  next frame into the picture the display is reading. That is tearing, and with
  fine detail it reads as stutter: perfect frame counts, zero drops, correct
  pacing, and a picture that still looks wrong. Hardware frames never hit it,
  because dmabufs are held by reference (`g_scanned`) until something replaces
  them — which is why 4K HEVC was the only format that looked right while every
  other one was broken.
* **audio** — half a second ahead, in the SDL3 mixer alongside ST sound.

The clock **free-runs** off the monotonic timer and is steered toward the audio
position (2% per frame, snapping past 0.5 s). It used to *be* the audio
position, which deadlocked: the clock could only advance when the demux loop
pushed audio, and that loop was blocked waiting for the clock.

Presentation is **paced by vblanks**, not by the clock. The display only changes
at a vblank, so a commit lands on whichever one comes next and a millisecond of
sleep jitter moves a frame a whole refresh period. The presenter blocks on
`drmWaitVBlank`, discards any frame a newer one has already superseded — all
within that vblank, or it can never catch up — and commits the one that belongs
on the next refresh. The commit itself is atomic and non-blocking with no
page-flip event, so it neither waits for the guest's presenter nor competes for
events on the shared fd.

Where the frame rate does not divide into the refresh (24 fps on 60 Hz gives a
2:3 cadence, 25 fps gives 2,2,3) the display is switched to a rate that does —
50 Hz for PAL, and back again on stop. Refresh only; the resolution never
changes, so the guest is unaffected. There is a floor of 48 Hz
(`PISTORM_VID_MINHZ`) because the desktop and mouse run at this rate too, and
24 Hz makes them unusable.

Software decode that falls behind gives away quality in stages — deblocking at
0.5 s, non-reference frames at 1.5 s, B-frames at 3 s — restoring it all on
catching up.

## Build

```
sudo apt install libavformat-dev libavcodec-dev libavutil-dev \
                 libswscale-dev libswresample-dev
make
```

That builds and plays everything — H.264 in hardware, the rest in software.
Hardware **HEVC** needs an FFmpeg the distro does not provide; see below.

## Getting hardware HEVC

Two things are required, and only one of them can be packaged.

**The kernel side.** Add to `/boot/firmware/config.txt` and reboot:

```
dtoverlay=rpivid-v4l2
```

`/dev/video19` should then exist and advertise `S265`:

```
v4l2-ctl -d /dev/video19 --list-formats-out
```

**The userspace side.** The V4L2 Request API hwaccels are not in Debian's
FFmpeg, so they have to come from the out-of-tree patch set:

```
make ffmpeg      # fetches a prebuilt tarball, or builds from source
make
```

`make ffmpeg` unpacks into `./ffmpeg` and the Makefile picks it up from there.
Prefer to install it system-wide instead? `bash tools/build-rpi-ffmpeg.sh` puts
it in `/opt/rpi-ffmpeg`, which is searched second.

### Why this needs care, and how to check it worked

**Debian trixie ships FFmpeg 7.1.3 and this builds FFmpeg 7.1.3.** Every
soname is therefore identical — `libavcodec.so.61` either way — and the distro
build works perfectly except that the hwaccel is missing. That is the most
annoying failure mode available: everything runs, nothing is fast, and nothing
in any filename tells you which one you got.

Two consequences, both already handled but worth knowing:

* **Nothing is ever added to the linker cache.** If these libraries landed on
  the default search path they would silently replace the system FFmpeg for
  VLC and everything else on the machine.
* **The choice is recorded in the emulator as an rpath**, not left to search
  order. For `./ffmpeg` that rpath is `$ORIGIN`-relative, so the tree can be
  moved or copied to another Pi and still resolve — and, importantly, it
  survives `sudo`, which resets the environment and would discard
  `LD_LIBRARY_PATH`.

Which one a build would use:

```
make ffmpeg-status
```

And which one the binary actually got — the only real proof:

```
ldd ./emulator | grep libav
```

Every line should point where you expect. If they point at `/usr/lib` when you
meant to use the patched build, the rpath did not take.

### Publishing a build for other people

```
PREFIX=/tmp/stage bash tools/build-rpi-ffmpeg.sh   # ~40-90 min on a Pi 4
bash tools/package-rpi-ffmpeg.sh /tmp/stage
```

That produces `pistorm-rpi-ffmpeg-7.1.3-deb13-arm64.tar.gz` and a `.sha256`.
Attach both to a GitHub release tagged `ffmpeg-7.1.3`; `make ffmpeg` derives
the URL from the clone's own `origin`, so forks fetch their own binaries rather
than someone else's.

The name carries the distro and architecture on purpose. These libraries link
against *this* system's glibc, libdrm and libudev, so a Bookworm Pi must fail
to download rather than install something that half works.

Three points on licensing, since the tarball contains binaries:

* The configure line has no `--enable-gpl` and no `--enable-nonfree`, so the
  result is **LGPL-2.1-or-later** — redistributable as shared libraries
  alongside this MIT-licensed emulator. Do not add those flags casually.
* The obligation is that the corresponding source be obtainable. That is why
  `build-rpi-ffmpeg.sh` pins an exact **commit** rather than a branch name: a
  moving branch would make the `SOURCE` file in the tarball untrue the moment
  upstream pushed.
* `package-rpi-ffmpeg.sh` copies the licence text, the build script and the
  commit hash into the tarball, so the paperwork travels with the binaries
  instead of depending on this repository staying online.

## Use

The file must be on a **HOSTFS drive** — the Pi opens it by path, so a video
inside an ACSI/IDE image is unreachable.

* `VIDPLAY.TTP` (`cdev/vidplay/`) — command line and drag & drop.
* `VIDGEM.PRG` (`cdev/vidgem/`) — GEM app: playlist, transport, and the picture
  mapped onto its own window, following it as you move and resize. When the
  overlay cannot draw small enough for the current window it resizes the window
  to the smallest workable size and refuses to shrink past it; if even that will
  not fit the desktop it keeps the picture hidden and says so rather than
  burying its own controls. Keys: SPACE pause · ←/→ ∓10 s · N/P prev/next ·
  L list · F fullscreen · W window · O open · +/- volume · Q quit.

## NatFeat interface

`NF_GET_ID("VIDPLAY")`, then `NF_CALL(id | subop, ...)`. Sub-ops 0–7 are
identical to MP3PLAY.

| Sub-op | Name | Arguments / result |
|--------|--------|--------------------------------------------------------|
| 0 | PLAY | ptr to path; 0 = OK, -1 = error |
| 1 | STOP | stop, hide the overlay, free everything |
| 2 | STATUS | 1 while playing or paused |
| 3 | PAUSE | param0: 1 = pause, 0 = resume |
| 4 | SEEK | param0: signed seconds, relative |
| 5 | POS | position in seconds (-1 n/a) |
| 6 | LEN | duration in seconds (0 unknown) |
| 7 | META | param0: 0=title 1=author 2=codecs; param1: buf; param2: len |
| 8 | RECT | x, y, w, h in display pixels. All 0 = auto letterbox. Negative w/h = hide the picture, keep the sound |
| 9 | VOLUME | 0..200 percent |
| 10 | INFO | 0=w 1=h 2=fps×100 3=audio 4=hwdec 5=volume 6=display w 7=display h 8=hidden 11=min drawable w 12=min drawable h |

`INFO 11/12` return **-1 until the first frame is decoded** — the constraint
depends on what the plane's source turns out to be. "Don't know" and "no limit"
must not be the same answer, or a front-end will show a picture that swallows
its own controls.

## Environment variables

| Variable | Effect |
|---|---|
| `PISTORM_VID_HWDEC=0` | force software decode |
| `PISTORM_VID_HWMAX=<w>` | try the stateful H.264 block beyond 1920 (it will fail) |
| `PISTORM_VID_BUDGET=<GB/s>` | HVS memory-bus budget (default 2.0) |
| `PISTORM_VID_MAXDOWN=<n>` | override the downscale limit directly |
| `PISTORM_VID_THREADS=n` | software decoder threads (default 3) |
| `PISTORM_VID_CPUS=mask` | hex affinity for the media threads (default `a` = CPUs 1,3) |
| `PISTORM_VID_PLANE=<id>` | force a specific DRM overlay plane |
| `PISTORM_VID_MODESET=0` | never switch the display refresh to match the frame rate |
| `PISTORM_VID_MINHZ=<hz>` | lowest refresh the matcher may select (default 48) |
| `PISTORM_VID_ATOMIC=0` | use blocking `SetPlane` instead of non-blocking atomic |
| `PISTORM_VID_DEBUG=1` | per-second decode / present statistics |

## Known limits

* **4K H.264 has no hardware path** (see above). H.265 at the same size is fine.
* No subtitle rendering; burnt-in works, soft is ignored.
* Seeking is keyframe-accurate (`AVSEEK_FLAG_BACKWARD`).
* The overlay is a hardware plane, so it composites above the *whole* Atari
  screen: another GEM window dragged over the video window is drawn underneath
  the picture.
* Video and MP3 are independent — starting a film does not stop an MP3.
* Needs the DRM display path (`PISTORM_VGA_DRM=1`, the default) and a spare
  YUV-capable overlay plane on the CRTC. `./drmprobe` lists them; the player
  prints the full plane table with zpos at startup.
* Both the guest plane and the video plane commit to the same CRTC and serialise
  on vblank, roughly doubling the fvdi render time. `PISTORM_DRM_ASYNC=1` puts
  the guest presenter on its non-blocking path and recovers most of it.
* Four 1080p NV12 scanout buffers are ~12 MB of CMA; hardware frames use the
  decoder's own buffers instead. If `CREATE_DUMB` fails, raise
  `dtoverlay=vc4-kms-v3d,cma-256`.
* The emulator links libdrm statically while libavcodec pulls in `libdrm.so.2`,
  and `-rdynamic` means our copy can interpose. Same library, same machine, so
  in practice a non-event — but worth suspecting if DRM misbehaves only when
  FFmpeg is linked in.
