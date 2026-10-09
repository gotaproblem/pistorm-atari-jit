/* SPDX-License-Identifier: MIT
 *
 * stbox_psgfx.h - PSGFX inside STBOX: GPU re-rendering of a game's own 3D,
 * at the display rate, without touching the game.
 *
 * HOW. A hook profile (configs/psgfx/<game>.hooks, written by psgfx-scan)
 * names the PCs in the game's 3D pipeline - projection, face emitters, the
 * start and end of a frame's draw list - and, for each, what to record from
 * the box's registers and RAM. The box's Musashi already calls
 * stbox_pc_hook() before every instruction; when a profile is armed that
 * hook tests one bit per even PC and, on a hit, evaluates the profile's
 * expressions into a small record ring (core 3: memory only, bounded).
 *
 * The render thread (cores 0/1) turns the records into camera-space faces
 * per game frame, waits until the box actually displays that frame (the
 * scanline capture names the screen base it showed), takes the face colours
 * and the sky/ground background from that displayed picture, and draws the
 * scene on the GPU (GLES 3.1, EGL surfaceless, loaded with dlopen - no build
 * dependency, and a box without libEGL simply stays on the normal path).
 * Between two game frames it interpolates every matched vertex in camera
 * space, so an 8 fps game is drawn at 50/60 Hz, one game frame late.
 *
 * The box still draws its own screen (shadow mode): emulated timing is
 * exactly the original's, and any frame the lift cannot explain falls back
 * to the normal picture.
 *
 * Enable: env PISTORM_STBOX_PSGFX = a .hooks file or a directory of them
 * (default: configs/psgfx relative to the emulator's working directory if it
 * exists; "0" disables). A profile arms only when its expect= code bytes are
 * present at every hooked PC, so other programs never trigger it.
 * PISTORM_STBOX_PSGFX_HZ = output rate (default 50; 60 for a 60 Hz desktop).
 */
#ifndef PISTORM_STBOX_PSGFX_H
#define PISTORM_STBOX_PSGFX_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hook profiles: expressions over the box's registers and RAM -------- */
#define PH_MAXARGS 10
#define PH_MAXCODE 48
#define PH_MAXHOOK 32

typedef struct { int16_t op; int32_t v; } ph_ins;
typedef struct { ph_ins c[PH_MAXCODE]; int n; } ph_expr;
typedef struct {
    uint32_t pc;
    int op;
    int has_when; ph_expr when;
    int nargs; ph_expr arg[PH_MAXARGS];
    uint8_t expect[8]; int nexpect;
} ph_hook;
typedef struct {
    ph_hook h[PH_MAXHOOK]; int n;
    char name[64];                         /* profile file name             */
    char lift[32];                         /* "#lift <kind>" line: sg2      */
    uint8_t bitmap[(1 << 24) / 2 / 8];     /* one bit per even PC           */
} ph_table;

int  ph_add(ph_table *t, const char *line, char *err, int errlen);
int  ph_verify(const ph_table *t, const uint8_t *ram, uint32_t ramsz);
static inline int ph_test(const ph_table *t, uint32_t pc)
{
    pc &= 0xffffff; return (t->bitmap[pc >> 4] >> ((pc >> 1) & 7)) & 1;
}

/* ---- core 3 -> render thread record ring -------------------------------- */
typedef struct {
    uint32_t vbl;                          /* stbox_shared.frame at the hit */
    int16_t  op, n;
    int32_t  a[PH_MAXARGS];
} stbox_psgfx_rec;
#define STBOX_PSGFX_RING 16384             /* power of two                  */
extern stbox_psgfx_rec stbox_psgfx_ring[STBOX_PSGFX_RING];
extern volatile uint32_t stbox_psgfx_head;    /* written by core 3 only     */
extern volatile uint32_t stbox_psgfx_tail;    /* written by the host only   */
extern volatile uint32_t stbox_psgfx_drops;   /* ring full: records lost    */

/* armed profile: the host publishes the table, then sets the flag; core 3
 * tests the flag (one load) in stbox_pc_hook and never writes either */
extern const ph_table *volatile stbox_psgfx_table;
extern volatile int stbox_psgfx_armed;
void stbox_psgfx_fire(uint32_t pc);           /* core 3, on a bitmap hit    */

static inline void stbox_psgfx_pc(uint32_t pc)
{
    if (stbox_psgfx_armed) {
        const ph_table *t = stbox_psgfx_table;
        if (ph_test(t, pc)) stbox_psgfx_fire(pc);
    }
}

/* Per captured frame: which screen base the box displayed, at which VBL.
 * Written by core 3 next to stbox_cap[] (kept out of stbox.h so the change
 * does not rebuild everything that includes it). */
typedef struct { uint32_t base, vbl; } stbox_cap_meta_t;
extern stbox_cap_meta_t stbox_cap_meta[];

/* ---- render thread ------------------------------------------------------- */
/* load profiles (once per box start); 0 = PSGFX off */
int  stbox_psgfx_init(void);
void stbox_psgfx_shutdown(void);
/* every render-loop pass: arm/disarm by expect bytes, drain the ring, lift
 * frames whose picture has been displayed. Cheap when nothing changed. */
void stbox_psgfx_poll(void);
/* 1 if a lifted scene is ready to draw (two game frames lifted, profile armed,
 * GPU up) */
int  stbox_psgfx_active(void);
/* draw the scene at box time T (VBLs, fractional) into dst (w x h pixels,
 * pitch in bytes; bgra=1 for XRGB8888 scanout, 0 for RGBA bytes).
 * Returns 0 on success. */
int  stbox_psgfx_render(double box_time, uint8_t *dst, int w, int h, int pitch, int bgra);
/* output rate wanted by the render loop (Hz) */
int  stbox_psgfx_hz(void);
/* the render thread is exiting: release the GL context from it (a restarted
 * box makes it current on its new render thread) */
void stbox_psgfx_thread_exit(void);
/* stats line for the health log */
void stbox_psgfx_stats(char *buf, int n);

#ifdef __cplusplus
}
#endif
#endif
