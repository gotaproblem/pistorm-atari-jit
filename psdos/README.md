# psdos - DOS games in a GEM window

psdos runs DOSBox Pure (libretro, ARM64 dynamic recompiler) as its own
process on the Pi. The Atari side is DOSGEM.PRG (apj-os-tools/dosgem): a GEM
window that shows the DOS screen and sends it keyboard, mouse and pads,
through the PSDOS NatFeat (platforms/atari/dos/psdos_client.c).

## Install

    BUILD=0 DOS=1 ./install-full.sh

Builds psdos and the DOSBox Pure core (about 30 minutes on two cores),
installs them to /usr/local/lib/psdos and starts psdos on demand
(psdos.socket). Games go in atari-share/dosgames, or anywhere DOSGEM can
open: a folder, .ZIP, .EXE/.COM/.BAT, .ISO/.CUE/.IMG.

Core options: /etc/psdos/psdos.conf, one `dosbox_pure_<option> = <value>`
per line. Rebuilding psdos alone:

    make -C psdos psdos
    sudo install -m 755 psdos/psdos /usr/local/lib/psdos/psdos
    sudo systemctl stop psdos.service      # the next connection starts the new one

## Input

- Keyboard and mouse: click in the window to capture them. Release with
  Ctrl+Alt+G (Ctrl+Option+G on a Mac keyboard), the middle mouse button,
  Scroll Lock or Ctrl+Alt+F12; Undo on an ST keyboard. Another window
  coming to the top releases too.
- Pads (`usb gamepad`, USB or Bluetooth, and the ST joystick ports) are
  DOS's whenever the window is on top - no capture needed. See
  GAMEPAD_README.md. DOSBox Pure's start menu (L/R tabs) has the Gamepad
  Mapper.

## Performance and limits

The emulated PC runs on ONE of the Pi 4's Linux cores (core 1, shared
with psdos's own frame thread and the emulator's host workers). DOSBox
Pure's automatic CPU speed settles at what that core can deliver, about
**55,000 cycles - a 486DX2-66 to Pentium-75 class PC** (measured on a
stock Pi 4, the emulation thread at 82-84% of the core).

- **Should run well:** real-mode and 486-era games - Commander Keen, the
  LucasArts adventures, Doom-era and earlier.
- **Not playable:** Pentium-class 3D games. Eradicator (1996) draws 3-4
  frames a second at 320x240, whatever the Atari side does.

The Atari side is not the limit: DOSGEM polls 50 times a second and puts
a 1.2-megapixel frame on the screen in about 21 ms (the copy itself is
done by the host, ~4 ms), so it could show about 25 frames a second.

What would help a demanding game, not pursued:
- overclocking the Pi (`arm_freq=2000`, `over_voltage=6`, with cooling):
  about a third more;
- giving the emulation thread core 1 alone, psdos's frame thread on
  core 0: up to 15-20% more. Simply allowing both cores
  (`PSDOS_CPUS=3`, `CPUAffinity=0 1`) was no better in testing;
- the game's own detail / view-size options.

A fixed `dosbox_pure_cycles` above what the core delivers only makes the
speed fall below 100%.

## Diagnosing a slow game

`journalctl -u psdos -f` while it runs. Every 5 s:

    stats: core 70.0 fps (wants 70.0), retro_run avg 4.2 ms ..., shown 4.0/s, not taken 6.0/s, late starts 0

core fps is the frame loop, not the game. "shown" is frames handed to
DOSGEM; "not taken" counts every refresh that arrived while DOSGEM still
had the last one, unchanged frames included, so it overstates what the
game draws. For the game itself add `dosbox_pure_perfstats = detailed` to
psdos.conf:

    core: Speed: 100.0%, DOS: 320x240@70.01hz, Actual: 70.02fps, Drawn: 4fps, Cycles: 53545 (DynRec)

Drawn is the game's real frame rate, Cycles the emulated CPU speed; Speed
under 100% means the Pi cannot keep up. The emulator console shows the
Atari side:

    [PSDOS] dosgem: ticks 50.0/s, taken 5.6/s, fetch 2.78 ms, after 18.0 ms avg ..., big blits 5.6/s 4.23 ms avg 1223 Kpx (copy-rows)

`top -H -p $(pidof psdos)`: one thread near 100% = the emulated PC is
CPU-bound.
