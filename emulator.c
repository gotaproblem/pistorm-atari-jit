// SPDX-License-Identifier: MIT

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include <unistd.h>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <chrono> // For non-blocking timing

#include <termios.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>       /* nanosleep() for the IPL stats thread */

#include "platforms/atari/et4000/et4000.h"
// #include "platforms/atari/et4000/native_vga.h"
#include "config_file/config_file.h"
#include "gpio/ps_protocol.h"
#include "platforms/atari/audio/dmasnd.h"
#include "platforms/atari/machine_cookie.h"
#include "platforms/atari/mfp_hub.h"
#include "platforms/atari/stbox/stbox.h"
#include "platforms/atari/audio/ym2149.h"
#include "platforms/atari/st_blitter.h"
#include "sysdeps.h"
#include "threaddep/thread.h"

/* JIT bridge entry points (jit_glue.cpp) */
#ifdef __cplusplus
extern "C"
{
#endif

  extern void jit_mem_init(void);
  extern void jit_cpu_set_perf_options(int cpu_clock_multiplier, int cpu_clock_multiplier_set,
                                       int m68k_speed, int m68k_speed_set,
                                       int jit_cache, int jit_cache_set);
  extern void jit_cpu_init(int cpu_level, int enable_fpu, int enable_ttram, int enable_addr32, int enable_jit);
  extern void jit_cpu_set_compatible(int on);
  extern void jit_cpu_set_mmu(int model);
  extern void pistorm_reset_state_dump(void);   /* cpu/newcpu.cpp */
  extern uint8_t pistorm_mfp_gpip_shim(uint8_t); /* pistorm_natmem.cpp */
  extern void pistorm_rez_sync_trace(uint32_t a, uint8_t v); /* pistorm_natmem.cpp */
  extern void jit_cpu_reset(void);
  extern void jit_cpu_execute(void);
  extern void pistorm_set_blitter_enabled(int enabled);
  extern void pistorm_set_blitter_mode(int mode);

#ifdef __cplusplus
}
#endif

extern int quit_program;
#ifdef WITH_THREADED_CPU
extern uae_sem_t cpu_wakeup_sema;
#endif
#include "platforms/atari/et4000/et4000.h"
#include "platforms/atari/IDE.h"
#include "platforms/atari/idedriver.h"
#include "platforms/atari/fdd/atari_fdd.h"
#include "platforms/atari/fdd/acsi.h"
#include "platforms/atari/fdd/platform_atari_fdd.h"
#include "platforms/atari/network/platform_atari_network.h"
#include "platforms/atari/kbd_usb.h"
#include "gpio/bus_lock.h"

#define IDEBASEADDR 0x00F00000
#define IDETOPADDR 0x00F00100

/* cpu_level is passed to jit_cpu_init() — values match ARAnyM: 0=68000 .. 4=68040 */
//static int cpu_level_jit = 3; /* default 68030 */

extern volatile uint32_t *ioread;

uint8_t emulator_exiting = 0;
volatile int cpu_emulation_running = 0;
static volatile uint8_t g_reset;
static volatile int pulse_reset_inprogress;
static volatile int reset_emulation;
static volatile int g_iack_in_progress;

bool screenGrab;

bool DMA_Sound_enabled;

/* ROM setup */
uint8_t rom_vector[8]; // first 8 bytes mirrored from TOS ROM to addresses 0x0 to 0x7
static uint8_t *rom_ptr, *tos_ptr_big, *tos_ptr_small, *cart_rom_ptr;
static uint8_t *rom_vector_ptr;
uint32_t ROM_START, ROM_END, ROM_MASK;

/* Exposed to memory-uae.h for direct ROM address mapping */
uint8_t *pistorm_rom_ptr = NULL;
uint32_t pistorm_rom_start = 0;
uint32_t pistorm_rom_end = 0;
uint32_t pistorm_rom_mask = 0;

/* TT-RAM setup */
uint8_t *tt_ram;
bool tt_ram_available;
#define TT_RAM_SIZE ((128u * 1024 * 1024) + 0x01000000u) // 128MB + 16MB

/* FDD setup */
extern "C" void *fdd_vbl_thread(void *arg);
extern "C" void fdd_vbl(void);

/* JIT statistics dump (C++ linkage, jit/arm/compemu_support_arm.cpp) */
extern void compiler_dump_stats(void);

/* IPL latch counters per level, read by the dmasnd =2 summary line
 * (i2/i4/i6): how many interrupt requests ipl_task presented to the CPU
 * per level. Single writer (ipl_task); the 1 Hz reader tolerates tears. */
extern "C" {
volatile unsigned pistorm_ipl_lat2 = 0;
volatile unsigned pistorm_ipl_lat4 = 0;
volatile unsigned pistorm_ipl_lat6 = 0;
/* IPL ASSERTION EPISODE counters: how many times the confirmed level
 * ENTERED 2/4/6, as distinct from how many interrupts we then presented
 * (the lat counters above). One is what the Atari's line did, the other
 * is what the guest saw - the difference is the whole redelivery
 * question. Counted on the level transition, not per poll sample, so
 * these are event rates and not sample rates. */
volatile unsigned pistorm_ipl_ep2 = 0;
volatile unsigned pistorm_ipl_ep4 = 0;
volatile unsigned pistorm_ipl_ep6 = 0;
/* HBL no-op skips: level-2 assertions dropped at the sampler because the
 * guest's HBL vector is a bare RTE (see pistorm_hbl_handler_is_rte). */
volatile unsigned pistorm_ipl_hbl_skipped = 0;
/* Verdict helper, defined in cpu/newcpu.cpp (needs regs.vbr). */
int pistorm_hbl_handler_is_rte(void);
}
bool FDD_enabled;

/* ATARI RAM cache setup */
//#define ADDR_MFP_GPIP 0x00FFFA01 // MFP General Purpose I/O
//#define ST_RAM_SIZE (4)          // 4MB Cache
//uint32_t STRAM_MAX_ADDR;
//bool RAM_CACHE_enabled;
uint8_t *st_ram_cache;

/* IDE setup */

/* ET4K setup */
bool ET4K_emutos_vga;
extern volatile int et4000_thread_ready;

#ifdef __cplusplus
extern "C"
{
#endif
  extern uint8_t *et4000_engine_vram_ptr(void);
#ifdef __cplusplus
}
#endif

ET4KADDRESSES_s et4kaddresses[GRAPHICS_DRIVERS] =
{
  {0x00000000, 0x00000000, 0x00000000, 0x00000000}, // NONE
  {0x00D00000, 0x00E00000, 0x00C00000, 0x00D00000}, // NOVA
  {0x00B00000, 0x00C00000, 0x00A00000, 0x00B00000}, // XVDI
  {0x00D00000, 0x00E00000, 0x00C00000, 0x00D00000}, // NVDI
  {0x00D00000, 0x00E00000, 0x00C00000, 0x00D00000}  // FVDI
};

ET4KADDRESSES_s *et4k_addr_ptr;

rtg_s rtg;
static inline bool et4k_enabled(void)
{
  return emulator_config_et4k_enabled();
}

static inline bool display_enabled(void)
{
  return emulator_config_display_enabled();
}

static inline int et4k_driver(void)
{
  return emulator_config_graphics_driver();
}

static inline int in_et4k_vram(uint32_t a)
{
  uint32_t base = et4kaddresses[et4k_driver()].vram_base;
  uint32_t top = et4kaddresses[et4k_driver()].vram_top;
  return a >= base && a < top;
}

#ifdef __cplusplus
extern "C"
{
#endif

  extern void *render_frame(void *);
  extern void pistorm_cpu_irqwatch_dump(uint32_t raw6, uint32_t latched6, uint32_t raw4, uint32_t latched4);
  extern void jit_request_cpu_exit(void);

#ifdef __cplusplus
}
#endif

/* Emulator setup */
unsigned int cpu_type;

extern uint8_t fc;
volatile uint8_t g_irq = 0;
volatile uint8_t g_ipl = 0;
uint32_t tt_ram_size = 0;

volatile uint8_t g_irq_mask;
extern volatile uint8_t g_buserr;
#ifdef __cplusplus
extern "C"
#endif
void pistorm_stram_memcfg_snoop(unsigned int a, unsigned int v, int size);

static inline void cpu_data_fc(void)
{
  fc = 5; /* supervisor data */
}

static inline bool blitter_disabled_addr(uint32_t address)
{
  if ((address & 0xFF000000u) == 0xFF000000u)
    address &= 0x00FFFFFFu;
  address &= 0x00FFFFFFu;
  return !emulator_config_blitter_enabled() && address >= 0x00FF8A00u && address < 0x00FF8C00u;
}

static uint32_t mfp_eoi8_writes;
static uint32_t mfp_eoi16_writes;
static uint32_t mfp_eoi_last_addr;
static uint32_t mfp_eoi_last_value;
static uint8_t mfp_eoi_last_fc;
static uint8_t mfp_write_shadow[0x30];
static uint64_t mfp_write_valid;
static uint32_t mfp_write_total;
struct mfp_write_log_s {
  uint32_t addr;
  uint32_t value;
  uint8_t word;
  uint8_t fc;
};
static mfp_write_log_s mfp_write_log[8];
static uint8_t mfp_write_log_pos;
extern "C" volatile uint32_t pistorm_mfp_iack_counts[16];
extern "C" volatile uint8_t pistorm_mfp_last_iack_vector;

#define MFP_WRITE_TRACKING 0
#define MFP_DIAG_BUS_SNAPSHOT 0
#define MFP_DIAG_MISSING_SUMMARY 0
#define ATARI_IRQ_RAW_DIAG 0
#define ATARI_IRQ_MISSING_DIAG 0
#define PISTORM_SERIAL_IRQ 0

static inline bool mfp_eoi_addr(uint32_t addr)
{
  uint32_t folded = addr & 0x00FFFFFFu;
  return folded == 0x00FFFA0Fu || folded == 0x00FFFA11u;
}

static inline bool mfp_reg_addr(uint32_t addr)
{
  uint32_t folded = addr & 0x00FFFFFFu;
  return folded >= 0x00FFFA00u && folded < 0x00FFFA30u;
}

static inline void mfp_shadow_byte(uint32_t addr, uint8_t value)
{
  uint32_t folded = addr & 0x00FFFFFFu;
  if (folded >= 0x00FFFA00u && folded < 0x00FFFA30u)
  {
    uint32_t index = folded - 0x00FFFA00u;
    mfp_write_shadow[index] = value;
    mfp_write_valid |= (1ULL << index);
  }
}

extern "C" void mfp_note_write(uint32_t addr, uint32_t value, bool word)
{
#if !MFP_WRITE_TRACKING
  (void)addr;
  (void)value;
  (void)word;
  return;
#endif

  if (!mfp_reg_addr(addr))
    return;

  uint32_t folded = addr & 0x00FFFFFFu;
  mfp_write_log[mfp_write_log_pos].addr = folded;
  mfp_write_log[mfp_write_log_pos].value = word ? (value & 0xFFFFu) : (value & 0xFFu);
  mfp_write_log[mfp_write_log_pos].word = word ? 1 : 0;
  mfp_write_log[mfp_write_log_pos].fc = fc;
  mfp_write_log_pos = (uint8_t)((mfp_write_log_pos + 1) & 7);
  mfp_write_total++;

  if (word)
  {
    mfp_shadow_byte(folded, (uint8_t)(value >> 8));
    mfp_shadow_byte(folded + 1, (uint8_t)value);
  }
  else
  {
    mfp_shadow_byte(folded, (uint8_t)value);
  }

  if (mfp_eoi_addr(addr))
  {
    if (word)
      mfp_eoi16_writes++;
    else
      mfp_eoi8_writes++;

    mfp_eoi_last_addr = folded;
    mfp_eoi_last_value = word ? (value & 0xFFFFu) : (value & 0xFFu);
    mfp_eoi_last_fc = fc;
  }

}

extern "C" void mfp_note_eoi_write(uint32_t addr, uint32_t value, bool word)
{
  mfp_note_write(addr, value, word);
}

#if MYWTC

/* --- Configuration & Addresses --- */
#define ADDR_DMA_DATA 0x00FF8604   // DMA Sector Count / Data
#define ADDR_DMA_STATUS 0x00FF8606 // DMA Status (Read) / Mode (Write)
#define ADDR_DMA_PTR_HI 0x00FF8608
#define ADDR_DMA_PTR_MID 0x00FF860A
#define ADDR_DMA_PTR_LO 0x00FF860C
#define ADDR_MFP_GPIP 0x00FFFA01 // MFP General Purpose I/O
#define ADDR_MFP_IPRA 0x00FFFA07 // Interrupt Pending Register A
#define ADDR_MFP_IERA 0x00FFFA09 // Interrupt Enable Register A

/* --- Global State --- */
uint8_t *st_ram_cache = NULL;

#endif

extern uae_u8 *natmem_offset;


void wait_ns(uint64_t nanoseconds)
{
  /* Busy-wait on the ARM generic timer (CNTVCT_EL0) instead of clock_gettime.
   * This runs in the ipl_task poll loop (and bus timing) millions of times/sec;
   * clock_gettime is a ~40ns vdso call and was showing as ~40% of the whole
   * profile. CNTVCT_EL0 is a single mrs (~1ns), no vdso, no memory access. */
  static uint64_t freq = 0;                 /* CNTFRQ_EL0, read once */
  if (!freq)
  {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq)
      freq = 54000000ULL;                   /* Pi 4 arch-timer fallback */
  }

  uint64_t start, now;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
  const uint64_t target = start + (nanoseconds * freq) / 1000000000ULL;

  do
  {
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    asm volatile("yield" ::: "memory");
  } while (now < target);
}

/*
 * Atari ST Interrupt handling task
 * Offload the detection to this task to optimise the cpu_task ()
 */
// Helper to get current time in microseconds
uint64_t get_time_us()
{
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

static uint32_t mfp_diag_buserr_mask;

static uint8_t mfp_diag_read8(uint32_t addr, unsigned bit)
{
  uint8_t old_fc = fc;
  uint8_t value;

  fc = 5; /* supervisor data */
  g_buserr = 0;
  value = ps_read_8(addr);
  if (g_buserr)
    mfp_diag_buserr_mask |= (1u << bit);
  g_buserr = 0;
  fc = old_fc;
  return value;
}

static void mfp_diag_dump(const char *why)
{
#if !MFP_DIAG_BUS_SNAPSHOT && !MFP_DIAG_MISSING_SUMMARY
  (void)why;
  return;
#endif
#if MFP_DIAG_BUS_SNAPSHOT
  mfp_diag_buserr_mask = 0;
  uint8_t gpip  = mfp_diag_read8(0x00FFFA01, 0);
  uint8_t aer   = mfp_diag_read8(0x00FFFA03, 1);
  uint8_t ddr   = mfp_diag_read8(0x00FFFA05, 2);
  uint8_t iera  = mfp_diag_read8(0x00FFFA07, 3);
  uint8_t ierb  = mfp_diag_read8(0x00FFFA09, 4);
  uint8_t ipra  = mfp_diag_read8(0x00FFFA0B, 5);
  uint8_t iprb  = mfp_diag_read8(0x00FFFA0D, 6);
  uint8_t isra  = mfp_diag_read8(0x00FFFA0F, 7);
  uint8_t isrb  = mfp_diag_read8(0x00FFFA11, 8);
  uint8_t imra  = mfp_diag_read8(0x00FFFA13, 9);
  uint8_t imrb  = mfp_diag_read8(0x00FFFA15, 10);
  uint8_t vr    = mfp_diag_read8(0x00FFFA17, 11);
  uint8_t tcdcr = mfp_diag_read8(0x00FFFA1D, 12);
  uint8_t tcdr  = mfp_diag_read8(0x00FFFA23, 13);
  uint8_t tddr  = mfp_diag_read8(0x00FFFA25, 14);

  fprintf(stderr,
          "[MFPDUMP] %s berr=%04X GPIP=%02X AER=%02X DDR=%02X IER=%02X/%02X IPR=%02X/%02X ISR=%02X/%02X IMR=%02X/%02X VR=%02X TCDCR=%02X TDRC/D=%02X/%02X EOI8=%u last=%06X:%04X fc=%u IACKlast=%02X IACK45=%u IACK46=%u\n",
          why, mfp_diag_buserr_mask, gpip, aer, ddr, iera, ierb, ipra, iprb, isra, isrb, imra, imrb,
          vr, tcdcr, tcdr, tddr,
          mfp_eoi8_writes, mfp_eoi_last_addr,
          mfp_eoi_last_value, mfp_eoi_last_fc, pistorm_mfp_last_iack_vector,
          pistorm_mfp_iack_counts[5], pistorm_mfp_iack_counts[6]);
#else
#define MFP_SHADOW(off) ((mfp_write_valid & (1ULL << (off))) ? mfp_write_shadow[(off)] : 0xFF)
  fprintf(stderr,
          "[MFPMISS] %s writes=%u IER=%02X/%02X IPR=%02X/%02X ISR=%02X/%02X IMR=%02X/%02X EOI8=%u last=%06X:%04X fc=%u IACKlast=%02X IACK45=%u IACK46=%u\n",
          why, mfp_write_total,
          MFP_SHADOW(0x07), MFP_SHADOW(0x09),
          MFP_SHADOW(0x0B), MFP_SHADOW(0x0D),
          MFP_SHADOW(0x0F), MFP_SHADOW(0x11),
          MFP_SHADOW(0x13), MFP_SHADOW(0x15),
          mfp_eoi8_writes,
          mfp_eoi_last_addr, mfp_eoi_last_value, mfp_eoi_last_fc,
          pistorm_mfp_last_iack_vector,
          pistorm_mfp_iack_counts[5], pistorm_mfp_iack_counts[6]);
#undef MFP_SHADOW
#endif
}

//#define ATARI_IRQ_RATE_PROFILE
/* PISTORM_IPL_STATS=1 helper: report interrupt EPISODE and DELIVERY rates
 * once per second, from a thread that is free to syscall.
 *
 * ep*  = how often the Atari's IPL line ENTERED that level
 * del* = how many interrupts we then PRESENTED to the guest
 *
 * On a 50 Hz RGB screen ep4 should read ~50 (60 Hz: ~60, mono: ~71.4).
 * Reading the two together says which half is wrong:
 *   ep4 ~= del4 ~= 50  - interrupts are fine, look elsewhere
 *   ep4 ~= 50, del4 higher - redelivery inside one blanking interval
 *   ep4 itself too high - the line is seen at level 4 more often than
 *                         the video hardware can possibly produce it */
static void *ipl_stats_task(void *)
{
  unsigned p2 = 0, p4 = 0, p6 = 0, q2 = 0, q4 = 0, q6 = 0;

  for (;;)
  {
    struct timespec ts = { 1, 0 };
    unsigned e2, e4, e6, d2, d4, d6;

    nanosleep(&ts, NULL);

    e2 = pistorm_ipl_ep2;  e4 = pistorm_ipl_ep4;  e6 = pistorm_ipl_ep6;
    d2 = pistorm_ipl_lat2; d4 = pistorm_ipl_lat4; d6 = pistorm_ipl_lat6;

    fprintf(stderr,
            "[ipl] ep2=%u ep4=%u ep6=%u | del2=%u del4=%u del6=%u  (per second)\n",
            e2 - p2, e4 - p4, e6 - p6, d2 - q2, d4 - q4, d6 - q6);

    p2 = e2; p4 = e4; p6 = e6;
    q2 = d2; q4 = d4; q6 = d6;
  }
  return NULL;
}

static void *ipl_task(void *)
{
  cpu_set_t cpuset;
  pthread_t thread;
  uint16_t status;
  uint8_t ipl;
  uint64_t irq_diag_next = 0;
  uint64_t irq_watch_next = 0;
  uint32_t raw_ipl0 = 0, raw_ipl2 = 0, raw_ipl4 = 0, raw_ipl6 = 0, raw_other = 0;
  uint32_t latched_ipl2 = 0, latched_ipl4 = 0, latched_ipl6 = 0;
  uint8_t ipl_cand = 0;                 /* synchronizer candidate level  */
  uint64_t ipl_cand_tick = 0;           /* arch-timer stamp of candidate */
  const char *ipl_raw_env = getenv("PISTORM_IPL_RAW");
  const int ipl_raw = (ipl_raw_env && *ipl_raw_env == '1');
  uint64_t ipl_confirm_ticks;           /* persistence window, arch ticks */
  uint8_t  av_held = 0;                 /* autovector level already latched
                                           this assertion episode (2/4)   */
  uint64_t av4_last_tick = 0;           /* last level-4 latch (refractory) */
  uint64_t av4_refract_ticks;
  {
    const char *e = getenv("PISTORM_IPL_CONFIRM_NS");
    const char *r = getenv("PISTORM_VBL_REFRACT_NS");
    uint64_t ns  = e ? strtoull(e, NULL, 10) : 2000;      /* default 2us */
    uint64_t rns = r ? strtoull(r, NULL, 10) : 5000000;   /* default 5ms */
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f)
      f = 54000000ULL;                  /* Pi 4 arch-timer fallback */
    ipl_confirm_ticks = ns * f / 1000000000ULL;
    av4_refract_ticks = rns * f / 1000000000ULL;
  }
  bool seen_ipl6 = false;
  unsigned no_ipl6_seconds = 0;
#ifdef ATARI_IRQ_RATE_PROFILE
  uint64_t irq_profile_next = 0;
  uint32_t irq_profile_ipl2 = 0;
  uint32_t irq_profile_ipl4 = 0;
  uint32_t irq_profile_ipl6 = 0;
#endif

  /* anchor this task to cpu3 */
  CPU_ZERO (&cpuset);
  CPU_SET (3, &cpuset);
  thread = pthread_self();
  pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);

  /* Keep the sampler under the normal scheduler. Running it as SCHED_FIFO at
   * priority 99 can starve the CPU/render paths when ET4000 is active. */
 

  while (!cpu_emulation_running);

  usleep(1000000);

  /* FDD 50 Hz "VBL" tick pacing, folded in from the old fdd_vbl_thread:
   * this loop owns core 3 outright and mostly burns it in wait_ns, so the
   * spare time services the tick for free. Paced on the arch timer
   * (CNTVCT_EL0, same clock as wait_ns) - no syscalls on the isolated core. */
  uint64_t fdd_freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(fdd_freq));
  if (!fdd_freq)
    fdd_freq = 54000000ULL;                 /* Pi 4 arch-timer fallback */
  const uint64_t fdd_step = fdd_freq / 50;  /* 20 ms */
  uint64_t fdd_next = 0;                    /* 0 = tick on first pass */

  while (cpu_emulation_running)
  {
    /* Housekeeping slot, checked before the bus-busy branch so a busy
     * stretch cannot stall it. fdd_vbl() is pure memory work (motor
     * timeout counters - no syscalls, no locks, tens of ns) and its timing
     * needs are loose, so the IPL sample below is never meaningfully
     * delayed. Admission rule for anything else added here: no syscalls,
     * no locks, bounded sub-microsecond work only. */
    if (FDD_enabled)
    {
      uint64_t hk_now;
      __asm__ volatile("mrs %0, cntvct_el0" : "=r"(hk_now));
      if (hk_now >= fdd_next)
      {
        fdd_vbl();
        fdd_next = hk_now + fdd_step;
      }
    }

    /* _MCH cookie watcher: independent of FDD, stride-gated (~every
     * 32768 passes = roughly every half-second). Bounded memory work. */
    {
      static unsigned mch_stride;
      if (!(++mch_stride & 0x7FFFu))
        machine_cookie_tick();
    }

    /* MFP in-service orphan watchdog. A garbled IACK vector read can
     * leave the real chip with a channel's in-service bit set that no
     * guest handler will ever EOI - that channel and everything below
     * it then go silent forever. Field cases, both caught in [STOP-MFP]
     * register dumps: Timer A (isra=20) killing Paula's replay, and
     * Timer C (isrb=20) killing hz200 so the whole machine crawls. The
     * BADMFPACK->ISRDELIVER fix covers the path we identified; this
     * covers whichever rare siblings remain. Signature that no healthy
     * machine shows: in-service AND same-channel pending, persisting
     * across two ~1s samples (real handlers hold in-service for
     * microseconds). Recovery: host-side EOI (write a 0 to the bit;
     * 1s leave other channels untouched), logged loudly. */
    {
      static unsigned wd_stride;
      static uint16_t wd_prev;
      if (!(++wd_stride & 0xFFFFu))          /* ~every second at 15us/pass */
      {
        uint16_t isr = ((uint16_t)ps_read_8(0x00FFFA0Fu) << 8) |
                        ps_read_8(0x00FFFA11u);
        uint16_t ipr = ((uint16_t)ps_read_8(0x00FFFA0Bu) << 8) |
                        ps_read_8(0x00FFFA0Du);
        uint16_t orph = isr & ipr;           /* in-service + re-pending  */

        /* Sanity gate. A genuine orphaned-IACK wedge is ONE channel (the
         * field cases were single Timer A / Timer C). 0xFFFF - or any byte
         * reading all-ones - is a floating/bus-errored read, not a real MFP
         * state: the CPU services one interrupt at a time, so "every channel
         * in-service AND pending at once" cannot happen. Acting on it fired
         * 16 host-side EOIs into the real MFP on an already-erroring bus.
         * Reject any implausible sample (all-ones bytes, or more than one
         * orphaned channel) and drop the persistence chain so a bad read
         * cannot combine with the next sample to trigger a recovery. */
        int orph_n = __builtin_popcount((unsigned)orph);
        if (isr == 0xFFFFu || ipr == 0xFFFFu ||
            (isr & 0x00FFu) == 0x00FFu || (isr & 0xFF00u) == 0xFF00u ||
            orph_n > 1)
        {
          wd_prev = 0;                       /* invalid: distrust, don't act */
        }
        else
        {
          uint16_t fire = orph & wd_prev;    /* same channel, two samples    */
          wd_prev = orph;
          if (fire)
          {
            int ch;
            for (ch = 15; ch >= 0; ch--)
            {
              if (fire & (1u << ch))
              {
                uint32_t reg = (ch >= 8) ? 0x00FFFA0Fu : 0x00FFFA11u;
                ps_write_8(reg, (uint8_t)~(1u << (ch & 7)));
                fprintf(stderr, "[MFP-WD] orphaned in-service ch%d cleared "
                        "(isr=%04X ipr=%04X) - garbled-IACK wedge recovered\n",
                        ch, isr, ipr);
              }
            }
            wd_prev = 0;
          }
        }
      }
    }

    /* STBOX sandbox micro-slice: the second, Musashi-emulated ST. Admission
     * rule compliant by construction - stbox_slice() is memory-only (the
     * whole sandbox machine lives in process memory; file I/O, DRM and
     * input feeding happen in stbox_host.c on the normal cores) and runs
     * at most one 64-guest-cycle burst per call (~200-400 ns host), only
     * when the 8 MHz pace owes one. stbox_core_armed() is a plain load,
     * false whenever no box is running. */
    if (stbox_core_armed_flag)
    {
      uint64_t sb_now;
      __asm__ volatile("mrs %0, cntvct_el0" : "=r"(sb_now));
      stbox_slice(sb_now);
    }

    /* Read IPL lines only when no thread owns the bus: while ps_bus_active
     * the Pi may be DRIVING the GPIO bank (address/data phases), and
     * GPLEV0 reads back the driven values - the IPL field then carries
     * data pattern bits, not the CPLD's IPL lines. Field-measured: ~13
     * phantom level-4/s + ~90 phantom level-2/s riding on IDE streaming
     * traffic, pacing Bad Apple's VBL-gated player ~12% fast. The sample
     * is bracketed (flag checked before AND after the read) so a
     * transaction starting mid-read also discards it; the persistence
     * filter below covers the residual race. */
    if (ps_bus_active)
    {
      asm volatile("yield" ::: "memory");
      continue;
    }
    status = *ioread;
    //if (ps_bus_active)
    //  continue;                       /* transaction raced the sample */
    if (status & 0x01)
    {
      // A very short sleep here is fine as it's just waiting for a hardware cycle finish
      asm volatile("yield" ::: "memory");
      wait_ns (250);
      continue;
    }

    /*
     * gpio 5 & 6 = ipl 1 & 2
     * Assumes CPLD has already inverted active-low to active-high binary values
     * ipl results in: 0, 2, 4, or 6 (ipl = xx0)
    */
    ipl = (status & 0x60) >> 4;

    /* IPL synchronizer: honour a CHANGED level only after it has held
     * stable for a persistence window (default 2us, PISTORM_IPL_CONFIRM_NS).
     * The ST drives IPL from two chips (GLUE: HBL/VBL levels 2/4, MFP:
     * level 6) and the lines do not switch atomically - transitions of a
     * level-6 edge pass through the phantom codes %100 (4) and %010 (2).
     * Field-measured with the i2/i4/i6 counters: ~75 level-4/s and ~150
     * level-2/s latched on a 60 Hz screen riding on ~350 level-6/s - and
     * Bad Apple, whose player is VBL-paced, ran ~12% fast on the phantom
     * VBLs (stg=37 vs ev/s=31, intermittent per boot).
     * Two consecutive samples agreeing (the 68000's own rule) did NOT
     * filter them: *ioread is the CPLD's PRESENTED status, and a phantom
     * the CPLD latched at its own sample moment is held until its next
     * refresh - back-to-back host reads agree on the same held phantom.
     * Stability across TIME is required instead. Real sources lose
     * nothing: VBL/HBL/MFP are level-sensitive and hold until serviced,
     * so a 2us confirm only kills transients.
     * PISTORM_IPL_RAW=1 restores single-sample latching (A/B probe);
     * PISTORM_IPL_CONFIRM_NS=0 leaves a two-sample minimum. */
    if (!ipl_raw)
    {
      if (ipl != ipl_cand)
      {
        ipl_cand = ipl;                 /* new candidate: stamp and wait */
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ipl_cand_tick));
        continue;
      }
      if (ipl_confirm_ticks && ipl != g_ipl)
      {
        uint64_t nowt;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(nowt));
        if (nowt - ipl_cand_tick < ipl_confirm_ticks)
          continue;                     /* not stable long enough yet */
      }
    }

    /* Only write g_ipl when it actually changes. This poller spins on cpu3 at
     * millions of iterations/sec; writing g_ipl every pass dirtied the cache
     * line it shares with g_irq/g_irq_mask, which the CPU thread reads in
     * intlev()/get_ipl() - false sharing that taxed guest execution on every
     * interrupt check. */
    if (g_ipl != ipl)
    {
      /* Count the ENTRY into each level - one per assertion episode,
       * whatever we later decide to deliver. Cheap: this branch is
       * already the rare one (see the false-sharing note above). */
      if (ipl == 2)      pistorm_ipl_ep2++;
      else if (ipl == 4) pistorm_ipl_ep4++;
      else if (ipl == 6) pistorm_ipl_ep6++;
      g_ipl = ipl;
    }

    /* Autovector edge semantics: one latch per ASSERTION EPISODE for the
     * GLUE-driven levels 2/4. On real hardware the CPU's IACK (a 6800-
     * style VPA/VMA handshake for autovectors) clears the GLUE's pending
     * state - one exception per VBL/HBL. Our IACK cycle evidently does
     * not complete that handshake: the line stays asserted for the whole
     * blanking interval, and every RTE inside that window met the still-
     * asserted line and delivered the SAME VBL again. Field-measured:
     * 75-84 level-4/s on a screen that can only make 50/60/71.4 - and
     * varying with guest load (extra deliveries per blank = blank time /
     * handler time), which no physical clock does. Bad Apple's VBL-gated
     * player hence ran ~12% fast, intermittently, for weeks.
     * av_held remembers the level latched this episode; it re-arms when
     * the line visibly leaves that level. The level-4 refractory (5ms,
     * PISTORM_VBL_REFRACT_NS, well under any frame period) covers the
     * 4->6->4 interleave where an MFP interrupt mid-blank briefly masks
     * the line and would otherwise re-arm the same VBL. Level 2 gets no
     * refractory: HBL's period is 64us. Level 6 (MFP, vector handshake
     * works) is untouched. */
    if (ipl != av_held)
      av_held = 0;                      /* line left the level: re-arm */
    if (ipl != 0 && ipl > g_irq && ipl > g_irq_mask && ipl != av_held)
    {
      /* HBL fast-reject: the GLUE asserts level 2 every scan line (~15 kHz).
       * If the guest's HBL autovector is a bare RTE, delivering it does
       * nothing - so skip the entire CPU-thread round-trip (block exit,
       * do_interrupt, re-entry) that do_interrupt() would only suppress
       * anyway. Latch the episode (av_held) so we re-check once per HBL line,
       * not per sample; the line self-clears after the blank, re-arming
       * av_held. A real HBL handler returns 0 here and is delivered normally. */
      if (ipl == 2)
      {
        if (pistorm_hbl_handler_is_rte())
        {
          av_held = 2;
          pistorm_ipl_hbl_skipped++;
          continue;
        }
      }
      if (ipl == 4 && av4_refract_ticks)
      {
        uint64_t nowt;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(nowt));
        if (nowt - av4_last_tick < av4_refract_ticks)
          continue;                     /* same VBL blanking interval */
        av4_last_tick = nowt;
      }
      if (ipl == 2 || ipl == 4)
        av_held = ipl;                  /* one latch per assertion */
#ifdef ATARI_LAT_DIAG
      /* Latency instrumentation: stamp the moment a level becomes pending;
       * intlev() measures the latch->delivery delta. */
      extern volatile uint64_t g_irq_latch_us;
      if (g_irq == 0)
        g_irq_latch_us = get_time_us();
#endif
      g_irq = ipl;
      if (ipl == 2)      pistorm_ipl_lat2++;
      else if (ipl == 4) pistorm_ipl_lat4++;
      else               pistorm_ipl_lat6++;
      jit_request_cpu_exit();
    }

    /* Virtual MFP channels (mfp_hub): keyboard injection (GPIP4 ch6),
     * dmasnd Timer A (ch13) / GPIP7 (ch15). The real MFP cannot raise
     * any of these - synthesise the level-6 here; intlev_ack() gets the
     * vector from mfp_hub_iack() without a bus IACK cycle. dmasnd_pump()
     * advances the host-side frame clock (it feeds hub events); it must
     * run on THIS thread only - the frame clock has a single-writer
     * t0+=dur sequence (see dmasnd_capture.c field lesson). */
    if (DMA_Sound_enabled)
      dmasnd_pump();
    if ((KBD_USB_enabled || DMA_Sound_enabled) &&
        mfp_hub_irq_wanted() && 6 > g_irq && 6 > g_irq_mask)
    {
      g_irq = 6;
      jit_request_cpu_exit();
    }

#ifdef ATARI_LAT_DIAG
    /* Stall watchdog (measurement only): while a latched interrupt is
     * waiting on delivery, sample the CPU thread's state from this thread. */
    {
      extern volatile uint64_t g_irq_latch_us;
      extern void jit_stall_probe(uint32_t age_us);
      const uint64_t t_latch = g_irq_latch_us;
      if (t_latch)
      {
        const uint64_t age = get_time_us() - t_latch;
        if (age > 2500)
          jit_stall_probe((uint32_t)age);
      }
    }
#endif /* ATARI_LAT_DIAG */
    /*
     * IPL pulses are short enough that scheduler sleep jitter can miss them,
     * especially under MiNT/ET4000 load. Keep this as a short busy wait on the
     * dedicated IPL core; the CPU side still owns the actual acknowledge/frame.
     */
    wait_ns (15000);

  }

  return (void*) NULL;
}

struct termios oldt, newt;
int oldf;
extern void logo(void);

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#if defined(__linux__)
#include <ucontext.h>
#endif

#define PISTORM_NATMEM_LIMIT ((uintptr_t)0x09000000u)

static void crash_dump_host_context(void *uctx)
{
#if defined(__linux__) && defined(__aarch64__)
  ucontext_t *uc = (ucontext_t *)uctx;
  fprintf(stderr, "       pc=%016llX lr=%016llX sp=%016llX\n",
          (unsigned long long)uc->uc_mcontext.pc,
          (unsigned long long)uc->uc_mcontext.regs[30],
          (unsigned long long)uc->uc_mcontext.sp);
  fprintf(stderr,
          "       x0=%016llX x1=%016llX x2=%016llX x3=%016llX x4=%016llX x5=%016llX x6=%016llX x7=%016llX\n",
          (unsigned long long)uc->uc_mcontext.regs[0],
          (unsigned long long)uc->uc_mcontext.regs[1],
          (unsigned long long)uc->uc_mcontext.regs[2],
          (unsigned long long)uc->uc_mcontext.regs[3],
          (unsigned long long)uc->uc_mcontext.regs[4],
          (unsigned long long)uc->uc_mcontext.regs[5],
          (unsigned long long)uc->uc_mcontext.regs[6],
          (unsigned long long)uc->uc_mcontext.regs[7]);
#elif defined(__linux__) && defined(__x86_64__)
  ucontext_t *uc = (ucontext_t *)uctx;
  fprintf(stderr, "       rip=%016llX rsp=%016llX\n",
          (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
          (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
#else
  (void)uctx;
#endif
}

static void crash_dump_symbol(const char *label, uintptr_t addr)
{
  Dl_info info;
  memset(&info, 0, sizeof info);
  if (addr && dladdr((void *)addr, &info) && info.dli_sname)
  {
    fprintf(stderr, "[SYM] %s=%p %s+0x%llx (%s)\n",
            label, (void *)addr, info.dli_sname,
            (unsigned long long)(addr - (uintptr_t)info.dli_saddr),
            info.dli_fname ? info.dli_fname : "?");
  }
}

static void crash_dump_mapping(const char *label, uintptr_t addr)
{
#if defined(__linux__)
  FILE *f = fopen("/proc/self/maps", "r");
  char line[512];

  if (!addr)
    return;
  if (!f)
  {
    fprintf(stderr, "[MAP] %s=%p maps unavailable\n", label, (void *)addr);
    return;
  }

  while (fgets(line, sizeof line, f))
  {
    unsigned long long lo, hi;
    char perms[8];
    if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 &&
        addr >= (uintptr_t)lo && addr < (uintptr_t)hi)
    {
      size_t len = strlen(line);
      if (len && line[len - 1] == '\n')
        line[len - 1] = 0;
      fprintf(stderr, "[MAP] %s=%p in %s +0x%llx\n",
              label, (void *)addr, line, (unsigned long long)(addr - (uintptr_t)lo));
      fclose(f);
      return;
    }
  }

  fclose(f);
  fprintf(stderr, "[MAP] %s=%p <unmapped>\n", label, (void *)addr);
#else
  (void)label;
  (void)addr;
#endif
}

#ifdef __cplusplus
extern "C" void pistorm_crash_dump_guest(void);
#else
extern void pistorm_crash_dump_guest(void);
#endif

/* WILD-PC GUARD support (see m68k_run_jit in cpu/newcpu.cpp).
 *
 * Which guest addresses may INSTRUCTIONS be fetched from? Data accesses
 * to bad addresses already bus-error through the banks, but natmem is
 * one flat mapping, so a wild PC resolves to a readable host pointer
 * and the JIT happily dispatches it - Xenon 2 under FreeMiNT rode an
 * RTS to odd $CCC9D3 (the 4-16MB hole) into a NULL block handler and
 * killed the HOST at pc=0. Permissive by design: the full 4MB ST-RAM
 * window is allowed even if less is fitted - the goal is keeping the
 * host alive, not perfect decode. */
/* Machine MMU flavour for the ST-RAM alias model (pistorm_natmem.cpp):
 * plain ST folds small-chip-in-big-config by dropping col/row MSBs
 * (A10/A20 in 2M mode); STE and later fold modulo. Driven by the cfg
 * "machine" key; no key = plain ST, matching the _MCH default. */
#ifdef __cplusplus
extern "C"
#endif
int emulator_machine_is_ste(void)
{
  uint32_t mch;
  if (!emulator_config_machine_set(&mch))
    return 0;                      /* no machine key: plain ST default */
  return mch >= 0x00010000u;       /* STE, MegaSTE, TT, Falcon         */
}

#ifdef __cplusplus
extern "C"
#endif
int pistorm_pc_executable(unsigned int pc)
{
  /* 24-bit machine: address bits 24-31 do not exist on a 68000's bus.
   * Mac software (via Spectre) tags pointers there and jumps through
   * them; judging the untruncated value declared perfectly good PCs
   * unmapped. Mask first, judge after. */
  {
    extern int pistorm_addr24;                          /* jit_glue.cpp  */
    if (pistorm_addr24)
      pc &= 0x00FFFFFFu;
  }
  if (pc < 0x400000u)                                   /* ST-RAM window */
    return 1;
  if (tt_ram_available &&
      pc >= 0x01000000u && pc - 0x01000000u < tt_ram_size)
    return 1;                                           /* TT/alt-RAM    */
  if (pc >= 0xE00000u && pc < 0xF00000u)                /* TOS 2.x ROM   */
    return 1;
  if (pc >= 0xFA0000u && pc < 0xFF0000u)                /* cart + TOS 1.x */
    return 1;
  return 0;
}

static void crash_handler(int sig, siginfo_t *si, void *uctx)
{
  extern void *pushall_call_handler;
  extern unsigned char *compiled_code;
  extern unsigned char *current_compile_p;
  extern unsigned char *popallspace;
  extern const int POPALLSPACE_SIZE;
  uintptr_t host = (uintptr_t)si->si_addr;
  uintptr_t base = (uintptr_t)natmem_offset;
  uintptr_t guest = host - base;
  uintptr_t pc = 0, lr = 0, sp = 0;
  uintptr_t et4k_vram = (uintptr_t)et4000_engine_vram_ptr();

  fprintf(stderr, "[%s] host=%p natmem=%p",
          sig == SIGILL ? "SIGILL" :
          sig == SIGBUS ? "SIGBUS" :
          sig == SIGFPE ? "SIGFPE" :
          sig == SIGABRT ? "SIGABRT" : "SIGSEGV",
          si->si_addr, (void *)natmem_offset);
  if (natmem_offset && host >= base && guest < PISTORM_NATMEM_LIMIT)
    fprintf(stderr, " guest_addr=0x%08lX\n", (unsigned long)guest);
  else
    fprintf(stderr, " guest_addr=<outside-natmem>\n");

  crash_dump_host_context(uctx);

#if defined(__linux__) && defined(__aarch64__)
  {
    ucontext_t *uc = (ucontext_t *)uctx;
    pc = (uintptr_t)uc->uc_mcontext.pc;
    lr = (uintptr_t)uc->uc_mcontext.regs[30];
    sp = (uintptr_t)uc->uc_mcontext.sp;
  }
#elif defined(__linux__) && defined(__x86_64__)
  {
    ucontext_t *uc = (ucontext_t *)uctx;
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
  }
#endif

  crash_dump_mapping("fault", host);
  crash_dump_mapping("pc", pc);
  crash_dump_mapping("lr", lr);
  crash_dump_mapping("sp", sp);
  crash_dump_mapping("natmem", base);
  crash_dump_mapping("et4k_vram", et4k_vram);
  crash_dump_symbol("pc", pc);
  crash_dump_symbol("lr", lr);
  if (lr >= 16)
  {
    const uint32_t *lr_code = (const uint32_t *)(lr - 16);
    fprintf(stderr, "[LRCODE] %08X %08X %08X %08X\n",
            lr_code[0], lr_code[1], lr_code[2], lr_code[3]);
  }
  if (popallspace || compiled_code)
  {
    uintptr_t popall_start = (uintptr_t)popallspace;
    uintptr_t popall_end = popall_start + (uintptr_t)POPALLSPACE_SIZE;
    uintptr_t jit_start = (uintptr_t)compiled_code;
    uintptr_t jit_end = (uintptr_t)current_compile_p;

    fprintf(stderr,
            "[JITRANGE] popall=%p-%p compiled=%p-%p pushall=%p pc_in_popall=%d pc_in_compiled=%d fault_in_compiled=%d\n",
            (void *)popall_start, (void *)popall_end,
            (void *)jit_start, (void *)jit_end,
            pushall_call_handler,
            popall_start && pc >= popall_start && pc < popall_end,
            jit_start && pc >= jit_start && pc < jit_end,
            jit_start && host >= jit_start && host < jit_end);
  }
  if (et4k_vram)
  {
    fprintf(stderr, "[DELTA] fault-et4k_vram=%lld pc-et4k_vram=%lld\n",
            (long long)(host - et4k_vram),
            (long long)(pc - et4k_vram));
  }

  if (sig == SIGILL)
  {
    uint32_t insn = si->si_addr ? *(volatile uint32_t *)si->si_addr : 0;
    fprintf(stderr, "       insn=0x%08x delta_from_pushall=+0x%lx\n",
            insn, (unsigned long)((char *)si->si_addr - (char *)pushall_call_handler));
  }

  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGFPE)
  {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "[%s] si_addr=%p si_code=%d backtrace=%d\n",
            sig == SIGBUS ? "BUS" : sig == SIGFPE ? "FPE" :
            sig == SIGABRT ? "ABRT" : "SEGV",
            si->si_addr, si->si_code, n);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
  }

  /* guest-side post-mortem: m68k regs, vectors, sysvars, stack */
  pistorm_crash_dump_guest();

  _exit(42);
}

// call once during init (top of main, before the CPU threads start):
static void install_crash_handler(void)
{
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);

  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
}

/* PISTORM_FOLDCHK: see the creation site in main(). Reads the screen
 * base from the model's sysvar, then compares REAL DRAM rows (over the
 * bus, cache bypassed) against each other and against the model. */
void *foldchk_thread(void *arg)
{
  (void)arg;
  sleep(15);

  if (!natmem_offset) return NULL;
  uint32_t vb = ((uint32_t)natmem_offset[0x44E] << 24) |
                ((uint32_t)natmem_offset[0x44F] << 16) |
                ((uint32_t)natmem_offset[0x450] << 8)  |
                 (uint32_t)natmem_offset[0x451];
  vb &= 0x00FFFFFEu;
  if (!vb || vb >= 0x400000u) {
    fprintf(stderr, "[FOLDCHK] v_bas_ad=%08X implausible - the guest has "
            "not reached screen setup; dumping guest state to show where "
            "it is stuck:\n", vb);
    pistorm_crash_dump_guest();
    return NULL;
  }
  fprintf(stderr, "[FOLDCHK] v_bas_ad=%08X\n", vb);

  uint16_t real0[8], real4[8], model0[8];
  int berr_seen = 0;
  fc = 5;                                   /* supervisor data */
  for (int i = 0; i < 8; i++) {
    real0[i]  = ps_read_16(vb + 2*i);
    if (g_buserr) { berr_seen = 1; g_buserr = 0; }
    real4[i]  = ps_read_16(vb + 0x400 + 2*i);
    if (g_buserr) { berr_seen = 1; g_buserr = 0; }
    model0[i] = (uint16_t)((natmem_offset[vb + 2*i] << 8) |
                            natmem_offset[vb + 2*i + 1]);
  }
  /* CRITICAL: our probe reads must not leave a sticky BERR latched for
   * the CPU thread to blame on an unrelated guest instruction - the
   * first version of this diagnostic double-faulted the guest that way
   * (halt reason 2 at PC=0x70, mid-VBL). */
  g_buserr = 0;
  if (berr_seen)
    fprintf(stderr, "[FOLDCHK] NOTE: bus error during real reads - the "
            "screen base lies BEYOND the real MMU's configured banks "
            "(guest sized more RAM than the machine has)\n");
  fprintf(stderr, "[FOLDCHK] real %06X :", vb);
  for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", real0[i]);
  fprintf(stderr, "\n[FOLDCHK] real %06X :", vb + 0x400);
  for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", real4[i]);
  fprintf(stderr, "\n[FOLDCHK] model %06X:", vb);
  for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", model0[i]);
  fprintf(stderr, "\n");

  int eq_model = 1, eq_fold = 1;
  for (int i = 0; i < 8; i++) {
    if (real0[i] != model0[i]) eq_model = 0;
    if (real0[i] != real4[i])  eq_fold = 0;
  }
  /* always include the guest state: when the screen looks right but the
   * machine is stalled (compat-core bring-up), the stuck PC is the
   * evidence that matters */
  pistorm_crash_dump_guest();

  if (eq_model)
    fprintf(stderr, "[FOLDCHK] VERDICT: real DRAM matches the model - real "
            "MMU is sane; suspect the shifter base registers\n");
  else if (eq_fold)
    fprintf(stderr, "[FOLDCHK] VERDICT: real DRAM FOLDS at +0x400 (plain-ST "
            "A10 fold) - the REAL MMU is still in 2M muxing; the guest's "
            "memcfg write never reached the chip\n");
  else
    fprintf(stderr, "[FOLDCHK] VERDICT: neither clean match nor clean fold - "
            "send me these three rows\n");
  return NULL;
}

void sigint_handler(int sig_num)
{
  cpu_emulation_running = 0;

  printf("\n[MAIN] Exiting\n");

  /* JIT statistics (PROFILE_UNTRANSLATED_INSNS: top-50 interpreted opcodes) */
  compiler_dump_stats();

  /* display ATARI logo on exit (HDMI) */
  // if ( RTG_enabled )
  //   logo ();

  usleep(100000);

  /* reset stdio tty properties */
  oldt.c_lflag |= ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  _exit(0);
}


void *cpu_task(void *)
{
  cpu_set_t cpuset;
  pthread_t thread;
  struct sched_param param;

  /* anchor this task to cpu2 */
  CPU_ZERO(&cpuset);
  CPU_SET(2, &cpuset);
  thread = pthread_self();
  pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);

  /* set real-time priority for this task */
  param.sched_priority = 99; // Highest possible priority
  sched_setscheduler(0, SCHED_FIFO, &param);

  // Lock memory to prevent swapping to SD card (causes random hangs)
  mlockall(MCL_CURRENT | MCL_FUTURE);

  while (!cpu_emulation_running);

  usleep(1000000);

  /*
   * Run emulation until user intervention
   */
  while (cpu_emulation_running)
  {
    jit_cpu_execute();
  }

  printf("[CPU] End of CPU thread\n");

  return (void *)NULL;
}


int main (int argc, char *argv[])
{
  struct emulator_config *config;
  int g;
  int err;
  pthread_t rtg_tid, e4k_tid, cpu_tid, flush_tid, ipl_tid, vbl_id;
  time_t t;
  char config_file[256];

  /* Claim the bus before anything touches the GPIO (gpio/bus_lock.c).
   * Two processes driving the same pins mid-cycle is not a cosmetic
   * problem, so refuse to start if ataritest - or a second emulator -
   * already holds it. The kernel drops this lock whenever the holder
   * dies, so it can never be left stale. */
  if (pistorm_bus_lock("emulator") != 0) {
    fprintf(stderr, "[MAIN] the PiStorm bus is already in use - "
                    "stop the other process first\n");
    return 1;
  }

  {
    // extern unsigned char *natmem_offset;       /* defined in pistorm_natmem.cpp */
    extern void *pushall_call_handler; /* defined in compemu_support */
    static struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    /* SIGBUS was previously unhandled: on AArch64 a store to a mapped-but-not-
     * backed page (e.g. past the end of the natmem/TT-RAM mmap) raises SIGBUS,
     * not SIGSEGV, so such a fault killed the process silently ("Bus error",
     * no dump). Route it - plus SIGFPE/SIGABRT - through the same post-mortem.
     * Signal handlers add no bus/pipeline traffic; they run only at death. */
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    fprintf(stderr, "[DBG] crash handler installed\n"); /* sanity check */
  }

  /* assign signal handlers
   *
   * SIGHUP and SIGTERM go to the same place as SIGINT, and that matters more
   * than it looks. SIGHUP is what closing a terminal window sends, and its
   * default action is to terminate immediately - no orderly shutdown at all,
   * while the JIT is mid-block, a film's decode and present threads are
   * running and the SDL audio callback is live. SIGTERM is what systemd sends
   * to stop the service, and had the same problem.
   *
   * sigint_handler() clears cpu_emulation_running, restores the tty and
   * _exit()s: the path that is actually exercised every time anyone presses
   * Ctrl-C. Routing all three there makes "close the window" and
   * "systemctl stop" behave the same way.
   *
   * SIGPIPE is ignored rather than handled. Once the terminal has gone, a
   * write to the old stdout raises it, and the default action would kill the
   * process during the very shutdown this is trying to make orderly. */
  signal(SIGINT,  sigint_handler);
  signal(SIGHUP,  sigint_handler);
  signal(SIGTERM, sigint_handler);
  signal(SIGPIPE, SIG_IGN);

  /*
   * save stdio tty properties and ammend for emulator use
   * tty properties are restored in sigint_handler ()
   * if the emulator abnormally aborts, it is possible the tty characteristics are not restored,
   * which may result in no command line echo. Blindly enter "stty echo" to restore
   */

  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

  /* Initialise PiSTorm */
  pulse_reset_inprogress = 0;

  ps_setup_protocol();
  ps_get_firmware_revision();
  ps_pulse_halt();
  ps_reset_state_machine();
  ps_pulse_reset();

  FDD_enabled = false;
  DMA_Sound_enabled = false;
  tt_ram_available = false;
  pistorm_rom_ptr = NULL;
  pistorm_rom_start = 0;
  pistorm_rom_end = 0;
  pistorm_rom_mask = 0;
  et4k_addr_ptr = NULL;
  screenGrab = false;

  /*
   * read command line arguments to determine config file to load
   */

  strcpy (config_file, "../configs/atari.cfg"); // default file

  for (g = 1; g < argc; g++)
  {
    if (strcmp(argv[g], "--config") == 0)
    {
      if (g + 1 >= argc)
      {
        printf("%s switch found, but no config filename specified.\n", argv[g]);
      }

      else
      {
        g++;
        FILE *chk = fopen(argv[g], "rb");

        if (chk == NULL)
        {
          printf("Config file %s does not exist, please check that you've specified the path correctly.\n", argv[g]);
        }

        else
        {
          fclose(chk);
          memset(config_file, 0, sizeof(config_file));
          strcpy(config_file, argv[g]);

          break;
        }
      }
    }
  }

  /* 
   * load the config 
   */
  printf("[CFG] Loading from %s\n", config_file);
  config = load_config_file(config_file);
  if (!config)
  {
    fprintf(stderr, "[CFG] Failed to load config %s\n", config_file);
    return 1;
  }
  emulator_config_set_current(config);
  pistorm_set_blitter_mode(emulator_config_blitter_mode());

  /*
   * initialise emulator with config file parameters
   */
  if (config->cpu_type)
    cpu_type = config->cpu_type;

  /*
   * point to rom image
   */
  //for (int ix = 0; ix < cfg->rom_count; ix++)
  //{
    if (config->rom.rom_size != 0)
    {
      /* config_file.c has already derived the base from the image length:
       * TOS 1.x is 192K at 0x00FC0000, EmuTOS / TOS 2.06 are 256K or more
       * at 0x00E00000. Use it. This used to test `rom_size >= 256K` and
       * hardcode 0x00E00000 with no else, so every 192K TOS 1.x image was
       * read off disk and then never mapped: ROM_START/ROM_END stayed 0,
       * the `address >= ROM_START && address < ROM_END` tests in the read
       * paths were all false, pistorm_rom_size came out 0 in jit_mem_init,
       * and pistorm_seed_reset_vector() copied 8 zero bytes to address 0.
       * The CPU then reset to SSP=0 PC=0 and executed nothing at all. */
      ROM_START = config->rom.rom_address;
      ROM_END = ROM_START + config->rom.rom_size;
      ROM_MASK = config->rom.rom_size - 1;

      pistorm_rom_ptr = config->rom.rom_ptr;
      pistorm_rom_start = ROM_START;
      pistorm_rom_end = ROM_END;
      pistorm_rom_mask = ROM_MASK;

      /* setup ROM boot vector interception */
      //for (int n = 0; n < 8; n++ )
      //  rom_vector [n] = *pistorm_rom_ptr++;

      printf ("[INIT] ROM loaded - %dK mapped at 0x%06X-0x%06X\n",
              config->rom.rom_size / 1024,
              (unsigned) ROM_START, (unsigned) ROM_END);
    }
  //}
  else {
    printf ("[INIT] ERROR - NO ROM\n");
  }

  

  /*
   * NOTE JIT configures the memory map, so local allocation has been removed
   * configure memory for TT-RAM if > 68000
   */
  if (config->ttram)
  {
    if (cpu_type < 2)
    {
      /* 68000/68010 are 24-bit parts: TT-RAM at 0x01000000 is unreachable and
       * enabling it would switch the core to 32-bit addressing, breaking the
       * 24-bit wraparound real STs (and their software) rely on. */
      printf("[INIT] TT-RAM requested but CPU is 680%s0 - ignored (24-bit bus)\n",
             cpu_type == 0 ? "0" : "1");
    }
    else
    {
      tt_ram_available = true;
      tt_ram_size = config->ttram_size ? config->ttram_size : (128u * 1024u * 1024u);
      /* Clamp to the natmem reserve ceiling (TT_RAM_SIZE in pistorm_natmem.cpp,
       * currently 256MB). Going higher needs both that reserve raised AND the
       * FVDI framebuffer at 0x20000000 relocated - see the note there. */
      if (tt_ram_size > 256u * 1024u * 1024u)
        tt_ram_size = 256u * 1024u * 1024u;
      printf("[INIT] TT-RAM allocated - %uMB\n", tt_ram_size >> 20);
    }
  }

  /*
   * Configure emulator interfaces
   */

  if (emulator_config_stram_cache_enabled())
    printf("[INIT] ST-RAM cache enabled\n");
  if (emulator_config_stram_direct_enabled())
    printf("[INIT] ST-RAM direct enabled\n");
  if (!emulator_config_native_hdmi_enabled())
    printf("[INIT] Native ST HDMI disabled\n");

  if (display_enabled())
    screenGrab = true;

  if (et4k_enabled())
  {
    et4k_addr_ptr = &et4kaddresses[et4k_driver()];
  }

  if (config->ide)
    InitIDE();

  /* 24-bit address space decision, needed BEFORE the bank table is
   * built: map_region replicates every mapping across the 256 16MB
   * mirrors of the 32-bit space when the machine is 24-bit (a 68000
   * ignores address bits 24-31 - Mac software under Spectre tags
   * pointers there and jumps through them). jit_cpu_init() computes the
   * same value later from the same inputs; this must exist first. */
  {
    extern int pistorm_addr24;                 /* jit_glue.cpp */
    pistorm_addr24 = !(tt_ram_available || config->addr32);
  }

  /* Initialise JIT memory mapping before any emulation thread exists. The
   * bank table depends on the config-driven flags above, and cpu_task enters
   * the JIT path once cpu_emulation_running is raised below. */
  jit_mem_init();
  rtg.natmem = natmem_offset;
  // printf ("main: natmem_offset %p\n", natmem_offset);

  if (config->fdd.enabled)
  {
    FDD_enabled = true;

    platform_fdd_init (config->fdd.img_path);
    //printf ("[INIT] FDD Image Attached %s\n", cfg->fdd.img_path);
  }
  else if (acsi_enabled())
  {
    /* Emulated ACSI targets without floppy emulation: the DMA-port
     * window still routes through fdd_io_* (which passes FDC traffic to
     * the real bus), so its state must be initialised. Images were
     * attached at cfg parse time ("acsi" lines). fdd_init is declared
     * with C linkage in atari_fdd.h (included above). */
    fdd_init ();
    printf ("[INIT] Emulated ACSI target(s) active, floppy passthrough\n");
  }

  if (config->kbd_usb)
  {
    KBD_USB_enabled = true;
    kbd_usb_force_mode = config->kbd_mode;
    kbd_usb_mouse_div  = config->kbd_mouse_div > 0 ? config->kbd_mouse_div : 1;
    if (kbd_usb_init (config->kbd_grab ? 1 : 0) != 0)
      KBD_USB_enabled = false;
  }

  if (platform_network_init_from_config(config) != 0)
    fprintf(stderr, "[NET] network init failed; continuing without network backend\n");

  /* --------------------------- */

  // install_crash_handler ();

  /* --------------------------- */

  /* Audio -> HDMI. ym2149 (emulated PSG shadowing the real chip) is
   * machine-independent - every ST has a PSG. DMA sound is STE hardware,
   * and an explicit `machine` line is the AUTHORITATIVE statement of
   * machine identity: ste/megaste switch it on, st/tt/falcon switch it
   * off, either way overriding a legacy `dma_sound` entry. Rationale
   * (field case): `dma_sound` in an otherwise-ST config made the bank
   * absorb $FF89xx, so EmuTOS's boot probe found DMA sound hardware and
   * set _SND - an "ST" that had quietly grown STE sound. Without a
   * machine line the old key still works, with a hint. */
  /* THE DEFAULT MACHINE IS A PLAIN ST. STE hardware exists only when
   * the cfg says so: `machine ste` (or megaste) is the one switch for
   * everything STE - DMA sound, STE shifter personality, _MCH cookie.
   * A dma_sound line on its own enables NOTHING (loud notice below, so
   * an old config cannot fail silently). */
  bool want_dmasnd = false;
  {
    extern int emulator_config_machine_kind(void);
    want_dmasnd = (emulator_config_machine_kind() == 1);   /* STE only */
    if (config->dma_sound && !want_dmasnd)
      printf ("[CFG] dma_sound line IGNORED - STE hardware needs "
              "`machine ste` (default machine is a plain ST)\n");
    else if (want_dmasnd)
      printf ("[CFG] machine STE-class: DMA sound ON\n");
  }
  if (want_dmasnd || config->ym2149) {
    if (dmasnd_init (NULL) == 0) {
      if (want_dmasnd) {
        if (dmasnd_capture_start() == 0) {
          DMA_Sound_enabled = true;
          printf ("[INIT] DMA Sound enabled\n");
        } else {
          DMA_Sound_enabled = false;
          fprintf(stderr, "[INIT] DMA Sound failed to start\n");
        }
      } else {
        printf ("[INIT] DMA Sound disabled (machine is not STE-class)\n");
      }
      if (config->ym2149) {
        if (ym2149_init () == 0)
          printf ("[INIT] YM2149 emulation enabled\n");
        else
          fprintf(stderr, "[INIT] YM2149 emulation failed to start\n");
      } else {
        printf ("[INIT] YM2149 emulation disabled\n");
      }
    } else {
      DMA_Sound_enabled = false;
      fprintf(stderr, "[INIT] audio device failed; DMA Sound / YM2149 unavailable\n");
    }
  } else {
    printf ("[INIT] DMA Sound disabled\n");
    printf ("[INIT] YM2149 emulation disabled\n");
  }

  /* start threads */
  err = pthread_create(&cpu_tid, NULL, &cpu_task, NULL);

  if (err != 0)
    printf("[ERROR] Cannot create CPU thread: [%s]", strerror(err));

  else
  {
    pthread_setname_np(cpu_tid, "pistorm: cpu");
    printf("[MAIN] CPU thread created successfully\n");
  }
#if !PISTORM_SERIAL_IRQ
  err = pthread_create(&ipl_tid, NULL, &ipl_task, NULL);

  if (err != 0)
    printf("[ERROR] Cannot create IPL thread: [%s]", strerror(err));

  else
  {
    pthread_setname_np(ipl_tid, "pistorm: ipl");
    printf("[MAIN] IPL thread created successfully\n");
  }

  /* PISTORM_IPL_STATS=1: one line per second of interrupt rates. Runs in
   * its own thread so nothing is printed from ipl_task, which spins on
   * cpu3 and must not syscall. Off unless the env var is set. */
  {
    const char *e = getenv("PISTORM_IPL_STATS");
    if (e && *e == '1')
    {
      pthread_t stats_tid;
      if (pthread_create(&stats_tid, NULL, &ipl_stats_task, NULL) != 0)
        printf("[ERROR] Cannot create IPL stats thread\n");
      else
      {
        pthread_setname_np(stats_tid, "pistorm: iplstat");
        pthread_detach(stats_tid);
        printf("[MAIN] IPL stats enabled (1 Hz)\n");
      }
    }
  }
#else
  printf("[MAIN] IPL thread disabled; CPU path polls IPL serially\n");
#endif

  if (display_enabled())
  {
    err = pthread_create(&e4k_tid, NULL, &render_frame, NULL);

    if (err != 0)
      printf("[ERROR] Cannot create display thread: [%s]", strerror(err));

    else
    {
      pthread_setname_np (e4k_tid, "pistorm: display");
      printf("[MAIN] Display thread created successfully\n");

      while (et4000_thread_ready == 0)
        usleep(1000);
      if (et4000_thread_ready < 0)
      {
        /* No HDMI/SDL surface available (e.g. headless Pi): don't abort the
         * whole emulator — carry on without the mirror; the real Atari video
         * output is unaffected. */
        fprintf(stderr, "[WARN] Display/native-HDMI init failed; continuing without screen mirror\n");
      }
    }
  }
  
  if ( FDD_enabled )
  {
#if PISTORM_SERIAL_IRQ
    /* No IPL task in this build to host the tick: keep the VBL thread. */
    err = pthread_create ( &vbl_id, NULL, &fdd_vbl_thread, NULL );

    if ( err != 0 )
      printf ( "[ERROR] Cannot create VBL thread: [%s]", strerror (err) );

    else
    {
      pthread_setname_np ( vbl_id, "pistorm: vbl" );
      printf ( "[MAIN] VBL thread created successfully\n" );
    }
#else
    /* The 50 Hz FDD tick rides the IPL poll loop on core 3 (see ipl_task);
     * the dedicated VBL thread is gone. */
    printf ( "[MAIN] FDD VBL tick folded into IPL task (no VBL thread)\n" );
#endif
  }
//FDD_enabled = false;

  /* PISTORM_FOLDCHK=1: one-shot real-vs-model screen RAM comparison,
   * ~15s after boot (desktop idle, screen static). Decides the
   * multiple-Fuji question with data instead of theory:
   *   - real DRAM at v_bas_ad == the natmem model  -> real MMU is sane,
   *     suspect the shifter base registers instead;
   *   - real row at base == real row at base+0x400 (the plain-ST A10
   *     fold) while both differ from the model -> the REAL MMU is still
   *     in 2M muxing over smaller chips: the guest's memcfg write never
   *     reached the chip. */
  {
    extern void *foldchk_thread(void *);
    if (getenv("PISTORM_FOLDCHK")) {
      pthread_t fold_tid;
      if (pthread_create(&fold_tid, NULL, foldchk_thread, NULL) == 0)
        pthread_setname_np(fold_tid, "pistorm: foldchk");
    }
  }

  time(&t); /* get date and time */

  printf("[MAIN] Emulation Running [%s] %s\n",
         (cpu_type == 0 ? "68000" : 
          cpu_type == 1 ? "68010" : 
          cpu_type == 2 ? "68020" : 
          cpu_type == 3 ? "68030" : 
          cpu_type == 4 ? "68040" : 
          "68060"), ctime(&t));

  printf("[MAIN] Press CTRL-C to terminate\n");
  printf("\n");

  /* Host-side _MCH cookie forcing (cfg "machine ..."): retires
   * AUTO\SETMCH.PRG for emulator boots. */
  {
    uint32_t mch;
    if (emulator_config_machine_set(&mch))
      machine_cookie_set(mch);
  }

  /* Initialise JIT CPU core */
  fprintf(stderr, "[MAIN] calling jit_cpu_init cpu_type=%d\n", cpu_type);
  fflush(stderr);
  jit_cpu_set_compatible(config->cpu_compatible ? 1 : 0);
  jit_cpu_set_mmu(config->mmu ? 1 : 0);

  /* Monitor-detect force lives at the ps_protocol layer (see the note
   * in ps_read_txn): dispatcher-level shims provably missed TOS's
   * boot-time read, so the wire itself is overridden. Must be armed
   * BEFORE the CPU thread starts booting TOS - the deciding read is in
   * the ROM's first dozen instructions. */
  {
    extern volatile int ps_gpip7_force;
    ps_gpip7_force = config->monitor_force;
  }
  jit_cpu_set_perf_options(config->cpu_clock_multiplier,
                           config->cpu_clock_multiplier_set ? 1 : 0,
                           config->m68k_speed,
                           config->m68k_speed_set ? 1 : 0,
                           config->jit_cache,
                           config->jit_cache_set ? 1 : 0);
  jit_cpu_init (cpu_type,
                config->fpu ? 1 : 0,
                tt_ram_available ? 1 : 0,  /* not config->ttram: may be vetoed for 68000/010 */
                config->addr32 ? 1 : 0,
                config->jit ? 1 : 0); /* cpu_type: 0=68000 1=010 2=020 3=030 4=040 */
  fprintf(stderr, "[MAIN] jit_cpu_init returned\n");
  fflush(stderr);

  /* Start Emulation */
  cpu_emulation_running = 1; /* start the threads running - up until now, they are just waiting/looping  */

  pthread_join(cpu_tid, NULL);

  printf("[MAIN] Emulation Ended\n");
  platform_network_shutdown();
  kbd_usb_shutdown();

  return 0;
}

/*
 * CPU RESET instruction has been called
 * NOTE this instruction toggles the hardware RESET signal. It DOES NOT reset the CPU,
 * nor should it touch the HALT signal
 */
void cpu_pulse_reset(void)
{
  pulse_reset_inprogress = 1;

  /* Log the guest's state AT the RESET instruction. Software that
   * switches worlds (Spectre GCR drops into Macintosh mode this way)
   * issues RESET to clear the peripherals and then continues executing
   * - the CPU, its registers and RAM must survive untouched. This is
   * the last known-good instant before that switch, so record where the
   * guest was: everything we had previously captured was aftermath.
   * PISTORM_RESET_DEBUG=1 also dumps the registers. */
  pistorm_reset_state_dump();     /* newcpu.cpp - see note above */

  ps_pulse_reset();
  mfp_hub_reset();   /* stale virtual pending/in-service dies with the reset */
  if (DMA_Sound_enabled)
    dmasnd_capture_reset();
  ym2149_reset();
  st_blitter_reset();

  pulse_reset_inprogress = 0;
}

/* Full cold-boot of the Atari.
 * call ONLY from the host key handler / main thread — never a signal
 * handler and never the JIT hot loop. */
void atari_hard_reset(void)
{
  extern void m68k_reset(void);    /* newcpu.cpp:3789 */
  extern void jit_cpu_reset(void); /* invalidate JIT translations */

  pulse_reset_inprogress = 1;

  ps_reset_state_machine(); /* resync CPLD bus engine, clear any wedged S-state */
  ps_pulse_reset();         /* pulse Atari RESET: MFP / GLUE / DMA / FDC / PSG */
  if (DMA_Sound_enabled)
    dmasnd_capture_reset();
  ym2149_reset();
  st_blitter_reset();
  pistorm_net_reset();

  jit_cpu_reset(); /* drop stale translations before re-fetch */
  m68k_reset();    /* reload SSP from (0), PC from (4) */

  pulse_reset_inprogress = 0;
}

inline void cpu_set_fc(unsigned int _fc)
{
  fc = _fc;
}

/*
 * emulator_mem_dispatch.c — Optimized memory access functions
 *
 * Drop-in replacement for the m68k_read/write_memory_N() functions and
 * platform_read_check / platform_write_check in emulator.c.
 *
 * Uses mem_dispatch page-table for O(1) device routing instead of
 * sequential if-chains.
 *
 * Integration into emulator.c:
 *   1. Add  #include "gpio/mem_dispatch.h"  at the top
 *   2. Call  mem_dispatch_init(cfg)  after setup_platform_atari() in cpu_task()
 *   3. Either:
 *      a) Replace the m68k_read/write_memory functions with these, or
 *      b) Wrap the old functions with:
 *         #ifdef USE_MEM_DISPATCH
 *           #include "emulator_mem_dispatch.c"
 *         #else
 *           ... old code ...
 *         #endif
 *
 * Performance improvement:
 *   Before: ~40-60ns dispatch overhead per memory access
 *   After:  ~3-5ns dispatch overhead (single array lookup)
 *   Net:    ~10-15% emulation speedup at ~2-3M accesses/sec
 *
 * April 2026
 */

#ifdef __cplusplus
extern "C"
{
#endif

  extern uint8_t et4000_io_read8(ET4000State *, uint32_t);
  extern uint16_t et4000_io_read16(ET4000State *, uint32_t);
  extern int et4000_io_write8(ET4000State *, uint32_t, uint8_t);
  extern int et4000_io_write16(ET4000State *, uint32_t, uint16_t);
  extern int et4000_io_write32(ET4000State *, uint32_t, uint32_t);
  extern uint8_t et4000_vram_read8(ET4000State *, uint32_t);
  extern uint16_t et4000_vram_read16(ET4000State *, uint32_t);
  extern uint32_t et4000_vram_read32(ET4000State *, uint32_t);
  extern void et4000_vram_write8(ET4000State *, uint32_t, uint8_t);
  extern void et4000_vram_write16(ET4000State *, uint32_t, uint16_t);
  extern void et4000_vram_write32(ET4000State *, uint32_t, uint32_t);
#ifdef __cplusplus
}
#endif

//#define RTG
#define NOT_OBSOLETE 0
#if NOT_OBSOLETE
extern bool IDE_enabled;
bool RAM_CACHE_enabled = false;//true;
#define STRAM_MAX_ADDR 0x00400000
//volatile uint8_t rom_vector[8];
//volatile uint8_t *st_ram_cache;
#endif
/*
 * ================================================================
 * RTG write snoop
 *
 * RTG is NOT a full interceptor — it observes writes to system variables
 * and palette registers, then lets the write pass through to the bus.
 * This must be called BEFORE the dispatch table for pages 0x00 and 0xFF.
 * ================================================================
 */
#ifdef RTG

#define OP_TYPE_BYTE 1
#define OP_TYPE_WORD 2
#define OP_TYPE_LONGWORD 4
#define SYS_VARS 0x420
#define SYS_VARS_TOP 0x5b4
#define PALETTE_REGS 0xFF8240 // through to 0xFF825F = 16 words

uint8_t RTG_PALETTE_REG[0x20];
uint8_t RTG_enabled = 1;

/* ST Shifter palette cache: $FF8240..$FF825E, 16 big-endian words. */
volatile uint16_t st_palette[16];

static inline void rtg_write_snoop(uint8_t type, uint32_t addr, uint32_t val)
{
  if (__builtin_expect(!RTG_enabled, 1))
    return;

  /* System variables 0x420-0x5B4 */
  if (addr >= SYS_VARS && addr < SYS_VARS_TOP)
  {
    if (addr == 0x448)
      rtg.PAL = val;
    else if (addr == 0x44c)
    {
      if (rtg.shift_mode != val)
      {
        rtg.shift_mode = val;
        rtg.res_changed = 1;
      }
    }
    else if (addr == 0x44e)
      rtg.vram_base = val;
  }

  if (addr == 0xFFFF820C)
    rtg.vram_base = val;

  /* Palette registers 0xFF8240-0xFF825F */
  if ((addr & 0x00FFFFFF) >= PALETTE_REGS &&
      (addr & 0x00FFFFFF) < (PALETTE_REGS + 0x20))
  {
    if (type == OP_TYPE_WORD)
      RTG_PALETTE_REG[((addr & 0x00FFFFFF) - PALETTE_REGS) >> 1] = val; // toRGB565((uint16_t)val);
    else if (type == OP_TYPE_LONGWORD)
    {
      RTG_PALETTE_REG[((addr & 0x00FFFFFF) - PALETTE_REGS) >> 1] = val;       // toRGB565((uint16_t)(val >> 16));
      RTG_PALETTE_REG[(((addr & 0x00FFFFFF) - PALETTE_REGS) >> 1) + 1] = val; // toRGB565((uint16_t)(val));
    }
  }

  /* VRAM snoop (pass-through, does NOT intercept) */
  // if (addr >= (uint32_t)rtg_s.vram_base &&
  //     addr < ((uint32_t)rtg_s.vram_base + 0x8000)) {
  //   if (type == OP_TYPE_BYTE)
  //     et4000_vram_write8  (g_et4000, addr, (uint8_t)val);
  //   else if (type == OP_TYPE_WORD)
  //     et4000_vram_write16 (g_et4000, addr, (uint16_t)val);
  //   else
  //     et4000_vram_write32 (g_et4000, addr, val);
  // }
}
#endif

/* ST Shifter palette cache: $FF8240..$FF825E, 16 big-endian words. */
#ifndef RTG
volatile uint16_t st_palette[16];
#endif

/* --- Logging & Sniffing Logic --- */
/*
void log_dma_event(void) {
    uint16_t status = ps_read_16(ADDR_DMA_STATUS);
    uint16_t count  = ps_read_16(ADDR_DMA_DATA);

    printf("\n[DMA INTERRUPT DETECTED]\n");
    printf("  Status (0xFF8606): 0x%04X\n", status);
    printf("  Sector Count:      %u\n", count);

    if (status & 0x01) printf("  >> [!] ERROR: DMA Bus/Device Error\n");
    if (status & 0x02) printf("  >> [OK] SUCCESS: Sector count is zero\n");
    if (status & 0x04) printf("  >> [i] DRQ: Data Request is Active\n");
    printf("--------------------------\n");
}
*/


/* DMA Snooping Flag: Set true when physical FDC/ACSI is actively updating ST-RAM */
// static std::atomic<bool> physical_dma_active(false);

/* --- Helper to notify emulator when EmuTOS toggles DMA controller registers --- */
// inline void set_dma_transfer_state(bool active) {
//     physical_dma_active.store(active, std::memory_order_relaxed);
// }

static inline uint32_t check_ff_st(uint32_t add)
{
  if ((add & 0xFF000000) == 0xFF000000)
    add &= 0x00FFFFFF;

  return add;
}

/* In "shifter ste" mode the mirror acts as the STE shifter the ST host
 * lacks. Stale-value guard: an OS that believes the machine is an ST
 * (probe failed on the real bus) sets screens by writing ONLY hi/mid
 * and never clears the STE extras - EmuTOS's boot probe leaves a test
 * value in $FF820D that then skews the desktop forever. So a plain
 * hi/mid base write clears the STE extras; software that really uses
 * them (STE demos) rewrites them every frame and loses nothing. */
static inline void st_video_ste_extras_clear(void)
{
  rtg.low = 0;
  rtg.linewidth = 0;
  rtg.hscroll = 0;
}

/* PISTORM_VID_DEBUG=1: trace every shifter-register write the guest
 * makes (base hi/mid, STE low/linewidth/hscroll, shift mode) so screen
 * programming sequences are measured, not assumed. Low-rate registers:
 * at most a few hundred lines/second during a demo. */
static inline int vid_dbg(void)
{
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("PISTORM_VID_DEBUG");
    v = (e && *e == '1') ? 1 : 0;
  }
  return v;
}

static inline void vid_dbg_w(uint32_t a, uint32_t value)
{
  if (vid_dbg())
    fprintf(stderr, "[vid] W %06X=%02X\n", a, value & 0xFF);
}

static inline void st_video_snoop8(uint32_t address, uint8_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8201) {
    rtg.high = value;
    st_video_ste_extras_clear();
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8203) {
    rtg.mid = value;
    st_video_ste_extras_clear();
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF820D) {
    rtg.low = value;
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF820F) {
    rtg.linewidth = value;         /* STE: extra words per scanline */
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8265) {
    rtg.hscroll = value & 0x0F;    /* STE: fine horizontal scroll */
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8260) {
    rtg.shift_mode = value;
    rtg.hw_rez = value;              /* hardware truth for the renderer */
    pistorm_rez_sync_trace(a, (uint8_t)value);  /* legacy path was invisible */
    vid_dbg_w(a, value);
  }
}

static inline void st_video_snoop16(uint32_t address, uint16_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8200) {
    rtg.high = (uint8_t)value;
    st_video_ste_extras_clear();
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8202) {
    rtg.mid = (uint8_t)value;
    st_video_ste_extras_clear();
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF820C) {
    rtg.low = (uint8_t)value;
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF820E) {
    rtg.linewidth = (uint8_t)value;
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8264) {
    rtg.hscroll = (uint8_t)(value & 0x0F);
    vid_dbg_w(a, value);
  }
  else if (a == 0x00FF8260) {
    rtg.shift_mode = (uint8_t)(value >> 8);
    rtg.hw_rez = (uint8_t)(value >> 8);  /* hardware truth for the renderer */
    pistorm_rez_sync_trace(a, (uint8_t)(value >> 8));
  }
  else if (a >= 0x00FF8240 && a < 0x00FF8260)
    st_palette[(a - 0x00FF8240) >> 1] = value;
}

static inline void st_video_snoop32(uint32_t address, uint32_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8200) {
    rtg.high = (uint8_t)(value >> 16);
    rtg.mid = (uint8_t)value;
    st_video_ste_extras_clear();
    vid_dbg_w(a, value >> 16);
    vid_dbg_w(a + 2, value);
  } else if (a == 0x00FF820C) {
    rtg.low = (uint8_t)(value >> 16);
    rtg.linewidth = (uint8_t)value;
    vid_dbg_w(a, value >> 16);
    vid_dbg_w(a + 2, value);
  } else if (a >= 0x00FF8240 && a < 0x00FF8260) {
    unsigned i = (a - 0x00FF8240) >> 1;
    st_palette[i] = (uint16_t)(value >> 16);
    if (i + 1 < 16)
      st_palette[i + 1] = (uint16_t)value;
  }
}

/* musashi hooks follow */
/* musashi should no longer be used - jit needs address banks for performance */
#if (0)
/* FDD */
//extern "C" {
//extern "C" uint32_t  fdd_io_read  (uint32_t addr, int size);
extern  void      fdd_io_write (uint32_t addr, uint32_t val, int size);
extern  bool      fdd_owns_address (uint32_t addr);
/* fdd_route_address / acsi_* come from atari_fdd.h / acsi.h up top */
extern "C" int  dma_snoop_active (void);
extern "C" int  dma_snoop_owns   (uint32_t addr);
extern "C" void dma_snoop_write  (uint32_t addr, uint32_t val, int size);
extern "C" void dma_snoop_read   (uint32_t addr, uint32_t val, int size);
//}
//extern volatile uint8_t psg_latch;
//extern void psg_intercept_write(uint32_t, uint8_t);
//extern volatile int fdc_interrupt_pending;
//volatile uint8_t g_gpip;


/* --- Musashi READ Callbacks --- */
extern "C"
{

  unsigned int m68k_read_memory_8(unsigned int address)
  {
    /*
     * main r/w io for emulation - JIT Enigine uses these too
     * seperate 32bit transfers and 24bit transfers
     * Note: g_buserr is cleared at the top of each r/w to avoid any malingerers
	    */
	    g_buserr = 0;

	    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size), 1))
	      return natmem_offset[address];

#if (NOT_OBSOLETE)
    // if (address < et4kaddresses [ET4K_driver].vram_base && address >= et4kaddresses [ET4K_driver].vram_top)
    //   printf ("rd8: address 0x%X\n", address);

    // if (address == 0xD012EE)
    //   printf ("rd8: bad address 0x%X\n", address);

    // if ( ET4K_enabled && rtg.vram_base ) {
    //   if (address >= rtg.vram_base && address < rtg.vram_base + 0x8000) {
    //    uint32_t off = et4kaddresses [ET4K_driver].vram_base + (address - rtg.vram_base);
    //    printf ("native vram rd8 0x%X\n", off);
    //   // et4000_vram_write32 (g_et4000, off, value);
    //  }
    //}
#if (1)
    //if (__builtin_expect(address < 0x8, 0))
    //  return rom_vector[address];

    // if (__builtin_expect (RAM_CACHE_enabled && (address > 7) && (address < STRAM_MAX_ADDR), 1))
    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR), 1))
      // return ps_read_8(address);
      return st_ram_cache[address];

    if (__builtin_expect(address >= ROM_START && address < ROM_END, 1))
      return natmem_offset[address];

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size), 1))
      return natmem_offset[address];

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
        return readIDEB(add);
    }

    if (et4k_enabled())
    {
      if (in_et4k_vram(address))
        return et4000_vram_read8(g_et4000, address);

      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top)
        return et4000_io_read8(g_et4000, address);
    }
#endif
#endif
    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    if (blitter_disabled_addr(address))
      return 0xFF;

    /* host-emulated STE DMA sound: serve reads from the register shadow
       (the range bus-errors on the real ST bus - hardware is host-side) */
    if (DMA_Sound_enabled && dmasnd_owns(address))
      return dmasnd_reg_read8(address);

    /* FDD / emulated ACSI */
    if (FDD_enabled || acsi_enabled()) {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        return pistorm_mfp_gpip_shim (ps_read_8 (address));
      }

      if (fdd_route_address (address))
        return fdd_io_read (address, 1);
	    }
    /* Real bus-master DMA (no "fdd" line): snoop the registers on their way
     * past so the transfer window is known, and sync the mirror when it
     * completes. See dma_snoop.h - without this the guest cannot see what a
     * real DMA controller wrote, because ST-RAM reads come from the mirror. */
    else if (dma_snoop_active () && dma_snoop_owns (address))
    {
      cpu_data_fc();
      uint8_t v = ps_read_8 (address);
      dma_snoop_read (address, v, 1);
      return v;
    }

    /* USB/Bluetooth keyboard injection shadows */
    if (KBD_USB_enabled) {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        return pistorm_mfp_gpip_shim (ps_read_8 (address));
      }
      if (address == 0x00FFFC00) {
        cpu_data_fc();
        return kbd_usb_acia_status_shim (ps_read_8 (address));
      }
      if (address == 0x00FFFC02) {
        cpu_data_fc();
        return kbd_usb_acia_data_shim ();
      }
    }
    /* Native mouse threshold without USB injection. Only the ACIA data
     * register is touched, and exactly once - reading it clears RDRF, so
     * the filter is handed the byte rather than reading it again. */
    else if (kbd_native_mouse_enabled () && address == 0x00FFFC02) {
      cpu_data_fc();
      return kbd_native_rx_filter (ps_read_8 (address));
    }

	    cpu_data_fc();
	    return ps_read_8(address);
  }

	  unsigned int m68k_read_memory_16(unsigned int address)
	  {
	    g_buserr = 0;

	    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 2), 1))
	      return __builtin_bswap16(*(uint16_t *)&natmem_offset[address]);

#if (NOT_OBSOLETE)
    // if (address < et4kaddresses [ET4K_driver].vram_base && address >= et4kaddresses [ET4K_driver].vram_top - 2)
    //   printf ("rd16: address 0x%X\n", address);

    // if (address == 0xD012EE) {
    //   printf ("rd16: bad address 0x%X\n", address);
    //   return 0;
    // }

    // if ( ET4K_enabled && rtg.vram_base ) {
    //   if (address >= rtg.vram_base && address < rtg.vram_base + 0x8000) {
    //    uint32_t off = et4kaddresses [ET4K_driver].vram_base + (address - rtg.vram_base);
    //    printf ("native vram rd16 0x%X\n", off);
    //   // et4000_vram_write32 (g_et4000, off, value);
    //  }
    //}
#if (1)
   // if (__builtin_expect(address < 0x8, 0))
   //   return __builtin_bswap16(*(uint16_t *)&rom_vector[address]);

    // if (__builtin_expect(RAM_CACHE_enabled && (address > 7) && (address < STRAM_MAX_ADDR), 1))
    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR - 2), 1))
      return __builtin_bswap16(*(uint16_t *)&st_ram_cache[address]);

    if (__builtin_expect(address >= ROM_START && address < ROM_END, 1))
      return __builtin_bswap16(*(uint16_t *)&natmem_offset[address]);

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 2), 1))
      return __builtin_bswap16(*(uint16_t *)&natmem_offset[address]);

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        return readIDE(add);
      }
    }

    if (et4k_enabled())
    {
      if (in_et4k_vram(address))
        return et4000_vram_read16(g_et4000, address);

      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top - 2)
        return et4000_io_read16(g_et4000, address);
    }
#endif
#endif

    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    if (blitter_disabled_addr(address))
      return 0xFFFF;

    /* host-emulated STE DMA sound: serve reads from the register shadow */
    if (DMA_Sound_enabled && dmasnd_owns(address))
      return dmasnd_reg_read16(address);

    /* FDD / emulated ACSI */
    if (FDD_enabled || acsi_enabled())
    {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        return pistorm_mfp_gpip_shim ((uint8_t)ps_read_16 (address));
      }

      if (fdd_route_address (address))
          return fdd_io_read (address, 2);
    }

    /* USB/Bluetooth keyboard injection shadows (register on high byte) */
    if (KBD_USB_enabled) {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        return pistorm_mfp_gpip_shim ((uint8_t)ps_read_16 (address));
      }
      if (address == 0x00FFFC00) {
        cpu_data_fc();
        uint16_t v = ps_read_16 (address);
        return (uint16_t)((kbd_usb_acia_status_shim ((uint8_t)(v >> 8)) << 8) | (v & 0xFF));
      }
      if (address == 0x00FFFC02) {
        cpu_data_fc();
        return (uint16_t)((kbd_usb_acia_data_shim () << 8) | 0xFF);
      }
    }

	    cpu_data_fc();
	    return ps_read_16(address);
  }

	  unsigned int m68k_read_memory_32(unsigned int address)
	  {
	    g_buserr = 0;

	    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 4), 1))
	      return __builtin_bswap32(*(uint32_t *)&natmem_offset[address]);

#if (NOT_OBSOLETE)
    // if (address < et4kaddresses [ET4K_driver].vram_base && address >= et4kaddresses [ET4K_driver].vram_top - 4)
    //   printf ("rd32: address 0x%X\n", address);

    // if (address == 0xD012EE)
    //   printf ("rd32: bad address 0x%X\n", address);

    // if ( ET4K_enabled && rtg.vram_base ) {
    //   if (address >= rtg.vram_base && address < rtg.vram_base + 0x8000) {
    //     uint32_t off = et4kaddresses [ET4K_driver].vram_base + (address - rtg.vram_base);
    //    printf ("native vram rd32 0x%X\n", off);
    //   // et4000_vram_write32 (g_et4000, off, value);
    //  }
    //}
#if (1)
    //if (__builtin_expect(address < 0x8, 0))
    //  return __builtin_bswap32(*(uint32_t *)&rom_vector[address]);

    // if (__builtin_expect(RAM_CACHE_enabled && (address > 7) && (address < STRAM_MAX_ADDR), 1))
    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR - 4), 1))
      // return ps_read_32(address);
      return __builtin_bswap32(*(uint32_t *)&st_ram_cache[address]);

    if (__builtin_expect(address >= ROM_START && address < ROM_END, 1))
      return __builtin_bswap32(*(uint32_t *)&natmem_offset[address]);

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 4), 1))
      return __builtin_bswap32(*(uint32_t *)&natmem_offset[address]);

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        return readIDEL(add);
      }
    }

    if (et4k_enabled())
    {
      if (in_et4k_vram(address))
        return et4000_vram_read32(g_et4000, address);

      /* xVDI reads this address to see if card is present */
      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top - 4)
        return 0x00000000;
    }
#endif
#endif

    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    if (blitter_disabled_addr(address))
      return 0xFFFFFFFF;

    /* host-emulated STE DMA sound: serve reads from the register shadow */
    if (DMA_Sound_enabled && dmasnd_owns(address))
      return dmasnd_reg_read32(address);

     /* FDD / emulated ACSI */
    if (FDD_enabled || acsi_enabled())
    {
      if (fdd_route_address (address))
          return fdd_io_read (address, 4);
    }

    cpu_data_fc();
    return ps_read_32(address);
  }

  /* --- Musashi WRITE Callbacks --- */

  void m68k_write_memory_8(unsigned int address, unsigned int value)
  {
    g_buserr = 0;

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size), 1))
    {
      natmem_offset[address] = value;
      return;
    }

    st_video_snoop8(address, (uint8_t)value);

    /* PSG shadow for the HDMI sink. The natmem dispatcher snoops this
     * (HW_PAGE_PSG); this legacy path did not, so any PSG write routed
     * here reached the real chip but never the emulated one - native
     * audio perfect, HDMI audio missing writes. */
    if (ym2149_active())
      ym2149_snoop8(address, (uint8_t)value);

    if (DMA_Sound_enabled && dmasnd_owns(address))
    {
      dmasnd_snoop8 (address, (uint8_t)value); /* host owns the STE sound regs */
      return;                                  /* don't BERR the real ST bus */
    }

#if (NOT_OBSOLETE)
    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR), 1))
    {

      st_ram_cache[address] = value;
      /* Immediately sync to physical motherboard so MFP/PSG/Video chips stay current */
      cpu_data_fc();
      ps_write_8(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size), 1))
    {
      natmem_offset[address] = value; // max 16 MB
      return;
    }

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        writeIDEB(add, value);
        return;
      }
    }

    if (et4k_enabled())
    {
      if (address >= 0x00D00300 && address < 0x00D00400)
        printf("emulator ET4000 0x%X\n", address);
      if (in_et4k_vram(address))
      {
        et4000_vram_write8(g_et4000, address, (uint8_t)value);
        return;
      }

      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top)
      {
        et4000_io_write8(g_et4000, address, (uint8_t)value);
        return;
      }
    }
#endif

    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return;

    if (blitter_disabled_addr(address))
      return;

    /* FDD */
    if (FDD_enabled || acsi_enabled())
    {
      if (fdd_route_address (address)) {
          fdd_io_write (address, value, 1);
          return;
      }
    }
    else if (dma_snoop_active () && dma_snoop_owns (address))
    {
      cpu_data_fc();
      ps_write_8 (address, (uint8_t)value);
      dma_snoop_write (address, value, 1);
      return;
    }

    cpu_data_fc();
    /* keep the ST-RAM alias model in sync with guest memcfg writes -
     * this legacy path is the one that runs for IO when the JIT is off,
     * and it forwarded $FF8001 to the real chip while the natmem model
     * kept its boot value (1MB Mega ST field case) */
    pistorm_stram_memcfg_snoop(address, value, 1);
    mfp_note_eoi_write(address, value, false);
    mfp_hub_write_snoop (address, value, 0);  /* one shadow for all virtual channels */
    if (KBD_USB_enabled)
    {
      if (address == 0x00FFFC00)
      {
        kbd_usb_ctrl_snoop ((uint8_t)value);
        value = kbd_usb_ctrl_filter ((uint8_t)value);
      }
      else if (address == 0x00FFFC02)
        kbd_usb_tx_snoop ((uint8_t)value);
    }
    else if (address == 0x00FFFC02 && kbd_native_mouse_enabled ())
      kbd_native_tx_snoop ((uint8_t)value);
    ps_write_8 (address, (uint8_t)value);
  }

#if (0)
  /* Coherent ST-RAM writer for non-CPU bus masters (FDC/IDE DMA).
   * DMA bypasses the CPU snoop-on-write in m68k_write_memory_8, so mirror
   * the bytes into st_ram_cache here to keep the cache coherent. */
  void stram_dma_write(uint32_t addr, const uint8_t *buf, unsigned int count)
  {
    if (RAM_CACHE_enabled && addr < STRAM_MAX_ADDR)
    {
      unsigned int n = count;
      if (addr + n > STRAM_MAX_ADDR)
        n = STRAM_MAX_ADDR - addr; /* clamp to cache size */

      memcpy(st_ram_cache + addr, buf, n);
    }
    for (unsigned int i = 0; i < count; i++)
      ps_write_8(addr + i, buf[i]);
  }
#endif

  void m68k_write_memory_16 (unsigned int address, unsigned int value)
  {
    g_buserr = 0;

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 2), 1))
    {
      uint16_t *ptr = (uint16_t *)(&natmem_offset[address]);
      *ptr = __builtin_bswap16(value);
      return;
    }

    st_video_snoop16(address, (uint16_t)value);

    if (ym2149_active())                        /* see the 8-bit path */
      ym2149_snoop16(address, (uint16_t)value);

    if (DMA_Sound_enabled && dmasnd_owns(address))
    {
      dmasnd_snoop16 (address, (uint16_t)value); /* host owns the STE sound regs */
      return;
    }

#if (NOT_OBSOLETE)
    // if ( ET4K_enabled && rtg.vram_base) {
    //   if (address >= rtg.vram_base && address < rtg.vram_base + 0x8000) {
    //     uint32_t off = et4kaddresses [ET4K_driver].vram_base + (address - rtg.vram_base);
    //     printf ("native vram wr16 0x%X\n", off);
    //     et4000_vram_write16 (g_et4000, off, (uint16_t)value);
    //  }
    //}

    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR - 2), 1))
    {
      *(uint16_t *)(st_ram_cache + address) = __builtin_bswap16(value);
      cpu_data_fc();
      ps_write_16(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < 0x01000000u + tt_ram_size - 2), 1))
    {
      uint16_t *ptr = (uint16_t *)(&natmem_offset[address]);
      *ptr = __builtin_bswap16(value);
      return;
    }

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        writeIDE(add, value);
        return;
      }
    }

    if (et4k_enabled())
    {
      if (in_et4k_vram(address))
      {
        et4000_vram_write16(g_et4000, address, (uint16_t)value);
        return;
      }

      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top - 2)
      {
        {
          et4000_io_write16(g_et4000, address, (uint16_t)value);
          return;
        }
      }
    }
#endif
    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st (address);

    if (address & 0xFF000000)
      return;

    if (blitter_disabled_addr(address))
      return;

    /* FDD */
    if (FDD_enabled || acsi_enabled())
    {
      if (fdd_route_address (address)) {
          fdd_io_write (address, value, 2);
          return;
      }

      //if (address == 0x4C2) {
      //  ps_write_16 (0x4C2, 3);
      //  return;
      //}
    }

    cpu_data_fc();
    pistorm_stram_memcfg_snoop(address, value, 2);
    mfp_note_eoi_write(address, value, true);
    mfp_hub_write_snoop (address, value, 1);  /* one shadow for all virtual channels */
    if (KBD_USB_enabled)
    {
      if (address == 0x00FFFC00)
      {
        kbd_usb_ctrl_snoop ((uint8_t)(value >> 8));
        value = (value & 0x00FF) |
                ((unsigned int)kbd_usb_ctrl_filter ((uint8_t)(value >> 8)) << 8);
      }
      else if (address == 0x00FFFC02)
        kbd_usb_tx_snoop ((uint8_t)(value >> 8));
    }
    ps_write_16 (address, (uint16_t)value);
  }

  void m68k_write_memory_32 (unsigned int address, unsigned int value)
  {
    g_buserr = 0;

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < (0x01000000u + tt_ram_size - 4)), 1))
    {
      uint32_t *ptr = (uint32_t *)(&natmem_offset[address]);
      *ptr = __builtin_bswap32(value);
      return;
    }

    st_video_snoop32(address, (uint32_t)value);
    pistorm_stram_memcfg_snoop(address & 0x00FFFFFFu, value, 4);

    if (ym2149_active())                        /* see the 8-bit path;
                                                 * catches the classic
                                                 * move.l #$RR00VV00,$FF8800 */
      ym2149_snoop32(address, (uint32_t)value);

    if (DMA_Sound_enabled && dmasnd_owns(address))
    {
      dmasnd_snoop32 (address, (uint32_t)value); /* host owns the STE sound regs */
      return;
    }

#if (NOT_OBSOLETE)
    // if (address < et4kaddresses [ET4K_driver].vram_base && address >= et4kaddresses [ET4K_driver].vram_top - 4) {
    //   printf ("wr32: address 0x%X\n", address);
    //   return;
    // }

    // if (address == 0xD012EE)
    //   printf ("wr21: bad address 0x%X\n", address);

    // if ( ET4K_enabled && rtg.vram_base ) {
    //   if (address >= rtg.vram_base && address < rtg.vram_base + 0x8000) {
    //    uint32_t off = et4kaddresses [ET4K_driver].vram_base + (address - rtg.vram_base);
    //    printf ("native vram wr32 0x%X\n", off);
    //    et4000_vram_write32 (g_et4000, off, value);
    //  }
    //}

    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR - 4), 1))
    {
      *(uint32_t *)(st_ram_cache + address) = __builtin_bswap32(value);
      cpu_data_fc();
      ps_write_32(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < (0x01000000u + tt_ram_size - 4)), 1))
    {
      uint32_t *ptr = (uint32_t *)(&natmem_offset[address]);
      *ptr = __builtin_bswap32(value);
      return;
    }

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        writeIDEL(add, value);
        return;
      }
    }

    if (et4k_enabled())
    {
      if (in_et4k_vram(address))
      {
        et4000_vram_write32(g_et4000, address, value);
        return;
      }

      else if (address >= et4kaddresses[et4k_driver()].io_base && address < et4kaddresses[et4k_driver()].io_top - 4)
      {
        et4000_io_write32(g_et4000, address, value);
        return;
      }
    }
#endif

    /* 24 bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return;

    if (blitter_disabled_addr(address))
      return;

    if ( FDD_enabled || acsi_enabled() )
    {
      if (fdd_route_address (address)) {
          fdd_io_write (address, value, 4);
          return;
      }
    }

    cpu_data_fc();
    ps_write_32 (address, value);
  }

} // end extern "C"

#endif
