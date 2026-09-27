# PSVIDEL.PRG - Falcon Videl + SuperVidel XBIOS (HDMI only)

AUTO-folder TSR, 68000, position independent, ~2 KB resident. It is the
XBIOS half of the host's emulated Falcon Videl / SuperVidel (cfg
`psvidel`, see `../../PSVIDEL.md`): the part a Falcon's TOS 4 and the
SuperVidel's SV_XBIOS would provide on real hardware.

* Arms the host's register emulation: `$FF8266` SPSHIFT, `$FF8210` line
  width, `$FF820E` line offset, `$FF8264/5` fine scroll, the
  `$FF8280-$FF82C3` timing block, the `$FF9800` palette and `$FF8006`
  (monitor type = VGA). Until then they bus-error exactly as on an ST, so
  boot-time Videl probes (EmuTOS) still find an ST.
* Cookies: `_VDO` = `$00030000` (Falcon video, replacing the ST/STE value)
  and `SupV` when the host has the video RAM window (32-bit bus: `ttram`
  or `addr32`); with `falcon_dsp`, `_SND` gains $1E (8-bit DMA, CODEC,
  DSP, matrix). A full cookie jar is copied into a 64-slot one.
* XBIOS (XBRA id `PSVD`): with `psvidel`, Physbase (2),
  Setscreen/VsetScreen (5), VsetMode (88), VgetMonitor (89 = VGA),
  VsetSync (90), VgetSize (91), VsetRGB (93), VgetRGB (94), Vfixmode (95),
  VsetMask (150), ct60_vmalloc (`$C60E`); with `falcon_dsp`, Dsp_Lock and
  Dsp_Unlock (104/105) and the sound calls locksnd ... buffptr (128-141).
  Everything else goes straight to TOS.
* Also arms the Falcon DSP host port (`$FFA200`) and sound registers
  (`$FF8900-$FF8943`) when the cfg has `falcon_dsp` (`../../FALCON-DSP.md`).

## Install

* Put `PSVIDEL.PRG` in `AUTO`, **before** `MINT.PRG` (and before anything
  else that hooks XBIOS video calls).
* Needs `psvidel` and/or `falcon_dsp` in the emulator cfg; with neither
  the program prints a line and does not stay resident.
* Harmless with fVDI / FVDICON: while no Falcon or SV mode is set, the
  HDMI shows whatever it showed before (fVDI, ET4000 or the ST mirror).

## Behaviour worth knowing

* `VsetMode(-1)` starts out as the Falcon name of the ST mode TOS booted
  in (ST low `$0192`, medium `$0199`, high `$0098`), so a program that
  saves the mode and restores it on exit puts the HDMI back.
* Setting an ST-compatible mode hands the HDMI back to the ST picture and
  writes the ST rez to the real shifter only if it differs.
* `VsetScreen(0, 0, 3, mode)` allocates cleared video RAM (or, without
  the window, 256-byte aligned ST-RAM from Mxalloc).
* With a Falcon/SV mode on, `Physbase` answers the PSVIDEL display
  address, which may be above 16 MB (video RAM).

## Build

`PSVIDEL.PRG` is committed prebuilt. With the crossmint toolchain:

    make

or with plain binutils (`binutils-m68k-linux-gnu` + python3):

    make binutils
