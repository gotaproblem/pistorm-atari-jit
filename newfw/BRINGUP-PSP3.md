# PSP3 bring-up - one variable per stage, fallback at every step

Fallback everywhere: reflash pistorm_psp2 (0x2A) - the driver binary is
shared, only env defaults differ. Nothing else changes.

## Stage 0 - gate (DONE when this file was written)
`sh newfw/sim/build_and_run_psp3.sh` -> 86 checks ALL PASS, including
zero-clock strobes, 24-transaction phase sweep (exact cycle counts), and
max-pace handshake liveness.

## Stage 1 - synthesise + flash
Quartus: open `newfw/quartus/pistorm_psp3.qpf`, compile. Expect: no
inferred latches OUTSIDE the five documented front-end attribute latches
(attr_a0/rd/byte/fc, csr_wv are negedge-pi_wr FFs, not latches - the
report should show pi_wr/pi_rd as clocks; that is by design). Generate
SVF as before, flash.

## Stage 2 - first contact (same binary, old timings)
Run WITHOUT any new env: shape=8 etc. Everything must behave exactly as
0x2A did (the front end accepts wide strobes trivially).
PASS: boot print `3.0r (PSP2)`; `--p2diag` all zeros, 0 timeouts.

## Stage 3 - drop the shapes (the whole point)
```
PISTORM_P2_SHAPE=1 PISTORM_P2_BLOCK_SHAPE=1 sudo ./ataritest --p2diag
```
PASS: all zeros, timeouts 0. The toggle makes any capture miss a LOUD
timeout, never silent corruption - the counter is the safety meter.
Then `--memory tests=rwx size=512` and compare MB/s with Stage 2: expect
roughly 2-3x.

## Stage 4 - floppy stopwatch
Emulator with `PISTORM_P2_SHAPE=1 PISTORM_P2_BLOCK_SHAPE=1
PISTORM_DMA_SNOOP=1 PISTORM_DMA_WRITE_GAP_US=0 PISTORM_DMA_YIELD_US=0`.
A chained-loader track (Xenon 2) in <=0.6 s. The mirror pull at ~0.5 ms
fits every inter-sector gap with margin - rev-quantisation should be
gone for every loader style, polled or chained.

## Stage 5 - regression
Xenon 2 full load + play, Gotek boot (FF Manager stays parked - its
direction-latch quirk is a DMA-chip matter, not a bus matter), desktop,
keyboard. Then the ACSI chapter reopens with a fast bus underneath it.
