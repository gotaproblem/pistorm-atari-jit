/*
 * FVDIMODE - pick the fVDI (aranym.sys) screen mode: .ACC and .PRG.
 *
 * fVDI reads its mode from FVDI.SYS once, at boot, and the AES keeps the
 * screen size it started with - so a mode cannot be switched under a
 * running desktop. This edits the `mode WxHxD@F` option on the aranym.sys
 * driver line of FVDI.SYS and offers a warm reboot to apply it.
 *
 * FVDI.SYS is looked for where fVDI itself looks (engine/loader.c): the
 * boot drive's root (an AUTO program's current directory), then C:\, then
 * A:\. The file is changed only inside that one value (or the option is
 * appended to the driver line when absent); everything else, line endings
 * included, is written back byte for byte. The previous file is kept as
 * FVDI.BAK beside it.
 *
 * 8 bit needs the packed-pixel ARANYM.SYS (aranym8packed.py) for programs
 * that check vq_scrninfo; 16 and 32 bit work with the stock driver.
 *
 * One binary serves both: mintlib's startup tells an accessory from an
 * application (_app), so FVDIMODE.ACC and FVDIMODE.PRG are the same file.
 *
 * Build with the m68k-atari-mint cross toolchain + gemlib: `make`.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <string.h>

extern short _app;                     /* mintlib: 1 = PRG, 0 = ACC */

/* ---------------------------------------------------------------- modes */

typedef struct { short w, h; const char *label; } res_t;

static const res_t g_res[] = {
    {  640,  480, " 640 x 480 "  },
    {  800,  600, " 800 x 600 "  },
    { 1024,  768, "1024 x 768 "  },
    { 1280,  720, "1280 x 720 "  },
    { 1280, 1024, "1280 x 1024"  },
    { 1920, 1080, "1920 x 1080"  },
};
#define NRES ((int)(sizeof g_res / sizeof g_res[0]))

static const short g_depth[] = { 8, 16, 32 };
static const char *const g_depth_label[] = {
    "256 (8 bit)", "65K (16 bit)", "16M (32 bit)"
};
#define NDEPTH 3

/* ------------------------------------------------------------ FVDI.SYS */

#define MAXCFG 16384

static char  g_path[16];               /* "X:\FVDI.SYS" */
static char  g_buf[MAXCFG + 1];
static long  g_len;
static long  g_vs, g_ve;               /* value span of `mode`, or vs = -1 */
static long  g_eol;                    /* end of the driver line (no CR/LF) */
static short g_cur_w, g_cur_h, g_cur_d, g_cur_f;   /* parsed, 0 = none */

/* Low-memory system variables. The empty asm hides the constant address
 * from GCC's null-page array-bounds heuristic. */
static long read_bootdev(void)
{
    volatile short *p = (volatile short *)0x446L;   /* _bootdev */
    __asm__ ("" : "+a" (p));
    return *p;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t';
}

static int is_eol(int c)
{
    return c == '\r' || c == '\n' || c == 0;
}

/* case-insensitive: does [p, p+n) equal s? */
static int tok_eq(const char *p, long n, const char *s)
{
    long i;
    for (i = 0; i < n; i++)
        if (!s[i] || lower(p[i]) != lower(s[i]))
            return 0;
    return s[n] == 0;
}

static int tok_has(const char *p, long n, const char *s)
{
    long i, k, m = (long)strlen(s);
    for (i = 0; i + m <= n; i++) {
        for (k = 0; k < m && lower(p[i + k]) == lower(s[k]); k++)
            ;
        if (k == m)
            return 1;
    }
    return 0;
}

static const char *parse_num(const char *p, const char *end, short *out)
{
    long v = 0;
    int any = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        any = 1;
    }
    *out = any ? (short)v : 0;
    return p;
}

static int load_cfg(void)
{
    static const char drives[3] = { 0, 'C', 'A' };
    long fh = -1;
    int i;

    for (i = 0; i < 3; i++) {
        char d = drives[i] ? drives[i]
                           : (char)('A' + (int)Supexec(read_bootdev));
        g_path[0] = d;
        strcpy(g_path + 1, ":\\FVDI.SYS");
        fh = Fopen(g_path, 0);
        if (fh >= 0)
            break;
    }
    if (fh < 0)
        return 0;
    g_len = Fread((short)fh, MAXCFG, g_buf);
    Fclose((short)fh);
    if (g_len <= 0 || g_len >= MAXCFG)
        return 0;
    g_buf[g_len] = 0;
    return 1;
}

/* Find the driver line (prefer aranym.sys) and its `mode` value. */
static int find_mode(void)
{
    long p = 0, first = -1, pick = -1;

    while (p < g_len) {
        long ls = p, t0, t1, d0, d1;
        while (p < g_len && !is_eol(g_buf[p]))
            p++;
        /* first token */
        t0 = ls;
        while (t0 < p && is_space(g_buf[t0]))
            t0++;
        t1 = t0;
        while (t1 < p && !is_space(g_buf[t1]))
            t1++;
        if (t1 - t0 >= 2 && t1 - t0 <= 3 && g_buf[t0] != '#' &&
            g_buf[t0] >= '0' && g_buf[t0] <= '9' &&
            g_buf[t0 + 1] >= '0' && g_buf[t0 + 1] <= '9' &&
            (t1 - t0 == 2 || lower(g_buf[t0 + 2]) == 'r' ||
             lower(g_buf[t0 + 2]) == 'p')) {
            d0 = t1;
            while (d0 < p && is_space(g_buf[d0]))
                d0++;
            d1 = d0;
            while (d1 < p && !is_space(g_buf[d1]))
                d1++;
            if (first < 0)
                first = ls;
            if (pick < 0 && tok_has(g_buf + d0, d1 - d0, "aranym"))
                pick = ls;
        }
        while (p < g_len && (g_buf[p] == '\r' || g_buf[p] == '\n'))
            p++;
    }
    if (pick < 0)
        pick = first;
    if (pick < 0)
        return 0;

    /* walk the chosen line's tokens for `mode <value>` */
    g_vs = -1;
    p = pick;
    while (p < g_len && !is_eol(g_buf[p]))
        p++;
    g_eol = p;
    p = pick;
    while (p < g_eol) {
        long t0, t1;
        while (p < g_eol && is_space(g_buf[p]))
            p++;
        t0 = p;
        while (p < g_eol && !is_space(g_buf[p]))
            p++;
        t1 = p;
        if (tok_eq(g_buf + t0, t1 - t0, "mode")) {
            while (p < g_eol && is_space(g_buf[p]))
                p++;
            g_vs = p;
            while (p < g_eol && !is_space(g_buf[p]))
                p++;
            g_ve = p;
            break;
        }
    }

    g_cur_w = g_cur_h = g_cur_d = g_cur_f = 0;
    if (g_vs >= 0 && g_ve > g_vs) {
        const char *q = g_buf + g_vs, *e = g_buf + g_ve;
        q = parse_num(q, e, &g_cur_w);
        if (q < e && lower(*q) == 'x') q = parse_num(q + 1, e, &g_cur_h);
        if (q < e && lower(*q) == 'x') q = parse_num(q + 1, e, &g_cur_d);
        if (q < e && *q == '@')        q = parse_num(q + 1, e, &g_cur_f);
    }
    return 1;
}

static char *put_num(char *s, long v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *s++ = t[--n];
    return s;
}

static int write_file(const char *path, const char *data, long len)
{
    long fh = Fcreate(path, 0);
    long wr;
    if (fh < 0)
        return 0;
    wr = Fwrite((short)fh, len, (void *)data);
    Fclose((short)fh);
    return wr == len;
}

static int save_cfg(int w, int h, int d)
{
    static char out[MAXCFG + 64];
    char val[40], *v = val, bak[16];
    long n = 0, vl;

    v = put_num(v, w);  *v++ = 'x';
    v = put_num(v, h);  *v++ = 'x';
    v = put_num(v, d);  *v++ = '@';
    v = put_num(v, g_cur_f ? g_cur_f : 60);
    *v = 0;
    vl = v - val;

    if (g_vs >= 0) {                    /* replace the value in place */
        memcpy(out, g_buf, g_vs);                         n = g_vs;
        memcpy(out + n, val, vl);                         n += vl;
        memcpy(out + n, g_buf + g_ve, g_len - g_ve);      n += g_len - g_ve;
    } else {                            /* append " mode <val>" */
        memcpy(out, g_buf, g_eol);                        n = g_eol;
        memcpy(out + n, " mode ", 6);                     n += 6;
        memcpy(out + n, val, vl);                         n += vl;
        memcpy(out + n, g_buf + g_eol, g_len - g_eol);    n += g_len - g_eol;
    }

    strcpy(bak, g_path);
    strcpy(bak + strlen(bak) - 3, "BAK");
    if (!write_file(bak, g_buf, g_len)) {
        form_alert(1, "[3][Could not write FVDI.BAK.|FVDI.SYS left unchanged.][ OK ]");
        return 0;
    }
    if (!write_file(g_path, out, n)) {
        form_alert(1, "[3][Could not write FVDI.SYS!|The old file is in FVDI.BAK.][ OK ]");
        return 0;
    }
    return 1;
}

static long do_reboot(void)
{
    void (**vec)(void) = (void (**)(void))4L;       /* reset PC */
    __asm__ ("" : "+a" (vec));
    (*vec)();
    return 0;
}

/* -------------------------------------------------------------- dialog */

enum {
    D_ROOT, D_TITLE, D_RUN, D_FILE, D_RESLBL, D_RESBOX,
    D_RES0, D_RES_END = D_RES0 + NRES - 1,
    D_DEPLBL, D_DEPBOX,
    D_DEP0, D_DEP_END = D_DEP0 + NDEPTH - 1,
    D_NOTE, D_SAVE, D_REBOOT, D_CANCEL,
    D_COUNT
};

static OBJECT g_tree[D_COUNT];
static char   g_run_str[48];
static char   g_file_str[48];
static int    g_built;

static void obj(int i, int parent, unsigned short type, unsigned short flags,
                long spec, int x, int y, int w, int h)
{
    OBJECT *o = &g_tree[i];
    o->ob_next = o->ob_head = o->ob_tail = -1;
    o->ob_type = type;
    o->ob_flags = flags;
    o->ob_state = 0;
    o->ob_spec.index = spec;
    o->ob_x = (short)x; o->ob_y = (short)y;
    o->ob_width = (short)w; o->ob_height = (short)h;
    if (parent >= 0) {
        OBJECT *p = &g_tree[parent];
        if (p->ob_head < 0) {
            p->ob_head = p->ob_tail = (short)i;
        } else {
            g_tree[p->ob_tail].ob_next = (short)i;
            p->ob_tail = (short)i;
        }
        o->ob_next = (short)parent;     /* last child points at parent */
    }
}

#define STR(s) ((long)(s))

static void build_tree(void)
{
    int i;

    obj(D_ROOT, -1, G_BOX, 0, 0x00021100L, 0, 0, 46, 18);
    g_tree[D_ROOT].ob_state = OS_OUTLINED;
    obj(D_TITLE, D_ROOT, G_STRING, 0, STR("fVDI screen mode"), 15, 1, 16, 1);
    obj(D_RUN,   D_ROOT, G_STRING, 0, STR(g_run_str),  2, 3, 42, 1);
    obj(D_FILE,  D_ROOT, G_STRING, 0, STR(g_file_str), 2, 4, 42, 1);

    obj(D_RESLBL, D_ROOT, G_STRING, 0, STR("Resolution:"), 2, 6, 11, 1);
    obj(D_RESBOX, D_ROOT, G_IBOX, 0, 0L, 2, 7, 42, 3);
    for (i = 0; i < NRES; i++)
        obj(D_RES0 + i, D_RESBOX, G_BUTTON, OF_SELECTABLE | OF_RBUTTON,
            STR(g_res[i].label), (i % 3) * 14, (i / 3) * 2, 13, 1);

    obj(D_DEPLBL, D_ROOT, G_STRING, 0, STR("Colours:"), 2, 11, 8, 1);
    obj(D_DEPBOX, D_ROOT, G_IBOX, 0, 0L, 2, 12, 42, 1);
    for (i = 0; i < NDEPTH; i++)
        obj(D_DEP0 + i, D_DEPBOX, G_BUTTON, OF_SELECTABLE | OF_RBUTTON,
            STR(g_depth_label[i]), i * 14, 0, 13, 1);

    obj(D_NOTE, D_ROOT, G_STRING, 0,
        STR("A new mode takes effect after a reboot."), 2, 14, 40, 1);

    obj(D_SAVE, D_ROOT, G_BUTTON, OF_SELECTABLE | OF_EXIT,
        STR("Save"), 2, 16, 12, 1);
    obj(D_REBOOT, D_ROOT, G_BUTTON, OF_SELECTABLE | OF_EXIT | OF_DEFAULT,
        STR("Save+Reboot"), 16, 16, 14, 1);
    obj(D_CANCEL, D_ROOT, G_BUTTON, OF_SELECTABLE | OF_EXIT | OF_LASTOB,
        STR("Cancel"), 32, 16, 12, 1);

    for (i = 0; i < D_COUNT; i++)
        rsrc_obfix(g_tree, i);
}

static void fmt_mode(char *s, const char *lead, int w, int h, int d, int f)
{
    strcpy(s, lead);
    s += strlen(s);
    if (!w || !h || !d) {
        strcpy(s, "not set");
        return;
    }
    s = put_num(s, w); *s++ = 'x';
    s = put_num(s, h); *s++ = 'x';
    s = put_num(s, d); strcpy(s, " bit");
    if (f) {
        s += 4; *s++ = ' '; *s++ = '@'; s = put_num(s, f);
        strcpy(s, " Hz");
    }
}

static void running_mode(int *w, int *h, int *d)
{
    short work_in[11], work_out[57], ext[57];
    short hnd, dummy, i;

    *w = *h = *d = 0;
    hnd = graf_handle(&dummy, &dummy, &dummy, &dummy);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &hnd, work_out);
    if (hnd <= 0)
        return;
    vq_extnd(hnd, 1, ext);
    *w = work_out[0] + 1;
    *h = work_out[1] + 1;
    *d = ext[4];
    v_clsvwk(hnd);
}

static void select_only(int first, int count, int pick)
{
    int i;
    for (i = 0; i < count; i++) {
        if (i == pick) g_tree[first + i].ob_state |= OS_SELECTED;
        else           g_tree[first + i].ob_state &= ~OS_SELECTED;
    }
}

static int selected(int first, int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (g_tree[first + i].ob_state & OS_SELECTED)
            return i;
    return -1;
}

static void run_dialog(void)
{
    short x, y, w, h, exitobj;
    int rw, rh, rd, i, ri = 0, di = 2;

    if (!load_cfg()) {
        form_alert(1, "[3][FVDI.SYS not found on the|boot drive, C: or A:.][ OK ]");
        return;
    }
    if (!find_mode()) {
        form_alert(1, "[3][No driver line in FVDI.SYS|(e.g. 01r aranym.sys ...).][ OK ]");
        return;
    }

    if (!g_built) {
        build_tree();
        g_built = 1;
    }

    running_mode(&rw, &rh, &rd);
    fmt_mode(g_run_str,  "Running now:  ", rw, rh, rd, 0);
    fmt_mode(g_file_str, "In FVDI.SYS:  ", g_cur_w, g_cur_h, g_cur_d, g_cur_f);

    /* preselect what FVDI.SYS asks for, else what is running */
    {
        int pw = g_cur_w ? g_cur_w : rw, ph = g_cur_h ? g_cur_h : rh;
        int pd = g_cur_d ? g_cur_d : rd;
        for (i = 0; i < NRES; i++)
            if (g_res[i].w == pw && g_res[i].h == ph)
                ri = i;
        for (i = 0; i < NDEPTH; i++)
            if (g_depth[i] == pd)
                di = i;
    }
    select_only(D_RES0, NRES, ri);
    select_only(D_DEP0, NDEPTH, di);

    wind_update(BEG_UPDATE);
    form_center(g_tree, &x, &y, &w, &h);
    form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
    objc_draw(g_tree, D_ROOT, MAX_DEPTH, x, y, w, h);
    exitobj = form_do(g_tree, 0) & 0x7fff;
    g_tree[exitobj].ob_state &= ~OS_SELECTED;
    form_dial(FMD_FINISH, 0, 0, 0, 0, x, y, w, h);
    wind_update(END_UPDATE);

    if (exitobj != D_SAVE && exitobj != D_REBOOT)
        return;

    ri = selected(D_RES0, NRES);
    di = selected(D_DEP0, NDEPTH);
    if (ri < 0 || di < 0)
        return;
    if (!save_cfg(g_res[ri].w, g_res[ri].h, g_depth[di]))
        return;

    if (exitobj == D_REBOOT) {
        if (form_alert(1, "[2][FVDI.SYS saved.|Reboot now? Unsaved work|in other programs is lost.][Reboot|Later]") == 1)
            Supexec(do_reboot);
    } else {
        form_alert(1, "[1][FVDI.SYS saved.|The new mode applies|after the next reboot.][ OK ]");
    }
}

int main(void)
{
    short msg[8];

    appl_init();
    if (_app) {
        graf_mouse(ARROW, NULL);
        run_dialog();
        appl_exit();
        return 0;
    }

    menu_register(gl_apid, "  fVDI Mode");
    for (;;) {
        evnt_mesag(msg);
        if (msg[0] == AC_OPEN)
            run_dialog();
    }
}
