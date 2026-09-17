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

/* How many choices `key` has, 0 if it is not one of these keys. */
int         se_count(const char *key);

/* Choice i, or NULL. */
const char *se_choice(const char *key, int i);

/* Where `val` sits in the list, or -1 (an unknown value, e.g. hand-edited). */
int         se_index(const char *key, const char *val);

#ifdef __cplusplus
}
#endif

#endif
