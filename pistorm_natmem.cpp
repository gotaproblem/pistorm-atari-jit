// SPDX-License-Identifier: MIT
//
// pistorm_natmem.cpp — single-array guest memory map for the Amiberry ARM64 JIT
// on pistorm-atari. Replaces Amiberry's shm/natmem allocator with one flat mmap
// reservation that we own, and wires the per-64KB bank table so that:
//
//   * RAM/ROM/TT-RAM are DIRECT-mapped  -> JIT bangs [x27 + addr], full speed
//   * I/O (HW regs, IDE) are HANDLER-routed -> ps_* over the bus, never banged
//   * ST-RAM reads are direct, writes are write-through-to-bus + SMC-invalidate
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
#include <sys/mman.h>
#include <string.h>
#include "platforms/atari/audio/dmasnd.h"

extern "C"
{
    unsigned int m68k_read_memory_8(uint32_t);
    unsigned int m68k_read_memory_16(uint32_t);
    unsigned int m68k_read_memory_32(uint32_t);
    void m68k_write_memory_8(uint32_t, unsigned int);
    void m68k_write_memory_16(uint32_t, unsigned int);
    void m68k_write_memory_32(uint32_t, unsigned int);
    extern volatile uint8_t fc; // CONFIRM type/name in your tree

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
}

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
}

void invalidate_block(blockinfo *); // verified in compemu_support_arm.cpp

/* cache_invalidate() is OURS (not an Amiberry symbol — the JIT exposes the
 * flush_icache function pointer instead). pistorm_smc() calls it only when a
 * write lands on a 4KB page previously marked as holding compiled code, so the
 * full flush here is coarse but infrequent. flush_icache is set during JIT
 * init; guard against the pre-init NULL. */
void cache_invalidate(void)
{
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
#define VGA_IO_SIZE (0x00010000u)
#define VGA_BASE_XVDI (0x00A00000u) // XVDI
#define VGA_IO_BASE_XVDI (0x00B00000u)
#define VGA_BASE_NOVA (0x00C00000u) // NOVA
#define VGA_IO_BASE_NOVA (0x00D00000u)

#define GUEST_RESERVE (TT_RAM_BASE + TT_RAM_SIZE) // 0x01000000 + 0x08000000 = 16MB + 128MB

uae_u8 *natmem_offset = NULL; // the one array; x27 in JIT

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

    extern bool ET4K_enabled;
    extern ET4KADDRESSES_s *et4k_addr_ptr;
    extern int ET4K_driver;
    // extern ET4000State *g_et4000;

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
    if (addr >= GUEST_RESERVE)
        return;
    if (code_page[addr >> PAGE_SHIFT] |
        code_page[(addr + sz - 1) >> PAGE_SHIFT])
        cache_invalidate(); // coarse but correct; targeted later
}

extern "C" void pistorm_dma_from_stram(uint32_t addr, uint8_t *dst, uint32_t n)
{
    if (addr + n < ST_RAM_SIZE)
        memcpy(dst, natmem_offset + addr, n);
}

/* DMA path (fdc.c) calls this so the mirror stays coherent with bus DMA-in */
extern "C" void pistorm_dma_to_stram(uaecptr addr, const uint8_t *src, uint32_t n)
{
    if (addr + n < ST_RAM_SIZE)
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
    fc_data();
    uae_u32 v = m68k_read_memory_32(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
}
static uae_u32 dm_wget(uaecptr a)
{
    PROF_DUMMY_R(a);
    fc_data();
    uae_u32 v = m68k_read_memory_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return (uint16_t)v;
}
static uae_u32 dm_bget(uaecptr a)
{
    PROF_DUMMY_R(a);
    fc_data();
    uae_u32 v = m68k_read_memory_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return (uint8_t)v;
}

/*
static uae_u32 dm_lget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u32 v=ps_read_32(a); pistorm_buserr(a,0,true,sz_long); return v; }
static uae_u32 dm_wget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u16 v=ps_read_16(a); pistorm_buserr(a,0,true,sz_word); return v; }
static uae_u32 dm_bget(uaecptr a){ PROF_DUMMY_R(a); fc_data(); a &= 0x00FFFFFF; uae_u8  v=ps_read_8 (a); pistorm_buserr(a,0,true,sz_byte); return v; }
*/

static void dm_lput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
    fc_data();
    m68k_write_memory_32(a, v);
    pistorm_buserr(a, v, false, sz_long);
}
static void dm_wput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
    fc_data();
    m68k_write_memory_16(a, (uint16_t)v);
    pistorm_buserr(a, v, false, sz_word);
}
static void dm_bput(uaecptr a, uae_u32 v)
{
    PROF_DUMMY_W(a);
    fc_data();
    m68k_write_memory_8(a, (uint8_t)v);
    pistorm_buserr(a, v, false, sz_byte);
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
    g_buserr = 0;
    fc_data();
    //a &= 0x00FFFFFF;
    uae_u32 v = ps_read_32(a);
    pistorm_buserr(a, 0, true, sz_long);
    return v;
}
static uae_u32 io_wget(uaecptr a)
{
    PROF_IO_R(a);
    g_buserr = 0;
    fc_data();
    //a &= 0x00FFFFFF;
    uae_u16 v = ps_read_16(a);
    pistorm_buserr(a, 0, true, sz_word);
    return v;
}
static uae_u32 io_bget(uaecptr a)
{
    PROF_IO_R(a);
    g_buserr = 0;
    fc_data();
    //a &= 0x00FFFFFF;
    uae_u8 v = ps_read_8(a);
    pistorm_buserr(a, 0, true, sz_byte);
    return v;
}
/*
static void io_lput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_32(a, v); pistorm_buserr(a,v,false,sz_long); }
static void io_wput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_16(a, (uint16_t)v); pistorm_buserr(a,v,false,sz_word); }
static void io_bput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); m68k_write_memory_8 (a, (uint8_t)v); pistorm_buserr(a,v,false,sz_byte); }
*/

static void io_lput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
    g_buserr = 0;
    fc_data();
    //a &= 0x00FFFFFF;
    ps_write_32(a, v);
    pistorm_buserr(a, v, false, sz_long);
}
static void io_wput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
    g_buserr = 0;
    fc_data();
   // a &= 0x00FFFFFF;
    ps_write_16(a, (uint16_t)v);
    pistorm_buserr(a, v, false, sz_word);
}
static void io_bput(uaecptr a, uae_u32 v)
{
    PROF_IO_W(a);
    g_buserr = 0;
    fc_data();
    //a &= 0x00FFFFFF;
    ps_write_8(a, (uint8_t)v);
    pistorm_buserr(a, v, false, sz_byte);
}

// static void io_lput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); a &= 0x00FFFFFF; dmasnd_snoop32(a,(uint32_t)v); ps_write_32(a, v);           pistorm_buserr(a,v,false,sz_long); }
// static void io_wput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); a &= 0x00FFFFFF; dmasnd_snoop16(a,(uint16_t)v); ps_write_16(a, (uint16_t)v); pistorm_buserr(a,v,false,sz_word); }
// static void io_bput(uaecptr a, uae_u32 v){ PROF_IO_W(a); fc_data(); a &= 0x00FFFFFF; dmasnd_snoop8 (a,(uint8_t)v);  ps_write_8 (a, (uint8_t)v);  pistorm_buserr(a,v,false,sz_byte); }

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

#define MFP_GPIP            0xFFFA01u

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
    if (fdd_owns_address (a)) { printf ("fdd io rd32 0x%X\n", a);
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
    if (a == MFP_GPIP) { printf ("fdd gpip rd16\n");
        uint8_t gpip = fdd_gpip (ps_read_8 (a));
        return gpip;
    }
    if (fdd_owns_address (a)) { printf ("fdd io rd16 0x%X\n", a);
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
    if (a == MFP_GPIP) { printf ("fdd gpip rd8\n");
        uint8_t gpip = fdd_gpip (ps_read_8 (a));
        pistorm_buserr (a, 0, true, sz_byte);
        return gpip;
    }
    if (fdd_owns_address (a)) { printf ("fdd io rd8 0x%X\n", a);
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
    if (fdd_owns_address (a)) { printf ("fdd io wr32 0x%X\n", a);
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
    if (fdd_owns_address (a)) { printf ("fdd io wr16 0x%X\n", a);
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
    if (fdd_owns_address (a)) { printf ("fdd io wr8 0x%X\n", a);
        fdd_io_write (a, v, 1);
        return;
    }
    ps_write_8 (a, (uint8_t)v);
    pistorm_buserr (a, v, false, sz_byte);
}

static int fdd_check (uaecptr a, uae_u32 sz) { return (a >= 0xFF8600u && a <= 0xFF860Fu) || (a >= 0xFF8800u && a <= 0xFF8803u) || a == MFP_GPIP; }     /* indirect: no direct host ptr */
static uae_u8 *fdd_xlate (uaecptr a) { return natmem_offset + a; } /* never used (check==0) */

static addrbank pistorm_fdd_bank = {
    fdd_lget, fdd_wget, fdd_bget,
    fdd_lput, fdd_wput, fdd_bput,
    fdd_xlate, fdd_check, NULL, "FDD", "FDD",
    fdd_lget, fdd_wget,
    ABFLAG_IO | ABFLAG_INDIRECT,
    /* jit_read_flag, jit_write_flag: force handler calls, no direct access */
    S_READ, S_WRITE};


/* ====================================================================== *
 * LOW-RAM bank — paste into pistorm_natmem.cpp next to pistorm_stram_bank.
 * --------------------------------------------------------------------- *
 * First 64KB of ST-RAM = exception vectors (0x000-0x3FF) + TOS/OS system
 * variables. The profiler showed 5.5M of 5.6M ST-RAM write-through bus
 * cycles land here. This region is CPU-only:
 *   - it is never the video framebuffer (top of ST-RAM, and your video is
 *     on the ET4000 anyway),
 *   - it is never a DMA buffer (BIOS/GEMDOS allocate those higher up),
 *   - nothing on the real Atari bus ever reads it.
 * So writes do NOT need write-through to ps_write. Keep them mirror-only.
 * Reads were already direct from the mirror. SMC-invalidate is retained in
 * case anything executes from low RAM (cheap no-op when it doesn't).
 * ====================================================================== */
static uae_u32 lo_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 lo_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 lo_bget(uaecptr a) { return natmem_offset[a]; }

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

static uae_u8 *lo_xlate(uaecptr a) { return natmem_offset + a; }
static int lo_check(uaecptr a, uae_u32 sz) { return 1; }

static addrbank pistorm_lowram_bank = {
    lo_lget, lo_wget, lo_bget,
    lo_lput, lo_wput, lo_bput,
    lo_xlate, lo_check, NULL, "ST-RAM-LOW", "ST-RAM-LOW",
    lo_lget, lo_wget,
    ABFLAG_RAM | ABFLAG_DIRECTACCESS,
    0, 0 /* reads AND writes direct — no bus */
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
/* ST-RAM bank — read direct from mirror, write-through to bus + SMC    */
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
    do_put_mem_long((uae_u32 *)(natmem_offset + a), v); // update mirror (BE)
    fc_data();
    a &= 0x00FFFFFF;
    ps_write_32(a, v); // write-through to bus
    // m68k_write_memory_32(a, v);
    // pistorm_buserr(a, v, false, sz_long);
    pistorm_smc(a, 4);
}
static void sr_wput(uaecptr a, uae_u32 v)
{
    PROF_STRAM_W(a);
    do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v);
    fc_data();
    a &= 0x00FFFFFF;
    ps_write_16(a, (uint16_t)v);
    // m68k_write_memory_16(a, (uae_u16)v);
    // pistorm_buserr(a, v, false, sz_word);
    pistorm_smc(a, 2);
}
static void sr_bput(uaecptr a, uae_u32 v)
{
    PROF_STRAM_W(a);
    natmem_offset[a] = (uae_u8)v;
    fc_data();
    a &= 0x00FFFFFF;
    ps_write_8(a, (uint8_t)v);
    // m68k_write_memory_8(a, (uae_u8)v);
    // pistorm_buserr(a, v, false, sz_byte);
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
static int tt_check(uaecptr a, uae_u32 sz) { return (a >= TT_RAM_BASE) && a < (TT_RAM_BASE + TT_RAM_SIZE); }

static addrbank pistorm_ttram_bank = {
    tt_lget, tt_wget, tt_bget,
    tt_lput, tt_wput, tt_bput,
    tt_xlate, tt_check, NULL, "TT-RAM", "TT-RAM",
    tt_lget, tt_wget,
    ABFLAG_RAM | ABFLAG_DIRECTACCESS | ABFLAG_CACHE_ENABLE_ALL,
    0, 0};
#if (0)
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
/* GUARD_BASE is now derived at run time from the ACTIVE aperture, not the
 * XVDI aperture: the 1MB immediately below et4k_addr_ptr->vram_base. For NOVA
 * (vram_base 0xC00000) that is 0xB00000..0xBFFFFF, which is where a save-under
 * under-run actually lands (e.g. the 0xBFFFFE bus error). Only GUARD_SIZE is a
 * compile-time constant. */
#define GUARD_SIZE (0x00200000u) /* 1MB */
#define GUARD_BASE (et4k_addr_ptr->vram_base - GUARD_SIZE)

static uae_u32 gd_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + a)); }
static uae_u32 gd_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + a)); }
static uae_u32 gd_bget(uaecptr a) { return natmem_offset[a]; }

static void gd_lput(uaecptr a, uae_u32 v) { do_put_mem_long((uae_u32 *)(natmem_offset + a), v); }
static void gd_wput(uaecptr a, uae_u32 v) { do_put_mem_word((uae_u16 *)(natmem_offset + a), (uae_u16)v); }
static void gd_bput(uaecptr a, uae_u32 v) { natmem_offset[a] = (uae_u8)v; }

static uae_u8 *gd_xlate(uaecptr a) { return natmem_offset + a; }
static int gd_check(uaecptr a, uae_u32 sz) { return (a >= GUARD_BASE && a < et4k_addr_ptr->vram_base - sz); }

static addrbank pistorm_guard_bank = {
    gd_lget, gd_wget, gd_bget,
    gd_lput, gd_wput, gd_bput,
    gd_xlate, gd_check, NULL, "FB-GUARD", "FB-GUARD",
    gd_lget, gd_wget,
    ABFLAG_RAM,
    0, 0 // fully direct, no special
};
#endif

/* VGA-VRAM bank — pure host, direct R/W, native (mprotect) SMC like stock fast RAM */
// static uae_u32 vga_lget(uaecptr a){ return do_get_mem_long((uae_u32*)(natmem_offset+a)); }
// static uae_u32 vga_wget(uaecptr a){ return do_get_mem_word((uae_u16*)(natmem_offset+a)); }
// static uae_u32 vga_bget(uaecptr a){ return natmem_offset[a]; }

extern "C" uint8_t *et4000_engine_vram_ptr(void);

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
    uae_u32 v = et4000_engine_vram_read32(a);
    return v;
}
static uae_u32 vga_wget(uaecptr a)
{
    uae_u32 v = et4000_engine_vram_read16(a);
    return v;
}
static uae_u32 vga_bget(uaecptr a)
{
    uae_u32 v = et4000_engine_vram_read8(a);
    return v;
}
static void vga_lput(uaecptr a, uae_u32 v) { et4000_engine_vram_write32(a, v); }
static void vga_wput(uaecptr a, uae_u32 v) { et4000_engine_vram_write16(a, (uae_u16)v); }
static void vga_bput(uaecptr a, uae_u32 v) { et4000_engine_vram_write8(a, (uae_u8)v); }

// static uae_u8 *vga_xlate(uaecptr a){ return NULL; }
// static uae_u8 *vga_xlate(uaecptr a){ return natmem_offset + a; }
static uae_u8 *vga_xlate(uaecptr a) { return et4000_engine_vram_ptr() + (a & (VRAM_SIZE - 1)); }
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
    ABFLAG_RAM | ABFLAG_DIRECTACCESS,
    S_READ, S_WRITE};

void et4000_engine_io_write(uint16_t port, uint8_t val);
uint8_t et4000_engine_io_read(uint16_t port);

static uae_u32 vga_io_lget(uaecptr a) { return 0xFF; } // uae_u32 v=(et4000_io_read16(g_et4000, a+1) | (et4000_io_read16(g_et4000, a) << 16)); return v; }
static uae_u32 vga_io_wget(uaecptr a)
{
    uae_u32 v = et4000_io_read16(g_et4000, a);
    return v;
}
static uae_u32 vga_io_bget(uaecptr a)
{
    uae_u32 v = et4000_io_read8(g_et4000, a);
    return v;
}
static void vga_io_lput(uaecptr a, uae_u32 v) { et4000_io_write32(g_et4000, a, v); }
static void vga_io_wput(uaecptr a, uae_u32 v) { et4000_io_write16(g_et4000, a, (uae_u16)v); }
static void vga_io_bput(uaecptr a, uae_u32 v) { et4000_io_write8(g_et4000, a, (uae_u8)v); }

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
/*
static void map_region(uaecptr start, uint32_t len, addrbank *b)
{
    for (uint32_t a = start; a < start + len; a += 0x10000)
    {
        unsigned page = bankindex(a);
        mem_banks[page] = b;
        if (b->baseaddr)
            baseaddr[page] = b->baseaddr - start; // host = baseaddr[page] + addr
        else
            baseaddr[page] = (uae_u8 *)(((uae_u8 *)b) + 1); // tagged non-null => special
    }
}
*/
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

    //printf ("Jit_mem_init: natmem_offset %p\n", natmem_offset);

    /* Safety net: fault loudly if the JIT ever direct-bangs an I/O hole. */
    //mprotect(natmem_offset + IO_IDE_BASE, IO_IDE_SIZE, PROT_NONE);
    //mprotect(natmem_offset + IO_HW_BASE, IO_HW_SIZE, PROT_NONE);

    //printf ("ET4K_enabled %d, vram base %p, vgaio base %p\n", 
     //   ET4K_enabled, 0, 0);//et4k_addr_ptr->vram_base, et4k_addr_ptr->io_base);
//usleep(1000);
    if (ET4K_enabled)
    {
        /* Fault loudly only if the JIT ever direct-bangs the ACTIVE aperture's
         * handler region (VRAM lives in the engine's own buffer; IO is pure
         * handler). Do NOT blanket-protect the inactive aperture or the guard:
         * the FB-GUARD below now backs the 1MB directly under the active
         * framebuffer (0xB00000 for NOVA) and MUST stay writable host RAM, or
         * the guard's own gd_* accesses would SIGSEGV. */
        mprotect(natmem_offset + 0x00A00000, 0x00100000, PROT_NONE);
        mprotect(natmem_offset + 0x00B00000, 0x00100000, PROT_NONE);
        mprotect(natmem_offset + et4k_addr_ptr->vram_base, VRAM_SIZE, PROT_NONE);
        mprotect(natmem_offset + et4k_addr_ptr->io_base, VGA_IO_SIZE, PROT_NONE);
        pistorm_vga_vram.baseaddr = natmem_offset + et4k_addr_ptr->vram_base;
        pistorm_vga_io.baseaddr = natmem_offset + et4k_addr_ptr->io_base;
    }

    pistorm_lowram_bank.baseaddr = natmem_offset + 0;
    pistorm_dummy_bank.baseaddr = natmem_offset + 0;
    pistorm_stram_bank.baseaddr = natmem_offset + 0;
    //pistorm_fdd_bank.baseaddr = natmem_offset + 0x00FF0000;
    pistorm_rom_bank.baseaddr = natmem_offset + ROM_BASE;
    // pistorm_guard_bank.baseaddr = natmem_offset + GUARD_BASE;
    pistorm_ide_bank.baseaddr = natmem_offset + IO_IDE_BASE;
    pistorm_io_bank.baseaddr = natmem_offset + IO_HW_BASE;
    pistorm_ttram_bank.baseaddr = natmem_offset + TT_RAM_BASE;

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
    map_region(0, 0x400000, &pistorm_lowram_bank); // overlay low 64KB: mirror-only
    
    if (ET4K_enabled) {
        map_region(et4k_addr_ptr->vram_base, VRAM_SIZE, &pistorm_vga_vram);
        map_region(et4k_addr_ptr->io_base, VGA_IO_SIZE, &pistorm_vga_io);
    }

    map_region(ROM_BASE, pistorm_rom_size, &pistorm_rom_bank);
    //map_region(0x00FF8000u, 0x08000, &pistorm_io_bank);  // HW I/O  alias 0xFFFF8xxx
    //map_region(0xFFFF8000u, 0x08000, &pistorm_io_bank);  // HW I/O  alias 0xFFFF8xxx
    map_region(0x00F00000u, 0x00100, &pistorm_ide_bank); // IDE 24-bit base
    map_region(0xFFF00000u, 0x00100, &pistorm_ide_bank); // IDE 32-bit alias (the hot one)

    //map_region(0x00FF0000u, 0x10000, &pistorm_fdd_bank);  // 24-bit HW I/O alias
   // map_region(0xFFFF0000u, 0x10000, &pistorm_fdd_bank);  // 32-bit alias (the hot one)

    map_region(TT_RAM_BASE, TT_RAM_SIZE, &pistorm_ttram_bank);
    // map_region(GUARD_BASE,  GUARD_SIZE,       &pistorm_guard_bank);

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