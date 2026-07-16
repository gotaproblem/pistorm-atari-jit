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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
#include "ps_protocol.h"



/* MUST match firmware - clear output bits only - bits to clear = 1 */
#define TXN_END 0x00FFFF1E
#define CHECK_BERR(x) ( !((x & 0x80) >> 7) )


/* Weak default so tools that link ps_protocol.c without the CPU core
 * (e.g. ataritest) still build. The emulator provides the strong override
 * in newcpu.cpp that actually raises the JIT bail flag. */
//__attribute__((weak)) void jit_signal_buserr(void) { }


_Alignas(uint64_t) volatile _Atomic uint8_t g_buserr = 0;
static volatile uint32_t g_status;

volatile uint32_t *gpio;
volatile uint32_t *ioset;
volatile uint32_t *ioclr;
volatile uint32_t *ioread;

volatile uint32_t *gpclk;
volatile uint32_t *clk_ctl;
volatile uint32_t *clk_div;

uint8_t fc;
volatile uint32_t g_buserr_addr = 0;

static atomic_flag ps_txn_lock = ATOMIC_FLAG_INIT;

static inline void ps_lock_bus(void)
{
  while (atomic_flag_test_and_set_explicit(&ps_txn_lock, memory_order_acquire))
    asm volatile ("yield" ::: "memory");
}

static inline void ps_unlock_bus(void)
{
  atomic_flag_clear_explicit(&ps_txn_lock, memory_order_release);
}

static inline void ps_wait_idle(void)
{
  while (*ioread & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");
}


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
      NULL,                    // Any adddress in our space will do
      BCM2708_PERI_SIZE,       // Map length
      PROT_READ | PROT_WRITE,  // Enable reading & writting to mapped memory
      MAP_SHARED,              // Shared with other processes
      fd,                      // File to map
      BCM2708_PERI_BASE        // Offset to GPIO peripheral
  );

  close(fd);

  if ( gpio_map == MAP_FAILED ) 
  {
    printf ( "mmap failed, errno = %d\n", errno );
    exit ( -1 );
  }

  gpio = ((volatile uint32_t *)gpio_map) + (GPIO_ADDR / 4);
  ioset  = gpio + (0x1c / 4); // GPSET0
  ioclr  = gpio + (0x28 / 4); // GPCLR0
  ioread = gpio + (0x34 / 4); // GPLEV0

  gpclk = ((volatile uint32_t *)gpio_map) + (GPCLK_ADDR / 4);
  clk_ctl = gpclk + (0x70 / 4);
  clk_div = gpclk + (0x74 / 4);
}

/* 
GPIO 
gpio + 0  = GPFSEL0 GPIO Function Select 0
gpio + 1  = GPFSEL1 GPIO Function Select 1
gpio + 2  = GPFSEL2 GPIO Function Select 2
gpio + 7  = GPSET   GPIO Pin Output Set 0
gpio + 10 = GPCLR   GPIO Pin Output Clear 0
gpio + 13 = GPLEV   GPIO Pin Levl 0
gpio + 16 = GPEDS   GPIO Pin Event Detect Status 0
gpio + 19 = GPREN   GPIO Pin Rising Edge Detect Enable 0
gpio + 22 = GPFEN   GPIO Pin Falling Edge Detect Enable 0
gpio + 25 = GPHEN   GPIO Pin High Detect Enable 0
gpio + 28 = GPLEN   GPIO Pin Low Detect Enable 0
gpio + 31 = GPAREN  GPIO Pin Asysnchronous Rising Edge Detect Enable 0
gpio + 34 = GPAFEN  GPIO Pin Asysnchronous Falling Edge Detect Enable 0
*/


#define PLLC 5 /* ARM CLOCK  - cpu_freq */
#define PLLD 6 /* CORE CLOCK - gpu_freq this should be used for a stable 200 MHz PI_CLK as it is NOT affected by overclocking CPU */
#define PLL_TO_USE PLLD

static 
void setup_gpclk ( void ) 
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

#ifdef USING_PI_CLK
  int div_i;
  int div_f;
  int clk;
  float div;
  int targetF = 125;

  clk = PLL_TO_USE == PLLC ? cpuf : coref;

  div = clk / (float)targetF;
  div_i = (int)div;
  div_f = (int)( (div - div_i) * 4096 );
  //printf ( "div = %f -> divi %d, divf %d\n", div, div_i, div_f );


  //printf ( "[INIT] Using clock divisor %.3f with PLL%c\n", div, PLL_TO_USE == PLLC ? 'C' : 'D' );
  printf ( "[INIT] GPIO clock is %d MHz\n", (int)(clk / div) );//PLL_TO_USE == PLLC ? cpuf / div : coref / div );


  /* stop clock */
  *clk_ctl = CLK_PASSWD | CLK_KILL;
  usleep(30);

  /* wait for clock to stop */
  while ( *clk_ctl & CLK_BUSY );
  usleep(100);

  /* bits 23:12 integer part of divisor */
  /* bite 11:0 fractional part of divisor */
  *clk_div = CLK_PASSWD | (6 << 12);
  //*clk_div = CLK_PASSWD | (div_i << 12) | (div_f & 0x7ff);  
  usleep(30);

  /* restart clock */
  *clk_ctl = CLK_PASSWD | CLK_ENABLE | PLLD;
  usleep(30);

  /* wait for clock to start */
  while ( (*clk_ctl & CLK_BUSY) == 0 );
  usleep(100);

  SET_GPIO_ALT ( PIN_CLK, 0 );  // assign clock to this gpio pin (PI_CLK)
#endif
}


void ps_setup_protocol ( void ) 
{
  create_dev_mem_mapping ();
  setup_gpclk ();

  gpio [0] = GPFSEL0_OUTPUT;
  gpio [1] = GPFSEL1_OUTPUT;
  gpio [2] = GPFSEL2_OUTPUT;

  *ioclr = TXN_END;
}

typedef struct {
  uint32_t addr;
  uint16_t data;
  uint8_t  fc;
  uint16_t io_type; // 
                    // WRITE BYTE = 0x01
                    // WRITE WORD = 0x00
                    // READ BYTE  = 0x03
                    // READ WORD  = 0x02
  uint8_t  berr;    // io related berr = 1
} ps_io_t;


#define txn_go() { \
  *ioset = PIN_WR; *ioset = PIN_WR; \
  *ioclr = PIN_WR; *ioclr = PIN_WR; \
  *ioclr = TXN_END; \
}

void ps_read (ps_io_t *ps_io);
void ps_write ( ps_io_t *ps_io );

inline
void ps_write ( ps_io_t *ps_io )
{
  register uint32_t status;

  ps_lock_bus ();
  asm volatile ("dmb sy" : : : "memory");

  ps_wait_idle ();

  *ioset = (ps_io->data << 8) | REG_DATA;
  txn_go ();

  *ioset = ((ps_io->addr & 0xffff) << 8) | REG_ADDR_LO;
  txn_go ();

  /* 
   * Atari ST uses a 24 bit address - PiSTorm has 32 bits available 
   * spare 8 bits are used for FC bits and transfer type (read/write)
   * spare bits 7,6,5 FC bits
   * spare bit  1 - write = 0, read = 1
   * spare bit  0 - word = 0, byte = 1
   * 
   * WRITE BYTE = 0x01
   * WRITE WORD = 0x00
   * READ BYTE  = 0x03
   * READ WORD  = 0x02
   */
  
  *ioset = (((ps_io->fc << 13) | ps_io->io_type | (ps_io->addr >> 16)) << 8) | REG_ADDR_HI;
  txn_go ();

  while (( status = *ioread ) & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");

  ps_io->berr = CHECK_BERR (status);
  ps_unlock_bus();
}


inline
void ps_write_8 (uint32_t addr, uint16_t data) 
{
  ps_io_t ps_io;

  ps_io.data = addr & 0x01 ? data & 0xFF : data << 8;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = WRITE_BYTE;

  ps_write (&ps_io);

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
}

inline
void ps_write_16 (uint32_t addr, uint16_t data) 
{
  ps_io_t ps_io;

  ps_io.data = data;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = WRITE_WORD;

  ps_write (&ps_io);

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
}

inline
void ps_write_32 (uint32_t addr, uint32_t data) 
{
  ps_write_16 (addr, (uint16_t)(data >> 16));
  ps_write_16 (addr + 2, (uint16_t)data);
}



inline
void ps_read (ps_io_t *ps_io)
{
  register uint32_t status;

  ps_lock_bus ();
  asm volatile ("dmb sy" : : : "memory");

  ps_wait_idle ();

  *ioset = ( (ps_io->addr & 0xffff) << 8 ) | REG_ADDR_LO;
  txn_go ();

  *ioset = (((ps_io->fc << 13) | ps_io->io_type | (ps_io->addr >> 16)) << 8) |  REG_ADDR_HI;
  txn_go ();

  *ioset = REG_DATA;// | PIN_RD;
  *ioset = PIN_RD;

  while ((status = *ioread) & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");

  status = *ioread;
 	*ioclr = TXN_END;

  ps_io->berr = CHECK_BERR (status);
  ps_io->data = status >> 8;
  ps_unlock_bus();
}

inline
uint16_t ps_read_16 (uint32_t addr) 
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_WORD;

  ps_read (&ps_io);

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  return ps_io.data;
}

uint16_t ps_read_16_fc (uint32_t addr, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = READ_WORD;

  ps_read (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  return ps_io.data;
}


inline
uint8_t ps_read_8 (uint32_t addr) 
{
  ps_io_t ps_io;
  uint32_t l;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_BYTE;

  ps_read (&ps_io);

  if (ps_io.berr) {
    g_buserr = 1; 
    g_buserr_addr = addr;
  } 
  
#if (0)
  /* ---- KBD/mouse ACIA data probe ---- */
  if ((addr & 0x00FFFFFF) == 0x00FFFC02) {          /* keyboard ACIA data register */
      uint8_t b = ((addr & 1) == 0) ? (uint8_t)(ps_io.data >> 8) : (uint8_t)ps_io.data;
      static uint8_t kbuf[48]; static int kn;
      kbuf[kn++ % 48] = b;
      if (kn % 48 == 0) {
          fprintf(stderr, "[KBD]");
          for (int i = 0; i < 48; i++) fprintf(stderr, " %02X", kbuf[i]);
          fprintf(stderr, "\n");
      }
  }
  /* ---- end probe ---- */
#endif

  if ( (addr & 1) == 0 ) return (uint8_t)(ps_io.data >> 8);
    return (uint8_t)ps_io.data;
}

uint8_t ps_read_8_fc (uint32_t addr, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = READ_BYTE;

  ps_read (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  if ((addr & 1) == 0)
    return (uint8_t)(ps_io.data >> 8);
  return (uint8_t)ps_io.data;
}


uint32_t ps_read_32 (uint32_t addr) 
{
  return (ps_read_16 (addr) << 16) | ps_read_16 (addr + 2);
}



void ps_write_status_reg ( uint16_t value ) 
{
  static int timeout;
  
  timeout = 1000000;
  ps_lock_bus();

  /* make sure no IO in progress */
  while ( __builtin_expect ((*ioread & PI_TXN_IN_PROGRESS) && timeout--, 1 ))
    asm volatile ("yield" ::: "memory");
  
  *ioset = (value << 8) | REG_STATUS;
  txn_go ();

  if ( timeout <= 0 )
    printf ( "ps_write_status_reg () timed-out\n" );

  /* when writing status, there isn't a PI_TXN_IN_PROGRESS */
  ps_unlock_bus();
}


uint32_t ps_read_status_reg () 
{
  uint32_t status;
  //int timeout = 1000000;

  ps_lock_bus();
  asm volatile ("dmb sy" : : : "memory");

  /* make sure no IO in progress */
  while (*ioread & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");

  //*(gpio + 0) = GPFSEL0_INPUT;
  //*(gpio + 1) = GPFSEL1_INPUT;
  //*(gpio + 2) = GPFSEL2_INPUT;
  
  *ioset = REG_STATUS;
  *ioset = PIN_RD;

  while (*ioread & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");

  status = *ioread;
 	*ioclr = TXN_END;

  //if ( timeout <= 0 )
  //  printf ( "ps_read_status_reg () timed-out\n" );

  /* return all 32 bits */
  ps_unlock_bus();
  return status;
}


/* reset state machine */
void ps_reset_state_machine () 
{
  ps_write_status_reg ( STATUS_BIT_INIT );
  usleep ( 1000 );
  ps_write_status_reg ( 0 );
  //usleep ( 1000 );
}

/* toggle HALT signal */
void ps_pulse_halt () 
{
  ps_write_status_reg ( STATUS_BIT_HALT );
  usleep ( 1000 );
  ps_write_status_reg ( 0 );
  //usleep ( 1000 );
}

/* hold reset low for 250ms - Atari ST needs min 100ms */
void ps_pulse_reset () 
{
  ps_write_status_reg ( STATUS_BIT_RESET );
  usleep ( 15 );
  ps_write_status_reg ( 0 );
  //usleep ( 15 );
}

/* read CPLD firmware revision */
/*
uint16_t ps_fw_rd ()
{
  uint16_t fw;

  fw = ps_read_status_reg ();
 
  return fw;//(fw >> 2);// & 0x3FF;
}
*/

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
    /* enable latchtype with 'P' key */
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
  {
    printf ( "ps_read_ipl () timed out\n" );

    //while ( *ioread & PI_TXN_IN_PROGRESS ) 
    /* if we are here then PI_TXN_IN_PROGRESS check has failed which means the firmware state machine needs reseting */
    //ps_reset_state_machine ();
    //return 0;
    //*ioclr = TXN_END;
  }

  /* RESET (0x40), BERR (0x20), IPL_ZERO (0x02) & PI_TXN_IN_PROGRESS (0x01) */
  return l;
#else
  return *ioread;
#endif
}

/* 
 * read CPLD firmware version 
 * 11 bit version data 
 * 
 * upper 3 bits (10,9,8) define release type and CPLD type
 * bits 10,9 - 0,1 = alpha
 * bits 10,9 - 1,0 = beta
 * bits 10,9 - 0,0 = release
 * bit 8 - 1 = EPM570, 0 = EPM240
 */
void ps_get_firmware_revision ( void )
{
  uint16_t fw = (ps_read_status_reg () >> 8) & 0x07FF;

  /* 
   * if alpha release, then all 8 bits will be version number 
   * eg. 0.1a - 0.255a 
   */
  if ( (fw & 0x600) == 0x200 )
    //printf ( "[INIT] PiSTorm firmware %s 0.%da\n",
    //  (fw & 0x100) ? "EPM570" : "EPM240",
    //  (fw & 0xff) );
    printf ( "[INIT] PiSTorm firmware 0.%da\n", (fw & 0xff) );

  /* 
   * then must be beta or release 
   * the 8 bits are split for major/minor
   * bits 7,6,5 = major (0-7)
   * bits 4,3,2,1,0 = minor (0-31)
   * max version therefore is 7.31
   */
  else
    printf ( "[INIT] PiSTorm firmware %s %d.%d%c\n",
      (fw & 0x100) ? "EPM570" : "EPM240",
      (fw & 0xe0) >> 5,
      (fw & 0x1f),
      (fw & 0x600) == 0x400 ? 'b' : 'r' );

  usleep (1000);
}


void ps_diag (void)
{
  // Make sure ps_setup_protocol() has been called

uint32_t val;

// Test A: GPIO6 -> CPLD -> GPIO0
*ioclr = 0x00FFFF6C;
*ioset = (1 << 6);           // set GPIO6 (PIN_WR)
val = *ioread;
*ioclr = (1 << 6);
printf("Test A (GPIO6->GPIO0): %d  [expect 1]\n", (val>>0)&1);

// Test B: GPIO2 -> CPLD -> GPIO1  
*ioclr = 0x00FFFF6C;
*ioset = (1 << 2);           // set GPIO2 (PI_CMD[0])
val = *ioread;
*ioclr = (1 << 2);
printf("Test B (GPIO2->GPIO1): %d  [expect 1]\n", (val>>1)&1);

// Test C: GPIO3 -> CPLD -> GPIO7 (inverted)
*ioclr = 0x00FFFF6C;
*ioset = (1 << 3);           // set GPIO3 (PI_CMD[1])
val = *ioread;
*ioclr = (1 << 3);
printf("Test C (GPIO3->GPIO7): %d  [expect 0]\n", (val>>7)&1);
}


void ps_latch_addr_st(uint32_t addr, uint8_t fc) {
    uint32_t BUS_MASK = 0x00FFFF00;

    // 1. Send A0-A15
    *ioclr = BUS_MASK | 0x0C;
    *ioset = ((addr & 0xFFFF) << 8) | (0<<2);
    
    // 2. Send A16-A23 + FC bits
    // addr >> 16 gets the top byte. 
    // fc & 0x07 shifted by 16 puts FC0-2 on GPIO 16, 17, 18.
    uint32_t hi_val = ((addr >> 16) & 0xFF) << 8;
    hi_val |= (uint32_t)(fc & 0x07) << 16;
    
    *ioclr = BUS_MASK | 0x0C;
    *ioset = hi_val | (1<<2);
    
    // Memory barrier to ensure pins are stable before the CPLD state machine moves
    __asm__ __volatile__ ("dmb sy" : : : "memory");
}



static inline uint32_t ps_read_st_safe_32(void) {
    uint32_t result;
    uint32_t status;
    uint32_t timeout = 5000;
    
    // Safety check: if GLEV0 is NULL here, we segfault
    if (!ioread) return 0xEEEE; 

    __asm__ __volatile__ ("dmb sy" : : : "memory");

    asm volatile (
        "mov   r4, %3               \n\t" // Load GLEV0 address into r4
        "1: ldr   %1, [r4]           \n\t" // Perform the read
        "tst   %1, %4               \n\t" // Check ACK/BERR
        "bne   2f                   \n\t" 
        "subs  %2, %2, #1           \n\t" 
        "bne   1b                   \n\t" 
        "mov   %0, #0xEE            \n\t" 
        "orr   %0, %0, #0xEE00      \n\t" 
        "b     3f                   \n\t" 
        
        "2: tst   %1, #0x80            \n\t" // Check BERR
        "beq   4f                   \n\t" 
        "mov   %0, #0xAD            \n\t" 
        "orr   %0, %0, #0xDE00      \n\t"
        "b     3f                   \n\t"

        "4: lsr   %0, %1, #8           \n\t" 
        "uxth  %0, %0               \n\t" 
        
        "3:                         \n\t"
        : "=&r" (result), "=&r" (status), "+r" (timeout)
        : "r" (ioread), "r" ((1<<5)|(1<<7))
        : "r4", "cc", "memory"            // Added r4 and memory to clobber list
    );

    return result;
}
