/*
 * setup_page.h - the pre-boot setup page: pick the section to boot, edit
 * its settings, save, boot.
 *
 * Draws on the real shifter through shifter_setup.c, reads keys from all
 * three sources through setup_input.c, and edits psctrl.cfg through
 * setup_cfg.c. Nothing here knows about the emulator's own config: the
 * page works on the file, and the emulator loads the section afterwards.
 */
#ifndef SETUP_PAGE_H
#define SETUP_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "shifter_setup.h"

/* Switch rows: a key whose value is empty (a bare key in the .cfg means
 * on) or one of the boolean words config_file.c understands. They show
 * and are written as enabled / disabled. Exposed for the harness. */
int         sp_is_switch(const char *val);
int         sp_row_is_switch(const char *key, const char *val);
int         sp_row_on(const char *key, const char *val);
const char *sp_switch_text(const char *val);

/* How a .cfg key and value are presented, and what a typed value is
 * written back as. Exposed for the harness. */
const char *sp_row_label(const char *key, const char *val);
const char *sp_row_value(const char *key, const char *val, char *buf,
                         unsigned long n);
const char *sp_value_from_edit(const char *key, const char *typed, char *buf,
                               unsigned long n);

/* The JIT and what depends on it (see setup_page.c). sp_jit_rule: why a
 * key cannot be ticked now, or NULL. sp_jit_follow: after `key` changed,
 * bring jit_cache / cpu_compatible / cpu_speed / jit_power into line;
 * `did` says what it changed ("" = nothing). Exposed for the harness. */
struct sc_cfg;
const char *sp_jit_rule(const struct sc_cfg *c, const char *sec, const char *key);
void        sp_jit_follow(struct sc_cfg *c, const char *sec, const char *key,
                          char *did, unsigned long n);
/* after `machine` or `shifter` changed: shifter st on st/megast, ste on ste */
void        sp_shifter_follow(struct sc_cfg *c, const char *sec, const char *key,
                              char *did, unsigned long n);

enum sp_result {
    SP_BOOT = 0,      /* boot the section named in `chosen`        */
    SP_QUIT,          /* leave without booting (the test tool)     */
    SP_ERROR
};

/* Runs until the countdown expires, Boot is chosen, or the page is quit.
 * `chosen` gets the section name to boot. */
/* The emulator's screendump writer (et4000.c: same directory, numbering
 * and encoder as the console's `s`). Called on F12 / Help with the page
 * as 640x400 XRGB; fills `path` with what it wrote. Unset = no dumps. */
typedef int (*sp_dump_fn)(const uint32_t *xrgb, int w, int h, char *path, unsigned long n);
void sp_set_dumper(sp_dump_fn fn);
/* "F12" or "HELP": the dump key the builds screen names for a build whose
 * `kbd` value is kbd_val (NULL = none) - F12 only when `kbd usb` is on. */
const char *sp_dump_key(const char *kbd_val);

enum sp_result sp_run(struct ss_screen *ss, const char *cfg_path,
                      char *chosen, unsigned long chosen_len);

#ifdef __cplusplus
}
#endif

#endif
