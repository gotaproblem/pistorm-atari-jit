/*
 * astos_view.c - see astos_view.h.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "astos.h"
#include "astos_view.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

enum { V_NONE, V_TEXT, V_HEX, V_PIC };
static int mode, can_text;
static int fd = -1;
static long long fsize;
static char title[64];
static long long top;           /* hex: first line; text: first wrapped line */
static int page_rows = 14;

/* ---- text: the file wrapped into lines, kept as offsets -------------- */
#define TEXT_MAX (4 << 20)
#define COLS 76
static char *text;
static long text_len;
static long *line_at;           /* where each wrapped line starts       */
static int  *line_len;          /* its bytes (before the wrap or EOL)   */
static long nlines;

static void wrap_text(void)
{
    free(line_at); free(line_len);
    line_at = NULL; line_len = NULL; nlines = 0;
    long cap = 1024;
    line_at = malloc(cap * sizeof *line_at);
    line_len = malloc(cap * sizeof *line_len);
    if (!line_at || !line_len) return;
    long i = 0;
    while (i < text_len) {
        long start = i;
        int col = 0;
        while (i < text_len && text[i] != '\n' && text[i] != '\r') {
            int w = text[i] == '\t' ? 8 - (col & 7) : 1;
            if (col + w > COLS) break;
            col += w;
            i++;
        }
        if (nlines == cap) {
            cap *= 2;
            long *a = realloc(line_at, cap * sizeof *a);
            int *b = realloc(line_len, cap * sizeof *b);
            if (!a || !b) { free(a ? a : line_at); free(b ? b : line_len); line_at = NULL; line_len = NULL; nlines = 0; return; }
            line_at = a; line_len = b;
        }
        line_at[nlines] = start;
        line_len[nlines] = (int)(i - start);
        nlines++;
        if (i < text_len && text[i] == '\r') { i++; if (i < text_len && text[i] == '\n') i++; }
        else if (i < text_len && text[i] == '\n') i++;
    }
}

/* text if the start has no NULs and is mostly printable */
static int looks_text(const unsigned char *b, long n)
{
    long ok = 0;
    for (long i = 0; i < n; i++) {
        if (!b[i]) return 0;
        if (b[i] >= 0x20 || b[i] == '\n' || b[i] == '\r' || b[i] == '\t' || b[i] == 0x1A)
            ok++;
    }
    return n == 0 || ok * 100 >= n * 95;
}

/* ---- pictures ------------------------------------------------------------ */
static uint8_t scr[32000];      /* the screen as shown                  */
static uint16_t scr_pal[4];
static int scr_mono = -1;
static uint8_t raw[32000];      /* the picture's own planar data        */
static uint16_t pic_pal[16];
static int pic_res;             /* 0 low, 1 medium, 2 high              */

static int unpack_bits(const uint8_t *in, long n, uint8_t *out, long want)
{
    long i = 0, o = 0;
    while (o < want && i < n) {
        int c = (int8_t)in[i++];
        if (c >= 0) {
            for (int k = 0; k <= c && o < want && i < n; k++)
                out[o++] = in[i++];
        } else if (c != -128) {
            if (i >= n) break;
            uint8_t b = in[i++];
            for (int k = 0; k < 1 - c && o < want; k++)
                out[o++] = b;
        }
    }
    return o == want ? 0 : -1;
}

/* Degas Elite stores each line plane by plane; the screen has the planes'
 * words interleaved */
static void interleave(const uint8_t *lines, int res)
{
    int np = res == 0 ? 4 : res == 1 ? 2 : 1, wpl = res == 0 ? 20 : 40, bpp = wpl * 2;
    for (int y = 0; y < (res == 2 ? 400 : 200); y++) {
        const uint8_t *l = lines + (long)y * np * bpp;
        uint8_t *d = raw + (long)y * (res == 2 ? 80 : 160);
        for (int w = 0; w < wpl; w++)
            for (int p = 0; p < np; p++) {
                d[(w * np + p) * 2] = l[p * bpp + w * 2];
                d[(w * np + p) * 2 + 1] = l[p * bpp + w * 2 + 1];
            }
    }
}

static int load_picture(const uint8_t *f, long n, const char *ext, char *why, int why_len)
{
    if (!strcasecmp(ext, "neo")) {
        if (n < 128 + 32000) { snprintf(why, (size_t)why_len, "NEO file too short"); return -1; }
        pic_res = f[3] & 3;
        for (int i = 0; i < 16; i++) pic_pal[i] = (uint16_t)(f[4 + 2 * i] << 8 | f[5 + 2 * i]);
        memcpy(raw, f + 128, 32000);
    } else {
        if (n < 34) { snprintf(why, (size_t)why_len, "too short for Degas"); return -1; }
        int res = f[0] << 8 | f[1];
        pic_res = res & 3;
        for (int i = 0; i < 16; i++) pic_pal[i] = (uint16_t)(f[2 + 2 * i] << 8 | f[3 + 2 * i]);
        if (res & 0x8000) {
            static uint8_t lines[32000];
            if (unpack_bits(f + 34, n - 34, lines, 32000) != 0) {
                snprintf(why, (size_t)why_len, "Degas Elite data broken");
                return -1;
            }
            interleave(lines, pic_res);
        } else {
            if (n < 34 + 32000) { snprintf(why, (size_t)why_len, "Degas file too short"); return -1; }
            memcpy(raw, f + 34, 32000);
        }
    }
    if (pic_res > 2) { snprintf(why, (size_t)why_len, "unknown resolution"); return -1; }
    return 0;
}

/* the colour index of a pixel of the picture */
static int pix(int x, int y)
{
    if (pic_res == 0) {
        const uint8_t *g = raw + y * 160 + (x >> 4) * 8;
        int bit = 15 - (x & 15), v = 0;
        for (int p = 0; p < 4; p++)
            v |= (((g[2 * p] << 8 | g[2 * p + 1]) >> bit) & 1) << p;
        return v;
    }
    if (pic_res == 1) {
        const uint8_t *g = raw + y * 160 + (x >> 4) * 4;
        int bit = 15 - (x & 15);
        return (((g[0] << 8 | g[1]) >> bit) & 1) | ((((g[2] << 8 | g[3]) >> bit) & 1) << 1);
    }
    return (raw[y * 80 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

/* $0RGB (STE: the 4th bit is the lowest) to 0..15 per gun */
static void rgb4(uint16_t c, int o[3])
{
    for (int g = 0; g < 3; g++) {
        int v = (c >> (8 - 4 * g)) & 15;
        o[g] = ((v & 7) << 1) | (v >> 3);
    }
}

static int lum(uint16_t c)
{
    int o[3];
    rgb4(c, o);
    return (o[0] * 30 + o[1] * 59 + o[2] * 11) * 255 / (15 * 100);
}

static uint16_t st3(const int o[3])
{
    return (uint16_t)(((o[0] >> 1) << 8) | ((o[1] >> 1) << 4) | (o[2] >> 1));
}

static void put_med(uint8_t *s, int x, int y, int pen)
{
    uint8_t *g = s + y * 160 + (x >> 4) * 4;
    int bit = 7 - (x & 7), hi = (x & 15) < 8 ? 0 : 1;
    for (int p = 0; p < 2; p++) {
        uint8_t *b = g + p * 2 + hi;
        if ((pen >> p) & 1) *b |= (uint8_t)(1 << bit); else *b &= (uint8_t)~(1 << bit);
    }
}

static void put_mono(uint8_t *s, int x, int y, int ink)
{
    uint8_t *b = s + y * 80 + (x >> 3);
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    if (ink) *b |= m; else *b &= (uint8_t)~m;
}

/* 16 colours to 4: k-means over the palette, weighted by use */
static void four_of(int map[16], uint16_t pal[4])
{
    long cnt[16] = { 0 };
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++)
            cnt[pix(x, y)]++;
    int col[16][3];
    for (int i = 0; i < 16; i++) rgb4(pic_pal[i], col[i]);
    double cen[4][3];
    int used[16] = { 0 };
    for (int k = 0; k < 4; k++) {                    /* the most used first */
        int best = -1;
        for (int i = 0; i < 16; i++)
            if (!used[i] && (best < 0 || cnt[i] > cnt[best])) best = i;
        used[best] = 1;
        for (int g = 0; g < 3; g++) cen[k][g] = col[best][g];
    }
    for (int it = 0; it < 12; it++) {
        double sum[4][3] = { { 0 } }, w[4] = { 0 };
        for (int i = 0; i < 16; i++) {
            int bk = 0;
            double bd = 1e9;
            for (int k = 0; k < 4; k++) {
                double d = 0;
                for (int g = 0; g < 3; g++) d += (col[i][g] - cen[k][g]) * (col[i][g] - cen[k][g]);
                if (d < bd) { bd = d; bk = k; }
            }
            map[i] = bk;
            for (int g = 0; g < 3; g++) sum[bk][g] += col[i][g] * (double)cnt[i];
            w[bk] += (double)cnt[i];
        }
        for (int k = 0; k < 4; k++)
            if (w[k] > 0)
                for (int g = 0; g < 3; g++) cen[k][g] = sum[k][g] / w[k];
    }
    for (int k = 0; k < 4; k++) {
        int o[3];
        for (int g = 0; g < 3; g++) o[g] = (int)(cen[k][g] + 0.5);
        pal[k] = st3(o);
    }
}

static void build_screen(int mono)
{
    memset(scr, 0, sizeof scr);
    scr_mono = mono;
    if (mono) {
        if (pic_res == 2) {                        /* as it is */
            int inv = !(pic_pal[0] & 1);
            for (int i = 0; i < 32000; i++) scr[i] = inv ? (uint8_t)~raw[i] : raw[i];
            return;
        }
        /* colour dithered into black and white: each pixel is a 2x2 block
         * (low) or 1x2 (medium) of the 640x400 screen */
        static const int bayer[4] = { 0, 2, 3, 1 };
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < (pic_res == 0 ? 320 : 640); x++) {
                int L = lum(pic_pal[pix(x, y)]);
                if (pic_res == 0) {
                    int white = (L * 4 + 127) / 255;
                    for (int s = 0; s < 4; s++)
                        put_mono(scr, x * 2 + (s & 1), y * 2 + (s >> 1), bayer[s] >= white);
                } else {
                    int white = (L * 2 + 127) / 255;
                    for (int s = 0; s < 2; s++)
                        put_mono(scr, x, y * 2 + s, ((s ^ (x & 1)) & 1) >= white);
                }
            }
        return;
    }
    if (pic_res == 1) {                            /* as it is */
        memcpy(scr, raw, 32000);
        for (int i = 0; i < 4; i++) scr_pal[i] = pic_pal[i] & 0x777;
        return;
    }
    if (pic_res == 2) {                            /* two rows: black, grey, white */
        int ink_black = pic_pal[0] & 1;
        scr_pal[0] = 0x777; scr_pal[1] = 0x000; scr_pal[2] = 0x444; scr_pal[3] = 0x777;
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 640; x++) {
                int a = pix(x, 2 * y), b = pix(x, 2 * y + 1);
                if (!ink_black) { a = !a; b = !b; }
                put_med(scr, x, y, a + b == 0 ? 0 : a + b == 2 ? 1 : 2);
            }
        return;
    }
    int map[16];                                   /* low: doubled, 4 colours */
    four_of(map, scr_pal);
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            int pen = map[pix(x, y)];
            put_med(scr, 2 * x, y, pen);
            put_med(scr, 2 * x + 1, y, pen);
        }
}

const uint8_t *view_screen(int mono, uint16_t pal[4])
{
    if (mode != V_PIC)
        return NULL;
    if (scr_mono != mono)
        build_screen(mono);
    if (pal)
        memcpy(pal, scr_pal, sizeof scr_pal);
    return scr;
}

/* ---- open / close ---------------------------------------------------------- */

void view_close(void)
{
    if (fd >= 0) close(fd);
    fd = -1;
    free(text); text = NULL; text_len = 0;
    free(line_at); free(line_len); line_at = NULL; line_len = NULL; nlines = 0;
    mode = V_NONE;
    scr_mono = -1;
}

int view_is_open(void) { return mode != V_NONE; }
int view_is_picture(void) { return mode == V_PIC; }

int view_open(const char *path, const char *ttl, char *why, int why_len)
{
    view_close();
    fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(why, (size_t)why_len, "cannot open it");
        view_close();
        return -1;
    }
    fsize = st.st_size;
    snprintf(title, sizeof title, "%s", ttl);
    top = 0;
    const char *dot = strrchr(ttl, '.');
    const char *ext = dot ? dot + 1 : "";
    int is_pic = !strcasecmp(ext, "neo") ||
                 ((ext[0] | 0x20) == 'p' && ((ext[1] | 0x20) == 'i' || (ext[1] | 0x20) == 'c') &&
                  ext[2] >= '1' && ext[2] <= '3' && !ext[3]);
    if (is_pic && fsize <= 64 * 1024) {
        uint8_t *f = malloc((size_t)fsize);
        if (f && pread(fd, f, (size_t)fsize, 0) == fsize &&
            load_picture(f, (long)fsize, ext, why, why_len) == 0) {
            free(f);
            mode = V_PIC;
            scr_mono = -1;
            printf("[ASTOS] view: %s, picture %s resolution\n", ttl,
                   pic_res == 0 ? "low" : pic_res == 1 ? "medium" : "high");
            return 0;
        }
        free(f);
        return -1;
    }
    /* text when it reads as text, else hex */
    unsigned char head[4096];
    ssize_t n = pread(fd, head, sizeof head, 0);
    can_text = n >= 0 && fsize <= TEXT_MAX && looks_text(head, n);
    if (can_text) {
        text = malloc((size_t)fsize + 1);
        if (text && pread(fd, text, (size_t)fsize, 0) == fsize) {
            text_len = (long)fsize;
            wrap_text();
            mode = V_TEXT;
            return 0;
        }
        free(text); text = NULL;
        can_text = 0;
    }
    mode = V_HEX;
    return 0;
}

/* ---- drawing text / hex ----------------------------------------------------- */

static long long rows_total(void)
{
    return mode == V_TEXT ? nlines : (fsize + 15) / 16;
}

void view_draw(int x, int y, int w, int h)
{
    g_fill(x, y, w, h, C_WHITE);
    g_fill(x, y, w, 11, C_ACCENT);
    g_frame(x, y, w, h, C_TEXT);
    char t[120];
    snprintf(t, sizeof t, "%s  -  %s, %lld bytes", title, mode == V_TEXT ? "text" : "hex", fsize);
    g_textn(x + 6, y + 2, t, (w - 12) / 8, C_WHITE);
    int ry = y + 14;
    page_rows = (h - 14 - 14) / 10;
    long long total = rows_total();
    if (top > total - page_rows) top = total - page_rows;
    if (top < 0) top = 0;
    for (int r = 0; r < page_rows && top + r < total; r++) {
        char line[96];
        if (mode == V_TEXT) {
            long i = line_at[top + r];
            int k = 0, col = 0;
            for (int j = 0; j < line_len[top + r] && k < 90; j++) {
                unsigned char c = (unsigned char)text[i + j];
                if (c == '\t') {
                    do line[k++] = ' '; while ((++col & 7) && k < 90);
                    continue;
                }
                line[k++] = (char)(c < 0x20 ? '.' : c);
                col++;
            }
            line[k] = '\0';
        } else {
            unsigned char b[16];
            long long off = (top + r) * 16;
            ssize_t got = pread(fd, b, 16, (off_t)off);
            if (got < 0) got = 0;
            int k = snprintf(line, sizeof line, "%08llX ", off);
            for (int j = 0; j < 16; j++) {
                if (j < got)
                    k += snprintf(line + k, sizeof line - (size_t)k, "%02X", b[j]);
                else
                    k += snprintf(line + k, sizeof line - (size_t)k, "  ");
                k += snprintf(line + k, sizeof line - (size_t)k, j == 7 ? "  " : " ");
            }
            k += snprintf(line + k, sizeof line - (size_t)k, " ");
            for (int j = 0; j < got; j++)
                line[k++] = (char)(b[j] < 0x20 || b[j] == 0x7F ? '.' : b[j]);
            line[k] = '\0';
        }
        g_textn(x + 8, ry + r * 10, line, (w - 16) / 8, C_TEXT);
    }
    char foot[120];
    long long last = top + page_rows < total ? top + page_rows : total;
    snprintf(foot, sizeof foot, "%s %lld-%lld of %lld  Up Down, Left Right page, Home, G end%s  Esc",
             mode == V_TEXT ? "lines" : "rows", total ? top + 1 : 0, last, total,
             can_text ? ", Tab hex" : "");
    g_textn(x + 8, y + h - 12, foot, (w - 16) / 8, C_DIM);
}

int view_wheel(int n)
{
    if (mode == V_PIC) return 0;
    top -= 3 * n;
    if (top < 0) top = 0;
    return 1;
}

int view_key(struct si_event k)
{
    if (mode == V_PIC) {                         /* any key: back */
        view_close();
        return 1;
    }
    long long total = rows_total();
    switch (k.key) {
    case SI_UP:    top--; break;
    case SI_DOWN:  top++; break;
    case SI_LEFT:  top -= page_rows - 1; break;      /* PgUp maps to Left too */
    case SI_RIGHT: top += page_rows - 1; break;
    case SI_HOME:  top = 0; break;
    case SI_SPACE: top += page_rows - 1; break;
    case SI_TAB:
        if (can_text) {
            long long byte = mode == V_TEXT ? (nlines ? line_at[top] : 0) : top * 16;
            mode = mode == V_TEXT ? V_HEX : V_TEXT;
            if (mode == V_HEX) top = byte / 16;
            else {
                top = 0;
                while (top + 1 < nlines && line_at[top + 1] <= byte) top++;
            }
        }
        break;
    case SI_CHAR:
        if (k.ch == 'G' || k.ch == 'g') top = total;     /* the end */
        break;
    case SI_ESC: case SI_BACKSPACE: case SI_ENTER:
        view_close();
        return 1;
    default: break;
    }
    if (top > total - page_rows) top = total - page_rows;
    if (top < 0) top = 0;
    return 0;
}
