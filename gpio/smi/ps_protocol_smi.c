// SPDX-License-Identifier: MIT

/*
  Original Copyright 2020 Claude Schwarz
  Code reorganized and rewritten by
  Niklas Ekström 2021 (https://github.com/niklasekstrom)
*/

/*
 * S.Bradford aka cryptodad
 * 2023, 2024, 2025, 2026 Jeeez really!
 * 
 * rewritten for the ATARI PiSTorm project
 */

/*
 * SMI (Secondary Memory Interface) backend added.
 * Build with -DSMI to replace GPIO bit-banging with hardware-timed SMI cycles.
 * Without -DSMI the original GPIO path is compiled unchanged.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ps_protocol.h"
#include "../m68k.h"



/* MUST match firmware - clear output bits only - bits to clear = 1 */
#define TXN_END 0xFFFFFF1E
#define CHECK_BERR(x) ( !((x & 0x80) >> 7) )
#define CHECK_IPL(x)  ( (x & 0x60) >> 4 )

volatile uint8_t  g_buserr;
volatile uint32_t g_status;

uint8_t fc;


/* =========================================================================
 * GPIO PATH  (compiled when SMI is NOT defined)
 * ========================================================================= */
#ifndef SMI

volatile uint32_t *gpio;
volatile uint32_t *ioset;
volatile uint32_t *ioclr;
volatile uint32_t *ioread;


static
void create_dev_mem_mapping ()
{
  int fd = open ( "/dev/mem", O_RDWR | O_SYNC );

  if ( fd < 0 )
  {
    printf ( "Unable to open /dev/mem.\n" );
    exit ( -1 );
  }

  void *gpio_map = mmap (
      NULL,
      BCM2708_PERI_SIZE,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      fd,
      BCM2708_PERI_BASE
  );

  close(fd);

  if ( gpio_map == MAP_FAILED )
  {
    printf ( "mmap failed, errno = %d\n", errno );
    exit ( -1 );
  }

  gpio   = ((volatile uint32_t *)gpio_map) + (GPIO_ADDR / 4);
  ioset  = gpio + (0x1c / 4); // GPSET0
  ioclr  = gpio + (0x28 / 4); // GPCLR0
  ioread = gpio + (0x34 / 4); // GPLEV0
}

/*
GPIO
gpio + 0  = GPFSEL0 GPIO Function Select 0
gpio + 1  = GPFSEL1 GPIO Function Select 1
gpio + 2  = GPFSEL2 GPIO Function Select 2
gpio + 7  = GPSET   GPIO Pin Output Set 0
gpio + 10 = GPCLR   GPIO Pin Output Clear 0
gpio + 13 = GPLEV   GPIO Pin Level 0
gpio + 16 = GPEDS   GPIO Pin Event Detect Status 0
gpio + 19 = GPREN   GPIO Pin Rising Edge Detect Enable 0
gpio + 22 = GPFEN   GPIO Pin Falling Edge Detect Enable 0
gpio + 25 = GPHEN   GPIO Pin High Detect Enable 0
gpio + 28 = GPLEN   GPIO Pin Low Detect Enable 0
gpio + 31 = GPAREN  GPIO Pin Asynchronous Rising Edge Detect Enable 0
gpio + 34 = GPAFEN  GPIO Pin Asynchronous Falling Edge Detect Enable 0
*/


static
void setup_gpclk ( int verbose )
{
  int cpuf, coref;
  FILE *fp;
  char junk[80];
  char *ptr;

  fp = popen ( "vcgencmd measure_clock arm", "r" );
  fgets ( junk, sizeof (junk), fp );
  pclose ( fp );

  ptr = strchr ( junk, '=' );
  sscanf ( (ptr + 1), "%d", &cpuf );
  cpuf /= 1000000;

  fp = popen ( "vcgencmd measure_clock core", "r" );
  fgets ( junk, sizeof (junk), fp );
  pclose ( fp );

  ptr = strchr ( junk, '=' );
  sscanf ( (ptr + 1), "%d", &coref );
  coref /= 1000000;

  printf ( "[INIT] CPU clock is %d MHz\n", cpuf );
  printf ( "[INIT] CORE clock is %d MHz\n", coref );
}


void ps_setup_protocol ( void )
{
  create_dev_mem_mapping ();
  setup_gpclk ( verbose );

  gpio [0] = GPFSEL0_OUTPUT;
  gpio [1] = GPFSEL1_OUTPUT;
  gpio [2] = GPFSEL2_OUTPUT;

  *ioclr = TXN_END;
}


/*
 * Atari ST 8MHz CPU clock
 * clock cycle = 125ns
 * half cycle (pulse) = 62.5ns
 *
 * The following reads and writes are optimised to make sure address and data are written
 * to meet bus-cycle requirements. That means address setup has to be done within one cycle (125ns),
 * likewise data too. The Pi4 should easily be capable of doing this.
 */

volatile uint32_t rd_spins;
volatile uint32_t wr_spins;

inline
void ps_write_16 ( uint32_t address, uint16_t data )
{
  /*
   * Pi4 GPIO writes ~3.5ns, reads ~100ns
   * Pi3 GPIO writes ~7.5ns
   */

  *ioset = ( data << 8 ) | REG_DATA | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = ( (address & 0xffff) << 8 ) | REG_ADDR_LO | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  /*
   * Atari ST uses a 24-bit address; spare 8 bits carry FC and transfer type:
   *   bits 7,6,5 - FC
   *   bit  1     - write=0, read=1
   *   bit  0     - word=0,  byte=1
   *
   * WRITE BYTE = 0x01  WRITE WORD = 0x00
   * READ  BYTE = 0x03  READ  WORD = 0x02
   */

  /* write 0x00 */
  *ioset = ( ( (fc << 13) | WRITE_WORD | (address >> 16) ) << 8 ) | REG_ADDR_HI | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  //while ( ( g_status = *ioread ) & PI_TXN_IN_PROGRESS );
  uint32_t spins = 0;
  while ( ( g_status = *ioread ) & PI_TXN_IN_PROGRESS )  {if (spins == 8) break; spins++;}
  //if (spins > 0) wr_spins = spins; // track average

  /*
   * Mar 2026 Tuxie identified BERR bug
   * do not clear g_buserr here; that is handled in bus error handling
   */
  if ( CHECK_BERR (g_status) )
    g_buserr = 1;
}


inline
void ps_write_8 ( uint32_t address, uint16_t data )
{
  if ( (address & 1) == 0 )
    data <<= 8;
  else
    data &= 0xff;

  *ioset = data << 8 | REG_DATA | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = ((address & 0xffff) << 8) | REG_ADDR_LO | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = (( (fc << 13) | WRITE_BYTE | (address >> 16) ) << 8) | REG_ADDR_HI | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  while ( ( g_status = *ioread ) & PI_TXN_IN_PROGRESS );

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;
}


inline
void ps_write_32 ( uint32_t address, uint32_t value )
{
  ps_write_16 ( address,     value >> 16 );
  ps_write_16 ( address + 2, value );
}


inline
uint16_t ps_read_16 ( uint32_t address )
{
  *ioset = ( (address & 0xffff) << 8 ) | REG_ADDR_LO | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = ( ( (fc << 13) | READ_WORD | (address >> 16) ) << 8 ) | REG_ADDR_HI | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = REG_DATA | PIN_RD;

  //while ( *ioread & PI_TXN_IN_PROGRESS );
  uint32_t spins = 0;
  while ( ( g_status = *ioread ) & PI_TXN_IN_PROGRESS ) {if (spins == 8) break; spins++;}
  //if (spins > 0) rd_spins = spins; // track average

  g_status = *ioread;

  *ioclr = TXN_END;

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;

  return (g_status >> 8);
}


inline
uint8_t ps_read_8 ( uint32_t address )
{
  *ioset = ((address & 0xffff) << 8) | REG_ADDR_LO | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = (( (fc << 13) | READ_BYTE | (address >> 16) ) << 8) | REG_ADDR_HI | PIN_WR;
  *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  *ioset = REG_DATA | PIN_RD;

  while ( *ioread & PI_TXN_IN_PROGRESS );

  g_status = *ioread;

  *ioclr = TXN_END;

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;

  if ( (address & 1) == 0 )
    return (g_status >> 16);
  else
    return (g_status >> 8);
}


uint32_t ps_read_32 ( uint32_t address )
{
  return ( ps_read_16 ( address ) << 16 ) | ps_read_16 ( address + 2 );
}


void ps_write_status_reg ( uint16_t value )
{
  static int timeout;

  timeout = 1000000;

  /* make sure no IO in progress */
  while ( timeout-- && (*ioread & PI_TXN_IN_PROGRESS) );

  *ioset = (value << 8) | REG_STATUS;
  *ioset = PIN_WR; *ioset = PIN_WR;
  *ioclr = PIN_WR; *ioclr = PIN_WR;
  *ioclr = TXN_END;

  if ( timeout <= 0 )
    printf ( "ps_write_status_reg () timed-out\n" );
}


uint32_t ps_read_status_reg ()
{
  static uint32_t l;
  static int timeout;

  timeout = 1000000;

  /* make sure no IO in progress */
  while ( timeout-- && (*ioread & PI_TXN_IN_PROGRESS) );

  *ioset = REG_STATUS | PIN_RD;

  while ( timeout-- && ( (l = *ioread) & PI_TXN_IN_PROGRESS ) );
  *ioclr = PIN_RD;
  *ioclr = TXN_END;

  return l;
}


/* read GPIO pins 0-7 - check that an IO is not running first */
uint8_t ps_read_ipl ()
{
#if (0)
  static uint32_t l;
  static int timeout;

  timeout = 1000000;

  while ( ( ( l = *ioread ) & PI_TXN_IN_PROGRESS ) && timeout-- )
    ;

  if ( timeout <= 0 )
    printf ( "ps_read_ipl () timed out\n" );

  /* RESET (0x40), BERR (0x20), IPL_ZERO (0x02) & PI_TXN_IN_PROGRESS (0x01) */
  return l;
#else
  return *ioread;
#endif
}

#endif /* !SMI */


/* =========================================================================
 * SMI PATH  (compiled when -DSMI is passed)
 *
 * The Secondary Memory Interface (SMI) peripheral on BCM2711 (Pi 4) provides
 * hardware-timed strobes, so there is no need to manually toggle PIN_WR /
 * PIN_RD or spin on software delays.  The CPLD register protocol is kept
 * identical to the GPIO path:
 *
 *   REG_ADDR_LO  – lower 16 bits of address + transfer-type flags
 *   REG_ADDR_HI  – upper address bits + FC bits (triggers PI_TXN_IN_PROGRESS)
 *   REG_DATA     – data register (read or write payload)
 *   REG_STATUS   – status / control register
 *
 * SMI address lines map directly to the CPLD register-select bits; SMI data
 * lines carry the 16-bit payload.  Hardware setup/strobe/hold timings are
 * configured in SMI_DSR0 and must be tuned to match the CPLD and latch
 * propagation delays on your board.
 * ========================================================================= */
#ifdef SMI

/* --------------------------------------------------------------------------
 * BCM2711 peripheral and SMI register map
 *
 * The SMI peripheral has two distinct register sets:
 *
 *   0x00–0x2C  DMA/FIFO path  (SMICS, SMIL, SMIA, SMID, SMIDSRn, SMIDSWn …)
 *              Used for bulk DMA transfers. NOT what we use.
 *
 *   0x34–0x3C  Direct mode    (SMIDCS, SMIDA, SMIDD)
 *              Used for single polled transfers. This is what we use.
 *
 * Previous code was incorrectly targeting the DMA registers (0x00, 0x08, 0x0C)
 * which is why SMIDCS_DONE never set — those are a completely different path.
 * -------------------------------------------------------------------------- */
#define PERIPH_BASE_PI4   0xfe000000UL
#define SMI_BASE_OFFSET   0x00600000UL
#define CM_BASE_OFFSET    0x00101000UL  /* Clock Manager              */
#define SMI_MAP_SIZE      4096

/* --- Timing registers (DMA path, used during init only) --- */
#define SMICS             (0x00 / 4)   /* DMA-path Control & Status  */
#define SMICS_ENABLE      (1u << 0)    /* Enable DMA path            */
#define SMIDSR0           (0x10 / 4)   /* Device 0 Read Settings     */
#define SMIDSW0           (0x14 / 4)   /* Device 0 Write Settings    */

/*
 * Timing field layout in SMIDSR0 / SMIDSW0:
 *   [31:30]  WIDTH   data bus width: 0=8-bit, 1=16-bit
 *   [29:24]  SETUP   address setup cycles
 *   [23:16]  STROBE  strobe width cycles
 *   [15: 8]  HOLD    hold cycles after strobe
 *   [ 7: 0]  PACE    inter-transfer pace cycles
 *
 * Each SMI clock unit = 1/SMI_CLK.  With a 125 MHz SMI clock each unit
 * is 8 ns.  The values below give 40 ns setup, 80 ns strobe, 40 ns hold.
 * Adjust to match your CPLD and latch propagation delays.
 */
#define SMI_WIDTH_16BIT   (1u << 30)
#define SMI_SETUP         5            /* 5 × 8 ns = 40 ns           */
#define SMI_STROBE        10           /* 10 × 8 ns = 80 ns          */
#define SMI_HOLD          5            /* 5 × 8 ns = 40 ns           */
#define SMI_PACE          0

#define SMI_TIMING_VAL    ( SMI_WIDTH_16BIT           | \
                            ((uint32_t)SMI_SETUP  << 24) | \
                            ((uint32_t)SMI_STROBE << 16) | \
                            ((uint32_t)SMI_HOLD   <<  8) | \
                            ((uint32_t)SMI_PACE        ) )

/* --- Direct-mode registers (the ones we actually use) --- */
#define SMIDCS            (0x34 / 4)   /* Direct Control & Status    */
#define SMIDA             (0x38 / 4)   /* Direct Address             */
#define SMIDD             (0x3C / 4)   /* Direct Data                */

/* SMIDCS bits */
#define SMIDCS_ENABLE     (1u << 0)    /* Enable direct transfers    */
#define SMIDCS_DONE       (1u << 1)    /* Transfer complete          */
#define SMIDCS_START      (1u << 3)    /* Start transfer             */
#define SMIDCS_WRITE      (1u << 5)    /* 1=write, 0=read            */

/* --- Clock Manager registers for SMI --- */
/* All CM writes require the password 0x5A in bits [31:24] */
#define CM_PASSWORD       0x5A000000UL
#define CM_SMICTL         (0xB0 / 4)   /* SMI Clock Control          */
#define CM_SMIDIV         (0xB4 / 4)   /* SMI Clock Divisor          */

/* CM_SMICTL bits */
#define CM_CTL_ENAB       (1u << 4)    /* Clock enable               */
#define CM_CTL_KILL       (1u << 5)    /* Kill clock immediately     */
#define CM_CTL_BUSY       (1u << 7)    /* Clock generator running    */
#define CM_CTL_SRC_PLLD   6u           /* PLLD (500 MHz) as source   */

/*
 * PLLD runs at 500 MHz on Pi 4.  A divisor of 4 gives 125 MHz SMI clock
 * (8 ns per cycle), which is a comfortable speed for driving the CPLD.
 * Increase the divisor (or adjust SMI_STROBE etc. above) if your board
 * needs slower timings.
 */
#define CM_SMIDIV_VAL     (4u << 12)   /* Integer divisor = 4        */

void wait_txn_done ( void );

static volatile uint32_t *smi_regs  = NULL;
static volatile uint32_t *cm_regs   = NULL;

/*
 * GPIO 0 carries PI_TXN_IN_PROGRESS (and BERR, IPL bits) directly as a pin
 * level, so we map GPLEV0 independently of the SMI peripheral.  This lets
 * wait_txn_done() poll the pin with a plain memory read — identical to the
 * GPIO path — rather than issuing a full SMI bus cycle to REG_STATUS.
 */
volatile uint32_t *gpio_ioread = NULL;


/* --------------------------------------------------------------------------
 * Low-level SMI direct-mode primitives
 * -------------------------------------------------------------------------- */

/*
 * smi_write_reg() – hardware-timed 16-bit write via SMI direct mode.
 *
 * Protocol (from bcm2835_smi kernel driver):
 *   1. Write SMIDCS = ENABLE | WRITE          (arm direction, no start yet)
 *   2. Write SMIDD  = data
 *   3. Write SMIDCS = ENABLE | WRITE | START  (fire the transfer)
 *   4. Spin on SMIDCS_DONE
 */
static inline
void smi_write_reg ( uint8_t reg_id, uint16_t data )
{
  smi_regs[SMIDA]  = (uint32_t)reg_id & 0x3F;
  smi_regs[SMIDCS] = SMIDCS_ENABLE | SMIDCS_WRITE;
  smi_regs[SMIDD]  = (uint32_t)data;
  smi_regs[SMIDCS] = SMIDCS_ENABLE | SMIDCS_WRITE | SMIDCS_START;

  int t = 100000;
  while ( !(smi_regs[SMIDCS] & SMIDCS_DONE) && --t > 0 );
  if ( t <= 0 )
    printf ( "[SMI] write_reg TIMEOUT reg=%u data=0x%04X SMIDCS=0x%08X\n",
             reg_id, data, smi_regs[SMIDCS] );
}


/*
 * smi_read_reg() – hardware-timed 16-bit read via SMI direct mode.
 *
 * Protocol:
 *   1. Write SMIDCS = ENABLE          (read direction, no start yet)
 *   2. Write SMIDCS = ENABLE | START  (fire the transfer)
 *   3. Spin on SMIDCS_DONE
 *   4. Read SMIDD
 */
static inline
uint16_t smi_read_reg ( uint8_t reg_id )
{
  smi_regs[SMIDA]  = (uint32_t)reg_id & 0x3F;
  smi_regs[SMIDCS] = SMIDCS_ENABLE;
  smi_regs[SMIDCS] = SMIDCS_ENABLE | SMIDCS_START;

  int t = 100000;
  while ( !(smi_regs[SMIDCS] & SMIDCS_DONE) && --t > 0 );
  if ( t <= 0 )
  {
    printf ( "[SMI] read_reg TIMEOUT reg=%u SMIDCS=0x%08X\n",
             reg_id, smi_regs[SMIDCS] );
    return 0;
  }

  return (uint16_t)(smi_regs[SMIDD] & 0xFFFF);
}


/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

static
void ps_smi_init ( void )
{
  int fd = open ( "/dev/mem", O_RDWR | O_SYNC );
  if ( fd < 0 )
  {
    printf ( "Unable to open /dev/mem for SMI.\n" );
    exit ( -1 );
  }

  /* Map SMI peripheral registers */
  void *map = mmap ( NULL, SMI_MAP_SIZE,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, PERIPH_BASE_PI4 + SMI_BASE_OFFSET );
  if ( map == MAP_FAILED )
  {
    printf ( "SMI mmap failed, errno = %d\n", errno );
    close ( fd );
    exit ( -1 );
  }
  smi_regs = (volatile uint32_t *)map;

  /* Map Clock Manager registers (separate 4K page at CM_BASE_OFFSET) */
  void *cm_map = mmap ( NULL, SMI_MAP_SIZE,
                        PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, PERIPH_BASE_PI4 + CM_BASE_OFFSET );
  if ( cm_map == MAP_FAILED )
  {
    printf ( "CM mmap failed, errno = %d\n", errno );
    close ( fd );
    exit ( -1 );
  }
  cm_regs = (volatile uint32_t *)cm_map;

  /* Map GPIO block (read-only) so we can poll GPLEV0 directly */
  void *gpio_map = mmap ( NULL, BCM2708_PERI_SIZE,
                          PROT_READ, MAP_SHARED,
                          fd, BCM2708_PERI_BASE );
  close ( fd );

  if ( gpio_map == MAP_FAILED )
  {
    printf ( "GPIO mmap failed, errno = %d\n", errno );
    exit ( -1 );
  }
  /* GPLEV0 is at GPIO base + 0x34 */
  gpio_ioread = ((volatile uint32_t *)gpio_map) + (GPIO_ADDR / 4) + (0x34 / 4);

  /* -----------------------------------------------------------------------
   * Enable the SMI clock via the Clock Manager.
   *
   * All CM register writes require the password 0x5A in bits [31:24],
   * otherwise the write is silently ignored.
   *
   * Sequence (from bcm2835_smi kernel driver):
   *   1. Kill the clock (write 0 — kills enable, keeps password)
   *   2. Wait for BUSY to clear
   *   3. Set the integer divisor in CM_SMIDIV
   *   4. Set the clock source (no enable yet)
   *   5. Set source + ENAB together to start the clock
   * ----------------------------------------------------------------------- */
  /*
   * Kill sequence: write KILL bit first, then set source without enable.
   * The clock manager requires a valid source to be selected before
   * CM_CTL_BUSY will de-assert — polling BUSY after KILL alone hangs.
   */
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_KILL;
  usleep ( 10 );
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_PLLD;   /* source, no enable */

  int cm_timeout = 10000;
  while ( (cm_regs[CM_SMICTL] & CM_CTL_BUSY) && --cm_timeout > 0 )
    usleep ( 1 );

  if ( cm_timeout <= 0 )
  {
    printf ( "[INIT] SMI clock BUSY stuck — run smi_reset and retry\n" );
    exit ( -1 );
  }

  cm_regs[CM_SMIDIV] = CM_PASSWORD | CM_SMIDIV_VAL;
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_PLLD;
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_PLLD | CM_CTL_ENAB;

  /* Brief settle time after clock enable */
  usleep ( 100 );

  /* -----------------------------------------------------------------------
   * Initialise SMI registers in the same order as the kernel driver
   * (bcm2835_smi.c :: smi_setup_regs):
   *
   *   1. Zero SMICS and SMIDCS — disables both paths cleanly
   *   2. Write timing registers
   *   3. Re-enable both CS registers
   *
   * SMICS (DMA-path, 0x00) must be explicitly zeroed even though we only
   * use the direct-mode path. The two paths share internal FIFO and status
   * logic. Leaving SMICS in a stale state (RXD/RXF set from a prior run)
   * prevents SMIDCS_DONE from ever asserting.
   * ----------------------------------------------------------------------- */
  /* Flush DMA-path FIFOs — CLEAR bit (bit 4) requires ENABLE to be set first */
  smi_regs[SMICS]  = SMICS_ENABLE;
  smi_regs[SMICS]  = SMICS_ENABLE | (1u << 4);  /* ENABLE | CLEAR */
  usleep ( 10 );
  smi_regs[SMICS]  = 0;
  smi_regs[SMIDCS] = 0;

  smi_regs[SMIDSR0] = SMI_TIMING_VAL;
  smi_regs[SMIDSW0] = SMI_TIMING_VAL;

  smi_regs[SMICS]  = SMICS_ENABLE;
  smi_regs[SMIDCS] = SMIDCS_ENABLE;

  printf ( "[INIT] SMI initialised: PLLD/%u = %u MHz, setup=%uns strobe=%uns hold=%uns\n",
           (CM_SMIDIV_VAL >> 12),
           500u / (CM_SMIDIV_VAL >> 12),
           SMI_SETUP  * (1000u / (500u / (CM_SMIDIV_VAL >> 12))),
           SMI_STROBE * (1000u / (500u / (CM_SMIDIV_VAL >> 12))),
           SMI_HOLD   * (1000u / (500u / (CM_SMIDIV_VAL >> 12))) );
}


static
void setup_gpclk ( int verbose )
{
  /* Report clocks for diagnostic purposes – same as GPIO path */
  int cpuf, coref;
  FILE *fp;
  char junk[80];
  char *ptr;

  fp = popen ( "vcgencmd measure_clock arm", "r" );
  fgets ( junk, sizeof (junk), fp );
  pclose ( fp );
  ptr = strchr ( junk, '=' );
  sscanf ( (ptr + 1), "%d", &cpuf );
  cpuf /= 1000000;

  fp = popen ( "vcgencmd measure_clock core", "r" );
  fgets ( junk, sizeof (junk), fp );
  pclose ( fp );
  ptr = strchr ( junk, '=' );
  sscanf ( (ptr + 1), "%d", &coref );
  coref /= 1000000;

  printf ( "[INIT] CPU clock is %d MHz\n", cpuf );
  printf ( "[INIT] CORE clock is %d MHz\n", coref );
}


void ps_setup_protocol ( int verbose )
{
  ps_smi_init ();
  setup_gpclk ( verbose );
}


/* --------------------------------------------------------------------------
 * Transaction helpers
 *
 * The CPLD register protocol is identical to the GPIO path.  The only
 * difference is that strobe timing is now provided by SMI hardware rather
 * than software loops.
 * -------------------------------------------------------------------------- */

/*
 * wait_txn_done()
 *
 * PI_TXN_IN_PROGRESS is a physical level on GPIO 0 (bit 0 of GPLEV0).
 * Polling it is a single 32-bit memory read — no SMI bus cycle needed.
 * BERR and IPL bits also live in the low byte of GPLEV0, so capturing
 * the final read into g_status gives CHECK_BERR() exactly what it needs,
 * identical to the GPIO path.
 */
inline
void wait_txn_done ( void )
{
  while ( ( g_status = *gpio_ioread ) & PI_TXN_IN_PROGRESS );
}


/* --------------------------------------------------------------------------
 * Public read/write API  – same signatures as the GPIO path
 * -------------------------------------------------------------------------- */

inline
void ps_write_16 ( uint32_t address, uint16_t data )
{
  /*
   * Same three-register sequence as GPIO path.
   * REG_DATA must be sent first (CPLD latches it while processing ADDR_HI).
   */
  smi_write_reg ( REG_DATA,    data );
  smi_write_reg ( REG_ADDR_LO, (uint16_t)(address & 0xFFFF) );

  /* ADDR_HI triggers PI_TXN_IN_PROGRESS on the CPLD */
  smi_write_reg ( REG_ADDR_HI,
                  (uint16_t)( (fc << 13) | WRITE_WORD | (address >> 16) ) );

  wait_txn_done ();

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;
}


inline
void ps_write_8 ( uint32_t address, uint16_t data )
{
  if ( (address & 1) == 0 )
    data <<= 8;
  else
    data &= 0xFF;

  smi_write_reg ( REG_DATA,    data );
  smi_write_reg ( REG_ADDR_LO, (uint16_t)(address & 0xFFFF) );
  smi_write_reg ( REG_ADDR_HI,
                  (uint16_t)( (fc << 13) | WRITE_BYTE | (address >> 16) ) );

  wait_txn_done ();

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;
}


inline
void ps_write_32 ( uint32_t address, uint32_t value )
{
  ps_write_16 ( address,     (uint16_t)(value >> 16) );
  ps_write_16 ( address + 2, (uint16_t)(value) );
}


inline
uint16_t ps_read_16 ( uint32_t address )
{
  smi_write_reg ( REG_ADDR_LO, (uint16_t)(address & 0xFFFF) );
  smi_write_reg ( REG_ADDR_HI,
                  (uint16_t)( (fc << 13) | READ_WORD | (address >> 16) ) );

  /*
   * Trigger the CPLD data-read cycle.
   * A dummy SMI read of REG_DATA is issued; the CPLD places the result on
   * the bus once PI_TXN_IN_PROGRESS clears, so we poll first then read.
   */
  wait_txn_done ();

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;

  /* Fetch the result that the CPLD has now placed on REG_DATA */
  return smi_read_reg ( REG_DATA );
}


inline
uint8_t ps_read_8 ( uint32_t address )
{
  smi_write_reg ( REG_ADDR_LO, (uint16_t)(address & 0xFFFF) );
  smi_write_reg ( REG_ADDR_HI,
                  (uint16_t)( (fc << 13) | READ_BYTE | (address >> 16) ) );

  wait_txn_done ();

  if ( CHECK_BERR (g_status) )
    g_buserr = 1;

  uint16_t word = smi_read_reg ( REG_DATA );

  /* Even address = high byte (UDS), odd address = low byte (LDS) */
  if ( (address & 1) == 0 )
    return (uint8_t)(word >> 8);
  else
    return (uint8_t)(word & 0xFF);
}


uint32_t ps_read_32 ( uint32_t address )
{
  return ( (uint32_t)ps_read_16 ( address ) << 16 )
       |  (uint32_t)ps_read_16 ( address + 2 );
}


void ps_write_status_reg ( uint16_t value )
{
  static int timeout;

  timeout = 1000000;

  /* Poll GPIO 0 pin directly — no SMI cycle needed */
  while ( (*gpio_ioread & PI_TXN_IN_PROGRESS) && (--timeout > 0) );

  if ( timeout <= 0 )
    printf ( "ps_write_status_reg () timed-out\n" );

  smi_write_reg ( REG_STATUS, value );
  /* No PI_TXN_IN_PROGRESS expected after a status write */
}


uint32_t ps_read_status_reg ( void )
{
  static int timeout;

  timeout = 1000000;

  /* Poll GPIO 0 pin — same as GPIO path */
  while ( (*gpio_ioread & PI_TXN_IN_PROGRESS) && (--timeout > 0) );

  /* Issue the actual status register read over SMI */
  return (uint32_t)smi_read_reg ( REG_STATUS );
}


uint8_t ps_read_ipl ( void )
{
  /* RESET, BERR, IPL and PI_TXN_IN_PROGRESS are all GPIO pin levels in
   * the low byte of GPLEV0 — read directly, no SMI cycle required */
  return (uint8_t)(*gpio_ioread & 0xFF);
}

#endif /* SMI */


/* =========================================================================
 * Functions common to both backends
 * ========================================================================= */

/* reset state machine */
void ps_reset_state_machine ()
{
  ps_write_status_reg ( STATUS_BIT_INIT );
  usleep ( 100000 );
  ps_write_status_reg ( 0 );
  usleep ( 1000 );
}

/* toggle HALT signal */
void ps_pulse_halt ()
{
  ps_write_status_reg ( STATUS_BIT_HALT );
  usleep ( 250000 );
  ps_write_status_reg ( 0 );
  usleep ( 1000 );
}

/* hold reset low for 250ms - Atari ST needs min 100ms */
void ps_pulse_reset ()
{
  ps_write_status_reg ( STATUS_BIT_RESET );
  usleep ( 250000 );
  ps_write_status_reg ( 0 );
  usleep ( 1000 );
}

/*
 * write PiSTorm latch type
 * latchtype must be 0 or 1 and is put on status bit 2
 */
void ps_write_latchtype ( uint16_t latchtype )
{
  uint16_t status;

  status = ps_read_status_reg () & 0xC000;
  printf ( "reading latch-type - status bits 0x%04X\n", status );

  if ( !( status & 0x8000 ) )
  {
    printf ( "latchtype not set yet\n" );
    ps_write_status_reg ( (uint16_t)(('P' << 3) | (latchtype << 2)) );
  }
  else
  {
    printf ( "latchtype already set to 0x%04X\n", (status & 0x4000) );
  }

  usleep ( 1000 );
  ps_write_status_reg ( 0 );
  usleep ( 1000 );
  status = (ps_read_status_reg () >> 8) & 0xC000;
  printf ( "reading latch-type - status bits 0x%04X\n", status );
}

/*
 * read CPLD firmware version
 * 11-bit version data
 *
 * upper 3 bits (10,9,8) define release type and CPLD type:
 *   bits 10,9 = 0,1  alpha
 *   bits 10,9 = 1,0  beta
 *   bits 10,9 = 0,0  release
 *   bit  8    = 1    EPM570, 0 = EPM240
 */
void ps_get_firmware_revision ( void )
{
  uint16_t fw = (ps_read_status_reg () >> 8) & 0x07FF;

  if ( (fw & 0x600) == 0x200 )
    printf ( "[INIT] PiSTorm firmware 0.%da\n", (fw & 0xFF) );
  else
    printf ( "[INIT] PiSTorm firmware %s %d.%d%c\n",
      (fw & 0x100) ? "EPM570" : "EPM240",
      (fw & 0xE0) >> 5,
      (fw & 0x1F),
      (fw & 0x600) == 0x400 ? 'b' : 'r' );

  usleep (1000);
}
