/*
 * astos_env.c - the environments ASTOS launches: the builds in
 * psctrl.cfg, read with the setup page's line editor (setup_cfg.c).
 *
 * Launching is run-pistorm.sh's job, not ours: ASTOS writes the section
 * name to /run/astos-launch and exits 44, the script runs
 *   ./emulator --config <cfg> --no-setup --build <section>
 * (which itself hands an `engine metal` build to stboxmetal), and when
 * that ends ASTOS is started again. ASTOS and the engines therefore never
 * share the bus: only one of them is ever running.
 */
#include <ctype.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include "astos.h"
#include "setup_cfg.h"

/* the boolean words config_file.c accepts; a bare key is on */
static int truthy(const char *v)
{
    if (!v)
        return 0;
    if (!*v)
        return 1;
    return !strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
           !strcasecmp(v, "true") || !strcasecmp(v, "enabled") ||
           !strcasecmp(v, "enable") || !strcasecmp(v, "exact");
}

static void make_title(const char *sec, char *out, size_t n)
{
    if (!strcasecmp(sec, "native"))      { snprintf(out, n, "Native ST"); return; }
    if (!strcasecmp(sec, "apj-os"))      { snprintf(out, n, "APJ-OS");    return; }
    if (strlen(sec) <= 4) {                       /* gem -> GEM */
        size_t i;
        for (i = 0; sec[i] && i + 1 < n; i++)
            out[i] = (char)toupper((unsigned char)sec[i]);
        out[i] = '\0';
        return;
    }
    snprintf(out, n, "%.23s", sec);
    out[0] = (char)toupper((unsigned char)out[0]);
}

/* a disk line's file name: "0:apj-os.img" -> "apj-os.img", path cut */
static const char *disk_name(const char *v)
{
    if (!v)
        return NULL;
    if (v[0] && v[1] == ':')
        v += 2;
    while (*v == ' ')
        v++;
    const char *s = strrchr(v, '/');
    return s ? s + 1 : v;
}

/* a switch word, as config_file.c reads one on a bus line */
static int switch_word(const char *v, int *on)
{
    static const char *const yes[] = { "enabled", "enable", "on", "true", "yes", "1" };
    static const char *const no[]  = { "disabled", "disable", "off", "false", "no", "0" };
    for (unsigned i = 0; i < sizeof yes / sizeof *yes; i++)
        if (!strcasecmp(v, yes[i])) { *on = 1; return 1; }
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++)
        if (!strcasecmp(v, no[i]))  { *on = 0; return 1; }
    return 0;
}

/* "IDE     on, apj-os.img +1" / "off": the bus switch (`ide`, or the
 * acsi line that is a switch word) and its image lines (`hdd`, or the
 * other acsi lines) */
static void bus_line(struct sc_cfg *c, const char *s, const char *sw_key,
                     const char *img_key, const char *label, char *out, size_t n)
{
    int on = 0, nimg = 0;
    const char *first = NULL;
    int ns = sc_count(c, s, sw_key);
    for (int i = 0; i < ns; i++) {
        const char *v = sc_get_n(c, s, sw_key, i);
        int w;
        if (v && (!*v || switch_word(v, &w)))
            on = !*v ? 1 : w;
    }
    int ni = sc_count(c, s, img_key);
    for (int i = 0; i < ni; i++) {
        const char *v = sc_get_n(c, s, img_key, i);
        int w;
        if (!v || !*v || switch_word(v, &w))
            continue;
        if (!first)
            first = disk_name(v);
        nimg++;
    }
    if (!on)
        snprintf(out, n, "%soff", label);
    else if (!first)
        snprintf(out, n, "%son, no image", label);
    else if (nimg > 1)
        snprintf(out, n, "%son, %.18s +%d", label, first, nimg - 1);
    else
        snprintf(out, n, "%son, %.22s", label, first);
}

/* "TOS 2.06" / "EmuTOS" from the ROM file's header (version at +2), or
 * the file name when it cannot be read. rom_path (from [psctrl], ~ the
 * invoking user's home) is where a bare name lives, relative to the
 * emulator's directory - where ASTOS runs. */
static void rom_text(struct sc_cfg *c, const char *rom, char *out, size_t n)
{
    if (!rom || !*rom) {
        snprintf(out, n, "no ROM");
        return;
    }
    char path[600];
    const char *rp = sc_get(c, "psctrl", "rom_path");
    if (strchr(rom, '/') || !rp || !*rp)
        snprintf(path, sizeof path, "%s", rom);
    else if (rp[0] == '~') {
        const struct passwd *pw = sc_invoking_user();
        snprintf(path, sizeof path, "%s%s/%s", pw && pw->pw_dir ? pw->pw_dir : "",
                 rp + 1, rom);
    } else
        snprintf(path, sizeof path, "%s/%s", rp, rom);

    static unsigned char buf[65536];
    FILE *f = fopen(path, "rb");
    size_t got = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    if (got < 0x30 || (buf[0] != 0x60)) {          /* no bra.s: not a TOS */
        snprintf(out, n, "%.30s", disk_name(rom));
        return;
    }
    for (size_t i = 0; i + 6 <= got; i++)
        if (!memcmp(buf + i, "EmuTOS", 6)) {
            snprintf(out, n, "EmuTOS");
            return;
        }
    unsigned v = (unsigned)buf[2] << 8 | buf[3];
    snprintf(out, n, "TOS %X.%02X", v >> 8, v & 0xFF);
}

static int is_machine_section(const char *s)
{
    return strcasecmp(s, "psctrl") && strcasecmp(s, "astos");
}

int env_load(struct sc_cfg *c, struct env *out, int max)
{
    char secs[MAX_ENV + 2][SC_SEC_LEN];
    int ns = sc_sections(c, secs, MAX_ENV + 2), n = 0;

    for (int i = 0; i < ns && n < max; i++) {
        const char *s = secs[i];
        if (!is_machine_section(s))
            continue;
        struct env *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->sec, sizeof e->sec, "%.15s", s);
        make_title(s, e->title, sizeof e->title);

        const char *eng = sc_get(c, s, "engine");
        e->metal = eng && !strncasecmp(eng, "metal", 5);
        int hdmi_vid = sc_get(c, s, "vga") != NULL || truthy(sc_get(c, s, "psvidel"));

        if (e->metal)                                   e->kind = K_METAL;
        else if (!strncasecmp(s, "apj", 3))             e->kind = K_APJ;
        else if (!strcasecmp(s, "native"))              e->kind = K_NATIVE;
        else if (hdmi_vid)                              e->kind = K_HDMI;
        else                                            e->kind = K_TOS;

        /* details panel: Machine (+ TOS), Memory, CPU, Blitter, IDE, ACSI,
         * Screen - 36 characters each. A metal build says so in its CPU
         * line; its memory and blitter are the board's own. */
        const char *mach = sc_get(c, s, "machine");
        const char *mw = mach && *mach ? mach : "st";
        const char *mname = !strcasecmp(mw, "ste") ? "Atari STE" :
                            !strcasecmp(mw, "megast") || !strcasecmp(mw, "mst") ? "Atari Mega ST" :
                            !strcasecmp(mw, "falcon") ? "Atari Falcon" : "Atari ST";
        const char *rom = sc_get(c, s, "rom");
        char tos[32];
        rom_text(c, rom, tos, sizeof tos);
        snprintf(e->line[0], sizeof e->line[0], "Machine %s, %.22s", mname, tos);

        if (e->metal)
            snprintf(e->line[1], sizeof e->line[1], "Memory  the board's ST-RAM");
        else {
            int falcon = !strcasecmp(mw, "falcon");
            const char *st = sc_get(c, s, falcon ? "falcon_stram" : "stram_size");
            const char *tt = sc_get(c, s, "ttram");
            char ttb[24] = "";
            if (tt && (truthy(tt) || isdigit((unsigned char)*tt)))
                snprintf(ttb, sizeof ttb, ", %sB TT-RAM",
                         isdigit((unsigned char)*tt) ? tt : "128M");
            snprintf(e->line[1], sizeof e->line[1], "Memory  %sB ST-RAM%s",
                     st && *st && isdigit((unsigned char)*st) ? st : "4M", ttb);
        }

        if (e->metal) {
            snprintf(e->line[2], sizeof e->line[2], "CPU     68000 8 MHz, real chips");
        } else {
            const char *cpu = sc_get(c, s, "cpu");
            const char *jp  = sc_get(c, s, "jit_power");
            /* jit_power absent = 1 (on) for a 68020 and up; a 68000/010
             * has no JIT */
            int big = cpu && atoi(cpu) >= 68020;
            int jit = big && (jp ? (*jp && *jp != '0') : 1);
            snprintf(e->line[2], sizeof e->line[2], "CPU     %s%s%s%s",
                     cpu && *cpu ? cpu : "68000",
                     truthy(sc_get(c, s, "fpu")) ? " + FPU" : "",
                     jit ? ", JIT" : ", interpreter",
                     truthy(sc_get(c, s, "cpu_compatible")) ? ", exact" : "");
        }

        /* blitter: real = the board's chip, a true word = emulated, a
         * false word = none, no line = what the machine shipped with */
        const char *bl = sc_get(c, s, "blitter");
        int blw;
        const char *bt;
        if (e->metal)
            bt = "the board's";
        else if (bl && !strcasecmp(bl, "real"))
            bt = "real chip";
        else if (bl && (!*bl || (switch_word(bl, &blw) && blw)))
            bt = "emulated";
        else if (bl && switch_word(bl, &blw) && !blw)
            bt = "none";
        else
            bt = strcasecmp(mw, "st") ? "emulated (default)" : "none (default)";
        snprintf(e->line[3], sizeof e->line[3], "Blitter %s", bt);

        bus_line(c, s, "ide", "hdd", "IDE     ", e->line[4], sizeof e->line[4]);
        bus_line(c, s, "acsi", "acsi", "ACSI    ", e->line[5], sizeof e->line[5]);

        const char *nh = sc_get(c, s, "native_hdmi");
        if (sc_get(c, s, "vga"))
            snprintf(e->line[6], sizeof e->line[6], "Screen  HDMI (graphics card)");
        else if (truthy(sc_get(c, s, "psvidel")))
            snprintf(e->line[6], sizeof e->line[6], "Screen  HDMI (PSVIDEL)");
        else if (nh && (truthy(nh) || !strcasecmp(nh, "snapshot")))
            snprintf(e->line[6], sizeof e->line[6], "Screen  ST monitor + HDMI");
        else
            snprintf(e->line[6], sizeof e->line[6], "Screen  ST monitor");
    }
    return n;
}

int env_launch(struct sc_cfg *c, const char *cfg_path, const char *sec)
{
    /* remember the choice for the next start (selection, not autostart) */
    const char *was = sc_get(c, "psctrl", "astos_last");
    if (!was || strcmp(was, sec)) {
        sc_set(c, "psctrl", "astos_last", sec);
        if (sc_save(c, cfg_path) != 0)
            fprintf(stderr, "[ASTOS] could not save %s: %s\n", cfg_path,
                    sc_save_error(c));
    }
    FILE *f = fopen(ASTOS_LAUNCH_FILE, "w");
    if (!f) {
        perror("[ASTOS] " ASTOS_LAUNCH_FILE);
        return -1;
    }
    fprintf(f, "%s\n", sec);
    return fclose(f);
}

/* a line pinned to drive A (A: or 0:) */
static int is_drive_a(const char *v)
{
    return v[0] && v[1] == ':' && ((v[0] | 0x20) == 'a' || v[0] == '0');
}

int env_play(const char *cfg_path, const char *sec, const char *image, const char *title)
{
    static struct sc_cfg c;
    if (sc_load(&c, cfg_path) != 0)
        return -1;
    sc_del_section(&c, ASTOS_PLAY_SECTION);
    if (sc_copy_section(&c, sec, ASTOS_PLAY_SECTION) != 0)
        return -1;
    /* drive A: this disk, first; the build's other disks stay after it -
     * an unpinned one then takes drive B, and on metal they are the disks
     * Ctrl+Alt+F2.. puts in */
    int n = sc_count(&c, ASTOS_PLAY_SECTION, "fdd");
    char keep[10][300];
    int nk = 0;
    for (int i = 0; i < n; i++) {
        const char *v = sc_get_n(&c, ASTOS_PLAY_SECTION, "fdd", i);
        if (v && !is_drive_a(v) && nk < 10)
            snprintf(keep[nk++], sizeof keep[0], "%s", v);
    }
    while (sc_count(&c, ASTOS_PLAY_SECTION, "fdd"))
        sc_set_n(&c, ASTOS_PLAY_SECTION, "fdd", 0, NULL);
    char a[PATH_MAX + 4];
    snprintf(a, sizeof a, "A:%s", image);
    if (sc_set_n(&c, ASTOS_PLAY_SECTION, "fdd", 0, a) != 0)
        return -1;
    for (int i = 0; i < nk; i++)
        sc_set_n(&c, ASTOS_PLAY_SECTION, "fdd", i + 1, keep[i]);
    if (title)
        sc_set(&c, ASTOS_PLAY_SECTION, "title", title);
    /* beside psctrl.cfg, owned like it */
    char out[PATH_MAX];
    const char *sl = strrchr(cfg_path, '/');
    if (sl)
        snprintf(out, sizeof out, "%.*s/astos-play.cfg", (int)(sl - cfg_path), cfg_path);
    else
        snprintf(out, sizeof out, "astos-play.cfg");
    if (sc_save(&c, out) != 0) {
        fprintf(stderr, "[ASTOS] could not save %s: %s\n", out, sc_save_error(&c));
        return -1;
    }
    struct stat st;
    if (stat(cfg_path, &st) == 0)
        (void)!chown(out, st.st_uid, st.st_gid);
    FILE *f = fopen(ASTOS_LAUNCH_FILE, "w");
    if (!f) {
        perror("[ASTOS] " ASTOS_LAUNCH_FILE);
        return -1;
    }
    fprintf(f, "%s\n%s\n", ASTOS_PLAY_SECTION, out);
    printf("[ASTOS] play %s in [%s] (from %s)\n", image, sec, out);
    return fclose(f);
}
