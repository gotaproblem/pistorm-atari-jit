/* SPDX-License-Identifier: MIT
 *
 * harness.c - headless STBOX batch runner (any host, no Pi, no DRM/SDL).
 *
 * Drives stbox_slice() with a synthetic arch timer so the sandbox runs as
 * fast as Musashi can go, boots a TOS image with a game disk in drive A,
 * pokes the keyboard/joystick now and then to get past title screens, and
 * reports what the box's own forensics report on the Pi: exceptions (with
 * disassembly), trace count, halts, and whether the screen keeps changing.
 *
 *   harness --tos ROM --disk IMG [--ste] [--secs N] [--ram KB] [--nokeys]
 *
 * Verdict line at the end: RUNNING / STUCK / CRASH / HALT, plus the facts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>

#include "stbox.h"
#include "m68k.h"

volatile int stbox_errand_active;              /* real-FDC bridge: absent */

/* disk loaders (.ST raw, .MSA) verbatim from stbox_host.c */
#include "diskload.inc"

#define CNTFRQ 54000000ull
#define TICKS_PER_CALL 431            /* ~64 guest cycles at 8.021248 MHz */

static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static uint32_t screen_hash(void)
{
    uint32_t vb = stbox_shared.video_base & (stbox_shared.ram_size - 1);
    if (vb > stbox_shared.ram_size - 32000) vb = stbox_shared.ram_size - 32000;
    return fnv(stbox_shared.ram + vb, 32000) ^ (uint32_t)stbox_shared.shift_res;
}

/* ---- input script and frame dumps (PSGFX work) -------------------------
 * script lines, frames = box VBL count (stbox_shared.frame):
 *   K f0 f1 scancode        key down at f0, up at f1
 *   J f0 f1 hex             joystick (port 1) state from f0, released at f1
 *   M f0 f1 buttons         mouse buttons (bit1 L, bit0 R) down f0, up f1
 *   D f0 f1 dx dy           mouse moves dx,dy every frame from f0 to f1
 */
typedef struct { char t; uint32_t f0, f1; int a, b; } scr_ev;
static scr_ev g_scr[4096]; static int g_nscr;
static void script_load(const char *path)
{
    FILE *f = fopen(path, "r"); char ln[256];
    if (!f) { perror(path); exit(2); }
    while (fgets(ln, sizeof ln, f) && g_nscr < 4096) {
        scr_ev e = {0}; char t;
        if (ln[0] == '#' || sscanf(ln, " %c", &t) != 1) continue;
        e.t = t;
        if (t == 'J') { unsigned v; if (sscanf(ln + 1, "%u %u %x", &e.f0, &e.f1, &v) == 3) { e.a = (int)v; g_scr[g_nscr++] = e; } }
        else if (t == 'D') { if (sscanf(ln + 1, "%u %u %d %d", &e.f0, &e.f1, &e.a, &e.b) == 4) g_scr[g_nscr++] = e; }
        else if (sscanf(ln + 1, "%u %u %d", &e.f0, &e.f1, &e.a) == 3) g_scr[g_nscr++] = e;
    }
    fclose(f);
}
static void script_frame(uint32_t fr)
{
    static int mbut;
    for (int i = 0; i < g_nscr; i++) {
        scr_ev *e = &g_scr[i];
        switch (e->t) {
        case 'K': if (fr == e->f0) stbox_key_event((uint8_t)e->a, 1); if (fr == e->f1) stbox_key_event((uint8_t)e->a, 0); break;
        case 'J': if (fr == e->f0) stbox_joy_event(1, (uint8_t)e->a); if (fr == e->f1) stbox_joy_event(1, 0); break;
        case 'M': if (fr == e->f0) { mbut = e->a; stbox_mouse_rel(0, 0, mbut); }
                  if (fr == e->f1) { mbut = 0; stbox_mouse_rel(0, 0, 0); } break;
        case 'D': if (fr >= e->f0 && fr <= e->f1) stbox_mouse_rel(e->a, e->b, mbut); break;
        }
    }
}
/* the newest complete captured frame as 320x200 RGB (low res only) */
static const uint8_t lvl8[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
int harness_cap_rgb(uint8_t *rgb)
{
    uint32_t idx = stbox_shared.cap_idx;
    if (idx >= STBOX_CAP_BUFS) return 0;
    const stbox_cap_t *c = &stbox_cap[idx];
    if (c->res != 0) return 0;
    for (int y = 0; y < 200; y++) {
        const uint8_t *s = c->pix + (size_t)y * STBOX_CAP_PITCH;
        for (int g = 0; g < 20; g++, s += 8) {
            uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3], p2 = (s[4] << 8) | s[5], p3 = (s[6] << 8) | s[7];
            for (int b = 15; b >= 0; b--) {
                int ci = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3);
                uint16_t p = c->pal[y][ci]; uint8_t *d = rgb + ((size_t)y * 320 + g * 16 + 15 - b) * 3;
                d[0] = lvl8[(p >> 8) & 7]; d[1] = lvl8[(p >> 4) & 7]; d[2] = lvl8[p & 7];
            }
        }
    }
    return 1;
}
static void dump_ppm(const char *dir, uint32_t fr)
{
    static uint8_t rgb[320 * 200 * 3];
    if (!harness_cap_rgb(rgb)) return;
    char p[512]; snprintf(p, sizeof p, "%s/f%06u.ppm", dir, fr);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n320 200\n255\n"); fwrite(rgb, 1, sizeof rgb, f); fclose(f);
}

#ifdef HARNESS_PSGFX
/* PSGFX in the harness: the render-thread code runs here on the harness
 * thread once per box VBL (EGL surfaceless - llvmpipe on a PC, V3D on a Pi)
 * and writes the GPU frames (1280x800 RGBA) and the box's own picture
 * (320x200 RGB) for comparison. HARNESS_PSGFX_OUT=prefix, _FROM/_TO frames. */
#include "stbox_psgfx.h"
static void harness_psgfx_frame(uint32_t fr)
{
    static int init = -1; static FILE *ff; static uint8_t *buf, *pane; static uint32_t from, to, n;
    if (init < 0) {
        init = stbox_psgfx_init();
        const char *o = getenv("HARNESS_PSGFX_OUT");          /* .mp4: ST | GPU side by side */
        from = getenv("HARNESS_PSGFX_FROM") ? (uint32_t)atoi(getenv("HARNESS_PSGFX_FROM")) : 0;
        to = getenv("HARNESS_PSGFX_TO") ? (uint32_t)atoi(getenv("HARNESS_PSGFX_TO")) : 0xffffffffu;
        if (o) {
            char cmd[1024];
            snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s 2560x800 -r 50 -i - "
                     "-vf scale=1920:600:flags=lanczos -c:v libx264 -crf 18 -pix_fmt yuv420p '%s'", o);
            ff = popen(cmd, "w");
        }
        buf = malloc(1280 * 800 * 4); pane = malloc(2560 * 800 * 4);
    }
    if (!init) return;
    stbox_psgfx_poll();
    if (fr < from || fr > to || !ff) goto stats;
    {
        static uint8_t st[320 * 200 * 3];
        int ok = stbox_psgfx_active() && stbox_psgfx_render((double)fr, buf, 1280, 800, 1280 * 4, 0) == 0;
        if (!ok) memset(buf, 0, 1280 * 800 * 4);
        harness_cap_rgb(st);
        for (int y = 0; y < 800; y++) {
            uint8_t *d = pane + (size_t)y * 2560 * 4;
            for (int x = 0; x < 1280; x++) { const uint8_t *c = st + ((y / 4) * 320 + x / 4) * 3; d[x * 4] = c[0]; d[x * 4 + 1] = c[1]; d[x * 4 + 2] = c[2]; d[x * 4 + 3] = 255; }
            memcpy(d + 1280 * 4, buf + (size_t)y * 1280 * 4, 1280 * 4);
        }
        fwrite(pane, 1, (size_t)2560 * 800 * 4, ff);
        n += ok;
        if (fr == to) { pclose(ff); ff = NULL; }
    }
stats:
    if (fr % 500 == 0) { char sb[192]; stbox_psgfx_stats(sb, sizeof sb); printf("  frame %u %s (%u GPU frames)\n", fr, sb, n); }
}
#endif

static const char *vecname(uint32_t v)
{
    switch (v) { case 2: return "BUS"; case 3: return "ADDR"; case 4: return "ILLEGAL";
                 case 8: return "PRIV"; default: return "?"; }
}

int main(int argc, char **argv)
{
    const char *tos = NULL, *disk = NULL;
    int ste = 0, secs = 60, ram_kb = 1024, keys = 1, verbose = 0, dump_every = 50;
    const char *dump_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tos") && i + 1 < argc) tos = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--ste")) ste = 1;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram_kb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nokeys")) keys = 0;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_load(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    if (!tos) { fprintf(stderr, "need --tos\n"); return 2; }

    /* ROM */
    static uint8_t rom[512 * 1024];
    FILE *f = fopen(tos, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", tos, strerror(errno)); return 2; }
    size_t rsz = fread(rom, 1, sizeof rom, f);
    fclose(f);

    uint32_t ram_size = (uint32_t)ram_kb * 1024u;
    uint8_t *ram = calloc(1, ram_size + 32768);
    stbox_shared.ram = ram; stbox_shared.ram_size = ram_size;

    stbox_core_set_machine(ste);
    if (stbox_core_setup(ram, ram_size, rom, (uint32_t)rsz)) {
        fprintf(stderr, "core setup failed\n"); return 2;
    }
    if (disk) {
        uint32_t dsz = 0;
        uint8_t *img = disk_load(disk, &dsz);
        if (!img) { fprintf(stderr, "disk load failed\n"); return 2; }
        stbox_core_disk_insert(img, dsz);          /* engine parked: synchronous */
    }

    uint64_t now = 1000;
    /* watch writes to the trace vector ($24..$27) */
    const char *w = getenv("HARNESS_WATCH");          /* lo[:hi] hex */
    uint32_t wlo = 0x24, whi = 0x28;
    if (w) { char *c = NULL; wlo = (uint32_t)strtoul(w, &c, 16);
             whi = (c && *c == ':') ? (uint32_t)strtoul(c + 1, NULL, 16) : wlo + 4; }
    stbox_watch_lo = wlo; stbox_watch_hi = whi;
    unsigned watch_seen = 0;
    #define FDC_MAX 200000
    static struct { unsigned sec; uint32_t dst, off; uint8_t cmd; } fdc_log[FDC_MAX];
    unsigned nfdc = 0;
    stbox_core_arm(now, CNTFRQ);

    /* run */
    uint32_t last_hash = 0, last_change_sec = 0;
    unsigned exc_seen = 0, trace_seen = 0, inner_exc_seen = 0;
    unsigned exc_by_vec[16] = {0};
    uint32_t last_fatal_ppc = 0, last_fatal_vec = 0; unsigned last_fatal_sec = 0;
    int halted = 0;
    unsigned key_phase = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

    for (unsigned sec = 1; sec <= (unsigned)secs; sec++) {
        uint32_t target = sec * 50;
        unsigned long guard = 0;
        while (stbox_shared.frame < target) {
            uint32_t fr_before = stbox_shared.frame;
            stbox_slice(now);
            now += TICKS_PER_CALL;
            if (stbox_shared.frame != fr_before) {
                uint32_t fr = stbox_shared.frame;
                if (g_nscr) script_frame(fr);
                if (dump_dir && dump_every > 0 && fr % (uint32_t)dump_every == 0) dump_ppm(dump_dir, fr);
#ifdef HARNESS_PSGFX
                harness_psgfx_frame(fr);
#endif
            }
            {
                unsigned wi = stbox_watch_idx;
                for (unsigned i = watch_seen; i != wi; i++) {
                    stbox_watch_ev ev = stbox_watch_ring[i & (STBOX_WATCH_RING - 1)];
                    if ((ev.pc & 0xFFFF00) == 0xFDC000) {
                        if (nfdc < FDC_MAX) { fdc_log[nfdc].sec = sec; fdc_log[nfdc].dst = ev.addr;
                                              fdc_log[nfdc].off = ev.val; fdc_log[nfdc].cmd = ev.pc & 0xFF; nfdc++; }
                    } else {
                        char d[96]; d[0] = 0;
                        m68k_disassemble(d, ev.pc, M68K_CPU_TYPE_68000);
                        printf("  %3us WATCH write %06X <- %08X by pc=%06X [%s]\n", sec, ev.addr, ev.val, ev.pc, d);
                    }
                }
                watch_seen = wi;
            }
            if (stbox_exc_count != inner_exc_seen) {
                inner_exc_seen = stbox_exc_count;
                stbox_exc_info_t e = stbox_exc_ring[(inner_exc_seen - 1) & (STBOX_EXC_RING - 1)];
                const char *at = getenv("HARNESS_AT");   /* only dump for this ppc */
                int want = !at || (uint32_t)strtoul(at, NULL, 16) == e.ppc;
                if (want && (e.vector == 3 || e.vector == 4 || e.vector == 8) && getenv("HARNESS_DASM")) {
                    /* HARNESS_DASM="start:end,start:end" hex - disassemble at the fault */
                    char spec[256]; strncpy(spec, getenv("HARNESS_DASM"), 255); spec[255] = 0;
                    for (char *s = strtok(spec, ","); s; s = strtok(NULL, ",")) {
                        char *c = strchr(s, ':'); if (!c) continue;
                        uint32_t a = (uint32_t)strtoul(s, NULL, 16), b = (uint32_t)strtoul(c + 1, NULL, 16);
                        printf("  --- disassembly %06X..%06X at the fault:\n", a, b);
                        while (a < b) { char d[96]; d[0] = 0;
                            int len = m68k_disassemble(d, a, M68K_CPU_TYPE_68000);
                            printf("   %06X  %s\n", a, d); a += len > 0 ? (uint32_t)len : 2u; }
                    }
                }
                if (want && (e.vector == 3 || e.vector == 4 || e.vector == 8) && getenv("HARNESS_PCHIST")) {
                    int N = atoi(getenv("HARNESS_PCHIST"));
                    unsigned idx = stbox_pc_ring_idx;
                    printf("  --- last %d PCs before vec%u at %06X (runs first..last xN, oldest first):\n", N, e.vector, e.ppc);
                    uint32_t run_a = 0, run_b = 0; int run_n = 0;
                    for (int k = N; k > 0; k--) {
                        uint32_t pc = stbox_pc_ring[(idx - (unsigned)k) & (STBOX_PC_RING - 1)];
                        if (run_n && pc >= run_a && pc <= run_b + 12 && pc >= run_b - 64) { if (pc > run_b) run_b = pc; run_n++; continue; }
                        if (run_n) printf("   %06X..%06X x%d\n", run_a, run_b, run_n);
                        run_a = run_b = pc; run_n = 1;
                    }
                    if (run_n) printf("   %06X..%06X x%d\n", run_a, run_b, run_n);
                }
            }
            if (stbox_core_take_halt_report()) { halted = 1; break; }
            if (++guard > 40000000ul) {            /* no VBLs at all: dead */
                fprintf(stderr, "  no VBL progress in second %u\n", sec);
                halted = 2; break;
            }
        }
        if (halted) {
            printf("  HALT at %us: double bus fault first=%06X second=%06X pc=%06X\n",
                   sec, stbox_halt_info.fault1, stbox_halt_info.fault2, stbox_halt_info.pc);
            break;
        }

        /* exceptions since last look */
        unsigned n = stbox_exc_count;
        if (n != exc_seen) {
            unsigned from = (n - exc_seen > STBOX_EXC_RING) ? n - STBOX_EXC_RING : exc_seen;
            for (unsigned i = from; i != n; i++) {
                stbox_exc_info_t e = stbox_exc_ring[i & (STBOX_EXC_RING - 1)];
                if (e.vector < 16) exc_by_vec[e.vector]++;
                if (e.vector == 3 || e.vector == 4 || e.vector == 8) {
                    last_fatal_ppc = e.ppc; last_fatal_vec = e.vector; last_fatal_sec = sec;
                }
                if (verbose || e.vector != 2) {
                    char d[96]; d[0] = 0;
                    m68k_disassemble(d, e.ppc, M68K_CPU_TYPE_68000);
                    printf("  %3us exc#%u vec%u %s ppc=%06X [%s] sr=%04X a0=%08X\n",
                           sec, i + 1, e.vector, vecname(e.vector), e.ppc, d, e.sr, e.a0);
                }
            }
            exc_seen = n;
        }
        if (stbox_trace_count != trace_seen) {
            if (verbose) printf("  %3us trace exceptions: %u\n", sec, stbox_trace_count);
            trace_seen = stbox_trace_count;
        }

        uint32_t h = screen_hash();
        if (h != last_hash) { last_hash = h; last_change_sec = sec; }

        if (verbose && (sec % 5) == 0)
            printf("  %3us frames=%u vidbase=%06X res=%u screen%s\n", sec,
                   stbox_shared.frame, stbox_shared.video_base, stbox_shared.shift_res,
                   last_change_sec == sec ? " changing" : " static");

        /* poke it: space / return / fire / '1', one every 3 s after boot */
        if (keys && sec >= 8 && (sec % 3) == 0) {
            switch (key_phase++ & 3) {
                case 0: stbox_key_event(0x39, 1); stbox_key_event(0x39, 0); break;   /* space  */
                case 1: stbox_key_event(0x1C, 1); stbox_key_event(0x1C, 0); break;   /* return */
                case 2: stbox_joy_event(1, 0x80); stbox_joy_event(1, 0x00); break;   /* fire   */
                default: stbox_key_event(0x02, 1); stbox_key_event(0x02, 0); break;  /* '1'    */
            }
        }
    }

#ifdef STBOX_FDC_TRACE
    if (getenv("HARNESS_FDCCMDS")) {
        extern struct { uint64_t cyc; uint8_t cmd, track, sector, data, side, was_pending, mode_hi; uint32_t dma, cnt; uint8_t result; } stbox_fdc_trace[];
        extern volatile unsigned stbox_fdc_trace_idx;
        unsigned lo = 0, hi = stbox_fdc_trace_idx;
        const char *r = getenv("HARNESS_FDCCMDS");   /* "from:to" indexes */
        if (r && strchr(r, ':')) { lo = (unsigned)atoi(r); hi = (unsigned)atoi(strchr(r, ':') + 1); if (hi > stbox_fdc_trace_idx) hi = stbox_fdc_trace_idx; }
        printf("  --- FDC commands %u..%u of %u:\n", lo, hi, stbox_fdc_trace_idx);
        for (unsigned i = lo; i < hi; i++) {
            __typeof__(stbox_fdc_trace[0]) *e = &stbox_fdc_trace[i & 65535];
            printf("   #%-5u cyc %10llu  cmd %02X  trk %2u sec %2u data %2u side %u  dma %06X cnt %u%s -> %s%02X\n",
                   i, (unsigned long long)e->cyc, e->cmd, e->track, e->sector, e->data, e->side, e->dma, e->cnt,
                   e->was_pending ? "  [BUSY:ignored]" : "", e->result == 0xFF ? "(pending)" : "st=", e->result);
        }
    }
#endif
    if (getenv("HARNESS_FDCMAP")) {
        printf("  --- FDC sector reads (grouped runs):\n");
        for (unsigned i = 0; i < nfdc; ) {
            unsigned j = i + 1;
            while (j < nfdc && fdc_log[j].sec == fdc_log[i].sec && fdc_log[j].cmd == fdc_log[i].cmd &&
                   fdc_log[j].dst == fdc_log[i].dst + 512u * (j - i) && fdc_log[j].off == fdc_log[i].off + 512u * (j - i)) j++;
            printf("   %3us  dst %06X..%06X  disk %06X  %4u sectors  cmd %02X\n", fdc_log[i].sec,
                   fdc_log[i].dst, fdc_log[i].dst + 512u * (j - i), fdc_log[i].off, j - i, fdc_log[i].cmd);
            i = j;
        }
        printf("  %u sectors total\n", nfdc);
    }
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double host_s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    unsigned ran = stbox_shared.frame / 50;

    const char *verdict;
    if (halted) verdict = "HALT";
    else if (last_fatal_sec && last_change_sec <= last_fatal_sec + 1 &&
             ran - last_change_sec >= 5) verdict = "CRASH";
    else if (ran - last_change_sec >= 10) verdict = "STUCK";
    else verdict = "RUNNING";

    char fatal[64] = "";
    if (last_fatal_sec)
        snprintf(fatal, sizeof fatal, " last-fatal=vec%u@%06X(%us)",
                 last_fatal_vec, last_fatal_ppc, last_fatal_sec);
    printf("VERDICT %-7s %s | %us guest in %.1fs host | bus=%u addr=%u ill=%u priv=%u trace=%u | "
           "screen last changed %us%s | vidbase=%06X res=%u\n",
           verdict, disk ? disk : "(no disk)", ran, host_s,
           exc_by_vec[2], exc_by_vec[3], exc_by_vec[4], exc_by_vec[8], stbox_trace_count,
           last_change_sec, fatal, stbox_shared.video_base, stbox_shared.shift_res);
    {   /* did the pokes reach the guest? acia_rx counts bytes the guest
         * actually read from $FFFC02 - the keyboard path end to end */
        uint32_t in[6];
        stbox_input_stats(in);
        printf("INPUT key=%u mouse=%u joy=%u raw=%u drop=%u acia_rx=%u\n",
               in[0], in[1], in[2], in[3], in[4], in[5]);
    }
    return 0;
}
