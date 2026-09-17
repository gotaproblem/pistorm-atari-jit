/*
 * setup_enums.c - see setup_enums.h.
 */
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
/* CONFITEM_MONITOR: forces the GPIP7 monitor-detect bit */
static const char *monitor[] = { "auto", "mono", "colour" };
/* the `vga` line is "<card> <driver>" */
static const char *vga[]     = { "ET4000AX FVDI", "ET4000AX NVDI",
                                 "ET4000AX NOVA", "ET4000AX XVDI", "NONE" };
/* jit_cache is in KB; 16384 is the compiled-in maximum */
static const char *cache[]   = { "2048", "4096", "8192", "16384" };
/* CONFITEM_TTRAM: a bool, or a size */
static const char *ttram[]   = { "disabled", "32M", "64M", "128M" };
/* CONFITEM_STBOX_MACHINE */
static const char *stbox[]   = { "st", "ste" };

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

struct table { const char *key; const char **list; int n; };

#define T(k, a) { k, a, (int)(sizeof(a) / sizeof(a[0])) }
static const struct table tables[] = {
    T("cpu", cpu), T("machine", machine), T("shifter", shifter),
    T("blitter", blitter), T("monitor", monitor), T("vga", vga),
    T("jit_cache", cache), T("ttram", ttram), T("stbox_machine", stbox),
};
#undef T

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
