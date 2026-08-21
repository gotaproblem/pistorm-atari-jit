/*
 * PSP3 - asynchronous front end on the proven PSP2 engine.
 * See ../DESIGN-PSP3.md.  Engine zone is byte-identical to PSP2 0x2A.
 *
 * Zones:
 *   FRONT END (async)  - '373 gates are combinational passthrough of the
 *                        Pi strobes; attributes captured at strobe FALL;
 *                        requests are async-set flags.  This zone is the
 *                        old firmware's speed with its rules written down:
 *                        the Pi guarantees lane stability around its own
 *                        strobe (same contract the external '373s use).
 *   CDC                - one req/clr handshake per event (go, ack, csr
 *                        read, csr write).  2-FF sync into the engine,
 *                        async clear back.  Quasi-static payload: attrs
 *                        close at the same strobe fall that sets the
 *                        request, so they are stable >=2 clocks before
 *                        the engine can look.
 *   ENGINE (8 MHz)     - UNCHANGED from PSP2 0x2A: S2/S3/S4 write shape,
 *                        split-edge strobe negation, release-on-grant
 *                        arbitration, watchdog, completion TOGGLE.
 *
 * DELIBERATE EXCEPTIONS to the PSP2 design laws, confined to the front
 * end and whitelisted in the sim gate:
 *   - pi_wr / pi_rd are used as clocks (negedge = capture point).
 *   - No other latches or async logic anywhere else.
 *
 * FWREV = 8'h30 ("3.0r").
 */

`default_nettype none

module pistorm_psp3_core (
    input  wire        clk,          /* M68K_CLK, 8 MHz               */

    /* ---- Pi interface ------------------------------------------- */
    input  wire        pi_wr,        /* PI_CMD_WR   (async, clock!)   */
    input  wire        pi_rd,        /* PI_CMD_RD   (async, clock!)   */
    input  wire [1:0]  pi_cmd,       /* PI_CMD                        */
    input  wire [15:0] pi_d_in,      /* PI_D pad inputs               */
    output reg  [15:0] pi_d_out,     /* PI_D drive value (CSR only)   */
    output wire        pi_d_oe,      /* core drives PI_D (CSR read)   */
    output reg         busy,         /* PI_TXN_IN_PROGRESS - TOGGLES  */
    output reg         pi_berr,      /* PI_BERR                       */
    output reg         pi_ipl1,
    output reg         pi_ipl2,

    /* ---- external latch control ----------------------------------*/
    output wire        ltch_a_0,     /* '373 gates: now WIRES driven  */
    output wire        ltch_a_8,     /* straight from the Pi strobe   */
    output wire        ltch_a_16,
    output wire        ltch_a_24,
    output reg         ltch_a_oe_n,
    output wire        ltch_d_wr_u,
    output wire        ltch_d_wr_l,
    output reg         ltch_d_wr_oe_n,
    output reg         ltch_d_rd_u,  /* '374 clock: capture on rise   */
    output reg         ltch_d_rd_l,
    output wire        ltch_d_rd_oe_n,

    /* ---- 68K bus -------------------------------------------------*/
    output reg  [2:0]  fc_out,
    output reg         bus_oe,
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
    output reg         reset_drive,
    output reg         halt_drive,
    input  wire        br_n,         /* async */
    output wire        bg_n,
    input  wire        bgack_n       /* async */
);

    localparam [7:0] FWREV = 8'h31;   /* 0x31: ACK RELEASES PI_D
       COMBINATIONALLY.  Bench (first PSP3 p2diag at shape=1): fight=YES,
       +142 spurious reads, unstable re-reads, 30-97%% errors.  The ack
       flag was caught instantly but the '374/CSR OE release still went
       THROUGH the CDC (~375ns) - the now-fast driver started its next
       ADDR phase into a still-driving '374.  Fight garbage in the
       address latches explained every signature, including bit12
       flicker minting spurious read-GOs.  Now the raw ack flag gates
       both OEs combinationally: ack fall = lanes free, in nanoseconds,
       engine bookkeeping follows at its leisure. */

    /* =========================================================
     * FRONT END - asynchronous zone.  Nothing here uses clk.
     * ========================================================= */
    wire sel_data = (pi_cmd == 2'd0);
    wire sel_alo  = (pi_cmd == 2'd1);
    wire sel_ahi  = (pi_cmd == 2'd2);
    wire sel_csr  = (pi_cmd == 2'd3);

    /* '373 gates: transparent for exactly the Pi's strobe width.  The
     * Pi sets the lanes before raising WR and holds them past the fall
     * (>=2 MMIO ops ~ 120ns+), dwarfing the '373's ~15ns needs.  This
     * is the old firmware's arrangement, which never lost a capture. */
    assign ltch_a_0    = sel_alo  & pi_wr;
    assign ltch_a_8    = sel_alo  & pi_wr;
    assign ltch_a_16   = sel_ahi  & pi_wr;
    assign ltch_a_24   = sel_ahi  & pi_wr;
    assign ltch_d_wr_u = sel_data & pi_wr;
    assign ltch_d_wr_l = sel_data & pi_wr;

    /* Attribute capture at strobe FALL (lanes still driven - the Pi's
     * trailing hold is the setup guarantee, as for the '373s above). */
    reg        attr_rd = 1'b0, attr_byte = 1'b0, attr_a0 = 1'b0;
    reg [2:0]  attr_fc = 3'd0;
    reg [5:0]  csr_wv  = 6'd0;      /* latched CSR write value        */

    always @(negedge pi_wr) begin
        if (sel_alo) attr_a0 <= pi_d_in[0];
        if (sel_ahi) begin
            attr_fc   <= pi_d_in[15:13];
            attr_rd   <= pi_d_in[12];
            attr_byte <= pi_d_in[11];
        end
        if (sel_csr) csr_wv <= pi_d_in[5:0];
    end

    /* Request flags: set at strobe fall (attrs already closed), cleared
     * asynchronously by the engine after consumption.  A flag catches
     * any strobe width - the "missed strobe" class cannot exist. */
    wire go_clr, csrw_clr, ack_clr, csrd_clr;
    reg  go_req = 1'b0, csrw_req = 1'b0, ack_req = 1'b0, csrd_req = 1'b0;
    reg  ltch_d_rd_oe_n_r = 1'b1;     /* engine-side OE state          */
    reg  pi_d_oe_r        = 1'b0;
    /* ACK = INSTANT RELEASE: the raw flag gates the pads so the Pi may
     * start its next phase immediately after the ack strobe.  The
     * engine's synced copy catches up within ~2 clocks and clears the
     * flag; the OR/ANDNOT keeps the release glitch-free meanwhile. */
    assign ltch_d_rd_oe_n = ltch_d_rd_oe_n_r | ack_req;
    assign pi_d_oe        = pi_d_oe_r & ~ack_req;

    always @(negedge pi_wr or posedge go_clr)
        if (go_clr)                                  go_req   <= 1'b0;
        else if (sel_data || (sel_ahi && pi_d_in[12])) go_req <= 1'b1;

    always @(negedge pi_wr or posedge csrw_clr)
        if (csrw_clr)          csrw_req <= 1'b0;
        else if (sel_csr)      csrw_req <= 1'b1;

    always @(negedge pi_rd or posedge ack_clr)
        if (ack_clr)           ack_req  <= 1'b0;
        else if (sel_data)     ack_req  <= 1'b1;

    always @(negedge pi_rd or posedge csrd_clr)
        if (csrd_clr)          csrd_req <= 1'b0;
        else if (sel_csr)      csrd_req <= 1'b1;

    /* =========================================================
     * CDC - 2-FF sync of each request into the engine domain,
     * registered clear back.  Round trip ~4 clocks; the protocol
     * (toggle completion) guarantees no second request can arrive
     * before the first is consumed.
     * ========================================================= */
    reg [1:0] s_go = 2'b00, s_csrw = 2'b00, s_ack = 2'b00, s_csrd = 2'b00;
    reg r_go_clr = 1'b0, r_csrw_clr = 1'b0, r_ack_clr = 1'b0, r_csrd_clr = 1'b0;
    assign go_clr   = r_go_clr;
    assign csrw_clr = r_csrw_clr;
    assign ack_clr  = r_ack_clr;
    assign csrd_clr = r_csrd_clr;

    /* engine-side control regs; pads see the negedge-staged copies */
    reg as_i = 1'b1, uds_i = 1'b1, lds_i = 1'b1, rw_i = 1'b1;
    reg vma_i = 1'b1, bg_i = 1'b1;
    reg as_q = 1'b1, uds_q = 1'b1, lds_q = 1'b1;
    always @(negedge clk) begin
        as_q  <= as_i;   uds_q <= uds_i;  lds_q <= lds_i;
    end
    /* split-edge strobes (PSP2 0x26, unchanged) */
    assign as_n  = as_i  & as_q;
    assign uds_n = uds_i & uds_q;
    assign lds_n = lds_i & lds_q;
    assign rw    = rw_i;   assign vma_n = vma_i;  assign bg_n = bg_i;

    initial begin
        pi_d_out = 16'd0;  pi_d_oe_r = 1'b0;
        busy = 1'b0;       pi_berr = 1'b0;
        pi_ipl1 = 1'b0;    pi_ipl2 = 1'b0;
        ltch_a_oe_n = 1'b1;
        ltch_d_wr_oe_n = 1'b1;
        ltch_d_rd_u = 1'b0; ltch_d_rd_l = 1'b0; ltch_d_rd_oe_n_r = 1'b1;
        fc_out = 3'd0;     bus_oe = 1'b0;
        e_clk = 1'b0;
        reset_drive = 1'b0; halt_drive = 1'b0;
    end

    /* ---- remaining input synchronisers (68k side, as PSP2) -------- */
    reg [1:0] s_dtack = 2'b11, s_berr  = 2'b11, s_vpa   = 2'b11;
    reg [1:0] s_br    = 2'b11, s_bgack = 2'b11, s_reset = 2'b11;
    reg [2:0] s_ipl1  = 3'b111, s_ipl2 = 3'b111;
    always @(posedge clk) begin
        s_dtack <= {s_dtack[0], dtack_n};
        s_berr  <= {s_berr[0],  berr_n};
        s_vpa   <= {s_vpa[0],   vpa_n};
        s_br    <= {s_br[0],    br_n};
        s_bgack <= {s_bgack[0], bgack_n};
        s_reset <= {s_reset[0], reset_n_in};
        s_ipl1  <= {s_ipl1[1:0], ipl_n[1]};
        s_ipl2  <= {s_ipl2[1:0], ipl_n[2]};
    end

    wire dtack_a = ~s_dtack[1];
    wire berr_a  = ~s_berr[1];
    wire vpa_a   = ~s_vpa[1];
    wire br_a    = ~s_br[1];
    wire bgack_a = ~s_bgack[1];

    /* ---- E clock (unchanged) -------------------------------------- */
    reg [3:0] e_cnt = 4'd0;
    always @(posedge clk) begin
        e_cnt <= (e_cnt == 4'd9) ? 4'd0 : e_cnt + 4'd1;
        e_clk <= (e_cnt >= 4'd5);
    end

    /* ---- arbitration state (unchanged) ----------------------------- */
    localparam A_OWN = 2'd0, A_GRANT = 2'd1, A_EXT = 2'd2, A_REC = 2'd3;
    reg [1:0] arb = A_OWN;
    wire engine_idle;
    wire ext_owns = (arb == A_EXT);
    reg st_br_seen = 1'b0, st_bgack_seen = 1'b0;

    reg        go_pending = 1'b0;
    reg        csr_rd_pending = 1'b0;
    reg        ctl_clr_sticky_pulse = 1'b0;

    /* ---- cycle engine (states unchanged) --------------------------- */
    localparam E_IDLE = 4'd0,  E_ADDR = 4'd1, E_DRIVE = 4'd2,
               E_WAIT = 4'd3,  E_CAP  = 4'd4, E_REL   = 4'd5,
               E_HOLD = 4'd6,  E_DRV1 = 4'd7, E_DRV2  = 4'd8,
               E_DRV3 = 4'd9,  E_VPAW = 4'd10, E_ABORT = 4'd11,
               E_CSR  = 4'd12;
    reg [3:0]  eng = E_IDLE;
    assign engine_idle = (eng == E_IDLE);

    reg [9:0]  wd = 10'd0;
    reg st_berr = 1'b0, st_wd = 1'b0;
    reg st_fight = 1'b0;
    reg dbg_mode = 1'b0;
    reg [6:0] cnt_wr = 7'd0;
    reg [7:0] cnt_rd = 8'd0;
    reg dbg_page2 = 1'b0;
    reg [7:0] cnt_br = 8'd0, cnt_bgack = 8'd0;
    reg br_prev = 1'b0, bgack_prev = 1'b0;

    /* ---- POR (unchanged rationale) --------------------------------- */
    reg [2:0] por_cnt = 3'd0;
    wire      por = (por_cnt != 3'd7);

    always @(posedge clk) begin
        /* ---------- defaults ----------------------------------------- */
        ltch_d_rd_u <= 1'b0; ltch_d_rd_l <= 1'b0;
        ctl_clr_sticky_pulse <= 1'b0;
        pi_ipl1 <= ~s_ipl1[2];
        pi_ipl2 <= ~s_ipl2[2];

        /* ---------- CDC: sync + consume + handshake clear ------------ */
        s_go   <= {s_go[0],   go_req};
        s_csrw <= {s_csrw[0], csrw_req};
        s_ack  <= {s_ack[0],  ack_req};
        s_csrd <= {s_csrd[0], csrd_req};

        /* GO */
        if (r_go_clr) begin
            if (!s_go[1]) r_go_clr <= 1'b0;          /* flag seen down  */
        end else if (s_go[1]) begin
            go_pending <= 1'b1;
            pi_berr    <= 1'b0;
            if (!ltch_d_rd_oe_n_r || pi_d_oe_r) st_fight <= 1'b1;
            r_go_clr   <= 1'b1;
        end

        /* CSR write */
        if (r_csrw_clr) begin
            if (!s_csrw[1]) r_csrw_clr <= 1'b0;
        end else if (s_csrw[1]) begin
            reset_drive <= csr_wv[1];
            halt_drive  <= csr_wv[2];
            dbg_mode    <= csr_wv[4];
            dbg_page2   <= csr_wv[5];
            if (csr_wv[3]) ctl_clr_sticky_pulse <= 1'b1;
            if (csr_wv[0]) begin                     /* engine soft-reset*/
                eng        <= E_IDLE;
                go_pending <= 1'b0;
                ltch_d_rd_oe_n_r <= 1'b1;
                pi_d_oe_r    <= 1'b0;
            end
            r_csrw_clr <= 1'b1;
        end

        /* read-data ack */
        if (r_ack_clr) begin
            if (!s_ack[1]) r_ack_clr <= 1'b0;
        end else if (s_ack[1]) begin
            ltch_d_rd_oe_n_r <= 1'b1;
            pi_d_oe_r        <= 1'b0;
            r_ack_clr      <= 1'b1;
        end

        /* CSR read GO */
        if (r_csrd_clr) begin
            if (!s_csrd[1]) r_csrd_clr <= 1'b0;
        end else if (s_csrd[1]) begin
            if (eng == E_IDLE)
                csr_rd_pending <= 1'b1;
            r_csrd_clr <= 1'b1;
        end

        /* ---------- sticky evidence + grant telemetry (unchanged) ---- */
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

        /* ---------- arbitration (unchanged, PSP2 0x29) ---------------- */
        case (arb)
            A_OWN:   if (br_a && engine_idle) begin
                         bg_i <= 1'b0;  arb <= A_GRANT;
                     end
            A_GRANT: if (bgack_a)      begin bg_i <= 1'b1; arb <= A_EXT; end
                     else if (!br_a)   begin bg_i <= 1'b1; arb <= A_REC; end
            A_EXT:   if (!bgack_a)     arb <= A_REC;
            A_REC:   arb <= A_OWN;
        endcase
        bus_oe      <= (arb == A_OWN);
        ltch_a_oe_n <= (arb == A_OWN) ? 1'b0 : 1'b1;

        /* ---------- cycle engine (unchanged, PSP2 0x2A) --------------- */
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
                    rw_i       <= attr_rd;
                    as_i       <= 1'b0;
                    if (attr_rd) begin
                        uds_i <= attr_byte ?  attr_a0 : 1'b0;
                        lds_i <= attr_byte ? ~attr_a0 : 1'b0;
                    end else
                        ltch_d_wr_oe_n <= 1'b0;
                    eng <= attr_rd ? E_WAIT : E_DRIVE;
                end
            end

            E_ADDR: begin
                ltch_d_wr_oe_n <= 1'b0;
                eng <= E_DRIVE;
            end

            E_DRIVE: begin
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
                    vma_i <= 1'b0;
                    eng   <= E_VPAW;
                end
            end

            E_VPAW: begin
                wd <= wd + 10'd1;
                if (berr_a || (&wd))
                    eng <= E_ABORT;
                else if (e_cnt == 4'd9)
                    eng <= attr_rd ? E_CAP : E_REL;
            end

            E_CAP: begin
                ltch_d_rd_u <= 1'b1;
                ltch_d_rd_l <= 1'b1;
                eng <= E_REL;
            end

            E_REL: begin
                as_i <= 1'b1; uds_i <= 1'b1; lds_i <= 1'b1; vma_i <= 1'b1;
                eng  <= E_HOLD;
            end

            E_HOLD: begin
                ltch_d_wr_oe_n <= 1'b1;
                rw_i <= 1'b1;
                if (attr_rd) begin
                    ltch_d_rd_oe_n_r <= 1'b0;
                    eng <= E_DRV1;
                end else begin
                    busy <= ~busy;
                    eng  <= E_IDLE;
                end
            end

            E_DRV1: eng <= E_DRV2;
            E_DRV2: eng <= E_DRV3;
            E_DRV3: begin
                busy <= ~busy;
                eng  <= E_IDLE;
            end

            E_CSR: begin
                pi_d_out <= dbg_page2 ? {cnt_br, cnt_bgack}
                          : dbg_mode
                          ? {st_fight, cnt_wr, cnt_rd}
                          : {~engine_idle, ~s_berr[1], ~s_dtack[1], ext_owns,
                             st_br_seen, st_bgack_seen, st_wd, st_berr,
                             FWREV};
                pi_d_oe_r  <= 1'b1;
                eng <= E_DRV1;
            end

            E_ABORT: begin
                pi_berr <= 1'b1;
                if (berr_a) st_berr <= 1'b1;
                else        st_wd   <= 1'b1;
                eng <= E_REL;
            end

            default: eng <= E_IDLE;
        endcase

        /* ---------- POWER-ON RESET: last, so it wins ------------------ */
        if (por) begin
            por_cnt        <= por_cnt + 3'd1;
            eng            <= E_IDLE;
            arb            <= A_OWN;
            bg_i           <= 1'b1;
            as_i           <= 1'b1;  uds_i <= 1'b1;  lds_i <= 1'b1;
            rw_i           <= 1'b1;  vma_i <= 1'b1;
            bus_oe         <= 1'b0;
            pi_berr <= 1'b0;  pi_d_oe_r <= 1'b0;
            ltch_a_oe_n    <= 1'b1;
            ltch_d_wr_oe_n <= 1'b1;  ltch_d_rd_oe_n_r <= 1'b1;
            go_pending     <= 1'b0;  csr_rd_pending <= 1'b0;
            r_go_clr <= 1'b1; r_csrw_clr <= 1'b1;   /* drain any powerup */
            r_ack_clr <= 1'b1; r_csrd_clr <= 1'b1;  /* noise in the flags */
            st_br_seen     <= 1'b0;  st_bgack_seen  <= 1'b0;
            st_berr        <= 1'b0;  st_wd          <= 1'b0;
            reset_drive    <= 1'b0;  halt_drive     <= 1'b0;
            wd             <= 10'd0;
        end
    end

endmodule


/* ==================================================================
 * Pad wrapper - the ONLY tri-states.  Port names match the board .qsf.
 * ================================================================== */
module pistorm_psp3_top (
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

    pistorm_psp3_core core (
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

    assign M68K_RESET_n = rst_drv ? 1'b0 : 1'bz;
    assign M68K_HALT_n  = hlt_drv ? 1'b0 : 1'bz;
endmodule
