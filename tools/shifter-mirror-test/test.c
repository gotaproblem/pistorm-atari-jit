/*
 * tools/shifter-mirror-test/test.c - host test for the exact HDMI mirror
 * (platforms/atari/video/shifter_mirror.c). No Pi, no ST: a simulated
 * 50 Hz beam drives the module through the same calls the emulator makes
 * (register writes after the bus write, screen RAM writes, VBL sightings,
 * video counter reads) on a fake clock, and the rebuilt frames are checked
 * pixel by pixel against what a Shifter would have shown.
 *
 *   make -C tools/shifter-mirror-test && tools/shifter-mirror-test/shm-test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "platforms/atari/video/shifter_mirror.h"

/* ---- what the module links against ---------------------------------- */
static uint64_t fake_t;
uint64_t shm_ticks_host(void) { return fake_t; }
volatile uint16_t st_palette[16];
static uint8_t *mem;
uint32_t pistorm_stram_top(void) { return 0x400000u; }

/* ---- the simulated machine ------------------------------------------ */
/* SHM_TEST_PPM: the ST's crystal off nominal; SHM_TEST_LAG: worst VBL
 * sighting lag in us (one sighting in 8 gets up to that) */
static double NS_PER_CYC = 1e9 / 8021247.0;
static double LAG_US = 40;
#define LINE 512
#define FRAME (313 * 512)
static double P;
static const uint32_t BASE = 0x078000u;
static double T0 = 1e9;                   /* origin of frame 0 */
static int VBL_OFS_CYC = 64;              /* the VBL comes 64 cycles after origin */
static int frame_k;
static int bytes_of_line[313];            /* truth for the counter model */

static double origin(int k) { return T0 + k * P; }
static void at(int k, int L, double c) { fake_t = (uint64_t)llround(origin(k) + (L * LINE + c) * NS_PER_CYC); }

/* the real counter at fake_t (only for the frame being run) */
int pistorm_shm_read_counter(uint32_t *cnt, uint64_t *t)
{
    double cyc = ((double)fake_t - origin(frame_k)) / NS_PER_CYC;
    int L = (int)(cyc / LINE);
    double c = cyc - L * LINE;
    uint32_t a = BASE;
    for (int i = 63; i < L && i < 263; i++)
        a += (uint32_t)bytes_of_line[i];
    if (L >= 63 && L < 263 && c >= 56) {
        int b = (int)((c - 56) / 2.0) & ~1;
        if (b > bytes_of_line[L]) b = bytes_of_line[L];
        a += (uint32_t)b;
    }
    *cnt = a;
    *t = fake_t;
    return 1;
}

static void io8(uint32_t a, uint8_t v)  { shm_io_write(a, v, 1); }
static void io16(uint32_t a, uint16_t v) { shm_io_write(a, v, 2); }

static void wram16(uint32_t a, uint16_t v)
{
    uint16_t old = (uint16_t)((mem[a] << 8) | mem[a + 1]);
    if (shm_ram_hit(a))
        shm_log_ram(a, 2, old, v);
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}

/* screen: plane 0 of 16-pixel group g on display line y is all ones when
 * (g + y) is even - colour 1 stripes on colour 0 */
static void fill_screen(void)
{
    for (int y = 0; y < 260; y++)
        for (int g = 0; g < 30; g++) {
            uint32_t a = BASE + (uint32_t)y * 160u + (uint32_t)g * 8u;
            uint16_t p0 = ((g + y) & 1) ? 0x0000 : 0xFFFF;
            mem[a] = (uint8_t)(p0 >> 8); mem[a + 1] = (uint8_t)p0;
            memset(mem + a + 2, 0, 6);
        }
}

/* ---- checks ----------------------------------------------------------- */
static uint32_t out[276 * 1024];
static int fails, checks;

static uint32_t px(int row, double cyc)   /* canvas pixel at a low-res cycle */
{
    int hx = (int)(2 * (cyc - 8));
    return out[row * 1024 + hx] & 0xFFFFFFu;
}

#define WHITE 0xFFFFFFu
#define BLACK 0x000000u
#define BLUE  0x000024u   /* $001 on an ST: 3-bit 1 -> 0x24 */
#define RED   0xFF0000u

static void expect(const char *what, uint32_t got, uint32_t want)
{
    checks++;
    if (got != want) {
        fails++;
        printf("  FAIL %s: got %06X want %06X\n", what, got, want);
    }
}

/* display line y, display pixel p: canvas row/cycle at a normal 50 Hz frame */
static uint32_t dpx(int y, int p) { return px(63 + y - 34, 56 + p); }

int main(void)
{
    if (getenv("SHM_TEST_PPM"))
        NS_PER_CYC = 1e9 / (8021247.0 * (1.0 + atof(getenv("SHM_TEST_PPM")) * 1e-6));
    if (getenv("SHM_TEST_LAG"))
        LAG_US = atof(getenv("SHM_TEST_LAG"));
    P = FRAME * NS_PER_CYC;
    mem = calloc(1, 0x400000);
    fill_screen();
    for (int i = 0; i < 313; i++)
        bytes_of_line[i] = 160;
    st_palette[0] = 0x001;
    st_palette[1] = 0x777;
    setenv("PISTORM_SHM_DEBUG", "0", 0);
    setenv("PISTORM_SHM_ROWS", "276", 0);   /* the checks use the full frame */
    setenv("PISTORM_SHM_X0", "8", 0);       /* ... and Hatari's 416-wide window */
    setenv("PISTORM_SHM_W", "416", 0);
    setenv("PISTORM_SHM_PAR", "1/1", 0);
    if (shm_init(mem, 0) != 0) {
        printf("init failed\n");
        return 1;
    }
    /* base and palette as the guest would set them */
    fake_t = (uint64_t)(T0 - P);
    io8(0xFF8201, (uint8_t)(BASE >> 16));
    io8(0xFF8203, (uint8_t)(BASE >> 8));
    io8(0xFF820A, 0x02);
    io8(0xFF8260, 0x00);
    io16(0xFF8240, 0x001);
    io16(0xFF8242, 0x777);

    /* before any VBL is seen: a plain frame from the registers, borders in
     * colour 0 (the boot screen) */
    {
        uint32_t w0, h0, sw0, sh0;
        fake_t = (uint64_t)(T0 - P / 2);
        int r = shm_render(out, 1024, &w0, &h0, &sw0, &sh0);
        printf("before the lock: render %d (%ux%u)\n", r, w0, h0);
        expect("pre-lock frame drawn", (uint32_t)r, 1);
        expect("pre-lock top border colour 0", px(5, 100), BLUE);
        expect("pre-lock line 0 px 5 colour 1", dpx(0, 5), WHITE);
        memset(out, 0, sizeof out);
    }

    unsigned seed = 314187;
    uint32_t w, h, sw, sh;
    int nframes = getenv("SHM_TEST_FRAMES") ? atoi(getenv("SHM_TEST_FRAMES")) : 70;
    for (frame_k = 0; frame_k < nframes; frame_k++) {
        const int k = frame_k;
        if (k == 0) {
            fake_t = (uint64_t)(origin(0) + VBL_OFS_CYC * NS_PER_CYC);
            shm_vbl_sighting();
        }
        at(k, 0, VBL_OFS_CYC + 200);
        shm_cpu_vbl_taken();
        for (int i = 0; i < 313; i++)
            bytes_of_line[i] = 160;

        /* scenario per frame */
        if (k == 41) {                         /* raster split mid-line */
            at(k, 63 + 100, 200); io16(0xFF8242, 0x700);
            at(k, 300, 0);        io16(0xFF8242, 0x777);
        }
        if (k == 42) {                         /* race the beam */
            at(k, 63 + 10, 0);   wram16(BASE + 60 * 160, 0x0000);   /* before line 60's fetch */
            at(k, 63 + 150, 300); wram16(BASE + 50 * 160, 0x0000);  /* after line 50's fetch */
        }
        if (k == 44) {                         /* top border off */
            at(k, 33, 492); io8(0xFF820A, 0x00);
            at(k, 34, 32);  io8(0xFF820A, 0x02);
        }
        if (k == 45) {                         /* right border off on display line 100 */
            at(k, 63 + 100, 376); io8(0xFF820A, 0x00);
            at(k, 63 + 100, 392); io8(0xFF820A, 0x02);
            bytes_of_line[63 + 100] = 204;
        }
        if (k == 46) {                         /* left border off on display line 120 */
            at(k, 63 + 119, 508); io8(0xFF8260, 0x02);
            at(k, 63 + 120, 8);   io8(0xFF8260, 0x00);
            bytes_of_line[63 + 120] = 186;
        }
        if (k == 48) {                         /* palette edge, to the cycle */
            at(k, 63 + 100, 220); io16(0xFF8242, 0x700);
            at(k, 300, 0);        io16(0xFF8242, 0x777);
        }
        if (k == 47) {                         /* bottom border off */
            at(k, 262, 492); io8(0xFF820A, 0x00);
            at(k, 263, 32);  io8(0xFF820A, 0x02);
        }

        /* the CPU's counter read, mid-display */
        seed = seed * 1103515245u + 12345u;
        at(k, 140, 200 + (double)((seed >> 16) % 1000u) / 100.0);   /* any phase */
        shm_cal_poll();

        /* the next VBL ends it, seen 0-40 us late (now and then much later) */
        seed = seed * 1103515245u + 12345u;
        double lag = (k < 2) ? 0 : (double)((seed >> 16) % 40000u);
        if (k >= 2 && ((seed >> 8) & 7) == 0)
            lag = (double)((seed >> 12) % 1000u) * LAG_US;
        fake_t = (uint64_t)(origin(k + 1) + VBL_OFS_CYC * NS_PER_CYC + lag);
        shm_vbl_sighting();
        /* the render loop wakes some time after */
        fake_t += 20000u + (uint64_t)((seed >> 4) % 500000u);
        int r = shm_render(out, 1024, &w, &h, &sw, &sh);
        if (k < 40)
            continue;
        if (r != 1) {
            printf("frame %d: render returned %d\n", k, r);
            fails++;
            continue;
        }
        if (k == 40) {
            printf("frame 40: plain screen (%ux%u shape %ux%u)\n", w, h, sw, sh);
            expect("size", w * 1000 + h, 832 * 1000 + 276);
            expect("line 0 px 5 colour 1", dpx(0, 5), WHITE);
            expect("line 0 px 20 colour 0", dpx(0, 20), BLUE);
            expect("line 1 px 5 colour 0", dpx(1, 5), BLUE);
            expect("top border", px(5, 100), BLUE);
            expect("left border", dpx(10, -20), BLUE);
            expect("bottom border", px(63 + 210 - 34, 100), BLUE);
            expect("above vblank-off: row 0 is line 34, visible", px(0, 100), BLUE);
        }
        if (k == 41) {
            printf("frame 41: palette split at line 100 cycle 200\n");
            expect("line 99 px 128 white", dpx(99, 128 + 0), (99 + 8) & 1 ? BLUE : WHITE);
            expect("line 100 px 96 still white", dpx(100, 96), WHITE);    /* cycle 152 */
            expect("line 100 px 128 red", dpx(100, 128), RED);           /* cycle 184 */
            expect("line 101 px 16 red", dpx(101, 16), RED);

        }
        if (k == 42) {
            printf("frame 42: screen writes before/after the fetch\n");
            expect("line 60 written before fetch: colour 0", dpx(60, 5), BLUE);
            expect("line 50 written after fetch: still 1", dpx(50, 5), WHITE);
        }
        if (k == 43) {
            printf("frame 43: the late write shows a frame later\n");
            expect("line 50 now colour 0", dpx(50, 5), BLUE);
            /* put it back for the later frames */
            at(k + 1, 0, 0);
            mem[BASE + 50 * 160] = 0xFF; mem[BASE + 50 * 160 + 1] = 0xFF;
            mem[BASE + 60 * 160] = 0xFF; mem[BASE + 60 * 160 + 1] = 0xFF;
        }
        if (k == 44) {
            printf("frame 44: top border removed\n");
            expect("row 0 = screen line 0 px 5", px(0, 56 + 5), WHITE);
            expect("row 1 = screen line 1 px 5", px(1, 56 + 5), BLUE);
        }
        if (k == 45) {
            printf("frame 45: right border removed on line 100\n");
            /* bytes 160.. of line 100 are line 101's first groups in memory:
             * group 0 (odd) colour 0, group 1 (even) colour 1 */
            expect("line 100 right border, cycle 380 (next line's group 0)", px(63 + 100 - 34, 380), BLUE);
            expect("line 100 right border, cycle 400 (next line's group 1)", px(63 + 100 - 34, 400), WHITE);
            expect("line 99 right border is border", px(63 + 99 - 34, 400), BLUE);
            /* line 101 now starts 44 bytes on: group 5.5 - plane 0 word is the
             * group-5 plane-2 word = 0, plane 1 = group 5 plane 3 = 0,
             * plane 2 = group 6 plane 0 = (6+101) odd -> 0 ... colour 0 */
            expect("line 101 shifted by 44 bytes", dpx(101, 5), BLUE);
        }
        if (k == 46) {
            printf("frame 46: left border removed on line 120\n");
            /* left-off line shows pixels from cycle 0: pixel 0 at cycle 0 is
             * the first group of line 120 = colour 1 */
            expect("line 120 starts in the left border", px(63 + 120 - 34, 10), WHITE);
            expect("line 119 border there", px(63 + 119 - 34, 10), BLUE);
        }
        if (k == 48) {
            printf("frame 48: a write at cycle 220 colours from cycle 192\n");
            expect("line 100 cycle 191 white", px(63 + 100 - 34, 191), WHITE);
            expect("line 100 cycle 192 red", px(63 + 100 - 34, 192), RED);
        }
        if (k == 47) {
            printf("frame 47: bottom border removed\n");
            expect("line 210 shown (was border)", px(63 + 210 - 34, 56 + 5), ((210) & 1) ? BLUE : WHITE);
        }
    }
    /* Extra VBL sightings: a level-4 line held while the CPU is slow to
     * take it, pre-empted and re-latched, is sighted again 2-14 ms into
     * the frame (GEMBench, 3 Nov 2026: up to 28 short gaps a second on a
     * 50 Hz screen, and the lock moved to 60 Hz). The frame rate must
     * stay with the GLUE's registers. */
    {
        int seen60 = 0, last = 0;
        for (int i = 0; i < 300; i++, frame_k++) {
            const int k = frame_k;
            seed = seed * 1103515245u + 12345u;
            fake_t = (uint64_t)(origin(k + 1) + VBL_OFS_CYC * NS_PER_CYC +
                                (double)((seed >> 16) % 40000u));
            shm_vbl_sighting();
            if (i >= 50 && i < 250) {
                for (int e = 0; e < 2; e++) {
                    seed = seed * 1103515245u + 12345u;
                    fake_t += 2000000u + (uint64_t)((seed >> 12) % 6000000u);
                    shm_vbl_sighting();
                }
            }
            fake_t = (uint64_t)(origin(k + 1) + P * 0.9);
            (void)shm_render(out, 1024, &w, &h, &sw, &sh);
            last = shm_locked_hz();
            if (last == 60)
                seen60++;
        }
        printf("re-latched sightings: 60 Hz in %d of 300 frames, ends at %d Hz\n", seen60, last);
        expect("frames locked at 60 Hz with sync=50", (uint32_t)seen60, 0);
        expect("lock ends at 50 Hz", (uint32_t)last, 50);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
