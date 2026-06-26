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

/* JIT bridge entry points (jit_glue.cpp) */
#ifdef __cplusplus
extern "C"
{
#endif

  extern void jit_mem_init(void);
  extern void jit_cpu_init(int cpu_level);
  extern void jit_cpu_reset(void);
  extern void jit_cpu_execute(void);
  extern void jit_set_irq(int level);

#ifdef __cplusplus
}
#endif

extern int quit_program;
#include "platforms/atari/et4000/et4000.h"
#include "platforms/atari/IDE.h"
#include "platforms/atari/idedriver.h"
#include "platforms/atari/fdd/atari_fdd.h"
#include "platforms/atari/fdd/platform_atari_fdd.h"

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
bool ET4K_enabled;
int ET4K_driver; // 0 = NOVA, 1 = XVDI, 2 = NVDI, 3 = FVDI
bool ET4K_emutos_vga;

ET4KADDRESSES_s et4kaddresses[GRAPHICS_CARD_TYPES] =
{
  {0x00D00000, 0x00D10000, 0x00C00000, 0x00D00000}, // NOVA
  {0x00B00000, 0x00B10000, 0x00A00000, 0x00B00000}, // XVDI
  {0x00D00000, 0x00D10000, 0x00C00000, 0x00D00000}, // NVDI
  {0x00D00000, 0x00D10000, 0x00C00000, 0x00D00000}  // FVDI
};

ET4KADDRESSES_s *et4k_addr_ptr;

rtg_s rtg;
#if (0)
/* The XVDI/NOVA driver's edge-of-screen block moves (cursor save/restore,
 * clipped blits) step a few bytes below the framebuffer base when the pointer
 * is at the extreme top-left or off the left edge — e.g. base-4 = 0x009FFFFC.
 * Those used to miss the aperture and fall through to the unmapped bus -> BERR.
 * Fold a small guard band below the aperture into the card; et4000.c masks the
 * offset with (VRAM_SIZE-1), so the under-run wraps to the top of unused VRAM. */
#define ET4K_VRAM_GUARD 0x10000u /* 64 KB; must stay inside the gap below the card */

static inline int in_et4k_vram(uint32_t a)
{
  uint32_t base = et4kaddresses[ET4K_driver].vram_base;
  uint32_t top = et4kaddresses[ET4K_driver].vram_top;
  return a >= base - ET4K_VRAM_GUARD && a < top;
}
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  extern void *render_frame(void *);

#ifdef __cplusplus
}
#endif

/* Emulator setup */
unsigned int cpu_type;
struct emulator_config *cfg = NULL;

extern uint8_t fc;
volatile uint8_t g_irq = 0;
uint32_t tt_ram_size = 0;

volatile uint8_t g_irq_mask;
extern volatile uint8_t g_buserr;

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
    __asm__ volatile("nop");

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

static void *ipl_task(void *)
{
  cpu_set_t cpuset;
  pthread_t thread;
  struct sched_param param;
  uint16_t status;
  uint8_t ipl;
  uint64_t ipl2_cooldown_until = 0;
  uint64_t ipl4_cooldown_until = 0;
  uint64_t ipl6_cooldown_until = 0;

  /* anchor this task to cpu3 */
  CPU_ZERO (&cpuset);
  CPU_SET (3, &cpuset);
  thread = pthread_self();
  pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);

  /* set real-time priority for this task */
  param.sched_priority = 99; // Highest possible priority
  sched_setscheduler(0, SCHED_FIFO, &param);
 

  while (!cpu_emulation_running);

  usleep(1000000);

  while (cpu_emulation_running)
  {
    /* read IPL lines only when bus transaction has ended */
    if ((status = *ioread) & 0x01)
    {
      // A very short sleep here is fine as it's just waiting for a hardware cycle finish
      asm volatile("yield" ::: "memory");
      continue;
    }

    /*
     * gpio 5 & 6 = ipl 1 & 2
     * Assumes CPLD has already inverted active-low to active-high binary values
     * ipl results in: 0, 2, 4, or 6 (ipl = xx0)
     */
    ipl = (status & 0x60) >> 4;

    uint64_t current_time = get_time_us();

    //printf ("ipl = %d, g_irq = %d, g_irq_mask = %d\n", ipl, g_irq, g_irq_mask );
    /*
     * Only flag a new IRQ if:
     * 1. The JIT has cleared the previous one (g_irq == 0)
     * 2. The sampled IPL is higher than the current CPU Status Register mask
     * 3. The specific IPL level is not currently in a hardware cooldown period
     */
    if (g_irq == 0 && ipl > g_irq_mask)
    {
      bool trigger_irq = false;

      if (ipl == 2 && current_time >= ipl2_cooldown_until)
      {
        // HBLANK pulse duration cooldown (~15 us)
        ipl2_cooldown_until = current_time + 15;
        trigger_irq = true;
      }
      else if (ipl == 4 && current_time >= ipl4_cooldown_until)
      {
        // VBLANK pulse duration cooldown (~200 us is typical for GLUE to drop the line,
        ipl4_cooldown_until = current_time + 200;//20000;
        trigger_irq = true;
      }
      else if (ipl == 6 && current_time >= ipl6_cooldown_until)
      {
        // MFP pulse duration cooldown (~50 us)
        // actually , is cooldown on MFP to be used ? iack is real on the bus
        ipl6_cooldown_until = current_time + 50;//1000;
        trigger_irq = true;
      }

      if (trigger_irq)
      {
        g_irq = ipl; // Safely notify JIT engine of a new valid IRQ assertion
      }
    }
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

static void crash_handler(int sig, siginfo_t *si, void *uctx)
{
  extern void *pushall_call_handler;
  // extern uae_u8 *natmem_offset;
  long guest = (char *)si->si_addr - (char *)natmem_offset;

  fprintf(stderr, "[%s] host=%p natmem=%p guest_addr=0x%lx\n",
          sig == SIGILL ? "SIGILL" : "SIGSEGV",
          si->si_addr, (void *)natmem_offset, guest);

  if (sig == SIGILL)
  {
    uint32_t insn = si->si_addr ? *(volatile uint32_t *)si->si_addr : 0;
    fprintf(stderr, "       insn=0x%08x delta_from_pushall=+0x%lx\n",
            insn, (unsigned long)((char *)si->si_addr - (char *)pushall_call_handler));
  }

  if (sig == SIGSEGV)
  {
    /* in the SIGSEGV/bus-error handler, before converting to guest vec-2 */
    fprintf(stderr, "[SEGV] si_addr=%p  guest=%08lX\n",
            si->si_addr, (unsigned long)((uae_u8 *)si->si_addr - natmem_offset));
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


int main(int argc, char *argv[])
{
  struct emulator_config *cfg;
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
  ET4K_enabled = false;
  ET4K_driver = 0;
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
  cfg = load_config_file(config_file);

  /*
   * initialise emulator with config file parameters
   */
  if (cfg->cpu_type)
    cpu_type = cfg->cpu_type;

  /*
   * point to rom image
   */
  //for (int ix = 0; ix < cfg->rom_count; ix++)
  //{
    if (cfg->rom.rom_size != 0)
    {
      // ATARI STe ROM
      if (cfg->rom.rom_size >= (256 * 1024))
      {
        ROM_START = 0x00E00000;
        ROM_END = ROM_START + cfg->rom.rom_size; // 0x00F00000;
        ROM_MASK = cfg->rom.rom_size - 1;        // 0x000FFFFF;
       
        pistorm_rom_ptr = cfg->rom.rom_ptr;
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
  if (cfg->ttram)
  {
    tt_ram_available = true;
    printf("[INIT] TT-RAM allocated - %dMB\n", (TT_RAM_SIZE - 0x01000000) >> 20);
  }

  /*
   * Configure emulator interfaces
   */
  
  ET4K_enabled = cfg->graphics.card;
  ET4K_driver = cfg->graphics.driver; // 1, 2, 3, 4 / NOVA, XVDI, NVDI, FVDI

  if (ET4K_enabled)
  {
    et4k_addr_ptr = &et4kaddresses[ET4K_driver];
    screenGrab = true;
  }

  if (cfg->ide)
    InitIDE();

  if (cfg->fdd.enabled)
  {
    FDD_enabled = true;

    platform_fdd_init (cfg->fdd.img_path);
    //printf ("[INIT] FDD Image Attached %s\n", cfg->fdd.img_path);
  }

  /* --------------------------- */

  // install_crash_handler ();

  /* --------------------------- */

  /* Initialise DMA Sound -> HDMI (STe only) */
  if (cfg->dma_sound) {
    DMA_Sound_enabled = true;

    dmasnd_init ("plughw:vc4hdmi0");
    dmasnd_capture_start(); /* runs the pump on its own thread */

    printf ("[INIT] DMA Sound -> HDMI enabled\n");
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

  err = pthread_create(&ipl_tid, NULL, &ipl_task, NULL);

  if (err != 0)
    printf("[ERROR] Cannot create IPL thread: [%s]", strerror(err));

  else
  {
    pthread_setname_np(ipl_tid, "pistorm: ipl");
    printf("[MAIN] IPL thread created successfully\n");
  }

  /* start JIT Engine */
  /* Initialise JIT memory mapping (must be after tt_ram is allocated) */
  jit_mem_init();

  rtg.natmem = natmem_offset;
  // printf ("main: natmem_offset %p\n", natmem_offset);

  if (ET4K_enabled)
  {
    err = pthread_create(&e4k_tid, NULL, &render_frame, NULL);

    if (err != 0)
      printf("[ERROR] Cannot create ET4000 thread: [%s]", strerror(err));

    else
    {
      pthread_setname_np (e4k_tid, "pistorm: et4000");
      printf("[MAIN] ET4000 thread created successfully\n");
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
  jit_cpu_init (cpu_type); /* cpu_type: 0=68000 1=010 2=020 3=030 4=040 */

  /* Start Emulation */
  cpu_emulation_running = 1; /* start the threads running - up until now, they are just waiting/looping  */

  pthread_join(cpu_tid, NULL);

  printf("[MAIN] Emulation Ended\n");

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

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < TT_RAM_SIZE), 1))
      return natmem_offset[address];

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
        return readIDEB(add);
    }

    if (ET4K_enabled)
    {
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top)
        return et4000_vram_read8(g_et4000, address);

      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top)
        return et4000_io_read8(g_et4000, address);
    }
#endif
#endif
    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    /* BLITTER disable */
    // if (address >= 0xFF8A00 && address < 0xFF8C00)
    //   return 0xFF;
    /* FDD */
    if (FDD_enabled) {
      if (address == MFP_GPIP)
        return fdd_gpip (ps_read_8 (address));

      if (fdd_owns_address (address))
        return fdd_io_read (address, 1);
    }

    return ps_read_8(address);
  }

  unsigned int m68k_read_memory_16(unsigned int address)
  {
    g_buserr = 0;

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

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < TT_RAM_SIZE - 2), 1))
      return __builtin_bswap16(*(uint16_t *)&natmem_offset[address]);

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        return readIDE(add);
      }
    }

    if (ET4K_enabled)
    {
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top - 2)
        return et4000_vram_read16(g_et4000, address);

      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top - 2)
        return et4000_io_read16(g_et4000, address);
    }
#endif
#endif

    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    /* BLITTER disable */
    // if (address >= 0xFF8A00 && address < 0xFF8C00)
    //   return 0xFFFF;
    /* FDD */
    if (FDD_enabled)
    {
      if (address == MFP_GPIP) {
        uint8_t gpip = ps_read_16 (address);
        return fdd_gpip (gpip);
      }

      if (fdd_owns_address (address))
          return fdd_io_read (address, 2);
    }

    return ps_read_16(address);
  }

  unsigned int m68k_read_memory_32(unsigned int address)
  {
    g_buserr = 0;

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

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < TT_RAM_SIZE - 4), 1))
      return __builtin_bswap32(*(uint32_t *)&natmem_offset[address]);

    if (IDE_enabled)
    {
      uint32_t add = address & 0x00FFFFFF;
      if (add >= IDEBASEADDR && add < IDETOPADDR)
      {
        return readIDEL(add);
      }
    }

    if (ET4K_enabled)
    {
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top - 4)
        return et4000_vram_read32(g_et4000, address);

      /* xVDI reads this address to see if card is present */
      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top - 4)
        return 0x00000000;
    }
#endif
#endif

    /* 24bit address space */
    // address &= 0x00FFFFFF;
    address = check_ff_st(address);

    if (address & 0xFF000000)
      return 0;

    /* BLITTER disable */
    // if (address >= 0xFF8A00 && address < 0xFF8C00)
    //   return 0xFFFFFFFF;
     /* FDD */
    if (FDD_enabled)
    {
      if (fdd_owns_address (address))
          return fdd_io_read (address, 4);
    }

    return ps_read_32(address);
  }

  /* --- Musashi WRITE Callbacks --- */

  void m68k_write_memory_8(unsigned int address, unsigned int value)
  {
    g_buserr = 0;

    if (address == 0xFFFF8201)
      rtg.high = (uint8_t)value;
    if (address == 0xFFFF8203)
      rtg.mid = (uint8_t)value;
    if (address == 0xFFFF820D)
      rtg.low = (uint8_t)value;
    if (address == 0xFFFF8260)
      rtg.shift_mode = (uint8_t)value;

    if (DMA_Sound_enabled)
      dmasnd_snoop8 (address, (uint8_t)value); /* snoop STE sound regs */

#if (NOT_OBSOLETE)
    if (__builtin_expect(RAM_CACHE_enabled && (address < STRAM_MAX_ADDR), 1))
    {

      st_ram_cache[address] = value;
      /* Immediately sync to physical motherboard so MFP/PSG/Video chips stay current */
      ps_write_8(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < TT_RAM_SIZE), 1))
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

    if (ET4K_enabled)
    {
      if (address >= 0x00D00300 && address < 0x00D00400)
        printf("emulator ET4000 0x%X\n", address);
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top)
      {
        et4000_vram_write8(g_et4000, address, (uint8_t)value);
        return;
      }

      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top)
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

    /* FDD */
    if (FDD_enabled)
    {
      if (fdd_owns_address (address)) {
          fdd_io_write (address, value, 1);
          return;
      }
    }

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

    if (address >= 0xFFFF8240 && address < 0xFFFF8260) {
      st_palette[(address - 0xFFFF8240) >> 1] = (uint16_t)value;
    }

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
      ps_write_16(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < TT_RAM_SIZE - 2), 1))
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

    if (ET4K_enabled)
    {
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top - 2)
      {
        et4000_vram_write16(g_et4000, address, (uint16_t)value);
        return;
      }

      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top - 2)
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

    ps_write_16 (address, (uint16_t)value);
  }

  void m68k_write_memory_32 (unsigned int address, unsigned int value)
  {
    g_buserr = 0;

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
      ps_write_32(address, value);
      return;
    }

    if (__builtin_expect(tt_ram_available && (address >= 0x01000000 && address < (TT_RAM_SIZE - 4)), 1))
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

    if (ET4K_enabled)
    {
      if (address >= et4kaddresses[ET4K_driver].vram_base && address < et4kaddresses[ET4K_driver].vram_top - 4)
      {
        et4000_vram_write32(g_et4000, address, value);
        return;
      }

      else if (address >= et4kaddresses[ET4K_driver].io_base && address < et4kaddresses[ET4K_driver].io_top - 4)
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

    if ( FDD_enabled )
    {
      if (fdd_owns_address (address)) {
          fdd_io_write (address, value, 4);
          return;
      }
    }

    ps_write_32 (address, value);
  }

} // end extern "C"

