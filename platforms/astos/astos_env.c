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
#include <stdlib.h>
#include <stdio.h>
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

        /* details panel: 6 lines, 36 characters each */
        if (e->metal) {
            snprintf(e->line[0], sizeof e->line[0], "Engine  stboxmetal, real chips");
            snprintf(e->line[1], sizeof e->line[1], "CPU     68000 8 MHz (in the Pi)");
        } else {
            const char *cpu = sc_get(c, s, "cpu");
            const char *jp  = sc_get(c, s, "jit_power");
            /* jit_power absent = 1 (on) for a 68020 and up; a 68000/010
             * has no JIT */
            int big = cpu && atoi(cpu) >= 68020;
            int jit = big && (jp ? (*jp && *jp != '0') : 1);
            snprintf(e->line[0], sizeof e->line[0], "Engine  JIT emulator");
            snprintf(e->line[1], sizeof e->line[1], "CPU     %s%s%s%s",
                     cpu && *cpu ? cpu : "68000",
                     truthy(sc_get(c, s, "fpu")) ? " + FPU" : "",
                     jit ? ", JIT" : ", interpreter",
                     truthy(sc_get(c, s, "cpu_compatible")) ? ", exact" : "");
        }
        const char *mach = sc_get(c, s, "machine");
        const char *st   = sc_get(c, s, "stram_size");
        const char *tt   = sc_get(c, s, "ttram");
        snprintf(e->line[2], sizeof e->line[2], "Machine %s%s%s%s",
                 mach && *mach ? mach : "st",
                 st && *st ? ", ST-RAM " : "", st && *st ? st : "",
                 (!e->metal && tt && truthy(tt)) ? ", TT-RAM" :
                 (!e->metal && tt && *tt && isdigit((unsigned char)*tt)) ? ", TT-RAM" : "");
        const char *rom = sc_get(c, s, "rom");
        snprintf(e->line[3], sizeof e->line[3], "ROM     %s", rom && *rom ? disk_name(rom) : "-");

        const char *hdd = sc_get(c, s, "hdd");
        const char *fdd = sc_get(c, s, "fdd");
        const char *acs = sc_get_n(c, s, "acsi", 1);
        int more = sc_count(c, s, "hdd") + sc_count(c, s, "fdd") +
                   (sc_count(c, s, "acsi") > 1 ? sc_count(c, s, "acsi") - 1 : 0);
        const char *first = hdd ? disk_name(hdd) : fdd ? disk_name(fdd) :
                            acs ? disk_name(acs) : NULL;
        if (first)
        {
            char extra[16] = "";
            if (more > 1)
                snprintf(extra, sizeof extra, " +%d", more - 1);
            snprintf(e->line[4], sizeof e->line[4], "Disks   %.24s%s", first, extra);
        }
        else
            snprintf(e->line[4], sizeof e->line[4], "Disks   -");

        const char *nh = sc_get(c, s, "native_hdmi");
        if (sc_get(c, s, "vga"))
            snprintf(e->line[5], sizeof e->line[5], "Screen  HDMI (graphics card)");
        else if (truthy(sc_get(c, s, "psvidel")))
            snprintf(e->line[5], sizeof e->line[5], "Screen  HDMI (PSVIDEL)");
        else if (nh && (truthy(nh) || !strcasecmp(nh, "snapshot")))
            snprintf(e->line[5], sizeof e->line[5], "Screen  ST monitor + HDMI");
        else
            snprintf(e->line[5], sizeof e->line[5], "Screen  ST monitor");
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
