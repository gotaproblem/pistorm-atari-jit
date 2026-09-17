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
};

/* 0 on success, -1 if the file cannot be read or has too many lines. */
int sc_load(struct sc_cfg *c, const char *path);
int sc_load_text(struct sc_cfg *c, const char *text);

/* The value (may be ""), or NULL if the key is not in that section. */
const char *sc_get(const struct sc_cfg *c, const char *sec, const char *key);

/* Set, add or (val == NULL) remove a key. A new key is added at the end
 * of its section; a new section is added at the end of the file.
 * Returns 0, or -1 if the file is full. */
int sc_set(struct sc_cfg *c, const char *sec, const char *key, const char *val);

/* Write the file, keeping the previous one as <path>.bak. The new file
 * gets the owner of the directory it lands in, so a save while running
 * as root under pistorm.service does not leave a root-owned .cfg. */
int sc_save(struct sc_cfg *c, const char *path);

/* Section names in file order, for the boot picker. Returns how many. */
int sc_sections(const struct sc_cfg *c, char out[][SC_SEC_LEN], int max);

/* Keys of one section in file order, for a settings list. */
int sc_keys(const struct sc_cfg *c, const char *sec,
            char out[][SC_KEY_LEN], int max);

/* Convenience for [psctrl]: integer with a default. */
int sc_get_int(const struct sc_cfg *c, const char *sec, const char *key, int dflt);

#ifdef __cplusplus
}
#endif

#endif
