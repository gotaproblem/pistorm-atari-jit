# FALCON-DSP - the Falcon's DSP56001 and sound matrix, on HDMI

The companion to PSVIDEL (`PSVIDEL.md`). PSVIDEL gives the guest the
Falcon's video chip; this gives it the rest of what Falcon software leans
on: the Motorola DSP56001, its host port, the SSI serial port, the 4x4
sound switching matrix, the 16-bit sound DMA (play and record) and the
CODEC's DAC - all emulated on the Pi, with the DAC coming out of HDMI.

Written for **Beats of Rage** (Dune, 2013), whose music is bITmASTER's
DSPMOD: a 68k driver that streams MOD sample data through the host port
to a 56001 program that mixes it and clocks it out of the SSI into the
DAC. That exact pairing - BOR's `SYS/DSPMOD.TCE` driving its DSP code -
runs in the test harness and its output matches libopenmpt's rendering
of the same MOD (envelope correlation 0.97, same spectrum).

## Switching it on

```
falcon_dsp true       # in the machine's cfg section
psvidel true          # for Falcon video too (BOR needs both)
ttram 128M            # PSVIDEL's video RAM wants a 32-bit bus
cpu 68040
```

and `PSVIDEL.PRG` in AUTO before `MINT.PRG` - the same TSR arms both.
Its boot line says what it found:

```
PSVIDEL: Falcon DSP56001 + sound matrix, XBIOS 104-105 128-141
```

The emulator log shows `[INIT] Falcon DSP56001 + sound matrix (HDMI)`,
`[FALCON] DSP56001 + sound matrix armed` at the TSR, and
`[FALCON] DSP booted (512 words)` each time a program loads the DSP.

Until the TSR runs nothing answers: `$FFA200` and the Falcon sound
registers bus-error (ST) or reach the real STE DMA chip (STE) exactly as
before, so boot probes are unaffected.

## What the guest gets

| | |
|---|---|
| DSP | DSP56001 at 64 MHz by default (2 x the Falcon's 32 MHz; `PISTORM_DSP_TURBO=1..8` sets the factor - see below): full instruction set, 56-bit accumulators with limiting and scaling, modulo and reverse-carry addressing, DO/REP, the 15-level stack, fast and long interrupts, X/Y data ROMs (sine, mu-law, A-law) |
| DSP memory | the Falcon's 32K words: P:$0000-$7FFF, Y:$0000-$3FFF = P:$0000-$3FFF, X:$0000-$3FFF = P:$4000-$7FFF (Falcon spec table 6.1), internal P/X/Y RAM |
| DSP reset | PSG port A bit 4 (high = reset), then the 512-word bootstrap through the host port, as on the Falcon |
| Host port | `$FFA200-$FFA207`: ICR, CVR (host commands), ISR, IVR, TX/RX H/M/L; DSP side HCR/HSR/HRX/HTX with their interrupts |
| SSI | network and normal mode, up to 8 slots, TE/RE, TDE/RDF, underrun/overrun, TX/RX interrupts |
| Matrix | `$FF8930/$FF8932` routing between DMA play, DSP transmit, (silent) external input and ADC, and DMA record, DSP receive, DAC |
| Sound DMA | `$FF8900-$FF8921`: play 8-bit stereo/mono and 16-bit stereo with 1-4 tracks and the monitor track, record 16-bit, repeat, Timer A (SCNT) and GPIP7 (SINT) frame events through the MFP hub |
| Rates | 25.175 MHz and 32 MHz clocks with the `$FF8935` prescaler (49170, 32780, 24585 ... Hz), or the STE rates when it is 0 |
| CODEC | the adder (`$FF8937`), attenuation (`$FF893A`), gain/input/GPIO registers |
| XBIOS | `Dsp_Lock`/`Dsp_Unlock` (104/105) and the sound calls `locksnd` ... `buffptr` (128-141), done host-side; cookie `_SND` gains $1E |

## How it runs

One engine thread (`falcon-dsp`, core 1, SCHED_OTHER, created on the
main thread) is the Falcon's sample clock. It follows CLOCK_MONOTONIC a
millisecond at a time: per sample period it fetches the playback DMA's
words, runs the DSP through the period with the SSI slots placed 1/8
frame apart, routes the matrix, writes the record DMA and hands the DAC's
pair to an SDL3 stream on the HDMI device (the same device as the STE DMA
sound and the YM2149). A 30 ms ring in front of the device absorbs its
lumpy reads; the ring's fill level only trims the clock when the device's
crystal drifts from the Pi's.

**Lockstep with the 68k.** While the DSP answers the 68k about once a
VBL (70% of VBLs to engage, 40% to let go), the sample clock is not the
wall clock: each answer buys exactly one VBL of sample periods (the VBL
period as ipl_task measures it), played at once, and nothing the 68k sends
after an answer reaches the DSP until they have played. Between those
bursts the DSP gets instruction cycles whenever the 68k waits on it, so it
reads, mixes and answers at once. The ring's fill trims the samples per
VBL by up to 15%, which is all the audio device needs.

This is for DSPMOD (BOR's music), which asks the DSP once a VBL how many
samples it played and mixes that many. On the wall clock the answer
follows when in its VBL the 68k happened to ask; BOR's VBL work on the
PiStorm varies enough that answers swung from a handful to twice a VBL's
worth, DSPMOD's buffers do not hold that, and the DSP ran off into garbage
(an answer of 0 is a 65536-pass mix loop). Locked, every answer is one
VBL's samples, as on a Falcon whose VBL is never late. A VBL the 68k
misses is a short gap in the sound instead. `PISTORM_FALCON_VBLSYNC=0`
keeps the wall clock throughout. The status line says which is in use.

The DSP runs twice as fast as a Falcon's by default. Programs pace
themselves on the SSI and the host port, not on cycle counts, so this only
makes the DSP finish its work sooner - which DSPMOD needs on the PiStorm:
BOR's VBL waits for the DSP's answer, and once one VBL runs late the next
one comes early and finds the DSP still mixing. That wait makes it late in
turn, the sample counts grow with the gaps, and a count far beyond a VBL's
worth ends in a 65536-pass mix loop. `PISTORM_DSP_TURBO=1` gives the
Falcon's speed; idle polling costs nothing at any setting.

The 68k and the DSP meet in lock-free FIFOs in the host port. A
`[FALCON] ... device underruns, ... clock resyncs, ... engine hiccups`
line (with the DSP's share of its core) appears every 10 s while any of
them is happening; `[FALCON] stall: ...` when the 68k has polled the host
port for 0.3 s with nothing moving (with the DSP's state, a few of its
PCs and the last words it sent the 68k, with the sample periods and
milliseconds between them); `[FALCON] DSP illegal instruction ...` when the
DSP program goes astray.

A DSP that polls a peripheral in place (`jclr #n,x:<<$ffe9,*` and the
like) is skipped forward to its next event, so waiting costs nothing.
BOR's music loop spends most of its time there; the harness runs 20 s of
it (68k emulation included) in 1.7 s on one x86 core.

## Differences from the hardware

* The host port FIFOs hold 1024 words each way, not the 56001's double
  buffer. DSPMOD writes several words between status checks, relying on
  the real DSP draining them within a few cycles; the FIFO makes that
  safe whatever the thread scheduling. TXDE means "room left", TRDY
  "the DSP has taken everything".
* A host command is marked taken when it is raised, not when the DSP
  accepts it.
* The matrix runs at the DAC's rate; a source on another clock is
  resampled by being read at that rate. Handshake mode is treated as
  continuous.
* Sound DMA reads and writes the host copy of guest RAM; a record buffer
  in write-through ST-RAM is not copied to the real chips.
* External input and the ADC are silence; the DSP-to-68k host interrupt
  (level 6 via IVR) is not raised - Falcon software overwhelmingly polls.
* The DSP XBIOS beyond the locks (`Dsp_LoadProg`, `Dsp_ExecProg`,
  `Dsp_DoBlock` ...) is not implemented yet: software that loads its DSP
  code through TOS rather than the bootstrap will find those calls
  answering like an ST.

## Files

| File | |
|---|---|
| `platforms/atari/falcon/dsp56k.[ch]` | the DSP56001 core (clean-room, MIT) |
| `platforms/atari/falcon/falcon_hw.c` | host port, SSI, bootstrap, matrix, DMA, engine thread, sound XBIOS |
| `platforms/atari/falcon/falcon_audio.c` | the SDL3 stream (built like `ym2149.c`) |
| `pistorm_natmem.cpp` | the `$FFA2xx` / `$FF89xx` intercepts and the PSG snoop |
| `atari-tools/psvidel/` | the TSR (cookies and XBIOS for both halves) |

The core was checked against 62 instruction-level cases (flags,
limiting, rounding, DIV, NORM, loops, addressing modes, moves) assembled
with `a56`, then end to end with BOR's own DSPMOD driver and module
running under a Musashi 68020 against this module.
