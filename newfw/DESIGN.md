# PSP2 — clean-sheet PiStorm-Atari firmware + protocol

Ground-up redesign. The previous Verilog is not referenced. The only carried
artifacts are **board facts**: pin/net names (from the .qsf, which describes
the PCB), the external '373/'374 latch datapath, and the Pi GPIO allocation.

## Design law

1. **The 68K side behaves indistinguishably from a real MC68000.** Your
   machine works perfectly with a real CPU; therefore every bus behaviour is
   resolved by asking "what does the real part do?" Address/data hold through
   DTACK, strobes released before buses, E runs continuously, arbitration per
   the datasheet.
2. **The Pi never dictates bus timing.** All Pi strobes are resynchronised
   into the 8 MHz bus-clock domain. Bus cycles run at native pace from the
   external latches. Writes are posted.
3. **Every PI_D handoff has an explicit owner and a guaranteed-valid rule.**
   No phase exists where "who is driving" is implicit or where a sampler
   races a driver. (Root cause of the 2026-08-17 corruption: the completing
   status sample doubled as the data sample with zero margin.)
4. **Nothing unsimulated is ever flashed.** The CXXRTL suite in `sim/` must
   pass before Quartus is even opened. The design is pure synchronous
   posedge logic — no latches, no multi-edge blocks, no incomplete
   sensitivity lists — so simulation and synthesis cannot diverge (root
   cause of the "passes sim, fails bench" year).
5. **Diagnostics are architecture.** Version + sticky fault flags readable
   at any time; evidence is only ever cleared by the Pi, never by machine
   reset.

## Hardware (fixed by the PCB)

```
Pi GPIO0   PI_TXN_IN_PROGRESS  CPLD->Pi   "BUSY" (redefined, see contract)
Pi GPIO1   PI_CMD_WR           Pi->CPLD   write strobe (async, resynced)
Pi GPIO2-3 PI_CMD[1:0]         Pi->CPLD   register select
Pi GPIO4   PI_CMD_RD           Pi->CPLD   read strobe  (async, resynced)
Pi GPIO5-6 PI_IPL1, PI_IPL2    CPLD->Pi   synchronised ~IPL_n[1], ~IPL_n[2]
Pi GPIO7   PI_BERR             CPLD->Pi   last cycle ended in BERR/watchdog
Pi GPIO8-23 PI_D[15:0]         bidir      shared data bus

'373 x4  address latches   clk LTCH_A_0/8/16/24, OE LTCH_A_OE_n  -> A-bus
'373 x2  write-data latches clk LTCH_D_WR_U/L,   OE LTCH_D_WR_OE_n -> D-bus
'374 x2  read-data latches  clk LTCH_D_RD_U/L,   OE LTCH_D_RD_OE_n -> PI_D
```

Key structural insight: **the external latches ARE the posted-transaction
slot.** They hold address and write data with zero CPLD flip-flops. The CPLD
holds only the attributes (FC, R/W, BYTE, A0) and a `pending` flag.

## Register map (PI_CMD)

| sel | write                       | read                       |
|-----|-----------------------------|----------------------------|
| 0   | DATA -> WR latches = **GO(write)** | read-data ack (releases RD OE) |
| 1   | ADDR_LO = A[15:0] (incl. A0)| —                          |
| 2   | ADDR_HI_CTL (below); RD=1 = **GO(read)** | —             |
| 3   | CSR control                 | CSR status = **GO(csr-read)** |

ADDR_HI_CTL: `[15:13]=FC  [12]=RD  [11]=BYTE  [10:8]=0  [7:0]=A[23:16]`

CSR write: `[0] engine soft-reset  [1] drive RESET_n low  [2] drive HALT_n
low  [3] clear sticky flags` (levels, not pulses; Pi sets then clears).

CSR read: `[7:0] FWREV=0x21  [8] sticky BERR  [9] sticky WATCHDOG
[10] sticky BGACK-seen  [11] sticky BR-seen  [12] ext master owns bus (live)
[13] DTACK_n (live, synced)  [14] BERR_n (live, synced)  [15] engine busy`

Sticky flags clear ONLY via CSR[3]. A machine RESET does not erase evidence.

## Protocol contract (PSP2)

**Pi strobe shape** (both WR and RD): high ≥ 260 ns, then low ≥ 260 ns.
Strobes are 2-FF synchronised and edge-detected at 8 MHz; 260 ns guarantees
capture. The driver enforces this with counted GPLEV readbacks, not NOPs.

**Write transaction (posted):**
1. Poll GPIO0 BUSY == 0 (plain GPLEV read, no bus transaction).
2. PI_D=A[15:0], sel=1, WR pulse.   (CPLD pulses LTCH_A_0+A_8)
3. PI_D=ADDR_HI_CTL (RD=0), sel=2, WR pulse.  (pulses LTCH_A_16+A_24,
   registers FC/BYTE/RW attributes)
4. PI_D=data, sel=0, WR pulse = GO. BUSY rises within 250 ns.
5. Pi returns to emulation immediately. The bus cycle runs at native pace;
   BUSY falls when strobes are released and holds are satisfied.

**Read transaction (blocking, guaranteed-valid):**
1–3. As above with RD=1; step 3 is GO. BUSY rises.
4. Engine runs the cycle; the '374s capture the D-bus **while AS is still
   asserted and the slave still driving** (the real 68000's sample point).
5. Engine releases the bus, enables LTCH_D_RD onto PI_D, holds it stable
   for **3 full clocks (375 ns), then drops BUSY**.
   **CONTRACT: BUSY falling edge guarantees PI_D valid and settled.** One
   GPLEV sample after observing BUSY low is correct by construction.
6. Pi pulses RD with sel=0 to acknowledge; CPLD releases RD OE within two
   clocks. Pi must not drive PI_D until 260 ns after the ack.

**CSR read:** as read, sel=3; allowed only when BUSY=0.

**PI_D ownership table** — at every instant exactly one driver:
Pi drives during steps 2–4(write)/2–3(read) strobe windows; CPLD ('374s or
CSR) drives only between BUSY-fall and the RD ack; the bus is otherwise
released. Turnaround gaps are mandated above.

**BERR/watchdog:** GPIO7 reflects "last cycle aborted" and is valid at the
same instant BUSY falls; cleared on next GO. Sampled in the same GPLEV word
as read data — no extra transaction.

## Cycle engine

Single synchronous FSM, posedge of the 8 MHz bus clock only. DTACK, BERR,
VPA, BR, BGACK all 2-FF synchronised. Cycle shape (min ~6 clocks, 750 ns —
slower than a real 68000's 500 ns; correctness first, and posted writes hide
it from the Pi):

```
IDLE  -> ADDR   FC driven, AS_n asserted; reads also assert UDS/LDS; writes
                drive RW low (data OE follows next state = data setup after
                RW, per 68K write timing)
ADDR  -> DRIVE  writes: WR-latch OE on, then strobes assert next state
DRIVE -> WAIT   sample DTACK_s each clock; watchdog counts
WAIT  -> CAP    reads: pulse LTCH_D_RD (capture while slave drives)
CAP   -> REL    negate AS/UDS/LDS
REL   -> HOLD   writes: data OE held one clock past strobe negation
                (125 ns hold — the 0.71a lesson, by design not by patch)
HOLD  -> IDLE / DRV1..DRV3 (reads: RD-OE settle window) -> BUSY falls
```

**VPA path:** VPA_n low in WAIT → assert VMA_n aligned to E low, complete at
the end of E high (ACIA/IKBD requires this). E is a free-running 10-state
divider (6 low / 4 high) that never stops — arbitration and reset do not
disturb its phase, same as the real part.

**BERR:** aborts the cycle cleanly (strobes released with the same hold
discipline), sets PI_BERR + sticky.

**Watchdog:** 1023 clocks ≈ 128 µs (double the GLUE's own 64 µs BERR) — a
cycle can never hang the machine; expiry behaves exactly like BERR and sets
its own sticky bit. It exists as a safety net, not a crutch: on a healthy
ST it must never fire, and firing is visible evidence.

## Arbitration (full, first-class)

Per MC68000 datasheet, synchronised inputs, own FSM:

```
OWN      : BR_s asserted -> wait engine cycle completion -> assert BG_n
GRANTED  : BGACK_s asserted -> negate BG_n -> EXT
           BR_s negated first -> negate BG_n -> OWN (withdrawn request)
EXT      : AS/UDS/LDS/RW/FC/VMA Hi-Z; LTCH_A_OE_n=1; LTCH_D_WR_OE_n=1.
           E keeps running. Engine start inhibited; a pending posted
           transaction WAITS here and executes after release — the Pi may
           even load the latches during EXT (their bus-side OEs are off),
           so DMA and Pi setup overlap with zero contention by design.
EXT ends : BGACK_s negated -> one recovery clock -> OWN
```

Stretched-DTACK cycle-stealing (the MMU's mechanism) is handled orthogonally
by the WAIT state: the engine simply waits, bounded by the watchdog. Both
DMA mechanisms are therefore safe by construction.

## LE budget (EPM240 = 240 LEs, no RAM blocks)

FF count: syncs (~14) + engine state (4) + attributes FC/RW/BYTE/A0 (6) +
pending/busy (3) + watchdog (10) + E (5) + arb (2) + CSR ctl (4) + sticky
(4) + strobe shapers (6) ≈ **58 FF**, comfortably inside 240 LEs with
combinational logic. Read data, address, and write data live in the external
latches — zero internal cost.

## Verification (before Quartus, always)

`sim/` builds the core through yosys→CXXRTL→g++ and runs:
T1 write cycle: strobe order, data setup/hold vs RW and strobes, hold past
   negation. T2 read cycle: capture while AS asserted, RD-OE settle ≥3 clk
   before BUSY falls. T3 DTACK stretch (cycle-steal) 0/2/16 clocks.
T4 BERR abort. T5 watchdog expiry + sticky. T6 full arbitration handover:
   mid-request during cycle, all outputs released in EXT, pending write
   executes after release. T7 VPA/E cycle. T8 back-to-back posted writes.
The core is tri-state-free (oe/out/in pairs); a 20-line top adds pads for
Quartus. CXXRTL two-state cannot lie about this design.

## Pi driver (ps_protocol_v2.c)

Same external API (`ps_read_8/16/32`, `ps_write_*`, `ps_read_status_reg`,
IPL read) — the emulator and ataritest do not change. Internally: BUSY pin
polling, contract-shaped strobes with counted-readback timing, single-sample
reads (valid by contract), posted writes. Build-selectable alongside the old
driver: `make PSP2=1`.
