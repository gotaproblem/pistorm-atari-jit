/*
 * Experimental
 */

/*
 * Oct 2026 1.0b (fwrev $420) = rev 256, first beta. Tested OK on hardware.
 *
 * Oct 2026 rev 256: 253/255 + a real RESET signal for the Pi on GPIO4
 * (PIN_12, free since the single-strobe protocol of rev 251).
 *
 *  9. PI_RESET (GPIO4), active low, registered on the c8m falling edge:
 *     LOW while the ST holds its RESET line low - reset button, power-on
 *     reset - for at least 16 c8m clocks (2 us) and the CPLD is NOT the
 *     one driving it (reset_out = the Pi's own ps_pulse_reset /
 *     STATUS_BIT_RESET). The Pi's own pulse, the line's rise after it,
 *     and glitches under 2 us never reach the pin. High again on the
 *     first clock RESET_n reads high. Pi bus protocol unchanged; the Pi
 *     side must have GPIO4 as an INPUT (it was the PI_CMD_RD output).
 *     Status readback bit 13 (raw M68K_RESET_n) is unchanged.
 */

/*
 * Oct 2026 rev 253: 252 + two fixes from the first hardware test (EmuTOS
 * boot, PISTORM_HDC_TRACE): the Pi's GPIP reads returned garbage, with no
 * BERR, right after the DMA chip's first bus tenure began.
 *
 *  7. Release the bus like a 68000: once BG is out and our cycle is over,
 *     AS/UDS/LDS/RW/FC/VMA and both latch OEs go high-Z WITHOUT waiting for
 *     BGACK. On the ST the GLUE drives the control lines for a DMA cycle;
 *     252 held them (and the address latches) until BGACK, fighting it.
 *  8. No hand-back on a BR edge alone. 252 went GRANT->OWNED the moment the
 *     synchronised BR read negated; if BGACK's assertion landed one sample
 *     later the engine started a Pi cycle inside the DMA tenure, which never
 *     saw BGACKi and completed on the DMA's DTACK - garbage, no BERR. Every
 *     return to OWNED now goes through SETTLE: two clean falling edges with
 *     BR and BGACK both negated. Cycle starts also require BGACKi.
 *
 * rev 252: 251 (single-strobe, GPIO4 free) + bus arbitration
 * rework. Pi protocol unchanged from 251 - no ps_protocol change needed.
 *
 *  1. BusArb replaced by BusArbSync: one negedge-c8m state machine, BR and
 *     BGACK through 2FF synchronisers, BG and bus_mine REGISTERED. The old
 *     BG was combinational off async BR/AS/DTACK (output block was
 *     "always @(clk or reset_n)" on next_state) and could glitch.
 *  2. bus_mine gates the START of a cycle (AS in S2, S2->S3). Once BG is
 *     out no new cycle begins - MC68000 behaviour. Before, the engine kept
 *     starting Pi cycles under a grant; with BGACK low it ran them with
 *     its outputs tri-stated and completed them on the FOREIGN master's
 *     DTACK: reads returned whatever the DMA had on the bus, writes were
 *     silently lost. Now the Pi just sees TXN stay high a little longer.
 *  3. A cycle whose AS is already out is allowed to finish (S2->S3 also
 *     when AS_INT is low) - the requester waits for AS negated before it
 *     asserts BGACK, as with a real 68000.
 *  4. Address latch OE and write-data latch OE gated by BGACKi. The engine
 *     parks in S2 with LTCH_A_OE_n LOW, so the address latches drove
 *     A1-A23 under every foreign master (blitter/DMA collisions), and a
 *     tenure starting in S7 left the WRITE-DATA latch on D0-D15 for its
 *     whole length (S7 holds until BGACKi returns, s7rst only with s0).
 *  5. Status readback bit 11 (raw GPIO19, previously constant 0) =
 *     arbitration active: 1 from BG assertion until BGACK is released.
 *  6. PI_BERR is reported if BGACK appears inside one of our own cycles
 *     (a master breaking protocol) instead of handing back foreign data.
 */

/* 
 * Sept 2026: single-strobe variant of 24D - PI_CMD_RD removed (GPIO4 free).
 * See the note at CMDWR / trigger below.  Everything else is 24D as-is.
 *
 * MUST define LATCH type - only select one
 * LS373 offers best performance but is not compatible with LS374 latches
 * LS374 offers slower performance but is compatible with both latch types
 */
 
 
//`define LS373
`define LS374

/*
 * DIAG_BERR2 - small diagnostic build of 1.0b (readback fwrev $43D,
 * prints as "EPM240 1.29b"; $43E was the first cut). Kept to a handful of LEs on purpose: a
 * bigger diag (157 LEs) re-placed the Pi strobe decodes and broke writes.
 *
 * At every Pi bus error (end of S5 with BERR_n or BGACKi low) the
 * readback bits the Pi does not use latch, for that cycle:
 *
 *   bit 15  M68K_BERR_n low       - the ST's BERR line (GLUE)
 *   bit 14  DTACK_n was low at some clock while our AS was low
 *   bit 12  BR_n already low at the first clock our AS was low - the DMA
 *           had asked for the bus before our cycle began (diag2c;
 *           diag2b had "BR or BG at any clock" here - always yes)
 *   bit 11  our AS had been low for 16+ c8m clocks (2 us) - a normal
 *           MFP read is a few clocks; a GLUE timeout is far longer
 *
 * (bit 11 is ~bus_mine in the release.) Held until the next bus error.
 * Comment the define out for the 1.0b release.
 */
`define DIAG_BERR2

/*
 * Bus Arbitration
 * bus grant should be asserted ASAP
 */		
	
	
module BusArbSync (
	input  wire clk,        // c8m - state changes on the FALLING edge
	input  wire reset_n,

	input  wire br_n,       // Bus Request,             raw pin, active low
	input  wire bgack_n,    // Bus Grant Acknowledge,   raw pin, active low

	output reg  bg_n,       // Bus Grant, active low, registered (no glitches)
	output reg  bus_mine    // 1 = the engine may START a cycle
);
	/*
	 * Falling edge: the engine's posedge states (S0/S2/S4/S6) and this
	 * machine never move on the same edge, and bus_mine is settled half a
	 * clock before the S2->S3 decision that reads it.
	 *
	 * OWNED   -> BR seen: assert BG, stop starting cycles. A cycle already
	 *            in flight completes; the requester waits for AS negated.
	 * GRANT   -> BGACK seen: negate BG (MC68000) -> FOREIGN.
	 *            BR withdrawn without BGACK: negate BG -> SETTLE.
	 * FOREIGN -> BGACK released -> SETTLE.
	 * SETTLE  -> two consecutive falling edges with BR and BGACK both
	 *            negated -> OWNED. Anything asserted meanwhile goes back to
	 *            GRANT/FOREIGN. This is what stops a BR-negate seen one
	 *            sample before a BGACK-assert from handing the bus back
	 *            in the middle of a DMA tenure (rev 253).
	 *
	 * BGACK low in OWNED (a master that skipped the handshake, BGACK stuck
	 * at power-up) goes straight to FOREIGN: never start a cycle under it.
	 */
	localparam OWNED   = 2'd0;
	localparam GRANT   = 2'd1;
	localparam FOREIGN = 2'd2;
	localparam SETTLE  = 2'd3;

	reg [1:0] br_s;
	reg [1:0] bgk_s;
	reg [1:0] st;
	reg       quiet;        /* SETTLE: one clean edge already seen */

	wire br    = ~br_s[1];
	wire bgack = ~bgk_s[1];

	always @(negedge clk or negedge reset_n) begin

		if (!reset_n) begin
			br_s     <= 2'b11;
			bgk_s    <= 2'b11;
			st       <= OWNED;
			bg_n     <= 1'b1;
			bus_mine <= 1'b1;
			quiet    <= 1'b0;
		end else begin
			br_s     <= {br_s[0],  br_n};
			bgk_s    <= {bgk_s[0], bgack_n};

			case (st)

				OWNED: begin
					quiet <= 1'b0;
					if (bgack) begin
						st       <= FOREIGN;
						bg_n     <= 1'b1;
						bus_mine <= 1'b0;
					end else if (br) begin
						st       <= GRANT;
						bg_n     <= 1'b0;
						bus_mine <= 1'b0;   /* no new cycle from here on */
					end else begin
						bg_n     <= 1'b1;
						bus_mine <= 1'b1;
					end
				end

				GRANT: begin
					bus_mine <= 1'b0;
					quiet    <= 1'b0;
					if (bgack) begin
						st   <= FOREIGN;
						bg_n <= 1'b1;       /* MC68000: negate BG once BGACK seen */
					end else if (!br) begin
						st   <= SETTLE;     /* requester gave up - or BGACK is a
											   sample behind; SETTLE tells */
						bg_n <= 1'b1;
					end
				end

				FOREIGN: begin
					bg_n     <= 1'b1;
					bus_mine <= 1'b0;
					quiet    <= 1'b0;
					if (!bgack)
						st <= SETTLE;
				end

				SETTLE: begin
					bus_mine <= 1'b0;
					if (bgack) begin
						st    <= FOREIGN;
						bg_n  <= 1'b1;
						quiet <= 1'b0;
					end else if (br) begin
						st    <= GRANT;
						bg_n  <= 1'b0;
						quiet <= 1'b0;
					end else if (quiet) begin
						st       <= OWNED;
						bg_n     <= 1'b1;
						bus_mine <= 1'b1;
						quiet    <= 1'b0;
					end else
						quiet <= 1'b1;
				end
			endcase
		end
	end

endmodule



	
/* 
 * E CLOCK 
 * A single period of clock E consists of 10 MC68000 clock periods (six clocks low, four clocks high)
 */
module E_CLK (
	input clk,
	output reg [3:0] e_counter,
	output reg ECLOCK
);
	localparam E0 = 4'd0;
	localparam E5 = 4'd5;
	localparam E9 = 4'd9;
	
	always @( negedge clk ) begin
	
		if (e_counter == E9)
			e_counter <= E0;
			
		else
			e_counter <= e_counter + 4'd1;
	end
	
	always @( e_counter ) begin
	
		if (e_counter == E9)
			ECLOCK <= 1'b0;
			
		else if (e_counter == E5)
			ECLOCK <= 1'b1;
	end
endmodule


/*
 * 74LS74 D-type flip-flop with active-LOW preset and clear
 * (matching real 74LS74 active-LOW PRE_n/CLR_n behavior)
 *
 * FIX: Changed from active-HIGH to active-LOW preset/clear
 *      to match real IC. Previous version had BGACKi stuck at 1.
 */
module FF(
	input d,
	input preset_n,
	input clear_n,
	input clk,
	output reg q
	);

	always @(posedge clk, negedge clear_n, negedge preset_n) begin

		if (!preset_n)       // PRE_n asserted (low) -> Q=1
			q <= 1'b1;

		else if (!clear_n)   // CLR_n asserted (low) -> Q=0
			q <= 1'b0;

		else
			q <= d;
	end

endmodule


/* 
 * March 2026 cryptodad NOTE
 * MAKE SURE .qsf file matches the CPLD pin names below
 * costly mistake cost me months of wasted development
 */ 
module pistormsxb_devEPM240(
    output reg     	PI_TXN_IN_PROGRESS, 	// GPIO0
	input           PI_CMD_WR,      		// GPIO1 command write
    input   [1:0]   PI_CMD,       			// GPIO[3..2]
	
	//input       	PI_CLK,     			// GPIO4
	//              GPIO4 was PI_CMD_RD - freed by the single-strobe interface, Sept 2026
	output          PI_RESET,       		// GPIO4 - ST RESET, active low (rev 256)
	output reg     	PI_IPL1,        		// GPIO5
	output reg      PI_IPL2,     			// GPIO6
    output reg     	PI_BERR,   				// GPIO7
	
    inout   [15:0]  PI_D,       			// GPIO[23..8]
 
    output reg      LTCH_A_0,
    output reg      LTCH_A_8,
    output reg		LTCH_A_16,
    output reg     	LTCH_A_24,
    output          LTCH_A_OE_n,
    output reg      LTCH_D_RD_U,
    output reg      LTCH_D_RD_L,
    output reg     	LTCH_D_RD_OE_n,
    output reg     	LTCH_D_WR_U,
    output reg     	LTCH_D_WR_L,
    output          LTCH_D_WR_OE_n,

    input           M68K_CLK,
    output   [2:0]  M68K_FC,

    output       	M68K_AS_n,
    output       	M68K_UDS_n,
    output       	M68K_LDS_n,
    output       	M68K_RW,

    //inout           M68K_DTACK_n,
	input           M68K_DTACK_n,
    //inout           M68K_BERR_n,
	input           M68K_BERR_n,
	
    input           M68K_VPA_n,
    output          M68K_E,
    output       	M68K_VMA_n,

    input   [2:0]   M68K_IPL_n,

    inout           M68K_RESET_n,
    inout           M68K_HALT_n,

    input           M68K_BR_n,
    output      	M68K_BG_n,
    input           M68K_BGACK_n
  );

	localparam REG_DATA 					= 2'd0;
	localparam REG_ADDR_LO 					= 2'd1;
	localparam REG_ADDR_HI 					= 2'd2;
	localparam REG_STATUS 					= 2'd3;
	
	localparam LO							= 1'b0;
	localparam HI							= 1'b1;
	
	localparam S0							= 3'd0;
	localparam S1							= 3'd1;
	localparam S2							= 3'd2;
	localparam S3							= 3'd3;
	localparam S4							= 3'd4;
	localparam S5							= 3'd5;
	localparam S6							= 3'd6;
	localparam S7							= 3'd7;
	
	localparam E0							= 4'd0;
	localparam E1							= 4'd1;
	localparam E2							= 4'd2;
	localparam E5							= 4'd5;
	localparam E6							= 4'd6;
	localparam E8							= 4'd8;
	localparam E9							= 4'd9;
	localparam E10							= 4'd10;

	
	/* ******************************************** */

	reg [15:0] data_out;
	
	/*
	 * cryptodad Oct'24
	 *
	 * 11 bit register holding CPLD firmware revision
	 * should be set via .qsf file -> update_version.tcl
	 */

	`include "version_reg.v"


	wire CMDWR;
	wire c8m;
	wire VM_INT;
	
	
	assign CMDWR							= PI_CMD_WR;

	/*
	 * Single-strobe interface (Sept 2026) - this file is 24D with only the
	 * PI_CMD_RD uses replaced:
	 *
	 *   status READ  : Pi puts REG_STATUS on PI_CMD with CMDWR LOW
	 *                  (a status WRITE has CMDWR high, as before)
	 *   data   READ  : after TXN drops the Pi asserts REG_DATA + CMDWR
	 *                  (instead of REG_DATA + CMDRD) to enable the read
	 *                  latch on to PI_D, reads, then clears TXN_END
	 *
	 * st_wr_release: the WR pin lags PI_CMD on the board and the Pi clears
	 * WR one or two stores before it clears PI_CMD, so for a moment after a
	 * status WRITE the pins look like a status READ while the Pi is still
	 * driving PI_D.  Set when a strobe falls with REG_STATUS selected,
	 * cleared as soon as PI_CMD leaves REG_STATUS; blocks the readback in
	 * that window.
	 */
	wire cmd_status							= (PI_CMD == REG_STATUS);
	reg  st_wr_release						= 1'b0;

	always @(negedge CMDWR or negedge cmd_status) begin
		if (!cmd_status)
			st_wr_release					<= 1'b0;
		else
			st_wr_release					<= 1'b1;
	end

	wire trigger;
	wire bus_mine;							/* BusArbSync: 1 = we may start a cycle */
	assign trigger							= (cmd_status && !CMDWR && !st_wr_release);

	/* bit 11 (raw GPIO19): arbitration active - BG out or foreign master */
`ifdef DIAG_BERR2
	reg  d_berr = 1'b0, d_dtk = 1'b0, d_arb = 1'b0, d_long = 1'b0;
	assign PI_D 							= trigger ? {d_berr, d_dtk, M68K_RESET_n, d_arb, d_long, 11'h43D} : 16'bz;
`else
	assign PI_D 							= trigger ? {PI_IPL2, PI_IPL1, M68K_RESET_n, M68K_HALT_n, ~bus_mine, fwrev} : 16'bz;
`endif
	
	reg [15:0] status;
	
	wire halt								= (status[1:0] == 2'b01) ? HI : LO; /* ps_protocol.c -> ps_write_status_reg (STATUS_BIT_HALT) */
	wire reset_out 							= (status[1:0] == 2'b10) ? HI : LO; /* ps_protocol.c -> ps_write_status_reg (STATUS_BIT_RESET) */
	wire reset_sm							= (status[1:0] == 2'b11) ? HI : LO; /* ps_protocol.c -> ps_write_status_reg (STATUS_BIT_INIT) */
	

	//assign PI_RESET							= M68K_RESET_n;
	assign M68K_RESET_n 					= (reset_out) ? LO : 1'bz;
	assign M68K_HALT_n 						= (halt) ? LO : 1'bz;
	

	
	reg op_req								= HI;
	reg op_rw 								= HI;
	reg op_uds_n 							= HI;
	reg op_lds_n 							= HI;

	reg [2:0] FC_INT;
	reg AS_INT;
	reg UDS_INT;
	reg LDS_INT;
	reg RW_INT;
	wire VMA_INT;

	reg M68K_VMA_nr 						= 1'd1;
	
	reg s0									= 1'd1; //M68K bus states
	reg s1									= 1'd0;
	reg s2									= 1'd0;
	reg s3									= 1'd0;
	reg s4									= 1'd0;
	reg s5									= 1'd0;
	reg s6									= 1'd0;
	reg s7									= 1'd0;
	
	reg a0;
	reg [2:0] op_fc 						= INT_ACK;
	
	wire [2:0] ipl;
	assign ipl 								= M68K_IPL_n;
	reg [2:0] ipl_1;
	reg [2:0] ipl_2;
	
	/* INTERRUPT CONTROL */
	
	/* IPL lines should be serviced on negative edge.
	 *
	 * Do not hold the previous Pi-visible IPL while the synchronizer samples
	 * disagree. Holding the old value can leave a stale IPL6 visible just long
	 * enough for the Pi to issue an interrupt acknowledge after the real Atari
	 * source has already gone away, which produces a spurious IACK/BERR.
	 */
	always @(negedge c8m or negedge M68K_RESET_n) begin
		
		if (!M68K_RESET_n) begin
			ipl_1							<= 3'b111;
			ipl_2							<= 3'b111;
			PI_IPL1							<= LO;
			PI_IPL2							<= LO;
		end else begin
			ipl_1 							<= ipl;
			ipl_2							<= ipl_1;
			
			if ( ipl_2 == ipl_1 ) begin
			
				PI_IPL1						<= ~ipl_2[1];
				PI_IPL2						<= ~ipl_2[2];
			end else begin
				PI_IPL1						<= LO;
				PI_IPL2						<= LO;
			end
		end
	end
	
	
	/* RESET */
	/* pulse when out of reset. delay by one clock pulse is required to prevent lock after reset */
	reg [1:0] resetfilter					= 2'b11;
	wire oor 								= (resetfilter == 2'b01) || reset_sm; 
	
	always @(negedge c8m) begin
	
		resetfilter 						<= {resetfilter[0], M68K_RESET_n};
	end

	/*
	 * rev 256: PI_RESET (GPIO4) - the ST's RESET line for the Pi.
	 *
	 * resetfilter[1] is RESET_n through two flops. rst_own is reset_out
	 * through one, so it rises before resetfilter[1] can show our own
	 * pulse and falls before resetfilter[1] can show the line's release:
	 * the counter never gets 16 clean clocks out of a Pi-driven reset.
	 * c8m-clocked: frozen if the ST's clock stops (power loss is sensed
	 * by the Pi separately, from the firmware revision readback).
	 */
	reg [3:0] rst_cnt						= 4'd0;
	reg       rst_own						= 1'b0;
	reg       pi_reset_r					= HI;

	assign PI_RESET							= pi_reset_r;

	always @(negedge c8m) begin

		rst_own								<= reset_out;

		if (resetfilter[1] || rst_own) begin
			rst_cnt							<= 4'd0;
			pi_reset_r						<= HI;
		end else if (rst_cnt == 4'd15)
			pi_reset_r						<= LO;
		else
			rst_cnt							<= rst_cnt + 4'd1;
	end
	
  
	/* 
	 * E CLOCK instantiation
	 */
	wire [3:0] e_counter;
	E_CLK e_clk ( c8m, e_counter, M68K_E );
	

	/* 
	 * CPU clock - ATARI 8MHz 
	 */
	assign c8m = M68K_CLK;
	
	
	/* Define FC bits */
	parameter USER_DATA						= 3'b001;
	parameter USER_PGM						= 3'b010;
	parameter SVR_DATA						= 3'b101;
	parameter SVR_PGM						= 3'b110;
	parameter INT_ACK						= 3'b111;
	
	/*
	 * 74LS74 BGACK synchronizer / Mega ST blitter fix
	 *
	 * FIX: FF now uses active-LOW preset_n/clear_n matching real 74LS74.
	 * PRE_n = M68K_RESET_n  -> during reset (RESET_n=0), Q is preset to 1
	 * CLR_n = M68K_BGACK_n  -> when BGACK asserted (BGACK_n=0), Q is cleared to 0
	 * D     = M68K_BGACK_n  -> normal operation: Q follows BGACK_n on posedge clk
	 *
	 * Result: BGACKi=1 when no external master (BGACK_n=1)
	 *         BGACKi=0 when external master has bus (BGACK_n=0)
	 */
	FF ls74 ( M68K_BGACK_n, M68K_RESET_n, M68K_BGACK_n, c8m, BGACKi );
	
	wire BGACKi;

	/*
	 * Bus Arbitration instantiation (rev 252)
	 *
	 * Raw BR_n / BGACK_n in - BusArbSync synchronises both itself. BGACKi
	 * (above) stays the asynchronous "get off the pins NOW" signal.
	 */
	BusArbSync BArb (
		.clk      ( c8m ),
		.reset_n  ( M68K_RESET_n ),
		.br_n     ( M68K_BR_n ),
		.bgack_n  ( M68K_BGACK_n ),
		.bg_n     ( M68K_BG_n ),
		.bus_mine ( bus_mine )
	);


	/* Latch output enables: internal regs, pins gated by BGACKi (rev 252) */
	reg LTCH_A_OE_r							= HI;
	reg LTCH_D_WR_OE_r						= HI;

	/*
	 * rev 253: bus_drv - are we allowed to drive the bus at all? Like a
	 * 68000: yes while the bus is ours or one of our cycles is still out
	 * (AS_INT low), plus half a clock after our AS goes high so AS is
	 * driven negated before it is let go (own_q, posedge). Never while
	 * BGACK is low (BGACKi, asynchronous). Gates every control output and
	 * both latch output enables.
	 */
	reg  own_q								= HI;
	wire bus_drv							= BGACKi & (bus_mine | ~AS_INT | own_q);

	always @(posedge c8m) begin
		own_q								<= bus_mine | ~AS_INT;
	end

	assign LTCH_A_OE_n						= LTCH_A_OE_r    | ~bus_drv;
	assign LTCH_D_WR_OE_n					= LTCH_D_WR_OE_r | ~bus_drv;


	/* BUS TRANSFER STATE MACHINE */
	wire s0rst 								= s1;
	wire s1rst 								= s2 || oor;
	wire s2rst 								= s3 || oor;
	wire s3rst 								= s4 || oor;
	wire s4rst 								= s5 || oor;
	wire s5rst 								= s6 || oor;
	wire s6rst 								= s7 || oor;
	wire s7rst 								= s0 || oor;
	
	
	/*
	 * 680xx Bus-Cycle 
	 * starts when BGK is high
	 */
	 
	 /* S0 */
	 always @(posedge c8m, posedge s0rst) begin
	 
		if (s0rst)
		  s0 <= 1'd0;
		  
		//else if (s7 || oor)
		else if ((s7 || oor) && BGACKi)
		//else if ((s7 || oor) && BGACKi && M68K_BERR_n)// && op_req)
		  s0 <= 1'b1;
	end
	
	// S1 
	always @(negedge c8m, posedge s1rst) begin
	
		if (s1rst)
		  s1 <= 1'd0;
		
		else if (s0)
		//else if (s0 && BGACKi)
		//else if (s0 && BGACKi && PI_TXN_IN_PROGRESS)
		  s1 <= 1'd1;
	end
	
	// S2
	always @(posedge c8m, posedge s2rst) begin
	
		if (s2rst)
		  s2 <= 1'd0;
		
		else if (s1)
		//else if (s1 && BGACKi)
		//else if (s1 && PI_TXN_IN_PROGRESS)
		//else if (s1 && op_req)
		  s2 <= 1'd1;
	end
	
	// S3
	// checking PI_TXN_IN_PROGRESS here results in expected performance of 3.7/3.7
	always @(negedge c8m, posedge s3rst) begin
	
		if (s3rst)
		  s3 <= 1'd0;
		
		//else if (s2)
		//else if (s2 && PI_TXN_IN_PROGRESS)
		/* rev 252: start only while the bus is ours; a cycle whose AS is
		 * already out (AS_INT low) always runs to completion */
		else if (s2 && PI_TXN_IN_PROGRESS && ((bus_mine && BGACKi) || !AS_INT))
		  s3 <= 1'd1;
	end
	
	// S4
	always @(posedge c8m, posedge s4rst) begin
	
		if (s4rst)
		  s4 <= 1'd0;
		  
		else if (s3)
		  s4 <= 1'd1;
	end
	
	
	reg got_dtack = LO;
	
	/* 
	 * possible to miss DTACK assertion so latch DTACK when seen
	 * copied from Will James - Sinclair QL
	 */
`ifdef DIAG_BERR2
	/* while our AS is low: clocks (saturating at 16), DTACK seen, BR or
	 * BG seen. Captured at the posedge that closes the PI_BERR latch. */
	reg [4:0] c_len							= 5'd0;
	reg       c_dtk = 1'b0, c_arb = 1'b0;

	always @(posedge c8m) begin
		if (AS_INT) begin
			c_len							<= 5'd0;
			c_dtk							<= 1'b0;
			c_arb							<= 1'b0;
		end else begin
			if (!c_len[4])
				c_len						<= c_len + 5'd1;
			if (!M68K_DTACK_n)
				c_dtk						<= 1'b1;
			if (c_len == 5'd0)			/* first clock of our cycle */
				c_arb						<= !M68K_BR_n;
		end

		if (s5 && (!M68K_BERR_n || !BGACKi)) begin
			d_berr							<= !M68K_BERR_n;
			d_dtk							<= c_dtk | !M68K_DTACK_n;
			d_arb							<= c_arb;
			d_long							<= c_len[4];
		end
	end
`endif

	always @(*) begin

		if (s2 || s3 || s5)
			got_dtack = LO;

		else if ( s4 && !M68K_DTACK_n )
			got_dtack = HI;
	end
	
	/*
	 * S5 - wait for end-of-cycle acknowledgement
	 *
	 * FIX v2: added explicit s4 guard. Without it, a glitch on BERR_n
	 *         or a stale got_dtack during S0-S3 could cause s5 to be
	 *         set in the wrong bus state. The one-hot async reset chain
	 *         (s5rst = s6 || oor) provides partial protection, but a
	 *         combinational glitch between reset edges would still pass
	 *         through. Requiring s4 makes the condition unambiguous.
	 */
	always @(negedge c8m, posedge s5rst) begin

		if (s5rst)
		  s5 <= 1'd0;

		else if ( s4 && (got_dtack || !M68K_BERR_n || (!M68K_VMA_nr && M68K_E)))// == E9)) )
		  s5 <= 1'b1;
	end
	
	/* S6 */
	always @(posedge c8m, posedge s6rst) begin
	
		if (s6rst)
		  s6 <= 1'd0;
		  
		else if (s5)
		  s6 <= 1'd1;
	end
	
	/* S7 */
	always @(negedge c8m, posedge s7rst) begin
	
		if (s7rst)
		  s7 <= 1'd0;
		  
		else if (s6)
		  s7 <= 1'd1;
	end
	
	
	
	//reg [2:0] s6delay = 3'd0;
	//reg s2delay = LO;
	//reg [7:0] s6delay = 8'd0;
	//reg s6delay2 = LO;
	//reg s6delay3 = LO;
	//reg s6delay4 = LO;
	//reg s6delay5 = LO;
	//reg s6delay6 = LO;
	//reg s6delay7 = LO;
	//reg s6delay8 = LO;
	//reg s7delay = LO;
	
	/*
	 * rd_ok: "the current transaction is a READ and the Pi has not yet
	 * collected the data".  Armed at the falling edge of the ADDR_HI strobe
	 * when its R/W bit says read (PI_D stable at that edge, same as the
	 * op_rw latch), cleared by the falling edge of the REG_DATA strobe the
	 * Pi uses to collect the data (its TXN_END clear).  The Pi may assert
	 * REG_DATA + CMDWR at any time after ADDR_HI - during the cycle while it
	 * polls, or after TXN drops - the read latch is on PI_D either way.
	 * A write's DATA command (which precedes its own ADDR_HI) never sees
	 * rd_ok set, so it cannot put the read latch on PI_D against the Pi.
	 */
	reg  rd_ok								= 1'b0;

	always @(negedge CMDWR) begin
		case (PI_CMD)
			REG_ADDR_HI: rd_ok				<= PI_D[9];
			REG_DATA:    rd_ok				<= 1'b0;
			default: ;
		endcase
	end

	/* Data is driven on to the Bus in S6 */
	/* when clk is high Q follows D, then when clk goes low, Q is latched */
	/* 373 level d-type flip/flop */	
	/* 374 edge d-type flip/flop */
	
	always begin
	
		/* read latch on to PI_D: REG_DATA + strobe while rd_ok (see above).
		 * NOT keyed off op_rw: with the write order DATA, LO, HI the DATA
		 * command of a write arrives while op_rw still holds the previous
		 * transaction's value, and a write following a read would put the
		 * read latch on PI_D against the Pi's data - corrupted writes. */
		LTCH_D_RD_OE_n 						<= !(PI_CMD == REG_DATA && CMDWR && rd_ok);	
		
		LTCH_D_WR_U 						<= (PI_CMD == REG_DATA && CMDWR);
		LTCH_D_WR_L 						<= (PI_CMD == REG_DATA && CMDWR);
		
		LTCH_A_0 							<= (PI_CMD == REG_ADDR_LO && CMDWR);
		LTCH_A_8 							<= (PI_CMD == REG_ADDR_LO && CMDWR);
		LTCH_A_16 							<= (PI_CMD == REG_ADDR_HI && CMDWR);
		LTCH_A_24 							<= (PI_CMD == REG_ADDR_HI && CMDWR);	
	
	
		if ( CMDWR ) begin
		
			case (PI_CMD)
				
				REG_ADDR_LO: begin
				
					a0 						<= PI_D[0];
				end
				
				// rising edge on latches/flip-flops, latches upper address 16 bit (actually 8bits)
				REG_ADDR_HI: begin
				
					op_rw 					<= PI_D[9];
					op_uds_n 				<= PI_D[8] ? a0 : LO;
					op_lds_n 				<= PI_D[8] ? !a0 : LO;
					op_fc 					<= PI_D[15:13];
					
					PI_TXN_IN_PROGRESS		<= HI;
				end
				
				REG_STATUS: begin
				
					status 					<= PI_D;
				end
			endcase
		end
		
		
		
		if (s0) begin

			RW_INT = HI;
		//	state = S0;
		end


		/*
		 * S1: enable address latch output
		 *
		 * FIX v2: LTCH_A_OE_n moved here from S2 to provide address
		 *         setup time before AS assertion. The address data is
		 *         already loaded in the latches by the Pi (REG_ADDR_LO/HI
		 *         writes). Enabling OE here lets the address propagate
		 *         through the 74LS373 (~15ns tpd) so it is stable on
		 *         the bus well before AS goes low in S2.
		 */
		if (s1) begin

			//LTCH_A_OE_n = LO;
		//	state = S1;
		end
		
		//if (s2 && PI_TXN_IN_PROGRESS) begin			
		if (s2) begin

		//	s2delay <= LO;
			LTCH_A_OE_r <= LO;
			
			/* rev 252: no new cycle once BG is out */
			if (PI_TXN_IN_PROGRESS && bus_mine && BGACKi) begin
		
				FC_INT <= op_fc;					
				PI_BERR <= HI;
				RW_INT <= op_rw;			
			
				//LTCH_A_OE_n <= LO;
				
			//	s2delay <= HI;
			//	if (s2delay) begin
				
					AS_INT <= LO;
			//	end
				
				if ( op_rw ) begin
				
					UDS_INT <= op_uds_n;
					LDS_INT <= op_lds_n;
				end
			end
		end
		
		
		if (s3) begin
		
			LTCH_D_WR_OE_r <= op_rw;
		end
		
		
		if (s4) begin
			
			if ( !op_rw ) begin
			
				UDS_INT <= op_uds_n;
				LDS_INT <= op_lds_n;
			end
		end			
		
		
		if (s5) begin
		
`ifdef LS373
			LTCH_D_RD_U <= op_rw;
			LTCH_D_RD_L <= op_rw;
`endif
			/* rev 252: BGACK inside our own cycle = the bus was taken from
			 * under us; the DTACK that got us here was not for this cycle */
			PI_BERR <= M68K_BERR_n & BGACKi;
		end
		
		/* latch data */
		if (s6) begin
`ifdef LS374			
			LTCH_D_RD_U <= op_rw;
			LTCH_D_RD_L <= op_rw;	
				
			//PI_TXN_IN_PROGRESS <= LO;
`endif
`ifdef LS373
			LTCH_D_RD_U <= LO;
			LTCH_D_RD_L <= LO;
			
			//PI_TXN_IN_PROGRESS <= LO;
`endif			
		//	s7delay <= LO;
		end
	
	
		if (s7) begin
			
			AS_INT <= HI; /* deassert Address Strobe */
			UDS_INT <= HI;
			LDS_INT <= HI;
			
`ifdef LS374
			LTCH_D_RD_U <= LO;
			LTCH_D_RD_L <= LO;
`endif
			PI_TXN_IN_PROGRESS <= LO;
		end
		
		if (s7rst) begin
		
			LTCH_A_OE_r <= HI; /* address bus high impedance */
			LTCH_D_WR_OE_r <= HI; /* data bus high impedance */	
		end
	end
	
	
	wire vmarst = (s7 || oor);
	
	always @( posedge c8m, posedge vmarst ) begin
	
		if ( vmarst )
			M68K_VMA_nr <= HI;
		
		else if ( s4 && !M68K_VPA_n && !M68K_E ) //E2	
			M68K_VMA_nr <= LO;
	end

	
	/* rev 253: bus_drv, not BGACKi alone - see its definition */
	assign M68K_FC 							= bus_drv ? FC_INT		: 3'bzzz;
	assign M68K_AS_n 						= bus_drv ? AS_INT 		: 1'bz;
	assign M68K_UDS_n 						= bus_drv ? UDS_INT 		: 1'bz;
	assign M68K_LDS_n 						= bus_drv ? LDS_INT 		: 1'bz;
	assign M68K_RW 							= bus_drv ? RW_INT		: 1'bz;
	assign M68K_VMA_n						= bus_drv ? M68K_VMA_nr 	: 1'bz;
  
endmodule
