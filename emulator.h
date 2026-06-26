// SPDX-License-Identifier: MIT
/**
 * pistorm
 * emulator function declarations
 */

#ifndef _EMULATOR_H
#define _EMULATOR_H

// see feature_set_macros(7)
//#define _GNU_SOURCE

#include <stdint.h>


/*
 * DMA transfers
 */
typedef struct {

  uint16_t seccount;
  uint16_t data;
  uint16_t status;
  uint16_t mode;
  uint8_t  hi;
  uint8_t  mid;
  uint8_t  lo;
  uint32_t address;
  int     reading; // 1 = target->RAM, 0 = RAM->target
  int      target;  // 1 = ACSI, 0 = FDD
} DMA_s;

/* 
 * DMA Sound (STe)
 * 0x00FF8900, 01 - 0x00FF8903 - 13 (odd bytes)
 */
typedef struct {
  union 
  {
    uint8_t data [0x26];
    struct {
      uint8_t control1;                 // 0x00FF8900
      uint8_t control2;                 // 0x00FF8901
      uint8_t pad1;
      uint8_t frame_start_hi;           // 0x00FF8903
      uint8_t pad2;
      uint8_t frame_start_mid;          // 0x00FF8905
      uint8_t pad3;
      uint8_t frame_start_lo;           // 0x00FF8907
      uint8_t pad4;
      uint8_t frame_count_hi;
      uint8_t pad5;
      uint8_t frame_count_mid;
      uint8_t pad6;
      uint8_t frame_count_lo;
      uint8_t pad7;
      uint8_t frame_end_hi;
      uint8_t pad8;
      uint8_t frame_end_mid;
      uint8_t pad9;
      uint8_t frame_end_lo;             // 0x00FF8913

      uint8_t pad10 [12];

      uint8_t sound_mode_control1;      // 0x00FF8920
      uint8_t sound_mode_control2;      // 0x00FF8921

      uint16_t microwire_data_reg;      // 0x00FF8922
      uint16_t microwire_mask_register; // 0x00FF8924
    } map;
  };
} DMA_Sound_s;

/*
void write16(uint32_t address,uint16_t data);
uint16_t read16(uint32_t address);
void write8(uint32_t address,uint16_t data);
uint16_t read8(uint32_t address);
*/

void cpu_pulse_reset(void);
void cpu_set_fc(unsigned int A);

void m68ki_int_ack(uint8_t int_level);
uint16_t cpu_irq_ack(int level);

unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);

void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

//void stop_cpu_emulation(uint8_t disasm_cur);

#endif /* _EMULATOR_H */
