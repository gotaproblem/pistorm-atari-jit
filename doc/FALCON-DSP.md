# FALCON-DSP - the Falcon's DSP56001 and sound matrix, on HDMI

The design note; `platforms/atari/falcon/README.md` is the user side
(setting it up, reading the log, tuning). The companion to PSVIDEL
(`PSVIDEL.md`). PSVIDEL gives the guest the
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
PSVIDEL: Falcon DSP56001 + sound matrix, XBIOS 104-105 109-110 128-141
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
| Sound DMA | `$FF8900-$FF8921`: play 8-bit stereo/mono and 16-bit stereo with 1-4 tracks and the monitor track, record 16-bit, repeat, frame-end events through the MFP hub (`$FF8900` bits 2-3 Timer A, bits 0-1 MFP-15, as on the hardware) |
| Rates | 25.175 MHz and 32 MHz clocks with the `$FF8935` prescaler (49170, 32780, 24585 ... Hz), or the STE rates when it is 0 |
| CODEC | the adder (`$FF8937`), attenuation (`$FF893A`), gain/input/GPIO registers |
| XBIOS | `Dsp_Lock`/`Dsp_Unlock` (104/105), `Dsp_ExecProg`/`Dsp_ExecBoot` (109/110) and the sound calls `locksnd` ... `buffptr` (128-141), done host-side; cookie `_SND` gains $1E |

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

**Lockstep with the 68k.** While the DSP feeds the DAC and answers the
68k about once a VBL (70% of VBLs to engage, 40% to let go, never above
3 a VBL), the sample clock is not the
wall clock: each answer buys one answer's worth of sample periods (the
answers' own average spacing - BOR answers 50 times a second on a 60 Hz
VGA VBL), played at once, and nothing the 68k sends
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
A DSP that is not on the SSI - one the 68k uses as a coprocessor through
the host port while the sound DMA plays, as ACE Tracker does - is never
locked. It is run the way Hatari runs it: on the 68k's own thread, not
the engine's. Hatari interleaves the two by cycle count; the PiStorm 68k
has none, so the DSP is made never to lag instead. On each host-port read
the DSP runs until it can go no further without the 68k - parked at a
`jclr #n,x:<<$ffe9,*` poll, a `jmp *` or a `WAIT` - or is blocked on a
full transmit register, and only then does the 68k see the port. It stays
settled until the 68k writes a word, a command or a flag, or reads a word,
so a 68k spinning on the ISR costs nothing. The DSP's state is invisible
to the 68k between reads, so this is the same as a 56001 that is always
done in time - which ACE needs (it fails on a real Falcon with a CPU
accelerator, where the 68k outruns the DSP). Every wait in ACE's DSP
program is one of those parked forms. A DSP that runs 2^20 cycles with
nothing moving on the port is cut off and resumed at the next read; the
5 s line counts these (`cut off`).

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

While a Falcon or SV mode is on, the guest's ST-RAM screen is no longer
written through to the real ST bus, and the 32 KB Shifter shadow is not
refilled on a screen flip: the HDMI shows the host copy, the real Shifter
shows nothing anyone looks at, and that bus traffic was most of a Falcon
game's frame time. A plain ST's missing joypad port is answered as idle
without a bus cycle, for the same reason.

## Sound DMA frames and Timer A

The play/record frame start and end registers are latched when a frame
starts, as on the Falcon (and STE, and in Hatari): a value written during
a frame is for the next one. Double-buffered players depend on it - ACE
Tracker writes its next buffer's start/end at the top of each Timer A
handler while the other buffer plays. Applied at once (as this code did
until 1 Oct 2026), the playing frame ended immediately on every other
buffer, so Timer A fired again inside the handler: ACE's "CPU Overload"
and a DSP reload.

The DMA plays on the wall clock and the engine catches up in bursts after
a hold-up (a device underrun, a busy core). On a Falcon the DMA and the
68k share one clock, so a frame end is never sooner than a frame after the
last. Here a frame end that would come less than half a frame after the
previous one waits, playing silence, until that much real time has
passed: a hiccup is a short gap in the sound, never a burst of Timer A
interrupts. The 5 s line counts these as `held back`.

## ACE Tracker (1 Oct 2026)

ACE Tracker 2.00 plays with its display running. Besides the sound DMA
frame latching above, it needed:

* Two DSP56001 core fixes, found by running ACE's DSP program in Hatari's
  DSP core and ours side by side and stopping at the first register
  difference: JCLR/JSET/JSCLR/JSSET no longer change the condition codes,
  and the R:Y class-I parallel move (`0001 deff W1MMMRRR`) no longer swaps
  its d (source A/B) and e (destination X0/X1) bits. The second wrote a
  mix coefficient in ACE's DSP code: two clicks per buffer, a loud buzz.
  ACE's whole recorded session is now word for word what Hatari's core
  produces.
* A host-port read with nothing there waits for the DSP (up to 50 ms)
  instead of returning the previous word after 1 ms, which put a stale
  word where ACE expected "OK!"/"SDS".
* In `ipl_task` (emulator.c): a VBL/HBL latched but not yet taken, then
  pre-empted by a virtual level-6 interrupt (the sound Timer A), is re-armed.
  Before, its line stayed asserted with no IACK and the one-latch-per-
  episode rule refused it: no VBL until a real level-6 interrupt (the
  mouse) moved the line. ACE's song display runs off its own VBL routine.

During playback the DSP stays on the engine's core. ACE's mix sits between
its Timer A handler's request and the block it reads back, so on the 68k's
thread the mix would come out of the 68k's time (tried: the GUI froze).

## Differences from the hardware

* The host port FIFOs hold 1024 words each way, not the 56001's double
  buffer. DSPMOD writes several words between status checks, relying on
  the real DSP draining them within a few cycles; the FIFO makes that
  safe whatever the thread scheduling. TXDE means "room left", TRDY
  "the DSP has taken everything". On the DSP->68k side, though, HTDE (the
  flag the DSP waits on before writing its next word) reflects only a
  small depth (`PISTORM_DSP_HTX_DEPTH`, default 4), not the whole FIFO:
  software that hands the DSP a request and reads a block back a word at
  a time (ACE Tracker) paces the DSP with that handshake, and an
  unbounded HTDE let its DSP run whole blocks ahead of the 68k across
  the two threads. DSPMOD sends one word a VBL this way, so it is
  unaffected. `PISTORM_DSP_SPIN=1` busy-runs the DSP for a window after each
  host access (lower round-trip latency, but pegs the engine core and made
  the machine fragile under mouse load - off by default).
* A host command is marked taken when it is raised, not when the DSP
  accepts it.
* The matrix runs at the DAC's rate; a source on another clock is
  resampled by being read at that rate. Handshake mode is treated as
  continuous.
* Sound DMA reads and writes the host copy of guest RAM; a record buffer
  in write-through ST-RAM is not copied to the real chips.
* External input and the ADC are silence; the DSP-to-68k host interrupt
  (level 6 via IVR) is not raised - Falcon software overwhelmingly polls.
* `Dsp_ExecProg` and `Dsp_ExecBoot` (109/110) reset the DSP and write the
  program straight into DSP memory, then run it from P:0 - the state TOS
  4's loader leaves - without the loader itself living in the DSP. The
  rest of the DSP XBIOS (`Dsp_LoadProg`, `Dsp_DoBlock`, abilities ...) is
  not implemented yet and answers like an ST.

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
