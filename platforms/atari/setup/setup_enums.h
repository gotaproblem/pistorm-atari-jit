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
