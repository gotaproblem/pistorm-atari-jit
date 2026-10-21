// SPDX-License-Identifier: MIT

/*
    Code reorganized and rewritten by 
    Niklas Ekström 2021 (https://github.com/niklasekstrom)
*/

#ifndef _PS_PROTOCOL_H
#define _PS_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 1 while a thread owns the bus and may be driving the GPIO bank
 * (ps_protocol.c). IPL samples from GPLEV0 taken while set may contain
 * driven data bits instead of the CPLD's IPL lines - discard them. */
extern volatile uint8_t ps_bus_active;
extern volatile int ps_gpip7_force;   /* GPIP7 monitor detect: 0 real, 1 mono, 2 colour */
extern volatile int      ps_gpip7_stable;   /* debounced: -1 none, 0 mono, 1 colour */
extern volatile unsigned ps_gpip7_glitches; /* single disagreeing reads discarded    */
extern volatile unsigned ps_gpip7_reads;    /* guest reads of the monitor-detect bit */

/* Sample the monitor-detect line properly before the guest's first read
 * decides the boot resolution. Call once, after the bus is up and before
 * the CPU thread starts. No-op if the bit is forced. */
void ps_gpip7_seed (void);

/* Take / release the bus exclusion a transaction takes, WITHOUT running a
 * transaction. ps_bus_trylock() returns 1 with the bus held (the caller
 * must then call ps_bus_unlock()) or 0 if a transaction owns it. This is
 * what makes ipl_task's GPLEV0 sample exclusive rather than merely
 * unlikely to collide - see ps_protocol.c. */
int  ps_bus_trylock (void);
void ps_bus_unlock (void);


#ifdef USING_PI_CLK
/* clock stuff */
#define PIN_CLK 4
#define CLK_KILL (1 << 5)
#define CLK_ENABLE (1 << 4)
#define CLK_BUSY (1 << 7)
#define CLK_PASSWD 0x5a000000
#define CLK_GP0_CTL 0x070
#define CLK_GP0_DIV 0x074
#define SET_GPIO_ALT(g, a)  \
  *(gpio + (((g) / 10))) |= \
      (((a) <= 3 ? (a) + 4 : (a) == 4 ? 3 : 2) << (((g) % 10) * 3))

#define PIN_RD (1 << 5)
#define PIN_WR (1 << 6)
#define PI_TXN_IN_PROGRESS 1
#else
//#define PIN_RD (1 << 4) // Sep 2026 new firmware does not use CMD_RD so frees up GPIO4 for other uses
#define PIN_ST_RESET (1 << 4) // GPIO4 INPUT: ST RESET, active low - CPLD firmware $256+
#define PIN_WR (1 << 1)
#define PI_TXN_IN_PROGRESS 1
#endif

/* commands */

#define REG_DATA    0x00 //(0 << 2)
#define REG_ADDR_LO 0x04 //(1 << 2)
#define REG_ADDR_HI 0x08 //(2 << 2)
#define REG_STATUS  0x0C //(3 << 2)

#define STATUS_BIT_HALT 1
#define STATUS_BIT_RESET 2
#define STATUS_BIT_INIT 3

#define LATCH_LS373 0
#define LATCH_LS374 1
#define LATCH_TYPE 0x0004
#define LATCH_TYPE_SET 0x0008

#define WRITE_BYTE 0x0100
#define WRITE_WORD 0x0000
#define READ_BYTE  0x0300
#define READ_WORD  0x0200

#if defined(PI5) && defined(USING_PI_CLK)
  #error "USING_PI_CLK (GPCLK0 on GPIO4) is not implemented for the Pi 5 / RP1"
#endif

#ifndef PI3
  #define BCM2708_PERI_BASE	0xFE000000
#else
  #define BCM2708_PERI_BASE 0x3F000000
#endif

#define BCM2708_PERI_SIZE 0x01000000

#define GPIO_ADDR 0x200000 /* GPIO controller */
#define GPCLK_ADDR 0x101000

#define GPIO_BASE (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */
#define GPCLK_BASE (BCM2708_PERI_BASE + 0x101000)

/* octal definitions */
#define GPIO_INPUT  00
#define GPIO_OUTPUT 01
#define GPIO_ALT_FN0 4

#ifdef USING_PI_CLK
#define FSEL0 GPIO_INPUT   // PI_TXN_IN_PROGRESS
#define FSEL1 GPIO_INPUT   // PI_IPL_ZERO
#define FSEL2 GPIO_OUTPUT  // PI_CMD 0
#define FSEL3 GPIO_OUTPUT  // PI_CMD 1
#define FSEL4 GPIO_ALT_FN0 // PI_CLK
//#define FSEL4 GPIO_OUTPUT // PI_CMD_RD
#define FSEL5 GPIO_OUTPUT  // PI_CMD_RD
#define FSEL6 GPIO_OUTPUT  // PI_CMD_WR
#define FSEL7 GPIO_INPUT   // PI_BERR
#define FSEL8 GPIO_OUTPUT
#define FSEL9 GPIO_OUTPUT
#else
#define FSEL0 GPIO_INPUT   // PI_TXN_IN_PROGRESS
#define FSEL1 GPIO_OUTPUT  // PI_CMD_WR
#define FSEL2 GPIO_OUTPUT  // PI_CMD 0
#define FSEL3 GPIO_OUTPUT  // PI_CMD 1
#define FSEL4 GPIO_INPUT   // PI_RESET from the CPLD (fw $256+); was PI_CMD_RD - must
                           // not be an output now the CPLD drives it
#define FSEL5 GPIO_INPUT   // PI_IPL1
#define FSEL6 GPIO_INPUT   // PI_IPL2
#define FSEL7 GPIO_INPUT   // PI_BERR
#define FSEL8 GPIO_OUTPUT
#define FSEL9 GPIO_OUTPUT
#endif

#define GPFSEL0_INPUT ((uint32_t)((FSEL0) | (FSEL1 << 3) | (FSEL2 << 6) | (FSEL3 << 9) | (FSEL4 << 12) | (FSEL5 << 15) | (FSEL6 << 18) | (FSEL7 << 21) | (GPIO_INPUT << 24) | (GPIO_INPUT << 27))) /* GPIO 0, 1, 5, 6 INPUTS */
#define GPFSEL1_INPUT 0x00000000 /* GPIO 10-19 all INPUTS */
#define GPFSEL2_INPUT 0x00000000 /* GPIO 20-23 all INPUTS */

#define GPFSEL0_OUTPUT ((uint32_t)((FSEL0) | (FSEL1 << 3) | (FSEL2 << 6) | (FSEL3 << 9) | (FSEL4 << 12) | (FSEL5 << 15) | (FSEL6 << 18) | (FSEL7 << 21) | (FSEL8 << 24) | (FSEL9 << 27))) /* GPIO 2, 3, 7-9 OUTPUTS */ 
#define GPFSEL1_OUTPUT 0x09249249 /* GPIO 10-19 OUTPUTS */
#define GPFSEL2_OUTPUT 0x00000249 /* GPIO 20-23 OUTPUTS */

#define NOP() asm("nop"); asm("nop"); asm("nop"); asm("nop"); asm("nop");



typedef union 
{
  struct 
  {
    uint16_t lo;
    uint16_t hi;
  };
  uint32_t address;

} t_a32;

uint8_t ps_read_8(uint32_t address);
uint8_t ps_read_8_fc(uint32_t address, uint8_t fc_value, uint8_t *berr_out);
uint16_t ps_read_16(uint32_t address);
uint16_t ps_read_16_fc(uint32_t address, uint8_t fc_value, uint8_t *berr_out);
uint32_t ps_read_32(uint32_t address);

void ps_write_8(uint32_t address, uint16_t data);
void ps_write_16(uint32_t address, uint16_t data);
void ps_write_32(uint32_t address, uint32_t data);

uint32_t ps_read_status_reg ( void );
void ps_write_status_reg (uint16_t value);

void ps_setup_protocol ( void );
void ps_reset_state_machine ( void );
void ps_pulse_reset ( void );
void ps_pulse_halt ( void );
void ps_write_latchtype ( uint16_t );
//uint16_t ps_fw_rd ( void );
void ps_read_ipl ( uint8_t* );
void ps_get_firmware_revision ( void );

/* Atari power sensing (status readback bits 10:0 = CPLD firmware revision,
 * bit 13 = M68K RESET_n). ps_fw_rev is the revision the CPLD answered at
 * start-up; a CPLD that stops answering it has lost its power - the ST's.
 * ps_power_poll: one sample for the main thread (0 alive, 1 power lost
 * confirmed, -1 no sample: the bus was busy). ps_power_wait: block until
 * the CPLD answers a valid revision with RESET_n high, stably. */
extern uint16_t ps_fw_rev;
int  ps_power_poll ( void );
void ps_power_wait ( void );

/* ST RESET on GPIO4 (CPLD firmware 1.0b, rev $420): the CPLD drives it low while
 * the ST holds RESET low - reset button, power-on reset - and never for the
 * Pi's own ps_pulse_reset. ps_st_reset_pin: -1 the firmware has no such pin
 * (or a PI_CLK build, where GPIO4 is the clock), 0 RESET high, 1 RESET low.
 * A plain GPLEV0 read: no bus lock, no transaction, any thread. */
/* Diagnostic CPLD firmware 1.0b-diag (readback fwrev $43F) latches the
 * cause of the last Pi bus error into status readback bits 15/14/12.
 * ps_berr_cause: -1 not that firmware, else PS_BERR_* bits. Takes the bus
 * lock (a status read): call only after the bus-erroring transaction. */
#define CPLD_FW_DIAG_BERR 0x43Fu
#define PS_BERR_GLUE   1   /* bit 15: M68K_BERR_n low - a real BERR from the ST */
#define PS_BERR_BGACK  2   /* bit 14: BGACK low inside our own cycle            */
#define PS_BERR_TENURE 4   /* bit 12: ...and the arbiter saw it as a DMA tenure */
int  ps_berr_cause ( void );

/* Page 2 of the diag readback (diag2 build): for the last bus error,
 *   bits 7:0 c8m clocks our AS had been low (255 = 255+), bit 8 DTACK
 *   seen, 9 BG out, 10 BR seen, 11 BGACK seen (all while our AS was low),
 *   12 VPA low; 15:13 = 101 marker. Returns -1 if not answering. One bus
 *   lock for select / read / deselect, so no other status reader ever
 *   sees page 2. */
int  ps_berr_detail ( void );

/* Small diag build 1.0b-diag2b (readback fwrev $43E): bits latched at the
 * last Pi bus error. ps_berr_diag2b: -1 if not that firmware, else the
 * PS_B2_* bits. One status read, so call only after the failing cycle. */
#define CPLD_FW_DIAG_BERR2 0x43Eu
#define PS_B2_GLUE  1   /* bit 15: the ST's BERR line                       */
#define PS_B2_DTACK 2   /* bit 14: DTACK seen while our AS was low          */
#define PS_B2_ARB   4   /* bit 12: BR or BG seen while our AS was low       */
#define PS_B2_LONG  8   /* bit 11: our AS had been low for 16+ clocks (2 us) */
#define PS_B2_C     16  /* diag2c ($43D): bit 12 (PS_B2_ARB) instead means BR_n
                           was already low at the first clock of our cycle */
#define CPLD_FW_DIAG_BERR2C 0x43Du
#define CPLD_FW_ARB_FIX1    0x43Cu  /* arbitration fix candidate + diag2c bits */
int  ps_berr_diag2b ( void );

#define CPLD_FW_RESET_PIN 0x256u   /* alpha 0.86a; released as 1.0b ($420) */
int  ps_st_reset_pin ( void );


#define read8 ps_read_8
#define read16 ps_read_16
#define read32 ps_read_32

#define write8 ps_write_8
#define write16 ps_write_16
#define write32 ps_write_32

#define write_reg ps_write_status_reg
#define read_reg ps_read_status_reg



#ifdef __cplusplus
}
#endif

#endif /* _PS_PROTOCOL_H */
