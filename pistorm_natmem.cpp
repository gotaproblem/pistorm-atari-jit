// SPDX-License-Identifier: MIT
//
// pistorm_natmem.cpp — single-array guest memory map for the Amiberry ARM64 JIT
// on pistorm-atari. Replaces Amiberry's shm/natmem allocator with one flat mmap
// reservation that we own, and wires the per-64KB bank table so that:
//
//   * RAM/ROM/TT-RAM are DIRECT-mapped  -> JIT bangs [x27 + addr], full speed
//   * I/O (HW regs, IDE) are HANDLER-routed -> ps_* over the bus, never banged
//   * ST-RAM reads are direct; writes can be write-through or cached + SMC-invalidate
//
// Verified against BlitterStudio/amiberry master: natmem_offset (uae_u8*),
// addrbank{lget..bput,baseaddr,flags,jit_read_flag,jit_write_flag,lgeti,wgeti},
// mem_banks[]/baseaddr[] (bankindex = addr>>16), invalidate_block/cache_invalidate,
// do_get_mem_*/do_put_mem_*.
//
// Symbols to confirm against YOUR live tree (/home/pistorm/pistorm-atari-jit/):
//   - `fc` declaration/type (set by ps_protocol before a bus cycle)
//   - S_READ / S_WRITE values (memory.h)
//   - the one-line hook in compile_block() that calls pistorm_mark_code()

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "jit/compemu.h"
#include "platforms/atari/et4000/et4000.h"
#include "config_file/config_file.h"
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "platforms/atari/audio/dmasnd.h"

extern "C"
{
    extern volatile uint8_t fc; // CONFIRM type/name in your tree

    unsigned int m68k_read_memory_8(unsigned int address);
    unsigned int m68k_read_memory_16(unsigned int address);
    unsigned int m68k_read_memory_32(unsigned int address);
    void m68k_write_memory_8(unsigned int address, unsigned int value);
    void m68k_write_memory_16(unsigned int address, unsigned int value);
    void m68k_write_memory_32(unsigned int address, unsigned int value);

    uint8_t ps_read_8(uint32_t address);
    uint16_t ps_read_16(uint32_t address);
    uint32_t ps_read_32(uint32_t address);

    void ps_write_8(uint32_t address, uint16_t data);
    void ps_write_16(uint32_t address, uint16_t data);
    void ps_write_32(uint32_t address, uint32_t data);

    uint8_t et4000_io_read8(ET4000State *, uint32_t);
    uint16_t et4000_io_read16(ET4000State *, uint32_t);
    int et4000_io_write8(ET4000State *, uint32_t, uint8_t);
    int et4000_io_write16(ET4000State *, uint32_t, uint16_t);
    int et4000_io_write32(ET4000State *, uint32_t, uint32_t);

    uint8_t et4000_vram_read8(ET4000State *, uint32_t);
    uint16_t et4000_vram_read16(ET4000State *, uint32_t);
    uint32_t et4000_vram_read32(ET4000State *, uint32_t);
    void et4000_vram_write8(ET4000State *, uint32_t, uint8_t);
    void et4000_vram_write16(ET4000State *, uint32_t, uint16_t);
    void et4000_vram_write32(ET4000State *, uint32_t, uint32_t);

    uint8_t et4000_engine_io_read(uint16_t port);
    void et4000_engine_io_write(uint16_t port, uint8_t val);

    uint8_t et4000_engine_vram_read8(uint32_t off);
    uint16_t et4000_engine_vram_read16(uint32_t off);
    uint32_t et4000_engine_vram_read32(uint32_t off);
    void et4000_engine_vram_write8(uint32_t off, uint8_t v);
    void et4000_engine_vram_write16(uint32_t off, uint16_t v);
    void et4000_engine_vram_write32(uint32_t off, uint32_t v);
    int et4000_engine_direct_vram_ok(void);
}

#ifndef ATARI_VGA_BANK_PROFILE
#define ATARI_VGA_BANK_PROFILE 0
#endif
#define ATARI_BLITTER_TRACE 0
#define ATARI_ACIA_TRACE 0

#if ATARI_VGA_BANK_PROFILE
typedef struct {
    uint64_t calls;
    uint64_t bytes;
    uint64_t total_ns;
    uint64_t max_ns;
} pistorm_vga_prof_counter_t;

enum {
    VGA_BANK_PROF_VRAM_RD_DIRECT,
    VGA_BANK_PROF_VRAM_WR_DIRECT,
    VGA_BANK_PROF_VRAM_RD_ENGINE,
    VGA_BANK_PROF_VRAM_WR_ENGINE,
    VGA_BANK_PROF_IO_RD,
    VGA_BANK_PROF_IO_WR,
    VGA_BANK_PROF_COUNT
};

static pistorm_vga_prof_counter_t vga_bank_prof[VGA_BANK_PROF_COUNT];
static uint64_t vga_bank_prof_window_start_ns;
static uint64_t vga_bank_prof_next_print_ns;
static double vga_bank_prof_peak_read_mb_s;
static double vga_bank_prof_peak_write_mb_s;

static int vga_bank_profile_enabled(void)
{
    return 1;
}

static uint64_t vga_bank_profile_now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void vga_bank_profile_init_window(uint64_t now)
{
    if (vga_bank_prof_next_print_ns)
        return;
    vga_bank_prof_window_start_ns = now;
    vga_bank_prof_next_print_ns = now + 5000000000ULL;
}

static uint64_t vga_bank_profile_bytes(unsigned a, unsigned b, unsigned c)
{
    return vga_bank_prof[a].bytes + vga_bank_prof[b].bytes + vga_bank_prof[c].bytes;
}

extern "C" void pistorm_vga_bank_profile_poll(void)
{
    uint64_t now = vga_bank_profile_now_ns();

    if (!vga_bank_profile_enabled())
        return;

    vga_bank_profile_init_window(now);
    if (now < vga_bank_prof_next_print_ns)
        return;

    double window_s = (double)(now - vga_bank_prof_window_start_ns) / 1000000000.0;
    if (window_s <= 0.0)
        window_s = 1.0;

    uint64_t read_bytes = vga_bank_profile_bytes(VGA_BANK_PROF_VRAM_RD_DIRECT,
                                                 VGA_BANK_PROF_VRAM_RD_ENGINE,
                                                 VGA_BANK_PROF_IO_RD);
    uint64_t write_bytes = vga_bank_profile_bytes(VGA_BANK_PROF_VRAM_WR_DIRECT,
                                                  VGA_BANK_PROF_VRAM_WR_ENGINE,
                                                  VGA_BANK_PROF_IO_WR);
    double read_mb_s = ((double)read_bytes / 1000000.0) / window_s;
    double write_mb_s = ((double)write_bytes / 1000000.0) / window_s;

    if (read_mb_s > vga_bank_prof_peak_read_mb_s)
        vga_bank_prof_peak_read_mb_s = read_mb_s;
    if (write_mb_s > vga_bank_prof_peak_write_mb_s)
        vga_bank_prof_peak_write_mb_s = write_mb_s;

    fprintf(stderr, "[VGABANK/5s] read MB/s=%.2f peak=%.2f\n",
            read_mb_s, vga_bank_prof_peak_read_mb_s);
    fprintf(stderr, "[VGABANK/5s] write MB/s=%.2f peak=%.2f\n",
            write_mb_s, vga_bank_prof_peak_write_mb_s);

    memset(vga_bank_prof, 0, sizeof(vga_bank_prof));
    vga_bank_prof_window_start_ns = now;
    vga_bank_prof_next_print_ns = now + 5000000000ULL;
}

static void vga_bank_profile_add(unsigned idx, uint64_t ns, unsigned bytes)
{
    if (!vga_bank_profile_enabled() || idx >= VGA_BANK_PROF_COUNT)
        return;

    vga_bank_profile_init_window(vga_bank_profile_now_ns());

    vga_bank_prof[idx].calls++;
    vga_bank_prof[idx].bytes += bytes;
    vga_bank_prof[idx].total_ns += ns;
    if (ns > vga_bank_prof[idx].max_ns)
        vga_bank_prof[idx].max_ns = ns;
}
#else
#define vga_bank_profile_enabled() 0
#define vga_bank_profile_now_ns() 0
#define vga_bank_profile_add(idx, ns, bytes) ((void)0)
extern "C" void pistorm_vga_bank_profile_poll(void) {}
#endif

extern "C"
{
    uint8_t readIDEB(uint32_t);
    uint16_t readIDE(uint32_t);
    uint32_t readIDEL(uint32_t);
    void writeIDEB(uint32_t, unsigned int);
    void writeIDE(uint32_t, unsigned int);
    void writeIDEL(uint32_t, uint32_t);
}

extern "C"
{
    uint32_t fdd_io_read      (uint32_t addr, int size);
    void     fdd_io_write     (uint32_t addr, uint32_t val, int size);
    bool     fdd_owns_address (uint32_t addr);
    uint8_t  fdd_gpip         (uint8_t other_gpip);
    void     mfp_note_eoi_write(uint32_t addr, uint32_t value, bool word);
}
extern bool FDD_enabled;

#define MFP_GPIP            0x00FFFA01u

void invalidate_block(blockinfo *); // verified in compemu_support_arm.cpp

/* cache_invalidate() is OURS (not an Amiberry symbol — the JIT exposes the
 * flush_icache function pointer instead). pistorm_smc() calls it only when a
 * write lands on a 4KB page previously marked as holding compiled code, so the
 * full flush here is coarse but infrequent. flush_icache is set during JIT
 * init; guard against the pre-init NULL. */
void cache_invalidate(void)
{
    extern uint32_t g_jp_smc;   /* attribution counter, newcpu.cpp */
    g_jp_smc++;
    if (flush_icache)
        flush_icache(0);
}

/* ------------------------------------------------------------------ */
/* INSTALL CONSTANTS — set these for your machine                      */
/* ------------------------------------------------------------------ */
static uint32_t pistorm_rom_size = 0; // derived from image length at load, 64KB-rounded
extern uint32_t ROM_START;
extern uint32_t ROM_END;

#define ST_RAM_SIZE (0x00400000u)  // 0x000000..
#define ROM_BASE (0x00E00000u)     // 0xFC0000 for TOS 1.x
#define ROM_TOP (0x00F00000u)      // ROM_END
#define ROM_MAX_SIZE (0x00100000u) // up to IDE @ 0xF00000 => 1MB max at 0xE00000
#define TT_RAM_BASE (0x01000000u)
#define TT_RAM_SIZE (0x08000000u) // 128MB
#define IO_IDE_BASE (0x00F00000u)
#define IO_IDE_SIZE (0x00000100u)
#define IO_HW_BASE (0x00FF8000u) //(0x00FF8000u)                  // shifter/DMA/PSG/MFP/ACIA
#define IO_HW_SIZE (0x00008000u) //(0x00008000u)                // 0xFF8000..0xFFFFFF, abuts TT_RAM_BASE (do NOT round to 64KB: that overruns TT-RAM)
#define VRAM_SIZE (0x00100000u)
#define VGA_IO_SIZE (0x00100000u)
#define VGA_BASE_XVDI (0x00A00000u) // XVDI
#define VGA_IO_BASE_XVDI (0x00B00000u)
#define VGA_BASE_NOVA (0x00C00000u) // NOVA
#define VGA_IO_BASE_NOVA (0x00D00000u)
#define FVDI_FB_BASE (0x20000000u)
#define FVDI_FB_MAX_BYTES (0x01000000u)

#define PISTORM_LEGACY_MEM_HOOKS 0

#define GUEST_RESERVE (TT_RAM_BASE + TT_RAM_SIZE) // 0x01000000 + 0x08000000 = 16MB + 128MB

uae_u8 *natmem_offset = NULL; // the one array; x27 in JIT
extern rtg_s rtg;
extern volatile uint16_t st_palette[16];

#define STRAM_LOW_WRITE_THROUGH_SIZE 0x000005B4u
#define STRAM_LOW_CONTROL_BANK_SIZE 0x00010000u
#define STRAM_SCREEN_WRITE_THROUGH_SIZE 0x00020000u
#define STRAM_DIRECT_START 0x00010000u
#define STRAM_DIRECT_TOP_RESERVE 0x00080000u
#define STRAM_DIRECT_END (ST_RAM_SIZE - STRAM_DIRECT_TOP_RESERVE)

static inline uae_u32 stram_be32(uaecptr a)
{
    return ((uae_u32)natmem_offset[a] << 24) |
           ((uae_u32)natmem_offset[a + 1] << 16) |
           ((uae_u32)natmem_offset[a + 2] << 8) |
           (uae_u32)natmem_offset[a + 3];
}

static inline uae_u16 stram_be16(uaecptr a)
{
    return ((uae_u16)natmem_offset[a] << 8) |
           (uae_u16)natmem_offset[a + 1];
}

static inline void stram_refresh_physbase(void)
{
    if (natmem_offset)
        rtg.vram_base = stram_be32(0x44e) & 0x00FFFFFEu;
}

static inline void stram_snoop_lowram(uaecptr a, int sz)
{
    a &= 0x00FFFFFFu;
    uae_u32 end = (uae_u32)a + (uae_u32)sz;
    if (!natmem_offset)
        return;

    if (a == 0x448u)
        rtg.PAL = (uae_u8)stram_be16(0x448);
    if (a == 0x44cu) {
        uae_u8 mode = (uae_u8)stram_be16(0x44c);
        if (rtg.shift_mode != mode) {
            rtg.shift_mode = mode;
            rtg.res_changed = 1;
        }
    }
    if (a < 0x452u && end > 0x44eu)
        stram_refresh_physbase();
}

static inline void st_video_snoop8(uint32_t address, uint8_t value)
{
    uint32_t a = address & 0x00FFFFFFu;

    if (a == 0x00FF8201u)
        rtg.high = value;
    else if (a == 0x00FF8203u)
        rtg.mid = value;
    else if (a == 0x00FF820Du)
        rtg.low = value;
    else if (a == 0x00FF8260u)
        rtg.shift_mode = value;
}

static inline void st_video_snoop16(uint32_t address, uint16_t value)
{
    uint32_t a = address & 0x00FFFFFFu;

    if (a == 0x00FF8200u)
        rtg.high = (uint8_t)value;
    else if (a == 0x00FF8202u)
        rtg.mid = (uint8_t)value;
    else if (a == 0x00FF820Cu)
        rtg.low = (uint8_t)value;
    else if (a == 0x00FF8260u)
        rtg.shift_mode = (uint8_t)(value >> 8);
    else if (a >= 0x00FF8240u && a < 0x00FF8260u)
        st_palette[(a - 0x00FF8240u) >> 1] = value;
}

static inline void st_video_snoop32(uint32_t address, uint32_t value)
{
    uint32_t a = address & 0x00FFFFFFu;

    if (a == 0x00FF8200u) {
        rtg.high = (uint8_t)(value >> 16);
        rtg.mid = (uint8_t)value;
    } else if (a == 0x00FF820Cu) {
        rtg.low = (uint8_t)(value >> 16);
    } else if (a >= 0x00FF8240u && a < 0x00FF8260u) {
        unsigned i = (a - 0x00FF8240u) >> 1;
        st_palette[i] = (uint16_t)(value >> 16);
        if (i + 1 < 16)
            st_palette[i + 1] = (uint16_t)value;
    }
}

static inline uae_u32 stram_screen_base(void)
{
    uae_u32 regbase = (((uae_u32)rtg.high) << 16) |
                      (((uae_u32)rtg.mid) << 8) |
                      ((uae_u32)rtg.low & 0xFEu);
    if (regbase && regbase < ST_RAM_SIZE)
        return regbase;

    if (rtg.vram_base && rtg.vram_base < ST_RAM_SIZE)
        return rtg.vram_base & 0x00FFFFFEu;

    if (natmem_offset) {
        uae_u32 physbase = stram_be32(0x44e) & 0x00FFFFFEu;
        if (physbase && physbase < ST_RAM_SIZE)
            return physbase;
    }

    return 0;
}

static inline int stram_range_overlaps(uaecptr a, int sz, uae_u32 start, uae_u32 len)
{
    uae_u32 end = (uae_u32)a + (uae_u32)sz;
    uae_u32 win_end = start + len;
    return a < win_end && end > start;
}

static inline int stram_needs_bus_write(uaecptr a, int sz)
{
    if (!emulator_config_stram_cache_enabled())
        return 1;

    a &= 0x00FFFFFFu;
    if (a < STRAM_LOW_WRITE_THROUGH_SIZE)
        return 1;

    uae_u32 screen = stram_screen_base();
    if (screen && stram_range_overlaps(a, sz, screen, STRAM_SCREEN_WRITE_THROUGH_SIZE))
        return 1;

    return 0;
}

/* The bank map itself (memory.cpp is not compiled). get_mem_bank(addr) ==
 * *mem_banks[addr>>16]. Neither stubs nor this file defined it before — both
 * comments pointed at the other. jit_mem_init() below fills every slot
 * (dummy_bank for unmapped space) so no entry is ever NULL at run time. */
addrbank *mem_banks[MEMORY_BANKS];

extern const uint8_t *pistorm_rom_ptr; // your loaded ROM bytes (m68k order)
extern uint32_t pistorm_rom_start, pistorm_rom_end;

#ifdef __cplusplus
extern "C"
{
#endif

    extern bool tt_ram_available;
    extern uint32_t tt_ram_size;
    extern ET4KADDRESSES_s *et4k_addr_ptr;

#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ */
/* SMC: per-4KB "page produced compiled code" bitmap                   */
/* ------------------------------------------------------------------ */
#define PAGE_SHIFT 12
static uint8_t code_page[GUEST_RESERVE >> PAGE_SHIFT]; // 8KB table for 32MB

extern void pistorm_mark_code(uaecptr pc) // call from compile_block()
{
    if (pc < GUEST_RESERVE)
        code_page[pc >> PAGE_SHIFT] = 1;
}

static inline void pistorm_smc(uaecptr addr, int sz)
{
    uae_u32 first_page, last_page;

    if (addr >= GUEST_RESERVE)
        return;

    first_page = addr >> PAGE_SHIFT;
    if (code_page[first_page]) {
        cache_invalidate(); // coarse but correct; targeted later
        return;
    }

    last_page = (addr + sz - 1) >> PAGE_SHIFT;
    if (last_page != first_page && code_page[last_page])
        cache_invalidate(); // coarse but correct; targeted later
}

extern "C" void pistorm_dma_from_stram(uint32_t addr, uint8_t *dst, uint32_t n)
{
    if (addr < ST_RAM_SIZE && n <= ST_RAM_SIZE - addr)
        memcpy(dst, natmem_offset + addr, n);
}

/* DMA path (fdc.c) calls this so the mirror stays coherent with bus DMA-in */
extern "C" void pistorm_dma_to_stram(uaecptr addr, const uint8_t *src, uint32_t n)
{
    if (addr < ST_RAM_SIZE && n <= ST_RAM_SIZE - addr)
        memcpy(natmem_offset + addr, src, n);
    pistorm_smc(addr, n);
}

/* ====================================================================== *
 * pistorm_bank_profile.inc.cpp
 * --------------------------------------------------------------------- *
 * Drop-in handler-bank profiler. Paste this block into pistorm_natmem.cpp
 * ABOVE the bank handlers (e.g. just after the fc_data()/fc_prog() helpers),
 * then add the PROF_* hooks shown at the bottom into the dm_* / io_* / sr_*
 * handlers. Build with -DPISTORM_BANK_PROFILE to enable; with the define
 * absent every hook compiles to nothing, so you can leave the hooks in
 * permanently at zero cost.
 *
 * Find the pid (ps aux | grep emulator) and:   kill -USR1 <pid>
 * to print: total reads/writes per bank, and the top-N hottest 64KB pages
 * across ALL handler banks — i.e. exactly where your bus cycles are going.
 * ====================================================================== */
#ifdef PISTORM_BANK_PROFILE
#include <signal.h>
#include <stdio.h>
#include <stdint.h>

static uint64_t prof_dummy_r = 0, prof_dummy_w = 0;
static uint64_t prof_io_r = 0, prof_io_w = 0;
static uint64_t prof_ide_r = 0, prof_ide_w = 0;
static uint64_t prof_fdd_r = 0, prof_fdd_w = 0;
static uint64_t prof_stram_w = 0;     /* ST-RAM writes go to the bus too */
static uint32_t prof_page_r[0x10000]; /* per-64KB-page read hits  */
static uint32_t prof_page_w[0x10000]; /* per-64KB-page write hits */

#define PROF_DUMMY_R(a)                      \
    do                                       \
    {                                        \
        prof_dummy_r++;                      \
        prof_page_r[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_DUMMY_W(a)                      \
    do                                       \
    {                                        \
        prof_dummy_w++;                      \
        prof_page_w[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_IO_R(a)                         \
    do                                       \
    {                                        \
        prof_io_r++;                         \
        prof_page_r[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_IO_W(a)                         \
    do                                       \
    {                                        \
        prof_io_w++;                         \
        prof_page_w[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_IDE_R(a)                        \
    do                                       \
    {                                        \
        prof_ide_r++;                        \
        prof_page_r[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_IDE_W(a)                        \
    do                                       \
    {                                        \
        prof_fdd_w++;                        \
        prof_page_w[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_FDD_R(a)                        \
    do                                       \
    {                                        \
        prof_fdd_r++;                        \
        prof_page_r[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_FDD_W(a)                        \
    do                                       \
    {                                        \
        prof_ide_w++;                        \
        prof_page_w[((a) >> 16) & 0xffff]++; \
    } while (0)
#define PROF_STRAM_W(a)                      \
    do                                       \
    {                                        \
        prof_stram_w++;                      \
        prof_page_w[((a) >> 16) & 0xffff]++; \
    } while (0)

static void prof_dump_top(const char *tag, uint32_t *tbl)
{
    /* Non-destructive top-16: snapshot-free, just scan 16 times. 64K scans
     * are nothing next to how long you've been running. */
    for (int k = 0; k < 16; k++)
    {
        uint32_t best = 0;
        int bi = -1;
        for (int i = 0; i < 0x10000; i++)
            if (tbl[i] > best)
            {
                best = tbl[i];
                bi = i;
            }
        if (bi < 0 || best == 0)
            break;
        fprintf(stderr, "[PROF] %s page %04X0000  hits=%u\n", tag, (unsigned)bi, best);
        tbl[bi] = 0; /* consume so the next pass finds the next */
    }
}

extern "C" void pistorm_bank_profile_dump(int sig)
{
    (void)sig;
    fprintf(stderr,
            "\n[PROF] ===== handler-bank totals =====\n"
            "[PROF] dummy  reads=%llu writes=%llu\n"
            "[PROF] io     reads=%llu writes=%llu\n"
            "[PROF] ide    reads=%llu writes=%llu\n"
            "[PROF] stram  writes=%llu (write-through to bus)\n"
            "[PROF] ----- hottest read pages -----\n",
            (unsigned long long)prof_dummy_r, (unsigned long long)prof_dummy_w,
            (unsigned long long)prof_io_r, (unsigned long long)prof_io_w,
            (unsigned long long)prof_ide_r, (unsigned long long)prof_ide_w,
            (unsigned long long)prof_stram_w);
    prof_dump_top("R", prof_page_r);
    fprintf(stderr, "[PROF] ----- hottest write pages -----\n");
    prof_dump_top("W", prof_page_w);
    fprintf(stderr, "[PROF] ================================\n\n");
    fflush(stderr);
    /* counters left as-is except the consumed top entries; call again later
     * to see the next tier, or wire a reset if you prefer windowed sampling. */
}

/* Call once from jit_mem_init() (or main) to arm the SIGUSR1 dumper. */
extern "C" void pistorm_bank_profile_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pistorm_bank_profile_dump;
    sigaction(SIGUSR1, &sa, NULL);
    fprintf(stderr, "[PROF] bank profiler armed: kill -USR1 %d to dump\n", (int)getpid());
}
#else
#define PROF_DUMMY_R(a) ((void)0)
#define PROF_DUMMY_W(a) ((void)0)
#define PROF_IO_R(a) ((void)0)
#define PROF_IO_W(a) ((void)0)
#define PROF_IDE_R(a) ((void)0)
#define PROF_IDE_W(a) ((void)0)
#define PROF_FDD_R(a) ((void)0)
#define PROF_FDD_W(a) ((void)0)
#define PROF_STRAM_W(a) ((void)0)
static inline void pistorm_bank_profile_init(void) {}
#endif

/* ---------------------------------------------------------------------- *
 * Hook placement (add the first line of each handler body):
 *
 *   dm_lget/wget/bget:  PROF_DUMMY_R(a);
 *   dm_lput/wput/bput:  PROF_DUMMY_W(a);
 *   io_lget/wget/bget:  PROF_IO_R(a);
 *   io_lput/wput/bput:  PROF_IO_W(a);
 *   sr_lput/wput/bput:  PROF_STRAM_W(a);   // optional: catch RAM write-through
 *
 * And in jit_mem_init(), once:  pistorm_bank_profile_init();
 * ---------------------------------------------------------------------- */

/* FC: drive the bus function-code from supervisor bit + access space */
static inline void fc_data(void) { fc = regs.s ? 0x5 : 0x1; } // 5 = supervisor data, 1 = user data
static inline void fc_prog(void) { fc = regs.s ? 0x6 : 0x2; } // 6 = supervisor program, 2 = user program

/* ps_protocol sets g_buserr when the addressed device never returns DTACK
 * (undecoded address on the real ST bus). The JIT engine acts on it HERE,
 * after the bus cycle has fully unwound — not inside ps_read, which is shared
 * with the interpreter/devices. hardware_exception2() raises a real 68k
 * vector-2 bus error: in JIT-compiled code it longjmps to the run loop
 * (JIT_HAS_BUS_ERROR_RECOVERY), in the interpreter it THROWs — either way it
 * does NOT return here. g_buserr is sticky, so clear it before raising. */
extern volatile uint8_t g_buserr;
static inline void pistorm_buserr(uaecptr a, uae_u32 v, bool read, int size)
{
    if (g_buserr)
    {
        hardware_exception2(a, v, read, false, size);
    }
}

/* ================================================================== */
/* Dummy bank — catch-all for unmapped space so no mem_banks[] slot is  */
/* ever NULL (a NULL entry => the JIT dereferences a null addrbank and   */
/* SIGSEGVs at a small offset). Routes to the bus like io_bank, but logs */
/* the first handful of accesses so we can see what guest address an     */
/* unmapped touch was aimed at (e.g. 32-bit Atari I/O at 0xFFFF8xxx).     */
/* ================================================================== */

#define DUMMY_LOG_LIMIT 256
static int dummy_log_n = 0;
static inline int dummy_log_ok(void) { return dummy_log_n < DUMMY_LOG_LIMIT ? (++dummy_log_n, 1) : 0; }
static inline bool dm_bus_addr(uaecptr *a)
{
    if ((*a & 0xFF000000) == 0xFF000000)
        *a &= 0x00FFFFFF;
    return (*a & 0xFF000000) == 0;
}

static inline uae_u32 ps_bus_lget(uaecptr a)
{
    uae_u32 hi = ps_read_16(a);
    uae_u32 lo = ps_read_16(a + 2);
    return (hi << 16) | (lo & 0xffff);
}

static inline void ps_bus_lput(uaecptr a, uae_u32 v)
{
    ps_write_16(a, (uae_u16)(v >> 16));
    ps_write_16(a + 2, (uae_u16)v);
}

static int pistorm_blitter_real_bus = 1;

extern "C" void pistorm_set_blitter_enabled(int enabled)
{
    pistorm_blitter_real_bus = enabled ? 1 : 0;
    //fprintf(stderr, "[NATMEM] Blitter %s\n", pistorm_blitter_real_bus ? "enabled" : "disabled");
}

static inline int blitter_addr(uaecptr a)
{
    a &= 0x00FFFFFF;
    return a >= 0x00FF8A00u && a < 0x00FF8C00u;
}

static inline int blitter_real_bus_enabled(void)
{
    return pistorm_blitter_real_bus;
}

static inline int blitter_hide_addr(uaecptr a)
{
    return blitter_addr(a) && !blitter_real_bus_enabled();
}

static inline uae_u32 blitter_absent_value(int size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static uae_u8 blitter_shadow[0x40];

static inline uae_u16 blitter_shadow_w(unsigned off)
{
    return ((uae_u16)blitter_shadow[off] << 8) | blitter_shadow[off + 1];
}

static inline uae_u32 blitter_shadow_l(unsigned off)
{
    return ((uae_u32)blitter_shadow[off] << 24) |
           ((uae_u32)blitter_shadow[off + 1] << 16) |
           ((uae_u32)blitter_shadow[off + 2] << 8) |
           blitter_shadow[off + 3];
}

static void blitter_shadow_write(uaecptr a, uae_u32 v, int size)
{
    unsigned off = (unsigned)(a & 0x3f);
    if (!blitter_addr(a) || off + (unsigned)size > sizeof(blitter_shadow))
        return;

    if (size == 4)
    {
        blitter_shadow[off] = (uae_u8)(v >> 24);
        blitter_shadow[off + 1] = (uae_u8)(v >> 16);
        blitter_shadow[off + 2] = (uae_u8)(v >> 8);
        blitter_shadow[off + 3] = (uae_u8)v;
    }
    else if (size == 2)
    {
        blitter_shadow[off] = (uae_u8)(v >> 8);
        blitter_shadow[off + 1] = (uae_u8)v;
    }
    else
    {
        blitter_shadow[off] = (uae_u8)v;
    }
}

static void blitter_sync_dest(void)
{
    uae_u32 dst = blitter_shadow_l(0x32) & 0x00FFFFFEu;
    int dx = (int16_t)blitter_shadow_w(0x2e);
    int dy = (int16_t)blitter_shadow_w(0x30);
    uint32_t xcnt = blitter_shadow_w(0x36);
    uint32_t ycnt = blitter_shadow_w(0x38);

    if (!xcnt)
        xcnt = 65536;
    if (!ycnt)
        ycnt = 65536;
    if (!xcnt || !ycnt || xcnt > 4096 || ycnt > 4096)
        return;

    int64_t xlast = (int64_t)(xcnt - 1) * dx;
    int64_t ylast = (int64_t)(ycnt - 1) * dy;
    int64_t minoff = 0;
    int64_t maxoff = 2;

    if (xlast < minoff)
        minoff = xlast;
    if (xlast + 2 > maxoff)
        maxoff = xlast + 2;
    if (ylast < minoff)
        minoff = ylast;
    if (ylast + 2 > maxoff)
        maxoff = ylast + 2;
    if (xlast + ylast < minoff)
        minoff = xlast + ylast;
    if (xlast + ylast + 2 > maxoff)
        maxoff = xlast + ylast + 2;

    int64_t start64 = (int64_t)dst + minoff;
    int64_t end64 = (int64_t)dst + maxoff;
    if (end64 <= 0 || start64 >= ST_RAM_SIZE)
        return;
    if (start64 < 0)
        start64 = 0;
    if (end64 > ST_RAM_SIZE)
        end64 = ST_RAM_SIZE;

    uae_u32 start = (uae_u32)start64 & ~1u;
    uae_u32 end = ((uae_u32)end64 + 1u) & ~1u;
    if (end > ST_RAM_SIZE)
        end = ST_RAM_SIZE;
    if (end <= start)
        return;

    uint8_t old_fc = fc;
    fc = 5;
    for (uae_u32 p = start; p + 1 < end; p += 2)
    {
        g_buserr = 0;
        uae_u16 w = ps_read_16(p);
        if (g_buserr)
        {
            g_buserr = 0;
            break;
        }
        natmem_offset[p] = (uae_u8)(w >> 8);
        natmem_offset[p + 1] = (uae_u8)w;
    }
    fc = old_fc;
    pistorm_smc(start, end - start);
}

static void blitter_after_write(uaecptr a, uae_u32 v, int size)
{
    blitter_shadow_write(a, v, size);
    if (!blitter_addr(a))
        return;

    uae_u32 folded = a & 0x00FFFFFFu;
    if (folded <= 0x00FF8A3Cu && folded + (uae_u32)size > 0x00FF8A3Cu && (blitter_shadow[0x3c] & 0x80))
    {
        uint8_t old_fc = fc;
        fc = 5;
        for (unsigned i = 0; i < 100000; i++)
        {
            g_buserr = 0;
            if (!(ps_read_8(0x00FF8A3Cu) & 0x80) || g_buserr)
                break;
        }
        g_buserr = 0;
        fc = old_fc;
        blitter_sync_dest();
    }
}

#if ATARI_BLITTER_TRACE
static inline void blitter_trace(const char *op, uaecptr a, uae_u32 v, int size)
{
    static unsigned count;
    if (!blitter_addr(a) || count >= 64)
        return;
    count++;
    fprintf(stderr, "[BLITTER] %s%d %06X %0*X berr=%u\n",
            op, size * 8, (unsigned)(a & 0x00FFFFFFu),
            size * 2, (unsigned)v, (unsigned)g_buserr);
}
#else
#define blitter_trace(op, a, v, size) ((void)0)
#endif

#if ATARI_ACIA_TRACE
static inline int acia_addr(uaecptr a)
{
    a &= 0x00FFFFFFu;
    return a >= 0x00FFFC00u && a < 0x00FFFC08u;
}

static inline void acia_trace(const char *op, uaecptr a, uae_u32 v, int size)
{
    static unsigned count;
    if (!acia_trace_enabled() || !acia_addr(a) || count >= 256000)
        return;
    count++;
    fprintf(stderr, "[ACIA] %s%d %06X -> %0*X berr=%u fc=%u\n",
            op, size * 8, (unsigned)(a & 0x00FFFFFFu),
            size * 2, (unsigned)v, (unsigned)g_buserr, (unsigned)fc);
}
#else
#define acia_trace(op, a, v, size) ((void)0)
#endif
/* reads: log the VALUE the bus returned, then raise BERR if undecoded */

// static uae_u32 dm_lget(uaecptr a){ fc_data(); uae_u32 v=m68k_read_memory_32(a); if(dummy_log_ok()) fprintf(stderr,"[DUMMY] lget @0x%08x -> 0x%08x%s\n",a,v,g_buserr?" BERR":""); pistorm_buserr(a,0,true,sz_long); return v; }
// static uae_u32 dm_wget(uaecptr a){ fc_data(); uae_u32 v=m68k_read_memory_16(a); if(dummy_log_ok()) fprintf(stderr,"[DUMMY] wget @0x%08x -> 0x%04x%s\n",a,v&0xffff,g_buserr?" BERR":""); pistorm_buserr(a,0,true,sz_word); return v; }
// static uae_u32 dm_bget(uaecptr a){ fc_data(); uae_u32 v=m68k_read_memory_8 (a); if(dummy_log_ok()) fprintf(stderr,"[DUMMY] bget @0x%08x -> 0x%02x%s\n",a,v&0xff,g_buserr?" BERR":""); pistorm_buserr(a,0,true,sz_byte); return v; }

// static void dm_lput(uaecptr a, uae_u32 v){ if(dummy_log_ok()) fprintf(stderr,"[DUMMY] lput @0x%08x = 0x%08x\n",a,v); fc_data(); m68k_write_memory_32(a, v); pistorm_buserr(a,v,false,sz_long); }
// static void dm_wput(uaecptr a, uae_u32 v){ if(dummy_log_ok()) fprintf(stderr,"[DUMMY] wput @0x%08x = 0x%04x\n",a,v&0xffff); fc_data(); m68k_write_memory_16(a, v); pistorm_buserr(a,v,false,sz_word); }
// static void dm_bput(uaecptr a, uae_u32 v){ if(dummy_log_ok()) fprintf(stderr,"[DUMMY] bput @0x%08x = 0x%02x\n",a,v&0xff); fc_data(); m68k_write_memory_8 (a, v); pistorm_buserr(a,v,false,sz_byte); }

static uae_u32 dm_lget(uaecptr a)
{
    PROF_DUMMY_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_32(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return 0;
    uae_u32 v = ps_bus_lget(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
#endif
}
static uae_u32 dm_wget(uaecptr a)
{
    PROF_DUMMY_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return (uae_u16)v;
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return 0;
    uae_u16 v = ps_read_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return v;
#endif
}
static uae_u32 dm_bget(uaecptr a)
{
    PROF_DUMMY_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return (uae_u8)v;
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return 0;
    uae_u8 v = ps_read_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return v;
#endif
}

/*
static uae_u32 dm_lget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u32 v=ps_read_32(a); pistorm_buserr(a,0,true,sz_long); return v; }
static uae_u32 dm_wget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u16 v=ps_read_16(a); pistorm_buserr(a,0,true,sz_word); return v; }
static uae_u32 dm_bget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u8  v=ps_read_8 (a); pistorm_buserr(a,0,true,sz_byte); return v; }
*/

static void dm_lput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_32(a, v);
    pistorm_buserr(a, v, false, sz_long);
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return;
    ps_bus_lput(a, v);
    pistorm_buserr(a, v, false, sz_long);
#endif
}
static void dm_wput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_16(a, v);
    pistorm_buserr(a, v, false, sz_word);
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return;
    ps_write_16(a, (uint16_t)v);
    pistorm_buserr(a, v, false, sz_word);
#endif
}
static void dm_bput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_8(a, v);
    pistorm_buserr(a, v, false, sz_byte);
#else
    g_buserr = 0;
    fc_data();
    if (!dm_bus_addr(&a))
        return;
    ps_write_8(a, (uint8_t)v);
    pistorm_buserr(a, v, false, sz_byte);
#endif
}

/*
static void dm_lput(uaecptr a, uae_u32 v){ PROF_DUMMY_W(a); fc_data(); ps_write_32(a, v); pistorm_buserr(a,v,false,sz_long); }
static void dm_wput(uaecptr a, uae_u32 v){ PROF_DUMMY_W(a); fc_data(); ps_write_16(a, (uint16_t)v); pistorm_buserr(a,v,false,sz_word); }
static void dm_bput(uaecptr a, uae_u32 v){ PROF_DUMMY_W(a); fc_data(); ps_write_8 (a, (uint8_t)v); pistorm_buserr(a,v,false,sz_byte); }
*/
static int dm_check(uaecptr a, uae_u32 sz) { return 0; }//return a < 0x01000000; }                 // not directly addressable
static uae_u8 *dm_xlate(uaecptr a) { return natmem_offset + a; }           // never used (check=0)

addrbank pistorm_dummy_bank = {
    dm_lget,
    dm_wget,
    dm_bget,
    dm_lput,
    dm_wput,
    dm_bput,
    dm_xlate,
    dm_check,
    NULL,
    "pistorm dummy",
    "pistorm dummy",
    dm_lget,
    dm_wget,
    ABFLAG_RAM | ABFLAG_INDIRECT,
    S_READ,
    S_WRITE,
};

/* NOVA aliases VGA I/O ports 0x3C0..0x3DF into the ST I/O area at 0xFF83C0..0xFF83DF */
// static inline int vga_ioalias(uaecptr a){ uint32_t o = a & 0xFFFF; return o >= 0x83B0 && o <= 0x83DF; }

/* ================================================================== */
/* I/O bank — handler-routed, hits the bus via your existing dispatch  */
/* ================================================================== */
/*
static uae_u32 io_lget(uaecptr a){ PROF_IO_R(a); fc_data(); uae_u32 v=m68k_read_memory_32(a); pistorm_buserr(a,0,true,sz_long); return v; }
static uae_u32 io_wget(uaecptr a){ PROF_IO_R(a); fc_data(); uae_u32 v=m68k_read_memory_16(a); pistorm_buserr(a,0,true,sz_word); return (uint16_t)v; }
static uae_u32 io_bget(uaecptr a){ PROF_IO_R(a); fc_data(); uae_u32 v=m68k_read_memory_8 (a); pistorm_buserr(a,0,true,sz_byte); return (uint8_t)v; }
*/
static uae_u32 io_lget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_32(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT semantics: raise a guest bus error exactly like a
         * machine with no blitter, so TOS/EmuTOS's presence probe fails and
         * it uses software rendering. Returning a value here (old code) made
         * the probe detect a phantom blitter whose writes were swallowed -
         * desktop with no menus. */
        blitter_trace("H", a, 0, 4);
        hardware_exception2(a, 0, true, false, sz_long);
        return 0xFFFFFFFFu; /* not reached */
    }
    if (FDD_enabled && fdd_owns_address(a))
        return fdd_io_read(a, 4);
    uae_u32 v = ps_bus_lget(a);
    pistorm_buserr(a, 0, true, sz_long);
    acia_trace("R", a, v, 4);
    blitter_trace("R", a, v, 4);
    return v;
#endif
}
static uae_u32 io_wget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return (uae_u16)v;
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("H", a, 0, 2);
        hardware_exception2(a, 0, true, false, sz_word);
        return 0xFFFFu; /* not reached */
    }
    if (FDD_enabled)
    {
        if (a == MFP_GPIP)
        {
            uae_u16 v = fdd_gpip(ps_read_8(a));
            pistorm_buserr(a, 0, true, sz_word);
            return v;
        }
        if (fdd_owns_address(a))
            return (uae_u16)fdd_io_read(a, 2);
    }
    uae_u16 v = ps_read_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    acia_trace("R", a, v, 2);
    blitter_trace("R", a, v, 2);
    return v;
#endif
}
static uae_u32 io_bget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    uae_u32 v = m68k_read_memory_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return (uae_u8)v;
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("H", a, 0, 1);
        hardware_exception2(a, 0, true, false, sz_byte);
        return 0xFFu; /* not reached */
    }
    if (FDD_enabled)
    {
        if (a == MFP_GPIP)
        {
            uae_u8 v = fdd_gpip(ps_read_8(a));
            pistorm_buserr(a, 0, true, sz_byte);
            return v;
        }
        if (fdd_owns_address(a))
            return (uae_u8)fdd_io_read(a, 1);
    }
    uae_u8 v = ps_read_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    acia_trace("R", a, v, 1);
    blitter_trace("R", a, v, 1);
    return v;
#endif
}
/*
static void io_lput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_32(a, v); pistorm_buserr(a,v,false,sz_long); }
static void io_wput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_16(a, (uint16_t)v); pistorm_buserr(a,v,false,sz_word); }
static void io_bput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_8 (a, (uint8_t)v); pistorm_buserr(a,v,false,sz_byte); }
*/

static void io_lput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_32(a, v);
    pistorm_buserr(a, v, false, sz_long);
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    st_video_snoop32(a, (uint32_t)v);
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 4);
        hardware_exception2(a, v, false, false, sz_long);
        return; /* not reached */
    }
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 4);
        return;
    }
    dmasnd_snoop32(a, (uint32_t)v);
    ps_bus_lput(a, v);
    pistorm_buserr(a, v, false, sz_long);
    acia_trace("W", a, v, 4);
    blitter_trace("W", a, v, 4);
    if (blitter_addr(a))
        blitter_after_write(a, v, 4);
#endif
}
static void io_wput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_16(a, v);
    pistorm_buserr(a, v, false, sz_word);
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    st_video_snoop16(a, (uint16_t)v);
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 2);
        hardware_exception2(a, v, false, false, sz_word);
        return; /* not reached */
    }
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 2);
        return;
    }
    mfp_note_eoi_write(a, v, true);
    dmasnd_snoop16(a, (uint16_t)v);
    ps_write_16(a, (uint16_t)v);
    pistorm_buserr(a, v, false, sz_word);
    acia_trace("W", a, v, 2);
    blitter_trace("W", a, v, 2);
    if (blitter_addr(a))
        blitter_after_write(a, v, 2);
#endif
}
static void io_bput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    fc_data();
    m68k_write_memory_8(a, v);
    pistorm_buserr(a, v, false, sz_byte);
#else
    g_buserr = 0;
    fc_data();
    a &= 0x00FFFFFF;
    st_video_snoop8(a, (uint8_t)v);
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 1);
        hardware_exception2(a, v, false, false, sz_byte);
        return; /* not reached */
    }
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 1);
        return;
    }
    mfp_note_eoi_write(a, v, false);
    dmasnd_snoop8(a, (uint8_t)v);
    ps_write_8(a, (uint8_t)v);
    pistorm_buserr(a, v, false, sz_byte);
    acia_trace("W", a, v, 1);
    blitter_trace("W", a, v, 1);
    if (blitter_addr(a))
        blitter_after_write(a, v, 1);
#endif
}

static int io_check(uaecptr a, uae_u32 sz) { return 0; }//return (a >= IO_HW_BASE && a < (IO_HW_BASE + IO_HW_SIZE) - sz); }                 // not directly addressable
static uae_u8 *io_xlate(uaecptr a) { return natmem_offset + a; } // never used (check=0)

static addrbank pistorm_io_bank = {
    io_lget,
    io_wget,
    io_bget,
    io_lput,
    io_wput,
    io_bput,
    io_xlate,
    io_check,
    NULL,
    "pistorm I/O",
    "pistorm I/O",
    io_lget,
    io_wget, // lgeti/wgeti (no code here)
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ,
    S_WRITE, // jit_read_flag/jit_write_flag
};

/* ================================================================== */
/* IDE I/F bank — handler-routed                                      */
/* ================================================================== */

#define IDE_REG_LO 0x00F00000u
#define IDE_REG_HI 0x00F00100u /* IDETOPADDR: 256-byte reg window */

static inline uae_u32 ide_fold(uaecptr a) { return a & 0x00FFFFFFu; }
static inline int ide_in_regs(uae_u32 a) { return a >= IDE_REG_LO && a < IDE_REG_HI; }

/* Reads: emulated controller for the register window, real bus for the rest of
 * the page (matches what the dummy/m68k_read_memory path used to do). */
static uae_u32 ide_lget(uaecptr a)
{
    PROF_IDE_R(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
        return readIDEL(a);
    uae_u32 v = ps_read_32(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
}
static uae_u32 ide_wget(uaecptr a)
{
    PROF_IDE_R(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
        return readIDE(a);
    uae_u32 v = ps_read_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return v;
}
static uae_u32 ide_bget(uaecptr a)
{
    PROF_IDE_R(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
        return readIDEB(a);
    uae_u32 v = ps_read_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return v;
}

static void ide_lput(uaecptr a, uae_u32 v)
{
    PROF_IDE_W(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
    {
        writeIDEL(a, v);
        return;
    }
    ps_write_32(a, v);
    pistorm_buserr(a, v, false, sz_long);
}
static void ide_wput(uaecptr a, uae_u32 v)
{
    PROF_IDE_W(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
    {
        writeIDE(a, (uint16_t)v);
        return;
    }
    ps_write_16(a, (uint16_t)v);
    pistorm_buserr(a, v, false, sz_word);
}
static void ide_bput(uaecptr a, uae_u32 v)
{
    PROF_IDE_W(a);
    g_buserr = 0;
    a = ide_fold(a);
    if (ide_in_regs(a))
    {
        writeIDEB(a, (uint8_t)v);
        return;
    }
    ps_write_8(a, (uint8_t)v);
    pistorm_buserr(a, v, false, sz_byte);
}

static int ide_check(uaecptr a, uae_u32 sz) { return 0; }     /* indirect: no direct host ptr */
static uae_u8 *ide_xlate(uaecptr a) { return natmem_offset; } /* never used (check==0) */

static addrbank pistorm_ide_bank = {
    ide_lget, ide_wget, ide_bget,
    ide_lput, ide_wput, ide_bput,
    ide_xlate, ide_check, NULL, "IDE", "IDE",
    ide_lget, ide_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    /* jit_read_flag, jit_write_flag: force handler calls, no direct access */
    S_READ, S_WRITE};


/* ================================================================== */
/* FDD I/F bank — handler-routed                                      */
/* ================================================================== */

//static inline uae_u32 fdd_fold(uaecptr a) { return a & 0x00FFFFFFu; }
//static inline int fdd_in_regs(uae_u32 a) { return a >= IDE_REG_LO && a < IDE_REG_HI; }

/* Reads: emulated controller for the register window, real bus for the rest of
 * the page (matches what the dummy/m68k_read_memory path used to do). */
static uae_u32 fdd_lget(uaecptr a)
{
    PROF_FDD_R(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
   // printf ("fdd_lget 0x%X\n", a);
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io rd32 0x%X\n", a);
        return fdd_io_read (a, 4);
    }
    uint32_t v = ps_read_32 (a);
    pistorm_buserr (a, 0, true, sz_long);
    return v;
}
static uae_u32 fdd_wget(uaecptr a)
{
    PROF_FDD_R(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
   // printf ("fdd_wget 0x%X\n", a);
    if (FDD_enabled && a == MFP_GPIP) { printf ("fdd gpip rd16\n");
        uint8_t gpip = fdd_gpip (ps_read_8 (a));
        return gpip;
    }
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io rd16 0x%X\n", a);
        return (uint16_t)fdd_io_read (a, 2);
    }
    uint16_t v = ps_read_16 (a);
    pistorm_buserr (a, 0, true, sz_word);
    return v;
}
static uae_u32 fdd_bget(uaecptr a)
{
    PROF_FDD_R(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
   // printf ("fdd_bget 0x%X\n", a);
    if (FDD_enabled && a == MFP_GPIP) { printf ("fdd gpip rd8\n");
        uint8_t gpip = fdd_gpip (ps_read_8 (a));
        pistorm_buserr (a, 0, true, sz_byte);
        return gpip;
    }
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io rd8 0x%X\n", a);
        return (uint8_t)fdd_io_read (a, 1);
    }
    uint8_t v = ps_read_8 (a);
    pistorm_buserr (a, 0, true, sz_byte);
    return v;
}

static void fdd_lput(uaecptr a, uae_u32 v)
{
    PROF_FDD_W(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
    //printf ("fdd_lput 0x%X\n", a);
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io wr32 0x%X\n", a);
        fdd_io_write (a, v, 4);
        return;
    }
    ps_write_32 (a, v);
    pistorm_buserr (a, v, false, sz_long);
}
static void fdd_wput(uaecptr a, uae_u32 v)
{
    PROF_FDD_W(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
   // printf ("fdd_wput 0x%X\n", a);
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io wr16 0x%X\n", a);
        fdd_io_write (a, v, 2);
        return;
    }
    ps_write_16 (a, (uint16_t)v);
    pistorm_buserr (a, v, false, sz_word);
}
static void fdd_bput(uaecptr a, uae_u32 v)
{
    PROF_FDD_W(a);
    g_buserr = 0;
    a &= 0x00FFFFFF;
    //printf ("fdd_bput 0x%X\n", a);
    if (FDD_enabled && fdd_owns_address (a)) { printf ("fdd io wr8 0x%X\n", a);
        fdd_io_write (a, v, 1);
        return;
    }
    ps_write_8 (a, (uint8_t)v);
    pistorm_buserr (a, v, false, sz_byte);
}

static int fdd_check (uaecptr a, uae_u32 sz) { return FDD_enabled && ((a >= 0xFF8600u && a <= 0xFF860Fu) || (a >= 0xFF8800u && a <= 0xFF8803u) || a == MFP_GPIP); }     /* indirect: no direct host ptr */
static uae_u8 *fdd_xlate (uaecptr a) { return natmem_offset + a; } /* never used (check==0) */

static addrbank pistorm_fdd_bank = {
    fdd_lget, fdd_wget, fdd_bget,
    fdd_lput, fdd_wput, fdd_bput,
    fdd_xlate, fdd_check, NULL, "FDD", "FDD",
    fdd_lget, fdd_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    /* jit_read_flag, jit_write_flag: force handler calls, no direct access */
    S_READ, S_WRITE};


/* ================================================================== */
/* Atari hardware I/O bank — one 64KB bank, narrow device dispatch       */
/* ================================================================== */

#define FPU_REG_BASE 0x00FFFA40u
#define FPU_REG_TOP  0x00FFFA60u
#define NOVA_IO_ALIAS_BASE 0x00FF83B0u
#define NOVA_IO_ALIAS_TOP  0x00FF83E0u
#define VGA_PORT_ALIAS_BASE 0x000003B0u
#define HW_PAGE_VIDEO       0x00FF8200u
#define HW_PAGE_FDD_DMA     0x00FF8600u
#define HW_PAGE_PSG         0x00FF8800u
#define HW_PAGE_DMASND      0x00FF8900u
#define HW_PAGE_BLITTER_LO  0x00FF8A00u
#define HW_PAGE_BLITTER_HI  0x00FF8B00u
#define HW_PAGE_MFP         0x00FFFA00u
#define HW_PAGE_ACIA        0x00FFFC00u

static inline uaecptr hw_fold_addr(uaecptr a)
{
    return a & 0x00FFFFFFu;
}

static inline uaecptr hw_page_addr(uaecptr a)
{
    return a & 0x00FFFF00u;
}

static inline int fpu_in_regs(uaecptr a)
{
    uint32_t folded = hw_fold_addr(a);
    return folded >= FPU_REG_BASE && folded < FPU_REG_TOP;
}

static inline int nova_io_alias_addr(uaecptr a)
{
    uint32_t folded = hw_fold_addr(a);
    return emulator_config_et4k_enabled() && et4k_addr_ptr &&
           folded >= NOVA_IO_ALIAS_BASE && folded < NOVA_IO_ALIAS_TOP;
}

static inline uint32_t nova_io_alias_card_addr(uaecptr a)
{
    uint32_t folded = hw_fold_addr(a);
    return et4k_addr_ptr->io_base + VGA_PORT_ALIAS_BASE + (folded - NOVA_IO_ALIAS_BASE);
}

static inline int hw_mfp_addr(uaecptr a)
{
    return a >= HW_PAGE_MFP && a < FPU_REG_BASE;
}

static inline uae_u32 hw_bus_lget(uaecptr a)
{
    uae_u32 v = ps_bus_lget(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
}

static inline uae_u32 hw_bus_wget(uaecptr a)
{
    uae_u16 v = ps_read_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return v;
}

static inline uae_u32 hw_bus_bget(uaecptr a)
{
    uae_u8 v = ps_read_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return v;
}

static inline void hw_bus_lput(uaecptr a, uae_u32 v)
{
    ps_bus_lput(a, v);
    pistorm_buserr(a, v, false, sz_long);
}

static inline void hw_bus_wput(uaecptr a, uae_u32 v)
{
    ps_write_16(a, (uae_u16)v);
    pistorm_buserr(a, v, false, sz_word);
}

static inline void hw_bus_bput(uaecptr a, uae_u32 v)
{
    ps_write_8(a, (uae_u8)v);
    pistorm_buserr(a, v, false, sz_byte);
}

static inline uae_u32 hw_blitter_lget(uaecptr a)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT semantics: raise a guest bus error exactly like a
         * machine with no blitter, so TOS/EmuTOS's presence probe fails and
         * it uses software rendering. Returning a value here (old code) made
         * the probe detect a phantom blitter whose writes were swallowed -
         * desktop with no menus. */
        blitter_trace("H", a, 0, 4);
        hardware_exception2(a, 0, true, false, sz_long);
        return 0xFFFFFFFFu; /* not reached */
    }

    uae_u32 v = hw_bus_lget(a);
    blitter_trace("R", a, v, 4);
    return v;
}

static inline uae_u32 hw_blitter_wget(uaecptr a)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("H", a, 0, 2);
        hardware_exception2(a, 0, true, false, sz_word);
        return 0xFFFFu; /* not reached */
    }

    uae_u32 v = hw_bus_wget(a);
    blitter_trace("R", a, v, 2);
    return v;
}

static inline uae_u32 hw_blitter_bget(uaecptr a)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("H", a, 0, 1);
        hardware_exception2(a, 0, true, false, sz_byte);
        return 0xFFu; /* not reached */
    }

    uae_u32 v = hw_bus_bget(a);
    blitter_trace("R", a, v, 1);
    return v;
}

static inline void hw_blitter_lput(uaecptr a, uae_u32 v)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 4);
        hardware_exception2(a, v, false, false, sz_long);
        return; /* not reached */
    }

    hw_bus_lput(a, v);
    blitter_trace("W", a, v, 4);
    blitter_after_write(a, v, 4);
}

static inline void hw_blitter_wput(uaecptr a, uae_u32 v)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 2);
        hardware_exception2(a, v, false, false, sz_word);
        return; /* not reached */
    }

    hw_bus_wput(a, v);
    blitter_trace("W", a, v, 2);
    blitter_after_write(a, v, 2);
}

static inline void hw_blitter_bput(uaecptr a, uae_u32 v)
{
    if (blitter_hide_addr(a))
    {
        /* Blitter ABSENT: guest bus error, see lget comment. */
        blitter_trace("h", a, v, 1);
        hardware_exception2(a, v, false, false, sz_byte);
        return; /* not reached */
    }

    hw_bus_bput(a, v);
    blitter_trace("W", a, v, 1);
    blitter_after_write(a, v, 1);
}

static inline uae_u32 hw_mfp_wget(uaecptr a)
{
    if (FDD_enabled && a == MFP_GPIP)
    {
        uae_u16 v = fdd_gpip(ps_read_8(a));
        pistorm_buserr(a, 0, true, sz_word);
        return v;
    }
    return hw_bus_wget(a);
}

static inline uae_u32 hw_mfp_bget(uaecptr a)
{
    if (FDD_enabled && a == MFP_GPIP)
    {
        uae_u8 v = fdd_gpip(ps_read_8(a));
        pistorm_buserr(a, 0, true, sz_byte);
        return v;
    }
    return hw_bus_bget(a);
}

static inline void hw_mfp_lput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 4);
        return;
    }
    hw_bus_lput(a, v);
}

static inline void hw_mfp_wput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 2);
        return;
    }
    mfp_note_eoi_write(a, v, true);
    hw_bus_wput(a, v);
}

static inline void hw_mfp_bput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 1);
        return;
    }
    mfp_note_eoi_write(a, v, false);
    hw_bus_bput(a, v);
}

static inline uae_u32 hw_fdd_lget(uaecptr a)
{
    if (FDD_enabled && fdd_owns_address(a))
        return fdd_io_read(a, 4);
    return hw_bus_lget(a);
}

static inline uae_u32 hw_fdd_wget(uaecptr a)
{
    if (FDD_enabled && fdd_owns_address(a))
        return (uae_u16)fdd_io_read(a, 2);
    return hw_bus_wget(a);
}

static inline uae_u32 hw_fdd_bget(uaecptr a)
{
    if (FDD_enabled && fdd_owns_address(a))
        return (uae_u8)fdd_io_read(a, 1);
    return hw_bus_bget(a);
}

static inline void hw_fdd_lput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 4);
        return;
    }
    hw_bus_lput(a, v);
}

static inline void hw_fdd_wput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 2);
        return;
    }
    hw_bus_wput(a, v);
}

static inline void hw_fdd_bput(uaecptr a, uae_u32 v)
{
    if (FDD_enabled && fdd_owns_address(a))
    {
        fdd_io_write(a, v, 1);
        return;
    }
    hw_bus_bput(a, v);
}

static uae_u32 hw_lget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    return io_lget(a);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a) || nova_io_alias_addr(a))
        return 0;

    switch (hw_page_addr(a))
    {
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            return hw_fdd_lget(a);
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            return hw_blitter_lget(a);
        case HW_PAGE_ACIA:
        {
            uae_u32 v = hw_bus_lget(a);
            acia_trace("R", a, v, 4);
            return v;
        }
        default:
            return hw_bus_lget(a);
    }
#endif
}

static uae_u32 hw_wget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    return io_wget(a);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a))
        return 0;
    if (nova_io_alias_addr(a))
        return et4000_io_read8(g_et4000, nova_io_alias_card_addr(a));
    if (hw_mfp_addr(a))
        return hw_mfp_wget(a);

    switch (hw_page_addr(a))
    {
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            return hw_fdd_wget(a);
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            return hw_blitter_wget(a);
        case HW_PAGE_ACIA:
        {
            uae_u32 v = hw_bus_wget(a);
            acia_trace("R", a, v, 2);
            return v;
        }
        default:
            return hw_bus_wget(a);
    }
#endif
}

static uae_u32 hw_bget(uaecptr a)
{
    PROF_IO_R(a);
#if PISTORM_LEGACY_MEM_HOOKS
    return io_bget(a);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a))
        return 0;
    if (nova_io_alias_addr(a))
        return et4000_io_read8(g_et4000, nova_io_alias_card_addr(a));
    if (hw_mfp_addr(a))
        return hw_mfp_bget(a);

    switch (hw_page_addr(a))
    {
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            return hw_fdd_bget(a);
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            return hw_blitter_bget(a);
        case HW_PAGE_ACIA:
        {
            uae_u32 v = hw_bus_bget(a);
            acia_trace("R", a, v, 1);
            return v;
        }
        default:
            return hw_bus_bget(a);
    }
#endif
}

static void hw_lput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    io_lput(a, v);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a) || nova_io_alias_addr(a))
        return;

    switch (hw_page_addr(a))
    {
        case HW_PAGE_VIDEO:
            st_video_snoop32(a, (uint32_t)v);
            hw_bus_lput(a, v);
            break;
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            hw_fdd_lput(a, v);
            break;
        case HW_PAGE_DMASND:
            dmasnd_snoop32(a, (uint32_t)v);
            hw_bus_lput(a, v);
            break;
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            hw_blitter_lput(a, v);
            break;
        case HW_PAGE_MFP:
            if (hw_mfp_addr(a))
                hw_mfp_lput(a, v);
            else
                hw_bus_lput(a, v);
            break;
        case HW_PAGE_ACIA:
            hw_bus_lput(a, v);
            acia_trace("W", a, v, 4);
            break;
        default:
            hw_bus_lput(a, v);
            break;
    }
#endif
}

static void hw_wput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    io_wput(a, v);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a))
        return;
    if (nova_io_alias_addr(a)) {
        et4000_io_write8(g_et4000, nova_io_alias_card_addr(a), (uae_u8)v);
        return;
    }

    switch (hw_page_addr(a))
    {
        case HW_PAGE_VIDEO:
            st_video_snoop16(a, (uint16_t)v);
            hw_bus_wput(a, v);
            break;
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            hw_fdd_wput(a, v);
            break;
        case HW_PAGE_DMASND:
            dmasnd_snoop16(a, (uint16_t)v);
            hw_bus_wput(a, v);
            break;
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            hw_blitter_wput(a, v);
            break;
        case HW_PAGE_MFP:
            if (hw_mfp_addr(a))
                hw_mfp_wput(a, v);
            else
                hw_bus_wput(a, v);
            break;
        case HW_PAGE_ACIA:
            hw_bus_wput(a, v);
            acia_trace("W", a, v, 2);
            break;
        default:
            hw_bus_wput(a, v);
            break;
    }
#endif
}

static void hw_bput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
#if PISTORM_LEGACY_MEM_HOOKS
    io_bput(a, v);
#else
    g_buserr = 0;
    fc_data();
    a = hw_fold_addr(a);

    if (fpu_in_regs(a))
        return;
    if (nova_io_alias_addr(a)) {
        et4000_io_write8(g_et4000, nova_io_alias_card_addr(a), (uae_u8)v);
        return;
    }

    switch (hw_page_addr(a))
    {
        case HW_PAGE_VIDEO:
            st_video_snoop8(a, (uint8_t)v);
            hw_bus_bput(a, v);
            break;
        case HW_PAGE_FDD_DMA:
        case HW_PAGE_PSG:
            hw_fdd_bput(a, v);
            break;
        case HW_PAGE_DMASND:
            dmasnd_snoop8(a, (uint8_t)v);
            hw_bus_bput(a, v);
            break;
        case HW_PAGE_BLITTER_LO:
        case HW_PAGE_BLITTER_HI:
            hw_blitter_bput(a, v);
            break;
        case HW_PAGE_MFP:
            if (hw_mfp_addr(a))
                hw_mfp_bput(a, v);
            else
                hw_bus_bput(a, v);
            break;
        case HW_PAGE_ACIA:
            hw_bus_bput(a, v);
            acia_trace("W", a, v, 1);
            break;
        default:
            hw_bus_bput(a, v);
            break;
    }
#endif
}

static int hw_check(uaecptr a, uae_u32 sz) { return 0; }
static uae_u8 *hw_xlate(uaecptr a) { return natmem_offset + a; }

static addrbank pistorm_hw_bank = {
    hw_lget, hw_wget, hw_bget,
    hw_lput, hw_wput, hw_bput,
    hw_xlate, hw_check, NULL, "pistorm HW I/O", "pistorm HW I/O",
    hw_lget, hw_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ, S_WRITE};


/* ====================================================================== *
 * LOW-RAM bank — paste into pistorm_natmem.cpp next to pistorm_stram_bank.
 * --------------------------------------------------------------------- *
 * First 64KB of ST-RAM = exception vectors (0x000-0x3FF) + TOS/OS system
 * variables. Match the working Musashi model: reads come from the mirror,
 * writes update the mirror and write through to the real bus.
 * ====================================================================== */
static uae_u32 lo_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 lo_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 lo_bget(uaecptr a) { return natmem_offset[a]; }
#if (0)
static void lo_lput(uaecptr a, uae_u32 v)
{
    do_put_mem_long((uae_u32 *)(natmem_offset + a), v);
    if (a >= 8)
    {
        g_buserr = 0;
        fc_data();
        ps_write_32(a & 0x00FFFFFF, v);
        pistorm_buserr(a, v, false, sz_long);
    }
    pistorm_smc(a, 4);
}
static void lo_wput(uaecptr a, uae_u32 v)
{
    do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v);
    if (a >= 8)
    {
        g_buserr = 0;
        fc_data();
        ps_write_16(a & 0x00FFFFFF, (uint16_t)v);
        pistorm_buserr(a, v, false, sz_word);
    }
    pistorm_smc(a, 2);
}
static void lo_bput(uaecptr a, uae_u32 v)
{
    natmem_offset[a] = (uae_u8)v;
    if (a >= 8)
    {
        g_buserr = 0;
        fc_data();
        ps_write_8(a & 0x00FFFFFF, (uint8_t)v);
        pistorm_buserr(a, v, false, sz_byte);
    }
    pistorm_smc(a, 1);
}
#else
static void lo_lput(uaecptr a, uae_u32 v)
{
    do_put_mem_long((uae_u32 *)(natmem_offset + a), v);
    pistorm_smc(a, 4);
}
static void lo_wput(uaecptr a, uae_u32 v)
{
    do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v);
    pistorm_smc(a, 2);
}
static void lo_bput(uaecptr a, uae_u32 v)
{
    natmem_offset[a] = (uae_u8)v;
    pistorm_smc(a, 1);
}
#endif
static uae_u8 *lo_xlate(uaecptr a) { return natmem_offset + a; }
static int lo_check(uaecptr a, uae_u32 sz) { return 1; }

static addrbank pistorm_lowram_bank = {
    lo_lget, lo_wget, lo_bget,
    lo_lput, lo_wput, lo_bput,
    lo_xlate, lo_check, NULL, "ST-RAM-LOW", "ST-RAM-LOW",
    lo_lget, lo_wget,
    ABFLAG_RAM | ABFLAG_DIRECTACCESS,
    0, 0 /* force JIT handler access for TOS system variables */
};

/* ---- in jit_mem_init(), in the baseaddr block (next to the others): ----
 *
 *     pistorm_lowram_bank.baseaddr = natmem_offset + 0;
 *
 * ---- and in jit_mem_init(), IMMEDIATELY AFTER the ST-RAM map_region: ----
 *
 *     map_region(0, ST_RAM_SIZE, &pistorm_stram_bank);
 *     map_region(0, 0x10000,     &pistorm_lowram_bank);   // overlay low 64KB: mirror-only
 *
 * The low overlay must come AFTER the ST-RAM map so it wins for 0x0-0xFFFF.
 *
 * TUNING: 0x10000 captures 99% of the write-through per the profile. You can
 * raise it (e.g. 0x80000) once you confirm nothing in that span is a live
 * framebuffer or DMA target — re-run the profiler's write-page list and check
 * no high-traffic page sits inside the range you make mirror-only.
 * ---------------------------------------------------------------------- */

/* ================================================================== */
/* ST-RAM bank — read direct from mirror, optional write-through + SMC  */
/* ================================================================== */

static uae_u32 sr_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 sr_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 sr_bget(uaecptr a) { return natmem_offset[a]; }

/*
static uae_u32 sr_lget(uaecptr a){ fc_data(); a &= 0x00FFFFFF; uae_u32 v=ps_read_32(a); pistorm_buserr(a,0,true,sz_long); return v; }
static uae_u32 sr_wget(uaecptr a){ fc_data(); a &= 0x00FFFFFF; uae_u32 v=ps_read_16(a); pistorm_buserr(a,0,true,sz_long); return v; }
static uae_u32 sr_bget(uaecptr a){ fc_data(); a &= 0x00FFFFFF; uae_u32 v=ps_read_8(a); pistorm_buserr(a,0,true,sz_long); return v;}
*/
static void sr_lput(uaecptr a, uae_u32 v)
{
    PROF_STRAM_W(a);
    g_buserr = 0;
    do_put_mem_long((uae_u32 *)(natmem_offset + a), v); // update mirror (BE)
    stram_snoop_lowram(a, 4);
    if (stram_needs_bus_write(a, 4)) {
        uae_u32 bus_a = a & 0x00FFFFFF;
        fc_data();
        ps_write_32(bus_a, v); // write-through to bus
    }
    pistorm_smc(a, 4);
}
static void sr_wput(uaecptr a, uae_u32 v)
{
    PROF_STRAM_W(a);
    g_buserr = 0;
    do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v);
    stram_snoop_lowram(a, 2);
    if (stram_needs_bus_write(a, 2)) {
        uae_u32 bus_a = a & 0x00FFFFFF;
        fc_data();
        ps_write_16(bus_a, (uint16_t)v);
    }
    pistorm_smc(a, 2);
}
static void sr_bput(uaecptr a, uae_u32 v)
{
    PROF_STRAM_W(a);
    g_buserr = 0;
    natmem_offset[a] = (uae_u8)v;
    stram_snoop_lowram(a, 1);
    if (stram_needs_bus_write(a, 1)) {
        uae_u32 bus_a = a & 0x00FFFFFF;
        fc_data();
        ps_write_8(bus_a, (uint8_t)v);
    }
    pistorm_smc(a, 1);
}
static uae_u8 *sr_xlate(uaecptr a) { return natmem_offset + a; }
static int sr_check(uaecptr a, uae_u32 sz) { return 1; } // return a < ST_RAM_SIZE; }

static addrbank pistorm_stram_bank = {
    sr_lget,
    sr_wget,
    sr_bget,
    sr_lput,
    sr_wput,
    sr_bput,
    sr_xlate,
    sr_check,
    NULL,
    "ST-RAM",
    "ST-RAM",
    sr_lget,
    sr_wget, // opcode fetch = direct read
    ABFLAG_RAM | ABFLAG_DIRECTACCESS,
    0,
    S_WRITE, // reads direct, writes special
};

/* ROM bank — direct read, writes ignored */
static uae_u32 rom_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 rom_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 rom_bget(uaecptr a) { return natmem_offset[a]; }
static void rom_nop_put(uaecptr, uae_u32) {}
static uae_u8 *rom_xlate(uaecptr a) { return natmem_offset + a; }
static int rom_check(uaecptr a, uae_u32 sz) { return 1; } // return (a >= ROM_BASE && a < ROM_TOP); }

static addrbank pistorm_rom_bank = {
    rom_lget, rom_wget, rom_bget,
    rom_nop_put, rom_nop_put, rom_nop_put,
    rom_xlate, rom_check, NULL, "TOS ROM", "TOS ROM",
    rom_lget, rom_wget,
    ABFLAG_ROM | ABFLAG_DIRECTACCESS | ABFLAG_CACHE_ENABLE_ALL,
    0, S_WRITE // fully direct, no special
};

/* TT-RAM bank — pure host, direct R/W, native (mprotect) SMC like stock fast RAM */
static uae_u32 tt_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 tt_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 tt_bget(uaecptr a) { return natmem_offset[a]; }
static void tt_lput(uaecptr a, uae_u32 v) { do_put_mem_long((uae_u32 *)(natmem_offset + a), v); }
static void tt_wput(uaecptr a, uae_u32 v) { do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v); }
static void tt_bput(uaecptr a, uae_u32 v) { natmem_offset[a] = (uae_u8)v; }
static uae_u8 *tt_xlate(uaecptr a) { return natmem_offset + a; }
static int tt_check(uaecptr a, uae_u32 sz) { return (a >= TT_RAM_BASE) && a < (TT_RAM_BASE + tt_ram_size); }

static addrbank pistorm_ttram_bank = {
    tt_lget, tt_wget, tt_bget,
    tt_lput, tt_wput, tt_bput,
    tt_xlate, tt_check, NULL, "TT-RAM", "TT-RAM",
    tt_lget, tt_wget,
    ABFLAG_RAM | ABFLAG_DIRECTACCESS | ABFLAG_CACHE_ENABLE_ALL,
    0, 0};

extern "C" uint8_t *et4000_engine_vram_ptr(void);

static uint8_t *pistorm_fvdi_fb;
static uint32_t pistorm_fvdi_mode_width = 640;
static uint32_t pistorm_fvdi_mode_height = 480;
static uint32_t pistorm_fvdi_mode_bpp = 32;
static int pistorm_fvdi_active;
static volatile uint64_t pistorm_fvdi_write_count_state;
static volatile uint64_t pistorm_fvdi_write_bytes_state;
static uint32_t pistorm_fvdi_first_write_state = 0xffffffffu;
static uint32_t pistorm_fvdi_last_write_state;

extern "C" uint32_t pistorm_fvdi_fb_base(void)
{
    return FVDI_FB_BASE;
}

extern "C" uint32_t pistorm_fvdi_fb_size(void)
{
    return FVDI_FB_MAX_BYTES;
}

extern "C" uint8_t *pistorm_fvdi_fb_ptr(void)
{
    return pistorm_fvdi_fb;
}

extern "C" uint32_t pistorm_fvdi_width(void)
{
    return pistorm_fvdi_mode_width;
}

extern "C" uint32_t pistorm_fvdi_height(void)
{
    return pistorm_fvdi_mode_height;
}

extern "C" uint32_t pistorm_fvdi_bpp(void)
{
    return pistorm_fvdi_mode_bpp;
}

extern "C" int pistorm_fvdi_is_active(void)
{
    return pistorm_fvdi_active;
}

extern "C" uint64_t pistorm_fvdi_write_count(void)
{
    return pistorm_fvdi_write_count_state;
}

extern "C" uint64_t pistorm_fvdi_write_bytes(void)
{
    return pistorm_fvdi_write_bytes_state;
}

extern "C" uint32_t pistorm_fvdi_first_write(void)
{
    return pistorm_fvdi_first_write_state;
}

extern "C" uint32_t pistorm_fvdi_last_write(void)
{
    return pistorm_fvdi_last_write_state;
}

extern "C" int pistorm_fvdi_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    uint64_t bytes;

    if (!pistorm_fvdi_fb || width == 0 || height == 0)
        return 0;
    if (bpp != 16 && bpp != 32)
        return 0;

    bytes = (uint64_t)width * height * (bpp / 8);
    if (bytes == 0 || bytes > FVDI_FB_MAX_BYTES)
        return 0;

    pistorm_fvdi_mode_width = width;
    pistorm_fvdi_mode_height = height;
    pistorm_fvdi_mode_bpp = bpp;
    pistorm_fvdi_active = 1;
    pistorm_fvdi_write_count_state = 0;
    pistorm_fvdi_write_bytes_state = 0;
    pistorm_fvdi_first_write_state = 0xffffffffu;
    pistorm_fvdi_last_write_state = 0;
    memset(pistorm_fvdi_fb, 0, (size_t)bytes);
    return 1;
}

/* Dirty byte extent since the render thread last fetched it. Single writer
 * (CPU thread), single reader (render thread, fetch-and-reset). A torn
 * min/max pair across the two exchanges is detected by the reader
 * (max <= min) and answered with a full-frame render, so no update can be
 * lost - at worst one frame renders more rows than needed. */
static uint32_t pistorm_fvdi_dirty_min = 0xffffffffu;
static uint32_t pistorm_fvdi_dirty_max = 0;

static inline void fvdi_note_write(uint32_t o, uint32_t bytes)
{
    pistorm_fvdi_write_count_state++;
    pistorm_fvdi_write_bytes_state += bytes;
    if (o < pistorm_fvdi_first_write_state)
        pistorm_fvdi_first_write_state = o;
    pistorm_fvdi_last_write_state = o;
    if (o < pistorm_fvdi_dirty_min)
        pistorm_fvdi_dirty_min = o;
    if (o + bytes > pistorm_fvdi_dirty_max)
        pistorm_fvdi_dirty_max = o + bytes;
}

extern "C" void pistorm_fvdi_fetch_dirty(uint32_t *mn, uint32_t *mx)
{
    *mn = __atomic_exchange_n(&pistorm_fvdi_dirty_min, 0xffffffffu, __ATOMIC_ACQ_REL);
    *mx = __atomic_exchange_n(&pistorm_fvdi_dirty_max, 0u, __ATOMIC_ACQ_REL);
}

extern "C" void pistorm_fvdi_note_host_write(uint32_t o, uint32_t bytes)
{
    fvdi_note_write(o, bytes);
}

static inline uint32_t fvdi_off(uaecptr a)
{
    return (uint32_t)(a - FVDI_FB_BASE);
}

static uae_u32 fvdi_lget(uaecptr a)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o + 3 >= FVDI_FB_MAX_BYTES)
        return 0;
    return ((uae_u32)pistorm_fvdi_fb[o] << 24) |
           ((uae_u32)pistorm_fvdi_fb[o + 1] << 16) |
           ((uae_u32)pistorm_fvdi_fb[o + 2] << 8) |
           pistorm_fvdi_fb[o + 3];
}

static uae_u32 fvdi_wget(uaecptr a)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o + 1 >= FVDI_FB_MAX_BYTES)
        return 0;
    return ((uae_u32)pistorm_fvdi_fb[o] << 8) | pistorm_fvdi_fb[o + 1];
}

static uae_u32 fvdi_bget(uaecptr a)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o >= FVDI_FB_MAX_BYTES)
        return 0;
    return pistorm_fvdi_fb[o];
}

static void fvdi_lput(uaecptr a, uae_u32 v)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o + 3 >= FVDI_FB_MAX_BYTES)
        return;
    pistorm_fvdi_fb[o] = (uae_u8)(v >> 24);
    pistorm_fvdi_fb[o + 1] = (uae_u8)(v >> 16);
    pistorm_fvdi_fb[o + 2] = (uae_u8)(v >> 8);
    pistorm_fvdi_fb[o + 3] = (uae_u8)v;
    fvdi_note_write(o, 4);
}

static void fvdi_wput(uaecptr a, uae_u32 v)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o + 1 >= FVDI_FB_MAX_BYTES)
        return;
    pistorm_fvdi_fb[o] = (uae_u8)(v >> 8);
    pistorm_fvdi_fb[o + 1] = (uae_u8)v;
    fvdi_note_write(o, 2);
}

static void fvdi_bput(uaecptr a, uae_u32 v)
{
    uint32_t o = fvdi_off(a);
    if (!pistorm_fvdi_fb || o >= FVDI_FB_MAX_BYTES)
        return;
    pistorm_fvdi_fb[o] = (uae_u8)v;
    fvdi_note_write(o, 1);
}

static uae_u8 *fvdi_xlate(uaecptr a)
{
    return pistorm_fvdi_fb ? pistorm_fvdi_fb + fvdi_off(a) : NULL;
}

static int fvdi_check(uaecptr a, uae_u32 sz)
{
    return pistorm_fvdi_fb &&
           a >= FVDI_FB_BASE &&
           sz <= FVDI_FB_MAX_BYTES &&
           a - FVDI_FB_BASE <= FVDI_FB_MAX_BYTES - sz;
}

static addrbank pistorm_fvdi_bank = {
    fvdi_lget, fvdi_wget, fvdi_bget,
    fvdi_lput, fvdi_wput, fvdi_bput,
    fvdi_xlate, fvdi_check, NULL, "fVDI-FB", "fVDI-FB",
    fvdi_lget, fvdi_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ, S_WRITE};

#if (1)
/* ================================================================== */
/* FB-GUARD bank — Amiga-faithful scratch just below the framebuffer.   */
/* On Amiga the RTG framebuffer is plain mapped RAM and never faults.    */
/* The Atari map leaves a hole below the card, so an off-screen cursor or       */
/* window save/restore that under-runs the framebuffer base used to bus-error   */
/* (e.g. 0xBFFFFE, one word below the NOVA 0xC00000 aperture). Back the 1MB      */
/* immediately below the ACTIVE aperture with pure host RAM so those accesses    */
/* are absorbed harmlessly instead of NAKing. The guard sits well above the      */
/* 0x400000 ST-RAM top (0xB00000 for NOVA, 0x900000 for XVDI), so EmuTOS memory  */
/* EmuTOS memory sizing (which stops at the first BERR at 0x400000) is   */
/* unaffected. Pure direct R/W: never reaches the bus, so g_buserr never */
/* fires here and the JIT bus-error recovery path is never entered.      */
/* ================================================================== */
/* GUARD_BASE is derived at run time from the active aperture. This models the
 * scratch/save-under area some Atari SVGA drivers use immediately below the
 * framebuffer aperture. It is intentionally host scratch RAM, not visible VRAM.
 * Observed XVDI access: 0x009DFFFC, four bytes below a 128K guard. */
#define GUARD_SIZE (0x00040000u)
#define GUARD_BASE (et4k_addr_ptr->vram_base - GUARD_SIZE)

static inline uae_u8 gd_read_byte(uaecptr a)
{
    if (a >= et4k_addr_ptr->vram_base && a < et4k_addr_ptr->vram_base + VRAM_SIZE)
        return et4000_engine_vram_read8(a);
    return natmem_offset[a];
}

static inline void gd_write_byte(uaecptr a, uae_u8 v)
{
    if (a >= et4k_addr_ptr->vram_base && a < et4k_addr_ptr->vram_base + VRAM_SIZE) {
        et4000_engine_vram_write8(a, v);
        return;
    }
    natmem_offset[a] = v;
}

static uae_u32 gd_lget(uaecptr a)
{
    return ((uae_u32)gd_read_byte(a) << 24) |
           ((uae_u32)gd_read_byte(a + 1) << 16) |
           ((uae_u32)gd_read_byte(a + 2) << 8) |
           gd_read_byte(a + 3);
}

static uae_u32 gd_wget(uaecptr a)
{
    return ((uae_u32)gd_read_byte(a) << 8) | gd_read_byte(a + 1);
}

static uae_u32 gd_bget(uaecptr a) { return gd_read_byte(a); }

static void gd_lput(uaecptr a, uae_u32 v)
{
    gd_write_byte(a, (uae_u8)(v >> 24));
    gd_write_byte(a + 1, (uae_u8)(v >> 16));
    gd_write_byte(a + 2, (uae_u8)(v >> 8));
    gd_write_byte(a + 3, (uae_u8)v);
}

static void gd_wput(uaecptr a, uae_u32 v)
{
    gd_write_byte(a, (uae_u8)(v >> 8));
    gd_write_byte(a + 1, (uae_u8)v);
}

static void gd_bput(uaecptr a, uae_u32 v) { gd_write_byte(a, (uae_u8)v); }

static uae_u8 *gd_xlate(uaecptr a) { return natmem_offset + a; }
static int gd_check(uaecptr a, uae_u32 sz) { return 0; }

static addrbank pistorm_guard_bank = {
    gd_lget, gd_wget, gd_bget,
    gd_lput, gd_wput, gd_bput,
    gd_xlate, gd_check, NULL, "FB-GUARD", "FB-GUARD",
    gd_lget, gd_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ, S_WRITE
};
#endif

/* VGA-VRAM bank — framebuffer style storage.
 *
 * ET4000 register I/O still goes through PCem, but CPU reads/writes to the
 * NOVA/XVDI VRAM aperture are just framebuffer bytes. Calling PCem's
 * svga_*_linear helpers for every byte/word/long adds a lot of hot-path work
 * and is unnecessary for the packed/linear SVGA modes GEM uses here.
 */
#define ET4K_VRAM_MASK 0x000FFFFFu

static inline uae_u8 *vga_vram_ptr(uaecptr a)
{
    return et4000_engine_vram_ptr() + (a & ET4K_VRAM_MASK);
}

static int vga_vram_mode(void)
{
    return 2;
}

static inline int vga_direct_ok(void)
{
    int mode = vga_vram_mode();
    if (mode == 2)
        return 0;
    if (!et4000_engine_vram_ptr())
        return 0;
    if (mode == 1)
        return 1;
    return et4000_engine_direct_vram_ok();
}

/*
static uae_u32 vga_lget(uaecptr a){ uae_u32 v=m68k_read_memory_32(a); return v; }
static uae_u32 vga_wget(uaecptr a){ uae_u32 v=m68k_read_memory_16(a); return v; }
static uae_u32 vga_bget(uaecptr a){ uae_u32 v=m68k_read_memory_8(a); return v; }
static void vga_lput(uaecptr a, uae_u32 v){ m68k_write_memory_32(a, v); }
static void vga_wput(uaecptr a, uae_u32 v){ m68k_write_memory_16(a, (uae_u16)v); }
static void vga_bput(uaecptr a, uae_u32 v){ m68k_write_memory_8(a, (uae_u8)v); }
*/
/*
static uae_u32 vga_lget(uaecptr a){ uae_u32 v=et4000_vram_read32(g_et4000, a); return v; }
static uae_u32 vga_wget(uaecptr a){ uae_u32 v=et4000_vram_read16(g_et4000, a); return v; }
static uae_u32 vga_bget(uaecptr a){ uae_u32 v=et4000_vram_read8(g_et4000, a); return v; }
static void vga_lput(uaecptr a, uae_u32 v){ et4000_vram_write32(g_et4000, a, v); }
static void vga_wput(uaecptr a, uae_u32 v){ et4000_vram_write16(g_et4000, a, (uae_u16)v); }
static void vga_bput(uaecptr a, uae_u32 v){ et4000_vram_write8(g_et4000, a, (uae_u8)v); }
*/

static uae_u32 vga_lget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v;
    if (!vga_direct_ok())
    {
        v = et4000_engine_vram_read32(a);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_ENGINE, vga_bank_profile_now_ns() - t0, 4);
        return v;
    }

    uae_u8 *p = vga_vram_ptr(a);
    v = ((uae_u32)p[0] << 24) | ((uae_u32)p[1] << 16) |
        ((uae_u32)p[2] << 8) | p[3];
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_DIRECT, vga_bank_profile_now_ns() - t0, 4);
    return v;
}

static uae_u32 vga_wget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v;
    if (!vga_direct_ok())
    {
        v = et4000_engine_vram_read16(a);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_ENGINE, vga_bank_profile_now_ns() - t0, 2);
        return v;
    }

    uae_u8 *p = vga_vram_ptr(a);
    v = ((uae_u32)p[0] << 8) | p[1];
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_DIRECT, vga_bank_profile_now_ns() - t0, 2);
    return v;
}

static uae_u32 vga_bget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v;
    if (!vga_direct_ok())
    {
        v = et4000_engine_vram_read8(a);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_ENGINE, vga_bank_profile_now_ns() - t0, 1);
        return v;
    }

    v = *vga_vram_ptr(a);
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_RD_DIRECT, vga_bank_profile_now_ns() - t0, 1);
    return v;
}

static void vga_lput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    if (!vga_direct_ok())
    {
        et4000_engine_vram_write32(a, v);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_ENGINE, vga_bank_profile_now_ns() - t0, 4);
        return;
    }

    uae_u8 *p = vga_vram_ptr(a);
    p[0] = (uae_u8)(v >> 24);
    p[1] = (uae_u8)(v >> 16);
    p[2] = (uae_u8)(v >> 8);
    p[3] = (uae_u8)v;
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_DIRECT, vga_bank_profile_now_ns() - t0, 4);
}

static void vga_wput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    if (!vga_direct_ok())
    {
        et4000_engine_vram_write16(a, (uae_u16)v);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_ENGINE, vga_bank_profile_now_ns() - t0, 2);
        return;
    }

    uae_u8 *p = vga_vram_ptr(a);
    p[0] = (uae_u8)(v >> 8);
    p[1] = (uae_u8)v;
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_DIRECT, vga_bank_profile_now_ns() - t0, 2);
}

static void vga_bput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    if (!vga_direct_ok())
    {
        et4000_engine_vram_write8(a, (uae_u8)v);
        if (t0)
            vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_ENGINE, vga_bank_profile_now_ns() - t0, 1);
        return;
    }

    *vga_vram_ptr(a) = (uae_u8)v;
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_VRAM_WR_DIRECT, vga_bank_profile_now_ns() - t0, 1);
}

// Keep ET4000 VRAM marked as a special bank, but still return a valid pointer:
// ARM64 get_n_addr_old() calls xlateaddr even for special memory. Returning
// NULL makes generated code dereference NULL +/- displacement.
static uae_u8 *vga_xlate(uaecptr a)
{
    return vga_vram_ptr(a);
}
// static uae_u8 *vga_xlate(uaecptr a){ return natmem_offset + a; }
static int vga_check(uaecptr a, uae_u32 sz)
{
    // return (a >= et4k_addr_ptr->vram_base && a < ((et4k_addr_ptr->vram_base + VRAM_SIZE) - sz));
    return 0;
}

static addrbank pistorm_vga_vram = {
    vga_lget, vga_wget, vga_bget,
    vga_lput, vga_wput, vga_bput,
    vga_xlate, vga_check, NULL, "VGA-VRAM", "VGA-VRAM",
    vga_lget, vga_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ, S_WRITE};

void et4000_engine_io_write(uint16_t port, uint8_t val);
uint8_t et4000_engine_io_read(uint16_t port);

static int vga_io_strict_width(void)
{
    return 0;
}

static uae_u32 vga_io_lget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v;

    if (vga_io_strict_width())
        v = ((uae_u32)et4000_io_read8(g_et4000, a) << 24) |
            ((uae_u32)et4000_io_read8(g_et4000, a + 1) << 16) |
            ((uae_u32)et4000_io_read8(g_et4000, a + 2) << 8) |
            et4000_io_read8(g_et4000, a + 3);
    else
        v = 0;

    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_RD, vga_bank_profile_now_ns() - t0, vga_io_strict_width() ? 4 : 0);
    return v;
}
static uae_u32 vga_io_wget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v;

    if (vga_io_strict_width())
        v = ((uae_u32)et4000_io_read8(g_et4000, a) << 8) |
            et4000_io_read8(g_et4000, a + 1);
    else
        v = et4000_io_read8(g_et4000, a);

    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_RD, vga_bank_profile_now_ns() - t0, vga_io_strict_width() ? 2 : 1);
    return v;
}
static uae_u32 vga_io_bget(uaecptr a)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    uae_u32 v = et4000_io_read8(g_et4000, a);
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_RD, vga_bank_profile_now_ns() - t0, 1);
    return v;
}
static void vga_io_lput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    if (vga_io_strict_width())
        et4000_io_write32(g_et4000, a, v);
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_WR, vga_bank_profile_now_ns() - t0, vga_io_strict_width() ? 4 : 0);
}
static void vga_io_wput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    if (vga_io_strict_width())
        et4000_io_write16(g_et4000, a, (uae_u16)v);
    else
        et4000_io_write8(g_et4000, a, (uae_u8)v);
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_WR, vga_bank_profile_now_ns() - t0, vga_io_strict_width() ? 2 : 1);
}
static void vga_io_bput(uaecptr a, uae_u32 v)
{
    uint64_t t0 = vga_bank_profile_enabled() ? vga_bank_profile_now_ns() : 0;
    et4000_io_write8(g_et4000, a, (uae_u8)v);
    if (t0)
        vga_bank_profile_add(VGA_BANK_PROF_IO_WR, vga_bank_profile_now_ns() - t0, 1);
}

/*
static uae_u32 vga_io_lget(uaecptr a){ uae_u32 v=et4000_engine_io_read (a); return v; }
static uae_u32 vga_io_wget(uaecptr a){ uae_u32 v=et4000_engine_io_read (a); return v; }
static uae_u32 vga_io_bget(uaecptr a){ uae_u32 v=et4000_engine_io_read (a); return v; }
static void vga_io_lput(uaecptr a, uae_u32 v){ et4000_engine_io_write (a, v);}
static void vga_io_wput(uaecptr a, uae_u32 v){ et4000_engine_io_write (a, v);}
static void vga_io_bput(uaecptr a, uae_u32 v){ et4000_engine_io_write (a, v); }
*/
// static uae_u8 *vga_io_xlate(uaecptr a){ return natmem_offset + a; }
static uae_u8 *vga_io_xlate(uaecptr a) { return NULL; }
static int vga_io_check(uaecptr a, uae_u32 sz)
{
    return 0; //(a >= (et4k_addr_ptr->io_base + 0x300) && a < ((et4k_addr_ptr->io_base + 0x00000400) - sz));
}

static addrbank pistorm_vga_io = {
    vga_io_lget, vga_io_wget, vga_io_bget,
    vga_io_lput, vga_io_wput, vga_io_bput,
    vga_io_xlate, vga_io_check, NULL, "VGA-IO", "VGA-IO",
    vga_io_lget, vga_io_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    S_READ, S_WRITE};

/* ------------------------------------------------------------------ */
/* map one 64KB-aligned region into the bank table (mirrors map_banks) */
/* ------------------------------------------------------------------ */

static void map_region(uaecptr start, uint32_t len, addrbank *b)
{
    uint64_t end = (uint64_t)start + len;
    for (uint64_t a = start; a < end; a += 0x10000)
    {
        unsigned page = bankindex((uint32_t)a);
        mem_banks[page] = b;
        if (b->baseaddr)
            baseaddr[page] = b->baseaddr - start; // host = baseaddr[page] + addr
        else
            baseaddr[page] = (uae_u8 *)(((uae_u8 *)b) + 1);
    }
}

/* ------------------------------------------------------------------ */
/* No-spill guarded-read slow path (PISTORM_JIT_GUARD=2).              */
/*                                                                     */
/* The JIT's fast path handles direct RAM/ROM/TT inline. Only true I/O */
/* reaches here. These dispatch helpers do the ordinary bank getter    */
/* call; the naked trampolines below wrap them so that x1-x18 and x30   */
/* are preserved across the call. That lets the JIT emit the slow-path  */
/* call WITHOUT flushing guest registers to memory (no prepare_for_call */
/* / flush_all), which is the whole point of the no-spill variant.     */
/* x0 is arg(adr)->result; x19-x28 (incl the JIT's R_MEMSTART=x27 and   */
/* R_REGSTRUCT=x28) are callee-saved and preserved by the C helper.    */
/* ------------------------------------------------------------------ */
extern "C" uae_u32 pistorm_grd_dispatch_l(uae_u32 adr) { return mem_banks[bankindex(adr)]->lget(adr); }
extern "C" uae_u32 pistorm_grd_dispatch_w(uae_u32 adr) { return mem_banks[bankindex(adr)]->wget(adr); }
extern "C" uae_u32 pistorm_grd_dispatch_b(uae_u32 adr) { return mem_banks[bankindex(adr)]->bget(adr); }

__asm__(
".text\n"
".p2align 2\n"
".global pistorm_grd_slow_l\n"
".global pistorm_grd_slow_w\n"
".global pistorm_grd_slow_b\n"
"pistorm_grd_slow_l:\n"
"  sub  sp, sp, #160\n"
"  stp  x1,  x2,  [sp, #0]\n"
"  stp  x3,  x4,  [sp, #16]\n"
"  stp  x5,  x6,  [sp, #32]\n"
"  stp  x7,  x8,  [sp, #48]\n"
"  stp  x9,  x10, [sp, #64]\n"
"  stp  x11, x12, [sp, #80]\n"
"  stp  x13, x14, [sp, #96]\n"
"  stp  x15, x16, [sp, #112]\n"
"  stp  x17, x18, [sp, #128]\n"
"  str  x30,      [sp, #144]\n"
"  bl   pistorm_grd_dispatch_l\n"
"  ldp  x1,  x2,  [sp, #0]\n"
"  ldp  x3,  x4,  [sp, #16]\n"
"  ldp  x5,  x6,  [sp, #32]\n"
"  ldp  x7,  x8,  [sp, #48]\n"
"  ldp  x9,  x10, [sp, #64]\n"
"  ldp  x11, x12, [sp, #80]\n"
"  ldp  x13, x14, [sp, #96]\n"
"  ldp  x15, x16, [sp, #112]\n"
"  ldp  x17, x18, [sp, #128]\n"
"  ldr  x30,      [sp, #144]\n"
"  add  sp, sp, #160\n"
"  ret\n"
"pistorm_grd_slow_w:\n"
"  sub  sp, sp, #160\n"
"  stp  x1,  x2,  [sp, #0]\n"
"  stp  x3,  x4,  [sp, #16]\n"
"  stp  x5,  x6,  [sp, #32]\n"
"  stp  x7,  x8,  [sp, #48]\n"
"  stp  x9,  x10, [sp, #64]\n"
"  stp  x11, x12, [sp, #80]\n"
"  stp  x13, x14, [sp, #96]\n"
"  stp  x15, x16, [sp, #112]\n"
"  stp  x17, x18, [sp, #128]\n"
"  str  x30,      [sp, #144]\n"
"  bl   pistorm_grd_dispatch_w\n"
"  ldp  x1,  x2,  [sp, #0]\n"
"  ldp  x3,  x4,  [sp, #16]\n"
"  ldp  x5,  x6,  [sp, #32]\n"
"  ldp  x7,  x8,  [sp, #48]\n"
"  ldp  x9,  x10, [sp, #64]\n"
"  ldp  x11, x12, [sp, #80]\n"
"  ldp  x13, x14, [sp, #96]\n"
"  ldp  x15, x16, [sp, #112]\n"
"  ldp  x17, x18, [sp, #128]\n"
"  ldr  x30,      [sp, #144]\n"
"  add  sp, sp, #160\n"
"  ret\n"
"pistorm_grd_slow_b:\n"
"  sub  sp, sp, #160\n"
"  stp  x1,  x2,  [sp, #0]\n"
"  stp  x3,  x4,  [sp, #16]\n"
"  stp  x5,  x6,  [sp, #32]\n"
"  stp  x7,  x8,  [sp, #48]\n"
"  stp  x9,  x10, [sp, #64]\n"
"  stp  x11, x12, [sp, #80]\n"
"  stp  x13, x14, [sp, #96]\n"
"  stp  x15, x16, [sp, #112]\n"
"  stp  x17, x18, [sp, #128]\n"
"  str  x30,      [sp, #144]\n"
"  bl   pistorm_grd_dispatch_b\n"
"  ldp  x1,  x2,  [sp, #0]\n"
"  ldp  x3,  x4,  [sp, #16]\n"
"  ldp  x5,  x6,  [sp, #32]\n"
"  ldp  x7,  x8,  [sp, #48]\n"
"  ldp  x9,  x10, [sp, #64]\n"
"  ldp  x11, x12, [sp, #80]\n"
"  ldp  x13, x14, [sp, #96]\n"
"  ldp  x15, x16, [sp, #112]\n"
"  ldp  x17, x18, [sp, #128]\n"
"  ldr  x30,      [sp, #144]\n"
"  add  sp, sp, #160\n"
"  ret\n"
);

/* ------------------------------------------------------------------ */
/* jit_mem_init(): called once from emulator.c before the CPU threads  */
/* ------------------------------------------------------------------ */

extern "C" void jit_mem_init(void)
{
    natmem_offset = (uae_u8 *)mmap(NULL, GUEST_RESERVE,
                                   PROT_READ | PROT_WRITE,// | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (natmem_offset == MAP_FAILED)
    {
        perror("natmem mmap");
        abort();
    }

    pistorm_fvdi_fb = (uint8_t *)mmap(NULL, FVDI_FB_MAX_BYTES,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                      -1, 0);
    if (pistorm_fvdi_fb == MAP_FAILED)
    {
        perror("fVDI framebuffer mmap");
        abort();
    }

    //printf ("Jit_mem_init: natmem_offset %p\n", natmem_offset);

    /* Safety net: fault loudly if the JIT ever direct-bangs an I/O hole. */
    //mprotect(natmem_offset + IO_IDE_BASE, IO_IDE_SIZE, PROT_NONE);
    //mprotect(natmem_offset + IO_HW_BASE, IO_HW_SIZE, PROT_NONE);

    //printf ("ET4K_enabled %d, vram base %p, vgaio base %p\n", 
     //   ET4K_enabled, 0, 0);//et4k_addr_ptr->vram_base, et4k_addr_ptr->io_base);
//usleep(1000);
    if (emulator_config_et4k_enabled())
    {
        /* Do not blanket-protect the inactive 0xA/0xB apertures here. For NOVA,
         * the FB-GUARD immediately below 0xC00000 lives inside 0xB00000..0xBFFFFF
         * and must stay writable host scratch RAM. The addrbank map below owns
         * routing/protection for the real active aperture and I/O window. */
        mprotect(natmem_offset + et4k_addr_ptr->vram_base, VRAM_SIZE, PROT_NONE);
        mprotect(natmem_offset + et4k_addr_ptr->io_base, VGA_IO_SIZE, PROT_NONE);
        pistorm_vga_vram.baseaddr = NULL;
        pistorm_vga_io.baseaddr = natmem_offset + et4k_addr_ptr->io_base;
    }

    pistorm_lowram_bank.baseaddr = natmem_offset + 0;
    pistorm_dummy_bank.baseaddr = natmem_offset + 0;
    pistorm_stram_bank.baseaddr = natmem_offset + 0;
    //pistorm_fdd_bank.baseaddr = natmem_offset + 0x00FF0000;
    pistorm_rom_bank.baseaddr = natmem_offset + ROM_BASE;
    pistorm_ide_bank.baseaddr = natmem_offset + IO_IDE_BASE;
    pistorm_io_bank.baseaddr = natmem_offset + IO_HW_BASE;
    pistorm_hw_bank.baseaddr = natmem_offset + 0x00FF0000;
    pistorm_ttram_bank.baseaddr = natmem_offset + TT_RAM_BASE;
#if (1)
    if (emulator_config_et4k_enabled())
        pistorm_guard_bank.baseaddr = natmem_offset + GUARD_BASE;
#endif

    /* ROM size from the loaded image, rounded up to a 64KB bank and clamped so
     * a 1MB EmuTOS at 0xE00000 abuts IDE at 0xF00000 without overlapping. */
    pistorm_rom_size = pistorm_rom_end - pistorm_rom_start; //(pistorm_tos_image_len + 0xFFFFu) & ~0xFFFFu;
    //if (pistorm_rom_size > ROM_MAX_SIZE)
     //   pistorm_rom_size = ROM_MAX_SIZE;

    if (pistorm_rom_ptr) // && pistorm_tos_image_len <= pistorm_rom_size)
        memcpy(natmem_offset + ROM_BASE, pistorm_rom_ptr, pistorm_rom_size);
   // else
    //    printf("[JITINIT] ROM copy failed\n");
    //printf ("rom ptr %p\n", pistorm_rom_ptr);
    //for (int n = 0; n < 0x30; n++) {
    //    printf ("0x%02X ", pistorm_rom_ptr[n]);
    //    if (n % 16 == 0)
    //        printf ("\n");
    //}
    //printf ("\n");
    /* Every mem_banks[] slot MUST be non-NULL: the JIT reads bank->jit_read_flag
     * (offset 0x6c) on every access, so a NULL slot => SIGSEGV at host 0x6c.
     * Fill the ENTIRE 32-bit space (incl. 32-bit Atari I/O at 0xFFFF8xxx, which
     * is above GUEST_RESERVE) with the dummy bus bank first, then overlay. */
    for (uint32_t i = 0; i < MEMORY_BANKS; i++)
        mem_banks[i] = &pistorm_dummy_bank;

    /* Default the whole space to the bus (handler) so any UNDECODED access still
     * reaches real hardware and can take a genuine BERR; then overlay the
     * direct-mapped regions. IDE (0xF00000) and HW (0xFF8000) stay on the bus. */
    // map_region(0,           0x01000000,       &pistorm_dummy_bank);
    map_region(0, GUEST_RESERVE, &pistorm_dummy_bank); //&pistorm_io_bank);
    map_region(0, ST_RAM_SIZE, &pistorm_stram_bank);
    if (emulator_config_stram_direct_enabled()) {
        /* Fast mirror-only RAM is deliberately separate from stram_cache.
         * stram_cache still uses the ST-RAM handler so it can write through
         * the live physical screen area for the real Atari shifter. */
       map_region(STRAM_DIRECT_START, STRAM_DIRECT_END - STRAM_DIRECT_START, &pistorm_lowram_bank);
        //map_region(0, 0x00400000, &pistorm_lowram_bank);
    }
    
    if (emulator_config_et4k_enabled()) {
        map_region(GUARD_BASE, GUARD_SIZE, &pistorm_guard_bank);
        map_region(et4k_addr_ptr->vram_base, VRAM_SIZE, &pistorm_vga_vram);
        map_region(et4k_addr_ptr->io_base, VGA_IO_SIZE, &pistorm_vga_io);
    }

    map_region(FVDI_FB_BASE, FVDI_FB_MAX_BYTES, &pistorm_fvdi_bank);

    map_region(ROM_BASE, pistorm_rom_size, &pistorm_rom_bank);
    map_region(0x00FF0000u, 0x10000, &pistorm_hw_bank); // Atari HW page, FPU probe, NOVA aliases
    map_region(0xFFFF0000u, 0x10000, &pistorm_hw_bank); // 32-bit alias
    map_region(0x00F00000u, 0x00100, &pistorm_ide_bank); // IDE 24-bit base
    map_region(0xFFF00000u, 0x00100, &pistorm_ide_bank); // IDE 32-bit alias (the hot one)

    //map_region(0x00FF0000u, 0x10000, &pistorm_fdd_bank);  // 24-bit HW I/O alias
   // map_region(0xFFFF0000u, 0x10000, &pistorm_fdd_bank);  // 32-bit alias (the hot one)

    if (tt_ram_available)
        map_region(TT_RAM_BASE, tt_ram_size, &pistorm_ttram_bank);
    // pistorm_bank_profile_init ();

    /* Force full-distrust off for direct regions, on for I/O via the bank
     * jit_*_flag above. special_mem_default mirrors comptrust; the JIT ORs in
     * each bank's jit_read_flag/jit_write_flag when it sets up an access. */
    special_mem_default = 0;
    special_mem = 0;
}

/* Real hardware overlays ROM at 0x0 for the first reads after reset so the CPU
 * fetches SSP@0 / PC@4 from ROM. Reproduce that by copying the ROM's first 8
 * bytes (raw m68k/BE order) into the mirror at 0 before m68k_reset() reads them. */
extern "C" void pistorm_seed_reset_vector(void)
{
    if (natmem_offset)
        memcpy(natmem_offset + 0, natmem_offset + ROM_BASE, 8);
}
