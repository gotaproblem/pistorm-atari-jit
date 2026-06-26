/* SPDX-License-Identifier: MIT
 *
 * pistorm_stubs.cpp
 * -----------------
 * The Amiberry CPU/JIT core references symbols that normally live in the Amiga
 * machine layer we do NOT compile (custom.cpp, cfgfile.cpp, memory.cpp, main.cpp,
 * savestate.cpp, inputrecord.cpp, audio.cpp, cpuboard.cpp, the osdep/gui layer).
 * This file satisfies them for the Atari pistorm target.
 *
 * The list here was DERIVED, not guessed: the core + softfloat were compiled to
 * objects on the host and `nm` was used to collect undefined symbols, minus the
 * ones our own code already provides. Signatures were taken verbatim from the
 * Amiberry headers so they match at link time.
 *
 * Three kinds of entries:
 *   1. currprefs / changed_prefs            — the real prefs globals (cfgfile.cpp)
 *   2. functional infrastructure            — event table, memory dispatch: REAL
 *                                             implementations, not no-ops
 *   3. Amiga machine-layer no-ops           — never exercised on the Atari; safe
 *                                             to return 0 / do nothing
 *
 * NOT defined here (provided elsewhere — do not duplicate):
 *   - intlev(), intlev_ack(), jit_set_irq(), jit_signal_buserr()   -> jit_glue.cpp
 *   - mem_banks[], the bank map, natmem_offset                     -> pistorm_natmem.cpp
 *   - compile_block(), build_comp(), check_for_cache_miss(),
 *     pushall_call_handler, pissoff_value, set_cache_state()       -> jit/ (AArch64)
 *
 * After the first real AArch64 link, any remaining "undefined reference" is a
 * symbol the host compile couldn't see (JIT-side). Add it here with the
 * signature the linker/header gives.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "events.h"
#include "gui.h"
#include "uae/vm.h" /* uae_vm_alloc/free signatures + UAE_VM_* protect flags */

#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mman.h> /* uae_vm_alloc: executable JIT translation cache */

/* ================================================================= *
 * 1. Preferences globals (normally defined in cfgfile.cpp).
 *    jit_cpu_init() in jit_glue.cpp fills the ~15 fields the CPU/JIT
 *    actually read (cpu_model, fpu_model, cachesize, address_space_24,
 *    mmu_model, cpu_compatible, cpu_cycle_exact, comptrustbyte..., etc.).
 * ================================================================= */
struct uae_prefs currprefs;
struct uae_prefs changed_prefs;

/* ================================================================= *
 * 2. Functional infrastructure — REAL implementations.
 * ================================================================= */

/* Event scheduler tables. events.cpp drives them; they just need to exist
 * and be zero-initialised (events_init/schedule populate them at runtime). */
struct ev eventtab[ev_max];
struct ev2 eventtab2[ev2_max];

/* Memory dispatch. The JIT uses direct natmem access for RAM/ROM, but the
 * interpreter fallback, the MMU paths and a few helpers still call these.
 * They route through the addrbank table that pistorm_natmem.cpp installs.
 *   get_mem_bank(addr) == *mem_banks[addr >> 16]   (from memory.h)
 * If pistorm_natmem.cpp already defines any of these, delete them here and
 * keep the natmem copy (the linker will flag the duplicate). */
uae_u8 *baseaddr[MEMORY_BANKS];

addrbank *get_mem_bank_real(uaecptr addr)
{
    return mem_banks[bankindex(addr)];
}
uae_u32 memory_get_byte(uaecptr addr) { return get_mem_bank(addr).bget(addr); }
uae_u32 memory_get_word(uaecptr addr) { return get_mem_bank(addr).wget(addr); }
uae_u32 memory_get_long(uaecptr addr) { return get_mem_bank(addr).lget(addr); }
uae_u32 memory_get_wordi(uaecptr addr) { return get_mem_bank(addr).wgeti(addr); }
uae_u32 memory_get_longi(uaecptr addr) { return get_mem_bank(addr).lgeti(addr); }
void memory_put_byte(uaecptr addr, uae_u32 v) { get_mem_bank(addr).bput(addr, v); }
void memory_put_word(uaecptr addr, uae_u32 v) { get_mem_bank(addr).wput(addr, v); }
void memory_put_long(uaecptr addr, uae_u32 v) { get_mem_bank(addr).lput(addr, v); }

uae_u8 *memory_get_real_address(uaecptr addr)
{
    addrbank *ab = mem_banks[bankindex(addr)];
    if (ab && ab->xlateaddr)
        return ab->xlateaddr(addr);
    if (ab && ab->baseaddr)
        return ab->baseaddr + (addr - ab->start);
    return NULL;
}
int memory_valid_address(uaecptr addr, uae_u32 size)
{
    addrbank *ab = mem_banks[bankindex(addr)];
    return (ab && ab->check) ? ab->check(addr, size) : 0;
}

/* ================================================================= *
 * 3. Globals referenced by the core but owned by the Amiga machine layer.
 *    Types are taken from the Amiberry headers; zero is the safe default.
 * ================================================================= */
int config_changed = 0;
int config_changed_flags = 0;
int quit_program = 0;
int savestate_state = 0;
int special_mem = 0;
int special_mem_default = 0;
int uae_boot_rom_type = 0;
int vpos = 0;
int input_record = 0;
int input_play = 0;
int inputrecord_debug = 0;
uae_u16 dmacon = 0;
uae_u16 intena = 0, intreq = 0, intreqr = 0;
uae_u32 hsync_counter = 0, vsync_counter = 0;
uaecptr rtarea_base = 0;
volatile uae_atomic uae_int_requested = 0;
bool cloanto_rom = false;

/* CE (cycle-exact) memory-type tables — indexed by 64KB bank; unused on Atari. */
uae_u8 ce_banktype[65536];
uae_u8 ce_cachable[65536];

/* GUI status block + savestate path (osdep/gui layer). */
struct gui_info gui_data;
TCHAR savestate_fname[MAX_DPATH];

/* NOTE: start_pc / start_pc_p are NOT defined here. On the AArch64 JIT build
 * compemu_support.cpp defines them; defining them here too gave "multiple
 * definition" at link. The `prop` symbol mentioned earlier did not surface at
 * the real link, so it's left out. */
#if (0)
/* A fallback bank that never gets hit on the Atari (pistorm_natmem.cpp maps
 * real banks over the whole space). Give it harmless accessors anyway. */
static uae_u32 dummy_get(uaecptr)
{
    printf("dummy_get should not be here\n");
    return 0;
}
static void dummy_put(uaecptr, uae_u32) { printf("dummy_put should not be here\n"); }
static int dummy_check(uaecptr, uae_u32)
{
    printf("dummy_check should not be here\n");
    return 0;
}
static uae_u8 *dummy_xlate(uaecptr)
{
    printf("dummy_xlate should not be here\n");
    return NULL;
}
addrbank dummy_bank = {
    dummy_get, dummy_get, dummy_get, /* lget, wget, bget */
    dummy_put, dummy_put, dummy_put, /* lput, wput, bput */
    dummy_xlate, dummy_check, NULL, NULL, NULL,
    dummy_get, dummy_get, /* lgeti, wgeti */
    ABFLAG_NONE, 0, 0, 0};
#endif
/* ================================================================= *
 * 3b. JIT-side globals normally defined in memory.cpp / events.cpp /
 *     vm.cpp (none of which we compile). Signatures/types verified by
 *     nm on the AArch64 objects + the Amiberry headers.
 * ================================================================= */

/* Memory-access policy for the JIT. Setting jit_n_addr_unsafe != 0 (and
 * canbang false) forces compemu_support down the SAFE path: every guest
 * read/write is emitted as a call to the memory-bank handler instead of a
 * direct natmem load/store. That's slower, but it's mandatory for bring-up
 * because our hardware I/O ($FFxxxx) lives behind handlers that hit the real
 * bus — direct natmem access would bypass them. Once ST-RAM-only direct access
 * is wired up safely these can be relaxed. */
bool canbang = true;       // default false
int jit_n_addr_unsafe = 1; // default 1 - still needed for now

/* events.h cycle bookkeeping (events.cpp not compiled). */
int pissoff = 0;
int pissoff_value = 0;
int pissoff_nojit_value = 0;

/* Amiga ROM/autoconfig banks. They don't exist on the Atari, but compile_block
 * compares a block's host address against kickmem_bank.baseaddr / rtarea_bank
 * .baseaddr. Zero-initialised => baseaddr is NULL, so (a) the rtarea test is
 * explicitly skipped (it guards on baseaddr != 0) and (b) real host code
 * pointers never fall in [0, 8*64K), so the kickmem test never matches. */
addrbank kickmem_bank = {};
addrbank rtarea_bank = {};

/* JIT fatal-error hook (declared in compemu_arm.h as void(const TCHAR*,...)).
 * Normally calls uae_reset(); during bring-up we want a loud, debuggable stop. */
void jit_abort(const TCHAR *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

/* Executable-memory allocator for the JIT translation cache. memory.cpp's
 * version wraps mmap; we provide the same. PROT_EXEC is required — this is the
 * buffer the recompiler writes AArch64 code into and then jumps to. */
void *uae_vm_alloc(size_t size, int flags, int protect)
{
    (void)flags;
    int prot = 0;
    if (protect & UAE_VM_READ)
        prot |= PROT_READ;
    if (protect & UAE_VM_WRITE)
        prot |= PROT_WRITE;
    if (protect & UAE_VM_EXECUTE)
        prot |= PROT_EXEC;
    if (prot == 0)
        prot = PROT_READ | PROT_WRITE;
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

bool uae_vm_free(void *address, size_t size)
{
    return munmap(address, size) == 0;
}

/* ================================================================= *
 * 4. Amiga machine-layer no-ops. Never exercised on the Atari pistorm
 *    (no Amiga custom chips, no GUI, no savestates, no input recording,
 *    no cycle-exact chipset). Signatures match the Amiberry headers.
 * ================================================================= */

/* --- GUI / user notification --- */
void gui_message(const TCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void gui_led(int, int, int) {}
void notify_user(int) {}
void statusline_clear(void) {}
bool is_mainthread(void) { return true; }

/* --- Amiga custom-chip interrupt registers --- */
bool INTREQ_0(uae_u16) { return false; }
void INTREQ_f(uae_u16) {}
void INTREQ_INT(int, int) {}
void event_doint_delay_do_ext(uae_u32) {}

/* --- audio --- */
void audio_deactivate(void) {}
bool audio_finish_pull(void) { return false; }
void check_prefs_changed_audio(void) {}

/* --- blitter / custom chipset --- */
int blitnasty(void) { return 0; }
void custom_cpuchange(void) {}
void custom_end_drawing(void) {}
void custom_prepare(void) {}
void custom_reset(bool, bool) {}
void init_custom(void) {}
void reset_frame_rate_hack(void) {}
bool isvsync_chipset(void) { return false; }
void vsync_clear(void) {}
void vsync_event_done(void) {}
int vsync_isdone(frame_time_t *) { return 0; }
void send_internalevent(int) {}

/* --- cycle-exact chipset access (we are never cycle-exact) --- */
void do_cycles_ce(int) {}
int do_cycles_cck(int c) { return c; }
void do_cycles_ce020(int) {}
bool is_cycle_ce(uaecptr) { return false; }
uae_u32 wait_cpu_cycle_read(uaecptr addr, int) { return memory_get_long(addr); }
uae_u32 wait_cpu_cycle_read_ce020(uaecptr addr, int) { return memory_get_long(addr); }
void wait_cpu_cycle_write(uaecptr addr, int, uae_u32 v) { memory_put_long(addr, v); }
void wait_cpu_cycle_write_ce020(uaecptr addr, int, uae_u32 v) { memory_put_long(addr, v); }

/* --- ROM / boot rom / accelerator board --- */
void a3000_fakekick(int) {}
void protect_roms(bool) {}
uaecptr need_uae_boot_rom(struct uae_prefs *) { return 0; }
bool cpuboard_fc_check(uaecptr, uae_u32 *, int, bool) { return false; }
bool cpuboard_forced_hardreset(void) { return false; }
uaecptr cpuboard_get_reset_pc(uaecptr *stack)
{
    if (stack)
        *stack = 0;
    return 0;
}

/* --- prefs / cpu fixups --- */
void fixup_cpu(struct uae_prefs *) {}
void set_config_changed(int) {}
void expansion_cpu_fallback(void) {}

/* --- FPU native path (we use softfloat: fp_init_softfloat lives in fpp_softfloat.cpp) --- */
/* not anymore :) hard FPU now works */
// void fp_init_native(void) { }

/* --- memory map maintenance --- */
void memory_clear(void) {}
void memory_restore(void) {}
void restore_banks(void) {}
void mman_set_barriers(bool) {}

/* --- savestate --- */
bool savestate_check(void) { return false; }
void savestate_init(void) {}
uae_u32 get_statefile_version(void) { return 0; }
uae_u16 restore_u16_func(uae_u8 **p)
{
    uae_u16 v = ((*p)[0] << 8) | (*p)[1];
    *p += 2;
    return v;
}
uae_u32 restore_u32_func(uae_u8 **p)
{
    uae_u32 v = ((*p)[0] << 24) | ((*p)[1] << 16) | ((*p)[2] << 8) | (*p)[3];
    *p += 4;
    return v;
}
void save_u16_func(uae_u8 **p, uae_u16 v)
{
    (*p)[0] = v >> 8;
    (*p)[1] = (uae_u8)v;
    *p += 2;
}
void save_u32_func(uae_u8 **p, uae_u32 v)
{
    (*p)[0] = v >> 24;
    (*p)[1] = v >> 16;
    (*p)[2] = v >> 8;
    (*p)[3] = (uae_u8)v;
    *p += 4;
}

/* --- input recording --- */
int inprec_open(const TCHAR *, const TCHAR *) { return 0; }
bool inprec_prepare_record(const TCHAR *) { return false; }
void inprec_startup(void) {}
void inprec_playdebug_cpu(int, uae_u16) {}
void inprec_recorddebug_cpu(int, uae_u16) {}

/* --- reset / restart --- */
void uae_reset(int, int) {}
void uae_restart(struct uae_prefs *, int, const TCHAR *) {}

/* --- target/host services --- */
frame_time_t uae_time(void) { return 0; }
void target_cpu_speed(void) {}
int target_get_display_scanline(int) { return -1; }
int sleep_millis_main(int) { return 0; }
void warpmode(int) {}

/* --- PRNG (uae_rand) --- */
static uae_u32 s_rngseed = 0x12345678u;
uae_u32 uaerand(void)
{
    s_rngseed = s_rngseed * 1103515245u + 12345u;
    return (s_rngseed >> 16) & 0x7fff;
}
uae_u32 uaerandgetseed(void) { return s_rngseed; }
void uaerandomizeseed(void) { s_rngseed ^= (uae_u32)time(NULL); }
uae_u32 uaesetrandseed(uae_u32 s)
{
    uae_u32 old = s_rngseed;
    s_rngseed = s;
    return old;
}

/* --- TCHAR<->char conversion. TCHAR == char here, so these just dup/copy. --- */
char *ua(const TCHAR *s) { return s ? strdup(s) : NULL; }
TCHAR *au(const char *s) { return s ? strdup(s) : NULL; }
char *ua_fs(const TCHAR *s, int defchar)
{
    (void)defchar;
    return s ? strdup(s) : NULL;
}
TCHAR *au_fs(const char *s) { return s ? strdup(s) : NULL; }
char *ua_copy(char *dst, int maxlen, const TCHAR *src)
{
    if (dst && maxlen > 0)
    {
        snprintf(dst, maxlen, "%s", src ? src : "");
    }
    return dst;
}
TCHAR *au_copy(TCHAR *dst, int maxlen, const char *src)
{
    if (dst && maxlen > 0)
    {
        snprintf(dst, maxlen, "%s", src ? src : "");
    }
    return dst;
}
