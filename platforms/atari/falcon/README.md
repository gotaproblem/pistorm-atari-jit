# Falcon DSP56001 and sound on the PiSTorm

The Falcon's Motorola DSP56001, its host port, the SSI serial port, the
4x4 sound switching matrix, the 16-bit sound DMA (play and record) and the
CODEC's DAC, all emulated on the Pi, with the sound coming out of HDMI.
Together with PSVIDEL (the Falcon's video chip, see `PSVIDEL.md`) it lets
Falcon software that talks to the DSP directly run on an ST or STE with
a PiSTorm. Beats of Rage (Dune, 2013) is the reference title: its DSPMOD
music player, sound effects and both screens work.

`FALCON-DSP.md` in the repository root is the design note - how the DSP
time is clocked, why, and what is measured. This file is the user side.

## Setting it up

1. Add the keys to the build's section in `psctrl.cfg` (or tick them on
   the setup page's Sound and Video tabs):

   ```
   falcon_dsp true       # the DSP and the Falcon sound hardware
   psvidel true          # the Falcon video chip on HDMI
   ttram 128M            # PSVIDEL's video RAM wants a 32-bit bus
   cpu 68030             # or 68040
   ```

2. Copy `atari-tools/psvidel/PSVIDEL.PRG` into the Atari's `AUTO` folder,
   before `MINT.PRG` if MiNT is used. The one TSR arms both halves and
   installs the Falcon XBIOS calls. At boot it prints what it found:

   ```
   PSVIDEL: Falcon Videl + SuperVidel XBIOS on the HDMI output
   PSVIDEL: video RAM at $A1000000, SupV cookie set
   PSVIDEL: Falcon DSP56001 + sound matrix, XBIOS 104-105 128-141
   ```

3. In the emulator's log, look for:

   ```
   [INIT] Falcon DSP56001 + sound matrix (HDMI)      at start-up
   [FALCON] DSP56001 + sound matrix armed             when the TSR runs
   [FALCON] DSP booted (512 words)                    each time a program loads the DSP
   ```

Until the TSR runs nothing answers: `$FFA200` and the Falcon sound
registers bus-error on an ST (or reach the real STE DMA chip on an STE)
exactly as before, so boot probes and non-Falcon software are unaffected.

The Falcon sound comes out of the same HDMI audio device as the STE DMA
sound and the YM2149, so `ym2149` or `dma_sound` may be on as well.

## What software gets

| | |
|---|---|
| DSP | a DSP56001 with the full instruction set, 56-bit accumulators, modulo and reverse-carry addressing, DO/REP, the stack, fast and long interrupts, the X/Y data ROMs; running at twice a Falcon's 32 MHz by default |
| DSP memory | the Falcon's 32K words of external SRAM with the Falcon's overlay (Y:0-3FFF = P:0-3FFF, X:0-3FFF = P:4000-7FFF), plus internal P/X/Y RAM |
| Loading | the real way: reset through PSG port A bit 4, then the 512-word bootstrap through the host port |
| Host port | `$FFA200-$FFA207`, both sides, with the DSP's host interrupts |
| SSI | network and normal mode, up to 8 slots, all the flags and interrupts |
| Matrix | `$FF8930/$FF8932`: DMA play, DSP transmit, external input (silent) and ADC (silent) routed to DMA record, DSP receive and the DAC |
| Sound DMA | `$FF8900-$FF8921`: 8-bit stereo/mono and 16-bit stereo play with 1-4 tracks, 16-bit record, repeat, Timer A and GPIP7 frame events |
| Rates | the 25.175 MHz and 32 MHz clocks with the `$FF8935` prescaler (49170, 32780, 24585 ... Hz), or the STE rates |
| CODEC | the adder, attenuation, gain, input and GPIO registers |
| XBIOS | `Dsp_Lock` / `Dsp_Unlock` (104/105) and `Locksnd` ... `Buffptr` (128-141); the `_SND` cookie gains bits 1-4 |

## Not there yet

* The DSP XBIOS beyond the locks: `Dsp_LoadProg`, `Dsp_ExecProg`,
  `Dsp_DoBlock` and friends answer as they would on an ST. Software that
  loads its DSP code through TOS rather than the bootstrap will not find
  the DSP. Beats of Rage boots its DSP itself.
* The DSP-to-68k host interrupt (level 6 through the IVR). Falcon
  software overwhelmingly polls the host port instead.
* External input and the ADC are silence. Microphone recording records
  silence; recording from DMA play or the DSP works.
* The host port FIFOs are 32768 words deep, not the 56001's one-word
  registers. This is deliberate (see the design note) and only matters
  to code that counts on a full port.

## Reading the log

With `PISTORM_FALCON_STATS=1`, every 10 seconds while the DSP is running:

```
[FALCON] 68k: VBL gap max 20.0 ms, 0 VBLs later than 25 ms; waits on the DSP avg 0.02 ms max 0.61 ms (500); request picked up by the DSP after avg 0.01 max 0.61 ms, answered after avg 0.01 max 0.02 ms
```

* **VBL gap** is the real VBL as the emulator sees it. A 50 Hz screen
  gives 20.0 ms; "later than 25 ms" counts VBLs the 68k was late to.
* **(500)** is how many times the 68k waited for a DSP answer: a music
  player asking once a VBL gives 500 per 10 s at 50 Hz.
* **waits on the DSP** is how long the 68k spent polling for those
  answers, split into the time until the DSP took the request and the
  time until it answered. Both should be well under a millisecond.

and, only when something is off:

```
[FALCON] 49170 Hz (lockstep with the 68k): 3 device underruns, 0 clock resyncs, 0 engine hiccups, 0 answers merged in 10 s (ring 2247 frames, average 2634, DSP load 12%)
```

* **device underruns**: the HDMI audio device found the buffer short -
  audible as crackle. A few at a mode change are normal.
* **lockstep with the 68k** / **wall clock**: how DSP time is being
  clocked (design note). Music players get lockstep.
* **DSP load**: the share of core 1 the DSP took.

If a program hangs waiting for the DSP:

```
[FALCON] stall: 68k polling the host port, nothing moving for 0.3 s - ...
```

followed by the DSP's registers, its last answers and the last 128 words
the 68k sent. `[FALCON] DSP illegal instruction near p:$xxxx` means the
DSP program itself went astray. Either is worth reporting with the lines
around it.

## Tuning

Environment variables, read at start-up:

| | |
|---|---|
| `PISTORM_DSP_TURBO=1..8` | the DSP's speed as a multiple of 32 MHz. Default 2. Programs pace themselves on the SSI and the host port, not on cycle counts, so a faster DSP only finishes its work sooner; 1 is a real Falcon's speed |
| `PISTORM_FALCON_VBLSYNC=0` | keep DSP time on the wall clock even for a program that talks to the DSP once a VBL (the default locks the two together, which is what such programs need on a PiSTorm) |
| `PISTORM_FALCON_STATS=1` | the `[FALCON] 68k:` timing line every 10 s (off by default) |

## Software that checks for a Falcon

Beats of Rage does not care what machine it is on; it just uses the
hardware. Others check the `_MCH` cookie and quit with "Falcon required"
when it is not `$00030000` - and under MiNT/fVDI that message goes to a
console nobody sees, so the program appears to do nothing. For those,
the build's `machine falcon` gives the Falcon's `_MCH` (on the STE
hardware personality otherwise). TOS, MiNT and everything else will then
also take the machine for a Falcon, so keep it to a build for such games.

## Beats of Rage

Both `falcon_dsp` and `psvidel` on, `PSVIDEL.PRG` in AUTO. Then:

* If it shows the hourglass and nothing else, `PSVIDEL.PRG` did not run
  (or `falcon_dsp` is off): the game's `Dsp_Lock` call failed and it is
  waiting for a key on the invisible ST console.
* `HALLFAME.DAT` and `DATAS.DAT` in the game's folder are rewritten when
  the game quits. A quit before the game had loaded them (the hourglass
  case above) writes zeros over them, and the hall of fame then crashes
  with an illegal instruction. Restore both from the archive.
* The game runs at the Falcon's 50 Hz and never drops a frame here; a
  real Falcon does, in busy scenes, so it may feel brisk.

## Files

| | |
|---|---|
| `dsp56k.[ch]` | the DSP56001 core (clean-room, MIT) |
| `falcon_hw.c` | host port, SSI, bootstrap, matrix, sound DMA, the engine thread, the sound XBIOS |
| `falcon_audio.c` | the SDL3 stream on the HDMI device |
| `falcon.h` | the API the rest of the emulator uses |
| `../../../pistorm_natmem.cpp` | the register intercepts and the PSG snoop |
| `../../../atari-tools/psvidel/` | the TSR |
