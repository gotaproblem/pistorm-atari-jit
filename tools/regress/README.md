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
| CPU / CPU16 | The governed 68000's speed, 1 s of a register loop against Timer C: within 2% of 8000 / 16000 kHz (`cpu_speed 8mhz` / `16mhz`). Measures, without a verdict, 1 s of long writes into the screen (`scr_kbs`, real-bus bound) and of MFP GPIP reads (`io_kreads`). The loop uses only instructions whose cycle counts are multiples of 4, so a real ST, which stretches the others to the next bus slot, and the governor, which counts the Motorola tables, agree on it. Hatari 2.4 cycle-exact: ST 7949 kHz, STE at 16 MHz 15971 kHz. |

The `drives` boot runs IDE and ACSI on fresh 8 MB pattern images, then the floppy test again on the shared DMA chip.

`tests.txt` keeps the boots from your own disk images. A boot there passes if it runs its time with no panic, crash or early exit, and every check in its checks column holds. Those checks read the emulator's own reports, which the runner turns on for these boots:

| Check | Holds when |
|---|---|
| `vbl=50` | every `[ipl] vbl: taken=` second after the first 15 is 49-51 |
| `gov>=K` | every `[GOV]` 5 s window after the first two reached K kHz |
| `noov` | no `[dmasnd] STE stream overflow` (sound never queued faster than real time) |
| `snd` | after 10 s of DMA sound, no second with the output queue empty while a frame plays |
| `shmcal` | the HDMI mirror's last three `[SHM]` summaries each accepted a video counter read (cal=0 was FastBobs's mirror drawing the wrong buffer) |

Each check also leaves a measurement (`vbl_min`/`vbl_max`, `gov_min_khz`, `snd_overflows`, `snd_starved_s`, `shm_cal_zero`) for the run-to-run comparison.

A tests.txt boot can also start a program off a hard disk image on its own (the `start` column). The runner builds the boot files twice, as a small hard disk on ACSI ID 0 (C:) and as a floppy in A:, and puts the image on ACSI ID 1 (D:) in place of the section's drives. Both copies are needed because EmuTOS boots from C: on a cold start whenever a hard disk exists, keeps the old `_bootdev` (A: after Hatari's fast boot) on a warm one, and runs only the boot drive's AUTO folder, `EMUDESK.INF` and accessories. The `extra cfg` column adds cfg lines to the section; a key given there replaces the section's own line for it.

| Directive | Floppy gets |
|---|---|
| `auto PATH [ARGS]` | `\AUTO\LAUNCH.PRG` plus `\RUN.INF` (PATH, ARGS). LAUNCH changes to PATH's folder and runs it with `Pexec`, and fails (`@T launch FAIL`) if the program returns. For TOS programs: FastBobs and Bad Apple. |
| `desk PATH` | `\EMUDESK.INF` with `#Z 01 PATH@`: the EmuTOS desktop autostarts PATH. For GEM programs, which need the AES. |
| `acc NAME ARG` | `guest/NAME.PRG` as `\NAME.ACC`, with ARG in `\NAME.INF`. `MPSEND` waits for Paula to start, then sends it `MP_START` with module ARG. Paula 2.6 and its `MP.PRG` crash on a 68000 when given a command line (a `free()` of a string literal), so the module is passed by message. |

`fastbobs`, `badapple` and `modplay` run from `dkimages/regression.img` this way, on the `[regression]` section of psctrl.cfg: a machine kept for these tests only (STE, EmuTOS 1.3, the governed 8 MHz 68000, native_hdmi, DMA sound, blitter), so the sections you change day to day never move the results. `psctrl-regression.cfg` here is that section; append it to `../configs/psctrl.cfg` once. All three start in Hatari 2.4 (EmuTOS 1.3, STE) from the disks the runner builds, booting from C: (cold) and from A: (fast boot).

Every guest check is counted against MFP Timer C (the real chip's crystal), so the expected figures are a real ST's, whatever the emulated CPU speed. All programs pass in Hatari 2.4 (cycle-exact ST/STE) on TOS 1.04 and 2.06. The one difference between TOS versions there: Timer B event mode counts 199 lines per frame under 2.06 and 200 under 1.04, every frame.

## Files

- `run.sh`: the runner.
- `suite.txt` / `tests.txt`: what to run.
- `guest/*.S`: the test programs (68000 asm, `lib.S` shared). `LAUNCH` and `MPSEND` are the starters for tests.txt; a program sets `post_user` in `lib.S` to run code in user mode after its supervisor part. The built `.PRG` files are committed, so the Pi needs no m68k toolchain.
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
