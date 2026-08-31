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
#define HOSTFS_MAX_DRIVES 32
#define HOSTFS_PATH_MAX 256


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

typedef struct HOSTFS
{
  bool enabled;
  char drive;
  char path[HOSTFS_PATH_MAX];
  bool readonly;
} HOSTFS_s;

typedef struct VGA
{
  graphics_card card;
  graphics_driver driver;
} VGA_s;

struct emulator_config {
  uint8_t cpu_type;
  bool jit;
  bool cpu_compatible;   /* prefetch-accurate 68000 core (interpreter only) */
  int  mmu_model;        /* 0 = off, 68040 = full 040 MMU (interpreter only) */
  int  monitor_force;    /* MFP GPIP7 monitor detect: 0 real, 1 mono, 2 colour */
  int loop_cycles;
  bool fpu;
  VGA_s graphics;
  int fps;
  bool ttram;
  uint32_t ttram_size;
  bool addr32;
  bool rtc;
  ROM_s rom;//rom[2];
  //int rom_count;
  bool ide;
  HDD_s hdd [8];
  FDD_s fdd;
  bool dma_sound;
  bool ym2149;
  bool kbd_usb;        /* "kbd usb": inject USB/Bluetooth input into IKBD */
  bool kbd_grab;       /* grab evdev devices away from the Pi console     */
  int  kbd_mode;       /* 0 = auto-detect real IKBD, 1 = merge, 2 = standalone */
  int  kbd_mouse_div;  /* host mouse count divisor (1 = raw)              */
  bool blitter;
  bool blitter_set;    /* cfg had an explicit blitter line (override) */
  bool machine_set;    /* cfg 'machine ...' present */
  uint32_t machine_mch;/* forced _MCH value */
  int machine_kind;    /* 0=st 1=ste 2=megast (PiStorm-capable machines) */
  bool shifter_set;    /* cfg set shifter explicitly */
  bool shifter_ste;   /* shifter ste: mirror honours STE video regs ($FF820D/0F/8265); default ST ignores them like the real chip */
  bool blitter_real;   /* blitter=true: false = emulated (default), true = real chip */
  bool stram_cache;
  bool stram_direct;
  uint32_t stram_size;   /* PHYSICAL ST-RAM bytes (0 = flat 4MB model) */
  bool vga_render;
  bool native_hdmi;
  bool cpu_clock_multiplier_set;
  int cpu_clock_multiplier;
  bool m68k_speed_set;
  int m68k_speed;
  bool jit_cache_set;
  int jit_cache;
  bool network_enabled;
  char network_backend[16];
  char network_tap[64];
  uint32_t network_base;
  char network_mac[32];
  uint8_t network_irq_level;
  char network_host_ip[32];
  char network_atari_ip[32];
  char network_netmask[32];
  bool network_debug;
  HOSTFS_s hostfs[HOSTFS_MAX_DRIVES];
  char stbox_tos[256];   /* TOS ROM for the sandboxed ST (STBOX)        */
  int  stbox_plane;      /* force a DRM overlay plane id (0 = auto)     */
};


unsigned int get_m68k_cpu_type(char *name);

#ifdef __cplusplus
extern "C" {
#endif

struct emulator_config *load_config_file(char *filename);
void free_config_file(struct emulator_config *cfg);

void emulator_config_set_current(const struct emulator_config *cfg);
const struct emulator_config *emulator_config_current(void);
bool emulator_config_machine_set(uint32_t *mch);
int emulator_config_machine_kind(void); /* -1 unset, 0 st, 1 ste, 2 megast */
bool emulator_config_fpu(void);
int  emulator_config_monitor_force(void); /* GPIP7: 0 real, 1 mono, 2 colour */
bool emulator_config_shifter_ste(void);
bool emulator_config_blitter_enabled(void);
int  emulator_config_blitter_mode(void);  /* 0=off 1=real 2=emulated */
bool emulator_config_stram_cache_enabled(void);
bool emulator_config_stram_direct_enabled(void);
bool emulator_config_native_hdmi_enabled(void);
bool emulator_config_display_enabled(void);
bool emulator_config_et4k_enabled(void);
bool emulator_config_fvdi_enabled(void);
int emulator_config_graphics_driver(void);
int emulator_config_fps(void);
const char *emulator_config_stbox_tos(void);   /* "" if unset */
int emulator_config_stbox_plane(void);         /* 0 if unset  */
uint32_t emulator_config_stram_size(void);     /* bytes; 0 = flat 4MB */

#ifdef __cplusplus
}
#endif

#endif /* _CONFIG_FILE_H */
