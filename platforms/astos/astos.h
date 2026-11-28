/*
 * astos.h - ASTOS, the Atari ST Operating System front end.
 *
 * ASTOS is what the machine boots into. It runs on the Pi with the 68k
 * halted, draws its desktop on the real Shifter (medium resolution on a
 * colour monitor, high on a mono one) and launches the psctrl.cfg builds
 * - the JIT emulator's builds and stboxmetal - which take the whole
 * screen until they end, when run-pistorm.sh brings ASTOS back.
 *
 * Design: doc/ASTOS.md (and astos-design.md in the project).
 *
 * Everything is drawn into a LOGICAL canvas of 640x200 pixels, one byte
 * a pixel holding a logical colour (C_*). astos_gfx.c packs it into the
 * Shifter's planar layout: 2 planes for medium, and for mono every
 * logical row becomes two 640x400 rows, so one layout serves both
 * monitors and keeps the medium-resolution 2:1 pixel shape.
 */
#ifndef ASTOS_H
#define ASTOS_H

#include <stdint.h>
#include "shifter_setup.h"

#define ASTOS_VERSION "0.1"

#define AW 640                      /* logical canvas                  */
#define AH 200

/* logical colours - mapped to Shifter pens per mode in astos_gfx.c */
enum {
    C_FACE = 0,     /* panels, taskbar, windows            */
    C_TEXT,         /* text, frames                        */
    C_ACCENT,       /* selection, title bars, start button */
    C_WHITE,        /* window interiors, text on accent    */
    C_DESK,         /* the desktop background              */
    C_DIM,          /* secondary text                      */
    C_NCOL
};

/* ---- astos_gfx.c ---------------------------------------------------- */
extern uint8_t g_px[AW * AH];

void g_clear(int c);
void g_fill(int x, int y, int w, int h, int c);
void g_hline(int x, int y, int w, int c);
void g_vline(int x, int y, int h, int c);
void g_frame(int x, int y, int w, int h, int c);
void g_text(int x, int y, const char *s, int c);     /* transparent    */
void g_textn(int x, int y, const char *s, int max_chars, int c);
int  g_text_w(const char *s);
void g_icon(int x, int y, int kind);                 /* 32x16 logical  */

/* palette + pack + pointer */
void g_set_mode(int mono);
int  g_mono(void);
void g_palette_st(uint16_t pal[4]);                  /* ST $0RGB       */
void g_palette_rgb(uint32_t rgb[4]);                 /* for HDMI/shots */
void g_pack(struct ss_screen *ss);                   /* canvas -> base */
void g_compose(struct ss_screen *ss, int mx, int my_phys, int show_ptr);
int  g_screenshot(const struct ss_screen *ss, const char *path);

/* ---- icons ---------------------------------------------------------- */
enum { K_TOS = 0, K_METAL, K_APJ, K_NATIVE, K_HDMI, K_NKIND };

/* ---- astos_env.c ---------------------------------------------------- */
#define MAX_ENV 16
struct env {
    char sec[16];           /* psctrl.cfg section name           */
    char title[24];         /* shown under the icon              */
    int  kind;              /* K_*                               */
    int  metal;             /* engine metal                      */
    char line[6][48];       /* the details panel                 */
};

struct sc_cfg;
int  env_load(struct sc_cfg *c, struct env *out, int max);
int  env_launch(struct sc_cfg *c, const char *cfg_path, const char *sec);
#define ASTOS_LAUNCH_FILE "/run/astos-launch"
#define ASTOS_EXIT_LAUNCH  44       /* run-pistorm.sh: start the build   */
#define ASTOS_EXIT_RESTART 42       /* run-pistorm.sh: start ASTOS again */
#define ASTOS_EXIT_HALT    43       /* run-pistorm.sh: shut the Pi down  */

/* ---- astos_sys.c ---------------------------------------------------- */
int  sys_temp_c10(void);            /* SoC temperature in 0.1 C, -1     */
int  sys_throttled(void);           /* vcgencmd get_throttled bits, -1  */
void sys_clock(char *buf, int n, int h12);
void sys_date(char *buf, int n, int mdy);

#endif
