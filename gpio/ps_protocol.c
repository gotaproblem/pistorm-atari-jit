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

/* Monitor-detect force for MFP GPIP bit 7, applied in ps_read_txn.
 * 0 = real wire, 1 = force mono (bit low), 2 = force colour (bit high).
 * Set once from the config after load; volatile because the CPU thread
 * reads it while the main thread writes it at startup. */
volatile int ps_gpip7_force = 0;

/* The debounced monitor-detect state, hoisted out of the read path so
 * the 1 Hz stats line can report it: -1 not sampled yet, 0 MONO,
 * 1 COLOUR. ps_gpip7_glitches counts single reads that disagreed with
 * the held value and were discarded; ps_gpip7_reads counts the guest's
 * reads of the bit at all. A guest that never reads it cannot be
 * choosing its resolution from it. */
volatile int      ps_gpip7_stable   = -1;
volatile unsigned ps_gpip7_glitches = 0;
volatile unsigned ps_gpip7_reads    = 0;

volatile uint32_t *gpio;
volatile uint32_t *ioset;
volatile uint32_t *ioclr;
volatile uint32_t *ioread;

#ifdef PI5
/* Pi 5: the 40-pin header GPIOs live in RP1, behind PCIe. RP1's RIO block
 * gives the same three operations the BCM2711 has (atomic SET, atomic CLR,
 * level read), so ioset/ioclr/ioread point at RIO's SET/CLR aliases of OUT
 * and at SYNC_IN, and every bit means the same GPIO as on the Pi 4.
 * What RP1 does NOT have is GPFSEL: a pin's direction is its OE bit. */
static volatile uint32_t *rp1_io;      /* io_bank0: STATUS/CTRL per pin  */
static volatile uint32_t *rp1_rio;     /* sys_rio0                       */
static volatile uint32_t *rp1_pads;    /* pads_bank0                     */
static volatile uint32_t *rio_oe_set;
static volatile uint32_t *rio_oe_clr;

/* Data bus direction for reads, RP1 form: the pin's OE bit (two posted
 * writes each way). See the Pi 4 form below for why. */
#define PS_DATA_PINS   0x00FFFF00u
#define PS_BUS_IN()    (*rio_oe_clr = PS_DATA_PINS)
#define PS_BUS_OUT()   (*rio_oe_set = PS_DATA_PINS)
#else
/* Data bus direction for reads.
 *
 * The CPLD drives D0-D15 (GPIO 8-23) for a data read (REG_DATA with WR
 * held) and for a status read (REG_STATUS). Those pins used to stay Pi
 * OUTPUTS throughout - driving the 0s txn_pulse() leaves on them - so
 * every read was the Pi and the CPLD driving the same wires against each
 * other, the CPLD merely winning. That is stress on both chips' output
 * stages, and a level read during that fight is a race (cf. the
 * "byte-swapped echoes of the Pi's own address phases" note below).
 * The original PiStorm code flipped GPFSEL to input here; do it again.
 *
 * Three GPFSEL writes each way (GPIO 8-9 sit in GPFSEL0, 10-19 GPFSEL1,
 * 20-23 GPFSEL2). GPFSEL2's GPIO 24-29 field is captured once at setup
 * and written back unchanged: GPIO 28/29 are the Pi 4 Ethernet PHY's
 * MDIO/MDC. */
static uint32_t gpfsel2_keep;          /* GPIO 24-29 function bits, as found */

/* GPFSEL0 as written by every direction flip. A variable, not the
 * constant, because GPIO4's field changes at run time: an input (ST RESET
 * from 1.x firmware) or ALT0 = GPCLK0 (PI_CLK for 2.x firmware, see
 * ps_fastclk_start). Writing the constant would switch the clock off. */
static uint32_t gpfsel0_in  = GPFSEL0_INPUT;
static uint32_t gpfsel0_out = GPFSEL0_OUTPUT;

#define PS_BUS_IN()  do { gpio [0] = gpfsel0_in;  gpio [1] = GPFSEL1_INPUT; \
                          gpio [2] = gpfsel2_keep | GPFSEL2_INPUT; } while (0)
#define PS_BUS_OUT() do { gpio [0] = gpfsel0_out; gpio [1] = GPFSEL1_OUTPUT; \
                          gpio [2] = gpfsel2_keep | GPFSEL2_OUTPUT; } while (0)
#endif

volatile uint32_t *gpclk;
volatile uint32_t *clk_ctl;
volatile uint32_t *clk_div;

uint8_t fc;
volatile uint32_t g_buserr_addr = 0;

static atomic_flag ps_txn_lock = ATOMIC_FLAG_INIT;

/* Extra GPLEV samples after TXN falls on a read, before the data is taken.
 * Set in ps_setup_protocol: 0 on BCM2711, 2 on BCM2837, PISTORM_RD_SETTLE
 * overrides (0..8). See the comment in ps_read_txn. */
static int ps_rd_settle = 0;

/* Raised for the whole time a thread owns the bus - i.e. the window in
 * which the Pi may be DRIVING the GPIO bank (address/data phases flip
 * GPFSEL to output). GPLEV0 then reads back the driven values, so an
 * IPL sample taken by ipl_task inside this window can contain data
 * pattern bits in the IPL field - phantom interrupt levels that scale
 * with bus traffic (field case: IDE streaming). ipl_task brackets its
 * GPLEV0 sample with this flag and discards samples taken while set.
 * Single writer at a time (whoever holds ps_txn_lock); readers only
 * need a coherent 0/1. */
volatile uint8_t ps_bus_active = 0;

/*
 * BUS OWNERSHIP - CPU thread fast path, timed hand-off for everyone else.
 *
 * Firmware 2.1b starts a cycle as soon as TXN is up, and a RAM access then
 * fits the ST's 500 ns slot only if the Pi turns round inside ~75 ns of
 * margin (ataritest --busprofile slack sweep, 28 Oct 2026: 566 ns at +0,
 * 1001 ns at +1 GPLEV). The old lock - atomic test-and-set + dmb sy, then
 * dmb sy + release, around EVERY transaction - costs ~78 ns, and either
 * half alone (atomic only / dmb only) was measured to cost the slot.
 *
 * So the CPU thread (registered with ps_bus_cpu_thread) takes NO atomic
 * and NO barrier per transaction:
 *   enter: ps_cpu_seq = odd (plain store), then look at ps_bus_req
 *          (plain load, normally 0 and in this core's cache)
 *   exit:  ps_cpu_seq = even (plain store)
 *
 * Every OTHER thread (and the CPU thread before it registers) takes
 * ps_txn_lock among themselves as before, then, while a CPU thread is
 * registered, raises ps_bus_req and WAITS PS_HANDOFF before looking at
 * ps_cpu_seq. Why the wait is enough without a barrier on the CPU side:
 * a store becomes visible to the other cores within D (store-buffer
 * drain, tens of ns on the A72; the CPU thread is SCHED_FIFO on an
 * isolated core). PS_HANDOFF is 500 ns (PISTORM_HANDOFF_NS, min 200). The CPU thread raises seq (odd) BEFORE it loads
 * ps_bus_req, and touches no GPIO until that load says 0. So
 *   - a CPU transaction that read req == 0 did so before req was visible
 *     (< req + D); its odd seq was stored before that and is visible by
 *     req + 2D, i.e. well inside PS_HANDOFF - the requester sees
 *     it odd and waits for the even store at its end;
 *   - a CPU transaction that starts later sees req == 1, puts seq back to
 *     even (after a dmb sy - its earlier GPIO writes complete) and spins
 *     until the requester clears req (after the requester's own dmb sy).
 * The requester pays ~0.7 us per acquisition; the CPU thread pays two plain
 * stores and one load.
 *
 * ipl_task's IPL sample does not stop the CPU thread at all: it samples
 * GPLEV0 seqlock-style (ps_bus_trylock / ps_bus_sample_ok): seq even
 * before, GPLEV read, PS_HANDOFF later seq unchanged - else the sample is
 * thrown away, exactly as a busy trylock was before.
 *
 * PS_NOLOCK (old ataritest builds) still compiles all of it out.
 */
static __thread int ps_tls_cpu = 0;             /* this thread is the CPU thread */
static volatile int ps_cpu_on = 0;              /* a CPU thread is registered     */
static volatile uint32_t ps_cpu_seq __attribute__ ((aligned (64))) = 0;  /* odd = CPU thread in a transaction */
static uint32_t ps_cpu_seq_local;               /* CPU thread only                */
static volatile uint32_t ps_bus_req __attribute__ ((aligned (64))) = 0;  /* another thread wants the bus */
static uint64_t ps_handoff_ticks = 28;          /* PS_HANDOFF (500 ns, PISTORM_HANDOFF_NS) in arch-timer ticks */
static uint64_t ps_settle_ticks  = 9;           /* ~150 ns: the CPU's last posted GPIO writes */
static int ps_held_req = 0;                     /* holder of ps_txn_lock raised ps_bus_req */
static uint32_t ps_sample_seq;                  /* ps_bus_trylock: seq seen before the sample */

static inline uint64_t ps_ticks (void)
{
  uint64_t t;
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (t) :: "memory");
  return t;
}

static inline void ps_spin_ticks (uint64_t n)
{
  uint64_t t0 = ps_ticks ();
  while (ps_ticks () - t0 < n)
    asm volatile ("yield" ::: "memory");
}

static void __attribute__ ((noinline, cold)) ps_cpu_yield_bus (void)
{
  do {
    asm volatile ("dmb sy" ::: "memory");      /* our GPIO writes have landed */
    ps_cpu_seq = ++ps_cpu_seq_local;            /* even: not in a transaction  */
    while (ps_bus_req)
      asm volatile ("yield" ::: "memory");
    asm volatile ("dmb sy" ::: "memory");      /* the requester's have too    */
    ps_cpu_seq = ++ps_cpu_seq_local;            /* odd again                   */
    asm volatile ("" ::: "memory");
  } while (ps_bus_req);
}

/* Another thread's acquisition (ps_txn_lock already held) */
static void ps_others_handoff (void)
{
  ps_bus_active = 1;
  ps_held_req = 0;
  if (ps_cpu_on) {
    ps_bus_req = 1;
    ps_held_req = 1;
    asm volatile ("dmb sy" ::: "memory");
    ps_spin_ticks (ps_handoff_ticks);           /* req visible to the CPU core */
    while (ps_cpu_seq & 1)                      /* let its transaction finish  */
      asm volatile ("yield" ::: "memory");
    ps_spin_ticks (ps_settle_ticks);            /* its last posted GPIO writes */
  }
  asm volatile ("dmb sy" ::: "memory");
}

static void ps_others_release (void)
{
  asm volatile ("dmb sy" ::: "memory");        /* our GPIO writes have landed */
  if (ps_held_req) {
    ps_held_req = 0;
    ps_bus_req = 0;
  }
  ps_bus_active = 0;
  atomic_flag_clear_explicit (&ps_txn_lock, memory_order_release);
}

#ifdef PS_NOLOCK
static inline void ps_lock_bus(void) { ps_bus_active = 1; }
static inline void ps_unlock_bus(void) { ps_bus_active = 0; }
#else
static inline void ps_lock_bus(void)
{
  if (__builtin_expect (ps_tls_cpu, 1)) {
    ps_cpu_seq = ++ps_cpu_seq_local;            /* odd */
    asm volatile ("" ::: "memory");
    if (__builtin_expect (ps_bus_req, 0))
      ps_cpu_yield_bus ();
    return;
  }
  while (atomic_flag_test_and_set_explicit(&ps_txn_lock, memory_order_acquire))
    asm volatile ("yield" ::: "memory");
  ps_others_handoff ();
}

static inline void ps_unlock_bus(void)
{
  if (__builtin_expect (ps_tls_cpu, 1)) {
    asm volatile ("" ::: "memory");
    ps_cpu_seq = ++ps_cpu_seq_local;            /* even */
    return;
  }
  ps_others_release ();
}
#endif

/* Register / unregister the calling thread as THE CPU thread (one at a
 * time). Taken under ps_txn_lock, so no other thread is mid-acquisition. */
void ps_bus_cpu_thread (int on)
{
  while (atomic_flag_test_and_set_explicit (&ps_txn_lock, memory_order_acquire))
    asm volatile ("yield" ::: "memory");
  if (on) {
    ps_cpu_seq_local = ps_cpu_seq | 0;
    if (ps_cpu_seq_local & 1)
      ps_cpu_seq_local++;
    ps_cpu_seq = ps_cpu_seq_local;              /* even */
    ps_tls_cpu = 1;
    ps_cpu_on = 1;
  } else if (ps_tls_cpu) {
    ps_cpu_on = 0;
    ps_tls_cpu = 0;
  }
  asm volatile ("dmb sy" ::: "memory");
  atomic_flag_clear_explicit (&ps_txn_lock, memory_order_release);
}

/* PS_PROFILE (ataritest builds only - the emulator never defines it):
 * count the GPLEV polls each transaction spends waiting for the CPLD,
 * and expose one lock/unlock pair, so `ataritest --busprofile` can split
 * an access into "the CPLD running the real bus cycle" and "the Pi's own
 * GPIO work + lock" from measurements instead of estimates. */
#ifdef PS_PROFILE
uint64_t ps_prof_rd_spins, ps_prof_wr_spins;
void ps_prof_lock_pair (void) { ps_lock_bus (); ps_unlock_bus (); }
uint16_t ps_prof_read16_nolock (uint32_t addr);   /* defined after ps_read_txn */
void     ps_prof_write16_nolock (uint32_t addr, uint16_t data);
#define PS_PROF_ADD(v, n) ((v) += (n))
#else
#define PS_PROF_ADD(v, n) ((void)0)
#endif

static inline void ps_wait_idle(void)
{
  while ( __builtin_expect ((*ioread & PI_TXN_IN_PROGRESS), 1 ))
  //while (*ioread & PI_TXN_IN_PROGRESS)
    asm volatile ("yield" ::: "memory");
}

/* Non-blocking form of the pair above, for a thread that wants to LOOK at
 * GPLEV0 without running a transaction - ipl_task's IPL sample.
 *
 * The old bracket (test ps_bus_active, read, test it again) was a
 * probability argument, not an exclusion: nothing stopped a transaction
 * from beginning after the first test, and the second test only caught
 * it because a transaction outlives the two instructions between them.
 * ps_read_ipl() - the CPU thread's own sampler, used by intlev_ack -
 * has always taken the real lock instead, and that is the difference
 * between "almost never wrong" and "cannot be wrong". ipl_task must not
 * BLOCK behind the CPU thread, so it takes the same lock without
 * waiting and skips the sample when the bus is busy; the next pass is
 * 15 us away and every real IPL source is level-held.
 *
 * Returns 1 with the bus held (caller MUST call ps_bus_unlock), or 0. */
int ps_bus_trylock (void)
{
  /* Look before writing (a failed test-and-set is still a store that
   * steals the cache line). ps_bus_active is raised by every non-CPU
   * holder; the CPU thread is excluded by the seqlock check below. */
  if (ps_bus_active)
    return 0;
  if (atomic_flag_test_and_set_explicit (&ps_txn_lock, memory_order_acquire))
    return 0;                       /* another (non-CPU) thread owns the bus */
  ps_bus_active = 1;
  ps_held_req = 0;
  asm volatile ("dmb sy" ::: "memory");
  ps_sample_seq = ps_cpu_seq;
  if (ps_cpu_on && (ps_sample_seq & 1)) {      /* CPU thread mid-transaction */
    ps_bus_active = 0;
    atomic_flag_clear_explicit (&ps_txn_lock, memory_order_release);
    return 0;
  }
  ps_spin_ticks (ps_settle_ticks);              /* a transaction that just ended:
                                                 its posted GPIO writes land */
  return 1;
}

/* After ps_bus_trylock + the GPLEV0 read: was the sample exclusive? With a
 * CPU thread registered it waits PS_HANDOFF and checks that ps_cpu_seq
 * did not move - a CPU transaction that overlapped the read has, by then,
 * made its odd store visible. 1 = keep the sample. Always 1 after
 * ps_bus_lock_within (the CPU thread was stopped). */
int ps_bus_sample_ok (void)
{
  if (ps_held_req || !ps_cpu_on)
    return 1;
  asm volatile ("dmb sy" ::: "memory");
  ps_spin_ticks (ps_handoff_ticks);
  return ps_cpu_seq == ps_sample_seq;
}

/* Take the bus for a thread that must not be starved of it (and that may
 * run transactions with it), waiting at most `ticks` of the arch timer
 * for the other threads; the CPU thread is then stopped by the timed
 * hand-off (one transaction at most). Returns 1 with the bus held
 * (caller MUST ps_bus_unlock), 0 if it could not be had in time. */
int ps_bus_lock_within (uint64_t ticks)
{
  uint64_t t0 = ps_ticks ();
  for (;;)
  {
    if (!ps_bus_active &&
        !atomic_flag_test_and_set_explicit (&ps_txn_lock, memory_order_acquire))
    {
      ps_others_handoff ();
      return 1;
    }
    if (ps_ticks () - t0 >= ticks)
      return 0;
    asm volatile ("yield" ::: "memory");
  }
}

void ps_bus_unlock (void)
{
  ps_others_release ();
}


#ifdef PI5
/* RP1 as the CPU sees it: peripherals at 0x1f_0000_0000 (PCIe BAR);
 * io_bank0 +0xD0000, sys_rio0 +0xE0000, pads_bank0 +0xF0000. /dev/gpiomem0
 * maps exactly those three blocks (0x30000 from io_bank0) without root,
 * so it is the fallback when /dev/mem is not available. */
#define RP1_PERI_BASE   0x1f00000000ULL
#define RP1_IO_BANK0    0x0d0000u
#define RP1_GPIO_SPAN   0x030000u      /* io_bank0 + sys_rio0 + pads_bank0 */
#define RP1_RIO_OFF     0x010000u      /* from io_bank0                    */
#define RP1_PADS_OFF    0x020000u
#define RIO_SET_ALIAS   0x2000u
#define RIO_CLR_ALIAS   0x3000u
#define RIO_OUT         0x00u
#define RIO_OE          0x04u
#define RIO_SYNC_IN     0x0Cu

static void create_dev_mem_mapping ()
{
  void *map = MAP_FAILED;
  const char *how = "/dev/mem";
  int fd = open ( "/dev/mem", O_RDWR | O_SYNC );

  if ( fd >= 0 )
  {
    map = mmap ( NULL, RP1_GPIO_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, (off_t)(RP1_PERI_BASE + RP1_IO_BANK0) );
    close ( fd );
  }

  if ( map == MAP_FAILED )
  {
    how = "/dev/gpiomem0";
    fd = open ( "/dev/gpiomem0", O_RDWR | O_SYNC );
    if ( fd >= 0 )
    {
      map = mmap ( NULL, RP1_GPIO_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
      close ( fd );
    }
  }

  if ( map == MAP_FAILED )
  {
    printf ( "Unable to map RP1 GPIO (/dev/mem and /dev/gpiomem0), errno = %d\n", errno );
    exit ( -1 );
  }

  rp1_io   = (volatile uint32_t *)map;
  rp1_rio  = (volatile uint32_t *)((volatile uint8_t *)map + RP1_RIO_OFF);
  rp1_pads = (volatile uint32_t *)((volatile uint8_t *)map + RP1_PADS_OFF);

  ioset      = rp1_rio + (RIO_SET_ALIAS + RIO_OUT) / 4;
  ioclr      = rp1_rio + (RIO_CLR_ALIAS + RIO_OUT) / 4;
  ioread     = rp1_rio + RIO_SYNC_IN / 4;
  rio_oe_set = rp1_rio + (RIO_SET_ALIAS + RIO_OE) / 4;
  rio_oe_clr = rp1_rio + (RIO_CLR_ALIAS + RIO_OE) / 4;

  gpio = NULL;                         /* no GPFSEL on RP1 */
  printf ( "[INIT] Pi 5: RP1 GPIO mapped via %s\n", how );
}

/* GPIO 0-23 to RIO: io_bank0 CTRL funcsel 5 (SYS_RIO), no overrides.
 * Pads: input enabled on every pin (RIO_IN needs it, and an output's
 * level is read back too), not open-drain, 8 mA, Schmitt, fast slew.
 * Direction: outputs where the Pi 4 path has GPFSEL = output -
 * GPIO 1-3 (WR, CMD0, CMD1) and 8-23 (D0-D15); inputs GPIO 0 (TXN),
 * 4 (ST RESET, fw $256+), 5/6 (IPL1/2), 7 (BERR). GPIO 24+ are left alone. */
#define RP1_FUNCSEL_RIO   5u
#define RP1_PAD_PIN       0x63u        /* IE | DRIVE=8mA | SCHMITT | SLEWFAST */
#define PS_OUT_PINS       0x00FFFF0Eu
#define PS_ALL_PINS       0x00FFFFFFu

static void rp1_setup_pins ( void )
{
  *ioclr = PS_ALL_PINS;                /* outputs low before enabling them */
  *rio_oe_clr = PS_ALL_PINS;

  for ( int n = 0; n < 24; n++ )
  {
    rp1_pads [1 + n] = RP1_PAD_PIN;    /* word 0 is VOLTAGE_SELECT */
    rp1_io [2 * n + 1] = RP1_FUNCSEL_RIO;
  }

  *rio_oe_set = PS_OUT_PINS;
}
#else
/* Peripheral base from the device tree, so one build serves every board.
 * /proc/device-tree/soc/ranges is big-endian cells:
 *   BCM2837 (Pi 3B/3A+/Zero 2 W): <7e000000 3f000000 01000000>
 *                                 child(1) parent(1) size(1)
 *   BCM2711 (Pi 4/400/CM4):       <7e000000 00000000 fe000000 01800000> [...]
 *                                 child(1) parent(2) size(1)
 * The parent address is the cell(s) after the 0x7e000000 child address;
 * the high parent cell is 0 on BCM2711, so the first non-zero cell wins. */
uint32_t ps_peri_base ( void )
{
  static uint32_t base = 0;
  if ( base )
    return base;

  uint8_t buf [64];
  int fd = open ( "/proc/device-tree/soc/ranges", O_RDONLY );
  if ( fd >= 0 )
  {
    ssize_t n = read ( fd, buf, sizeof buf );
    close ( fd );
    for ( int i = 1; n >= 12 && i < n / 4; i++ )
    {
      uint32_t c = (uint32_t)buf [4*i] << 24 | (uint32_t)buf [4*i+1] << 16 |
                   (uint32_t)buf [4*i+2] << 8 | buf [4*i+3];
      if ( c == 0 )
        continue;                            /* high half of a 2-cell parent */
      if ( c == 0x3F000000u || c == 0xFE000000u || c == 0x20000000u )
      {
        base = c;
        break;
      }
      break;                                 /* unexpected layout: fall back */
    }
  }

  if ( !base )
  {
    base = BCM2708_PERI_BASE;
    printf ( "[GPIO] device tree unreadable - assuming peripheral base 0x%08X\n", base );
  }
  else
    printf ( "[GPIO] peripheral base 0x%08X (%s)\n", base,
             base == 0xFE000000u ? "BCM2711 / Pi 4" : "BCM2837 / Pi 3, Zero 2 W" );

  return base;
}

int ps_host_is_bcm2837 ( void )
{
  return ps_peri_base () == 0x3F000000u;
}

static void create_dev_mem_mapping ()
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
      ps_peri_base ()          // Offset to GPIO peripheral (run-time detected)
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
#endif

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

static void setup_gpclk ( void ) 
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


/* PI_CLK - GPCLK0 on GPIO4 for CPLD firmware 2.x (the fast-clock line).
 *
 * 2.x firmware times its read release with this clock (TXN drops 32-40 ns
 * after the read latches are clocked, instead of a whole half-clock later
 * at S7). 62.5 MHz from PLLD with an INTEGER divider - 750/12 on BCM2711,
 * 500/8 on BCM2837 - so there is no MASH fractional jitter.
 *
 * Only started when the CPLD answers major version >= 2: 1.x firmware
 * DRIVES GPIO4 (ST RESET to the Pi), and a Pi clock on the same wire would
 * fight it. PISTORM_FASTCLK=0 keeps it off (2.x then releases reads at S7,
 * exactly as 1.2e). The clock is stopped and GPIO4 made an input again at
 * exit; after a kill -9, reboot the Pi before flashing a 1.x firmware. */
int ps_fastclk = 0;
static int ps_pulse_sets;
static int fwrev_plausible (uint16_t fw);

#define CM_PASSWD    0x5a000000u
#define CM_SRC_PLLD  6u
#define CM_ENAB      (1u << 4)
#define CM_KILL      (1u << 5)
#define CM_BUSY      (1u << 7)

static void ps_fastclk_stop (void)
{
#ifndef PI5
  if (!ps_fastclk)
    return;
  ps_fastclk = 0;
  gpfsel0_in  = GPFSEL0_INPUT;          /* GPIO4 back to an input */
  gpfsel0_out = GPFSEL0_OUTPUT;
  gpio [0] = gpfsel0_out;
  *clk_ctl = CM_PASSWD | CM_SRC_PLLD;   /* ENAB off: stops at a clean edge */
  for (int i = 0; i < 100000 && (*clk_ctl & CM_BUSY); i++)
    ;
#endif
}

static void ps_fastclk_start (void)
{
#ifndef PI5
  const char *e = getenv ("PISTORM_FASTCLK");
  uint16_t fw = (uint16_t)((ps_read_status_reg () >> 8) & 0x07FF);
  int beta_rel = (fw & 0x600) == 0x400 || (fw & 0x600) == 0x000;
  int major = (fw >> 5) & 7;

  if (!beta_rel || major < 2 || !fwrev_plausible (fw))
    return;                             /* 1.x / alpha: GPIO4 is the CPLD's */

  /* 2.x protocol: short WR strobe (see txn_pulse) */
  {
    const char *st = getenv ("PISTORM_STROBE");
    int n = st ? atoi (st) : 2;     /* 2.1b busprofile: 3.55 / 3.89 MB/s, 0/0 */
    ps_pulse_sets = (n == 2 || n == 3) ? n : 0;     /* 4 / other = 1.x strobe */
    printf ("[GPIO] firmware %d.%d protocol: WR strobe %s%s\n", major, fw & 0x1f,
            ps_pulse_sets == 3 ? "3 sets + WR clear" :
            ps_pulse_sets == 2 ? "2 sets + WR clear" : "4 sets + 2 WR clears (1.x)",
            st ? " (PISTORM_STROBE)" : "");
  }
  if (e && *e == '0') {
    printf ("[GPIO] PI_CLK off (PISTORM_FASTCLK=0) - firmware releases reads at S7\n");
    return;
  }

  uint32_t div = ps_host_is_bcm2837 () ? 8 : 12;   /* PLLD 500 / 750 MHz */

  *clk_ctl = CM_PASSWD | CM_SRC_PLLD;   /* ENAB off, wait for it to stop */
  for (int i = 0; i < 100000 && (*clk_ctl & CM_BUSY); i++)
    ;
  if (*clk_ctl & CM_BUSY) {
    *clk_ctl = CM_PASSWD | CM_KILL;
    usleep (10);
  }
  *clk_div = CM_PASSWD | (div << 12);  /* integer divider, MASH 0 */
  usleep (10);
  *clk_ctl = CM_PASSWD | CM_SRC_PLLD;
  *clk_ctl = CM_PASSWD | CM_SRC_PLLD | CM_ENAB;
  for (int i = 0; i < 100000 && !(*clk_ctl & CM_BUSY); i++)
    ;

  /* GPIO4 = ALT0 (GPCLK0) in both direction-flip words, then apply */
  gpfsel0_in  = (gpfsel0_in  & ~(7u << 12)) | (4u << 12);
  gpfsel0_out = (gpfsel0_out & ~(7u << 12)) | (4u << 12);
  gpio [0] = gpfsel0_out;
  ps_fastclk = 1;
  atexit (ps_fastclk_stop);

  printf ("[GPIO] PI_CLK on GPIO4: %s MHz (PLLD/%u) for CPLD firmware %d.%d - "
          "reads released 32-40 ns after S6%s\n",
          "62.5", (unsigned)div, major, fw & 0x1f,
          (*clk_ctl & CM_BUSY) ? "" : " - WARNING: GPCLK0 did not report BUSY");
#endif
}

void ps_setup_protocol ( void ) 
{
  create_dev_mem_mapping ();
  setup_gpclk ();

#ifndef PI5
  {
    const char *e = getenv ("PISTORM_RD_SETTLE");
    ps_rd_settle = e ? atoi (e) : (ps_peri_base () == 0x3F000000u ? 2 : 0);
    if (ps_rd_settle < 0) ps_rd_settle = 0;
    if (ps_rd_settle > 8) ps_rd_settle = 8;
    printf ("[GPIO] read settle %d GPLEV sample%s after TXN%s\n", ps_rd_settle,
            ps_rd_settle == 1 ? "" : "s", e ? " (PISTORM_RD_SETTLE)" : "");
  }
#endif

#ifdef PI5
  rp1_setup_pins ();
#else
  /* GPFSEL2 covers GPIO 20-29, but PiStorm only owns 20-23 (bits 0-11).
   * Keep the rest as found: on a Pi 4, GPIO 28/29 are the Ethernet PHY's
   * MDIO/MDC (ALT5). Writing the whole register turned them into plain
   * inputs, cutting the GENET driver off from its PHY - with a cable
   * plugged in the kernel then hangs or reboots. */
  gpfsel2_keep = gpio [2] & ~0x00000FFFu;
  PS_BUS_OUT ();
#endif

  *ioclr = TXN_END;

  ps_fastclk_start ();                  /* 2.x firmware only */

  {
    uint64_t f;
    const char *h = getenv ("PISTORM_HANDOFF_NS");
    long ns = h ? atol (h) : 500;   /* > 2x store visibility, with margin */
    if (ns < 200) ns = 200;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (f));
    ps_handoff_ticks = (f * (uint64_t)ns) / 1000000000ull + 1;
    ps_settle_ticks  = (f * 150ull) / 1000000000ull + 1;
  }
}

typedef struct {
  uint32_t addr;
  uint16_t data;
  uint16_t io_type; // 
                    // WRITE BYTE = 0x01
                    // WRITE WORD = 0x00
                    // READ BYTE  = 0x03
                    // READ WORD  = 0x02
  uint8_t  berr;    // io related berr = 1
  uint8_t  fc;
} ps_io_t;


/* The hot paths cache the GPIO register pointers in const locals: the
 * spin loops' "memory"-clobbering yield (and any barrier) otherwise
 * forces the three GLOBAL pointers to be reloaded around every access.
 */
/* WR strobe.
 *
 * 1.x firmware (ps_pulse_sets == 0): 4 WR sets, 2 WR-only clears, then
 * TXN_END - unchanged (689ddc6: 4 stores of WR high).
 *
 * 2.x firmware: ps_pulse_sets WR sets (default 2 - firmware 2.1b,
 * 28 Oct 2026: read16 3.55 MB/s, write16 3.89 MB/s, 0/0 data checks;
 * 3 measured slower, 700 vs 545 ns reads), ONE WR-only clear, then TXN_END. The WR-only
 * clear must stay: the CPLD captures a0 / op_rw / op_fc / op_uds/lds /
 * status / rd_ok while WR is high and holds them as it falls - if TXN_END
 * dropped WR, CMD and data in the same store, that edge would see CMD and
 * data already cleared (ataritest --busprofile, 28 Oct 2026: reads 92%
 * wrong without it, 0/32768 with it). PISTORM_STROBE=2|3|4 overrides the
 * set count on 2.x firmware. Saves 2-3 GPIO stores per strobe, 3 strobes
 * per write and 2 per read: ~25-45 ns of Pi time per transaction, the
 * margin the next ST bus slot is won or lost by. */
static int ps_pulse_sets = 0;

__attribute__((always_inline)) static inline void txn_pulse (volatile uint32_t *const set,
                              volatile uint32_t *const clr)
{
  if (__builtin_expect (ps_pulse_sets == 2, 1)) {
    *set = PIN_WR; *set = PIN_WR;
    *clr = PIN_WR;
  } else if (ps_pulse_sets == 3) {
    *set = PIN_WR; *set = PIN_WR; *set = PIN_WR;
    *clr = PIN_WR;
  } else {
    *set = PIN_WR; *set = PIN_WR; *set = PIN_WR; *set = PIN_WR;
    *clr = PIN_WR; *clr = PIN_WR;
  }
  *clr = TXN_END;
}

void ps_read (ps_io_t *ps_io);
void ps_write ( ps_io_t *ps_io );

/* Stall attribution (measurement only): which blocking host path is the CPU
 * thread inside? Read by jit_stall_probe (newcpu.cpp) from the ipl thread.
 * 1=ps_read lock-wait  2=ps_read txn  3=ps_write lock-wait  4=ps_write txn
 * 5=et4k io rd  6=et4k io wr  7=et4k vram rd  8=et4k vram wr */
//volatile uint32_t g_cpu_where = 0;
//volatile uint32_t g_cpu_where_addr = 0;

/* The transaction cores run WITHOUT the bus lock so 32-bit accesses can
 * take it once for both halves. ps_lock_bus() already ends with a full
 * dmb sy (after raising ps_bus_active), so the cores need no barrier of
 * their own - the second back-to-back dmb the old bodies carried was
 * pure overhead. */

/* A bus cycle that never completes (TXN stuck high).
 *
 * Seen on the H5 board with firmware 253: a read of the keyboard ACIA
 * ($FFFC00 - a VPA/E-clock cycle) occasionally never finishes, the arbiter
 * idle (status bit 11 = 0). STATUS_BIT_INIT clears it (proved by a restart
 * recovering), so do that here instead of hanging forever: after ~50 ms -
 * far longer than any legitimate cycle, DMA tenures included - pulse INIT.
 * TXN is still set and the address/opcode are still latched, so the CPLD
 * simply re-runs the same cycle. Called with the bus lock held, in the
 * middle of a transaction's poll, so it talks to the CPLD directly.
 *
 * The counter is one add+compare per GPLEV read (~74 ns each): free.
 * 675000 x ~74 ns = ~50 ms. */
#define PS_TXN_STUCK_SPINS 675000u

static unsigned ps_txn_unsticks;

/* --- Atari power sensing --------------------------------------------
 *
 * The CPLD is powered by the ST. Its status readback (rev 253 Verilog:
 * {IPL2, IPL1, RESET_n, HALT_n, ~bus_mine, fwrev[10:0]}) is combinational
 * and works with TXN stuck, and a powered CPLD always answers the same
 * revision word - the one captured at start-up (ps_fw_rev). So "the CPLD
 * no longer answers that word" is the ST's power going, whatever the
 * floating pins happen to read (no assumption about all-ones/all-zeros,
 * no assumption about whether a dead bus leaves TXN high or low). Both
 * ways a dead bus can look are covered:
 *   - cycles never complete: the CPU thread sits in ps_txn_unstick with
 *     the bus lock held, so the sample is taken THERE, after the INIT;
 *   - cycles complete with garbage: the main thread samples through the
 *     lock (ps_power_poll) every 250 ms.
 * Either path, after PS_POWER_DEAD_SAMPLES consecutive mismatches, calls
 * pistorm_power_lost() (emulator.c), which never returns. */
#define PS_POWER_DEAD_SAMPLES 8          /* 8 x 50 ms stuck, or 8 x 250 ms polled */
#define PS_POWER_ALIVE_SAMPLES 5         /* stable answers before "power back" */

uint16_t ps_fw_rev;                      /* set by ps_get_firmware_revision */
static unsigned ps_power_dead;           /* consecutive mismatched samples */
/* emulator.c's strong definition tears the machine down and exits 42;
 * ataritest links this file without emulator.c, so a weak fallback */
__attribute__((weak)) void pistorm_power_lost (void)
{
  fprintf (stderr, "\n[POWER] Atari power lost (CPLD no longer answers firmware "
           "rev $%03X) - stopping\n", (unsigned)ps_fw_rev);
  exit (1);
}

static inline uint16_t status_fwrev (uint32_t status)
{
  return (uint16_t)((status >> 8) & 0x07FF);
}

/* A revision word the CPLD could actually be answering (0x7FF / 0x000 are
 * not: bits 10:9 = 11 is no release type, and 0x000 is "release 0.0" on an
 * EPM240 - never shipped). Used only at start-up to tell "no power" from
 * "old firmware"; at run time the exact start-up word is the test. */
static int fwrev_plausible (uint16_t fw)
{
  return fw != 0x7FF && fw != 0x000 && (fw & 0x600) != 0x600;
}

/* One dead/alive sample. Returns 1 when PS_POWER_DEAD_SAMPLES consecutive
 * samples disagree with the start-up revision. */
static int power_sample (uint32_t status)
{
  if (ps_fw_rev == 0)                    /* no reference yet (init) */
    return 0;
  if (status_fwrev (status) == ps_fw_rev) {
    ps_power_dead = 0;
    return 0;
  }
  return ++ps_power_dead >= PS_POWER_DEAD_SAMPLES;
}

static void __attribute__((noinline, cold))
ps_txn_unstick (const char *what, const ps_io_t *io, uint32_t lev, int is_read)
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;
  volatile uint32_t *const rd  = ioread;

  if (ps_txn_unsticks++ < 16)
    fprintf (stderr, "[PS] %s cycle STUCK ~50 ms: addr $%06X fc %u type %u "
             "data $%04X - GPLEV 0x%08X - INIT to restart it (%u so far)\n",
             what, (unsigned)(io->addr & 0xFFFFFF), (unsigned)io->fc,
             (unsigned)io->io_type, (unsigned)io->data, (unsigned)lev,
             ps_txn_unsticks);

  *clr = TXN_END;                       /* drop REG_DATA + WR of the poll */
  PS_BUS_OUT ();                        /* status writes drive D0-D15 */

  *set = (STATUS_BIT_INIT << 8) | REG_STATUS;
  txn_pulse (set, clr);
  for (int i = 0; i < 1000; i++)        /* ~74 us of INIT: >> one c8m edge */
    (void)*rd;
  *set = (0 << 8) | REG_STATUS;
  txn_pulse (set, clr);

  /* Is the CPLD still there? Same readback as ps_read_status_reg: CMD =
   * REG_STATUS with WR low (txn_pulse's TXN_END cleared CMD, so the
   * CPLD's st_wr_release has dropped), a moment to settle, sample. */
  {
    uint32_t st;
    PS_BUS_IN ();
    *set = REG_STATUS;
    for (int i = 0; i < 16; i++)        /* ~1 us: the readback is combinational */
      (void)*rd;
    st = *rd;
    *clr = TXN_END;
    PS_BUS_OUT ();
    if (power_sample (st))
      pistorm_power_lost ();            /* does not return */
  }

  if (is_read) {                        /* back into the read poll state */
    PS_BUS_IN ();
    *set = REG_DATA;
    *set = PIN_WR;
  }
}

__attribute__((always_inline))  static inline uint32_t ps_write_txn ( ps_io_t *ps_io )
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;
  volatile uint32_t *const lev = ioread;
  uint32_t status;

  *set = (ps_io->data << 8) | REG_DATA;
  txn_pulse (set, clr);

  *set = ((ps_io->addr & 0xffff) << 8) | REG_ADDR_LO;
  txn_pulse (set, clr);

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
  
  *set = (((ps_io->fc << 13) | ps_io->io_type | (ps_io->addr >> 16)) << 8) | REG_ADDR_HI;
  txn_pulse (set, clr);

  /* the GPLEV sample that shows the transaction complete already
   * carries the final status bits - the old separate status read was
   * one uncached GPIO read per transfer, thrown away */
  {
    uint32_t spins = 0;
    do {
      status = *lev;
      if (__builtin_expect (++spins == PS_TXN_STUCK_SPINS, 0)) {
        ps_txn_unstick ("write", ps_io, status, 0);
        spins = 0;
      }
    } while (status & PI_TXN_IN_PROGRESS);
    PS_PROF_ADD (ps_prof_wr_spins, spins);
  }

  ps_io->berr = CHECK_BERR (status);
  return status;
}

__attribute__((always_inline)) inline void ps_write ( ps_io_t *ps_io )
{
  ps_lock_bus ();
  ps_write_txn (ps_io);
  ps_unlock_bus();
}

inline void ps_write_8 (uint32_t addr, uint16_t data) 
{
  ps_io_t ps_io;

  ps_io.data = addr & 0x01 ? data & 0xFF : data << 8;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = WRITE_BYTE;

  ps_write (&ps_io);

  if (__builtin_expect(ps_io.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
}

inline void ps_write_16 (uint32_t addr, uint16_t data) 
{
  ps_io_t ps_io;

  ps_io.data = data;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = WRITE_WORD;

  ps_write (&ps_io);

  if (__builtin_expect(ps_io.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
}

inline void ps_write_32 (uint32_t addr, uint32_t data) 
{
  ps_io_t txn;

  txn.data = (uint16_t)(data >> 16);
  txn.addr = addr;
  txn.fc = fc;
  txn.io_type = WRITE_WORD;

  ps_lock_bus ();
  ps_write_txn (&txn);

  txn.data = (uint16_t)data;
  txn.addr = addr + 2;

  /* one lock + one barrier pair for the whole longword */
  
  ps_write_txn (&txn);
  ps_unlock_bus ();

  if (__builtin_expect(txn.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr + 2;
  }
}



__attribute__((always_inline)) static inline uint32_t ps_read_txn (ps_io_t *ps_io)
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;
  volatile uint32_t *const lev = ioread;
  uint32_t status;

  *set = ( (ps_io->addr & 0xffff) << 8 ) | REG_ADDR_LO;
  txn_pulse (set, clr);

  *set = (((ps_io->fc << 13) | ps_io->io_type | (ps_io->addr >> 16)) << 8) |  REG_ADDR_HI;
  txn_pulse (set, clr);

  PS_BUS_IN ();                         /* the CPLD drives D0-D15 now */
  *set = REG_DATA;
  *set = PIN_WR;

  {
    uint32_t spins = 0;
    do {
      status = *lev;
      if (__builtin_expect (++spins == PS_TXN_STUCK_SPINS, 0)) {
        ps_txn_unstick ("read", ps_io, status, 1);
        spins = 0;
      }
    } while (status & PI_TXN_IN_PROGRESS);
    PS_PROF_ADD (ps_prof_rd_spins, spins);
  }

  /* RE-SAMPLE AFTER THE HANDSHAKE - the zero-margin contract was a fault.
   *
   * Commit bd98cce ("the completing GPLEV sample IS the status - drop the
   * re-read") made the data come from the SAME sample that first observes
   * TXN_IN_PROGRESS low. That demands data be valid at the Pi's pads on
   * the very instant TXN falls - but TXN (GPIO0, CPLD) and data (GPIO8-23,
   * '374s) arrive via different drivers, flight paths and pads. It is a
   * nanosecond race, decided by temperature and Vdd, differently per boot.
   *
   * Measured (2026-08-17, dma_snoop vrfy instrument): 28-69 of 512 bytes
   * differing between two reads of UNCHANGING RAM on bad boots, zero on
   * good ones; whole sectors returning byte-swapped echoes of the Pi's
   * own address phases. A real 68000 never sees this because it samples
   * on a clock edge with datasheet setup; this loop sampled on a race.
   *
   * Each extra GPLEV read adds ~50-100ns of settle after the TXN edge.
   * Sweep result: 1 was not enough, 4 is clean.
   *
   * 2026-10-18: BCM2837 (Pi 3A+/3B/Zero 2 W) reads the GPIO block faster
   * than BCM2711, so with zero settle the sample lands on the edge and
   * ataritest shows ~0.02% of reads returning 0000/FFFF or echoes of the
   * opcode byte ($0300/$0100/$0081), at stock clocks. Sweep on a 3A+:
   * 0 fails, 1..3 clean with identical throughput, so the BCM2837 default
   * is 2 (one sample above the edge). BCM2711's ~74 ns GPLEV read is settle
   * enough on its own, so the Pi 4 default stays 0.
   * ps_rd_settle is set once in ps_setup_protocol (per SoC, or from
   * PISTORM_RD_SETTLE); the loop below is one predictable branch when 0. */
  for (int i = 0; i < ps_rd_settle; i++)
    status = *lev;

  *clr = TXN_END;
  PS_BUS_OUT ();
  ps_io->berr = CHECK_BERR (status);
  ps_io->data = status >> 8;

  /* Monitor-detect override (cfg `monitor mono|colour`).
   *
   * MFP GPIP bit 7 ($FFFA01) is the ST's monitor-detect wire: LOW =
   * SM124 mono, HIGH = colour. TOS reads it ONCE, in its first dozen
   * instructions, to choose the boot resolution - and that read was
   * observed to bypass every dispatcher-level shim we placed (TOS 1.04
   * took its colour branch at $FC00AA with no shimmed GPIP read logged,
   * while all other MFP traffic showed). This is the layer nothing can
   * bypass: every guest read - fast core, compat core, FC-tagged, byte,
   * word, or the long composed of two words - terminates in this
   * transaction, so the force is applied to the wire itself.
   *
   * Addressing: GPIP is the LOW byte. A byte read of $FFFA01 extracts
   * the low byte of data; a word read of $FFFA00 carries GPIP in its
   * low byte; the high byte of either is not GPIP. */
#if (1)
  {
    uint32_t a = ps_io->addr & 0x00FFFFFFu;
    if (__builtin_expect ((ps_io->io_type == READ_BYTE && a == 0x00FFFA01u) ||
                          (ps_io->io_type == READ_WORD && a == 0x00FFFA00u), 0)) {
      if (ps_gpip7_force == 1)
        ps_io->data &= ~0x0080u;     /* mono monitor present   */
      else if (ps_gpip7_force == 2)
        ps_io->data |=  0x0080u;     /* colour monitor present */
      else {
        /* Real wire, DEBOUNCED. The monitor-detect line is a DC level:
         * it changes when a monitor is unplugged, not between two reads
         * 20 ms apart. TOS 1.04 polls it every VBL and switches the
         * shifter to high resolution the moment it reads "mono" - and
         * one misread sample of this bit over the bus was enough to put
         * a colour machine into 71.4 Hz timing for the rest of the run
         * (field-measured: 72 VBL/s on a 50 Hz screen, some boots and
         * not others, games 44% fast). The value the guest sees only
         * changes after four consecutive reads agree, 80 ms at TOS's
         * poll rate, so a real monitor swap still works. */
        static int run = 0;
        static uint16_t bad_val = 0; static uint32_t bad_st = 0;
        int bit = (ps_io->data & 0x0080u) ? 1 : 0;
        if (ps_gpip7_stable < 0)
          ps_gpip7_stable = bit;
        else if (bit != ps_gpip7_stable) {
          bad_val = (uint16_t)ps_io->data; bad_st = status;   /* what was actually read */
          if (++run >= 4) {
            ps_gpip7_stable = bit; run = 0;
            fprintf (stderr, "[GPIP] monitor detect now %s\n", bit ? "COLOUR" : "MONO");
          }
        } else if (run) {
          if (ps_gpip7_glitches < 8)
            /* The byte read on the last disagreeing sample is the fact that
             * matters: $6F (bit 7 alone) = the wire went low; $00/$80/$FF or
             * other bits off = a corrupted transfer. Also the raw GPLEV word
             * (status bits 0-7 = CPLD status incl. IPL/BERR) and the size. */
            fprintf (stderr, "[GPIP] monitor-detect glitch: read %s %d time%s, kept %s "
                             "(value $%0*X %s, gplev=%08X)\n",
                     ps_gpip7_stable ? "mono" : "colour", run, run == 1 ? "" : "s",
                     ps_gpip7_stable ? "colour" : "mono",
                     ps_io->io_type == READ_BYTE ? 2 : 4, bad_val,
                     ps_io->io_type == READ_BYTE ? "byte $FFFA01" : "word $FFFA00", bad_st);
          ps_gpip7_glitches++;
          run = 0;
        }
        ps_gpip7_reads++;
        if (ps_gpip7_stable)
          ps_io->data |=  0x0080u;
        else
          ps_io->data &= ~0x0080u;
      }
    }
  }
#endif
  return status;
}

__attribute__((always_inline)) inline void ps_read (ps_io_t *ps_io)
{
  ps_lock_bus ();
  ps_read_txn (ps_io);
  ps_unlock_bus();
}

inline uint16_t ps_read_16 (uint32_t addr) 
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_WORD;

  ps_read (&ps_io);

  if (__builtin_expect(ps_io.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  return ps_io.data;
}

#ifdef PS_PROFILE
/* the same lock with inner-shareable barriers instead of full-system ones:
 * does a lighter lock alone bring an access back inside the ST's slot? */
static inline void ps_prof_lock_ish (void)
{
  while (atomic_flag_test_and_set_explicit(&ps_txn_lock, memory_order_acquire))
    asm volatile ("yield" ::: "memory");
  ps_bus_active = 1;
  asm volatile ("dmb ish" ::: "memory");
}
static inline void ps_prof_unlock_ish (void)
{
  asm volatile ("dmb ish" ::: "memory");
  ps_bus_active = 0;
  atomic_flag_clear_explicit(&ps_txn_lock, memory_order_release);
}
uint16_t ps_prof_read16_ish (uint32_t addr)
{
  ps_io_t io = { 0 };
  io.addr = addr; io.fc = fc; io.io_type = READ_WORD;
  ps_prof_lock_ish (); ps_read_txn (&io); ps_prof_unlock_ish ();
  return io.data;
}
void ps_prof_write16_ish (uint32_t addr, uint16_t data)
{
  ps_io_t io = { 0 };
  io.data = data; io.addr = addr; io.fc = fc; io.io_type = WRITE_WORD;
  ps_prof_lock_ish (); ps_write_txn (&io); ps_prof_unlock_ish ();
}

/* VARIANTS of one transaction, for measuring what each part of the
 * protocol costs and whether it can go. flags:
 *   PV_NOLOCK  no ps_lock_bus / ps_unlock_bus
 *   PV_NODIR   reads leave D0-D15 as Pi outputs (no GPFSEL flip) - the
 *              pre-13-Oct behaviour, CPLD and Pi both driving the pins
 *   PV_PULSE1  strobe = 1 WR set store (normal: 4 sets + 2 WR clears)
 *   PV_PULSE2  strobe = 2 WR set stores, no WR clears (TXN_END clears WR)
 * Only for ataritest: correctness is checked by the caller. */
/*   PV_WRCLR   with PULSE1/2: one WR-only clear before the TXN_END clear.
 *              The CPLD arms rd_ok on the FALLING edge of WR, sampling
 *              PI_CMD and PI_D[9]; when TXN_END clears WR, CMD and data in
 *              one store, that edge sees CMD=REG_DATA and rd_ok never arms
 *              - the read latch never reaches PI_D (the short-strobe read
 *              corruption, reproduced in simulation). */
/*   PV_HICLR   the WR-only clear on a READ's ADDR_HI pulse only - the one
 *              edge rd_ok is armed on; every other pulse stays short */
/*   PV_LK_FULL / PV_LK_ATOM / PV_LK_DMB  an explicit lock, independent of
 *              PS_NOLOCK: the emulator's lock (atomic + dmb sy each way), the
 *              atomic acquire/release alone, or the two dmb sy alone - where
 *              do its ~78 ns go? */
enum { PV_NOLOCK = 1, PV_NODIR = 2, PV_PULSE1 = 4, PV_PULSE2 = 8, PV_WRCLR = 16, PV_HICLR = 32,
       PV_LK_FULL = 64, PV_LK_ATOM = 128, PV_LK_DMB = 256 };

static inline void pv_lock (int f)
{
  if (f & (PV_LK_FULL | PV_LK_ATOM))
    while (atomic_flag_test_and_set_explicit (&ps_txn_lock, memory_order_acquire))
      asm volatile ("yield" ::: "memory");
  if (f & (PV_LK_FULL | PV_LK_ATOM | PV_LK_DMB)) {
    ps_bus_active = 1;
    if (f & (PV_LK_FULL | PV_LK_DMB))
      asm volatile ("dmb sy" ::: "memory");
    return;
  }
  if (!(f & PV_NOLOCK)) ps_lock_bus ();
}

static inline void pv_unlock (int f)
{
  if (f & (PV_LK_FULL | PV_LK_ATOM | PV_LK_DMB)) {
    if (f & (PV_LK_FULL | PV_LK_DMB))
      asm volatile ("dmb sy" ::: "memory");
    ps_bus_active = 0;
    if (f & (PV_LK_FULL | PV_LK_ATOM))
      atomic_flag_clear_explicit (&ps_txn_lock, memory_order_release);
    return;
  }
  if (!(f & PV_NOLOCK)) ps_unlock_bus ();
}

static inline void pv_pulse (volatile uint32_t *const set, volatile uint32_t *const clr, int f)
{
  if (f & PV_PULSE1) { *set = PIN_WR; if (f & PV_WRCLR) *clr = PIN_WR; *clr = TXN_END; return; }
  if (f & PV_PULSE2) { *set = PIN_WR; *set = PIN_WR; if (f & PV_WRCLR) *clr = PIN_WR; *clr = TXN_END; return; }
  txn_pulse (set, clr);
}

static int pv_wait (volatile uint32_t *const lev, uint32_t *st)
{
  uint32_t spins = 0, status;
  do {
    status = *lev;
    if (++spins > 10000000u) { *st = status; return -1; }
  } while (status & PI_TXN_IN_PROGRESS);
  ps_prof_rd_spins += spins;
  *st = status;
  return 0;
}

uint16_t ps_prof_read16_var (uint32_t addr, int f, int *err)
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;
  volatile uint32_t *const lev = ioread;
  uint32_t status;
  pv_lock (f);
  *set = ((addr & 0xffff) << 8) | REG_ADDR_LO;
  pv_pulse (set, clr, f);
  *set = (((fc << 13) | READ_WORD | (addr >> 16)) << 8) | REG_ADDR_HI;
  pv_pulse (set, clr, (f & PV_HICLR) ? (f | PV_WRCLR) : f);
  if (!(f & PV_NODIR)) PS_BUS_IN ();
  *set = REG_DATA;
  *set = PIN_WR;
  if (pv_wait (lev, &status)) *err = 1;
  for (int i = 0; i < ps_rd_settle; i++)
    status = *lev;
  *clr = TXN_END;
  if (!(f & PV_NODIR)) PS_BUS_OUT ();
  pv_unlock (f);
  return (uint16_t)(status >> 8);
}

void ps_prof_write16_var (uint32_t addr, uint16_t data, int f, int *err)
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;
  volatile uint32_t *const lev = ioread;
  uint32_t status;
  pv_lock (f);
  *set = (data << 8) | REG_DATA;
  pv_pulse (set, clr, f);
  *set = ((addr & 0xffff) << 8) | REG_ADDR_LO;
  pv_pulse (set, clr, f);
  *set = (((fc << 13) | WRITE_WORD | (addr >> 16)) << 8) | REG_ADDR_HI;
  pv_pulse (set, clr, f);
  if (pv_wait (lev, &status)) *err = 1;
  pv_unlock (f);
}

/* one GPFSEL flip each way (D0-D15 in, then back out), on an idle bus */
void ps_prof_dir_pair (void) { PS_BUS_IN (); PS_BUS_OUT (); }

/* the same transactions without the bus lock (ataritest is the only
 * thread on the bus): what the lock and its two dmb sy barriers cost */
uint16_t ps_prof_read16_nolock (uint32_t addr)
{
  ps_io_t io = { 0 };
  io.addr = addr; io.fc = fc; io.io_type = READ_WORD;
  ps_read_txn (&io);
  return io.data;
}
void ps_prof_write16_nolock (uint32_t addr, uint16_t data)
{
  ps_io_t io = { 0 };
  io.data = data; io.addr = addr; io.fc = fc; io.io_type = WRITE_WORD;
  ps_write_txn (&io);
}
#endif

inline uint16_t ps_read_16_fc (uint32_t addr, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = READ_WORD;

  ps_read (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;

  if (__builtin_expect(ps_io.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  return ps_io.data;
}

inline uint8_t ps_read_8 (uint32_t addr) 
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_BYTE;

  ps_read (&ps_io);

  if (__builtin_expect(ps_io.berr, 0)) {
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

inline uint8_t ps_read_8_fc (uint32_t addr, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = READ_BYTE;

  ps_read (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;

  if (__builtin_expect(ps_io.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }

  if ((addr & 1) == 0)
    return (uint8_t)(ps_io.data >> 8);
  return (uint8_t)ps_io.data;
}

/* For threads other than the CPU thread (ipl_task, the stats thread).
 * The function code is an argument - never the CPU thread's `fc` global,
 * which says what the GUEST is doing at that moment and is user mode for
 * most of a MiNT session - and a bus error is reported through berr_out
 * ONLY. g_buserr is the CPU thread's sticky flag (clear, transact, check,
 * raise a guest bus error); another thread setting it can land between
 * that clear and that check. */
uint8_t ps_read_8_quiet (uint32_t addr, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = READ_BYTE;

  ps_read (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;

  if ((addr & 1) == 0)
    return (uint8_t)(ps_io.data >> 8);
  return (uint8_t)ps_io.data;
}

void ps_write_8_quiet (uint32_t addr, uint8_t data, uint8_t fc_value, uint8_t *berr_out)
{
  ps_io_t ps_io;

  ps_io.data = (addr & 0x01) ? data : (uint16_t)(data << 8);
  ps_io.addr = addr;
  ps_io.fc = fc_value;
  ps_io.io_type = WRITE_BYTE;

  ps_write (&ps_io);

  if (berr_out)
    *berr_out = ps_io.berr;
}

/* ---- keyboard ACIA receive rescue ----------------------------------
 *
 * The 6850 holds ONE received byte. The next byte completes 1.28 ms
 * later (10 bits at 7812.5 baud) and, if the first is still unread, is
 * lost as an overrun. The guest reads the ACIA from its level-6 handler,
 * and the CPU thread can only take that interrupt between host calls: a
 * long NatFeat (field case: FVDI BLIT_AREA while scrolling a picture,
 * 1-8 ms) holds it off for longer than one character. A byte lost in a
 * mouse packet turns the rest of the packet into key presses with no
 * key release, and TOS key repeat then clicks until a key is released -
 * the constant beeping.
 *
 * So once a level 6 has waited 250 us unmasked, ipl_task empties the
 * receiver into this FIFO (ps_acia_kbd_rescue), and every guest read of
 * $FFFC00/$FFFC02 goes through ps_acia_kbd_read8/16, which serve the
 * FIFO before the chip: status shows RDRF|IRQ while it holds bytes, a
 * data read pops it. Nothing is invented and nothing is reordered - the
 * FIFO only ever holds bytes taken out of the chip, oldest first, and
 * the chip cannot hold a newer one than the FIFO's last.
 *
 * Both sides run UNDER THE BUS LOCK. That is what makes the FIFO test
 * and the chip access one step: a guest read cannot see "FIFO empty",
 * lose the bus to a rescue, and then read the chip's data register
 * again (which would return the byte just taken - a duplicate). It is
 * also why a rescue is correct wherever the CPU thread happens to be,
 * even between a guest's status read and its data read: the status
 * said RDRF, and the data read then pops that same byte from the FIFO.
 *
 * The MFP needs nothing: the byte's arrival already latched channel 6
 * pending (GPIP4 edge), so the interrupt still comes when the call
 * returns, and the handler - TOS, EmuTOS and FreeMiNT all loop while
 * GPIP4 is low - finds the bytes. pistorm_natmem.cpp's GPIP shim holds
 * GPIP4 low while the FIFO is not empty (ps_acia_kbd_pending). */
#define ACIA_KBD_CTRL  0x00FFFC00u
#define ACIA_KBD_DATA  0x00FFFC02u
/* Power of two. Sized for the longest host call seen, not the typical
 * one: PSIMG.1 (decoding the APJ-OS wallpaper) held the CPU thread for
 * 177 and 200 ms back to back at startup, and a moving mouse filled a
 * 64-byte FIFO and lost 68 bytes. The IKBD link cannot deliver more than
 * 781 bytes a second (7812.5 baud, 10 bits a byte), so 1024 bytes covers
 * over a second of continuous traffic. */
#define ACIA_KQ_SIZE   1024u
static uint8_t           acia_kq[ACIA_KQ_SIZE];
static volatile unsigned acia_kq_head;     /* next to pop (CPU thread)   */
static volatile unsigned acia_kq_tail;     /* next to push (ipl_task)    */
volatile unsigned ps_acia_kbd_rescued = 0; /* bytes taken by ipl_task    */
volatile unsigned ps_acia_kbd_dropped = 0; /* FIFO full: byte lost       */

int ps_acia_kbd_pending (void)
{
  return acia_kq_head != acia_kq_tail;
}

/* Guest byte read of $FFFC00 or $FFFC02 (any other address: a plain
 * read). Same function-code and g_buserr behaviour as ps_read_8. */
uint8_t ps_acia_kbd_read8 (uint32_t addr)
{
  ps_io_t ps_io;
  uint32_t a = addr & 0x00FFFFFFu;
  uint8_t v;

  ps_lock_bus ();
  if (a == ACIA_KBD_DATA && acia_kq_head != acia_kq_tail)
  {
    v = acia_kq[acia_kq_head & (ACIA_KQ_SIZE - 1u)];
    acia_kq_head = acia_kq_head + 1u;
    ps_unlock_bus ();
    return v;
  }
  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_BYTE;
  ps_read_txn (&ps_io);
  v = (addr & 1) ? (uint8_t)ps_io.data : (uint8_t)(ps_io.data >> 8);
  if (a == ACIA_KBD_CTRL && acia_kq_head != acia_kq_tail)
    v |= 0x81u;                            /* IRQ | RDRF                */
  ps_unlock_bus ();

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
  return v;
}

/* Guest word read of $FFFC00 or $FFFC02: the 6850 is on D8-D15, so the
 * register is the HIGH byte. */
uint16_t ps_acia_kbd_read16 (uint32_t addr)
{
  ps_io_t ps_io;
  uint32_t a = addr & 0x00FFFFFFu;
  uint16_t v;

  ps_lock_bus ();
  if (a == ACIA_KBD_DATA && acia_kq_head != acia_kq_tail)
  {
    v = (uint16_t)((acia_kq[acia_kq_head & (ACIA_KQ_SIZE - 1u)] << 8) | 0xFFu);
    acia_kq_head = acia_kq_head + 1u;
    ps_unlock_bus ();
    return v;
  }
  ps_io.data = 0;
  ps_io.addr = addr;
  ps_io.fc = fc;
  ps_io.io_type = READ_WORD;
  ps_read_txn (&ps_io);
  v = ps_io.data;
  if (a == ACIA_KBD_CTRL && acia_kq_head != acia_kq_tail)
    v |= 0x8100u;                          /* IRQ | RDRF                */
  ps_unlock_bus ();

  if (ps_io.berr) {
    g_buserr = 1;
    g_buserr_addr = addr;
  }
  return v;
}

/* Guest wrote the keyboard ACIA's control register. A master reset
 * (CR1:CR0 = 11) clears the chip's receiver; the FIFO holds bytes that
 * were in that receiver, so it is cleared with it - otherwise bytes from
 * before an IKBD/ACIA reset would reach the guest after it. */
void ps_acia_kbd_ctrl_written (uint8_t cr)
{
  if ((cr & 0x03u) != 0x03u)
    return;
  ps_lock_bus ();
  acia_kq_head = acia_kq_tail;
  ps_unlock_bus ();
}

/* ipl_task: if the keyboard ACIA holds a byte, move it into the FIFO.
 * Never waits for the bus. Supervisor function code, never touches
 * g_buserr. Returns 1 if a byte was taken, 0 otherwise. */
int ps_acia_kbd_rescue (void)
{
  ps_io_t ps_io;
  int took = 0;

  /* Up to 50 us for the bus: the rescue is only ever running because a
   * byte is at risk, and a trylock that keeps losing the hand-off to a
   * busy CPU thread would let it overrun. */
  {
    static uint64_t wait_ticks;
    if (!wait_ticks)
    {
      uint64_t f;
      __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (f));
      wait_ticks = (f ? f : 54000000ULL) / 20000u;
    }
    if (!ps_bus_lock_within (wait_ticks))
      return 0;
  }
  {
    ps_io.data = 0;
    ps_io.addr = ACIA_KBD_CTRL;
    ps_io.fc = 5;
    ps_io.io_type = READ_BYTE;
    ps_read_txn (&ps_io);
    if (!ps_io.berr && ((ps_io.data >> 8) & 0x01u))       /* RDRF */
    {
      ps_io.data = 0;
      ps_io.addr = ACIA_KBD_DATA;
      ps_io.fc = 5;
      ps_io.io_type = READ_BYTE;
      ps_read_txn (&ps_io);
      if (!ps_io.berr)
      {
        if (acia_kq_tail - acia_kq_head < ACIA_KQ_SIZE)
        {
          acia_kq[acia_kq_tail & (ACIA_KQ_SIZE - 1u)] = (uint8_t)(ps_io.data >> 8);
          acia_kq_tail = acia_kq_tail + 1u;
          ps_acia_kbd_rescued++;
          took = 1;
        }
        else
          ps_acia_kbd_dropped++;
      }
    }
  }
  ps_bus_unlock ();
  return took;
}

inline uint32_t ps_read_32 (uint32_t addr) 
{
  ps_io_t txn;
  uint16_t hi_data;

  txn.data = 0;
  txn.addr = addr;
  txn.fc = fc;
  txn.io_type = READ_WORD;

  ps_lock_bus ();
  ps_read_txn (&txn);

  hi_data = txn.data;
  txn.addr = addr + 2;

  /* one lock + one barrier pair for the whole longword */
  ps_read_txn (&txn);
  ps_unlock_bus ();

  if (__builtin_expect(txn.berr, 0)) {
    g_buserr = 1;
    g_buserr_addr = addr + 2;
  }

  return ((uint32_t)hi_data << 16) | txn.data;
}



/* Bounded idle wait for the status-register paths ONLY.
 *
 * A status write is how the Pi resets the CPLD (STATUS_BIT_INIT / RESET),
 * and the CPLD latches REG_STATUS on CMDWR whatever its cycle engine is
 * doing - it does not need TXN low. Waiting for TXN without a limit meant a
 * CPLD left with TXN stuck high (a cycle that never completed) could not be
 * reset by anything short of a power cycle: the emulator hung at the first
 * status read after the clock lines, and `ataritest --reset` hung too.
 * ~150 ms of polling, then carry on. Reports the raw GPLEV once per stuck
 * episode: bit 0 TXN, bits 8+ the data pins. */
static int ps_wait_idle_bounded (const char *who)
{
  static int stuck_reported;
  for (uint32_t n = 0; n < 2000000u; n++) {
    if (!(*ioread & PI_TXN_IN_PROGRESS)) {
      stuck_reported = 0;
      return 0;
    }
    asm volatile ("yield" ::: "memory");
  }
  if (!stuck_reported) {
    stuck_reported = 1;
    fprintf (stderr, "[PS] %s: TXN stuck high (GPLEV 0x%08X) - CPLD cycle "
             "never completed; going ahead anyway\n", who, (unsigned)*ioread);
  }
  return -1;
}

void ps_write_status_reg ( uint16_t value ) 
{
  volatile uint32_t *const set = ioset;
  volatile uint32_t *const clr = ioclr;

  ps_lock_bus();

  /* make sure no IO in progress - but never forever, see above */
  ps_wait_idle_bounded ("ps_write_status_reg");
  
  *set = (value << 8) | REG_STATUS;
  txn_pulse (set, clr);

  /* when writing status, there isn't a PI_TXN_IN_PROGRESS */
  ps_unlock_bus();
}


uint32_t ps_read_status_reg () 
{
  uint32_t status;

  ps_lock_bus();
  asm volatile ("dmb sy" : : : "memory");

  /* make sure no IO in progress - bounded: the status readback is
   * combinational in the CPLD and works with TXN stuck, which is exactly
   * when it is most wanted (bit 11 = arbitration state on fw 253) */
  int stuck = ps_wait_idle_bounded ("ps_read_status_reg");
  
  PS_BUS_IN ();                         /* the CPLD drives the status word */
  *ioset = REG_STATUS;
  
  if (!stuck)
    ps_wait_idle_bounded ("ps_read_status_reg");

  status = *ioread;
  if (stuck) {
    /* once per distinct reading, not per call: ps_power_wait samples
     * this every few hundred ms for as long as the ST is off */
    static uint32_t last_stuck_status = ~0u;
    if (status != last_stuck_status) {
      last_stuck_status = status;
      fprintf (stderr, "[PS] status read with TXN stuck: GPLEV 0x%08X - "
               "word 0x%04X, bit11 (arbitration active) = %u\n",
               (unsigned)status, (unsigned)((status >> 8) & 0xFFFF),
               (unsigned)((status >> 19) & 1));
    }
  }
 	*ioclr = TXN_END;
  PS_BUS_OUT ();

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
}

/* toggle HALT signal */
void ps_pulse_halt () 
{
  ps_write_status_reg ( STATUS_BIT_HALT );
  usleep ( 1000 );
  ps_write_status_reg ( 0 );
}

/* hold reset low for 250ms - Atari ST needs min 100ms */
void ps_pulse_reset () 
{
  ps_write_status_reg ( STATUS_BIT_RESET );
  usleep ( 15 );
  ps_write_status_reg ( 0 );
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

/* Seed the monitor-detect debounce before the guest boots.
 *
 * The debounce in ps_read_txn() protects every read of the bit EXCEPT
 * the first one: `stable` starts at -1 and the first sample becomes the
 * held value with nothing to compare it against. That one read is the
 * one that matters - TOS decides the boot resolution from the monitor
 * bit in the ROM's first dozen instructions, and a single bad sample
 * there puts a colour machine into 640x400 and 71.4 Hz, which is 44%
 * fast for anything VBL-paced.
 *
 * So take the sample here, before the CPU thread exists, with time to
 * take it properly: 17 reads of a DC line, majority wins. The spread is
 * printed because it is the measurement that says whether the wire is
 * trustworthy at all - "colour 17/17" and "colour 9 / mono 8" mean very
 * different things and only one of them is a monitor.
 *
 * No-op when `monitor mono|colour` has already forced the bit. */
void ps_gpip7_seed (void)
{
  int mono = 0, colour = 0;

  if (ps_gpip7_force)
    return;

  for (int i = 0; i < 17; i++) {
    uint8_t berr = 0;
    uint8_t g = ps_read_8_fc (0x00FFFA01u, 5, &berr);
    if (berr)
      continue;
    if (g & 0x80u) colour++; else mono++;
  }

  if (!mono && !colour) {
    fprintf (stderr, "[GPIP] monitor detect: no readable sample at startup, "
                     "leaving it to the first guest read\n");
    return;
  }

  ps_gpip7_stable = colour >= mono ? 1 : 0;
  fprintf (stderr, "[GPIP] monitor detect at startup: %s (colour %d / mono %d "
                   "of 17)%s\n",
           ps_gpip7_stable ? "COLOUR" : "MONO", colour, mono,
           (mono && colour) ? "  <- the wire is not steady, this is suspect"
                            : "");
}

/* read GPIO pins 0-7 - check that an IO is not running first */
void ps_read_ipl (uint8_t *ipl)
{
  ps_lock_bus();
  asm volatile ("dmb sy" : : : "memory");
  ps_wait_idle ();

  *ipl = (*ioread & 0x60) >> 4; // GPIO5 & GPIO6 = IPL1 & IPL2

  ps_unlock_bus();
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

  ps_fw_rev = fwrev_plausible (fw) ? fw : 0;   /* reference for power sensing */
  if (!ps_fw_rev) {
    printf ( "[INIT] PiSTorm firmware: CPLD not answering (word $%03X)\n", fw );
    return;
  }

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


/* ST RESET on GPIO4 - see ps_protocol.h. Only EPM240 firmware from alpha
 * $256 (and any beta) drives the pin; with older firmware GPIO4 is an
 * unused CPLD pin with a weak pull-up, so it would read "high" anyway,
 * but say "no pin" rather than pretend. */
int ps_st_reset_pin ( void )
{
  if (ps_fastclk)
    return -1;                           /* GPIO4 is PI_CLK (firmware 2.x) */
#ifdef USING_PI_CLK
  return -1;                             /* GPIO4 is GPCLK0 in this build */
#else
  uint16_t fw = ps_fw_rev;

  /* alpha EPM240 rev 256+, or any beta (EPM240 $4xx / EPM570 $5xx) */
  if ( !( ((fw & 0x700) == 0x200 && fw >= CPLD_FW_RESET_PIN) ||
          (fw & 0x600) == 0x400 ) )
    return -1;

  return ( *ioread & PIN_ST_RESET ) ? 0 : 1;
#endif
}


/* Main-thread power sample. The CPU thread holds the bus lock for whole
 * cycles; a cycle that is merely slow (a DMA tenure) is a few ms, so give
 * the lock up to ~5 ms and otherwise report "no sample" rather than stall
 * the caller. Never spins on a lock the CPU thread holds for ever (the
 * stuck case): that case is sensed inside ps_txn_unstick instead. */
int ps_power_poll ( void )
{
  if (ps_fw_rev == 0)
    return -1;
  {
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (f));
    if (!ps_bus_lock_within (f / 200))  /* ~5 ms */
      return -1;
  }

  /* body of ps_read_status_reg, lock already held */
  PS_BUS_IN ();
  *ioset = REG_STATUS;
  ps_wait_idle_bounded ("ps_power_poll");
  uint32_t status = *ioread;
  *ioclr = TXN_END;
  PS_BUS_OUT ();

  ps_unlock_bus ();
  return power_sample (status) ? 1 : 0;
}

/* Block until the ST is powered: the CPLD answers a plausible revision
 * (the start-up one, when known) with M68K RESET_n high - the ST's own
 * power-on reset over - for PS_POWER_ALIVE_SAMPLES consecutive samples
 * 200 ms apart. Prints once on entry and once on return. Called before
 * the emulation threads exist (start-up), so plain status reads. */
void ps_power_wait ( void )
{
  unsigned alive = 0;
  printf ("[POWER] Atari is off (CPLD not answering) - waiting for power...\n");
  fflush (stdout);
  for (;;) {
    uint32_t st = ps_read_status_reg ();
    uint16_t fw = status_fwrev (st);
    int ok = (ps_fw_rev ? fw == ps_fw_rev : fwrev_plausible (fw)) &&
             ((st >> 8) & 0x2000);      /* bit 13: RESET_n high */
    alive = ok ? alive + 1 : 0;
    if (alive >= PS_POWER_ALIVE_SAMPLES)
      break;
    usleep (200000);
  }
  ps_power_dead = 0;
  printf ("[POWER] power is back (firmware rev $%03X, RESET_n high) - starting\n",
          (unsigned)status_fwrev (ps_read_status_reg ()));
  fflush (stdout);
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
