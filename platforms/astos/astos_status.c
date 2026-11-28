/*
 * astos_status.c - the Pi status window and the events log.
 *
 * Status (once a second while it is open; the throttle word and the
 * kernel log are watched all the time):
 *   CPU      load per core (/proc/stat), ARM clock now / max (mailbox)
 *   Clocks   core (VPU), V3D (mailbox GET_CLOCK_RATE_MEASURED)
 *   Volts    core, SDRAM controller / I/O / PHY (mailbox GET_VOLTAGE -
 *            what vcgencmd measure_volts reads)
 *   Temp     SoC, against the 80 C soft limit and 85 C hard limit
 *   Power    the four get_throttled conditions: now / before / never
 *   Memory   Pi RAM used / total (/proc/meminfo), GPU memory (mailbox)
 *   SD       free space on /, SD errors seen in the kernel log
 *   Pi       model (device tree), kernel, uptime, IP, Wi-Fi link
 *
 * Events, kept across reboots in astos-events.log beside psctrl.cfg:
 *   - each throttle condition starting / clearing while ASTOS runs, and
 *     one that happened while a build ran (its since-boot bit appeared)
 *   - kernel log lines that matter: SD (mmc) errors, filesystem errors /
 *     read-only remounts, the out-of-memory killer, under-voltage - read
 *     from /dev/kmsg, so what happened during a build is caught next time
 *     ASTOS starts (the last record read is kept in /run)
 *   - how the last environment ended (run-pistorm.sh writes
 *     /run/astos-last-exit), the ST reset button, ST power lost
 * A warning or error also shows as a toast on the desktop for a while.
 */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "astos.h"
#include "setup_input.h"

#define LOG_NAME   "astos-events.log"
#define KMSG_SEQ   "/run/astos-kmsg-seq"
#define THR_SEEN   "/run/astos-throttled"
#define LAST_EXIT  "/run/astos-last-exit"
#define MAX_EV     200
#define TOAST_S    10

/* ---- the events ----------------------------------------------------- */

static struct ev { time_t t; int sev; char text[72]; } ev[MAX_EV];
static int nev;                         /* newest last                  */
static char log_path[600];
static char toast[80];
static time_t toast_until;

static void log_line(const struct ev *e)
{
    if (!log_path[0])
        return;
    FILE *f = fopen(log_path, "a");
    if (!f)
        return;
    struct tm tm;
    localtime_r(&e->t, &tm);
    char ts[24];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s %s %s\n", ts, e->sev == EV_ERROR ? "ERROR" : e->sev == EV_WARN ? "WARN " : "INFO ",
            e->text);
    fclose(f);
}

static void add(time_t t, int sev, const char *text, int write)
{
    if (nev == MAX_EV) {
        memmove(ev, ev + 1, sizeof ev[0] * (MAX_EV - 1));
        nev--;
    }
    struct ev *e = &ev[nev++];
    e->t = t;
    e->sev = sev;
    snprintf(e->text, sizeof e->text, "%s", text);
    if (write) {
        log_line(e);
        printf("[ASTOS] event: %s\n", text);
        if (sev >= EV_WARN) {
            snprintf(toast, sizeof toast, "%s", text);
            toast_until = time(NULL) + TOAST_S;
        }
    }
}

void stat_event(int sev, const char *fmt, ...)
{
    char b[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    add(time(NULL), sev, b, 1);
}

const char *stat_toast(void)
{
    return time(NULL) < toast_until ? toast : NULL;
}

/* the log as it was: the last MAX_EV lines; trimmed when it grows past
 * 1000 lines */
static void load_log(void)
{
    FILE *f = fopen(log_path, "r");
    if (!f)
        return;
    char line[160];
    int lines = 0;
    while (fgets(line, sizeof line, f)) {
        lines++;
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        char sev[8], *rest;
        if (strlen(line) < 26 ||
            sscanf(line, "%d-%d-%d %d:%d:%d %7s", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec, sev) != 7)
            continue;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        rest = line + 26;
        rest[strcspn(rest, "\n")] = '\0';
        add(mktime(&tm), !strcmp(sev, "ERROR") ? EV_ERROR : !strcmp(sev, "WARN") ? EV_WARN : EV_INFO,
            rest, 0);
    }
    fclose(f);
    if (lines > 1000 && (f = fopen(log_path, "w"))) {
        fclose(f);
        for (int i = 0; i < nev; i++)
            log_line(&ev[i]);
    }
}

/* ---- the kernel log ------------------------------------------------- */

static int kfd = -1;
static int sd_errors;                    /* SD card errors this session */
static unsigned long long kseq;          /* the last record looked at   */

static const struct { const char *has, *also; int sev; const char *say; } kpat[] = {
    { "Under-voltage detected", NULL,   EV_WARN,  "kernel: under-voltage detected" },
    { "Voltage normalised",     NULL,   EV_INFO,  "kernel: voltage normal again" },
    { "mmc",                    "error", EV_ERROR, "SD card error" },
    { "mmc",                    "timeout", EV_ERROR, "SD card timeout" },
    { "EXT4-fs error",          NULL,   EV_ERROR, "filesystem error on the SD card" },
    { "read-only",              "emount", EV_ERROR, "a filesystem was remounted read-only" },
    { "Out of memory: Killed process", NULL, EV_ERROR, "out of memory: a program was killed" },
    { "I/O error",              "dev mmcblk", EV_ERROR, "SD card I/O error" },
};

static void read_kmsg(void)
{
    if (kfd < 0)
        return;
    char rec[1024];
    for (;;) {
        ssize_t n = read(kfd, rec, sizeof rec - 1);
        if (n < 0) {
            if (errno == EPIPE)                 /* overwritten: go on */
                continue;
            return;                             /* EAGAIN: all read   */
        }
        rec[n] = '\0';
        unsigned pri;
        unsigned long long seq, ts;
        char *msg = strchr(rec, ';');
        if (!msg || sscanf(rec, "%u,%llu,%llu", &pri, &seq, &ts) != 3)
            continue;
        if (seq <= kseq)
            continue;
        kseq = seq;
        msg++;
        msg[strcspn(msg, "\n")] = '\0';
        for (unsigned i = 0; i < sizeof kpat / sizeof *kpat; i++) {
            if (!strstr(msg, kpat[i].has) || (kpat[i].also && !strstr(msg, kpat[i].also)))
                continue;
            /* when it happened: boot time + the record's own clock */
            struct timespec up;
            clock_gettime(CLOCK_BOOTTIME, &up);
            time_t when = time(NULL) - up.tv_sec + (time_t)(ts / 1000000ULL);
            char b[80];
            if (strstr(kpat[i].say, "killed")) {          /* name the victim */
                const char *p = strstr(msg, "Killed process");
                const char *o = p ? strchr(p, '(') : NULL;
                snprintf(b, sizeof b, "out of memory: killed %.30s", o ? o : "a program");
            } else
                snprintf(b, sizeof b, "%s", kpat[i].say);
            if (strstr(b, "SD card"))
                sd_errors++;
            add(when, kpat[i].sev, b, 1);
            break;
        }
    }
}

/* ---- the figures ---------------------------------------------------- */

static int throttled = -1, thr_was = -1;
static unsigned long long cpu_prev[4][2];      /* busy, total per core */
static int cpu_pct[4] = { -1, -1, -1, -1 };

static void read_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int c;
        unsigned long long u, ni, sy, id, io, irq, sirq, st;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &c, &u, &ni, &sy, &id, &io, &irq, &sirq, &st) != 9 || c < 0 || c > 3)
            continue;
        unsigned long long busy = u + ni + sy + irq + sirq + st, tot = busy + id + io;
        if (cpu_prev[c][1] && tot > cpu_prev[c][1])
            cpu_pct[c] = (int)(100 * (busy - cpu_prev[c][0]) / (tot - cpu_prev[c][1]));
        cpu_prev[c][0] = busy;
        cpu_prev[c][1] = tot;
    }
    fclose(f);
}

static const char *const cond_name[4] = {
    "under-voltage", "ARM frequency capped", "throttled", "soft temperature limit",
};

void stat_init(const char *cfg_path)
{
    /* the log lives beside psctrl.cfg (visible over the Samba share) */
    const char *slash = strrchr(cfg_path, '/');
    if (slash)
        snprintf(log_path, sizeof log_path, "%.*s/%s", (int)(slash - cfg_path), cfg_path, LOG_NAME);
    else
        snprintf(log_path, sizeof log_path, "%s", LOG_NAME);
    load_log();

    /* the kernel log since the last record ASTOS looked at */
    FILE *f = fopen(KMSG_SEQ, "r");
    if (f) {
        if (fscanf(f, "%llu", &kseq) != 1)
            kseq = 0;
        fclose(f);
    }
    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    read_kmsg();

    /* how the last environment ended (run-pistorm.sh) */
    f = fopen(LAST_EXIT, "r");
    if (f) {
        char build[40] = "";
        int code = -1;
        if (fscanf(f, "%39s %d", build, &code) == 2) {
            if (code == 0 || code == 42)
                stat_event(EV_INFO, "[%s] ended normally (%d)", build, code);
            else if (code == 1)
                stat_event(EV_ERROR, "[%s] failed to start (1)", build);
            else if (code > 128)
                stat_event(EV_ERROR, "[%s] crashed (signal %d)", build, code - 128);
            else
                stat_event(EV_WARN, "[%s] ended with code %d", build, code);
        }
        fclose(f);
        unlink(LAST_EXIT);
    }

    /* since-boot throttle bits that appeared while ASTOS was not running */
    f = fopen(THR_SEEN, "r");
    if (f) {
        if (fscanf(f, "%i", &thr_was) != 1)
            thr_was = -1;
        fclose(f);
    }
    read_cpu();
}

void stat_save(void)
{
    FILE *f = fopen(KMSG_SEQ, "w");
    if (f) {
        fprintf(f, "%llu\n", kseq);
        fclose(f);
    }
    if (throttled >= 0 && (f = fopen(THR_SEEN, "w"))) {
        fprintf(f, "0x%X\n", (unsigned)throttled);
        fclose(f);
    }
}

void stat_tick(int th)
{
    if (th >= 0) {
        int now = th & 0xF, past = (th >> 16) & 0xF;
        if (thr_was < 0) {                       /* the first look this boot */
            for (int b = 0; b < 4; b++) {
                int sev = b == 1 || b == 2 ? EV_INFO : EV_WARN;
                if ((now >> b) & 1)
                    stat_event(sev, "%s now", cond_name[b]);
                else if ((past >> b) & 1)
                    stat_event(sev, "%s since boot", cond_name[b]);
            }
        } else {
            int wnow = thr_was & 0xF, wpast = (thr_was >> 16) & 0xF;
            for (int b = 0; b < 4; b++) {
                int sev = b == 1 || b == 2 ? EV_INFO : EV_WARN;
                if ((now >> b) & 1 && !((wnow >> b) & 1))
                    stat_event(sev, "%s now", cond_name[b]);
                else if (!((now >> b) & 1) && (wnow >> b) & 1)
                    stat_event(EV_INFO, "%s cleared", cond_name[b]);
                else if ((past >> b) & 1 && !((wpast >> b) & 1))
                    stat_event(sev, "%s (while a build ran)", cond_name[b]);
            }
        }
        thr_was = th;
    }
    throttled = th;
    read_kmsg();
    read_cpu();
}

/* ---- the window ----------------------------------------------------- */

#define WX 4
#define WY 1
#define WW 632
#define WH 186
#define RH 10
#define BTN_W 88
#define FOOT_Y (WY + WH - 18)
#define NV 13                          /* event rows on screen */

static int open_, events_view, ev_top;

void stat_open(int events) { open_ = 1; events_view = events; ev_top = 0; }
int  stat_is_open(void)    { return open_; }

int stat_wheel(int n)
{
    if (!events_view)
        return 0;
    int max = nev - NV;
    ev_top -= 3 * n;
    if (ev_top > max) ev_top = max;
    if (ev_top < 0) ev_top = 0;
    return 1;
}

static void frame(const char *title)
{
    g_fill(WX + 2, WY + 1, WW, WH, C_TEXT);
    g_fill(WX, WY, WW, WH, C_WHITE);
    g_fill(WX, WY, WW, 11, C_ACCENT);
    g_frame(WX, WY, WW, WH, C_TEXT);
    g_text(WX + 6, WY + 2, title, C_WHITE);
}

static void button(int x, const char *label)
{
    g_fill(x, FOOT_Y, BTN_W, 12, C_FACE);
    g_frame(x, FOOT_Y, BTN_W, 12, C_TEXT);
    g_text(x + (BTN_W - g_text_w(label)) / 2, FOOT_Y + 2, label, C_TEXT);
}

static void row(int r, const char *label, const char *val, int c)
{
    int y = WY + 16 + r * RH;
    g_text(16, y, label, C_ACCENT);
    g_textn(96, y, val, 66, c);
}

static unsigned mhz(uint32_t id, uint32_t tag)
{
    uint32_t v[2] = { id, 0 };
    return sys_mbox(tag, v, 1, 2) == 0 ? v[1] / 1000000u : 0;
}

static double volts(uint32_t id)
{
    uint32_t v[2] = { id, 0 };
    return sys_mbox(0x00030003, v, 1, 2) == 0 ? v[1] / 1e6 : 0.0;
}

static void read_text(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = '\0';
    out[strcspn(out, "\n")] = '\0';
}

static void draw_status(void)
{
    char b[128];
    frame("Pi status");

    snprintf(b, sizeof b, "core 0 %3d%%  core 1 %3d%%  core 2 %3d%%  core 3 %3d%%",
             cpu_pct[0], cpu_pct[1], cpu_pct[2], cpu_pct[3]);
    if (cpu_pct[0] < 0) snprintf(b, sizeof b, "measuring...");
    row(0, "CPU", b, C_TEXT);

    unsigned arm = mhz(3, 0x00030047), arm_max = mhz(3, 0x00030004);
    unsigned core = mhz(4, 0x00030047), v3d = mhz(5, 0x00030047);
    if (arm)
        snprintf(b, sizeof b, "ARM %u MHz (max %u)   core %u MHz   V3D %u MHz",
                 arm, arm_max, core, v3d);
    else
        snprintf(b, sizeof b, "-");
    row(1, "Clocks", b, C_TEXT);

    double vc = volts(1);
    if (vc > 0)
        snprintf(b, sizeof b, "core %.2fV   SDRAM controller %.2fV  I/O %.2fV  PHY %.2fV",
                 vc, volts(2), volts(4), volts(3));
    else
        snprintf(b, sizeof b, "-");
    row(2, "Volts", b, C_TEXT);

    int t = sys_temp_c10();
    if (t >= 0)
        snprintf(b, sizeof b, "%d.%d C   (soft limit 80 C, hard limit 85 C)", t / 10, t % 10);
    else
        snprintf(b, sizeof b, "-");
    row(3, "Temp", b, t >= 800 ? C_ACCENT : C_TEXT);

    for (int c = 0; c < 4; c++) {
        if (throttled < 0) {
            row(4 + c, c == 0 ? "Power" : "", "-", C_TEXT);
            continue;
        }
        int now = (throttled >> c) & 1, past = (throttled >> (16 + c)) & 1;
        snprintf(b, sizeof b, "%-24s %s", cond_name[c],
                 now ? "NOW" : past ? "since boot, not now" : "never since boot");
        row(4 + c, c == 0 ? "Power" : "", b, now ? C_ACCENT : C_TEXT);
    }

    char mt[64], ma[64];
    long total = 0, avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %ld", &total) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld", &avail) == 1) continue;
        }
        fclose(f);
    }
    uint32_t gv[2] = { 0, 0 };
    int gpu = sys_mbox(0x00010006, gv, 0, 2) == 0 ? (int)(gv[1] >> 20) : -1;
    snprintf(mt, sizeof mt, "%ldMB used of %ldMB", (total - avail) >> 10, total >> 10);
    if (gpu >= 0)
        snprintf(ma, sizeof ma, ", GPU %dMB", gpu);
    else
        ma[0] = '\0';
    snprintf(b, sizeof b, "%s%s", mt, ma);
    row(8, "Memory", b, C_TEXT);

    struct statvfs sv;
    if (statvfs("/", &sv) == 0)
        snprintf(b, sizeof b, "%.1fGB free of %.1fGB, %d error%s seen",
                 (double)sv.f_bavail * sv.f_frsize / 1e9,
                 (double)sv.f_blocks * sv.f_frsize / 1e9, sd_errors, sd_errors == 1 ? "" : "s");
    else
        snprintf(b, sizeof b, "-");
    row(9, "SD card", b, sd_errors ? C_ACCENT : C_TEXT);

    char model[80];
    struct utsname un;
    read_text("/proc/device-tree/model", model, sizeof model);
    uname(&un);
    row(10, "Pi", model[0] ? model : "-", C_TEXT);
    double up = 0;
    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int ud = (int)up / 86400, uh = ((int)up % 86400) / 3600, um = ((int)up % 3600) / 60;
    if (ud)
        snprintf(b, sizeof b, "%.30s, up %dd %dh %dm", un.release, ud, uh, um);
    else
        snprintf(b, sizeof b, "%.30s, up %dh %dm", un.release, uh, um);
    row(11, "Kernel", b, C_TEXT);

    /* the first IPv4 address that is not loopback, and the Wi-Fi link */
    char ip[64] = "no network";
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo")) {
                char a[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
                snprintf(ip, sizeof ip, "%s (%.10s)", a, i->ifa_name);
                break;
            }
        freeifaddrs(ifa);
    }
    char wl[64] = "";
    f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            char name[16];
            int st;
            float q;
            if (sscanf(line, " %15[^:]: %x %f", name, &st, &q) == 3)
                snprintf(wl, sizeof wl, ", Wi-Fi link %.0f/70", q);
        }
        fclose(f);
    }
    snprintf(b, sizeof b, "%s%s", ip, wl);
    row(12, "Network", b, C_TEXT);

    button(16, "Events...");
    button(16 + BTN_W + 8, "Close");
    g_text(16 + 2 * (BTN_W + 8) + 8, FOOT_Y + 2, "E events   Esc close", C_DIM);
}

static void draw_events(void)
{
    frame("Pi events  (newest first)");
    if (!nev)
        g_text(16, WY + 16, "Nothing logged yet.", C_DIM);
    for (int r = 0; r < NV; r++) {
        int i = nev - 1 - (ev_top + r);
        if (i < 0)
            break;
        struct tm tm;
        localtime_r(&ev[i].t, &tm);
        char ts[24], b[110];
        strftime(ts, sizeof ts, "%d/%m %H:%M:%S", &tm);
        /* medium has one colour to spare for text, so the severity is a
         * word: ERROR / WARN in black, information in the dim colour */
        snprintf(b, sizeof b, "%s  %-5s  %s", ts,
                 ev[i].sev == EV_ERROR ? "ERROR" : ev[i].sev == EV_WARN ? "WARN" : "", ev[i].text);
        g_textn(16, WY + 16 + r * RH, b, 76, ev[i].sev >= EV_WARN ? C_TEXT : C_DIM);
    }
    if (ev_top > 0)
        g_text(WX + WW - 20, WY + 16, "^", C_ACCENT);
    if (ev_top + NV < nev)
        g_text(WX + WW - 20, WY + 16 + (NV - 1) * RH, "v", C_ACCENT);
    button(16, "Status...");
    button(16 + BTN_W + 8, "Close");
    char where[80];
    snprintf(where, sizeof where, "kept in %s", LOG_NAME);
    g_text(16 + 2 * (BTN_W + 8) + 8, FOOT_Y + 2, where, C_DIM);
}

void stat_draw(void)
{
    if (events_view)
        draw_events();
    else
        draw_status();
}

int stat_key(int key)
{
    switch (key) {
    case SI_ESC:
        open_ = 0;
        return 1;
    case SI_TAB:
        events_view = !events_view;
        ev_top = 0;
        break;
    case SI_CHAR:
        break;
    case SI_UP:
        if (events_view && ev_top > 0) ev_top--;
        break;
    case SI_DOWN:
        if (events_view && ev_top + NV < nev) ev_top++;
        break;
    case SI_HOME:
        ev_top = 0;
        break;
    default:
        break;
    }
    return 0;
}

int stat_click(int x, int y)
{
    if (y >= FOOT_Y && y < FOOT_Y + 12) {
        if (x >= 16 && x < 16 + BTN_W) {             /* Events / Status */
            events_view = !events_view;
            ev_top = 0;
            return 0;
        }
        if (x >= 16 + BTN_W + 8 && x < 16 + 2 * BTN_W + 8) {
            open_ = 0;
            return 1;
        }
    }
    if (events_view && x >= WX + WW - 24) {           /* the scroll marks */
        if (y < WY + 16 + 3 * RH && ev_top > 0) ev_top--;
        else if (y > WY + 16 + (NV - 3) * RH && ev_top + NV < nev) ev_top++;
    }
    return 0;
}
