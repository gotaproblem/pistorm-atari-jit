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
#define PIN_RD (1 << 4)
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
#define FSEL4 GPIO_OUTPUT  // PI_CMD_RD
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
