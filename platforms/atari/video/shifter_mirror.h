/* shifter_mirror.h - beam-exact HDMI copy of the ST Shifter (see shifter_mirror.c)
 *
 * Three sides:
 *   CPU thread   shm_io_write()  every guest write to $FF8200-$FF8265, after
 *                                the bus write (palette, sync, rez, base, STE
 *                                extras), stamped with the time it landed
 *                shm_ram_hit()/shm_log_ram()  ST-RAM writes that fall in the
 *                                screen being shown (or about to be)
 *                shm_cpu_vbl_taken()  the CPU took the VBL interrupt
 *                shm_cal_poll()  once a frame, read the real video counter
 *                                (governed 68000 only)
 *   ipl_task     shm_vbl_sighting()  the GLUE's level-4 line came on
 *   render       shm_render()    rebuild the frame just shown, line by line
 */
#ifndef SHIFTER_MIRROR_H
#define SHIFTER_MIRROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- event log --------------------------------------------------------- */
typedef struct {
    uint64_t t;          /* arch timer ticks (cntvct), when the write landed */
    uint32_t a;          /* kind << 24 | address (RAM) / register / index   */
    uint16_t o;          /* old value                                       */
    uint16_t n;          /* new value                                       */
} shm_ev_t;

#define SHM_K_RAM16  1u  /* a = ST address (even), o/n = word               */
#define SHM_K_RAM8   2u  /* a = ST address, o/n = byte                      */
#define SHM_K_REG    3u  /* a = $FF82xx low byte (0x01..0x65), o/n = byte   */
#define SHM_K_PAL    4u  /* a = colour index 0..15, o/n = word              */

#define SHM_KIND(a)  ((a) >> 24)
#define SHM_ADDR(a)  ((a) & 0x00FFFFFFu)

extern int shm_on;                       /* exact mirror active             */

/* ST-RAM write filter: [lo, lo+len) for the register base (0), the base
 * latched at the last VBL the CPU took (1) and the one before (2). All len
 * are 0 while the mirror is off, so the test is false without a flag. */
extern volatile uint32_t shm_rlo[3], shm_rlen[3];

static inline int shm_ram_hit(uint32_t a)
{
    return ((a - shm_rlo[0]) < shm_rlen[0]) |
           ((a - shm_rlo[1]) < shm_rlen[1]) |
           ((a - shm_rlo[2]) < shm_rlen[2]);
}

/* size 1 or 2 (callers split longs), BEFORE the mirror store */
void shm_log_ram(uint32_t a, int size, uint32_t oldv, uint32_t newv);

/* AFTER the bus write; a = 24-bit address, size 1/2/4 */
void shm_io_write(uint32_t a, uint32_t v, int size);

/* newcpu.cpp, when the level-4 autovector is taken */
void shm_cpu_vbl_taken(void);

/* events.cpp governor, every check (~4 us of guest time). Cheap test inline. */
extern volatile uint64_t shm_cal_lo;     /* next read window opens (ticks) */
void shm_cal_poll_slow(uint64_t now);

static inline uint64_t shm_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    extern uint64_t shm_ticks_host(void);
    return shm_ticks_host();
#endif
}

static inline void shm_cal_poll(void)
{
    if (__builtin_expect(shm_on, 0)) {
        uint64_t now = shm_ticks();
        if (now >= shm_cal_lo)
            shm_cal_poll_slow(now);
    }
}

/* ---- ipl_task ---------------------------------------------------------- */
void shm_vbl_sighting(void);

/* ---- render thread ----------------------------------------------------- */
/* natmem: host copy of ST-RAM; ste: shifter ste semantics.
 * Returns 0 once the log is allocated (shm_on), -1 on failure. */
int  shm_init(const uint8_t *natmem, int ste);

/* Rebuild the newest complete frame into dst (XRGB8888, pitch in pixels).
 *   1   drawn: *w x *h pixels, to be displayed with the shape *sw x *sh
 *   0   nothing new since the last call (keep what is on screen)
 *  -1   no frame model yet (no VBL lock) - caller uses its old path */
int  shm_render(uint32_t *dst, uint32_t pitch, uint32_t *w, uint32_t *h,
                uint32_t *sw, uint32_t *sh);

/* frame rate the model is locked to: 50, 60, 71, or 0 */
int  shm_locked_hz(void);

/* pistorm_natmem.cpp: read the real video counter ($FF8205/07/09).
 * *t = arch-timer ticks at the low-byte read. 0 = torn read, try later. */
int  pistorm_shm_read_counter(uint32_t *cnt, uint64_t *t);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTER_MIRROR_H */
