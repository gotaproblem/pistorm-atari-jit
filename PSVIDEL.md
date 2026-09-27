# PSVIDEL - a Falcon Videl and a SuperVidel, on the HDMI output only

The ST under the PiStorm has an ST shifter and nothing else. PSVIDEL gives
the guest the Falcon's video chip anyway, plus the SuperVidel's extensions,
all emulated on the Pi and shown on its HDMI. The ST's own monitor carries
on showing whatever the real shifter shows - this is **HDMI only**.

It is built on the fVDI work: the same render thread, staging buffer and
DRM present as the fVDI framebuffer, the same 640x480 .. 1920x1200 limits,
8/16/32 bit. Where fVDI gives *GEM* those resolutions, PSVIDEL gives them
to software that drives the video hardware itself: Falcon games and demos
that call `VsetMode` or program the Videl registers, and SuperVidel
software (ScummVM's SV build, SV Doom/Quake ports and the like).

## Switching it on

```
psvidel true          # in the machine's cfg section
ttram 128M            # or addr32 - the video RAM window needs a 32-bit bus
cpu 68040             # Falcon/SV software wants an 030 or better
```

and `PSVIDEL.PRG` (`atari-tools/psvidel/`) in the AUTO folder, before
`MINT.PRG`. Nothing is visible to the guest until that program arms it,
so boot-time probes - EmuTOS looks for a Videl at `$FF8282` - still find
a plain ST and TOS boots exactly as before.

`native_hdmi`, `vga ... FVDI` and ET4000 keep working alongside it: while
no Falcon/SV mode is on, the HDMI shows the same source as before. When a
program sets one, PSVIDEL takes the HDMI; when the program puts an ST mode
back (as every well-behaved one does on exit), the previous source gets a
full redraw and carries on.

## What the guest sees

| | |
|---|---|
| Cookies | `_VDO` = `$00030000`; `SupV` when the video RAM window exists |
| XBIOS | `VsetMode`, `VsetScreen`, `Physbase`, `VgetMonitor` (VGA), `VgetSize`, `VsetRGB`, `VgetRGB`, `Vfixmode`, `VsetSync`, `VsetMask`, `ct60_vmalloc` |
| Videl registers | `$FF820E` offset, `$FF8210` line width, `$FF8264/5` fine scroll, `$FF8266` SPSHIFT, `$FF8280-$FF82C3` timing (`$FF8280`/`$FF82A0` count like a 31 kHz beam), `$FF9800-$FF9BFF` palette, `$FF8006` monitor = VGA |
| Shared with the ST | `$FF8201/03/0D` screen base and `$FF8260` ST shift still go to the real chip and are snooped |
| SuperVidel | extended modes, video RAM at `$A1000000-$A7FFFFFF` (112 MB), the `$A0000000` alias of the low 16 MB, SuperBlitter + firmware version at `$80010000` |

`_MCH` is left alone - use `SETMCH.PRG` if a program insists on a Falcon
machine cookie.

### Modes

The monitor is always VGA, so TV/RGB modes are fixed up the way TOS 4
does on a VGA screen (`Vfixmode`).

* **Falcon**: 320 or 640 wide, 480 or 240 (VERTFLAG) lines, 1/2/4/8
  bitplanes or 16-bit true colour. 640x480 true colour, which a real
  Falcon cannot drive on VGA, is allowed.
* **ST compatible** (STMODES): the HDMI goes back to the ST picture and
  the real shifter is put on that ST rez if it is not there already.
* **SuperVidel** (SVEXT + base resolution): 640x480, 800x600, 1024x768,
  1280x1024, 1600x1200; with OVERSCAN 1280x720, 1680x1050, 1920x1080,
  1920x1200. 2560x1440 is refused (beyond the render stage). 8-bit chunky
  (BPS8C), 16-bit RGB565, 32-bit xRGB (big endian A,R,G,B) and the
  bitplane depths.

`VsetMode` fills the register file with the values TOS 4.04 programs, so
software that reads the Videl back - to save it, or to nudge one register,
like ScummVM moving VDB/VDE to show 400 of 480 lines - sees a real Falcon.

### Register-level software

Writing `$FF8266` with the 2-, 256- or 65536-colour bit selects the Falcon
shifter; the picture then follows the registers: width = line width x 16
/ bpp, height from VDB/VDE and the doubling/interlace bits of `$FF82C2`,
pitch = (line width + offset) x 2, fine scroll from `$FF8265`, palette
from `$FF9800`. Writing `$FF8260` (the ST shift) last - or `$FF8266` with
none of those bits - is "ST again". That last rule differs from a real
Falcon, where SPSHIFT = 0 is the 16-colour Falcon mode; it is there so a
register save/restore of an ST screen does not pull the HDMI away from
the desktop. Falcon 16-colour modes set through `VsetMode` work normally.

## Where the frame comes from

The display base is whichever was set last: the `$FF8201/03/0D` registers
(24-bit, snooped from the real bus) or `VsetScreen`/`Setscreen` (32-bit,
through the TSR).

* **ST-RAM** (and the `$A0000000` alias of it, which is how SuperVidel
  software writes to its screen). The JIT writes ST-RAM straight into host
  memory, so there is no dirty map: the renderer keeps a copy of the last
  frame and converts only the rows that differ. Falcon resolutions are
  small, so this costs little.
* **Video RAM** (`$A1000000+`, from `ct60_vmalloc` or `VsetScreen(0,0,3,m)`).
  Host RAM behind an I/O bank with a 4 KB page dirty map: only rows whose
  pages were written since the last frame are converted, and only those
  rows reach the DRM buffers (the fVDI dirty-band present). A direct
  pointer handed out for a range (Fread straight into video RAM) marks
  the whole range dirty.

Everything is converted to XRGB8888 in the render thread's staging buffer;
the HVS scales it to the HDMI mode, exactly like fVDI.

## SuperBlitter

The firmware version reads 9, so software uses the command FIFO. Blits
run synchronously on the CPU thread when the ninth FIFO word (or a
`CONTROL` write with bit 0) arrives, and the blitter never reads busy.
Mode 0 is a copy, mode 1 copies the bytes whose `SRC2` (mask) byte is
non-zero - the two modes ScummVM uses. Other modes are treated as a copy.

## Limits and differences from the real thing

* The DSP and the Falcon sound system are the separate `falcon_dsp`
  switch (`FALCON-DSP.md`); IDE and the rest of the Falcon are not
  emulated.
* No video on the ST's own monitor for Falcon/SV modes, by design.
* Timing is not cycle-exact: `$FF82A0` / `$FF8280` count a free-running
  59.94 Hz / 31.47 kHz beam; the VBL is still the ST's own (50/60/71 Hz).
* A 24-bit machine (no TT-RAM, no `addr32`) gets the registers and the
  Falcon modes, but no video RAM, no `SupV` cookie and no SuperBlitter;
  `VsetScreen(0,0,3,m)` then allocates ST-RAM.
* `$FF820E`/`$FF8264` exist on an STE: with `shifter ste` their byte/word
  writes also go to the real chip.

## Files

| File | |
|---|---|
| `platforms/atari/psvidel/psvidel.[ch]` | register file, mode logic, video RAM, SuperBlitter, conversion |
| `pistorm_natmem.cpp` | the `$FF82xx`/`$FF98xx`/`$FF8006` intercepts, the three banks at `$A0000000`, `$A1000000`, `$80010000` |
| `platforms/atari/et4000/et4000.c` | `blit_psvidel()`, the render-source hand-over |
| `platforms/atari/network/atari_natfeat.cpp` | the `PSVIDEL` NatFeat (see NATFEATS.md) |
| `atari-tools/psvidel/` | `PSVIDEL.PRG`, the XBIOS TSR |

## Checking it

With the TSR installed the boot shows

```
PSVIDEL: Falcon Videl + SuperVidel XBIOS on the HDMI output
PSVIDEL: video RAM at $A1000000, SupV cookie set
```

and the emulator log `[PSVIDEL] armed, mode $0192, VRAM 112MB at $A1000000`.
Every mode change is logged, e.g.
`[PSVIDEL] VsetMode($401F) -> 640x480 8-bit SuperVidel`.
