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

enum sp_result {
    SP_BOOT = 0,      /* boot the section named in `chosen`        */
    SP_QUIT,          /* leave without booting (the test tool)     */
    SP_ERROR
};

/* Runs until the countdown expires, Boot is chosen, or the page is quit.
 * `chosen` gets the section name to boot. */
enum sp_result sp_run(struct ss_screen *ss, const char *cfg_path,
                      char *chosen, unsigned long chosen_len);

#ifdef __cplusplus
}
#endif

#endif
