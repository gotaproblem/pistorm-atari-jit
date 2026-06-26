// SPDX-License-Identifier: MIT

#ifndef _CONFIG_FILE_H
#define _CONFIG_FILE_H

//#include "m68k.h"

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

//#pragma once

#define SIZE_KILO 1024
#define SIZE_MEGA (1024 * 1024)
#define SIZE_GIGA (1024 * 1024 * 1024)

#define M68K_CPU_TYPES 7


/* CPU types for use in m68k_set_cpu_type() */
enum
{
	M68K_CPU_TYPE_INVALID,
	M68K_CPU_TYPE_68000,
	M68K_CPU_TYPE_68010,
	M68K_CPU_TYPE_68020,
	M68K_CPU_TYPE_68030,
	M68K_CPU_TYPE_68040,
  M68K_CPU_TYPE_68060
};

typedef enum {
  CONFITEM_NONE,
  CONFITEM_CPU,
  CONFITEM_FPU,
  CONFITEM_LOOPCYCLES,
  CONFITEM_GRAPHICS_CARD,
  CONFITEM_FPS,
  CONFITEM_TTRAM,
  CONFITEM_RTC,
  CONFITEM_ROM,
  CONFITEM_IDE,
  CONFITEM_HDD,
  CONFITEM_FDD,
  CONFITEM_DMA_SOUND,
  CONFITEM_NUM,
} config_items;

typedef enum {
  NO_GRAPHICS_CARD,
  ET4000AX,
  ATI,
  MATROX,
  GRAPHICS_CARD_TYPES
} graphics_card;



typedef enum {
  NO_GRAPHICS_DRIVER,
  NOVA,
  XVDI,
  NVDI,
  FVDI,
  GRAPHICS_DRIVERS
} graphics_driver;

typedef struct ROM
{
  int rom_size;
  char rom_path [256];
  uint8_t *rom_ptr;
  uint32_t rom_address;
} ROM_s;

typedef struct HDD
{
  int drive;
  char img_path [256];
} HDD_s;

typedef struct FDD
{
  bool enabled;
  char img_path [256];
} FDD_s;

typedef struct VGA
{
  graphics_card card;
  graphics_driver driver;
} VGA_s;

struct emulator_config {
  uint8_t cpu_type;
  int loop_cycles;
  bool fpu;
  VGA_s graphics;
  int fps;
  bool ttram;
  bool rtc;
  ROM_s rom;//rom[2];
  //int rom_count;
  bool ide;
  HDD_s hdd [8];
  FDD_s fdd;
  bool dma_sound;
};


unsigned int get_m68k_cpu_type(char *name);

#ifdef __cplusplus
extern "C" {
#endif
struct emulator_config *load_config_file(char *filename);
#ifdef __cplusplus
}
#endif

void free_config_file(struct emulator_config *cfg);


#endif /* _CONFIG_FILE_H */

