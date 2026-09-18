/*
 * setup_enums.c - see setup_enums.h.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "setup_enums.h"

/* cpu_types[] in config_file.c, without NONE */
static const char *cpu[]     = { "68000", "68010", "68020", "68030",
                                 "68040", "68060" };
/* CONFITEM_MACHINE: st / ste / megast are the only hosts for the board */
static const char *machine[] = { "st", "ste", "megast" };
/* CONFITEM_SHIFTER: strncasecmp(arg, "ste") decides, so two values */
static const char *shifter[] = { "st", "ste" };
/* CONFITEM_BLITTER: "real" = pass-through, else a boolean */
static const char *blitter[] = { "disabled", "enabled", "real" };
static const char *blitter_l[]={ "off", "emulated", "real - the board's own chip" };
/* CONFITEM_MONITOR: forces the GPIP7 monitor-detect bit */
static const char *monitor[] = { "auto", "mono", "colour" };
/* the `vga` line is "<card> <driver>". Only the ET4000 has an
 * implementation (platforms/atari/et4000); ATI and MATROX are names in
 * graphics_card_types[] with nothing behind them, so they are not offered.
 * The four drivers are real: each selects an address map in emulator.c's
 * et4kaddresses[] (NOVA/NVDI/FVDI at $D00000, XVDI at $B00000). */
static const char *vga[]     = { "ET4000AX FVDI", "ET4000AX NVDI",
                                 "ET4000AX NOVA", "ET4000AX XVDI", "NONE" };
/* jit_cache is in KB; 16384 is the compiled-in maximum */
static const char *cache[]   = { "2048", "4096", "8192", "16384" };
/* CONFITEM_TTRAM: a bool, or a size; the parser clamps at 256 MB, which
 * is the natmem reserve and the emulator.c ceiling ("all three must agree") */
static const char *ttram[]   = { "disabled", "32M", "64M", "128M", "256M" };
/* CONFITEM_STBOX_MACHINE */
static const char *stbox[]   = { "st", "ste" };
/* audio_frames: the SDL buffer ladder (psctrl_settings.cpp L_frames) */
static const char *frames[]  = { "512", "1024", "2048", "4096", "8192" };
/* jit_power: 0 is the JIT off; 1..6 is the compiled-chain budget on a
 * log2 ladder, 256 << (n-1), so 3 is the 1024 default (psctrl_settings.cpp) */
static const char *jitpow[]  = { "0", "1", "2", "3", "4", "5", "6" };
static const char *jitpow_l[]= { "0 - JIT disabled", "1 - 256", "2 - 512",
                                 "3 - 1024 (default)", "4 - 2048",
                                 "5 - 4096", "6 - 8192" };
/* stram_size. The Pi-side ST-RAM is a flat 4 MB backed by Pi memory, and
 * that is what the guest sees when the key is ABSENT - a 1 MB board is
 * told it has 4 MB. Setting the key does the opposite: the Pi aliases
 * exactly like the real banks so TOS sizes the true amount, which the
 * native Shifter display needs (TOS puts the frame buffer at the top of
 * what it thinks it has). So "not set" is the way to get 4 MB, and the
 * sizes below CAP at the chips actually on the board. The empty value
 * means "remove the key". */
static const char *stram[]   = { "", "128K", "512K", "1M", "2M", "2560K", "4M" };
static const char *stram_l[] = { "not set - flat 4 MB, Pi-backed (APJ-OS)",
                                 "128K  (honest: caps at the board)",
                                 "512K  (honest: caps at the board)",
                                 "1M    (honest: caps at the board)",
                                 "2M    (honest: caps at the board)",
                                 "2.5M  (honest: caps at the board)",
                                 "4M    (honest: caps at the board)" };

/* The words the parser accepts for the same choice. A bare key counts as
 * the empty string: `blitter` alone means enabled, `ttram` alone means
 * 128M (CONFITEM_TTRAM: empty or a true word -> 128 MB). */
struct alias { const char *key, *word, *choice; };
static const struct alias aliases[] = {
    { "blitter", "",            "enabled"  },
    { "blitter", "on",          "enabled"  },
    { "blitter", "1",           "enabled"  },
    { "blitter", "yes",         "enabled"  },
    { "blitter", "true",        "enabled"  },
    { "blitter", "enable",      "enabled"  },
    { "blitter", "off",         "disabled" },
    { "blitter", "0",           "disabled" },
    { "blitter", "no",          "disabled" },
    { "blitter", "false",       "disabled" },
    { "blitter", "disable",     "disabled" },
    { "ttram",   "",            "128M"     },
    { "ttram",   "on",          "128M"     },
    { "ttram",   "1",           "128M"     },
    { "ttram",   "yes",         "128M"     },
    { "ttram",   "true",        "128M"     },
    { "ttram",   "enabled",     "128M"     },
    { "ttram",   "enable",      "128M"     },
    { "ttram",   "off",         "disabled" },
    { "ttram",   "0",           "disabled" },
    { "ttram",   "no",          "disabled" },
    { "ttram",   "false",       "disabled" },
    { "ttram",   "disable",     "disabled" },
    { "monitor", "",            "auto"     },
    { "monitor", "color",       "colour"   },
    { "monitor", "rgb",         "colour"   },
    { "monitor", "sc1224",      "colour"   },
    { "monitor", "monochrome",  "mono"     },
    { "monitor", "sm124",       "mono"     },
    { "monitor", "high",        "mono"     },
    { "shifter", "",            "st"       },
    { "machine", "mst",         "megast"   },
};

struct table { const char *key; const char **list; const char **labels; int n; };

#define T(k, a)      { k, a, NULL, (int)(sizeof(a) / sizeof(a[0])) }
#define TL(k, a, l)  { k, a, l,    (int)(sizeof(a) / sizeof(a[0])) }
static const struct table tables[] = {
    T("cpu", cpu), T("machine", machine), T("shifter", shifter),
    TL("blitter", blitter, blitter_l), T("monitor", monitor), T("vga", vga),
    T("jit_cache", cache), T("ttram", ttram), T("stbox_machine", stbox),
    T("audio_frames", frames),
    TL("jit_power", jitpow, jitpow_l),
    TL("stram_size", stram, stram_l),
};
#undef T
#undef TL

/*
 * Keys the page offers even when the section does not have them: a
 * machine with no `network` line could not be given one, because the
 * page could only edit what was already in the file. A row for one of
 * these shows its default until it is set, and setting it writes the key.
 */
static const struct { const char *key, *dflt; } known[] = {
    { "network",     "disabled" },
    { "jit_power",   "3"        },
    { "jit_cache",   "16384"    },
    { "stram_size",  ""         },   /* "" = not set = flat 4 MB */
    { "blitter",     "enabled"  },
    { "dma_sound",   "disabled" },
    { "acsi",        "disabled" },
    { "monitor",     "auto"     },
    { "shifter",     "st"       },
    { "stbox_tos",   ""         },
};

const char *se_known(int i)
{
    if (i < 0 || i >= (int)(sizeof known / sizeof known[0]))
        return NULL;
    return known[i].key;
}

const char *se_known_default(const char *key)
{
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcasecmp(key, known[i].key))
            return known[i].dflt;
    return NULL;
}

/*
 * Where a setting belongs. Only the exceptions are listed: anything not
 * here is SE_BOTH, so a new .cfg key shows up in both sections rather
 * than vanishing.
 */
static const struct { const char *key; int env; } env_of[] = {
    /* the ST's own video: APJ-OS draws through fVDI on the ET4000 */
    { "monitor",        SE_GEM },
    { "shifter",        SE_GEM },
    /* slow the machine down to ST speed - for ST software, not a desktop */
    { "cpu_compatible", SE_GEM },
    { "m68k_speed",     SE_GEM },
    { "cpu_clock_multiplier", SE_GEM },
    /* vga is NOT here: a GEM build drives the ET4000 through NVDI.
     * native_hdmi and fps are NOT here: native_hdmi is the ST-screen
     * mirror on HDMI (config_file.c: "the native_hdmi ST-screen mirror"),
     * which a GEM machine on an HDMI monitor needs, and fps paces the
     * HDMI render thread whichever source it shows. */
    /* the ST Box - a sandboxed ST in a GEM window under APJ-OS */
    { "stbox_tos",      SE_APJ },
    { "stbox_machine",  SE_APJ },
    { "stbox_plane",    SE_APJ },
    { "stbox_slice_cyc",SE_APJ },
    { "stbox_telemetry",SE_APJ },
};

/* Retired: parsed and read by nothing (the parser ignores them with a
 * note). Never a row, so a leftover line cannot pass for a setting.
 * Developer: real, but tuning/debug knobs - hidden unless Tab. */
static const char *retired[]   = { "vga_render", "loopcycles", "rtc" };
static const char *developer[] = { "addr32", "stram_cache", "stram_direct",
                                   "network_debug", "jit" };

int se_retired(const char *key)
{
    for (unsigned i = 0; i < sizeof retired / sizeof retired[0]; i++)
        if (!strcasecmp(key, retired[i]))
            return 1;
    return 0;
}

int se_developer(const char *key)
{
    for (unsigned i = 0; i < sizeof developer / sizeof developer[0]; i++)
        if (!strcasecmp(key, developer[i]))
            return 1;
    return 0;
}

/* Keys that only matter while another key is on. */
static const struct { const char *key, *needs; } needs_of[] = {
    { "network_backend",  "network" },
    { "network_tap",      "network" },
    { "network_base",     "network" },
    { "network_mac",      "network" },
    { "network_irq",      "network" },
    { "network_host_ip",  "network" },
    { "network_atari_ip", "network" },
    { "network_netmask",  "network" },
    { "network_debug",    "network" },
};

int se_env(const char *key)
{
    for (unsigned i = 0; i < sizeof env_of / sizeof env_of[0]; i++)
        if (!strcasecmp(key, env_of[i].key))
            return env_of[i].env;
    return SE_BOTH;
}

const char *se_needs(const char *key)
{
    for (unsigned i = 0; i < sizeof needs_of / sizeof needs_of[0]; i++)
        if (!strcasecmp(key, needs_of[i].key))
            return needs_of[i].needs;
    return NULL;
}

/*
 * The catalogue. Placed by what reads the key: Machine = the CPU/memory/
 * ROM shape, Video = ET4000 + HDMI + shifter, Sound = YM/DMA, Input =
 * kbd/usb/mouse, Drives = the images (expanded to slots by the page),
 * Network = the tap, Tuning = the PSCTRL live tunables and developer
 * knobs. Kind decides the control: LIST has a chooser (se_count > 0),
 * SWITCH toggles, TEXT is typed, INT is typed and checked as a number.
 */
static const struct { const char *key; int tab; int kind; const char *tick; } cat[] = {
    /* Machine */
    { "cpu",            SE_TAB_MACHINE, SE_K_LIST,   "68040"   },
    { "fpu",            SE_TAB_MACHINE, SE_K_SWITCH, "enabled" },
    { "mmu",            SE_TAB_MACHINE, SE_K_SWITCH, "enabled" },
    { "machine",        SE_TAB_MACHINE, SE_K_LIST,   "ste"     },
    { "stram_size",     SE_TAB_MACHINE, SE_K_LIST,   "1M"      },
    { "ttram",          SE_TAB_MACHINE, SE_K_LIST,   "128M"    },
    { "blitter",        SE_TAB_MACHINE, SE_K_LIST,   "enabled" },
    { "jit_power",      SE_TAB_MACHINE, SE_K_LIST,   "3"       },
    { "jit_cache",      SE_TAB_MACHINE, SE_K_LIST,   "16384"   },
    { "rom",            SE_TAB_MACHINE, SE_K_TEXT,   ""        },
    { "stbox_tos",      SE_TAB_MACHINE, SE_K_TEXT,   ""        },
    { "stbox_machine",  SE_TAB_MACHINE, SE_K_LIST,   "ste"     },
    { "cpu_compatible", SE_TAB_MACHINE, SE_K_SWITCH, "enabled" },
    /* Video */
    { "vga",            SE_TAB_VIDEO,   SE_K_LIST,   "ET4000AX FVDI" },
    { "fps",            SE_TAB_VIDEO,   SE_K_INT,    "60"      },
    { "native_hdmi",    SE_TAB_VIDEO,   SE_K_SWITCH, "enabled" },
    { "monitor",        SE_TAB_VIDEO,   SE_K_LIST,   "auto"    },
    { "shifter",        SE_TAB_VIDEO,   SE_K_LIST,   "st"      },
    { "stbox_plane",    SE_TAB_VIDEO,   SE_K_INT,    "0"       },
    { "drm_dirtyband",  SE_TAB_VIDEO,   SE_K_SWITCH, "1"       },
    { "drm_async",      SE_TAB_VIDEO,   SE_K_SWITCH, "1"       },
    { "vbl_refract_ns", SE_TAB_VIDEO,   SE_K_INT,    "5000000" },
    /* Sound */
    { "ym2149",         SE_TAB_SOUND,   SE_K_SWITCH, "enabled" },
    { "dma_sound",      SE_TAB_SOUND,   SE_K_SWITCH, "enabled" },
    { "ym_gain",        SE_TAB_SOUND,   SE_K_INT,    "100"     },
    { "ym_lag_ms",      SE_TAB_SOUND,   SE_K_INT,    "100"     },
    { "lmc",            SE_TAB_SOUND,   SE_K_SWITCH, "1"       },
    { "audio_frames",   SE_TAB_SOUND,   SE_K_LIST,   "2048"    },
    /* Input */
    { "kbd",            SE_TAB_INPUT,   SE_K_SWITCH, "usb"     },
    { "usb",            SE_TAB_INPUT,   SE_K_SWITCH, "gamepad" },
    { "mouse_thresh",   SE_TAB_INPUT,   SE_K_INT,    "0"       },
    { "mouse_scale",    SE_TAB_INPUT,   SE_K_INT,    "1"       },
    /* Drives - the page expands hdd/acsi/hostfs into slots (step 3) */
    { "ide",            SE_TAB_DRIVES,  SE_K_SWITCH, "enabled" },
    { "hdd",            SE_TAB_DRIVES,  SE_K_TEXT,   ""        },
    { "acsi",           SE_TAB_DRIVES,  SE_K_SWITCH, "enabled" },   /* the switch; images are the other acsi lines */
    { "fdd",            SE_TAB_DRIVES,  SE_K_TEXT,   ""        },
    { "hostfs",         SE_TAB_DRIVES,  SE_K_TEXT,   ""        },
    /* Network */
    { "network",        SE_TAB_NETWORK, SE_K_SWITCH, "enabled" },
    { "network_backend",SE_TAB_NETWORK, SE_K_TEXT,   "tap"     },
    { "network_tap",    SE_TAB_NETWORK, SE_K_TEXT,   "tap0"    },
    { "network_base",   SE_TAB_NETWORK, SE_K_TEXT,   "0x00F10000" },
    { "network_mac",    SE_TAB_NETWORK, SE_K_TEXT,   ""        },
    { "network_irq",    SE_TAB_NETWORK, SE_K_INT,    "4"       },
    { "network_host_ip",SE_TAB_NETWORK, SE_K_TEXT,   "192.168.50.1" },
    { "network_atari_ip",SE_TAB_NETWORK,SE_K_TEXT,   "192.168.50.2" },
    { "network_netmask",SE_TAB_NETWORK, SE_K_TEXT,   "255.255.255.0" },
    { "network_debug",  SE_TAB_NETWORK, SE_K_SWITCH, "enabled" },
    /* Tuning - live tunables and developer knobs */
    { "jit",            SE_TAB_TUNING,  SE_K_SWITCH, "enabled" },
    { "m68k_speed",     SE_TAB_TUNING,  SE_K_INT,    "0"       },
    { "cpu_clock_multiplier", SE_TAB_TUNING, SE_K_INT, "1"     },
    { "stram_cache",    SE_TAB_TUNING,  SE_K_SWITCH, "enabled" },
    { "stram_direct",   SE_TAB_TUNING,  SE_K_SWITCH, "enabled" },
    { "addr32",         SE_TAB_TUNING,  SE_K_SWITCH, "enabled" },
    { "comp_constjump", SE_TAB_TUNING,  SE_K_SWITCH, "1"       },
    { "compnf",         SE_TAB_TUNING,  SE_K_SWITCH, "1"       },
    { "compfpu",        SE_TAB_TUNING,  SE_K_SWITCH, "1"       },
    { "blit_timed_ns",  SE_TAB_TUNING,  SE_K_INT,    "0"       },
    { "stbox_slice_cyc",SE_TAB_TUNING,  SE_K_INT,    "64"      },
    { "stbox_telemetry",SE_TAB_TUNING,  SE_K_SWITCH, "1"       },
    { "ipl_confirm_ns", SE_TAB_TUNING,  SE_K_INT,    "2000"    },
    { "blit_trace",     SE_TAB_TUNING,  SE_K_INT,    "0"       },
    { "debug",          SE_TAB_TUNING,  SE_K_TEXT,   ""        },
};
#define NCAT ((int)(sizeof cat / sizeof cat[0]))
static const char *tab_names[SE_TAB_N] = { "Machine", "Video", "Sound", "Input",
                                           "Drives", "Network", "Tuning" };

const char *se_tab_name(int tab)
{
    return (tab >= 0 && tab < SE_TAB_N) ? tab_names[tab] : "";
}

int se_tab_count(int tab)
{
    int n = 0;
    for (int i = 0; i < NCAT; i++)
        if (cat[i].tab == tab)
            n++;
    return n;
}

const char *se_tab_key(int tab, int i)
{
    for (int k = 0; k < NCAT; k++)
        if (cat[k].tab == tab && i-- == 0)
            return cat[k].key;
    return NULL;
}

int se_kind(const char *key)
{
    for (int k = 0; k < NCAT; k++)
        if (!strcasecmp(key, cat[k].key))
            return cat[k].kind;
    return SE_K_TEXT;
}

const char *se_tick_value(const char *key)
{
    for (int k = 0; k < NCAT; k++)
        if (!strcasecmp(key, cat[k].key))
            return cat[k].tick;
    return "";
}

/*
 * What the cpu line allows. config_file.c drops fpu/ttram/addr32 below a
 * 68020 ("CPU is 24-bit only"), the MMU cores exist for 68030+ only, and
 * cpu_compatible is the prefetch-accurate 68000 core. A key outside its
 * rule is greyed on the page, skipped by the cursor and never ticked.
 */
static const struct { const char *key; const char *cpus; const char *why; } rule[] = {
    { "fpu",            " 68020 68030 68040 68060 ", "needs 68020+" },
    { "ttram",          " 68020 68030 68040 68060 ", "needs 68020+" },
    { "addr32",         " 68020 68030 68040 68060 ", "needs 68020+" },
    { "mmu",            " 68030 68040 68060 ",       "needs 68030+" },
    { "cpu_compatible", " 68000 ",                   "68000 only"   },
};

const char *se_cpu_rule(const char *key, const char *cpu)
{
    char pad[16];
    if (!cpu || !*cpu)
        cpu = "68000";                        /* config_file.c: cpu_type 0 */
    snprintf(pad, sizeof pad, " %.12s ", cpu);
    for (unsigned i = 0; i < sizeof rule / sizeof rule[0]; i++)
        if (!strcasecmp(key, rule[i].key))
            return strstr(rule[i].cpus, pad) ? NULL : rule[i].why;
    return NULL;
}

/*
 * Switches the emulator has ON when the line is absent, and the word
 * that turns them off. config_file.c starts from all-zero except jit and
 * blitter; the PSCTRL tunables (psctrl_tunables.c) default lmc and
 * drm_dirtyband to 1 and comp_* to -1 (= the JIT's own default, on).
 */
static const struct { const char *key; const char *off; } absent_on[] = {
    { "jit",            "disabled" },
    { "blitter",        "disabled" },
    { "lmc",            "0" },
    { "drm_dirtyband",  "0" },
    { "comp_constjump", "0" },
    { "compnf",         "0" },
    { "compfpu",        "0" },
    /* not an absent-on default: written out so the image lines below it
     * stay in the file, ignored, until the switch comes back */
    { "acsi",           "disabled" },
};

const char *se_off_value(const char *key)
{
    for (unsigned i = 0; i < sizeof absent_on / sizeof absent_on[0]; i++)
        if (!strcasecmp(key, absent_on[i].key))
            return absent_on[i].off;
    return NULL;
}

/*
 * Ranges of the typed numbers, as PSCTRL's own table (psctrl_settings.cpp)
 * and config_file.c clamp them. The default is the catalogue tick value.
 */
static const struct { const char *key; long lo, hi; } irange[] = {
    { "fps",             10, 60 },
    { "vbl_refract_ns",   0, 20000000 },
    { "ym_gain",          0, 400 },        /* hundredths: 100 = unity */
    { "ym_lag_ms",        5, 200 },
    { "mouse_thresh",     0, 15 },
    { "mouse_scale",      1, 16 },
    { "stbox_plane",      0, 128 },
    { "stbox_slice_cyc",  8, 4096 },
    { "blit_timed_ns",    0, 2000 },
    { "ipl_confirm_ns",   0, 1000000 },
    { "m68k_speed",      -1, 20 },
    { "cpu_clock_multiplier", 0, 8 },
    { "blit_trace",       0, 1000000 },
    { "network_irq",      1, 7 },
};

int se_int_range(const char *key, long *lo, long *hi)
{
    for (unsigned i = 0; i < sizeof irange / sizeof irange[0]; i++)
        if (!strcasecmp(key, irange[i].key)) {
            if (lo) *lo = irange[i].lo;
            if (hi) *hi = irange[i].hi;
            return 1;
        }
    return 0;
}

/*
 * Which build a key belongs to. A build whose name starts with "apj" is
 * an APJ-OS build (apj-os, apj-os-test ...); anything else is a GEM
 * machine. stbox_* only mean something under APJ-OS; the ST-speed and
 * ST-video keys only mean something to a GEM machine.
 */
const char *se_env_rule(const char *key, const char *section)
{
    int apj = section && !strncasecmp(section, "apj", 3);
    int env = se_env(key);
    if (env == SE_APJ && !apj) return "apj-os only";
    if (env == SE_GEM && apj)  return "gem only";
    return NULL;
}

static const struct table *find(const char *key)
{
    for (unsigned i = 0; i < sizeof tables / sizeof tables[0]; i++)
        if (!strcasecmp(key, tables[i].key))
            return &tables[i];
    return NULL;
}

int se_count(const char *key)
{
    const struct table *t = find(key);
    return t ? t->n : 0;
}

const char *se_choice(const char *key, int i)
{
    const struct table *t = find(key);
    if (!t || i < 0 || i >= t->n)
        return NULL;
    return t->list[i];
}

const char *se_label(const char *key, int i)
{
    const struct table *t = find(key);
    if (!t || i < 0 || i >= t->n)
        return NULL;
    return t->labels ? t->labels[i] : t->list[i];
}

int se_index(const char *key, const char *val)
{
    const struct table *t = find(key);
    if (!t || !val)
        return -1;
    for (int i = 0; i < t->n; i++)
        if (!strcasecmp(val, t->list[i]))
            return i;
    /* a spelling the parser accepts for one of the choices */
    for (unsigned a = 0; a < sizeof aliases / sizeof aliases[0]; a++)
        if (!strcasecmp(key, aliases[a].key) &&
            !strcasecmp(val, aliases[a].word))
            for (int i = 0; i < t->n; i++)
                if (!strcasecmp(aliases[a].choice, t->list[i]))
                    return i;
    return -1;
}
