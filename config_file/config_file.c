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


const char *cpu_types[M68K_CPU_TYPES] = {
  "NONE",
  "68000",
  "68010",
  "68020",
  "68030",
  "68040",
  "68060"
};

const char *config_item_names[CONFITEM_NUM] = {
  "NONE",
  "cpu",
  "fpu",
  "loopcycles",
  "vga",
  "fps",
  "ttram",
  "rtc",
  "rom",
  "ide",
  "hdd",
  "fdd",
  "dma_sound"
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

int get_config_item_type(char *cmd) {
  for (int i = 0; i < CONFITEM_NUM; i++) {
    if (strcmp(cmd, config_item_names[i]) == 0) {
      return i;
    }
  }

  return CONFITEM_NONE;
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

  int ret_int = 0;

  if (strlen(str) > 2 && str[0] == '0' && str[1] == 'x') {
    for (int i = 2; i < (int)strlen(str); i++) {
      if (str[i] >= '0' && str[i] <= '9') {
        ret_int = (str[i] - '0') | (ret_int << 4);
      }
      else {
        switch(str[i]) {
          case 'A': ret_int = 0xA | (ret_int << 4); break;
          case 'B': ret_int = 0xB | (ret_int << 4); break;
          case 'C': ret_int = 0xC | (ret_int << 4); break;
          case 'D': ret_int = 0xD | (ret_int << 4); break;
          case 'E': ret_int = 0xE | (ret_int << 4); break;
          case 'F': ret_int = 0xF | (ret_int << 4); break;
          case 'K': ret_int = ret_int * SIZE_KILO; break;
          case 'M': ret_int = ret_int * SIZE_MEGA; break;
          case 'G': ret_int = ret_int * SIZE_GIGA; break;
          default:
            printf ("[CFG] Unknown character %c in hex value.\n", str[i]);
            break;
        }
      }
    }
    return ret_int;
  }
  else {
    ret_int = atoi(str);
    if (str[strlen(str) - 1] == 'K')
      ret_int = ret_int * SIZE_KILO;
    else if (str[strlen(str) - 1] == 'M')
      ret_int = ret_int * SIZE_MEGA;
    else if (str[strlen(str) - 1] == 'G')
      ret_int = ret_int * SIZE_GIGA;

    return ret_int;
  }
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
  cfg->cpu_type = M68K_CPU_TYPE_68000;
  
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
      
      case CONFITEM_FPU:
        cfg->fpu = false;

        if (cfg->cpu_type == M68K_CPU_TYPE_68020)
          cfg->fpu = true;
        break;

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
        break;

      case CONFITEM_TTRAM:
        cfg->ttram = false;

        if (cfg->cpu_type != M68K_CPU_TYPE_68000)
          cfg->ttram = true;
        break;

      case CONFITEM_RTC:
        cfg->rtc = true;
        break;

      case CONFITEM_ROM:
        {
          //static int rom_count = 0;
          FILE *fp;

          //if (rom_count == 2)
          //{
          //  printf ("[CFG] too many ROMs requested\n");
          //  break;
          //}

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
/*
          else if (cfg->rom[rom_count].rom_size == 192 * 1024)
            cfg->rom[rom_count].rom_address = 0x00FC0000;

          else if (cfg->rom[rom_count].rom_size <= 128 * 1024)
            cfg->rom[rom_count].rom_address = 0x00FA0000;
*/
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

         // rom_count++;
         // cfg->rom_count = rom_count;
         /*
         for (int n = 0; n < 0x30; n++) {
              printf ("0x%02X ", cfg->rom.rom_ptr[n]);
              if (n % 16 == 0)
                  printf ("\n");
          }
          printf ("\n");
          */
        }
        break;

      case CONFITEM_IDE:
        cfg->ide = true; //get_int (parse_line + str_pos) + 1;
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
        {
          cfg->dma_sound = true;
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

