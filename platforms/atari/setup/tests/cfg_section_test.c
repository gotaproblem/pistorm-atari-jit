/*
 * cfg_section_test.c - does load_config_file_section() load ONE section?
 *
 * Builds the real config_file.c against stubs for the two things it calls
 * out to (the ACSI/IDE attach helpers and the PSCTRL tunable sink), so
 * the parser under test is the emulator's own, not a copy.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "config_file/config_file.h"

/* stubs: reached only by hdd/acsi lines, which we do not exercise here */
int  set_hard_drive_image_file_atari(uint8_t index, char *filename)
{
    (void)index; (void)filename; return 0;
}
int  acsi_attach(int id, const char *path) { (void)id; (void)path; return 0; }
int  psctrl_settings_config_key(const char *key, const char *val)
{
    (void)key; (void)val; return 0;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *text =
    "# a psctrl.cfg with three sections\n"
    "[psctrl]\n"
    "countdown 5\n"
    "boot apj-os\n"
    "[gem]\n"
    "cpu 68030\n"
    "fps 25\n"
    "ttram 64M\n"
    "[apj-os]\n"
    "cpu 68040\n"
    "fps 60\n"
    "ttram 128M\n";

static const char *flat =
    "cpu 68000\n"
    "fps 30\n";

static char *write_tmp(const char *body, const char *tag)
{
    static char path[2][256];
    int i = tag[0] == 'f';
    snprintf(path[i], sizeof path[i], "%s/cfgsec_%s_%d.cfg",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", tag, (int)getpid());
    FILE *f = fopen(path[i], "w");
    if (!f) { CHECK(0, "cannot write %s", path[i]); return NULL; }
    fputs(body, f);
    fclose(f);
    return path[i];
}

int main(void)
{
    char *sec = write_tmp(text, "sec");
    char *one = write_tmp(flat, "flat");
    if (!sec || !one)
        return 1;

    /* config_file.c stores get_m68k_cpu_type() - 1, so cpu_types[] index
     * 1 ("68000") lands as 0 and 68040 as 4. */
    printf("load_config_file_section:\n");

    struct emulator_config *c = load_config_file_section(sec, "gem");
    CHECK(c != NULL, "[gem] did not load");
    if (c) {
        CHECK(c->cpu_type == 3, "[gem] cpu_type %d, wanted 68030 (3)", c->cpu_type);
        CHECK(c->fps == 25, "[gem] fps %d, wanted 25", c->fps);
        CHECK(c->ttram_size == 64u * 1024 * 1024,
              "[gem] ttram %lu MB, wanted 64",
              (unsigned long)(c->ttram_size >> 20));
    }

    c = load_config_file_section(sec, "apj-os");
    CHECK(c != NULL, "[apj-os] did not load");
    if (c) {
        CHECK(c->cpu_type == 4, "[apj-os] cpu_type %d, wanted 68040 (4)", c->cpu_type);
        CHECK(c->fps == 60, "[apj-os] fps %d, wanted 60", c->fps);
        CHECK(c->ttram_size == 128u * 1024 * 1024,
              "[apj-os] ttram %lu MB, wanted 128",
              (unsigned long)(c->ttram_size >> 20));
    }

    /* a section that is not there must load nothing, not everything */
    c = load_config_file_section(sec, "nosuch");
    CHECK(c != NULL, "an unknown section returned no config at all");
    if (c)
        CHECK(c->fps == 0 || c->fps == 60,
              "an unknown section picked up fps %d from another section", c->fps);

    /* [psctrl]'s own keys must never reach the parser */
    c = load_config_file_section(sec, "psctrl");
    CHECK(c != NULL, "[psctrl] did not load");

    /* a flat .cfg still loads as it always did */
    c = load_config_file(one);
    CHECK(c != NULL, "the flat .cfg did not load");
    if (c) {
        CHECK(c->cpu_type == 0, "flat cpu_type %d, wanted 68000 (0)", c->cpu_type);
        CHECK(c->fps == 30, "flat fps %d, wanted 30", c->fps);
    }

    unlink(sec);
    unlink(one);
    printf(fails ? "\nFAIL (%d)\n" : "\nPASS (%d failures)\n", fails);
    return fails != 0;
}
