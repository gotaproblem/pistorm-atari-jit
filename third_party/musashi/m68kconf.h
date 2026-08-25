/* ======================================================================== */
/* ========================= LICENSING & COPYRIGHT ======================== */
/* ======================================================================== */
/*
 *                                  MUSASHI
 *                                Version 3.32
 *
 * A portable Motorola M680x0 processor emulation engine.
 * Copyright Karl Stenerud.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */



#ifndef M68KCONF__HEADER
#define M68KCONF__HEADER


/* Configuration switches.
 * Use M68K_OPT_SPECIFY_HANDLER for configuration options that allow callbacks.
 * M68K_OPT_SPECIFY_HANDLER causes the core to link directly to the function
 * or macro you specify, rather than using callback functions whose pointer
 * must be passed in using m68k_set_xxx_callback().
 */
#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2


/* ======================================================================== */
/* ============================== MAME STUFF ============================== */
/* ======================================================================== */

/* If you're compiling this for MAME, only change M68K_COMPILE_FOR_MAME
 * to M68K_OPT_ON and use m68kmame.h to configure the 68k core.
 */
#ifndef M68K_COMPILE_FOR_MAME
#define M68K_COMPILE_FOR_MAME      M68K_OPT_OFF
#endif /* M68K_COMPILE_FOR_MAME */


#if M68K_COMPILE_FOR_MAME == M68K_OPT_OFF


/* ======================================================================== */
/* ============================= CONFIGURATION ============================ */
/* ======================================================================== */

/* Turn ON if you want to use the following M68K variants */
#ifndef M68K_EMULATE_010
#define M68K_EMULATE_010            M68K_OPT_ON
#endif

#ifndef M68K_EMULATE_EC020
#define M68K_EMULATE_EC020          M68K_OPT_ON
#endif

#ifndef M68K_EMULATE_020
#define M68K_EMULATE_020            M68K_OPT_ON
#endif

#ifndef M68K_EMULATE_030
#define M68K_EMULATE_030            M68K_OPT_ON
#endif

#ifndef M68K_EMULATE_040
#define M68K_EMULATE_040            M68K_OPT_ON
#endif


/* If ON, the CPU will call m68k_read_immediate_xx() for immediate addressing
 * and m68k_read_pcrelative_xx() for PC-relative addressing.
 * If off, all read requests from the CPU will be redirected to m68k_read_xx()
 */
#ifndef M68K_SEPARATE_READS
#define M68K_SEPARATE_READS         M68K_OPT_OFF
#endif

/* If ON, the CPU will call m68k_write_32_pd() when it executes move.l with a
 * predecrement destination EA mode instead of m68k_write_32().
 * To simulate real 68k behavior, m68k_write_32_pd() must first write the high
 * word to [address+2], and then write the low word to [address].
 */
#ifndef M68K_SIMULATE_PD_WRITES
#define M68K_SIMULATE_PD_WRITES     M68K_OPT_OFF
#endif

/* If ON, CPU will call the interrupt acknowledge callback when it services an
 * interrupt.
 * If off, all interrupts will be autovectored and all interrupt requests will
 * auto-clear when the interrupt is serviced.
 */
#ifndef M68K_EMULATE_INT_ACK
#define M68K_EMULATE_INT_ACK        M68K_OPT_ON
#define M68K_INT_ACK_CALLBACK(A)    your_int_ack_handler_function(A)
#endif


/* If ON, CPU will call the breakpoint acknowledge callback when it encounters
 * a breakpoint instruction and it is running a 68010+.
 */
#ifndef M68K_EMULATE_BKPT_ACK
#define M68K_EMULATE_BKPT_ACK       M68K_OPT_OFF
#define M68K_BKPT_ACK_CALLBACK()    your_bkpt_ack_handler_function()
#endif


/* If ON, the CPU will monitor the trace flags and take trace exceptions
 */
#ifndef M68K_EMULATE_TRACE
#define M68K_EMULATE_TRACE          M68K_OPT_OFF
#endif


/* If ON, CPU will call the output reset callback when it encounters a reset
 * instruction.
 */
#ifndef M68K_EMULATE_RESET
#define M68K_EMULATE_RESET          M68K_OPT_OFF
#define M68K_RESET_CALLBACK()       your_reset_handler_function()
#endif

/* If ON, CPU will call the callback when it encounters a cmpi.l #v, dn
 * instruction.
 */
#ifndef M68K_CMPILD_HAS_CALLBACK
#define M68K_CMPILD_HAS_CALLBACK     M68K_OPT_OFF
#define M68K_CMPILD_CALLBACK(v,r)    your_cmpild_handler_function(v,r)
#endif


/* If ON, CPU will call the callback when it encounters a rte
 * instruction.
 */
#ifndef M68K_RTE_HAS_CALLBACK
#define M68K_RTE_HAS_CALLBACK       M68K_OPT_OFF
#define M68K_RTE_CALLBACK()         your_rte_handler_function()
#endif

/* If ON, CPU will call the callback when it encounters a tas
 * instruction.
 */
#ifndef M68K_TAS_HAS_CALLBACK
#define M68K_TAS_HAS_CALLBACK       M68K_OPT_OFF
#define M68K_TAS_CALLBACK()         your_tas_handler_function()
#endif

/* If ON, CPU will call the callback when it encounters an illegal instruction,
 * passing the opcode as argument. If the callback returns 1, then it gets
 * treated as a normal instruction, and the illegal exception in canceled. If it
 * returns 0, the exception occurs normally.
 * The callback looks like int callback(int opcode)
 * You should put M68K_OPT_SPECIFY_HANDLER here if you can to use it, otherwise
 * it will use a dummy default handler and you'll have to call
 * m68k_set_illg_instr_callback explicitly.
 */
#ifndef M68K_ILLG_HAS_CALLBACK
#define M68K_ILLG_HAS_CALLBACK      M68K_OPT_OFF
#define M68K_ILLG_CALLBACK(opcode)  your_op_illg_handler_function(opcode)
#endif

/* If ON, CPU will call the callback when it encounters a TRAP instruction,
 * passing the trap code as an argument. If the callback returns 1, then it's
 * considered handled and control passes back to the program. If it returns 0,
 * the exception is processed normally.
 * The callback looks like int callback(int trap)
 * You should put M68K_OPT_SPECIFY_HANDLER here if you want use it, otherwise
 * it uses a dummy default handler and you'll have to call
 * m68k_set_trap_instr_callback explicitly.
 */
#ifndef M68K_TRAP_HAS_CALLBACK
#define M68K_TRAP_HAS_CALLBACK  	M68K_OPT_OFF
#define M68K_TRAP_CALLBACK(trap)	your_op_trap_handler_function(trap)
#endif

/* If ON, CPU will call the set fc callback on every memory access to
 * differentiate between user/supervisor, program/data access like a real
 * 68000 would.  This should be enabled and the callback should be set if you
 * want to properly emulate the m68010 or higher. (moves uses function codes
 * to read/write data from different address spaces)
 */
#ifndef M68K_EMULATE_FC
#define M68K_EMULATE_FC             M68K_OPT_OFF
#define M68K_SET_FC_CALLBACK(A)     your_set_fc_handler_function(A)
#endif

/* If ON, CPU will call the pc changed callback when it changes the PC by a
 * large value.  This allows host programs to be nicer when it comes to
 * fetching immediate data and instructions on a banked memory system.
 */
#ifndef M68K_MONITOR_PC
#define M68K_MONITOR_PC             M68K_OPT_OFF
#define M68K_SET_PC_CALLBACK(A)     your_pc_changed_handler_function(A)
#endif


/* If ON, CPU will call the instruction hook callback before every
 * instruction.
 */
#ifndef M68K_INSTRUCTION_HOOK
#define M68K_INSTRUCTION_HOOK       M68K_OPT_OFF
#define M68K_INSTRUCTION_CALLBACK(pc) your_instruction_hook_function(pc)
#endif


/* If ON, the CPU will emulate the 4-byte prefetch queue of a real 68000 */
#ifndef M68K_EMULATE_PREFETCH
#define M68K_EMULATE_PREFETCH       M68K_OPT_ON
#endif


/* If ON, the CPU will generate address error exceptions if it tries to
 * access a word or longword at an odd address.
 * NOTE: This is only emulated properly for 68000 mode.
 */
#ifndef M68K_EMULATE_ADDRESS_ERROR
#define M68K_EMULATE_ADDRESS_ERROR  M68K_OPT_ON
#endif


/* Turn ON to enable logging of illegal instruction calls.
 * M68K_LOG_FILEHANDLE must be #defined to a stdio file stream.
 * Turn on M68K_LOG_1010_1111 to log all 1010 and 1111 calls.
 */
#ifndef M68K_LOG_ENABLE
#define M68K_LOG_ENABLE             M68K_OPT_OFF
#define M68K_LOG_1010_1111          M68K_OPT_OFF
#define M68K_LOG_TRAP               M68K_OPT_OFF
#define M68K_LOG_FILEHANDLE         some_file_handle
#endif

/*
 * Emulate PMMU: if you enable this, there will be a test to see if the current
 * chip has some enabled PMMU added to every memory access, so enable this only
 * if it's useful.
 */
#ifndef M68K_EMULATE_PMMU
#define M68K_EMULATE_PMMU           M68K_OPT_ON
#endif

/* ----------------------------- COMPATIBILITY ---------------------------- */

/* The following options set optimizations that violate the current ANSI
 * standard, but will be compliant under the forthcoming C9X standard.
 */


/* If ON, the enulation core will use 64-bit integers to speed up some
 * operations.
*/
#ifndef M68K_USE_64_BIT
#define M68K_USE_64_BIT             M68K_OPT_ON
#endif


#endif /* M68K_COMPILE_FOR_MAME */

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */

#endif /* M68KCONF__HEADER */

/* ======================================================================== */
/* PISTORM-ATARI STBOX INTEGRATION (local addition, not upstream)           */
/*                                                                          */
/* This tree's emulator.c already exports extern "C" m68k_read_memory_*     */
/* etc. - they are the MAIN machine's bus accessors, inherited from the     */
/* original PiStorm's use of Musashi, and the JIT calls them. The sandbox   */
/* must neither collide with them at link time nor ever reach the real     */
/* bus, so the whole Musashi bus interface is renamed here. m68k.h          */
/* includes this file before declaring the prototypes, so declarations,     */
/* internal call sites and our definitions in platforms/atari/stbox/ all    */
/* follow the rename together.                                              */
/* ======================================================================== */
#define m68k_read_memory_8        stbox_bus_r8
#define m68k_read_memory_16       stbox_bus_r16
#define m68k_read_memory_32       stbox_bus_r32
#define m68k_write_memory_8       stbox_bus_w8
#define m68k_write_memory_16      stbox_bus_w16
#define m68k_write_memory_32      stbox_bus_w32
#define m68k_read_disassembler_8  stbox_dasm_r8

/* STBOX is a plain-ST 68000. With the 040/FPU emulation compiled in,
 * F-line opcodes that match FPU encodings EXECUTE as FPU instructions
 * (swallowing their extension words) instead of raising vector 11 -
 * empirically verified: $F624 sailed through on CPU_TYPE_68000. TOS
 * 1.04's AES is built on Line-F trampolines, so this derailed the
 * instruction stream on innocent desktop clicks (two bombs, reboot).
 * Compile the box as 68000-generation only. */
#undef  M68K_EMULATE_010
#define M68K_EMULATE_010            M68K_OPT_OFF
#undef  M68K_EMULATE_EC020
#define M68K_EMULATE_EC020          M68K_OPT_OFF
#undef  M68K_EMULATE_020
#define M68K_EMULATE_020            M68K_OPT_OFF
#undef  M68K_EMULATE_030
#define M68K_EMULATE_030            M68K_OPT_OFF
#undef  M68K_EMULATE_EC030
#define M68K_EMULATE_EC030          M68K_OPT_OFF
#undef  M68K_EMULATE_040
#define M68K_EMULATE_040            M68K_OPT_OFF
#undef  M68K_EMULATE_EC040
#define M68K_EMULATE_EC040          M68K_OPT_OFF

/* instruction hook: PC history ring for the STBOX halt report. At a
 * throttled 8 MHz the cost is noise; the forensics are not. */
#ifdef __cplusplus
extern "C" void stbox_pc_hook(unsigned int pc);
#else
extern void stbox_pc_hook(unsigned int pc);
#endif
#undef  M68K_INSTRUCTION_HOOK
#define M68K_INSTRUCTION_HOOK       M68K_OPT_SPECIFY_HANDLER
#undef  M68K_INSTRUCTION_CALLBACK
#define M68K_INSTRUCTION_CALLBACK(pc) stbox_pc_hook(pc)
#define m68k_read_disassembler_16 stbox_dasm_r16
#define m68k_read_disassembler_32 stbox_dasm_r32
