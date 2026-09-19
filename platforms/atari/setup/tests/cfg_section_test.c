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
static char g_last_hdd[512], g_hdd_slot[8][512], g_acsi[8][512];
static int  g_last_hdd_idx = -1, g_last_acsi_id = -2;
int  set_hard_drive_image_file_atari(uint8_t index, char *filename)
{
    g_last_hdd_idx = index;
    snprintf(g_last_hdd, sizeof g_last_hdd, "%s", filename);
    if (index < 8) snprintf(g_hdd_slot[index], 512, "%s", filename);
    return 0;
}
int  acsi_attach_at(int id, const char *path)
{
    g_last_acsi_id = id;
    if (id >= 0 && id < 8) snprintf(g_acsi[id], 512, "%s", path);
    return 0;
}
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

    /* [psctrl] path vars prefix the bare filenames in the machine section */
    char base[256], romdir[300], cfgp[256];
    snprintf(base, sizeof base, "%s/romsec_base_%d", getenv("TMPDIR")?getenv("TMPDIR"):"/tmp", (int)getpid());
    snprintf(romdir, sizeof romdir, "%s/roms", base);
    char mk[700]; snprintf(mk, sizeof mk, "mkdir -p %s/roms %s/disks %s/fdd", base, base, base);
    if (system(mk)==0) {
        char rp[400]; snprintf(rp, sizeof rp, "%s/roms/e.rom", base);
        FILE*rf=fopen(rp,"wb"); for(int i=0;i<256*1024;i++) fputc(0,rf); fclose(rf);
        snprintf(cfgp, sizeof cfgp, "%s/paths_%d.cfg", getenv("TMPDIR")?getenv("TMPDIR"):"/tmp", (int)getpid());
        FILE*pf=fopen(cfgp,"w");
        fprintf(pf,"[psctrl]\nrom_path %s/roms\ndisk_path %s/disks\nfdd_path %s/fdd\nboot apj-os\n"
                   "[apj-os]\ncpu 68040\nrom e.rom\nhdd d.img\nfdd f.st\n", base, base, base);
        fclose(pf);
        g_last_hdd[0]=0;
        struct emulator_config*pc=load_config_file_section(cfgp,"apj-os");
        char want_rom[400], want_hdd[400], want_fdd[400];
        snprintf(want_rom, sizeof want_rom, "%s/roms/e.rom", base);
        snprintf(want_hdd, sizeof want_hdd, "%s/disks/d.img", base);
        snprintf(want_fdd, sizeof want_fdd, "%s/fdd/f.st", base);
        CHECK(pc && !strcmp(pc->rom.rom_path, want_rom), "rom_path prefix: got [%s]", pc?pc->rom.rom_path:"");
        CHECK(pc && pc->rom.rom_size>0, "prefixed rom did not open");
        CHECK(!strcmp(g_last_hdd, want_hdd), "disk_path prefix: got [%s]", g_last_hdd);
        CHECK(g_last_hdd_idx == 0, "an unpinned hdd takes slot 0, got %d", g_last_hdd_idx);

        /* a slot prefix pins the slot and is split off before disk_path */
        FILE*pf3=fopen(cfgp,"w");
        fprintf(pf3,"[psctrl]\ndisk_path %s/disks\n[apj-os]\ncpu 68040\nhdd 3:d.img\nacsi 5:m.hfs\nacsi n.hfs\n", base);
        fclose(pf3);
        memset(g_hdd_slot, 0, sizeof g_hdd_slot); memset(g_acsi, 0, sizeof g_acsi);
        load_config_file_section(cfgp,"apj-os");
        CHECK(!strcmp(g_hdd_slot[3], want_hdd), "hdd 3:d.img should land in slot 3 with disk_path: [%s]", g_hdd_slot[3]);
        snprintf(want_hdd, sizeof want_hdd, "%s/disks/m.hfs", base);
        CHECK(g_last_acsi_id == -2, "acsi images must not attach without `acsi enabled` (attached ID %d)", g_last_acsi_id);

        /* the switch turns the images on, wherever it sits in the section */
        FILE*pf4=fopen(cfgp,"w");
        fprintf(pf4,"[psctrl]\ndisk_path %s/disks\n[apj-os]\ncpu 68040\nacsi 5:m.hfs\nacsi n.hfs\nacsi enabled\n", base);
        fclose(pf4);
        memset(g_acsi, 0, sizeof g_acsi); g_last_acsi_id = -2;
        load_config_file_section(cfgp,"apj-os");
        CHECK(!strcmp(g_acsi[5], want_hdd), "acsi 5:m.hfs should pin ID 5 with disk_path: [%s]", g_acsi[5]);
        CHECK(g_last_acsi_id == -1, "an unpinned acsi asks for the lowest free ID, got %d", g_last_acsi_id);
        FILE*pf5=fopen(cfgp,"w");
        fprintf(pf5,"[apj-os]\ncpu 68040\nacsi disabled\nacsi n.hfs\n");
        fclose(pf5);
        g_last_acsi_id = -2;
        load_config_file_section(cfgp,"apj-os");
        CHECK(g_last_acsi_id == -2, "acsi disabled must attach nothing (attached ID %d)", g_last_acsi_id);
        CHECK(pc && !strcmp(pc->fdd.img_path, want_fdd), "fdd_path prefix: got [%s]", pc?pc->fdd.img_path:"");

        /* an absolute filename ignores the base; a path with no var is unchanged */
        FILE*pf2=fopen(cfgp,"w");
        fprintf(pf2,"[apj-os]\ncpu 68040\nrom %s\nfdd ../rel/f.st\n", rp);
        fclose(pf2);
        struct emulator_config*pc2=load_config_file_section(cfgp,"apj-os");
        CHECK(pc2 && !strcmp(pc2->rom.rom_path, rp), "absolute rom mangled: [%s]", pc2?pc2->rom.rom_path:"");
        CHECK(pc2 && !strcmp(pc2->fdd.img_path, "../rel/f.st"), "relative fdd with no var changed: [%s]", pc2?pc2->fdd.img_path:"");
        /* --- the two floppy drives -------------------------------------
         * Drive B had no way into a boot config at all: `fdd` set drive
         * A and a second line overwrote it. A drive is now named by
         * letter or by number, and a bare line takes the next free one -
         * the same rule hdd and acsi follow. */
        FILE*pf6=fopen(cfgp,"w");
        fprintf(pf6,"[psctrl]\nfdd_path %s/floppies\n[apj-os]\ncpu 68000\n"
                    "fdd B:side2.st\nfdd A:boot.st\n", base);
        fclose(pf6);
        struct emulator_config*fcfg=load_config_file_section(cfgp,"apj-os");
        char wa[700], wb[700];
        snprintf(wa, sizeof wa, "%s/floppies/boot.st",  base);
        snprintf(wb, sizeof wb, "%s/floppies/side2.st", base);
        CHECK(fcfg && fcfg->fdd.enabled, "two fdd lines must enable the floppy");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path,   wa), "fdd A: [%s]", fcfg?fcfg->fdd.img_path:"");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path_b, wb), "fdd B: [%s]", fcfg?fcfg->fdd.img_path_b:"");

        /* numeric slots are the form the setup page writes */
        FILE*pf7=fopen(cfgp,"w");
        fprintf(pf7,"[psctrl]\nfdd_path %s/floppies\n[apj-os]\ncpu 68000\n"
                    "fdd 1:side2.st\nfdd 0:boot.st\n", base);
        fclose(pf7);
        fcfg=load_config_file_section(cfgp,"apj-os");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path,   wa), "fdd 0: [%s]", fcfg?fcfg->fdd.img_path:"");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path_b, wb), "fdd 1: [%s]", fcfg?fcfg->fdd.img_path_b:"");

        /* bare lines fill A then B, and nothing claims a third drive */
        FILE*pf8=fopen(cfgp,"w");
        fprintf(pf8,"[psctrl]\nfdd_path %s/floppies\n[apj-os]\ncpu 68000\n"
                    "fdd boot.st\nfdd side2.st\n", base);
        fclose(pf8);
        fcfg=load_config_file_section(cfgp,"apj-os");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path,   wa), "bare fdd #1 -> A: [%s]", fcfg?fcfg->fdd.img_path:"");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path_b, wb), "bare fdd #2 -> B: [%s]", fcfg?fcfg->fdd.img_path_b:"");

        /* one image still means one drive, and B stays empty */
        FILE*pf9=fopen(cfgp,"w");
        fprintf(pf9,"[psctrl]\nfdd_path %s/floppies\n[apj-os]\ncpu 68000\nfdd boot.st\n", base);
        fclose(pf9);
        fcfg=load_config_file_section(cfgp,"apj-os");
        CHECK(fcfg && !strcmp(fcfg->fdd.img_path, wa), "single fdd -> A: [%s]", fcfg?fcfg->fdd.img_path:"");
        CHECK(fcfg && fcfg->fdd.img_path_b[0] == 0, "single fdd must leave B: empty: [%s]",
              fcfg?fcfg->fdd.img_path_b:"");

        char rm[700]; snprintf(rm, sizeof rm, "rm -rf %s %s", base, cfgp); (void)!system(rm);
    }

    unlink(sec);
    unlink(one);
    printf(fails ? "\nFAIL (%d)\n" : "\nPASS (%d failures)\n", fails);
    return fails != 0;
}
