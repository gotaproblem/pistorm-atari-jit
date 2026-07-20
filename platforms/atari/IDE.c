// SPDX-License-Identifier: MIT

//
//  IDE.c
//  Originally based on Omega's IDE emulation,
//  created by Matt Parsons on 06/03/2019.
//  Copyright © 2019 Matt Parsons. All rights reserved.
//

#define _LARGEFILE64_SOURCE 

#include "platforms/atari/IDE.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <endian.h>
#include "config_file/config_file.h"
//#include "atari-registers.h"
#include "platforms/atari/idedriver.h"

#define DEBUGPRINT 0
#if DEBUGPRINT
#define DEBUG_PRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...) ;
#endif

#define DEBUG_IDE
#ifdef DEBUG_IDE
#define DEBUG ; // printf
#else
#define DEBUG(...)
#endif

#define IDEBASE 0x00F00000
#define IDETOP  0x00F00100

/* Focused IDE access trace (make IDEDBG=1). Register names by offset so the
 * driver's probe sequence is readable. Status/alt-status reads are logged
 * only on VALUE CHANGE so a BSY spin-wait shows two lines, not thousands. */
#ifdef ATARI_IDE_DIAG
static const char *ide_regname(int base)
{
    switch (base) {
        case 0x00: return "DATA";
        case 0x05: return "ERR/FEAT";
        case 0x09: return "SECCNT";
        case 0x0d: return "SECNUM";
        case 0x11: return "CYLLO";
        case 0x15: return "CYLHI";
        case 0x19: return "DEVHEAD";
        case 0x1d: return "STAT/CMD";
        case 0x39: return "CTRL/ALT";
        default:   return "?";
    }
}
#define IDE_LOG_W(addr, port, base, val) \
    fprintf(stderr, "[IDEw] @%06X p%d %-8s <= %02X %s\n", \
            (unsigned)(addr), (port), ide_regname(base), (unsigned)((val)&0xff), \
            atariIDE[port] ? "" : "(NO DRIVE)")
#define IDE_LOG_R(addr, port, base, val) \
    fprintf(stderr, "[IDEr] @%06X p%d %-8s => %02X %s\n", \
            (unsigned)(addr), (port), ide_regname(base), (unsigned)((val)&0xff), \
            atariIDE[port] ? "" : "(NO DRIVE)")
#define IDE_LOG_STAT(addr, port, base, val) do {                 \
        static int last = -1;                                    \
        if (((val)&0xff) != last) {                              \
            last = (val)&0xff;                                   \
            fprintf(stderr, "[IDEr] @%06X p%d %-8s => %02X%s\n", \
                    (unsigned)(addr), (port), ide_regname(base), \
                    (unsigned)((val)&0xff),                      \
                    atariIDE[port] ? "" : " (NO DRIVE)");        \
        }                                                        \
    } while (0)
#else
#define IDE_LOG_W(a,p,b,v)    ((void)0)
#define IDE_LOG_R(a,p,b,v)    ((void)0)
#define IDE_LOG_STAT(a,p,b,v) ((void)0)
#endif


static struct ide_controller *atariIDE [4] = {NULL, NULL, NULL, NULL};


int atarifd;
char *atari_image_file[IDE_MAX_HARDFILES];
bool IDE_enabled;

struct ide_controller *get_ide ( int index ) 
{
  return atariIDE [index];
}


void set_hard_drive_image_file_atari ( uint8_t index, char *filename ) 
{
  if (atari_image_file[index] != NULL)
    free(atari_image_file[index]);

  atari_image_file[index] = calloc(1, strlen(filename) + 1);
  strcpy(atari_image_file[index], filename);
}

void InitIDE (void) 
{
  uint8_t num_IDE_drives = 0;
  int port = 0;

  for ( int i = 0; i < IDE_MAX_HARDFILES && port < 4; i++ ) 
  {
    port = (i / 2);

    if ( atari_image_file [i] ) 
    {
      atarifd = open ( atari_image_file[i], O_RDWR | O_LARGEFILE );

      if (atarifd != -1) 
      {
        if ( ! atariIDE [port] )
            atariIDE [port] = IDE_allocate ( "cf" );
      }

      if (atarifd == -1) 
      {
        printf ( "[IDE%d] HDD%d Image %s failed to open\n", port, i, atari_image_file[i] );
      } 
      
      else 
      {
        if ( strcmp ( atari_image_file [i] + ( strlen (atari_image_file [i] ) - 2 ), "st" ) == 0 )
        {
          //printf ( "[IDE%d] Attaching FDD image %s.\n", port, atari_image_file [i] );

          ide_attach_st ( atariIDE [port], i, atarifd );
          num_IDE_drives++;

          printf ( "[IDE%d] FDD Image %s attached as HDD%d\n", port, atari_image_file [i], i );
        }

        else if ( strcmp ( atari_image_file [i] + ( strlen (atari_image_file [i] ) - 3 ), "img" ) == 0 
          || strncmp ( atari_image_file [i], "/dev/loop0", 10 ) == 0 )
        {
          ide_attach_hdf ( atariIDE [port], i, atarifd );
          num_IDE_drives++;
          
          printf ("[IDE%d] HDD%d Image Attached %s\n", port, i, atari_image_file[i]);
        }
      }
    }
  }

  for ( int n = 0; n < 4; n ++ )
    if ( atariIDE [n] )
      IDE_reset_begin ( atariIDE [n] );


  if ( num_IDE_drives == 0 ) 
    IDE_enabled = false;

  else
    IDE_enabled = true;
}




void ide_dump_stats(void)
{
  static struct ide_stats_s last;
  if (ide_stats.sectors_read == last.sectors_read &&
      ide_stats.sectors_written == last.sectors_written &&
      ide_stats.commands == last.commands)
    return;                       /* quiet when idle */
  fprintf(stderr,
          "[IDE] rd=%u wr=%u words=%u polls=%u cmds=%u last:",
          ide_stats.sectors_read, ide_stats.sectors_written,
          ide_stats.data_words, ide_stats.status_polls, ide_stats.commands);
  for (unsigned i = 0; i < 8; i++)
    fprintf(stderr, " %02X",
            ide_stats.last_cmds[(ide_stats.cmd_idx + i) & 7]);
  fprintf(stderr, "\n");
  last = ide_stats;
}

void writeIDEB ( uint32_t address, unsigned int value ) 
{
  uint8_t IDE_action;
  int port;
  int base;

  port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */

  IDE_LOG_W(address, port, (int)(address - IDEBASE - (0x40 * port)), value);

  if ( atariIDE [port] )
  {
    base = address - IDEBASE - ( 0x40 * port );

    switch ( base )
    {
      case GFEAT_OFFSET:
        //DEBUG_PRINTF ("Write to GFEAT: %.2X.\n", value);
        IDE_action = IDE_feature_w;
        break;

      case GCMD_OFFSET:
        //DEBUG_PRINTF ("Write to GCMD: %.2X.\n", value);
        IDE_action = IDE_command_w;
        ide_stats.commands++;
        ide_stats.last_cmds[ide_stats.cmd_idx++ & 7] = (uint8_t)value;
        break;

      case GSECTCOUNT_OFFSET:
        IDE_action = IDE_sec_count;
        break;

      case GSECTNUM_OFFSET:
        IDE_action = IDE_sec_num;
        break;

      case GCYLLOW_OFFSET:
        IDE_action = IDE_cyl_low;
        break;

      case GCYLHIGH_OFFSET:
        IDE_action = IDE_cyl_hi;
        break;

      case GDEVHEAD_OFFSET:
        //DEBUG_PRINTF ("Write to GDEVHEAD: %.2X.\n", value);
        IDE_action = IDE_dev_head;
        break;

      case GCTRL_OFFSET:
        //DEBUG_PRINTF ("Write to GCTRL: %.2X.\n", value);
        IDE_action = IDE_devctrl_w;
       break;

      //case GIRQ_4000_OFFSET:
      //  IDE_a4k_irq = value;
        // Fallthrough
      //case GIRQ_OFFSET:
      //  IDE_irq = (IDE_irq & value) | (value & (IDE_IRQ_RESET | IDE_IRQ_BERR));
      //  return;
      default:
        //printf ( "%s: unserviced request 0x%x\n", __func__, ((address - IDEBASE)) );//IDE_IDE_base) - IDE_IDE_adj) );
        return;
    }

    //goto skip_idewrite8;

//IDEwrite8:
    IDE_write8 ( atariIDE [port], IDE_action, value );

    return;

//skip_idewrite8:
  //  return;
  }

#if (0)
  switch (address) {
    /*case 0xDD203A:
      DEBUG_PRINTF ("Write bye to A4000 IDE: %.2X\n", value);
      IDE_a4k = value;
      return;*/
   // case GIDENT:
      //DEBUG_PRINTF ("Write to GIDENT: %d\n", value);
   //   ataricounter = 0;
    //  return;
    case GCONF:
      //DEBUG_PRINTF ("Write to GCONF: %d\n", IDE_cfg);
      IDE_cfg = value;
      return;
    //case RAMSEY_REG:
      //ramsey_cfg = value & 0x0F;
     // return;
    case GINT:
      IDE_int = value;
      return;
    case GCS:
      IDE_cs_mask = value & ~3;
      IDE_cs &= ~3;
      IDE_cs |= value & 3;
      DEBUG_PRINTF ("Write to GCS: %d\n", IDE_cs);
      atariide0->selected = IDE_cs;
      return;
  }
  
#endif
}


void writeIDE ( uint32_t address, unsigned int value ) 
{
  static int port;
  static int base;

  port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */
  base = address - IDEBASE - ( 0x40 * port );

  if ( atariIDE [port] ) 
  {
    //if ( base == GDATA_OFFSET )
      IDE_write16 ( atariIDE [port], IDE_data, value );

    return;
  }

  //DEBUG("Write Word to IDE Space 0x%06x (0x%06x)\n", address, value);
}


void writeIDEL ( uint32_t address, unsigned int value ) 
{
  static int port;
  static int base;

  port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */
  base = address - IDEBASE - ( 0x40 * port );

  if ( atariIDE [port] ) 
  {
    if ( base == GDATA_OFFSET )
    {
      IDE_write16 ( atariIDE [port], IDE_data, value >> 16 ) ;
      IDE_write16 ( atariIDE [port], IDE_data, value & 0xffff );
    }
  }
  //DEBUG("Write Long to IDE Space 0x%06x (0x%06x)\n", address, value);
}


uint8_t readIDEB ( uint32_t address ) 
{
  //static int port;
  //static int base;
  static uint8_t IDE_action;

  int port = (address & 0xf0) >> 6;
  int base = address - IDEBASE - (0x40 * port);

  if (!atariIDE[port]) {
      /* Empty port. The status/alt-status registers must read BSY-clear or a
        * "spin while BSY" probe loops forever on 0xFF (bit7 = BSY). 0x00 =
        * not busy / not ready -> the driver falls through to its no-drive
        * path. Other registers keep the floating-bus 0xFF so the ATA
        * signature check still fails and reports "no device". */
      if (base == GSTATUS_OFFSET || base == GCTRL_OFFSET) {
          IDE_LOG_STAT(address, port, base, 0x00);
          return 0x00;
      }
      IDE_LOG_R(address, port, base, 0xFF);
      return 0xFF;
  }

  //port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */
  
  if ( atariIDE [port] ) 
  {
    //base = address - IDEBASE - ( 0x40 * port );
   
    switch ( base ) 
    {
      case GERROR_OFFSET:
        IDE_action = IDE_error_r;
        break;

      case GSTATUS_OFFSET:
        ide_stats.status_polls++;
        IDE_action = IDE_status_r;
        break;

      case GSECTCOUNT_OFFSET:
        IDE_action = IDE_sec_count;
        break;

      case GSECTNUM_OFFSET:
        IDE_action = IDE_sec_num;
        break;

      case GCYLLOW_OFFSET:
        IDE_action = IDE_cyl_low;
        break;

      case GCYLHIGH_OFFSET:
        IDE_action = IDE_cyl_hi;
        break;

      case GDEVHEAD_OFFSET:
        IDE_action = IDE_dev_head;
        break;
        
      case GCTRL_OFFSET:
        IDE_action = IDE_altst_r;
        break;

      // case GIRQ_4000_OFFSET:
      //case GIRQ_OFFSET:
      //  return 0x80;
        //IDE_irq = (IDE_irq & value) | (value & (IDE_IRQ_RESET | IDE_IRQ_BERR));
        //default:
        //  printf ( "%s: unserviced command = 0x%x\n", __func__, ((address - IDEBASE) ));//- IDE_IDE_adj) );
      default:
        return 0xFF;
    }

    {
      uint8_t rv = IDE_read8 ( atariIDE [port], IDE_action );
      if (base == GSTATUS_OFFSET || base == GCTRL_OFFSET)
        IDE_LOG_STAT(address, port, base, rv);   /* transition-logged */
      else
        IDE_LOG_R(address, port, base, rv);
      return rv;
    }
  }

  //DEBUG("Read Byte From IDE Space 0x%06x\n", address);
  return 0xFF;
}


uint16_t readIDE ( uint32_t address ) 
{
  static int port;
  static int base;

  port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */
  base = address - IDEBASE - ( 0x40 * port );

  if ( atariIDE [port] ) 
  {
    if ( base == GDATA_OFFSET ) 
    {
      ide_stats.data_words++;
      return IDE_read16 ( atariIDE [port], IDE_data );
    }

    //if (address == GIRQ_A4000) {
    //  IDE_a4k_irq = 0x8000;
    //  return 0x8000;
    //}
  }

  //DEBUG("Read Word From IDE Space 0x%06x\n", address);
  return 0x8000;
}


uint32_t readIDEL ( uint32_t address ) 
{
  static int port;
  static int base;
  static uint32_t value;
  
  port = (address & 0xf0) >> 6; /* get IDE interface number 0 - 3 */
  base = address - IDEBASE - ( 0x40 * port );

  if ( atariIDE [port] ) 
  {
    if ( base == GDATA_OFFSET ) 
    {
      ide_stats.data_words += 2;          /* long access = two words   */
      value = IDE_read16 ( atariIDE [port], IDE_data );
      
      return value << 16 | IDE_read16 ( atariIDE [port], IDE_data ) ;
    }
  }

  //DEBUG("Read Long From IDE Space 0x%06x\n", address);
  return 0x8000;
}
