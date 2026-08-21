# PSP2 bring-up - staged, one variable per step, five boots per verdict

Rules (from FINDINGS.md, honoured this time):
- ONE change per stage. A stage fails -> revert that stage only.
- Intermittent system: 5 boots per verdict, not 1.
- Every stage has a printed pass line; no stage is judged by feel.

## Stage 0 - build the gate
```
cd newfw/sim && sh build_and_run.sh        # must print ALL PASS
```
Quartus: new project, TOP_LEVEL_ENTITY `pistorm_psp2_top`, add ONLY
`newfw/rtl/pistorm_psp2.v`, carry the pin assignments over from the old
.qsf (net names match on purpose). Synthesise; note LE usage (expect well
under 240). Generate .svf.
PASS: sim ALL PASS + clean synthesis, zero inferred latches in the report.

## Stage 1 - driver swap, no flash yet (proves nothing broke)
```
cp gpio/ps_protocol.c gpio/ps_protocol_v1.c.bak
cp newfw/pi/ps_protocol_v2.c gpio/ps_protocol.c
make emulator ataritest                     # must build clean
```
Do NOT run against old firmware - the contracts differ. This stage only
proves the build.

## Stage 2 - flash + first contact
Flash the PSP2 .svf. Run `ataritest` alone:
PASS: boot print shows `PiSTorm firmware EPM240 1.1r (PSP2)`.
FAIL: wrong/garbled revision -> the CSR read contract is broken; nothing
else is worth testing. Revert flash, back to sim.

## Stage 3 - memory soak (the raw bus, no emulation)
```
sudo ./ataritest --memory tests=rwx size=512 loop=yes    # >= 30 min
```
PASS: zero errors for the whole soak. This exercises reads AND writes
through the new contract at full rate.
FAIL: capture the error pattern (bit? byte lane? address-correlated?) -
with the contract explicit, any failure here is signal integrity, and the
pattern names the line.

## Stage 4 - read-path proof (the instrument that found the old fault)
Boot emulator, `PISTORM_DMA_SNOOP=1`, and in dma_snoop set
`PISTORM_RD_SETTLE=0` explicitly (the v2 driver needs NO settle - the
firmware guarantees validity; prove it).
PASS: `vrfy: clean` on every pull, five boots.

## Stage 5 - floppy
Gotek menu image, 5 boots:
PASS: `boot checksum = $1234` on every boot, menu loads and runs.
Note: with posted writes + native-pace cycles, the loader's RAM writes
can no longer starve the DMA FIFO (the engine, not the Pi, owns bus
timing). The write-gap pacing should be unnecessary:
`PISTORM_DMA_WRITE_GAP_US=0` from the start.

## Stage 6 - ACSI
HDC attached, 5 boots: drive letters, folders POPULATED, files open.
Watch `wchk`/`hs`/`done` lines as before.

## Stage 7 - strip the mitigation stack (one per step, 3 boots each)
In this order, each proven unnecessary before removing the next:
1. `PISTORM_DMA_YIELD_US=0`   (GPIP poll yield)
2. `PISTORM_DMA_SETTLE_US=0`  (pre-pull settle)
3. stable-read loop in sync_pull (leave the vrfy CHECK in forever - it is
   the canary, it costs one extra read per byte only when snoop is on)
Anything that turns out to still be needed is a real finding about PSP2 -
record it, don't paper it.

## Stage 8 - regression freeze
Five clean boots of: EmuTOS desktop + Gotek + ACSI + keyboard + sound.
Tag the repo. Record LE count, FWREV, and the five-boot log in notes.

## If a stage fails
The failure IS the data. Capture the log, revert only that stage, and
bring the failure back - with the sim gate and the explicit contract, a
bench failure now localises to a handful of nameable causes instead of a
year of fog.
