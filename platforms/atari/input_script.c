/*
 * input_script.c - PISTORM_INPUT_SCRIPT=file: play a regression game's
 * input (tools/regress/games/<game>.in, the format tools/regress/hatari/
 * drive.py plays in Hatari) into the guest.
 *
 *   start D:\JIMPOWER\START.TOS   time 0 is this program's Pexec
 *   seconds 110                   then stop the emulator (exit 0)
 *   3.0  fire                     joystick 1 fire for 0.2 s
 *   3.0  joy1 right fire          joystick 1 held so until the next joy1
 *   3.5  joy1 -                   joystick 1 let go
 *   4.0  key 0x39                 ST scancode pressed for 0.1 s
 *   4.0  keydown 0x1D / keyup 0x1D
 *   72   mark play                "[INPUT] mark play" in the log, for the
 *                                 runner's checks
 *   (shots, machine: Hatari's; ignored here)
 *
 * Keys go to the guest through kbd_usb.c's injection ring, the joystick
 * through joy_usb.c's pad state - both the paths a USB keyboard and pad
 * take, so the guest reads them from the ACIA as it reads the ST's own
 * keyboard, and the native hotkeys watch them the same way. Needs the
 * injection layer: emulator.c turns it on (as "usb gamepad") when this
 * variable is set. Timed on the host's clock, like drive.py on Hatari's.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "platforms/atari/input_script.h"
#include "platforms/atari/joy_usb.h"
#include "platforms/atari/kbd_usb.h"

extern void pistorm_request_exit(int mode);

enum { EV_JOY, EV_KD, EV_KU, EV_MARK };
typedef struct { double t; int kind; int v; char label[48]; } ev_t;

static ev_t   *evs;
static int     nev;
static char    start_name[128];          /* basename, upper case */
static char    start_path[256];
static double  seconds = 60;
static volatile int armed;               /* waiting for the Pexec */
static volatile int go;
static struct timespec t0;
static pthread_t tid;

static int add(double t, int kind, int v, const char *label)
{
    ev_t *n = realloc(evs, (size_t)(nev + 1) * sizeof *evs);
    if (!n)
        return -1;
    evs = n;
    evs[nev].t = t;
    evs[nev].kind = kind;
    evs[nev].v = v;
    snprintf(evs[nev].label, sizeof evs[nev].label, "%s", label ? label : "");
    nev++;
    return 0;
}

static int joy_bits(char *s)
{
    int bits = 0;
    for (char *w = strtok(s, " \t"); w; w = strtok(NULL, " \t")) {
        if (!strcmp(w, "up")) bits |= STJOY_UP;
        else if (!strcmp(w, "down")) bits |= STJOY_DOWN;
        else if (!strcmp(w, "left")) bits |= STJOY_LEFT;
        else if (!strcmp(w, "right")) bits |= STJOY_RIGHT;
        else if (!strcmp(w, "fire")) bits |= STJOY_FIRE;
        else if (!strcmp(w, "-")) ;
        else return -1;
    }
    return bits;
}

static int cmp_ev(const void *a, const void *b)
{
    const ev_t *x = a, *y = b;
    if (x->t != y->t)
        return x->t < y->t ? -1 : 1;
    return 0;
}

/* parse; 0 ok, else the bad line's number */
static int parse(FILE *f)
{
    char ln[256];
    int n = 0;
    while (fgets(ln, sizeof ln, f)) {
        n++;
        char *h = strchr(ln, '#');
        if (h)
            *h = 0;
        char *s = ln;
        while (isspace((unsigned char)*s))
            s++;
        for (char *e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); )
            *--e = 0;
        if (!*s)
            continue;
        if (!strncmp(s, "start ", 6)) {
            const char *p = s + 6;
            while (*p == ' ')
                p++;
            snprintf(start_path, sizeof start_path, "%s", p);
            const char *b = strrchr(p, '\\');
            b = b ? b + 1 : p;
            snprintf(start_name, sizeof start_name, "%s", b);
            for (char *c = start_name; *c; c++)
                *c = (char)toupper((unsigned char)*c);
            continue;
        }
        if (!strncmp(s, "seconds ", 8)) {
            seconds = atof(s + 8);
            continue;
        }
        if (!strncmp(s, "shots ", 6) || !strncmp(s, "machine ", 8))
            continue;
        char *end;
        double t = strtod(s, &end);
        if (end == s)
            return n;
        while (*end == ' ' || *end == '\t')
            end++;
        char word[32] = "";
        if (sscanf(end, "%31s", word) != 1)
            return n;
        char *rest = end + strlen(word);
        while (*rest == ' ' || *rest == '\t')
            rest++;
        if (!strcmp(word, "fire")) {
            add(t, EV_JOY, STJOY_FIRE, NULL);
            add(t + 0.2, EV_JOY, 0, NULL);
        } else if (!strcmp(word, "joy1")) {
            int b = joy_bits(rest);
            if (b < 0)
                return n;
            add(t, EV_JOY, b, NULL);
        } else if (!strcmp(word, "key") || !strcmp(word, "keydown") ||
                   !strcmp(word, "keyup")) {
            char *e2;
            long k = strtol(rest, &e2, 0);
            if (e2 == rest || k < 1 || k > 0x7F)
                return n;
            if (word[3] == 0) {
                add(t, EV_KD, (int)k, NULL);
                add(t + 0.1, EV_KU, (int)k, NULL);
            } else
                add(t, word[3] == 'd' ? EV_KD : EV_KU, (int)k, NULL);
        } else if (!strcmp(word, "mark")) {
            add(t, EV_MARK, 0, rest);
        } else
            return n;
    }
    qsort(evs, (size_t)nev, sizeof *evs, cmp_ev);   /* stable enough: ties keep no order need */
    return 0;
}

static double since_t0(void)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (double)(n.tv_sec - t0.tv_sec) + (double)(n.tv_nsec - t0.tv_nsec) * 1e-9;
}

static void *player(void *arg)
{
    (void)arg;
    while (!go)
        usleep(10000);
    printf("[INPUT] %s started - playing %d events over %g s\n", start_path, nev, seconds);
    fflush(stdout);
    int i = 0;
    int joy = 0;
    while (since_t0() < seconds) {
        const double now = since_t0();
        while (i < nev && evs[i].t <= now) {
            const ev_t *e = &evs[i++];
            switch (e->kind) {
            case EV_JOY:
                joy = e->v;
                joy_usb_script_set(1, (uint8_t)joy);
                printf("[INPUT] %6.2f s joystick 1 $%02X\n", now, joy);
                break;
            case EV_KD:
                kbd_usb_script_key((uint8_t)e->v, 1);
                printf("[INPUT] %6.2f s key $%02X down\n", now, e->v);
                break;
            case EV_KU:
                kbd_usb_script_key((uint8_t)e->v, 0);
                printf("[INPUT] %6.2f s key $%02X up\n", now, e->v);
                break;
            case EV_MARK:
                printf("[INPUT] mark %s at %.1f s\n", e->label, now);
                break;
            }
            fflush(stdout);
        }
        usleep(5000);
    }
    joy_usb_script_set(1, 0);
    printf("[INPUT] script done after %g s - stopping\n", seconds);
    fflush(stdout);
    pistorm_request_exit(0);
    return NULL;
}

int input_script_init(void)
{
    const char *path = getenv("PISTORM_INPUT_SCRIPT");
    if (!path || !*path)
        return 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[INPUT] cannot open %s\n", path);
        return 0;
    }
    int bad = parse(f);
    fclose(f);
    if (bad) {
        fprintf(stderr, "[INPUT] %s line %d not understood - no input script\n", path, bad);
        return 0;
    }
    if (!start_name[0]) {
        fprintf(stderr, "[INPUT] %s has no start line - no input script\n", path);
        return 0;
    }
    if (pthread_create(&tid, NULL, player, NULL)) {
        fprintf(stderr, "[INPUT] thread create failed\n");
        return 0;
    }
    pthread_detach(tid);
    armed = 1;
    printf("[INPUT] %s: %d events, waiting for %s to start\n", path, nev, start_name);
    return 1;
}

int input_script_wanted(void)
{
    const char *path = getenv("PISTORM_INPUT_SCRIPT");
    return path && *path;
}

/* cpu/newcpu.cpp, at a GEMDOS Pexec (load and go, or load): the program
 * name as the guest gave it. The first one whose file name is the
 * script's start program is time 0. CPU thread. */
void input_script_pexec(const char *name)
{
    if (!armed || !name)
        return;
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    if (strcasecmp(b, start_name))
        return;
    armed = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    go = 1;
}

int input_script_armed(void)
{
    return armed;
}
