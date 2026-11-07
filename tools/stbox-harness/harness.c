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
 *           [--dump PREFIX]   (PREFIX.ppm + PREFIX.rows: the last frame drawn
 *                              exactly as the Pi draws it, rows summarised)
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

/* ---- frame dump: the newest capture rendered as the Pi shows it ---- */
#include "stbox_render.h"
static int render_newest(uint32_t *px, int *w, int *h)
{
    uint32_t idx = stbox_shared.cap_idx;
    if (idx >= STBOX_CAP_BUFS) return 0;
    return stbox_render_cap(&stbox_cap[idx], stbox_shared.ste, px, 1024, w, h);
}

/* PPM of the newest frame plus a row summary: identical rows folded, each
 * row as colour runs "RRGGBB x0-x1". */
static void dump_frame(const char *prefix)
{
    if (getenv("HARNESS_VIEW") && !strcmp(getenv("HARNESS_VIEW"), "monitor"))
        stbox_render_set_view(24, 384);
    static uint32_t px[1024 * 512];
    int w = 0, h = 0;
    memset(px, 0, sizeof px);
    if (!render_newest(px, &w, &h)) { printf("DUMP none\n"); return; }
    char fn[512];
    snprintf(fn, sizeof fn, "%s.ppm", prefix);
    FILE *f = fopen(fn, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * 1024 + x];
            fputc((v >> 16) & 255, f); fputc((v >> 8) & 255, f); fputc(v & 255, f);
        }
        fclose(f);
    }
    snprintf(fn, sizeof fn, "%s.rows", prefix);
    f = fopen(fn, "w");
    if (!f) return;
    fprintf(f, "frame %dx%d\n", w, h);
    char prev[4096] = "", cur[4096];
    int run0 = 0;
    for (int y = 0; y <= h; y++) {
        cur[0] = 0;
        if (y < h) {
            int x0 = 0; size_t n = 0;
            for (int x = 1; x <= w; x++) {
                uint32_t a = px[(size_t)y * 1024 + x0] & 0xFFFFFF;
                if (x == w || (px[(size_t)y * 1024 + x] & 0xFFFFFF) != a) {
                    if (n < sizeof cur - 32) n += (size_t)snprintf(cur + n, sizeof cur - n, " %06X %d-%d", a, x0, x - 1);
                    x0 = x;
                }
            }
        }
        if (y == h || strcmp(cur, prev)) {
            if (y) fprintf(f, "rows %3d-%3d:%s\n", run0, y - 1, prev);
            strcpy(prev, cur); run0 = y;
        }
    }
    fclose(f);
}

#ifdef STBOX_LINE_LOG
/* PREFIX.lines: every line of the last complete frame - vertical DE, rate
 * and resolution at the start, the sync/rez writes (cycle:rate/rez), what
 * the GLUE decided (first fetch/pixel cycle, bytes, rez) and the counter */
static void dump_lines(const char *prefix)
{
    char fn[512];
    snprintf(fn, sizeof fn, "%s.lines", prefix);
    FILE *f = fopen(fn, "w");
    if (!f) return;
    const unsigned fr = stbox_linelog_frame;
    const stbox_linelog_t *L = stbox_linelog[(fr + 1) & 1];  /* the complete one */
    fprintf(f, "frame %u (previous, complete)\n line vde len f0 r0 lin | fst pst bytes rez ext | vcnt   npev | writes c:f/r\n", fr - 1);
    for (int y = 0; y < 512; y++) {
        const stbox_linelog_t *l = &L[y];
        if (!l->len) continue;
        fprintf(f, "%4d %u %3u %2u %u %u | %3d %3d %3u %u %u | %06X %2u |", y, l->vde, l->len, l->f0, l->r0,
                l->left_in, l->fstart, l->pstart, l->bytes, l->rez, l->extra, l->vcnt & 0xFFFFFF, l->npev);
        for (int i = 0; i < l->nch; i++) fprintf(f, " %u:%u/%u", l->ch_c[i], l->ch_f[i], l->ch_r[i]);
        fputc('\n', f);
    }
    fclose(f);
}
#endif

/* ---- --wav FILE: the box's YM writes played through emu2149 (the synth
 * the Pi's stbox_psg.c uses) at their timestamps, 50 kHz mono 16-bit ---- */
#include "emu2149.h"
extern stbox_psg_ev stbox_psg_ring[STBOX_PSG_RING];
extern volatile unsigned stbox_psg_head, stbox_psg_tail;
static FILE *g_wav;
static PSG *g_wpsg;
static uint64_t g_wrender_ns;            /* render clock                   */
static uint32_t g_wav_frames;
static int32_t g_wdc, g_wacc; static int g_wsub;
static void wav_open(const char *fn)
{
    g_wav = fopen(fn, "wb");
    if (!g_wav) return;
    static const uint8_t hdr[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0, 1,0, 1,0, 0x50,0xC3,0,0, 0xA0,0x86,1,0, 2,0, 16,0, 'd','a','t','a',0,0,0,0 };
    fwrite(hdr, 1, 44, g_wav);
    g_wpsg = PSG_new(2000000u, 250000u);
    PSG_setVolumeMode(g_wpsg, 1);
    PSG_reset(g_wpsg);
}
static void wav_pump(uint64_t now_ticks)
{
    if (!g_wav) return;
    const uint64_t now_ns = now_ticks * 1000ull / (CNTFRQ / 1000000ull);
    const uint64_t lag = 30000000ull;                  /* as stbox_psg.c */
    if (now_ns < lag) return;
    unsigned t = stbox_psg_tail, h = stbox_psg_head;
    while (g_wrender_ns + 4000 <= now_ns - lag) {
        while (t != h) {
            stbox_psg_ev *e = &stbox_psg_ring[t & (STBOX_PSG_RING - 1)];
            if (e->ticks * 1000ull / (CNTFRQ / 1000000ull) > g_wrender_ns) break;
            PSG_writeReg(g_wpsg, e->reg, e->val);
            t++;
        }
        int32_t v = PSG_calc(g_wpsg);
        g_wdc += ((v << 12) - g_wdc) >> 12;
        g_wacc += v - (g_wdc >> 12);
        if (++g_wsub == 5) {                           /* 250 -> 50 kHz  */
            int32_t o = g_wacc / 5; g_wacc = 0; g_wsub = 0;
            if (o > 32767) o = 32767; if (o < -32768) o = -32768;
            int16_t sm = (int16_t)o; fwrite(&sm, 2, 1, g_wav); g_wav_frames++;
        }
        g_wrender_ns += 4000;
    }
    stbox_psg_tail = t;
}
static void wav_close(void)
{
    if (!g_wav) return;
    uint32_t d = g_wav_frames * 2, r = d + 36;
    fseek(g_wav, 4, SEEK_SET); fwrite(&r, 4, 1, g_wav);
    fseek(g_wav, 40, SEEK_SET); fwrite(&d, 4, 1, g_wav);
    fclose(g_wav);
}

static const char *vecname(uint32_t v)
{
    switch (v) { case 2: return "BUS"; case 3: return "ADDR"; case 4: return "ILLEGAL";
                 case 8: return "PRIV"; default: return "?"; }
}

int main(int argc, char **argv)
{
    const char *tos = NULL, *disk = NULL;
    int ste = 0, secs = 60, ram_kb = 1024, keys = 1, verbose = 0;
    const char *dump = NULL;     /* --dump PREFIX: PREFIX.ppm + PREFIX.rows at the end */
    const char *dumpat = NULL;   /* --dumpat 2,5,10: also PREFIX-<sec>.* at those seconds */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tos") && i + 1 < argc) tos = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--ste")) ste = 1;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram_kb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nokeys")) keys = 0;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--dumpat") && i + 1 < argc) dumpat = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_open(argv[++i]);
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
        /* HARNESS_BURST=N: call the slice every N x 64 cycles of host time,
         * as a busy ipl_task does on the Pi (bursts), instead of evenly */
        static int burst = -1;
        if (burst < 0) { const char *e = getenv("HARNESS_BURST"); burst = e ? atoi(e) : 1; if (burst < 1) burst = 1; }
        while (stbox_shared.frame < target) {
            stbox_slice(now);
            now += TICKS_PER_CALL * (unsigned)burst;
            wav_pump(now);
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

        if (getenv("HARNESS_PC")) {
            unsigned idx = stbox_pc_ring_idx;
            printf("  %3us pc:", sec);
            for (int k = 8; k > 0; k--) printf(" %06X", stbox_pc_ring[(idx - (unsigned)k) & (STBOX_PC_RING - 1)]);
            printf("\n");
        }
        if (dump && dumpat) {
            char list[256]; snprintf(list, sizeof list, ",%s,", dumpat);
            char key[16]; snprintf(key, sizeof key, ",%u,", sec);
            if (strstr(list, key)) {
                char pf[512]; snprintf(pf, sizeof pf, "%s-%02u", dump, sec);
                dump_frame(pf);
#ifdef STBOX_LINE_LOG
                dump_lines(pf);
#endif
            }
        }
        uint32_t h = screen_hash();
        if (h != last_hash) { last_hash = h; last_change_sec = sec; }

        if (verbose && (sec % 5) == 0)
            printf("  %3us frames=%u vidbase=%06X res=%u screen%s\n", sec,
                   stbox_shared.frame, stbox_shared.video_base, stbox_shared.shift_res,
                   last_change_sec == sec ? " changing" : " static");

        /* HARNESS_KEYS="6:31,9:39,12:F": at second 6 tap scancode $31 ('n'),
         * at 9 $39 (space), at 12 joystick fire. Replaces the default pokes. */
        const char *hk = getenv("HARNESS_KEYS");
        if (hk) {
            char buf[512]; snprintf(buf, sizeof buf, "%s", hk);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
                unsigned s2 = 0; char what[16] = "";
                if (sscanf(t, "%u:%15s", &s2, what) == 2 && s2 == sec) {
                    if (what[0] == 'F' && !what[1]) { stbox_joy_event(1, 0x80); stbox_joy_event(1, 0x00); }
                    else { unsigned sc = (unsigned)strtoul(what, NULL, 16);
                           stbox_key_event((uint8_t)sc, 1); stbox_key_event((uint8_t)sc, 0); }
                }
            }
        }
        /* poke it: space / return / fire / '1', one every 3 s after boot */
        else if (keys && sec >= 8 && (sec % 3) == 0) {
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
    wav_close();
    if (dump) dump_frame(dump);
#ifdef STBOX_LINE_LOG
    if (dump) dump_lines(dump);
#endif
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
