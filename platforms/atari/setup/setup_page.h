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
#include "setup_input.h"

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
/* Before a save: give every build a line for each switch it is missing,
 * set to its no-line default, and the mouse_thresh / mouse_scale lines */
void sp_fill_defaults(struct sc_cfg *c);
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

/* For ASTOS. sp_run_edit: one build's editor only (section), or with
 * section "" the builds screen with no countdown; leaving returns
 * SP_QUIT, "Boot now" returns SP_BOOT with the build in `chosen`.
 * sp_set_poll: read keys from fn instead of si_poll (NULL restores). */
typedef struct si_event (*sp_poll_fn)(int timeout_ms);
enum sp_result sp_run_edit(struct ss_screen *ss, const char *cfg_path,
                           const char *section, char *chosen,
                           unsigned long chosen_len);
void sp_set_poll(sp_poll_fn fn);

/* The editor as a model (ASTOS draws its own settings window over it):
 * the page's rows, rules and pickers for one build, without the page.
 * spm_open loads the build; spm_set_tab picks a tab (setup_enums.h
 * SE_TAB_*); rows are read with spm_row. spm_activate is the page's
 * Enter on a row (a list opens its choices, a file key the picker, a
 * text key the editor, a switch flips), spm_toggle its Space (tick /
 * untick: a set key is removed from the build). The SPM_* mode says
 * what to show next; spm_choose / spm_pick / spm_commit finish it, and
 * spm_msg says what happened. Nothing is written until spm_save. */
enum spm_kind { SPM_KEY = 0, SPM_LABEL, SPM_SLOT, SPM_ADD };
enum spm_mode { SPM_DONE = 0, SPM_CHOOSE, SPM_PICK, SPM_EDIT };
struct spm_row {
    enum spm_kind kind;
    char key[32];          /* the cfg key                              */
    char name[48];         /* what the row is called                   */
    char value[96];        /* what it is set to, as shown              */
    int  on;               /* the build has it (a switch: it is on)    */
    int  usable;           /* can be changed here                      */
    const char *why;       /* greyed: why not ("needs 68020+"), or NULL */
    const char *hint;      /* default and range of a number, or NULL   */
    int  is_switch, is_list, is_file, is_text;
    char whybuf[48];
};
int  spm_open(const char *cfg_path, const char *section);
int  spm_tab(void);
void spm_set_tab(int tab);
int  spm_rows(void);
void spm_row(int i, struct spm_row *out);
enum spm_mode spm_activate(int i);
enum spm_mode spm_toggle(int i);
int  spm_choices(void);
const char *spm_choice(int i);
int  spm_choice_off(int i);
int  spm_choice_cur(void);
void spm_choose(int i);
int  spm_files(void);
const char *spm_file(int i);
int  spm_file_cur(void);
enum spm_mode spm_pick(int i);          /* -1: type a name instead */
const char *spm_edit_text(void);
enum spm_mode spm_commit(const char *text);
void spm_cancel(void);
const char *spm_msg(void);
int  spm_dirty(void);
const char *spm_section(void);
int  spm_save(void);

#ifdef __cplusplus
}
#endif

#endif
