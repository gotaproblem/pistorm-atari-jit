/*
 * setup_cfg.c - see setup_cfg.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "setup_cfg.h"

static void lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* "[gem]" -> "gem", else 0 */
static int section_header(const char *line, char *out, unsigned long n)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line != '[')
        return 0;
    line++;
    unsigned long i = 0;
    while (*line && *line != ']' && i + 1 < n)
        out[i++] = (char)tolower((unsigned char)*line++);
    out[i] = '\0';
    return *line == ']' && i > 0;
}

/* first token of a line, lower-cased; 0 for a comment or a blank line */
static int line_key(const char *line, char *out, unsigned long n)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#' || *line == '/' || *line == '[' ||
        *line == '\r' || *line == '\n')
        return 0;
    unsigned long i = 0;
    while (*line && !isspace((unsigned char)*line) && i + 1 < n)
        out[i++] = (char)tolower((unsigned char)*line++);
    out[i] = '\0';
    return i > 0;
}

static const char *line_value(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    while (*line && !isspace((unsigned char)*line))
        line++;
    while (*line == ' ' || *line == '\t')
        line++;
    return line;
}

int sc_load_text(struct sc_cfg *c, const char *text)
{
    memset(c, 0, sizeof *c);
    char sec[SC_SEC_LEN] = "";

    while (*text) {
        const char *nl = strchr(text, '\n');
        unsigned long len = nl ? (unsigned long)(nl - text) : strlen(text);
        if (len && text[len - 1] == '\r')
            len--;
        if (c->n >= SC_MAX_LINES)
            return -1;
        struct sc_line *l = &c->line[c->n++];
        if (len >= SC_LINE_LEN)
            len = SC_LINE_LEN - 1;
        memcpy(l->text, text, len);
        l->text[len] = '\0';

        char name[SC_SEC_LEN];
        if (section_header(l->text, name, sizeof name))
            snprintf(sec, sizeof sec, "%s", name);
        snprintf(l->sec, sizeof l->sec, "%s", sec);
        if (!line_key(l->text, l->key, sizeof l->key))
            l->key[0] = '\0';

        if (!nl)
            break;
        text = nl + 1;
    }
    return 0;
}

int sc_load(struct sc_cfg *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    static char buf[SC_MAX_LINES * SC_LINE_LEN];
    unsigned long n = fread(buf, 1, sizeof buf - 1, f);
    int over = !feof(f);
    fclose(f);
    if (over)
        return -1;
    buf[n] = '\0';
    if (sc_load_text(c, buf) != 0)
        return -1;
    snprintf(c->path, sizeof c->path, "%s", path);
    return 0;
}

static struct sc_line *find(const struct sc_cfg *c, const char *sec,
                           const char *key)
{
    char s[SC_SEC_LEN], k[SC_KEY_LEN];
    snprintf(s, sizeof s, "%s", sec); lower(s);
    snprintf(k, sizeof k, "%s", key); lower(k);
    for (int i = 0; i < c->n; i++)
        if (c->line[i].key[0] && !strcmp(c->line[i].sec, s) &&
            !strcmp(c->line[i].key, k))
            return (struct sc_line *)&c->line[i];
    return NULL;
}

const char *sc_get(const struct sc_cfg *c, const char *sec, const char *key)
{
    struct sc_line *l = find(c, sec, key);
    return l ? line_value(l->text) : NULL;
}

int sc_get_int(const struct sc_cfg *c, const char *sec, const char *key, int dflt)
{
    const char *v = sc_get(c, sec, key);
    if (!v || !*v)
        return dflt;
    char *end;
    long n = strtol(v, &end, 0);
    return end == v ? dflt : (int)n;
}

/* index one past the last line of a section, or -1 if there is no such
 * section; trailing blank lines stay below the insertion point */
static int section_end(const struct sc_cfg *c, const char *sec)
{
    char s[SC_SEC_LEN];
    snprintf(s, sizeof s, "%s", sec); lower(s);
    int last = -1;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->line[i].sec, s) && c->line[i].text[0])
            last = i;
    return last < 0 ? -1 : last + 1;
}

static int insert(struct sc_cfg *c, int at, const char *sec, const char *text)
{
    if (c->n >= SC_MAX_LINES)
        return -1;
    memmove(&c->line[at + 1], &c->line[at],
            (unsigned long)(c->n - at) * sizeof c->line[0]);
    c->n++;
    struct sc_line *l = &c->line[at];
    memset(l, 0, sizeof *l);
    snprintf(l->sec, sizeof l->sec, "%s", sec);
    snprintf(l->text, sizeof l->text, "%s", text);
    line_key(l->text, l->key, sizeof l->key);
    return 0;
}

int sc_set(struct sc_cfg *c, const char *sec, const char *key, const char *val)
{
    char s[SC_SEC_LEN], k[SC_KEY_LEN];
    snprintf(s, sizeof s, "%s", sec); lower(s);
    snprintf(k, sizeof k, "%s", key); lower(k);

    struct sc_line *l = find(c, s, k);
    if (!val) {                                  /* remove the key */
        if (!l)
            return 0;
        int at = (int)(l - c->line);
        memmove(&c->line[at], &c->line[at + 1],
                (unsigned long)(c->n - at - 1) * sizeof c->line[0]);
        c->n--;
        c->dirty = 1;
        return 0;
    }
    if (l) {
        char text[SC_LINE_LEN];
        if (*val)
            snprintf(text, sizeof text, "%s %s", k, val);
        else
            snprintf(text, sizeof text, "%s", k);
        if (strcmp(text, l->text)) {
            snprintf(l->text, sizeof l->text, "%s", text);
            c->dirty = 1;
        }
        return 0;
    }

    char text[SC_LINE_LEN];
    if (*val)
        snprintf(text, sizeof text, "%s %s", k, val);
    else
        snprintf(text, sizeof text, "%s", k);

    int at = section_end(c, s);
    if (at < 0) {                                /* new section at the end */
        char hdr[SC_LINE_LEN];
        snprintf(hdr, sizeof hdr, "[%s]", s);
        if (c->n && c->line[c->n - 1].text[0] && insert(c, c->n, s, "") != 0)
            return -1;
        if (insert(c, c->n, s, hdr) != 0)
            return -1;
        at = c->n;
    }
    if (insert(c, at, s, text) != 0)
        return -1;
    c->dirty = 1;
    return 0;
}

/* The emulator runs as root under pistorm.service, so a file it creates
 * would come out root-owned and the user could no longer edit their own
 * config. Same fix as config_file_save.c: take the owner of the
 * directory the file lands in. */
static void take_ownership(FILE *out, const char *path)
{
    char dir[512];
    const char *slash = strrchr(path, '/');
    struct stat st, dst;
    mode_t mode = 0664;

    if (stat(path, &st) == 0)
        mode |= st.st_mode & 07777;
    if (!slash)
        snprintf(dir, sizeof dir, ".");
    else if (slash == path)
        snprintf(dir, sizeof dir, "/");
    else
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);

    /* best effort both times: a save is not worth failing over the
     * ownership of the file it just wrote */
    int fd = fileno(out);
    if (stat(dir, &dst) == 0 && dst.st_uid != 0) {
        int rc = fchown(fd, dst.st_uid, dst.st_gid);
        (void)rc;
    }
    (void)fchmod(fd, mode);
}

int sc_save(struct sc_cfg *c, const char *path)
{
    if (!path)
        path = c->path;
    if (!path || !*path)
        return -1;

    char tmp[600], bak[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(bak, sizeof bak, "%s.bak", path);

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    take_ownership(f, path);
    for (int i = 0; i < c->n; i++)
        fprintf(f, "%s\n", c->line[i].text);
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (access(path, F_OK) == 0) {
        unlink(bak);
        if (rename(path, bak) != 0) {
            unlink(tmp);
            return -1;
        }
    }
    if (rename(tmp, path) != 0)
        return -1;
    snprintf(c->path, sizeof c->path, "%s", path);
    c->dirty = 0;
    return 0;
}

int sc_sections(const struct sc_cfg *c, char out[][SC_SEC_LEN], int max)
{
    int n = 0;
    for (int i = 0; i < c->n && n < max; i++) {
        if (!c->line[i].sec[0])
            continue;
        int seen = 0;
        for (int k = 0; k < n; k++)
            seen |= !strcmp(out[k], c->line[i].sec);
        if (!seen)
            snprintf(out[n++], SC_SEC_LEN, "%s", c->line[i].sec);
    }
    return n;
}

int sc_keys(const struct sc_cfg *c, const char *sec,
            char out[][SC_KEY_LEN], int max)
{
    char s[SC_SEC_LEN];
    snprintf(s, sizeof s, "%s", sec); lower(s);
    int n = 0;
    for (int i = 0; i < c->n && n < max; i++)
        if (c->line[i].key[0] && !strcmp(c->line[i].sec, s))
            snprintf(out[n++], SC_KEY_LEN, "%s", c->line[i].key);
    return n;
}
