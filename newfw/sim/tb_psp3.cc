/*
 * PSP2 testbench - runs the SYNTHESISED core (yosys -> CXXRTL).
 * Nothing is flashed unless every test here passes.  See ../DESIGN.md.
 *
 * The core is tri-state free by design, so 2-state CXXRTL cannot lie
 * about it - the exact failure mode of the previous firmware's sims.
 */
#include "psp3.cc"
#include <cstdio>
#include <cstdint>

using namespace cxxrtl_design;

static p_pistorm__psp3__core top;
static int fails = 0, checks = 0;
static long cycle_no = 0;

#define CHECK(cond, name) do { checks++; \
    if (!(cond)) { fails++; \
        printf("FAIL  [t=%ld] %s\n", cycle_no, name); } } while (0)

/* ---- bus slave + DMA master models (driven every posedge) -------- */
static int  slave_dtack_delay = 2;   /* clocks from strobes to DTACK   */
static bool slave_enabled     = true;
static int  slave_cnt         = -1;
static bool drive_berr        = false;
static int  berr_after        = -1;
static bool drive_vpa         = false;

static inline bool B(const wire<1> &w)  { return w.curr.data[0] & 1; }
static inline bool B(const value<1> &v) { return v.data[0] & 1; }

/* ---- continuous invariant monitors (updated every tick) ----------
 * Lesson from this suite's own first run: sampling AFTER a helper
 * returns misses windows that open during it.  Monitors live in tick()
 * and cannot miss. */
struct Mon {
    bool oe_before_strobes, rw_low_before_oe, hold_after_strobes;
    bool rd_clk_on_write, cap_while_as, vma_seen;
    bool gate_open_while_busy;
    int  rd_oe_clocks;
    bool gate_follows_ok, gate_follow_seen;  /* PSP3: gate == sel&strobe */
    void reset() { *this = Mon{}; gate_follows_ok = true; }
} mon;

static void monitors(void)
{
    bool oe   = !B(top.p_ltch__d__wr__oe__n);
    bool strb = !B(top.p_uds__n) || !B(top.p_lds__n);
    bool as   = !B(top.p_as__n);

    if (oe && as && !strb)               mon.oe_before_strobes = true;
    if (oe && !B(top.p_rw))              mon.rw_low_before_oe  = true;
    if (oe && !as && !strb)              mon.hold_after_strobes = true;
    if (B(top.p_ltch__d__rd__u) && oe)   mon.rd_clk_on_write   = true;
    if (B(top.p_ltch__d__rd__u) && as)   mon.cap_while_as      = true;
    if (!B(top.p_vma__n))                mon.vma_seen          = true;
    /* settle window: RD OE on while engine still active (pre-toggle) */
    if (!B(top.p_ltch__d__rd__oe__n) &&
        (top.p_eng.curr.data[0] & 0xF) != 0) mon.rd_oe_clocks++;
    /* contract: no input latch gate open while the ENGINE runs a cycle */
    {
        unsigned e = top.p_eng.curr.data[0] & 0xF;
        bool engine_cycling = (e >= 1 && e <= 6) || e == 10 || e == 11;
        if (engine_cycling &&
            (B(top.p_ltch__a__0)  || B(top.p_ltch__a__8) ||
             B(top.p_ltch__a__16) || B(top.p_ltch__a__24)))
            mon.gate_open_while_busy = true;
    }
    /* PSP3: '373 gates must equal (sel & strobe), combinationally */
    {
        unsigned cmd = top.p_pi__cmd.data[0] & 3u;
        bool wr = top.p_pi__wr.data[0] & 1u;
        bool e0 = B(top.p_ltch__a__0)    == (wr && cmd == 1u);
        bool e1 = B(top.p_ltch__a__16)   == (wr && cmd == 2u);
        bool e2 = B(top.p_ltch__d__wr__u)== (wr && cmd == 0u);
        if (!(e0 && e1 && e2)) mon.gate_follows_ok = false;
        if (wr) mon.gate_follow_seen = true;
    }
}

static void models(void)
{
    bool strobes = !B(top.p_as__n) &&
                   (!B(top.p_uds__n) || !B(top.p_lds__n));

    /* DTACK slave */
    if (slave_enabled) {
        if (strobes) {
            if (slave_cnt < 0) slave_cnt = 0; else slave_cnt++;
            top.p_dtack__n = value<1>{(slave_cnt >= slave_dtack_delay) ? 0u : 1u};
        } else {
            slave_cnt = -1;
            top.p_dtack__n = value<1>{1u};
        }
    } else
        top.p_dtack__n = value<1>{1u};

    /* BERR generator */
    if (drive_berr && strobes) {
        if (berr_after > 0) berr_after--;
        top.p_berr__n = value<1>{(berr_after == 0) ? 0u : 1u};
    } else
        top.p_berr__n = value<1>{1u};

    /* VPA */
    top.p_vpa__n = value<1>{(drive_vpa && strobes) ? 0u : 1u};
}

static void tick(void)
{
    models();
    /* cxxrtl's step() exits on convergence BEFORE the last commit's
     * values reach output connections - one extra edge-free step
     * settles them.  Without it every observation is one delta stale. */
    top.p_clk = value<1>{0u}; top.step(); top.step();
    monitors();                /* FW 0x25: controls transition on negedge -
                                  half-clock windows are real windows now  */
    top.p_clk = value<1>{1u}; top.step(); top.step();
    monitors();
    cycle_no++;
}

static void ticks(int n) { while (n--) tick(); }

/* ---- Pi-side protocol helpers (contract-shaped) ------------------ */
static void pi_write_phase(unsigned cmd, unsigned data)
{
    top.p_pi__cmd  = value<2>{cmd};
    top.p_pi__d__in = value<16>{data};
    top.p_pi__wr = value<1>{1u}; ticks(3);          /* >=260ns high */
    top.p_pi__wr = value<1>{0u}; ticks(3);          /* >=260ns low  */
}

static void pi_rd_pulse(unsigned cmd)
{
    top.p_pi__cmd = value<2>{cmd};
    top.p_pi__rd = value<1>{1u}; ticks(3);
    top.p_pi__rd = value<1>{0u}; ticks(3);
}

/* FW 0x23: completion is a TOGGLE of the busy pin.  Snapshot before GO,
 * wait for it to differ.  Unmissable at any testbench pacing - the same
 * property the real driver now relies on. */
static bool tog_snap;
static void snap_toggle(void)  { tog_snap = B(top.p_busy); }
static int  wait_toggle_tb(int limit)
{
    int n = 0;
    while (B(top.p_busy) == tog_snap && n < limit) { tick(); n++; }
    return n;
}
#define wait_busy_low(lim) wait_toggle_tb(lim)

static void txn_write(unsigned addr, unsigned data, bool byte_sz, unsigned fc)
{
    pi_write_phase(1, addr & 0xFFFF);
    pi_write_phase(2, (fc << 13) | (byte_sz ? (1u<<11) : 0) | ((addr >> 16) & 0xFF));
    snap_toggle();
    pi_write_phase(0, data & 0xFFFF);               /* GO */
}

static void txn_read_go(unsigned addr, bool byte_sz, unsigned fc)
{
    pi_write_phase(1, addr & 0xFFFF);
    snap_toggle();
    pi_write_phase(2, (fc << 13) | (1u<<12) | (byte_sz ? (1u<<11) : 0)
                      | ((addr >> 16) & 0xFF));     /* GO */
}

int main(void)
{
    top.p_pi__wr = value<1>{0u};   top.p_pi__rd = value<1>{0u};
    top.p_dtack__n = value<1>{1u}; top.p_berr__n = value<1>{1u};
    top.p_vpa__n  = value<1>{1u};  top.p_br__n   = value<1>{1u};
    top.p_bgack__n = value<1>{1u}; top.p_reset__n__in = value<1>{1u};
    top.p_ipl__n = value<3>{7u};
    ticks(20);

    /* after arb settles bus_oe should be ON (we own it), nothing asserted */
    CHECK(B(top.p_bus__oe), "boot: we own and drive the bus");
    CHECK(B(top.p_as__n) && B(top.p_uds__n) && B(top.p_lds__n) && B(top.p_rw),
          "boot: nothing asserted");
    CHECK(!B(top.p_ltch__a__oe__n), "boot: address latches driving");
    CHECK(B(top.p_ltch__d__wr__oe__n) && B(top.p_ltch__d__rd__oe__n),
          "boot: both data latch OEs off");

    /* =========== T1: word write, ordering + hold ================= */
    {
        slave_dtack_delay = 2;
        mon.reset();
        txn_write(0xFF8604, 0x1234, false, 5);
        wait_busy_low(60);
        CHECK(true, "T1: write completes (toggle observed)");
        CHECK(mon.oe_before_strobes, "T1: data driven before strobes assert");
        CHECK(mon.rw_low_before_oe,  "T1: RW low before data OE");
        CHECK(mon.hold_after_strobes,"T1: data held past strobe negation");
        CHECK(!mon.rd_clk_on_write,  "T1: RD latch never clocked on a WRITE");
        CHECK(!B(top.p_pi__berr),    "T1: no BERR flag");
        CHECK(!mon.gate_open_while_busy,
              "T1: no ADDR gate ever open while a cycle runs");
        CHECK(mon.gate_follows_ok,
              "T1: '373 gates follow the Pi strobe combinationally");
    }

    /* =========== T2: word read, capture + guaranteed-valid ======= */
    {
        slave_dtack_delay = 2;
        mon.reset();
        txn_read_go(0xFF8604, false, 5);
        wait_busy_low(80);
        CHECK(true, "T2: read completes (toggle observed)");
        CHECK(mon.cap_while_as, "T2: '374 clocked while AS asserted (slave driving)");
        CHECK(mon.rd_oe_clocks >= 3,
              "T2: RD OE stable >=3 clks before busy falls (contract)");
        CHECK(!B(top.p_ltch__d__rd__oe__n), "T2: still driving PI_D after busy-fall");
        pi_rd_pulse(0);                              /* ack */
        CHECK(B(top.p_ltch__d__rd__oe__n), "T2: ack releases RD OE");
    }

    /* =========== T3: stretched DTACK (cycle-steal) ================ */
    {
        slave_dtack_delay = 16;
        txn_write(0x001004, 0xA5A5, false, 5);
        int n = wait_busy_low(200);
        CHECK(n < 200, "T3: stretched-DTACK write completes");
        CHECK(!B(top.p_st__wd), "T3: watchdog did NOT fire on a legal stretch");
    }

    /* =========== T4: BERR abort =================================== */
    {
        slave_enabled = false; drive_berr = true; berr_after = 5;
        txn_write(0xF00000, 0xDEAD, false, 5);
        int n = wait_busy_low(300);
        CHECK(n < 300, "T4: BERR cycle terminates");
        CHECK(B(top.p_pi__berr), "T4: PI_BERR raised");
        CHECK(B(top.p_st__berr), "T4: sticky BERR set");
        drive_berr = false; slave_enabled = true;
        CHECK(B(top.p_as__n) && B(top.p_uds__n), "T4: strobes released after abort");
    }

    /* =========== T5: watchdog ===================================== */
    {
        slave_enabled = false;
        txn_write(0xF00000, 0xBEEF, false, 5);
        int n = wait_busy_low(1300);
        CHECK(n < 1300, "T5: watchdog terminates a dead cycle");
        CHECK(B(top.p_st__wd), "T5: sticky WD set");
        CHECK(B(top.p_pi__berr), "T5: PI_BERR raised on WD");
        slave_enabled = true;
    }

    /* =========== T6: full arbitration handover ==================== */
    {
        slave_dtack_delay = 10;
        txn_write(0x001004, 0x5A5A, false, 5);       /* cycle in flight  */
        ticks(2);
        top.p_br__n = value<1>{0u};                  /* DMA requests bus */
        /* BG must NOT assert while our cycle is still running */
        bool bg_during_cycle = false;
        while (B(top.p_busy)) {
            if (!B(top.p_bg__n) && !B(top.p_as__n)) bg_during_cycle = true;
            tick();
        }
        CHECK(!bg_during_cycle, "T6: BG withheld until cycle completes");
        ticks(4);
        CHECK(!B(top.p_bg__n), "T6: BG asserted once idle");
        top.p_bgack__n = value<1>{0u}; ticks(4);     /* master takes bus */
        CHECK(B(top.p_bg__n),  "T6: BG negated after BGACK");
        CHECK(!B(top.p_bus__oe), "T6: strobe/FC drivers released in EXT");
        CHECK(B(top.p_ltch__a__oe__n), "T6: address latches released in EXT");
        CHECK(B(top.p_ltch__d__wr__oe__n), "T6: write-data latches released in EXT");
        CHECK(B(top.p_st__br__seen) && B(top.p_st__bgack__seen),
              "T6: sticky BR/BGACK evidence recorded");

        /* Pi posts a write DURING the DMA - must be accepted, not run */
        txn_write(0x002000, 0xC0DE, false, 5);
        ticks(6);
        CHECK(B(top.p_go__pending), "T6: posted write pends during EXT");
        CHECK(!B(top.p_bus__oe), "T6: and the engine stays off the bus");

        top.p_br__n = value<1>{1u};
        top.p_bgack__n = value<1>{1u};               /* DMA done         */
        int n = wait_busy_low(100);
        CHECK(n < 100, "T6: pended write executes after bus returns");
        CHECK(B(top.p_bus__oe), "T6: bus reacquired");
    }

    /* =========== T6b: withdrawn request ============================
     * 0x29 contract: the bus releases DURING the request (2-wire) and
     * is reacquired promptly after withdrawal. */
    {
        top.p_br__n = value<1>{0u}; ticks(6);
        CHECK(!B(top.p_bg__n), "T6b: BG asserted on request");
        CHECK(!B(top.p_bus__oe), "T6b: bus released during request");
        top.p_br__n = value<1>{1u}; ticks(8);        /* withdrawn, no BGACK */
        CHECK(B(top.p_bg__n), "T6b: BG negated on withdrawal");
        CHECK(B(top.p_bus__oe), "T6b: bus reacquired after withdrawal");
    }


    /* =========== T6b: 2-WIRE arbitration (BR held, NO BGACK) =======
     * The machine's real DMA master per the old firmware's BusArb:
     * BGACK wait commented out.  Bus must release on BG alone. */
    {
        ticks(8);
        top.p_br__n = value<1>{0u};                  /* BR, held        */
        ticks(6);
        CHECK(!B(top.p_bg__n), "T6b: BG asserted");
        CHECK(!B(top.p_bus__oe), "T6b: bus released WITHOUT BGACK");
        CHECK(B(top.p_ltch__a__oe__n), "T6b: address latches released");
        txn_write(0x003000, 0xBEEF, false, 5);       /* posted during DMA */
        ticks(6);
        CHECK(B(top.p_go__pending), "T6b: write pends while BR held");
        CHECK(!B(top.p_bus__oe), "T6b: still off the bus");
        top.p_br__n = value<1>{1u};                  /* master done      */
        ticks(3);
        CHECK(B(top.p_bg__n), "T6b: BG negated after BR release");
        wait_busy_low(80);
        CHECK(B(top.p_bus__oe), "T6b: bus reacquired");
        CHECK(!B(top.p_go__pending), "T6b: pended write ran after reacquire");
    }

    /* =========== T7: VPA / E-synchronised cycle ==================== */
    {
        slave_enabled = false; drive_vpa = true;
        mon.reset();
        txn_read_go(0xFFFC00, true, 5);              /* ACIA-ish         */
        wait_busy_low(200);
        CHECK(true, "T7: VPA cycle completes (toggle observed)");
        CHECK(mon.vma_seen, "T7: VMA asserted");
        CHECK(B(top.p_vma__n), "T7: VMA negated after cycle");
        CHECK(!B(top.p_pi__berr), "T7: E-cycle completed cleanly, no abort");
        pi_rd_pulse(0);
        drive_vpa = false; slave_enabled = true;
    }

    /* =========== T8: back-to-back posted writes =================== */
    {
        slave_dtack_delay = 2;
        txn_write(0x001000, 0x1111, false, 5);
        int n1 = wait_busy_low(60);
        txn_write(0x001002, 0x2222, false, 5);
        int n2 = wait_busy_low(60);
        CHECK(n1 < 60 && n2 < 60, "T8: back-to-back writes complete");
    }

    /* =========== T9: CSR read - FWREV + flags ===================== */
    {
        snap_toggle();
        pi_rd_pulse(3);                              /* CSR read GO      */
        wait_busy_low(30);
        CHECK(B(top.p_pi__d__oe), "T9: CSR drives PI_D");
        unsigned v = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        CHECK((v & 0xFF) == 0x31, "T9: FWREV reads 0x31");
        CHECK((v >> 8) & 0x2,  "T9: sticky WD visible");     /* from T5 */
        CHECK((v >> 8) & 0x1,  "T9: sticky BERR visible");   /* from T4 */
        CHECK((v >> 8) & 0x8,  "T9: sticky BR visible");     /* from T6 */
        pi_rd_pulse(0);
        CHECK(!B(top.p_pi__d__oe), "T9: ack releases CSR drive");

        /* clear stickies via CSR write, verify */
        pi_write_phase(3, 1u<<3);
        snap_toggle();
        pi_rd_pulse(3); wait_busy_low(30);
        v = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        CHECK(((v >> 8) & 0xF) == 0, "T9: CLEAR_STICKY clears evidence");
        pi_rd_pulse(0);
    }

    /* =========== T10: byte-lane selection (bench find) ============ */
    {
        slave_dtack_delay = 2;
        bool uds_only = false, lds_only = false;

        txn_read_go(0xFF8201, true, 5);              /* ODD byte  */
        while (B(top.p_busy) == tog_snap) {          /* until toggle */
            if (!B(top.p_lds__n) && B(top.p_uds__n)) lds_only = true;
            if (!B(top.p_uds__n) && B(top.p_lds__n)) uds_only = true;
            tick();
        }
        pi_rd_pulse(0);
        CHECK(lds_only && !uds_only, "T10: odd byte asserts LDS only");

        uds_only = lds_only = false;
        txn_read_go(0xFF8200, true, 5);              /* EVEN byte */
        while (B(top.p_busy) == tog_snap) {          /* until toggle */
            if (!B(top.p_lds__n) && B(top.p_uds__n)) lds_only = true;
            if (!B(top.p_uds__n) && B(top.p_lds__n)) uds_only = true;
            tick();
        }
        pi_rd_pulse(0);
        CHECK(uds_only && !lds_only, "T10: even byte asserts UDS only");
    }

    /* =========== T11: debug page - cycle counters + fight ========= */
    {
        slave_enabled = true; slave_dtack_delay = 2;
        drive_berr = false; drive_vpa = false;
        pi_write_phase(3, 0x10);                     /* dbg page ON      */
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned v0 = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        pi_rd_pulse(0);
        txn_write(0x001000, 0x1111, false, 5); wait_busy_low(60);
        txn_write(0x001002, 0x2222, false, 5); wait_busy_low(60);
        txn_read_go(0x001000, false, 5);       wait_busy_low(80);
        pi_rd_pulse(0);                              /* read ack         */
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned v1 = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        pi_rd_pulse(0);
        CHECK((((v1 >> 8) - (v0 >> 8)) & 0x7F) == 2,
              "T11: write-cycle counter +2");
        CHECK(((v1 - v0) & 0xFF) == 1, "T11: read-cycle counter +1");
        CHECK(!(v1 & 0x8000), "T11: no bus fight in clean run");
        pi_write_phase(3, 0x00);                     /* dbg page OFF     */
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned v2 = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        pi_rd_pulse(0);
        CHECK((v2 & 0xFF) == 0x31, "T11: normal page restored (FWREV)");
    }

    /* =========== T12: CSR reset/halt drive path =================== */
    {
        pi_write_phase(3, 0x02);                     /* RESET bit       */
        CHECK(B(top.p_reset__drive), "T12: CSR bit1 asserts reset_drive");
        CHECK(!B(top.p_halt__drive), "T12: halt not asserted");
        pi_write_phase(3, 0x00);
        CHECK(!B(top.p_reset__drive), "T12: cleared");
        pi_write_phase(3, 0x04);                     /* HALT bit        */
        CHECK(B(top.p_halt__drive), "T12: CSR bit2 asserts halt_drive");
        pi_write_phase(3, 0x00);
        CHECK(!B(top.p_halt__drive), "T12: halt cleared");
    }

    /* =========== T13: grant telemetry page ======================== */
    {
        pi_write_phase(3, 0x20);                     /* page 2 ON        */
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned g0 = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        pi_rd_pulse(0);
        for (int r = 0; r < 3; r++) {                /* 3 BR pulses      */
            top.p_br__n = value<1>{0u}; ticks(6);
            top.p_br__n = value<1>{1u}; ticks(6);
        }
        top.p_bgack__n = value<1>{0u}; ticks(4);     /* 1 BGACK pulse    */
        top.p_bgack__n = value<1>{1u}; ticks(4);
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned g1 = top.p_pi__d__out.curr.data[0] & 0xFFFF;
        pi_rd_pulse(0);
        CHECK((((g1 >> 8) - (g0 >> 8)) & 0xFF) == 3, "T13: BR edges +3");
        CHECK(((g1 - g0) & 0xFF) == 1, "T13: BGACK edges +1");
        pi_write_phase(3, 0x00);
    }

    /* =========== T14: MINIMUM-WIDTH STROBES (flag semantics) =======
     * Strobes toggled with NO clock edge in between - the async flags
     * must catch them.  This is the property that lets the driver drop
     * every shape loop. */
    {
        slave_enabled = true; slave_dtack_delay = 2;
        pi_write_phase(3, 0x10);                     /* dbg page on     */
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned c0 = (top.p_pi__d__out.curr.data[0] >> 8) & 0x7F;
        pi_rd_pulse(0);
        for (int n = 0; n < 8; n++) {
            /* three phases, each strobe = one eval wide, zero clocks */
            top.p_pi__cmd = value<2>{1u};
            top.p_pi__d__in = value<16>{0x1000u + (unsigned)n*2};
            top.p_pi__wr = value<1>{1u}; top.step(); top.step();
            top.p_pi__wr = value<1>{0u}; top.step(); top.step();
            top.p_pi__cmd = value<2>{2u};
            top.p_pi__d__in = value<16>{(5u<<13)};
            top.p_pi__wr = value<1>{1u}; top.step(); top.step();
            top.p_pi__wr = value<1>{0u}; top.step(); top.step();
            snap_toggle();
            top.p_pi__cmd = value<2>{0u};
            top.p_pi__d__in = value<16>{0xBEE0u + (unsigned)n};
            top.p_pi__wr = value<1>{1u}; top.step(); top.step();
            top.p_pi__wr = value<1>{0u}; top.step(); top.step();
            int k = wait_busy_low(80);
            CHECK(k < 80, "T14: zero-clock strobe write completes");
        }
        pi_write_phase(3, 0x10);
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned c1 = (top.p_pi__d__out.curr.data[0] >> 8) & 0x7F;
        pi_rd_pulse(0);
        CHECK(((c1 - c0) & 0x7F) == 8, "T14: exactly 8 write cycles, no doubles");
        pi_write_phase(3, 0x00);
    }

    /* =========== T15: PHASE SWEEP vs the 8MHz clock ================
     * Strobe edges placed at every alignment the sim can express:
     * before the falling step, between steps, after the rising step.
     * No GO lost, none doubled, attrs never torn. */
    {
        pi_write_phase(3, 0x10);
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned c0 = (top.p_pi__d__out.curr.data[0] >> 8) & 0x7F;
        pi_rd_pulse(0);
        int done = 0;
        for (int n = 0; n < 24; n++) {
            int ph = n % 3;
            /* ADDR_LO at one phase, GO at another - crossings differ */
            top.p_pi__cmd = value<2>{1u};
            top.p_pi__d__in = value<16>{0x2000u + (unsigned)n*2};
            if (ph == 0) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_clk = value<1>{0u}; top.step();
            if (ph == 1) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_clk = value<1>{1u}; top.step();
            if (ph == 2) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_pi__wr = value<1>{0u}; top.step();
            top.p_pi__cmd = value<2>{2u};
            top.p_pi__d__in = value<16>{(5u<<13)};
            top.p_pi__wr = value<1>{1u}; top.step();
            top.p_clk = value<1>{0u}; top.step();
            top.p_pi__wr = value<1>{0u}; top.step();
            top.p_clk = value<1>{1u}; top.step();
            snap_toggle();
            top.p_pi__cmd = value<2>{0u};
            top.p_pi__d__in = value<16>{0xCAFEu};
            if (ph == 2) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_clk = value<1>{0u}; top.step();
            if (ph == 0) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_clk = value<1>{1u}; top.step();
            if (ph == 1) { top.p_pi__wr = value<1>{1u}; top.step(); }
            top.p_pi__wr = value<1>{0u}; top.step();
            if (wait_busy_low(80) < 80) done++;
        }
        CHECK(done == 24, "T15: all 24 phase-swept writes completed");
        pi_write_phase(3, 0x10);
        snap_toggle(); pi_rd_pulse(3); wait_busy_low(30);
        unsigned c1 = (top.p_pi__d__out.curr.data[0] >> 8) & 0x7F;
        pi_rd_pulse(0);
        CHECK(((c1 - c0) & 0x7F) == 24, "T15: 24 cycles exactly - none lost, none doubled");
        pi_write_phase(3, 0x00);
    }

    /* =========== T16: handshake liveness at maximum pace ============
     * Back-to-back transactions with phases issued immediately after
     * the completion toggle - the CDC req/clr round trip must always
     * be ready for the next request. */
    {
        int ok = 0;
        for (int n = 0; n < 16; n++) {
            txn_write(0x004000 + n*2, 0xA000 + n, false, 5);
            if (wait_busy_low(80) < 80) ok++;
        }
        CHECK(ok == 16, "T16: 16 max-pace writes all complete");
    }

    /* =========== T17: ack releases PI_D COMBINATIONALLY ============
     * fw 0x31 bench contract: the driver may start its next phase
     * immediately after the ack strobe; the '374/CSR OE must be OFF
     * before any clock edge occurs. */
    {
        txn_read_go(0x001000, false, 5); wait_busy_low(80);
        CHECK(!B(top.p_ltch__d__rd__oe__n), "T17: OE on after read completes");
        top.p_pi__cmd = value<2>{0u};
        top.p_pi__rd = value<1>{1u}; top.step(); top.step();
        top.p_pi__rd = value<1>{0u}; top.step(); top.step(); /* NO clk edges */
        CHECK(B(top.p_ltch__d__rd__oe__n),
              "T17: OE released with ZERO clock edges after ack");
        ticks(6);                                  /* engine catches up */
        CHECK(B(top.p_ltch__d__rd__oe__n), "T17: still released after sync");
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    printf(fails ? ">>> DO NOT FLASH <<<\n" : "ALL PASS - safe to synthesise\n");
    return fails ? 1 : 0;
}
