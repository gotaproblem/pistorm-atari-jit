# The HDMI mirror of the ST screen: beam-exact

`native_hdmi true` puts a copy of what the real Shifter shows on the HDMI
output. This document covers the **beam-exact** mirror
(`platforms/atari/video/shifter_mirror.c`). It is now the default. The old
mirror, which copied the screen once per VBL, is still available as
`native_hdmi snapshot`.

## What it shows

| | snapshot (old) | exact (new) |
|---|---|---|
| picture area | 320x200 / 640x200 / 640x400 | framed like the RGB output on a monitor: 384 cycles x 270 lines with 5:4 pixels (matched against photos of both outputs on one 1080p panel); on 1080p every low-res pixel is 5x4 output pixels and the border colour fills the panel |
| top / bottom border removal | not shown | shown (vertical DE from the 60/50 Hz switch on lines 33 / 262) |
| left / right border, +2/-2, stop-middle, full right | not shown | shown; each line gets its own length and start |
| palette splits (Timer B, HBL, Spectrum 512) | per line, placed by timestamp, +/- 1-3 lines | at the pixel, to the 4-cycle bus slot |
| med/low splits | one mode per frame | per line |
| screen base | taken whenever ipl_task saw the VBL (late) | the value at the true VBL |
| drawing into the visible screen | shows the end-of-frame state | each word as the Shifter fetched it |
| frame rate | the panel's (60 Hz) | HDMI switches to the ST's 50 or 60 Hz; every frame is shown once |
| STE line width / fine scroll | per frame | per line (`shifter ste`) |

## How it works (short)

1. **Log.** Every write that changes the picture is stamped with the arch
   timer *after* its bus write completes: palette, `$FF820A`, `$FF8260`,
   screen base, STE extras (`shm_io_write`, `pistorm_natmem.cpp` video
   page). ST-RAM writes into the screen being shown, or about to be shown,
   are logged with the value they overwrote (`shm_ram_w` in the `sr_*put`
   accessors, and the blitter). That is one lock-free 4 MB ring written only
   by the CPU thread.
2. **Frame timing.** ipl_task stamps each VBL it sees (the only work left on
   core 3: no screen copy and no palette timeline any more). Sightings are
   never early, so their lower envelope on a fixed period is the true VBL.
   With the governed 68000 (games: `cpu_speed 8mhz`, JIT off), the CPU
   thread also reads the **real video counter** once per frame, mid-display,
   outside any interrupt handler (`shm_cal_poll` from the governor, 4 bus
   reads). The counter gives the exact line and cycle the beam was on, so
   the frame origin is known to within the counter's 4-cycle step. A
   second-order loop then tracks the ST's crystal against the Pi's.
3. **Rebuild** (render thread, never core 3). For the frame that just ended:
   - The registers are replayed up to its start.
   - The GLUE runs line by line. For each line it decides length (512/508/224),
     display start (4/52/56) and end (164/372/376/460/512), and whether
     vertical DE is on (lines 34/63 to 234/263/310), from the sync and
     resolution state at the GLUE's decision cycles. This is the STF table
     Hatari documents in `video.c` (wakestate 1). Writes are snapped to the
     4-cycle bus grid.
   - Screen memory is copied once, then every logged write that landed after
     the Shifter fetched that word is undone.
   - Each line is decoded and coloured. A palette write at cycle *c* colours
     pixels from cycle *c-28* on (Shifter pipeline, Hatari's spec512
     alignment).

## Limits

- Timing is the Pi's view of when each bus write completed. Effects that
  need 2-cycle precision (a +2 line made by a switch on cycle 54) are
  decided on that view.
- Without the governor (JIT on, the APJ-OS desktop), there is no counter
  read. The origin then comes from the VBL envelope plus the offset the last
  calibration measured (`PISTORM_SHM_VBLOFS`, default -86 as measured on a Pi 4, before any calibration). That
  is good to a few microseconds, which is plenty for a desktop.
- Floppy/ACSI DMA into the visible screen is not logged. Nor are writes
  through the unhooked `stram_direct` window, but those do not reach the
  real Shifter either.
- The STE is modelled as an STF plus line width, fine scroll and counter
  writes. The GST MCU's preload differences (left border +20 on the STE)
  are not modelled.
- 71 Hz mono is drawn as 640x400 with no beam model.

## Configuration

```
native_hdmi true        # beam-exact (default when on); also: exact
native_hdmi snapshot    # the old once-a-VBL copy
native_hdmi false
```

Environment (tuning and diagnosis):

| variable | default | |
|---|---|---|
| `PISTORM_SHM=0` | | force the old snapshot mirror |
| `PISTORM_SHM_DEBUG=1` | | one `[SHM]` summary line every 5 s; `=2` adds a line per counter read |
| `PISTORM_SHM_MODESET=0` | | leave the HDMI refresh alone |
| `PISTORM_SHM_CAL=0` | | no video counter reads |
| `PISTORM_SHM_X0` / `_W` | 24 / 384 | horizontal window, in cycles (as a real monitor shows the RGB output; 8 / 416 = Hatari's window) |
| `PISTORM_SHM_PAR` | 5/4 | pixel aspect width/height; 384 x 5/4 = 480, so 480x270 = 16:9 and 1080p is 5x4 per pixel, filling the panel |
| `PISTORM_SHM_ROWS` | 270 | rows shown (276 = Hatari's full framing, x3 on 1080p) |
| `PISTORM_SHM_TOP50` / `_TOP60` | 37 / 8 | first frame line shown (34 / 5 with 276 rows) |
| `PISTORM_SHM_PALDX` | 28 | palette-to-pixel delay, cycles |
| `PISTORM_SHM_CALK` | 0 | counter read latency, cycles |
| `PISTORM_SHM_VBLOFS` | -86 | frame origin minus VBL, cycles (uncalibrated only; -86 measured on a Pi 4) |

### Reading the `[SHM]` line

```
[SHM] 50Hz P=19978.9us frames=251 cal=251/251 err=-2.7..3.0 avg 0.00 cyc rej=0
      vblofs=-81cyc(measured) ev/f=310 ram/f=290 undone/f=12.0 lapped=0 render=900us |
      lines/f left=0.0 +2=0.0 right=0.0 -2=0.0 mid=0.0 full=0.0 node=0.0 top_off=0 bot_off=0
```

- `cal=a/b`: counter reads used / attempted. `err` is how far each read was
  from the model, in cycles. On a steady lock it stays within the counter's
  +/-2 step, with an average near 0.
- `vblofs`: frame origin minus the VBL, as measured. This is the value to
  give `PISTORM_SHM_VBLOFS` for JIT sessions.
- `ev/f`, `ram/f`, `undone/f`: events per frame, screen writes per frame,
  and writes undone because they landed after the fetch.
- `lapped`: the 4 MB log was overrun before the frame was rebuilt (the
  frame is then drawn without the undo).
- `lines/f`: border tricks the GLUE model saw, per frame.
- `top_off` / `bot_off`: frames with the top / bottom border removed.

## Host test

`tools/shifter-mirror-test` drives the module on a fake clock with a
simulated 50 Hz beam: a plain screen, a mid-line palette split checked to
the cycle, writes before and after the fetch, top/bottom/left/right border
removal, and counter calibration against a VBL that lags by up to ms. It
also tests crystal offsets via `SHM_TEST_PPM`.

```
make -C tools/shifter-mirror-test && tools/shifter-mirror-test/shm-test
```
