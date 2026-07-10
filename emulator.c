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

#include "platforms/atari/et4000/et4000.h"
// #include "platforms/atari/et4000/native_vga.h"
#include "config_file/config_file.h"
#include "gpio/ps_protocol.h"
#include "platforms/atari/audio/dmasnd.h"
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
  extern void jit_cpu_reset(void);
  extern void jit_cpu_execute(void);
  extern void pistorm_set_blitter_enabled(int enabled);

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
#include "platforms/atari/fdd/platform_atari_fdd.h"
#include "platforms/atari/network/platform_atari_network.h"

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
  struct timespec start, current;

  // Get the initial hardware clock value
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);

  // Calculate the target end time in absolute nanoseconds
  uint64_t start_ns = ((uint64_t)start.tv_sec * 1000000000ULL) + start.tv_nsec;
  uint64_t target_ns = start_ns + nanoseconds;

  uint64_t current_ns = 0;

  // Hardware busy-loop
  do
  {
    clock_gettime(CLOCK_MONOTONIC_RAW, &current);
    current_ns = ((uint64_t)current.tv_sec * 1000000000ULL) + current.tv_nsec;

    // Inline assembly "no-operation" instruction
    // Prevents the compiler (-O3) from optimizing away this empty loop
    asm volatile ("nop");
    //asm volatile ("yield" ::: "memory");
  } while (current_ns < target_ns);
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

  while (cpu_emulation_running)
  {
    /* read IPL lines only when bus transaction has ended */
    if ((status = *ioread) & 0x01)
    {
      // A very short sleep here is fine as it's just waiting for a hardware cycle finish
      asm volatile("yield" ::: "memory");
      //wait_ns (250);
      continue;
    }

    /*
     * gpio 5 & 6 = ipl 1 & 2
     * Assumes CPLD has already inverted active-low to active-high binary values
     * ipl results in: 0, 2, 4, or 6 (ipl = xx0)
    */
    ipl = (status & 0x60) >> 4;

    g_ipl = ipl;
    if (ipl != 0 && ipl > g_irq && ipl > g_irq_mask)
    {
      g_irq = ipl;
      jit_request_cpu_exit();
    }
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
          sig == SIGILL ? "SIGILL" : "SIGSEGV",
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

  if (sig == SIGSEGV)
  {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "[SEGV] si_addr=%p backtrace=%d\n", si->si_addr, n);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
  }

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

void sigint_handler(int sig_num)
{
  cpu_emulation_running = 0;

  printf("\n[MAIN] Exiting\n");

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
    fprintf(stderr, "[DBG] crash handler installed\n"); /* sanity check */
  }

  /* assign signal handlers */
  signal(SIGINT, sigint_handler);

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
  pistorm_set_blitter_enabled(emulator_config_blitter_enabled() ? 1 : 0);

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
      // ATARI STe ROM
      if (config->rom.rom_size >= (256 * 1024))
      {
        ROM_START = 0x00E00000;
        ROM_END = ROM_START + config->rom.rom_size; // 0x00F00000;
        ROM_MASK = config->rom.rom_size - 1;        // 0x000FFFFF;
       
        pistorm_rom_ptr = config->rom.rom_ptr;
        pistorm_rom_start = ROM_START;
        pistorm_rom_end = ROM_END;
        pistorm_rom_mask = ROM_MASK;

        //rom_ptr = (uint8_t*)cfg->rom.rom_ptr;
        //tos_ptr_big = rom_ptr;
        //rom_vector_ptr = (uint8_t*)cfg->rom.rom_ptr;
        //pistorm_rom_ptr = rom_ptr; pistorm_rom_start = ROM_START; pistorm_rom_end = ROM_END; pistorm_rom_mask = ROM_MASK;
        //printf ("rom ptr %p\n", pistorm_rom_ptr);
        //break;
      }

      /* setup ROM boot vector interception */
      //for (int n = 0; n < 8; n++ )
      //  rom_vector [n] = *pistorm_rom_ptr++;

      printf ("[INIT] ROM loaded\n");
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
    tt_ram_available = true;
    tt_ram_size = config->ttram_size ? config->ttram_size : (128u * 1024u * 1024u);
    if (tt_ram_size > 128u * 1024u * 1024u)
      tt_ram_size = 128u * 1024u * 1024u;
    printf("[INIT] TT-RAM allocated - %uMB\n", tt_ram_size >> 20);
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

  if (platform_network_init_from_config(config) != 0)
    fprintf(stderr, "[NET] network init failed; continuing without network backend\n");

  /* --------------------------- */

  // install_crash_handler ();

  /* --------------------------- */

  /* Initialise DMA Sound -> HDMI (STe only) */
  if (config->dma_sound) {
    if (dmasnd_init (NULL) == 0 && dmasnd_capture_start() == 0) {
      DMA_Sound_enabled = true;
      printf ("[INIT] DMA Sound enabled\n");
    } else {
      DMA_Sound_enabled = false;
      fprintf(stderr, "[INIT] DMA Sound failed to start\n");
    }
  } else {
    printf ("[INIT] DMA Sound disabled\n");
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
        fprintf(stderr, "[ERROR] Display thread failed to initialise\n");
        return 1;
      }
    }
  }
  
  if ( FDD_enabled )
  {
    // Start VBL timer thread
    err = pthread_create ( &vbl_id, NULL, &fdd_vbl_thread, NULL );

    if ( err != 0 )
      printf ( "[ERROR] Cannot create VBL thread: [%s]", strerror (err) );

    else
    {
      pthread_setname_np ( vbl_id, "pistorm: vbl" );
      printf ( "[MAIN] VBL thread created successfully\n" );
    }
  }
//FDD_enabled = false;
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

  /* Initialise JIT CPU core */
  fprintf(stderr, "[MAIN] calling jit_cpu_init cpu_type=%d\n", cpu_type);
  fflush(stderr);
  jit_cpu_set_perf_options(config->cpu_clock_multiplier,
                           config->cpu_clock_multiplier_set ? 1 : 0,
                           config->m68k_speed,
                           config->m68k_speed_set ? 1 : 0,
                           config->jit_cache,
                           config->jit_cache_set ? 1 : 0);
  jit_cpu_init (cpu_type,
                config->fpu ? 1 : 0,
                config->ttram ? 1 : 0,
                config->addr32 ? 1 : 0,
                config->jit ? 1 : 0); /* cpu_type: 0=68000 1=010 2=020 3=030 4=040 */
  fprintf(stderr, "[MAIN] jit_cpu_init returned\n");
  fflush(stderr);

  /* Start Emulation */
  cpu_emulation_running = 1; /* start the threads running - up until now, they are just waiting/looping  */

  pthread_join(cpu_tid, NULL);

  printf("[MAIN] Emulation Ended\n");
  platform_network_shutdown();

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

  printf("[RESET] soft CPU RST\n");
  ps_pulse_reset();
  if (DMA_Sound_enabled)
    dmasnd_capture_reset();

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

static inline void st_video_snoop8(uint32_t address, uint8_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8201)
    rtg.high = value;
  else if (a == 0x00FF8203)
    rtg.mid = value;
  else if (a == 0x00FF820D)
    rtg.low = value;
  else if (a == 0x00FF8260)
    rtg.shift_mode = value;
}

static inline void st_video_snoop16(uint32_t address, uint16_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8200)
    rtg.high = (uint8_t)value;
  else if (a == 0x00FF8202)
    rtg.mid = (uint8_t)value;
  else if (a == 0x00FF820C)
    rtg.low = (uint8_t)value;
  else if (a == 0x00FF8260)
    rtg.shift_mode = (uint8_t)(value >> 8);
  else if (a >= 0x00FF8240 && a < 0x00FF8260)
    st_palette[(a - 0x00FF8240) >> 1] = value;
}

static inline void st_video_snoop32(uint32_t address, uint32_t value)
{
  uint32_t a = address & 0x00FFFFFFu;

  if (a == 0x00FF8200) {
    rtg.high = (uint8_t)(value >> 16);
    rtg.mid = (uint8_t)value;
  } else if (a == 0x00FF820C) {
    rtg.low = (uint8_t)(value >> 16);
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
    /* FDD */
    if (FDD_enabled) {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        return fdd_gpip (ps_read_8 (address));
      }

      if (fdd_owns_address (address))
        return fdd_io_read (address, 1);
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
    /* FDD */
    if (FDD_enabled)
    {
      if (address == MFP_GPIP) {
        cpu_data_fc();
        uint8_t gpip = ps_read_16 (address);
        return fdd_gpip (gpip);
      }

      if (fdd_owns_address (address))
          return fdd_io_read (address, 2);
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
     /* FDD */
    if (FDD_enabled)
    {
      if (fdd_owns_address (address))
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

    if (DMA_Sound_enabled)
      dmasnd_snoop8 (address, (uint8_t)value); /* snoop STE sound regs */

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
    if (FDD_enabled)
    {
      if (fdd_owns_address (address)) {
          fdd_io_write (address, value, 1);
          return;
      }
    }

    cpu_data_fc();
    mfp_note_eoi_write(address, value, false);
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

    if (DMA_Sound_enabled)
      dmasnd_snoop16 (address, (uint16_t)value);

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
    if (FDD_enabled)
    {
      if (fdd_owns_address (address)) {
          fdd_io_write (address, value, 2);
          return;
      }

      //if (address == 0x4C2) {
      //  ps_write_16 (0x4C2, 3);
      //  return;
      //}
    }

    cpu_data_fc();
    mfp_note_eoi_write(address, value, true);
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

    if (DMA_Sound_enabled)
      dmasnd_snoop32 (address, (uint32_t)value);

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

    if ( FDD_enabled )
    {
      if (fdd_owns_address (address)) {
          fdd_io_write (address, value, 4);
          return;
      }
    }

    cpu_data_fc();
    ps_write_32 (address, value);
  }

} // end extern "C"

#endif
