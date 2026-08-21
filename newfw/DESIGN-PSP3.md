# PSP3 - asynchronous front end on the proven PSP2 engine

## Why

PSP2's design law "one 8 MHz clock, everything synchronised" produced the
first provably clean bus this project has had (0 errors across hundreds of
thousands of transactions, working 3-wire arbitration, perfect DMA). It
also made every Pi transaction cost 5-8 us: each strobe must be *held*
long enough for a 125 ns-per-stage synchroniser to see it, so the driver
burns "shape" readbacks on every phase. Consequences measured on the
bench:

- mirror pull = 512 bytes ~ 1.5 ms, vs a floppy inter-sector gap of
  ~1.4 ms -> one missed header = one lost 200 ms revolution -> 2 s/track
  floppy loads (9 revs/track instead of ~2).
- any future live-RAM execution is off the table at 5+ us/access.

The old firmware was fast because its Pi-facing logic was *asynchronous*
(combinational latch gates, unclocked capture) - and unreliable because
nothing disciplined the crossing into the c8m engine (the 5-13% dirty
reads, missed strobes, phantom IPLs all lived on that seam). The fast
PI_CLK pin is commented out in the board qsf - there is no fast clock to
lean on, and there never was one in service.

PSP3 = the old firmware's async speed + PSP2's engine + a textbook CDC
on the one seam that ever mattered.

## Architecture

Three zones, one crossing:

```
 Pi GPIO  ->  [ ASYNC FRONT END ]  --CDC-->  [ PSP2 ENGINE @ 8MHz ]  -> 68k bus
              latch gates, attrs             UNCHANGED: S4 strobes,
              GO/ack flags                   split-edge negation, arb,
                                             watchdog, toggle completion
```

### Zone 1: async front end (no clock)

* **'373 gates are wires.** `LTCH_A_0/8 = (PI_CMD==ADDR_LO) & PI_WR`,
  similarly ADDR_HI and DATA. The Pi drives both the data lanes and the
  strobe, so data-vs-gate timing is the Pi's own program order - the
  same contract the external latches have always lived by. Gate width =
  strobe width = whatever the Pi holds (one readback, ~100-200 ns, is
  plenty for a '373/'374; no 8 MHz sampling is involved).
* **Attribute latches** (rd/byte/fc/a23:16 from ADDR_HI, a0 from
  ADDR_LO): transparent while the strobe is high, closed at strobe fall.
  Deliberate latch inference, confined to this zone and documented -
  the Pi guarantees data stability through the strobe, exactly as it
  does for the external '373s.
* **Request flags**, one per event, set by the strobe edge itself
  (async-set FF clocked by PI_WR/PI_RD fall so attributes are already
  closed): `go_req` (DATA write, or ADDR_HI write with RD=1),
  `csr_req` (RD pulse sel 3), `ack_req` (RD pulse sel 0). A flag
  catches ANY strobe width - the entire "missed strobe" class dies here.

### Zone 2: the crossing (the only new thing that must be right)

Classic single-bit req/clear handshake per flag:

```
front end:  req  set by Pi strobe (async)
engine:     req_s <= 2FF sync of req; on req_s: consume, pulse clr
front end:  FF cleared by clr (async clear); Pi may then send the next
```

Ordering guarantee: attrs close at the strobe fall; `go_req` sets at the
same fall; the engine sees `go_req` two 8 MHz clocks later at the
earliest - attrs are static long before use (quasi-static crossing).
The Pi cannot legally issue a new phase before completion (toggle
contract, unchanged), so req/clr can never race a second request.

* Completion stays a **toggle** on PI_TXN_IN_PROGRESS - unchanged, still
  unmissable at any observer latency, still carries the data-valid
  guarantee (flipped after the 3-clock RD-OE settle).
* BERR still rides bit 7 of the completion sample. CSR, debug pages,
  grant telemetry: unchanged.

### Zone 3: engine - byte-identical to PSP2 0x2A

Everything from dispatch to completion is the code that measured
perfect: S2/S3/S4 write shape, split-edge strobe negation, 2-wire+3-wire
arbitration with release-on-grant, 128 us watchdog, POR block. The only
edits are where `wr_edge/rd_edge` decode used to live (replaced by the
synced request flags).

## What it buys (honest numbers)

| item                    | PSP2 0x2A     | PSP3 target |
|-------------------------|---------------|-------------|
| phase strobe cost (Pi)  | ~0.6-1.3 us   | ~0.15 us    |
| read transaction        | 5-8 us        | ~2-2.5 us (engine-floor bound) |
| posted write (Pi cost)  | ~2-3 us       | ~0.8 us     |
| 512-byte mirror pull    | ~1.5 ms       | ~0.55 ms    |
| floppy track (chained)  | ~2 s          | ~0.45 s     |

The engine's ~1.5-1.9 us bus-cycle latency (dispatch + DTACK sync +
settle) is the new floor; the front end stops being the cost. Live-RAM
execution at ~2 us/access remains 2-3x slower than a real 68000 - PSP3
does NOT promise cycle-exact live-RAM gaming. What it promises is the
mirror tax becoming invisible: every pull fits every gap with 2x margin.

## Verification plan (nothing flashes before ALL PASS)

1. Full PSP2 suite (74 checks) ported - engine invariants must hold
   bit-for-bit.
2. **Phase sweep**: every strobe driven at every alignment vs the 8 MHz
   clock (sub-clock offsets), 0 lost / 0 doubled GOs, attrs never torn.
3. **Minimum-width strobes**: 1-cycle-equivalent pulses must register
   (flag semantics), verifying the driver may drop shape loops.
4. Handshake liveness: req->clr round trip bounded; back-to-back
   transactions at maximum driver pace.
5. Yosys `check -assert`: the ONLY intentional latches are the five
   front-end attribute latches (whitelisted by name).

## Bench plan

Stage A: flash, revision prints 3.0r, `--p2diag` all zeros with
PISTORM_P2_SHAPE=1. Stage B: memory soak. Stage C: floppy stopwatch -
a chained-loader track in <=0.6 s. Stage D: Xenon 2 + Gotek regression,
ACSI later. Fallback at every stage: reflash 0x2A, driver v2 (kept).
