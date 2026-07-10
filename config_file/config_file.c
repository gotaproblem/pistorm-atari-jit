// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include "config_file.h"
#include "../platforms/atari/fdd/atari_fdd.h"
//#include "../platforms/atari/fdd/psg.h"
#include <fcntl.h>

extern void set_hard_drive_image_file_atari ( uint8_t, char* );


typedef enum {
  CONFITEM_NONE,
  CONFITEM_CPU,
  CONFITEM_JIT,
  CONFITEM_FPU,
  CONFITEM_LOOPCYCLES,
  CONFITEM_GRAPHICS_CARD,
  CONFITEM_FPS,
  CONFITEM_TTRAM,
  CONFITEM_ADDR32,
  CONFITEM_RTC,
  CONFITEM_ROM,
  CONFITEM_IDE,
  CONFITEM_HDD,
  CONFITEM_FDD,
  CONFITEM_DMA_SOUND,
  CONFITEM_BLITTER,
  CONFITEM_STRAM_CACHE,
  CONFITEM_STRAM_DIRECT,
  CONFITEM_VGA_RENDER,
  CONFITEM_NATIVE_HDMI,
  CONFITEM_CPU_CLOCK_MULTIPLIER,
  CONFITEM_M68K_SPEED,
  CONFITEM_JIT_CACHE,
  CONFITEM_NETWORK,
  CONFITEM_NETWORK_BACKEND,
  CONFITEM_NETWORK_TAP,
  CONFITEM_NETWORK_BASE,
  CONFITEM_NETWORK_MAC,
  CONFITEM_NETWORK_IRQ,
  CONFITEM_NETWORK_HOST_IP,
  CONFITEM_NETWORK_ATARI_IP,
  CONFITEM_NETWORK_NETMASK,
  CONFITEM_NETWORK_DEBUG,
  CONFITEM_HOSTFS,
} config_item;

typedef struct {
  const char *name;
  config_item item;
} config_switch_def;

static const struct emulator_config *current_config;

const char *cpu_types[M68K_CPU_TYPES] = {
  "NONE",
  "68000",
  "68010",
  "68020",
  "68030",
  "68040",
  "68060"
};

static const config_switch_def config_switches[] = {
  { "cpu", CONFITEM_CPU },
  { "jit", CONFITEM_JIT },
  { "fpu", CONFITEM_FPU },
  { "loopcycles", CONFITEM_LOOPCYCLES },
  { "vga", CONFITEM_GRAPHICS_CARD },
  { "fps", CONFITEM_FPS },
  { "ttram", CONFITEM_TTRAM },
  { "addr32", CONFITEM_ADDR32 },
  { "rtc", CONFITEM_RTC },
  { "rom", CONFITEM_ROM },
  { "ide", CONFITEM_IDE },
  { "hdd", CONFITEM_HDD },
  { "fdd", CONFITEM_FDD },
  { "dma_sound", CONFITEM_DMA_SOUND },
  { "blitter", CONFITEM_BLITTER },
  { "stram_cache", CONFITEM_STRAM_CACHE },
  { "stram_direct", CONFITEM_STRAM_DIRECT },
  { "vga_render", CONFITEM_VGA_RENDER },
  { "native_hdmi", CONFITEM_NATIVE_HDMI },
  { "cpu_clock_multiplier", CONFITEM_CPU_CLOCK_MULTIPLIER },
  { "m68k_speed", CONFITEM_M68K_SPEED },
  { "jit_cache", CONFITEM_JIT_CACHE },
  { "network", CONFITEM_NETWORK },
  { "network_backend", CONFITEM_NETWORK_BACKEND },
  { "network_tap", CONFITEM_NETWORK_TAP },
  { "network_base", CONFITEM_NETWORK_BASE },
  { "network_mac", CONFITEM_NETWORK_MAC },
  { "network_irq", CONFITEM_NETWORK_IRQ },
  { "network_host_ip", CONFITEM_NETWORK_HOST_IP },
  { "network_atari_ip", CONFITEM_NETWORK_ATARI_IP },
  { "network_netmask", CONFITEM_NETWORK_NETMASK },
  { "network_debug", CONFITEM_NETWORK_DEBUG },
  { "hostfs", CONFITEM_HOSTFS },
};

const char *graphics_card_types[GRAPHICS_CARD_TYPES] = {
  "NONE",
  "ET4000AX",
  "ATI",
  "MATROX"
};

const char *graphics_card_drivers[GRAPHICS_DRIVERS] = {
  "NONE",
  "NOVA",
  "XVDI",
  "NVDI",
  "FVDI"
};

char cfg_filename[256];

static config_item get_config_item_type(char *cmd) {
  for (size_t i = 0; i < sizeof(config_switches) / sizeof(config_switches[0]); i++) {
    if (strcmp(cmd, config_switches[i].name) == 0) {
      return config_switches[i].item;
    }
  }

  return CONFITEM_NONE;
}

void emulator_config_set_current(const struct emulator_config *cfg)
{
  current_config = cfg;
}

const struct emulator_config *emulator_config_current(void)
{
  return current_config;
}

bool emulator_config_blitter_enabled(void)
{
  return current_config ? current_config->blitter : true;
}

bool emulator_config_stram_cache_enabled(void)
{
  return current_config ? current_config->stram_cache : false;
}

bool emulator_config_stram_direct_enabled(void)
{
  return current_config ? current_config->stram_direct : false;
}

bool emulator_config_native_hdmi_enabled(void)
{
  return current_config ? current_config->native_hdmi : true;
}

bool emulator_config_display_enabled(void)
{
  return current_config ? current_config->graphics.card != NO_GRAPHICS_CARD : false;
}

bool emulator_config_et4k_enabled(void)
{
  return current_config &&
         current_config->graphics.card == ET4000AX &&
         current_config->graphics.driver != FVDI;
}

bool emulator_config_fvdi_enabled(void)
{
  return current_config &&
         current_config->graphics.card != NO_GRAPHICS_CARD &&
         current_config->graphics.driver == FVDI;
}

int emulator_config_graphics_driver(void)
{
  if (!current_config)
    return NO_GRAPHICS_DRIVER;
  return current_config->graphics.driver;
}

int emulator_config_fps(void)
{
  int fps = current_config ? current_config->fps : 0;
  if (fps < 10 || fps > 60)
    fps = 25;
  return fps;
}

char *uppercase ( char *str )
{
  for ( int n = 0; n < strlen ( str ); n++ )
  {
    str [n] = toupper ( str [n] );
  }

  return str;
}

unsigned int get_m68k_cpu_type(char *name) 
{
  for (int i = 0; i < M68K_CPU_TYPES; i++) 
  {
    if (strcmp(name, cpu_types[i]) == 0) 
    {
      printf ("[CFG] Set CPU type to %s\n", cpu_types[i]);
      return i;
    }
  }

  printf ("[CFG] Invalid CPU type %s specified, defaulting to 68000.\n", name);
  return M68K_CPU_TYPE_68000;
}

void trim_whitespace(char *str) {
  while (strlen(str) != 0 && (str[strlen(str) - 1] == ' ' || str[strlen(str) - 1] == '\t' || str[strlen(str) - 1] == 0x0A || str[strlen(str) - 1] == 0x0D)) {
    str[strlen(str) - 1] = '\0';
  }
}

unsigned int get_int(char *str) {
  if (strlen(str) == 0)
    return -1;

  while (*str == ' ' || *str == '\t')
    str++;

  char *end = NULL;
  unsigned long ret_int = strtoul(str, &end, 0);

  while (end && (*end == ' ' || *end == '\t'))
    end++;

  if (end && *end) {
    switch (toupper((unsigned char)*end)) {
      case 'K':
        ret_int *= SIZE_KILO;
        break;
      case 'M':
        ret_int *= SIZE_MEGA;
        break;
      case 'G':
        ret_int *= SIZE_GIGA;
        break;
      default:
        break;
    }
  }

  return (unsigned int)ret_int;
}

static int get_signed_int(char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;

  if (*str == '\0')
    return 0;

  char value[32];
  memset(value, 0, sizeof(value));
  size_t n = 0;
  while (str[n] && str[n] != ' ' && str[n] != '\t' && n + 1 < sizeof(value)) {
    value[n] = (char)tolower((unsigned char)str[n]);
    n++;
  }

  if (strcmp(value, "max") == 0 ||
      strcmp(value, "fast") == 0 ||
      strcmp(value, "fastest") == 0 ||
      strcmp(value, "turbo") == 0)
    return -1;

  return (int)strtol(str, NULL, 0);
}

static int get_size_kb(char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;

  if (*str == '\0')
    return 0;

  char *end = NULL;
  unsigned long value = strtoul(str, &end, 0);

  while (end && (*end == ' ' || *end == '\t'))
    end++;

  if (end && *end) {
    switch (toupper((unsigned char)*end)) {
      case 'G':
        value *= SIZE_MEGA;
        break;
      case 'M':
        value *= SIZE_KILO;
        break;
      case 'K':
      default:
        break;
    }
  }

  return (int)value;
}

static bool get_bool_default_true(char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;

  if (*str == '\0')
    return true;

  char value[32];
  memset(value, 0, sizeof(value));
  size_t n = 0;
  while (str[n] && str[n] != ' ' && str[n] != '\t' && n + 1 < sizeof(value)) {
    value[n] = (char)tolower((unsigned char)str[n]);
    n++;
  }

  return strcmp(value, "0") != 0 &&
         strcmp(value, "off") != 0 &&
         strcmp(value, "no") != 0 &&
         strcmp(value, "false") != 0 &&
         strcmp(value, "disabled") != 0 &&
         strcmp(value, "disable") != 0;
}

static int hostfs_drive_index(char drive)
{
  drive = (char)toupper((unsigned char)drive);
  if (drive >= 'A' && drive <= 'Z')
    return drive - 'A';
  if (drive >= '0' && drive <= '5')
    return 26 + drive - '0';
  return -1;
}

static bool is_bool_false(char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;

  char value[32];
  memset(value, 0, sizeof(value));
  size_t n = 0;
  while (str[n] && str[n] != ' ' && str[n] != '\t' && n + 1 < sizeof(value)) {
    value[n] = (char)tolower((unsigned char)str[n]);
    n++;
  }

  return strcmp(value, "0") == 0 ||
         strcmp(value, "off") == 0 ||
         strcmp(value, "no") == 0 ||
         strcmp(value, "false") == 0 ||
         strcmp(value, "disabled") == 0 ||
         strcmp(value, "disable") == 0;
}

static bool is_bool_true(char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;

  char value[32];
  memset(value, 0, sizeof(value));
  size_t n = 0;
  while (str[n] && str[n] != ' ' && str[n] != '\t' && n + 1 < sizeof(value)) {
    value[n] = (char)tolower((unsigned char)str[n]);
    n++;
  }

  return strcmp(value, "1") == 0 ||
         strcmp(value, "on") == 0 ||
         strcmp(value, "yes") == 0 ||
         strcmp(value, "true") == 0 ||
         strcmp(value, "enabled") == 0 ||
         strcmp(value, "enable") == 0;
}

void get_next_string(char *str, char *str_out, int *strpos, char separator) {
  int str_pos = 0, out_pos = 0, startquote = 0, endstring = 0;

  if (!str_out)
    return;

  if (strpos)
    str_pos = *strpos;

  while ((str[str_pos] == ' ' || str[str_pos] == '\t') && str_pos < (int)strlen(str)) {
    str_pos++;
  }

  if (str[str_pos] == '\"') {
    str_pos++;
    startquote = 1;
  }


  for (int i = str_pos; i < (int)strlen(str); i++) {
    str_out[out_pos] = str[i];

    if (startquote) {
      if (str[i] == '\"')
        endstring = 1;
    } else {
      if ((separator == ' ' && (str[i] == ' ' || str[i] == '\t')) || str[i] == separator) {
        endstring = 1;
      }
    }

    if (endstring) {
      str_out[out_pos] = '\0';
      if (strpos) {
        *strpos = i + 1;
      }
      break;
    }

    out_pos++;
    if (i + 1 == (int)strlen(str) && strpos) {
      *strpos = i + 1;
      str_out[out_pos] = '\0';
    }
  }
}


struct emulator_config *load_config_file(char *filename) {
  FILE *in = fopen(filename, "rb");
  if (in == NULL) {
    printf ("[CFG] Failed to open config file %s for reading.\n", filename);
    return NULL;
  }

  char *parse_line = NULL;
  char cur_cmd[128];
  struct emulator_config *cfg = NULL;
  int cur_line = 1;

  parse_line = (char *)calloc(1, 512);
  if (!parse_line) {
    printf ("[CFG] Failed to allocate memory for config file line buffer.\n");
    return NULL;
  }
  cfg = (struct emulator_config *)calloc(1, sizeof(struct emulator_config));
  if (!cfg) {
    printf ("[CFG] Failed to allocate memory for temporary emulator config.\n");
    free(cfg);
    cfg = NULL;
    return cfg;
  }

  memset(cfg, 0x00, sizeof(struct emulator_config));
  cfg->cpu_type = M68K_CPU_TYPE_68000 - 1;
  cfg->jit = true;
  cfg->blitter = true;
  cfg->vga_render = true;
  cfg->native_hdmi = true;
  
  while (!feof(in)) 
  {
    int str_pos = 0;
    memset(parse_line, 0x00, 512);
    fgets(parse_line, 512, in);

    if (strlen(parse_line) <= 2 || parse_line[0] == '#' || parse_line[0] == '/')
      goto skip_line;

    trim_whitespace(parse_line);

    get_next_string(parse_line, cur_cmd, &str_pos, ' ');

    switch (get_config_item_type (cur_cmd)) 
    {
      case CONFITEM_CPU:
        cfg->cpu_type = get_m68k_cpu_type(parse_line + str_pos) - 1;
        break;

      case CONFITEM_JIT:
        cfg->jit = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] JIT %s\n", cfg->jit ? "enabled" : "disabled");
        break;

      case CONFITEM_FPU:
        cfg->fpu = get_bool_default_true(parse_line + str_pos);
        break;

      // depricated
      case CONFITEM_LOOPCYCLES:
        cfg->loop_cycles = get_int(parse_line + str_pos);
        break;
      
      case CONFITEM_GRAPHICS_CARD:
        {
          cfg->graphics.card = 0;
          cfg->graphics.driver = 0;

          char next[128];
          memset(next, 0, 128);
          get_next_string(parse_line, next, &str_pos, ' ');

          for (int i = 0; i < GRAPHICS_CARD_TYPES; i++) 
          {
            if (strcmp(next, graphics_card_types [i]) == 0) 
            {
              printf ("[CFG] Set VGA card to %s\n", graphics_card_types[i]);
              cfg->graphics.card = i;

              memset(next, 0, 128);
              get_next_string(parse_line, next, &str_pos, ' ');

              for (int j = 0; j < GRAPHICS_DRIVERS; j++) 
              { 
                if (strcmp(next, graphics_card_drivers [j]) == 0) 
                {
                  printf ("[CFG] Set VGA driver to %s\n", graphics_card_drivers [j]);
                  cfg->graphics.driver = j;
                  break;
                }
              }
              break;
            }
          }
        }
        break;

      case CONFITEM_FPS:
        cfg->fps = get_int (parse_line + str_pos);
        /* valid frame rates are 50, 60, 70 */
        if (cfg->fps < 50 || cfg->fps > 120)
          cfg->fps = 50;
        printf ("[CFG] Set VGA FPS to %d Hz\n", cfg->fps);
        break;

      case CONFITEM_TTRAM:
        
        cfg->ttram = get_bool_default_true(parse_line + str_pos);
        if (cfg->ttram) {
          
          char *arg = parse_line + str_pos;
          while (*arg == ' ' || *arg == '\t')
            arg++;

          if (*arg == '\0' || is_bool_true(arg))
          {
            cfg->ttram_size = 128u * SIZE_MEGA;
          }
          else if (is_bool_false(arg))
          {
            cfg->ttram = false;
            cfg->ttram_size = 0;
          }
          else
          {
            cfg->ttram_size = get_int(arg);
          }
        }
        
        break;

      case CONFITEM_ADDR32:
        cfg->addr32 = get_bool_default_true(parse_line + str_pos);
        break;

      // depricated
      case CONFITEM_RTC:
        cfg->rtc = true;
        break;

      case CONFITEM_ROM:
        {
          FILE *fp;

          /* open file */
          strcpy (cfg->rom.rom_path, parse_line + str_pos);
          fp = fopen ( cfg->rom.rom_path, "rb" );

          if ( !fp )
          { 
            printf ( "[CFG] Failed to open rom image %s\n", cfg->rom.rom_path );

            break;
          }

          fseek ( fp, 0, SEEK_END );

          cfg->rom.rom_size = (int)ftell (fp);

          if (cfg->rom.rom_size >= 256 * 1024)
            cfg->rom.rom_address = 0x00E00000;

          else
          {
            printf ("[CFG] unexpected ROM size %d - can not load\n", cfg->rom.rom_size);
            break;
          }
          
          /* alloc mem */
          cfg->rom.rom_ptr = calloc (1, cfg->rom.rom_size);

          if (cfg->rom.rom_ptr == NULL) {
            perror("[CFG] ABORT - temp rom copy failed"); 
            abort();
          }
          /* read file and write to memory */
          fseek (fp, 0, 0);
          fread (cfg->rom.rom_ptr, 1, cfg->rom.rom_size, fp);
          fclose (fp);
          printf ("[CFG] %dK ROM image %s loaded\n", 
            cfg->rom.rom_size / 1024, cfg->rom.rom_path);
        }
        break;

      case CONFITEM_IDE:
        cfg->ide = get_bool_default_true(parse_line + str_pos);
        break;

      case CONFITEM_HDD:
        {
        static int idx = 0;

        if (idx < 8)
          set_hard_drive_image_file_atari ( idx++, parse_line + str_pos );
        }
        break;

      case CONFITEM_FDD:
        {
          cfg->fdd.enabled = true;
          strcpy (cfg->fdd.img_path, parse_line + str_pos);
        }
        break;

      case CONFITEM_DMA_SOUND:
        cfg->dma_sound = get_bool_default_true(parse_line + str_pos);;
        break;

      case CONFITEM_BLITTER:
        cfg->blitter = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] Blitter %s\n", cfg->blitter ? "enabled" : "disabled");
        break;

      case CONFITEM_STRAM_CACHE:
        cfg->stram_cache = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] ST-RAM cache %s\n", cfg->stram_cache ? "enabled" : "disabled");
        break;

      case CONFITEM_STRAM_DIRECT:
        cfg->stram_direct = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] ST-RAM direct %s\n", cfg->stram_direct ? "enabled" : "disabled");
        break;

      case CONFITEM_VGA_RENDER:
        cfg->vga_render = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] VGA render %s\n", cfg->vga_render ? "enabled" : "disabled");
        break;

      case CONFITEM_NATIVE_HDMI:
        cfg->native_hdmi = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] Native HDMI %s\n", cfg->native_hdmi ? "enabled" : "disabled");
        break;

      case CONFITEM_CPU_CLOCK_MULTIPLIER:
        cfg->cpu_clock_multiplier = (int)get_int(parse_line + str_pos);
        cfg->cpu_clock_multiplier_set = true;
        printf ("[CFG] CPU clock multiplier %d\n", cfg->cpu_clock_multiplier);
        break;

      case CONFITEM_M68K_SPEED:
        cfg->m68k_speed = get_signed_int(parse_line + str_pos);
        cfg->m68k_speed_set = true;
        printf ("[CFG] m68k_speed %d\n", cfg->m68k_speed);
        break;

      case CONFITEM_JIT_CACHE:
        cfg->jit_cache = get_size_kb(parse_line + str_pos);
        cfg->jit_cache_set = true;
        printf ("[CFG] JIT cache %dKB\n", cfg->jit_cache);
        break;

      case CONFITEM_NETWORK:
        cfg->network_enabled = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] Network %s\n", cfg->network_enabled ? "enabled" : "disabled");
        break;

      case CONFITEM_NETWORK_BACKEND:
        get_next_string(parse_line, cfg->network_backend, &str_pos, ' ');
        printf ("[CFG] Network backend %s\n", cfg->network_backend);
        break;

      case CONFITEM_NETWORK_TAP:
        get_next_string(parse_line, cfg->network_tap, &str_pos, ' ');
        printf ("[CFG] Network TAP %s\n", cfg->network_tap);
        break;

      case CONFITEM_NETWORK_BASE:
        cfg->network_base = get_int(parse_line + str_pos);
        printf ("[CFG] Network base 0x%06X\n", cfg->network_base);
        break;

      case CONFITEM_NETWORK_MAC:
        get_next_string(parse_line, cfg->network_mac, &str_pos, ' ');
        printf ("[CFG] Network MAC %s\n", cfg->network_mac);
        break;

      case CONFITEM_NETWORK_IRQ:
        cfg->network_irq_level = (uint8_t)get_int(parse_line + str_pos);
        printf ("[CFG] Network IRQ IPL%u\n", cfg->network_irq_level);
        break;

      case CONFITEM_NETWORK_HOST_IP:
        get_next_string(parse_line, cfg->network_host_ip, &str_pos, ' ');
        printf ("[CFG] Network host IP %s\n", cfg->network_host_ip);
        break;

      case CONFITEM_NETWORK_ATARI_IP:
        get_next_string(parse_line, cfg->network_atari_ip, &str_pos, ' ');
        printf ("[CFG] Network Atari IP %s\n", cfg->network_atari_ip);
        break;

      case CONFITEM_NETWORK_NETMASK:
        get_next_string(parse_line, cfg->network_netmask, &str_pos, ' ');
        printf ("[CFG] Network netmask %s\n", cfg->network_netmask);
        break;

      case CONFITEM_NETWORK_DEBUG:
        cfg->network_debug = get_bool_default_true(parse_line + str_pos);
        printf ("[CFG] Network debug %s\n", cfg->network_debug ? "enabled" : "disabled");
        break;

      case CONFITEM_HOSTFS:
        {
          char drive[8];
          char option[32];
          int idx;

          memset(drive, 0, sizeof(drive));
          memset(option, 0, sizeof(option));
          get_next_string(parse_line, drive, &str_pos, ' ');
          idx = drive[0] ? hostfs_drive_index(drive[0]) : -1;
          if (idx < 0) {
            printf ("[CFG] Invalid hostfs drive '%s' on line %d.\n", drive, cur_line);
            break;
          }

          while (parse_line[str_pos] == ' ' || parse_line[str_pos] == '\t')
            str_pos++;

          if (parse_line[str_pos] == '\0') {
            printf ("[CFG] Missing hostfs path for drive %c on line %d.\n",
                    (char)toupper((unsigned char)drive[0]), cur_line);
            break;
          }

          strncpy(cfg->hostfs[idx].path, parse_line + str_pos,
                  sizeof(cfg->hostfs[idx].path) - 1);
          cfg->hostfs[idx].path[sizeof(cfg->hostfs[idx].path) - 1] = '\0';
          trim_whitespace(cfg->hostfs[idx].path);

          char *last_space = strrchr(cfg->hostfs[idx].path, ' ');
          if (last_space) {
            strncpy(option, last_space + 1, sizeof(option) - 1);
            for (char *p = option; *p; p++)
              *p = (char)tolower((unsigned char)*p);
            if (strcmp(option, "readonly") == 0 || strcmp(option, "ro") == 0) {
              *last_space = '\0';
              trim_whitespace(cfg->hostfs[idx].path);
              cfg->hostfs[idx].readonly = true;
            } else if (strcmp(option, "readwrite") == 0 || strcmp(option, "rw") == 0) {
              *last_space = '\0';
              trim_whitespace(cfg->hostfs[idx].path);
              cfg->hostfs[idx].readonly = false;
            }
          }

          cfg->hostfs[idx].enabled = true;
          cfg->hostfs[idx].drive = (char)toupper((unsigned char)drive[0]);
          printf ("[CFG] HostFS %c: %s%s\n",
                  cfg->hostfs[idx].drive,
                  cfg->hostfs[idx].path,
                  cfg->hostfs[idx].readonly ? " readonly" : "");
        }
        break;

      case CONFITEM_NONE:
      default:
        printf ("[CFG] Unknown config item %s on line %d.\n", cur_cmd, cur_line);
        break;
    }

  skip_line:
    cur_line++;
  }

  if (cfg->cpu_type < M68K_CPU_TYPE_68020 - 1)
  {
    if (cfg->fpu)
      printf ("[CFG] FPU ignored: CPU does not support external FPU\n");
    cfg->fpu = false;

    if (cfg->ttram)
      printf ("[CFG] TT-RAM ignored: CPU is 24-bit only\n");
    cfg->ttram = false;
    cfg->ttram_size = 0;

    if (cfg->addr32)
      printf ("[CFG] 32-bit address space ignored: CPU is 24-bit only\n");
    cfg->addr32 = false;
  }
  else
  {
    if (cfg->fpu)
      printf ("[CFG] FPU enabled\n");

    if (cfg->ttram)
    {
      if (cfg->ttram_size == 0)
        cfg->ttram_size = 128u * SIZE_MEGA;
      if (cfg->ttram_size > 128u * SIZE_MEGA)
        cfg->ttram_size = 128u * SIZE_MEGA;
      cfg->addr32 = true;
      printf ("[CFG] TT-RAM enabled - %uMB\n", cfg->ttram_size >> 20);
    }

    if (cfg->addr32)
      printf ("[CFG] 32-bit address space enabled\n");
  }
  /*
  goto load_successful;

  load_failed:;
  if (cfg) {
    for (int i = 0; i < MAX_NUM_MAPPED_ITEMS; i++) {
      if (cfg->map_data[i])
        free(cfg->map_data[i]);
      cfg->map_data[i] = NULL;
    }
    free(cfg);
    cfg = NULL;
  }
  load_successful:;
  if (parse_line)
    free(parse_line);
*/
  return cfg;
}
