/*
 * PSP2 - clean-sheet PiStorm-Atari CPLD firmware.  See ../DESIGN.md.
 *
 * Design law (enforced structurally, not by convention):
 *   - ONE clock (8 MHz bus clock), posedge only, everywhere.
 *   - Every asynchronous input is 2-FF synchronised before use.
 *   - The core is tri-state free: every pad is an (out, oe, in) triple.
 *     pistorm_psp2_top (bottom of file) is the only place 'z' appears.
 *   - Every register is assigned a default at the top of its always block:
 *     latch inference is impossible by construction.
 *
 * FWREV (CSR[7:0]) = 8'h21.  Bump on EVERY functional change.
 */

`default_nettype none

module pistorm_psp2_core (
    input  wire        clk,          /* M68K_CLK, 8 MHz               */

    /* ---- Pi interface ------------------------------------------- */
    input  wire        pi_wr,        /* PI_CMD_WR   (async)           */
    input  wire        pi_rd,        /* PI_CMD_RD   (async)           */
    input  wire [1:0]  pi_cmd,       /* PI_CMD                        */
    input  wire [15:0] pi_d_in,      /* PI_D pad inputs               */
    output reg  [15:0] pi_d_out,     /* PI_D drive value (CSR only)   */
    output reg         pi_d_oe,      /* core drives PI_D (CSR read)   */
    output reg         busy,         /* PI_TXN_IN_PROGRESS - TOGGLES
                                        once per completed transaction  */
    output reg         pi_berr,      /* PI_BERR                       */
    output reg         pi_ipl1,
    output reg         pi_ipl2,

    /* ---- external latch control ----------------------------------*/
    output reg         ltch_a_0,     /* '373 clocks, active-high gate */
    output reg         ltch_a_8,
    output reg         ltch_a_16,
    output reg         ltch_a_24,
    output reg         ltch_a_oe_n,
    output reg         ltch_d_wr_u,
    output reg         ltch_d_wr_l,
    output reg         ltch_d_wr_oe_n,
    output reg         ltch_d_rd_u,  /* '374 clock: capture on rise   */
    output reg         ltch_d_rd_l,
    output reg         ltch_d_rd_oe_n,

    /* ---- 68K bus -------------------------------------------------*/
    output reg  [2:0]  fc_out,
    output reg         bus_oe,       /* drive AS/UDS/LDS/RW/FC/VMA    */
    output wire        as_n,
    output wire        uds_n,
    output wire        lds_n,
    output wire        rw,
    input  wire        dtack_n,      /* async */
    input  wire        berr_n,       /* async */
    input  wire        vpa_n,        /* async */
    output reg         e_clk,
    output wire        vma_n,
    input  wire [2:0]  ipl_n,        /* async */
    input  wire        reset_n_in,   /* async (monitor only)          */
    output reg         reset_drive,  /* 1 = pull RESET_n low (OD)     */
    output reg         halt_drive,   /* 1 = pull HALT_n  low (OD)     */
    input  wire        br_n,         /* async */
    output wire        bg_n,
    input  wire        bgack_n       /* async */
);

    /* engine-side control regs; pads see the negedge-staged copies */
    reg as_i = 1'b1, uds_i = 1'b1, lds_i = 1'b1, rw_i = 1'b1;
    reg vma_i = 1'b1, bg_i = 1'b1;
    reg as_q = 1'b1, uds_q = 1'b1, lds_q = 1'b1;
    always @(negedge clk) begin
        as_q  <= as_i;   uds_q <= uds_i;  lds_q <= lds_i;
    end
    /* split-edge strobes: assert (fall) at posedge with the engine,
     * negate (rise) only at the following negedge - old fw s2/s4 vs s7 */
    assign as_n  = as_i  & as_q;
    assign uds_n = uds_i & uds_q;
    assign lds_n = lds_i & lds_q;
    assign rw    = rw_i;   assign vma_n = vma_i;  assign bg_n = bg_i;

    localparam [7:0] FWREV = 8'h2A;   /* 0x23: COMPLETION IS A TOGGLE.
       Bench proof that level-BUSY was unfixable: with 6-read strobes the
       Pi missed 0.3% of rises; with 12-read strobes the whole cycle fit
       inside the Pi's own strobe tail and it missed 100% of them - the
       observer cannot be required to win a race against its own overhead.
       The PI_TXN_IN_PROGRESS pin now TOGGLES once per completed
       transaction (after the read-data settle, preserving the validity
       contract).  A toggle is unmissable at ANY observer latency.
       0x24: '373 GATE PULSES WIDENED 125ns -> 375ns.  With the toggle
       handshake proven loss-free (zero timeouts in 300k transactions),
       residual 0.3% write loss decoded as the external latches missing a
       single-clock gate pulse: address latches kept the previous address,
       data latches the previous data - the proven old firmware never
       pulsed gates at all, it held them at level for the whole strobe.
       Three clocks restores that electrical margin; the Pi holds the
       lanes far longer, so there is no downside.
       0x25: BUS CONTROLS MOVED TO A NEGEDGE OUTPUT STAGE.  0x24 proved
       the residual 0.3% loss was below the protocol: writes complete
       (DTACK, toggle) but never store, reads are perfect, and no Pi-side
       knob moves the number.  Root cause: this engine drove AS/UDS/LDS/RW
       ~15ns after the posedge of the same 8MHz clock GLUE and the MMU
       sample on - negating strobes half a clock earlier than a 68000
       and inside the chipset's hold window, truncating the tail of the
       MMU's write-CAS window on unlucky alignments.  A real 68000's
       ~60ns tCLAV puts every transition mid-half-cycle; the old proven
       firmware negated AS/UDS/LDS in s7, a NEGEDGE state.  The negedge
       output stage reproduces that: engine unchanged (posedge), the six
       control outputs transition on the falling edge.
       0x26: 0x25 was WORSE (1.5% vs 0.3%) - shifting the ASSERT edges to
       negedge starved the chipset's posedge samplers of setup.  The old
       proven firmware is split-edge: AS/UDS/LDS assert out of POSEDGE
       states (s2/s4) and negate in s7, a NEGEDGE state; RW is pure
       posedge (s0/s2).  0x26 clones that exactly.  Active-low makes it
       one gate: out = sig_i & sig_q (negedge copy) - falls with the
       engine at posedge, rises only at the following negedge.  '374
       read capture unchanged (E_CAP posedge = old fw LS374 s6), and the
       late strobe negation extends slave data hold past the capture
       edge.
       0x27: INSTRUMENTED.  0.3% is invariant across four firmwares -
       stop guessing, start counting.  CSR write bit4 = debug page: CSR
       then reads {st_fight, cnt_wr[6:0], cnt_rd[7:0]} - executed-cycle
       counters incremented at dispatch.  If lost writes execute as READ
       cycles (stale attr path) cnt_wr comes up short; if cnt_wr is exact
       the cycles ran and the loss is board/chipset-side.  st_fight goes
       sticky if any Pi strobe arrives while the '374/CSR still drives
       PI_D (unacked-read bus fight - would corrupt the FOLLOWING op:
       the adjacent-pair signature).
       0x28: WRITE STROBES ONE CLOCK EARLIER - 68000-EXACT S4 TIMING.
       0x27's counters came back EXACT with fight=no: every lost write
       executed as a genuine write cycle.  The loss is in the chipset's
       reception.  Remaining divergence from BOTH working references
       (real 68000: AS at S2, write UDS/LDS at S4 = +1 clock; old fw:
       s2 -> s4 = +1 clock): this engine asserted write strobes +2
       clocks after AS.  Reads - strobes WITH AS, 68000-exact - have
       never lost a transaction; the failure is 100% write-side.  If the
       MMU samples the byte strobes on its fixed slot schedule, a strobe
       one clock late is DTACKed but never CAS'd: complete-but-unstored.
       Now: dispatch = AS+RW+data OE (S2/S3), next clock = strobes (S4).
       0x29: BUS RELEASED ON GRANT, BGACK OPTIONAL (2-WIRE ARBITRATION).
       FDD evidence: BR asserts in EVERY DMA transfer window, the DMA's
       FIFO->RAM bursts never land, address counter frozen, LOST DATA.
       The proven old firmware's BusArb has the BGACK wait COMMENTED OUT
       ("skip to EXT_MASTER") - this machine's DMA master holds BR and
       never asserts BGACK.  0x28 kept driving the bus through the whole
       grant waiting for that BGACK, strangling every DMA burst.  Now the
       bus (AS/UDS/LDS/RW/FC/VMA + address latch OE) is released the
       moment BG asserts; BGACK still honoured if a 3-wire master shows
       up; reacquire one recovery clock after BR negates.
       0x2A: GRANT TELEMETRY.  CSR write bit5 selects debug page 2:
       {br_edges[7:0], bgack_edges[7:0]}, free-running edge counters on
       the synced BR/BGACK inputs.  A healthy 512-byte floppy DMA window
       is ~32 FIFO bursts; reading the deltas across a window tells us
       whether a dead window (RAM untouched) had its ~32 grants (bus ran,
       data went the WRONG WAY) or zero (DMA never engaged).  The one
       number no Pi-side instrument could ever see. */

    /* Power-up state: NOTHING asserted, NOTHING granted, NOTHING driven at
     * the Pi. A device that wakes up owning things it was not given is how
     * fights start. */
    initial begin
        pi_d_out = 16'd0;  pi_d_oe = 1'b0;
        busy = 1'b0;       pi_berr = 1'b0;
        pi_ipl1 = 1'b0;    pi_ipl2 = 1'b0;
        ltch_a_0 = 1'b0;   ltch_a_8 = 1'b0;
        ltch_a_16 = 1'b0;  ltch_a_24 = 1'b0;
        ltch_a_oe_n = 1'b1;
        ltch_d_wr_u = 1'b0; ltch_d_wr_l = 1'b0; ltch_d_wr_oe_n = 1'b1;
        ltch_d_rd_u = 1'b0; ltch_d_rd_l = 1'b0; ltch_d_rd_oe_n = 1'b1;
        fc_out = 3'd0;     bus_oe = 1'b0;
        e_clk = 1'b0;
        reset_drive = 1'b0; halt_drive = 1'b0;
    end

    /* =========================================================
     * Input synchronisers - nothing async is used raw, anywhere.
     * ========================================================= */
    /* power-up: active-low inputs presumed NEGATED until proven live */
    reg [1:0] s_wr    = 2'b00, s_rd    = 2'b00;
    reg [1:0] s_dtack = 2'b11, s_berr  = 2'b11, s_vpa   = 2'b11;
    reg [1:0] s_br    = 2'b11, s_bgack = 2'b11, s_reset = 2'b11;
    reg [2:0] s_ipl1  = 3'b111, s_ipl2 = 3'b111;   /* extra stage on IPL: they can change
                                   asynchronously mid-look */
    always @(posedge clk) begin
        s_wr    <= {s_wr[0],    pi_wr};
        s_rd    <= {s_rd[0],    pi_rd};
        s_dtack <= {s_dtack[0], dtack_n};
        s_berr  <= {s_berr[0],  berr_n};
        s_vpa   <= {s_vpa[0],   vpa_n};
        s_br    <= {s_br[0],    br_n};
        s_bgack <= {s_bgack[0], bgack_n};
        s_reset <= {s_reset[0], reset_n_in};
        s_ipl1  <= {s_ipl1[1:0], ipl_n[1]};
        s_ipl2  <= {s_ipl2[1:0], ipl_n[2]};
    end

    wire wr_sync   = s_wr[1];
    wire rd_sync   = s_rd[1];
    reg  wr_prev = 1'b0, rd_prev = 1'b0;
    wire wr_edge = wr_sync & ~wr_prev;      /* one clock, in-domain */
    wire rd_edge = rd_sync & ~rd_prev;

    wire dtack_a = ~s_dtack[1];             /* asserted (active-low in) */
    wire berr_a  = ~s_berr[1];
    wire vpa_a   = ~s_vpa[1];
    wire br_a    = ~s_br[1];
    wire bgack_a = ~s_bgack[1];

    /* =========================================================
     * E clock - 10 states, 6 low / 4 high, free-running forever.
     * ========================================================= */
    reg [3:0] e_cnt = 4'd0;
    always @(posedge clk) begin
        e_cnt <= (e_cnt == 4'd9) ? 4'd0 : e_cnt + 4'd1;
        e_clk <= (e_cnt >= 4'd5);           /* high for 6,7,8,9 */
    end

    /* =========================================================
     * Arbitration - full 68000-style.  See DESIGN.md.
     * ========================================================= */
    localparam A_OWN = 2'd0, A_GRANT = 2'd1, A_EXT = 2'd2, A_REC = 2'd3;
    reg [1:0] arb = A_OWN;
    wire engine_idle;                        /* from engine below */
    wire ext_owns = (arb == A_EXT);

    reg st_br_seen = 1'b0, st_bgack_seen = 1'b0;   /* sticky evidence */

    /* =========================================================
     * Pi command capture + transaction attributes
     * ========================================================= */
    reg        go_pending = 1'b0;
    reg        attr_rd = 1'b0, attr_byte = 1'b0;
    reg [2:0]  attr_fc = 3'd0;
    reg        attr_a0 = 1'b0;
    reg        csr_rd_pending = 1'b0;
    reg        go_arm = 1'b0;            /* GO seen; commit next clock  */
    reg [1:0]  ga_lo_sr = 2'b00;         /* '373 gate stretchers: each  */
    reg [1:0]  ga_hi_sr = 2'b00;         /* pulse held 3 clocks (375ns) */
    reg [1:0]  gd_wr_sr = 2'b00;         /* - see FWREV 0x24 note       */

    /* CSR control bits */
    reg ctl_clr_sticky_pulse = 1'b0;

    /* =========================================================
     * Cycle engine
     * ========================================================= */
    localparam E_IDLE = 4'd0,  E_ADDR = 4'd1, E_DRIVE = 4'd2,
               E_WAIT = 4'd3,  E_CAP  = 4'd4, E_REL   = 4'd5,
               E_HOLD = 4'd6,  E_DRV1 = 4'd7, E_DRV2  = 4'd8,
               E_DRV3 = 4'd9,  E_VPAW = 4'd10, E_ABORT = 4'd11,
               E_CSR  = 4'd12;
    reg [3:0]  eng = E_IDLE;
    assign engine_idle = (eng == E_IDLE);

    reg [9:0]  wd = 10'd0;                   /* watchdog: 1023 clk ~128us */
    reg st_berr = 1'b0, st_wd = 1'b0;        /* sticky */
    reg st_fight = 1'b0;                     /* strobe while PI_D driven */
    reg dbg_mode = 1'b0;                     /* CSR page select          */
    reg [6:0] cnt_wr = 7'd0;                 /* executed write cycles    */
    reg [7:0] cnt_rd = 8'd0;                 /* executed read cycles     */
    reg dbg_page2 = 1'b0;
    reg [7:0] cnt_br = 8'd0, cnt_bgack = 8'd0;   /* grant telemetry      */
    reg br_prev = 1'b0, bgack_prev = 1'b0;

    /* =========================================================
     * Power-on reset - MAX II FACT: every register wakes at 0 and the
     * device does not honour initial values the way simulators do.  A
     * counter that powers at 0 (guaranteed) counts 8 clocks of POR during
     * which the block below FORCES every safe state.  The initial block
     * above still serves simulation; this serves silicon.  Same states,
     * two enforcement paths, no divergence possible.
     * ========================================================= */
    reg [2:0] por_cnt = 3'd0;
    wire      por = (por_cnt != 3'd7);

    always @(posedge clk) begin
        /* ---------- defaults every clock: no latches possible -------- */
        wr_prev  <= wr_sync;
        rd_prev  <= rd_sync;
        ltch_d_rd_u <= 1'b0; ltch_d_rd_l <= 1'b0;

        /* '373 gate stretchers: fire on accept, hold 3 clocks total */
        ga_lo_sr <= {1'b0, ga_lo_sr[1]};
        ga_hi_sr <= {1'b0, ga_hi_sr[1]};
        gd_wr_sr <= {1'b0, gd_wr_sr[1]};
        ltch_a_0  <= (ga_lo_sr != 2'b00);  ltch_a_8  <= (ga_lo_sr != 2'b00);
        ltch_a_16 <= (ga_hi_sr != 2'b00);  ltch_a_24 <= (ga_hi_sr != 2'b00);
        ltch_d_wr_u <= (gd_wr_sr != 2'b00);
        ltch_d_wr_l <= (gd_wr_sr != 2'b00);
        ctl_clr_sticky_pulse <= 1'b0;
        pi_ipl1 <= ~s_ipl1[2];
        pi_ipl2 <= ~s_ipl2[2];

        /* ---------- Pi register writes (in-domain edges) ------------- */
        if (wr_edge) begin
            if (!ltch_d_rd_oe_n || pi_d_oe)      /* lanes still driven!  */
                st_fight <= 1'b1;
            case (pi_cmd)
                2'd1: begin                          /* ADDR_LO          */
                    ga_lo_sr <= 2'b11;               /* 3-clock gate     */
                    ltch_a_0 <= 1'b1;  ltch_a_8 <= 1'b1;
                    attr_a0  <= pi_d_in[0];
                end
                2'd2: begin                          /* ADDR_HI_CTL      */
                    ga_hi_sr <= 2'b11;               /* 3-clock gate     */
                    ltch_a_16 <= 1'b1; ltch_a_24 <= 1'b1;
                    attr_fc   <= pi_d_in[15:13];
                    attr_rd   <= pi_d_in[12];
                    attr_byte <= pi_d_in[11];
                    if (pi_d_in[12])                 /* GO (read)        */
                        go_arm <= 1'b1;
                end
                2'd0: begin                          /* DATA = GO(write) */
                    gd_wr_sr <= 2'b11;               /* 3-clock gate     */
                    ltch_d_wr_u <= 1'b1; ltch_d_wr_l <= 1'b1;
                    go_arm      <= 1'b1;
                end
                2'd3: begin                          /* CSR write        */
                    reset_drive <= pi_d_in[1];
                    halt_drive  <= pi_d_in[2];
                    dbg_mode    <= pi_d_in[4];
                    dbg_page2   <= pi_d_in[5];
                    if (pi_d_in[3]) ctl_clr_sticky_pulse <= 1'b1;
                    if (pi_d_in[0]) begin            /* engine soft-reset*/
                        eng        <= E_IDLE;
                        go_pending <= 1'b0;
                        ltch_d_rd_oe_n <= 1'b1;
                        pi_d_oe    <= 1'b0;
                    end
                end
            endcase
        end
        if (rd_edge) begin
            case (pi_cmd)
                2'd0: begin                          /* read-data ack    */
                    ltch_d_rd_oe_n <= 1'b1;
                end
                2'd3: begin                          /* CSR read GO      */
                    if (eng == E_IDLE)
                        csr_rd_pending <= 1'b1;
                end
                default: ;
            endcase
        end

        /* GO commit, ONE CLOCK after the accepting strobe: the '373 gate
         * pulsed on the accept clock and is CLOSED by now, so when the Pi
         * sees BUSY rise the latches provably hold their data and the
         * lanes may be released immediately.  "BUSY high => lanes free"
         * becomes a firmware guarantee instead of a driver-side delay.
         * (Bench find: releasing inside the open gate captured float at
         * ~0.2%/txn, uniformly, both directions.) */
        if (go_arm) begin
            go_arm     <= 1'b0;
            go_pending <= 1'b1;
            pi_berr    <= 1'b0;
        end

        /* ---------- sticky evidence ---------------------------------- */
        br_prev    <= br_a;   bgack_prev <= bgack_a;
        if (br_a    && !br_prev)    cnt_br    <= cnt_br    + 8'd1;
        if (bgack_a && !bgack_prev) cnt_bgack <= cnt_bgack + 8'd1;
        if (br_a)    st_br_seen    <= 1'b1;
        if (bgack_a) st_bgack_seen <= 1'b1;
        if (ctl_clr_sticky_pulse) begin
            st_br_seen <= 1'b0; st_bgack_seen <= 1'b0;
            st_berr    <= 1'b0; st_wd         <= 1'b0;
            st_fight   <= 1'b0;
        end

        /* ---------- arbitration -------------------------------------- */
        case (arb)
            A_OWN:   if (br_a && engine_idle) begin
                         bg_i <= 1'b0;  arb <= A_GRANT;
                     end
            A_GRANT: if (bgack_a)      begin bg_i <= 1'b1; arb <= A_EXT; end
                     else if (!br_a)   begin bg_i <= 1'b1; arb <= A_REC; end
            A_EXT:   if (!bgack_a)     arb <= A_REC;
            A_REC:   arb <= A_OWN;                   /* one recovery clk */
        endcase

        /* bus drive enable: released the moment we GRANT - the old
         * firmware's bench truth is that this machine's DMA master is
         * 2-wire (BR held, no BGACK); it takes the bus on BG alone.  A
         * real 68000 also floats its outputs from grant until BR/BGACK
         * clears.  Grant only happens from engine-idle, so nothing is
         * in flight when the drivers drop. */
        bus_oe      <= (arb == A_OWN);
        ltch_a_oe_n <= (arb == A_OWN) ? 1'b0 : 1'b1;

        /* ---------- cycle engine -------------------------------------- */
        case (eng)
            E_IDLE: begin
                as_i <= 1'b1; uds_i <= 1'b1; lds_i <= 1'b1; rw_i <= 1'b1;
                vma_i <= 1'b1;
                wd    <= 10'd0;
                if (csr_rd_pending) begin
                    csr_rd_pending <= 1'b0;
                    eng <= E_CSR;
                end
                else if (go_pending && arb == A_OWN && !br_a) begin
                    go_pending <= 1'b0;
                    if (attr_rd) cnt_rd <= cnt_rd + 8'd1;
                    else         cnt_wr <= cnt_wr + 7'd1;
                    fc_out     <= attr_fc;
                    rw_i         <= attr_rd;           /* low for write */
                    as_i       <= 1'b0;
                    if (attr_rd) begin               /* reads: strobes now */
                        uds_i <= attr_byte ?  attr_a0 : 1'b0;
                        lds_i <= attr_byte ? ~attr_a0 : 1'b0;
                    end else
                        ltch_d_wr_oe_n <= 1'b0;      /* drive data (S3)   */
                    eng <= attr_rd ? E_WAIT : E_DRIVE;
                end
            end

            E_ADDR: begin                            /* write: data OE   */
                ltch_d_wr_oe_n <= 1'b0;              /* after RW low     */
                eng <= E_DRIVE;
            end

            E_DRIVE: begin                           /* write: strobes   */
                uds_i <= attr_byte ?  attr_a0 : 1'b0;
                lds_i <= attr_byte ? ~attr_a0 : 1'b0;
                eng   <= E_WAIT;
            end

            E_WAIT: begin
                wd <= wd + 10'd1;
                if (berr_a || (&wd))
                    eng <= E_ABORT;
                else if (dtack_a)
                    eng <= attr_rd ? E_CAP : E_REL;
                else if (vpa_a) begin
                    vma_i <= 1'b0;                   /* simple E-sync:   */
                    eng   <= E_VPAW;                 /* complete at end  */
                end                                  /* of E high        */
            end

            E_VPAW: begin
                wd <= wd + 10'd1;
                if (berr_a || (&wd))
                    eng <= E_ABORT;
                else if (e_cnt == 4'd9) begin        /* E about to fall  */
                    eng <= attr_rd ? E_CAP : E_REL;
                end
            end

            E_CAP: begin                             /* '374 capture     */
                ltch_d_rd_u <= 1'b1;                 /* rising edge NOW, */
                ltch_d_rd_l <= 1'b1;                 /* AS still low,    */
                eng <= E_REL;                        /* slave driving    */
            end

            E_REL: begin
                as_i <= 1'b1; uds_i <= 1'b1; lds_i <= 1'b1; vma_i <= 1'b1;
                eng  <= E_HOLD;
            end

            E_HOLD: begin                            /* 125ns hold past  */
                ltch_d_wr_oe_n <= 1'b1;              /* strobe negation  */
                rw_i <= 1'b1;
                if (attr_rd) begin
                    ltch_d_rd_oe_n <= 1'b0;          /* drive PI_D       */
                    eng <= E_DRV1;
                end else begin
                    busy <= ~busy;                   /* write complete:  */
                    eng  <= E_IDLE;                  /* TOGGLE           */
                end
            end

            E_DRV1: eng <= E_DRV2;                   /* 3 clocks = 375ns */
            E_DRV2: eng <= E_DRV3;                   /* of guaranteed    */
            E_DRV3: begin                            /* settle, THEN     */
                busy <= ~busy;                       /* TOGGLE = done,   */
                eng  <= E_IDLE;                      /* data guaranteed  */
            end

            E_CSR: begin
                pi_d_out <= dbg_page2 ? {cnt_br, cnt_bgack}
                          : dbg_mode
                          ? {st_fight, cnt_wr, cnt_rd}
                          : {~engine_idle, ~s_berr[1], ~s_dtack[1], ext_owns,
                             st_br_seen, st_bgack_seen, st_wd, st_berr,
                             FWREV};
                pi_d_oe  <= 1'b1;
                eng <= E_DRV1;                       /* same settle path */
            end

            E_ABORT: begin
                pi_berr <= 1'b1;
                if (berr_a) st_berr <= 1'b1;
                else        st_wd   <= 1'b1;
                eng <= E_REL;                        /* clean release,   */
            end                                      /* same hold rules  */

            default: eng <= E_IDLE;
        endcase

        /* CSR drive released on the read-data ack (rd_edge sel 0) like a
         * normal read; also drop it whenever we re-enter IDLE from DRV3 */
        if (rd_edge && pi_cmd == 2'd0)
            pi_d_oe <= 1'b0;

        /* ---------- POWER-ON RESET: last in the block, so it wins ----- */
        if (por) begin
            por_cnt        <= por_cnt + 3'd1;
            eng            <= E_IDLE;
            arb            <= A_OWN;
            bg_i           <= 1'b1;
            as_i           <= 1'b1;  uds_i <= 1'b1;  lds_i <= 1'b1;
            rw_i             <= 1'b1;  vma_i <= 1'b1;
            bus_oe         <= 1'b0;
            pi_berr <= 1'b0;  pi_d_oe <= 1'b0;
            ltch_a_oe_n    <= 1'b1;
            ltch_d_wr_oe_n <= 1'b1;  ltch_d_rd_oe_n <= 1'b1;
            go_pending     <= 1'b0;  csr_rd_pending <= 1'b0;
            go_arm         <= 1'b0;
            ga_lo_sr <= 2'b00; ga_hi_sr <= 2'b00; gd_wr_sr <= 2'b00;
            st_br_seen     <= 1'b0;  st_bgack_seen  <= 1'b0;
            st_berr        <= 1'b0;  st_wd          <= 1'b0;
            reset_drive    <= 1'b0;  halt_drive     <= 1'b0;
            wd             <= 10'd0;
        end
    end

endmodule


/* ==================================================================
 * Pad wrapper - the ONLY tri-states in the design.  Port names match
 * the board .qsf nets so pin assignments carry over unchanged.
 * ================================================================== */
module pistorm_psp2_top (
    output wire        PI_TXN_IN_PROGRESS,
    input  wire        PI_CMD_WR,
    input  wire [1:0]  PI_CMD,
    input  wire        PI_CMD_RD,
    output wire        PI_IPL1,
    output wire        PI_IPL2,
    output wire        PI_BERR,
    inout  wire [15:0] PI_D,

    output wire        LTCH_A_0,
    output wire        LTCH_A_8,
    output wire        LTCH_A_16,
    output wire        LTCH_A_24,
    output wire        LTCH_A_OE_n,
    output wire        LTCH_D_RD_U,
    output wire        LTCH_D_RD_L,
    output wire        LTCH_D_RD_OE_n,
    output wire        LTCH_D_WR_U,
    output wire        LTCH_D_WR_L,
    output wire        LTCH_D_WR_OE_n,

    input  wire        M68K_CLK,
    output wire [2:0]  M68K_FC,
    output wire        M68K_AS_n,
    output wire        M68K_UDS_n,
    output wire        M68K_LDS_n,
    output wire        M68K_RW,
    input  wire        M68K_DTACK_n,
    input  wire        M68K_BERR_n,
    input  wire        M68K_VPA_n,
    output wire        M68K_E,
    output wire        M68K_VMA_n,
    input  wire [2:0]  M68K_IPL_n,
    inout  wire        M68K_RESET_n,
    inout  wire        M68K_HALT_n,
    input  wire        M68K_BR_n,
    output wire        M68K_BG_n,
    input  wire        M68K_BGACK_n
);
    wire [15:0] d_out;  wire d_oe;
    wire [2:0]  fc;     wire bus_oe;
    wire as_i, uds_i, lds_i, rw_i, vma_i, rst_drv, hlt_drv;

    pistorm_psp2_core core (
        .clk(M68K_CLK),
        .pi_wr(PI_CMD_WR), .pi_rd(PI_CMD_RD), .pi_cmd(PI_CMD),
        .pi_d_in(PI_D), .pi_d_out(d_out), .pi_d_oe(d_oe),
        .busy(PI_TXN_IN_PROGRESS), .pi_berr(PI_BERR),
        .pi_ipl1(PI_IPL1), .pi_ipl2(PI_IPL2),
        .ltch_a_0(LTCH_A_0), .ltch_a_8(LTCH_A_8),
        .ltch_a_16(LTCH_A_16), .ltch_a_24(LTCH_A_24),
        .ltch_a_oe_n(LTCH_A_OE_n),
        .ltch_d_wr_u(LTCH_D_WR_U), .ltch_d_wr_l(LTCH_D_WR_L),
        .ltch_d_wr_oe_n(LTCH_D_WR_OE_n),
        .ltch_d_rd_u(LTCH_D_RD_U), .ltch_d_rd_l(LTCH_D_RD_L),
        .ltch_d_rd_oe_n(LTCH_D_RD_OE_n),
        .fc_out(fc), .bus_oe(bus_oe),
        .as_n(as_i), .uds_n(uds_i), .lds_n(lds_i), .rw(rw_i),
        .dtack_n(M68K_DTACK_n), .berr_n(M68K_BERR_n), .vpa_n(M68K_VPA_n),
        .e_clk(M68K_E), .vma_n(vma_i),
        .ipl_n(M68K_IPL_n),
        .reset_n_in(M68K_RESET_n),
        .reset_drive(rst_drv), .halt_drive(hlt_drv),
        .br_n(M68K_BR_n), .bg_n(M68K_BG_n), .bgack_n(M68K_BGACK_n)
    );

    assign PI_D       = d_oe   ? d_out : 16'bz;
    assign M68K_FC    = bus_oe ? fc    : 3'bz;
    assign M68K_AS_n  = bus_oe ? as_i  : 1'bz;
    assign M68K_UDS_n = bus_oe ? uds_i : 1'bz;
    assign M68K_LDS_n = bus_oe ? lds_i : 1'bz;
    assign M68K_RW    = bus_oe ? rw_i  : 1'bz;
    assign M68K_VMA_n = bus_oe ? vma_i : 1'bz;

    assign M68K_RESET_n = rst_drv ? 1'b0 : 1'bz;     /* open drain */
    assign M68K_HALT_n  = hlt_drv ? 1'b0 : 1'bz;
endmodule
