# Regression suite

One command, then walk away:

```
cd ~/pistorm-atari-jit
make -j2
nohup tools/regress/run.sh > /dev/null 2>&1 &
```

When it finishes, read `tools/regress/results/<commit>/summary.txt`. It has:

- the pass count;
- every failure, tagged **NEW** if it passed last time;
- what was **FIXED** since the last run;
- every check;
- every measurement, with the previous run's figure beside it.

Exit status: 0 all passed, 1 something failed, 2 setup problem.

`tools/regress/run.sh core-206 drives` runs just those boots.

## What it covers

`suite.txt` lists the boots. Each one is a fresh floppy built by `mkimg.py`, with test programs in `A:\AUTO`. TOS runs them at boot, and they report through NatFeats `NF_STDERR`.

| Program | Checks |
|---|---|
| IRQ | VBL rate, HBL per frame, MFP Timers A/B/D (delay mode) against the real Timer C, Timer B event count per frame, `stop #$2300` / `stop #$2100` wake-ups, in-service blocking (software EOI) vs automatic EOI. It also measures how far into the line a Timer B handler got (the raster wobble). |
| ACIA | IKBD round trips through the keyboard ACIA (time-of-day, mouse button action): every byte, in order, once; no ACIA errors, no empty interrupts, nothing stuck. |
| YM | YM2149 registers 0-13 read back, and a speed measurement. |
| FDD | 300 KB pattern file through GEMDOS, raw `Floprd` mid-track and full-track, 64 KB write + read-back + delete. |
| IDE | Emulated IDE, driven directly: IDENTIFY capacity, reads checked sector by sector, INTRQ per sector and gone after the last, write + read-back, past-the-end refusal. |
| ACSI | Emulated ACSI through the DMA chip (the EmuTOS sequence): TEST UNIT READY, INQUIRY (LUN 1 = `$7F`), reads checked, DMA address counter after a transfer, write + read-back, past-the-end status. |
| DMASND | STE DMA sound: frame-end events at the right rate, frame counter movement, stop. |

The `drives` boot runs IDE and ACSI on fresh 8 MB pattern images, then the floppy test again on the shared DMA chip.

`tests.txt` keeps the older boots from your own disk images. A boot there passes if it runs its time with no panic, crash or early exit.

Every guest check is counted against MFP Timer C (the real chip's crystal), so the expected figures are a real ST's, whatever the emulated CPU speed. All programs pass in Hatari 2.4 (cycle-exact ST/STE) on TOS 1.04 and 2.06. The one difference between TOS versions there: Timer B event mode counts 199 lines per frame under 2.06 and 200 under 1.04, every frame.

## Files

- `run.sh`: the runner.
- `suite.txt` / `tests.txt`: what to run.
- `guest/*.S`: the test programs (68000 asm, `lib.S` shared). The built `.PRG` files are committed, so the Pi needs no m68k toolchain.
- `build.sh`: rebuilds the `.PRG` files. Needs `binutils-m68k-linux-gnu` and python3.
- `mkimg.py`: builds the floppies and the IDE/ACSI pattern images (python3, no other dependencies).
- `history.txt` / `metrics.txt` / `results/`: written by runs, not committed.

## Adding a check

1. Add it to a guest program as one `@T name PASS|FAIL detail` line (the `CHECK` macro in `lib.S`). Add any number as `@M name value`.
2. Run `tools/regress/build.sh`.
3. Check it under Hatari: build a floppy with `mkimg.py st x.st --auto guest/X.PRG --auto guest/END.PRG` and boot it with `--natfeats on`.
4. Commit the `.S` and the `.PRG` together.

## Timeline capture

`REGRESS_TIMELINE=1 tools/regress/run.sh core-104` also records the IPL timeline with level 2 (`PISTORM_IPL_TIMELINE=2`) for each boot, keeps it as `results/<commit>/<boot>.tl.gz`, and puts what `tools/ipl-timeline/hbl.py` finds in the summary: for every HBL that never reached the CPU during `irq_hbl`, whether a VBL/MFP interrupt was in the way, the CPU did not take a latched level 2, the line was seen but not latched, or the line's drop between two HBLs was too short for the confirm time or never seen. It is measurement only and changes no timing decision.
