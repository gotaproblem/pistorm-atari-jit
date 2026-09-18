/*
 * setup_cfg.h - psctrl.cfg: one file, three sections.
 *
 *   [psctrl]   the setup page's own keys (countdown, last boot choice)
 *   [gem]      the plain GEM machine
 *   [apj-os]   the APJ-OS machine
 *
 * This is a LINE EDITOR over the file, not a serialiser, for the same
 * reason config_file_save.c is: the emulator's parser discards comments
 * and ordering and normalises values, so writing a parsed structure back
 * out would rewrite the file's meaning as well as strip its comments.
 * Every line this code does not change is copied through byte for byte.
 *
 * A key's value is the rest of its line, so `hostfs S /home/...` and a
 * bare `fpu` (present, empty value) both work. Comments are whole lines
 * starting with # or /, which is what the emulator's parser accepts.
 */
#ifndef SETUP_CFG_H
#define SETUP_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

#define SC_MAX_LINES   768
#define SC_SEC_LEN     16
#define SC_KEY_LEN     40
#define SC_LINE_LEN    256

struct sc_line {
    char sec[SC_SEC_LEN];      /* "" before the first section header */
    char key[SC_KEY_LEN];      /* "" for a comment or a blank line   */
    char text[SC_LINE_LEN];    /* the line itself, no newline        */
};

struct sc_cfg {
    struct sc_line line[SC_MAX_LINES];
    int  n;
    int  dirty;
    char path[512];
    char err[96];               /* why the last sc_save failed */
};

/* 0 on success, -1 if the file cannot be read or has too many lines. */
int sc_load(struct sc_cfg *c, const char *path);
int sc_load_text(struct sc_cfg *c, const char *text);

/* The value (may be ""), or NULL if the key is not in that section.
 * sc_get is the FIRST occurrence; some keys repeat (hdd up to 8 times,
 * in order), so sc_get_n addresses the nth (0-based). */
const char *sc_get(const struct sc_cfg *c, const char *sec, const char *key);
const char *sc_get_n(const struct sc_cfg *c, const char *sec, const char *key,
                     int n);

/* Set, add or (val == NULL) remove a key. A new key is added at the end
 * of its section; a new section is added at the end of the file.
 * Returns 0, or -1 if the file is full. sc_set edits the first
 * occurrence; sc_set_n edits the nth, and adds only when n is the next
 * one (n == count), so a second hdd can be appended but never a gap. */
int sc_set(struct sc_cfg *c, const char *sec, const char *key, const char *val);
int sc_set_n(struct sc_cfg *c, const char *sec, const char *key, int n,
             const char *val);

/* How many times `key` appears in `sec`. */
int sc_count(const struct sc_cfg *c, const char *sec, const char *key);

/* Write the file, keeping the previous one as <path>.bak. The new file
 * gets the owner of the directory it lands in, so a save while running
 * as root under pistorm.service does not leave a root-owned .cfg. */
int sc_save(struct sc_cfg *c, const char *path);
/* the step and errno text of the last failed save ("rename .tmp: ...") */
const char *sc_save_error(const struct sc_cfg *c);

/* Section names in file order, for the boot picker. Returns how many. */
int sc_sections(const struct sc_cfg *c, char out[][SC_SEC_LEN], int max);

/* Keys of one section in file order, for a settings list. */
int sc_keys(const struct sc_cfg *c, const char *sec,
            char out[][SC_KEY_LEN], int max);

/* Find psctrl.cfg the way the emulator finds its .cfg - relative to the
 * binary's directory, in the runtime tree beside the repo (INSTALL-README
 * section 2) - and NOT through $HOME, which is /root under sudo and under
 * pistorm.service. Search order:
 *
 *   ../configs/psctrl.cfg      the runtime tree, as run-pistorm.sh uses
 *   configs/psctrl.cfg         a repo-local config
 *   ~<invoking user>/configs/psctrl.cfg   ($SUDO_USER, not $HOME)
 *
 * If none exists but configs/psctrl.cfg.default does, it is copied to the
 * first writable candidate and that path returned, so a fresh tree comes
 * up configured. Returns 0 and fills `out`, or -1. `created` is set to 1
 * when the file was made from the default. */
int sc_locate(char *out, unsigned long n, int *created);

/* Same, with the home directory given rather than looked up. sc_locate()
 * is this with the invoking user's home; the harness passes a scratch
 * one so the test cannot be swayed by a real ~/configs/psctrl.cfg. */
int sc_locate_at(const char *home, char *out, unsigned long n, int *created);

/* Convenience for [psctrl]: integer with a default. */
int sc_get_int(const struct sc_cfg *c, const char *sec, const char *key, int dflt);

#ifdef __cplusplus
}
#endif

#endif
