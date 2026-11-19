# Games and demos timed to the 68000

Games, demos and music players that count cycles, split the palette on Timer B,
or open the borders need a 68000 that runs at an ST's speed and takes its
interrupts when an ST would. That is a build of its own in `psctrl.cfg`:

```
[games]
cpu 68000
jit_power 0
cpu_compatible enabled
cpu_speed 8mhz
```

`cpu_compatible` is the prefetch-accurate core; `cpu_speed 8mhz` turns the
governor on. The setup page writes the last two lines when `cpu_compatible` is
ticked. With the JIT on, none of this applies.

What follows is what makes that build keep time, so a report of a game that
does not can say which part failed. Tested on a Pi 4 (ST) and a Pi 3A+
(Mega ST), native Shifter output and HDMI, 50 and 60 Hz. Turrican II,
Shadow of the Beast II, Robocop 2 and Defender of the Crown were the test set.

## The pieces

| Part | What it does | Where |
|---|---|---|
| The governor | Paces the interpreter at the board's own 68000 clock. An 8 MHz PAL ST is 8.021247 MHz; the real Shifter's frame (160256 cycles, measured by the beam model) gives the figure, re-measured once a second, so the guest does not drift 0.27 % against the beam | `cpu/events.cpp`, `platforms/atari/video/beam_clock.c` |
| Interrupt cost | An ST's interrupt costs its IACK cycle as well: 56 cycles for the MFP, 54..63 for HBL/VBL (the E-clock wait), as Hatari counts them, not the generic 44 | `cpu/newcpu.cpp` |
| The lead at an interrupt | A handler starts from real time: whatever lead the guest had over the clock is parked at the outermost interrupt's entry and waited out after the handler. Turrican II's lower border failed in the frames that started 24-56 us ahead | `cpu/events.cpp` |
| Beam-timed Timer B handlers | A Timer B in event-count mode fires a fixed time after a line's display ends; the handler's clock is set to where a real 68000 would be (28 cycles after the display end) and its accesses to the Shifter wait for their moment. A late read is answered with the counter as it stood at the guest's time; a poll of the same register is put on real time. The handler is on that clock until its own RTE (or 40000 cycles). Defender's bottom border opened in 2 of 2775 frames before this | `cpu/events.cpp`, `pistorm_natmem.cpp` |
| Lead write-off | A lead over 1 ms is no lead a 68000 can have - it is an accounting fault, written off and reported with where it came from | `cpu/events.cpp` |
| IPL while idle | While the CPU thread waits for an interrupt (STOP, a wait loop), the IPL lines are looked at every 1 us instead of every 15 | `emulator.c` |
| The tickless kernel | Keeps the kernel's 250 Hz tick off the emulator's cores; Defender's bottom lines jittered until it was in. See INSTALL-README.md | `tools/nohz-kernel/` |

## When a game still misbehaves

One run, with these on, gets what is needed:

```bash
sudo env PISTORM_GOV_REPORT=1 PISTORM_CPU_DIAG=1 \
     PISTORM_RASTER_TRACE=/home/pistorm/raster.txt \
     sh run-pistorm.sh 2>&1 | tee game.log
python3 tools/raster-trace/analyse.py /home/pistorm/raster.txt > raster-report.txt
```

Stop the emulator while the bad screen is showing: the raster trace is a ring
of the last 2M events and ends where you stopped.

- `[GOV] reaching N kHz of the M kHz target` every 5 s: the rate reached, how
  many Timer B handlers ran on the beam's clock (and why others did not), and
  any leads written off. A steady rate with the picture still wrong means it
  is not CPU speed.
- `[GOV] board 68000 clock measured: N Hz` - the clock being followed.
- `[GOV] lead of N us written off ... at pc ...` - the first 20 are printed.
- At a crash, `PISTORM_CPU_DIAG=1` adds `[IRQTL]`: the last 8192 interrupts,
  RTEs, STOPs and TRAPs with their times and the governor's lead.
- `analyse.py` says, frame by frame, which line each colour change landed on,
  and what Timer B's state was in the frames that shifted.
- `tools/stall-probe/stall_probe.c` checks whether a core loses time with
  nothing on it (run with the emulator stopped).

## Switches

All are for diagnosis; the defaults are the tested ones.

| Variable | Default | |
|---|---|---|
| `PISTORM_GOV_KHZ=n` | the board's clock | a rate by hand, in kHz (1000..64000), taken as given |
| `PISTORM_GOV_REPORT=1` | off | the `[GOV]` line every 5 s |
| `PISTORM_GOV_PARK=0` | on | no parking of the lead at an interrupt |
| `PISTORM_GOV_BEAM=0` | on | no beam-timed Timer B handlers |
| `PISTORM_GOV_BEAM_CYC=n` | 28 | where the handler starts, in cycles after the display end (0..200) |
| `PISTORM_GOV_LEAD_MAX_US=n` | 1000 | the largest lead kept, in us; 0 = never write one off |
| `PISTORM_GOV_STEP=n` | 32 | guest cycles between governor checks (8..2000) |
| `PISTORM_GOV_CATCHUP=x` | 2 | catch-up cap as a multiple of the rate; 0 = none |
| `PISTORM_IPL_PASS_NS=n` | 15000 | IPL sampling period while the CPU runs |
| `PISTORM_IPL_IDLE_NS=n` | 1000 | IPL sampling period while it waits; 0 = off (the running period) |
| `PISTORM_WRBEHIND=1` | off | experimental: RAM writes made in a handler go to the bus when it ends |
| `PISTORM_RASTER_VCOUNT=1` | off | the raster trace also reads the video counter at each write (bus reads inside the handler - Shadow of the Beast II crashed with it) |

## Known

- Turrican II's title screen still flickers slightly; the game itself is solid.
