# stboxmetal - an ST in the Pi driving the real ST's chips

`stboxmetal` runs a plain 8 MHz ST inside the Pi - Musashi 68000, the box's
own MFP, GLUE timing, ACIA, FDC/DMA - and drives the board's **real**
Shifter, YM and IKBD. Every video and sound register write the box makes is
played onto the real bus at the cycle it was made, against the real beam.
The real monitor shows the picture, the real YM plays the sound, the real
keyboard, mouse and joysticks are the input.

Why: with the CPU on the Pi and the chips on the far side of the PiStorm
bus, every bus read waits ~1 us and interrupts are seen late, so games need
per-game timing fixes. Here the timing that matters (MFP timers, Timer B,
HBL/VBL, the video counter) is the box's own and deterministic; the real
chips only receive writes. No per-game tweaks.

Source: `platforms/atari/stbox/stbox_metal.c` (with `stbox.c`, the box's
machine, and `tools/beamtest/beamtest.c`, the beam lock and bus timing).

## Running it

Normally from the setup page: a `psctrl.cfg` build with `engine metal`
(see [PSCTRL-CFG.md](PSCTRL-CFG.md#engine---engine-metal)). Picking it
makes the emulator exec, in the same console and with the same exit codes:

    ./stboxmetal --config ../configs/psctrl.cfg --build <section>

`make` builds `stboxmetal` with the emulator (`make stboxmetal` alone).
It needs root for `/dev/mem`, as the emulator does.

By hand (stop the service first):

    sudo systemctl stop pistorm
    cd ~/pistorm-atari-jit
    sudo ./stboxmetal --config ../configs/psctrl.cfg --build metal 2>&1 | tee stboxmetal.log

| option | |
|---|---|
| `--config FILE --build SECTION` | take the build's ROM, disks and keys from `psctrl.cfg` |
| `--tos ROM` `--disk IMAGE` | without a config: a TOS ROM and up to 10 floppy images |
| `--cal CYCLES` / `--cal-secs S` | write-landing calibration: fix it, or measure it for S seconds |
| `--vdec CYCLE` | the real GLUE's vertical 50/60 Hz decision cycle |
| `--log quiet\|full` | console output (as `metal_log`) |
| `--hdmi on\|off` | the HDMI mirror (as `native_hdmi`) |
| `--capture-every S` | a frame capture every S seconds, no key needed (for games that react to any key) |
| `--core N` | the bus thread's core (default 3) |
| `--ahead CYCLES` | how far the box may run ahead of the beam (default 1024) |

## Keys (real keyboard, or a USB/Bluetooth one)

| key | |
|---|---|
| Ctrl+Alt+Undo | back to the setup page (exit 42); started by hand: a cold reset of the box |
| Ctrl+Alt+F1..F10 | put disk 1..10 in drive A |
| Ctrl+Alt+Help | capture the next frame (see below) |
| Ctrl+Alt+Shift+Undo | quit (so does Ctrl+C) |
| the ST's reset button | stboxmetal stops, waits for the button to be let go and starts again: the same build boots afresh. Needs the RESET pin of CPLD firmware 1.0b; `PISTORM_RESET_PIN=0` turns it off |

On a PC keyboard End or PgDn is Undo and PgUp is Help.

## Calibration

Two values depend on the Pi and the PiStorm, not the build:

- **calibration** - where a write lands on the real bus relative to when it
  is issued (a 10 s right-border sweep);
- **vertical decision** - the cycle where the real GLUE decides 50/60 Hz
  (a 5 s sweep).

Both are measured the first time a Pi model runs a metal build without
`metal_cal` / `metal_vdec`, and saved in `stboxmetal.cal` beside
`psctrl.cfg`, one line per Pi model. A Pi 4 and a Pi 3A+ booting cloned SD
cards each keep their own. Delete a model's line (or set `metal_cal
measure`) to measure again.

## What goes to the real ST

- Writes to `$FF8200-$FF82FF` (video base, sync, palette, resolution) and
  `$FF8800-$FF88FF` (YM), at their cycle.
- RAM the real Shifter may show, copied through a dirty bitmap (the current
  screen first).
- Colour runs closer together than the bus can take (a `movem` to the
  palette) are started early by their deficit, so the run ends on time.
- A colour write more than 64 us late is dropped rather than shown on the
  wrong line.

Never: the MMU, MFP, FDC/DMA or ACIA - the box emulates them. The real IKBD
is read and its bytes fed to the box's IKBD model.

## Timing model

The box follows Hatari 2.4.1 where the real ST's behaviour was measured
against it:

- **Timer B** counts the end of the display line, or its start when AER
  bit 3 is set, 24 cycles after the edge.
- **MFP interrupts** reach the 68000 4 cycles after they are raised; an
  instruction ending sooner is followed by one more before the interrupt.
  The exception costs 44 cycles, the MFP's vectored IACK 12 more. Each MFP
  register access costs 4 cycles.
- **ACIA (6850)**: a keyboard byte that arrives before the previous one was
  read is lost and sets OVRN, as on the real chip.

The 68000 takes an interrupt only between instructions, so a long one
(`movem.l` of 10 registers is 92 cycles, a `ror.l Dn` up to 38) delays a
raster interrupt. Games that change colours in the left border can show
the occasional band starting late; Hatari's cycle-exact display
(`--spec512 1`) shows the same.

## The HDMI mirror

With a display on the Pi's HDMI, the Pi mirrors the box's own picture of
every frame: side borders, overscan, raster splits and mid-line colour
changes exact. It runs on core 2, never the bus core.

- The panel follows the ST's frame rate: after 2 s the mode is matched to
  the 50, 60 or 71 Hz class and its pixel clock trimmed (within 0.5 %) to
  the ST's exact rate, so frames are not dropped or repeated.
- `native_hdmi false` or `--hdmi off` leaves the Pi's screen alone; with no
  display connected the mirror is off.
- The 5 s report (`metal_log full`) has an HDMI line: frames drawn, never
  drawn, flips dropped, longest draw.

## Console output and captures

`metal_log quiet` (default): a start-up line, then only what needs
attention - a lost beam lock, dropped writes or keyboard bytes, a halt
report, captures. `metal_log full`: the calibration tables, a status report
every 5 s (writes, late writes, writing %, HDMI), the input log and border
checks.

**Frame capture** (Ctrl+Alt+Help or `--capture-every`), N counting up:

- `[FRAME N]` lines in the log: every displayed line's video counter and
  stale RAM, and every timed write - its cycle, register, value, how late
  it reached the bus (`b` = in a burst) and the PC that made it;
- `stboxmetal-frame-N.ppm` - the box's own picture of the frame, borders
  included: what the real monitor should show;
- `stboxmetal-frame-N-real.scr` - the 32000 bytes the real Shifter showed,
  read back from the real RAM.

**Halt report** (a double bus fault): registers, stack, the instructions
before the halt and the box's whole RAM in `stboxmetal-halt-N.ram`.

**Stall report**: no writes and no RAM change for 5 s prints the PC,
registers, a loop histogram, MFP, ACIA and exceptions, and saves
`stboxmetal-stall-N.ram`.

## Not yet

- The real floppy drive (disk images only).
- STE.
- Pera Putnik loader pictures (Jim Power): the left third shows garbage in
  the box itself.
