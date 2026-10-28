/*
 * 2.2b (fwrev $542): 2.1b + the ST RESET button back for the Pi. GPIO4 is
 * PI_CLK since 2.0b, so the filtered "RESET held >= 2 us, not by the Pi"
 * signal (pi_reset_r, rev 256) is now status-word bit 12 (was HALT_n,
 * unused by the Pi). Bit 13 stays raw RESET_n (power sensing).
 */

/*
 * 2.1b (fwrev $541): 2.0b + FAST START (see as_fast). The cycle starts as
 * soon as TXN has been high 40-48 ns (address latches valid), anywhere from
 * S1 until S3 - as the 2024 3.91 MB/s design did - instead of only on the
 * rising edge after a falling-edge decision. Never within ~16 ns of a c8m
 * falling edge (arbiter). No PI_CLK: identical to 2.0b.
 */

/*
 * PiStorm SXB EPM570 firmware 2.0b (fwrev $540) - FAST CLOCK line.
 *
 * Base: 1.2e (hardware-verified 0/0 on ataritest --busprofile, 28 Oct 2026),
 * which is 1.1b plus:
 *   - start decided one half-clock earlier (falling edge entering S1);
 *   - address latch OE from S1 (AS must never fall before the address);
 *   - writes release the Pi at S6 (the write data/address latches hold
 *     until S7; nothing to race).
 *
 * 2.0b adds PI_CLK: GPCLK0 from the Pi on GPIO4 (CPLD pin 12 = GCLK0),
 * 62.5 MHz (PLLD/12 on a Pi 4, PLLD/8 on a Pi 3 - integer dividers, no
 * MASH jitter). It times the READ release only - see rd_rel. Everything
 * else still runs on the ST's 8 MHz clock.
 *
 * GPIO4 was PI_RESET (ST RESET to the Pi). The Pi reads RESET_n from the
 * status word instead (bit 13). The Pi only starts GPCLK0 when this
 * firmware (major >= 2) answers, so an older Pi binary leaves GPIO4 an
 * input and 2.0b behaves as 1.2e; a new Pi binary never drives GPIO4
 * against an older firmware's PI_RESET output.
 *
 * Simulated (Icarus, fwsim/): 0 bad, 0 address/data violations, RAM/ROM/
 * mixed, lock on/off, every PI_CLK phase, and with PI_CLK absent.
 */

/*
 * Oct 2026 1.2d (fwrev $524), EXPERIMENTAL. 1.2c + TXN released at S6 for
 * WRITES ONLY (reads keep S7, 62.5 ns after the '374 capture - 1.2b's
 * S6 release on reads raced that capture and corrupted reads).
 *
 * Why: 1.2c measured the same as 1.1b. With TXN at S7 the Pi cannot have
 * the next request in before the S1 decision (125 ns later), so the
 * early start never fires. A write has no read latch to race; its data
 * and address latches must hold until S7 (UDS/LDS, AS negate at S7
 * entry), and the Pi's next latch strobe comes >= ~110 ns after it sees
 * TXN drop (measured), i.e. after S7 entry. Verify with ataritest
 * --busprofile: the write check reads back through S7-released reads.
 */

/*
 * Oct 2026 1.2c (fwrev $523), EXPERIMENTAL - bus throughput. 1.1b + ONE
 * change (1.2b's change 16 alone; 1.2b's S6 TXN release is NOT here):
 *
 * 16. go decided one half-clock earlier, 68000 style: also on the falling
 *     edge that enters S1 (s0 still high), and as_on may rise on the
 *     rising edge that enters S2 (s1 still high). Same conditions as
 *     fix2 (TXN, bus_mine, BGACKi, br_clear, BR pin high); as_busy to
 *     the arbiter is still go | as_on. If TXN is not pending at S1 the
 *     engine parks in S2 and decides on later falling edges as in 1.1b.
 *
 * Why not 1.2b: releasing TXN on the S6 rising edge - the same edge that
 * clocks the read '374s - corrupted data (ataritest --busprofile data
 * checks, 28 Oct 2026: 1.2b 3462/32768 ROM reads and 1202 RAM writes
 * wrong; 1.1b 0/0). The Pi's GPLEV sample raced the latch outputs, the
 * class ps_protocol.c's "RE-SAMPLE AFTER THE HANDSHAKE" note describes.
 * TXN stays at S7 here, 62.5 ns after the capture.
 */

/*
 * Experimental
 */

/*
 * EPM570 build of 1.1b: identical logic and pinout, fwrev $521 (bit 8 =
 * EPM570), from version_reg_570.v. EPM240/EPM570 T100 are pin-compatible.
 *
 * Oct 2026 1.1b (fwrev $421): 1.0b + 68000-style cycle start and stricter
 * bus arbitration. Fixes the MFP bus errors seen during ACSI/floppy DMA
 * (HDDriver's GPIP / Timer C polls), measured with diagnostic builds:
 *
 *  A. GLUE BERR, AS low 2us+, no DTACK: a Pi cycle that began after the
 *     DMA had raised BR was never answered and the GLUE timed it out.
 *  B. BGACK inside our cycle: BG given while our AS was low and the GLUE
 *     took the bus without waiting for AS to rise.
 *
 * 10. br_clear = BR through ONE flop (the arbiter's br is two).
 * 11. BG is held off while our AS is out (as_busy); once given it stays.
 * 12. go    - start decision on the FALLING edge (68000 S1): parked in S2,
 *             TXN pending, bus_mine, BGACKi, br_clear and the BR pin high,
 *             AS not already out. FC, R/W and PI_BERR are set from it.
 * 13. as_on - AS (and UDS/LDS on a read) on the next RISING edge (68000
 *             S2), cleared at S7. AS_INT = ~as_on: no asynchronous latch
 *             drives AS any more.
 * 14. as_busy to the arbiter = go | as_on: once the decision is made, BG
 *     waits until our AS is out and back.
 * S3 follows S2 when as_on; S4-S7, data latching, VMA unchanged.
 * Tested on hardware (fix2 build): no MFP bus errors under TOS 1.04 /
 * 1.62 / 2.06, HDDriver on a real ACSI HDC, floppy DMA.
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
 * Bus Arbitration
 * bus grant should be asserted ASAP
 */		
	
	
module BusArbSync (
	input  wire clk,        // c8m - state changes on the FALLING edge
	input  wire reset_n,

	input  wire br_n,       // Bus Request,             raw pin, active low
	input  wire bgack_n,    // Bus Grant Acknowledge,   raw pin, active low
	input  wire as_busy,    // 1 = one of our cycles has AS asserted (fix 11)
	output wire br_clear,   // BR through one flop: 1 = not requested (fix 10)

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

	assign br_clear = br_s[0];

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
						bg_n     <= as_busy; /* fix 11: not while our AS is low */
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
					end else if (bg_n)
						bg_n <= as_busy;    /* fix 11: give BG once our AS is up;
											   once given it stays */
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
						bg_n  <= as_busy;
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
module pistormsxb_devEPM570(
    output reg     	PI_TXN_IN_PROGRESS, 	// GPIO0
	input           PI_CMD_WR,      		// GPIO1 command write
    input   [1:0]   PI_CMD,       			// GPIO[3..2]
	
	//input       	PI_CLK,     			// GPIO4
	//              GPIO4 was PI_CMD_RD - freed by the single-strobe interface, Sept 2026
	input           PI_CLK,         		// GPIO4 - GPCLK0 62.5 MHz from the Pi (2.0b)
	                                		//   (was PI_RESET: the ST RESET sense moves
	                                		//    to the status readback, bit 13)
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

	`include "version_reg_570.v"	/* 2.2b: fwrev $542 */


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
	wire br_clear;							/* BusArbSync: BR not requested, 1 flop (fix 10) */
	assign trigger							= (cmd_status && !CMDWR && !st_wr_release);

	/* bit 11 (raw GPIO19): arbitration active - BG out or foreign master */
	/* 2.2b: status readback moved below the reset filter (bit 12 = pi_reset_r) */
	
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
	reg  go								= 1'b0;	/* fix2: start decision, falling edge */
	reg  as_on								= 1'b0;	/* fix2: AS asserted, rising edge     */
	reg  fast_arm							= 1'b0;	/* 2.1b: fast start (PI_CLK) */
	reg  as_fast							= 1'b0;
	wire AS_INT								= ~(as_on | as_fast);	/* 2.1b: + fast start */
	reg UDS_INT;
	reg LDS_INT;
	reg RW_INT;
	reg cyc_rw								= 1'b1;	/* 1.2d: R/W of the cycle on the bus */

	/*
	 * 2.0b: READ RELEASE TIMED BY PI_CLK (62.5 MHz, 16 ns).
	 *
	 * The read '374s are clocked on the rising edge that enters S6 (as 1.1b).
	 * 1.1b released TXN at S7, 62.5 ns later - safe, but the Pi then cannot
	 * get the next request in before the next start decision, and every
	 * RAM read takes two ST slots (1000 ns). The 2024 200 MHz firmware
	 * released ~10 ns BEFORE the latch outputs had the new data (a race:
	 * the "re-sample after the handshake" history in ps_protocol.c).
	 *
	 * Here (s6 || s7) of a read is sampled on BOTH edges of PI_CLK (8 ns
	 * apart) and TXN drops RD_REL after S6 entry:
	 *   RD_REL 0: later of the 2nd rising/falling samples -> 24..32 ns
	 *   RD_REL 1: earlier of the 3rd samples               -> 32..40 ns  (default)
	 *   RD_REL 2: later of the 3rd samples                 -> 40..48 ns
	 * vs LS374 clock->Q 28 ns max (15-19 typ). The data is valid at the
	 * Pi's pads before TXN falls even at the datasheet maximum.
	 *
	 * No PI_CLK (a Pi binary that does not start GPCLK0): rd_rel never
	 * rises and every read is released at S7 exactly as 1.2e/1.1b.
	 * Writes keep 1.2e's S6 release. rd_rel stays high through S7, so the
	 * S7 fallback below cannot clear a NEW transaction's TXN.
	 */
	localparam RD_REL						= 1;
	wire       rd_hold						= (s6 || s7) && cyc_rw;
	reg  [2:0] rd_p							= 3'b000;	/* rising-edge samples  */
	reg  [2:0] rd_n							= 3'b000;	/* falling-edge samples */
	always @(posedge PI_CLK) rd_p <= {rd_p[1:0], rd_hold};
	always @(negedge PI_CLK) rd_n <= {rd_n[1:0], rd_hold};
	wire       rd_rel						= (RD_REL == 0) ? (rd_p[1] & rd_n[1]) :
											  (RD_REL == 1) ? (rd_p[2] | rd_n[2]) :
											                  (rd_p[2] & rd_n[2]);
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

	/* 2.0b: GPIO4 is PI_CLK now. 2.2b: pi_reset_r goes to the Pi in the
	 * status word instead, bit 12 (was HALT_n, which the Pi never read) */

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

	/* status readback (combinational):
	 *   15 IPL2  14 IPL1  13 RESET_n (raw)  12 ST RESET held, not by the Pi
	 *   (pi_reset_r, active low - 2.2b; was HALT_n)  11 ~bus_mine  10:0 fwrev */
	assign PI_D 							= trigger ? {PI_IPL2, PI_IPL1, M68K_RESET_n, pi_reset_r, ~bus_mine, fwrev} : 16'bz;
	
  
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
		.as_busy  ( go | as_on | fast_arm | as_fast ),
		.br_clear ( br_clear ),
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
	wire bus_drv							= BGACKi & (bus_mine | go | ~AS_INT | own_q);

	always @(posedge c8m) begin
		own_q								<= bus_mine | ~AS_INT;
	end

	assign LTCH_A_OE_n						= LTCH_A_OE_r    | ~bus_drv;
	assign LTCH_D_WR_OE_n					= LTCH_D_WR_OE_r | ~bus_drv;


	/*
	 * fix2: cycle start, 68000 style.
	 *   falling edge: decide (go) - bus ours, BR pin high, no BGACK, TXN pending
	 *   rising edge:  AS out (as_on) - S2
	 *   falling edge: S3 (s3 needs s2 && as_on)
	 * as_on is cleared by S7 (falling edge) or a reset/INIT (oor).
	 */
	wire as_clr								= s7 || oor;

	always @(negedge c8m, posedge oor) begin
		if (oor)
			go								<= 1'b0;
		else
			/* 1.2c (16): also at the falling edge entering S1 (s0 high) */
			go								<= (s0 || (s2 && !as_on)) && PI_TXN_IN_PROGRESS &&
											   bus_mine && BGACKi && br_clear &&
											   M68K_BR_n;	/* BR pin at this edge: a cycle never
															   starts once BR is out (go is a flop -
															   metastability has half a clock to
															   settle before as_on samples it) */
	end

	always @(posedge c8m, posedge as_clr) begin
		if (as_clr)
			as_on							<= 1'b0;
		/* 1.2c (16): s1 = the rising edge that enters S2 */
		else if ((s1 || s2) && go && BGACKi)
			as_on							<= 1'b1;
	end

	/*
	 * 2.1b: FAST START, timed by PI_CLK - the 2024 design's S2.
	 *
	 * as_on above starts a cycle only on a RISING edge, decided on the
	 * falling edge before it: a TXN that arrives 1 ns after that falling
	 * edge waits a whole clock, misses the ST's slot and costs 500 ns. The
	 * 2024 200 MHz firmware asserted AS the moment TXN arrived anywhere in
	 * its S2 (falling edge to falling edge) and reached 3.91 MB/s.
	 *
	 * Here, from S1 entry until S3 (the falling edge S3 is taken on), once
	 * TXN has been high FAST_DLY (the address '374s are clocked by the same
	 * ADDR_HI strobe that raised TXN: 28 ns max to valid address):
	 *   fast_arm (PI_CLK rising): FC, R/W, PI_BERR set up
	 *   as_fast  (next PI_CLK falling, 8 ns on): AS (+UDS/LDS on a read)
	 *   S3 follows on the next c8m falling edge (s3 needs as_on | as_fast).
	 * Same start conditions as go: bus ours, BGACK clear, BR clear and the
	 * BR pin high. Never armed within ~16 ns of a c8m FALLING edge (the
	 * arbiter's edge - BG and bus_mine change there - and the edge S3
	 * samples as_fast on): c8m is tracked in the PI_CLK domain.
	 * Without PI_CLK nothing here moves: as_on / go start every cycle.
	 * Cleared with as_on at S7 / INIT.
	 */
	localparam FAST_DLY						= 1;	/* TXN high before arming:
													   0 = 24-32 ns, 1 = 40-48 ns */
	reg  [2:0] txn_p						= 3'b000;
	reg  [2:0] txn_n						= 3'b000;
	always @(posedge PI_CLK) txn_p <= {txn_p[1:0], PI_TXN_IN_PROGRESS};
	always @(negedge PI_CLK) txn_n <= {txn_n[1:0], PI_TXN_IN_PROGRESS};
	wire       txn_old						= (FAST_DLY == 0) ? (txn_p[1] & txn_n[1]) :
											                    (txn_p[2] & txn_n[2]);
	/* c8m phase seen from PI_CLK: samples since the last c8m edge */
	reg  [2:0] c8_hi						= 3'd0;	/* rising-edge samples with c8m high */
	reg  [2:0] c8_lo						= 3'd0;	/* rising-edge samples with c8m low  */
	always @(posedge PI_CLK) begin
		c8_hi <= c8m ? ((c8_hi == 3'd7) ? c8_hi : c8_hi + 3'd1) : 3'd0;
		c8_lo <= c8m ? 3'd0 : ((c8_lo == 3'd7) ? c8_lo : c8_lo + 3'd1);
	end
	/* c8m high 62.5 ns = ~3.9 samples: from the 3rd sample on a falling
	 * edge is <= ~30 ns away; the 1st low sample is <= 16 ns after one */
	wire       fast_guard					= (c8m && c8_hi >= 3'd2) || (!c8m && c8_lo <= 3'd1);
	always @(posedge PI_CLK, posedge as_clr) begin
		if (as_clr)
			fast_arm						<= 1'b0;
		else if ((s1 || (s2 && !as_on)) && txn_old && !fast_guard &&
				 bus_mine && BGACKi && br_clear && M68K_BR_n)
			fast_arm						<= 1'b1;
	end
	always @(negedge PI_CLK, posedge as_clr) begin
		if (as_clr)
			as_fast							<= 1'b0;
		else if (fast_arm && !(c8m && c8_hi >= 3'd3))
			as_fast							<= 1'b1;
	end

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
		/* fix2: the start was decided on the previous falling edge (go) and
		 * AS went out on the rising edge (as_on) - S3 just follows */
		else if (s2 && (as_on || as_fast))	/* 2.1b: + fast start */
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
		/* 2.1b: FC / R/W as soon as the start is decided (fast_arm 8 ns
		 * before as_fast; go half a clock before as_on) - a fast start in
		 * S1 must not wait for S2 to set them */
		if ((s1 || s2) && (fast_arm || go) && !as_on) begin

			FC_INT <= op_fc;
			PI_BERR <= HI;
			RW_INT <= op_rw;
			cyc_rw <= op_rw;
		end

		/* 2.1b: a fast-started READ in S1 - UDS/LDS with AS */
		if (s1 && as_fast && op_rw) begin

			UDS_INT <= op_uds_n;
			LDS_INT <= op_lds_n;
		end

		if (s1) begin

			/* 1.2e: address latch OE from S1. With 1.2c's early go, as_on
			 * rises on the edge that enters S2 - the same moment the old
			 * "if (s2)" enabled the OE - so the address reached the bus
			 * ~20 ns AFTER AS fell and GLUE/MMU decoded the previous
			 * address. Simulation: this is 1.2d's write corruption. */
			LTCH_A_OE_r <= LO;
		//	state = S1;
		end
		
		//if (s2 && PI_TXN_IN_PROGRESS) begin			
		if (s2) begin

		//	s2delay <= LO;
			LTCH_A_OE_r <= LO;
			
			/* fix2: FC / R/W / PI_BERR from the registered start decision
			 * (falling edge); AS itself is as_on (rising edge) */
			if (go) begin
		
				FC_INT <= op_fc;					
				PI_BERR <= HI;
				RW_INT <= op_rw;			
				cyc_rw <= op_rw;		/* 1.2d: held until the next go */
			end

			/* read: UDS/LDS with AS (68000 S2) */
			if ((as_on || as_fast) && op_rw) begin
				
				UDS_INT <= op_uds_n;
				LDS_INT <= op_lds_n;
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
`endif
			/* 1.2d: a write releases the Pi here; a read waits for S7 */
			/* cyc_rw = THIS cycle's R/W (latched with go in S2);
			 * op_rw may already hold the next transaction's in S7 */
			if (!cyc_rw || rd_rel)		/* 2.0b: reads RD_REL after S6 */
				PI_TXN_IN_PROGRESS <= LO;
`ifdef LS373
			LTCH_D_RD_U <= LO;
			LTCH_D_RD_L <= LO;
			
			//PI_TXN_IN_PROGRESS <= LO;
`endif			
		//	s7delay <= LO;
		end
	
	
		if (s7) begin
			
			/* AS deasserts via as_on (cleared by s7) */
			UDS_INT <= HI;
			LDS_INT <= HI;
			
`ifdef LS374
			LTCH_D_RD_U <= LO;
			LTCH_D_RD_L <= LO;
`endif
			/* 1.2d: reads only - a write's TXN dropped at S6, and a new
			 * ADDR_HI may already be here, so a write must not clear it */
			if (cyc_rw && !rd_rel)		/* 2.0b: only if PI_CLK is absent */
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
