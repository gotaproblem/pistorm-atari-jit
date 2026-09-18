/*
 * setup_enums.h - the .cfg keys whose value is one of a known set.
 *
 * Every list here is taken from config_file.c, not invented: the cpu
 * names are its cpu_types[] minus NONE, the graphics pairs are
 * graphics_card_types[] x graphics_card_drivers[] as the `vga` line
 * spells them, and machine / shifter / blitter / monitor are the words
 * its own parser accepts. So a value picked here always parses.
 */
#ifndef SETUP_ENUMS_H
#define SETUP_ENUMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Which environment a setting belongs to, and what it depends on.
 *
 * APJ-OS runs FreeMiNT + XaAES + fVDI on the ET4000 over HDMI, so the
 * ST's own video shape (monitor detect, shifter model) means nothing
 * there; plain GEM runs on the ST's shifter, so the ET4000/HDMI keys mean
 * nothing to it. Rows outside the section's environment are hidden, and
 * Tab shows them greyed rather than gone.
 *
 * Some keys are only interesting when another one is on - the eight
 * network_* keys behind `network`, the stbox_* keys behind `stbox` - so
 * they hide with it. */
#define SE_GEM   0x1
#define SE_APJ   0x2
#define SE_BOTH  (SE_GEM | SE_APJ)

int         se_env(const char *key);          /* SE_* mask; BOTH if unlisted */
const char *se_needs(const char *key);        /* key that must be on, or NULL */

/* Keys the page never offers to change:
 *   retired   - parsed and read by nothing (vga_render, loopcycles, rtc):
 *               the parser ignores them, and the page hides them so a
 *               leftover line cannot be mistaken for a setting
 *   developer - real, but tuning/debug knobs (addr32, stram_cache,
 *               stram_direct, network_debug, jit): hidden unless Tab */
int         se_retired(const char *key);
int         se_developer(const char *key);

/* Keys worth offering even when the .cfg does not have them, so a
 * machine can be given a setting it is missing rather than only edited.
 * Walk them with se_known(i); se_known_default() is the value a row gets
 * when it is added. */
const char *se_known(int i);
const char *se_known_default(const char *key);

/* The catalogue: every key the emulator accepts, on the tab that holds
 * the code that reads it. The editor lists ALL of them, ticked when the
 * build has the line. Repeatable keys (hdd, acsi, hostfs) are one entry
 * here; the Drives tab expands them into slots. */
enum se_tab { SE_TAB_MACHINE = 0, SE_TAB_VIDEO, SE_TAB_SOUND, SE_TAB_INPUT,
              SE_TAB_DRIVES, SE_TAB_NETWORK, SE_TAB_TUNING, SE_TAB_N };
enum se_kind { SE_K_LIST = 0, SE_K_SWITCH, SE_K_TEXT, SE_K_INT };

const char *se_tab_name(int tab);
int         se_tab_count(int tab);               /* keys on that tab */
const char *se_tab_key(int tab, int i);          /* the ith key      */
int         se_kind(const char *key);            /* SE_K_*           */
/* what a key is written as the moment it is ticked: a switch "enabled",
 * a list its first real choice (se_known_default if listed), a text key
 * "" = ask for the value first */
const char *se_tick_value(const char *key);

/* NULL = the key is allowed with this cpu, else why not ("needs 68020+") */
const char *se_cpu_rule(const char *key, const char *cpu);
/* a switch the emulator has ON when the line is absent (jit) */
int         se_absent_on(const char *key);

/* How many choices `key` has, 0 if it is not one of these keys. */
int         se_count(const char *key);

/* Choice i - the value written to the .cfg - or NULL. */
const char *se_choice(const char *key, int i);

/* What choice i reads as on screen; the value itself when no separate
 * label exists ("3 - 1024 blocks (default)" for jit_power 3). */
const char *se_label(const char *key, int i);

/* Where `val` sits in the list, or -1 (an unknown value, e.g. hand-edited). */
int         se_index(const char *key, const char *val);

#ifdef __cplusplus
}
#endif

#endif
