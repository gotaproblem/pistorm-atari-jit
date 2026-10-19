/*
 * native_probe.c - see native_probe.h.
 *
 * Every register test is one byte read with a supervisor function code
 * through the quiet accessor, which reports the bus error to us and
 * nowhere else. A chip that is fitted answers; an address nothing
 * decodes gets BERR from the GLUE, exactly the test TOS itself makes at
 * boot. Everything probed is printed, so a machine that comes out wrong
 * can be read off the log rather than guessed at.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "native_probe.h"
#include "shifter_setup.h"
#include "setup_cfg.h"
#include "gpio/ps_protocol.h"

extern uint8_t fc;                        /* gpio/ps_protocol.c */
extern volatile uint8_t g_buserr;         /* the CPU thread's sticky flag */
uint8_t ps_read_8_quiet(uint32_t addr, uint8_t fc_value, uint8_t *berr_out);

#define REG_MEMCFG   0x00FF8001u
#define REG_HSCROLL  0x00FF8265u   /* STE shifter                       */
#define REG_DMASND   0x00FF8901u   /* STE DMA sound control             */
#define REG_BLITTER  0x00FF8A00u   /* BLiTTER halftone RAM              */
#define REG_MSTE     0x00FF8E21u   /* Mega STE cache/speed control      */
#define REG_MEGARTC  0x00FFFC21u   /* MC146818 RTC (Mega ST, Mega STE)  */
#define REG_TTSHIFT  0x00FF8400u   /* TT palette                        */

#define FC_SUPER_DATA 0x5

/* 1 if a byte read at `a` completes without a bus error */
static int answers(uint32_t a)
{
    uint8_t berr = 0;
    (void)ps_read_8_quiet(a, FC_SUPER_DATA, &berr);
    return !berr;
}

static uint8_t bank_bits(uint32_t size)
{
    if (size >= (2048u << 10)) return 2;
    if (size >= (512u << 10))  return 1;
    return 0;
}

/* A TOS header: bra.s at +0, version at +2, os_beg (its own base) at +8. */
static int tos_header_at(uint32_t base, uint16_t *ver)
{
    uint8_t berr = 0;
    uint16_t w0 = ps_read_16_fc(base, FC_SUPER_DATA, &berr);
    if (berr || (w0 & 0xFF00) != 0x6000)
        return 0;
    uint16_t v  = ps_read_16_fc(base + 2, FC_SUPER_DATA, &berr);
    uint32_t os_beg = ((uint32_t)ps_read_16_fc(base + 8, FC_SUPER_DATA, &berr) << 16) |
                       ps_read_16_fc(base + 10, FC_SUPER_DATA, &berr);
    if (berr || os_beg != base)
        return 0;
    *ver = v;
    return 1;
}

int np_probe(struct np_result *r)
{
    memset(r, 0, sizeof *r);
    uint8_t old_fc = fc;
    fc = FC_SUPER_DATA;                    /* the GLUE ignores FC 0 */

    ps_write_8(REG_MEMCFG, 0x0A);          /* 2M|2M while probing */
    r->bank0 = ss_probe_bank(0x000000u);
    r->bank1 = ss_probe_bank(0x200000u);
    if (r->bank0)
        ps_write_8(REG_MEMCFG, (uint8_t)((bank_bits(r->bank0) << 2) |
                                         bank_bits(r->bank1)));
    printf("[NATIVE] ST-RAM: bank0 %uK bank1 %uK\n",
           r->bank0 >> 10, r->bank1 >> 10);
    if (!r->bank0) {
        fc = old_fc;
        g_buserr = 0;
        printf("[NATIVE] bank 0 did not answer - no machine to describe\n");
        return -1;
    }

    r->ste_shifter = answers(REG_HSCROLL);
    r->dma_sound   = answers(REG_DMASND);
    r->blitter     = answers(REG_BLITTER);
    r->mste_ctrl   = answers(REG_MSTE);
    r->mega_rtc    = answers(REG_MEGARTC);
    r->tt_shifter  = answers(REG_TTSHIFT);
    printf("[NATIVE] chips: $FF8265 STE-shifter %s, $FF8901 DMA-sound %s, "
           "$FF8A00 blitter %s, $FF8E21 MSTE-ctrl %s, $FFFC21 RTC %s, "
           "$FF8400 TT-shifter %s\n",
           r->ste_shifter ? "yes" : "no", r->dma_sound ? "yes" : "no",
           r->blitter ? "yes" : "no", r->mste_ctrl ? "yes" : "no",
           r->mega_rtc ? "yes" : "no", r->tt_shifter ? "yes" : "no");

    /* `machine` takes st / ste / megast (falcon is an experiment, never a
     * probe result). STE = the DMA sound chip answers ($FF8901), the test
     * TOS makes: a plain ST with TOS 2.06 answered $FF8265 (the GLUE
     * decodes the whole shifter page), so the scroll register says
     * nothing and is only logged. A Mega STE is an STE to the emulator; a
     * TT would be too, and is logged so it is not mistaken for one. */
    r->ste_shifter = r->dma_sound;
    if (r->dma_sound)
        strcpy(r->machine, "ste");
    else if (r->blitter && r->mega_rtc)
        strcpy(r->machine, "megast");
    else
        strcpy(r->machine, "st");

    /* the ROM: TOS 2/3 at $E00000 first (a TOS 1.x machine bus-errors
     * there), then TOS 1.x at $FC0000 */
    uint16_t ver = 0;
    if (tos_header_at(0x00E00000u, &ver)) {
        r->rom_base = 0x00E00000u;
        r->rom_size = (ver >= 0x0300) ? (512u << 10) : (256u << 10);
    } else if (tos_header_at(0x00FC0000u, &ver)) {
        r->rom_base = 0x00FC0000u;
        r->rom_size = 192u << 10;
    }
    r->tos_version = ver;
    if (r->rom_base)
        printf("[NATIVE] ROM: TOS %X.%02X at $%06X, %uK\n",
               ver >> 8, ver & 0xFF, r->rom_base, r->rom_size >> 10);
    else
        printf("[NATIVE] ROM: no TOS header at $E00000 or $FC0000\n");

    fc = old_fc;
    g_buserr = 0;                          /* nothing latched for the guest */
    return 0;
}

int np_copy_rom(const struct np_result *r, const char *path)
{
    if (!r->rom_base || !r->rom_size)
        return -1;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[NATIVE] cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    uint8_t old_fc = fc;
    fc = FC_SUPER_DATA;
    static uint8_t buf[512u << 10];
    for (uint32_t off = 0; off < r->rom_size; off += 2) {
        uint16_t w = ps_read_16(r->rom_base + off);
        buf[off]     = (uint8_t)(w >> 8);
        buf[off + 1] = (uint8_t)w;
    }
    /* read it twice: a word that differs between passes is a bus that
     * is not giving us the ROM reliably, and a bad image is worse than
     * none */
    uint32_t bad = 0;
    for (uint32_t off = 0; off < r->rom_size; off += 2) {
        uint16_t w = ps_read_16(r->rom_base + off);
        if (buf[off] != (uint8_t)(w >> 8) || buf[off + 1] != (uint8_t)w)
            bad++;
    }
    fc = old_fc;
    g_buserr = 0;
    if (bad) {
        fprintf(stderr, "[NATIVE] ROM read unstable: %u words differed "
                "between two passes - not saving\n", bad);
        fclose(f);
        unlink(tmp);
        return -1;
    }
    int ok = fwrite(buf, 1, r->rom_size, f) == r->rom_size;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        fprintf(stderr, "[NATIVE] cannot save %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    /* like sc_save: owned by whoever owns the directory, not root */
    char dir[600];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        struct stat st;
        if (stat(dir, &st) == 0)
            (void)chown(path, st.st_uid, st.st_gid);
    }
    printf("[NATIVE] ROM saved to %s (%uK)\n", path, r->rom_size >> 10);
    return 0;
}

/* ~ or ~/ to the invoking user's home, as config_file.c and sc_locate do */
static void expand_home(const char *in, char *out, size_t n)
{
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) {
        const char *user = getenv("SUDO_USER");
        const struct passwd *pw = user && *user ? getpwnam(user) : NULL;
        if (!pw)
            pw = getpwuid(getuid());
        snprintf(out, n, "%s%s", pw && pw->pw_dir ? pw->pw_dir : "", in + 1);
    } else {
        snprintf(out, n, "%s", in);
    }
}

static void size_text(uint32_t bytes, char *out, size_t n)
{
    if (bytes >= (1024u << 10) && (bytes & ((1024u << 10) - 1)) == 0)
        snprintf(out, n, "%uM", bytes >> 20);
    else
        snprintf(out, n, "%uK", bytes >> 10);
}

int np_ensure_native_section(const char *cfg_path)
{
    static struct sc_cfg c;
    if (sc_load(&c, cfg_path) != 0)
        return 0;
    char secs[8][SC_SEC_LEN];
    int n = sc_sections(&c, secs, 8);
    if (n <= 0)
        return 0;                          /* a flat .cfg: no builds */
    for (int i = 0; i < n; i++)
        if (!strcasecmp(secs[i], "native"))
            return 0;                      /* already there: leave it */
    if (n >= 8) {
        printf("[NATIVE] psctrl.cfg already has 8 builds - not adding [native]\n");
        return 0;
    }

    struct np_result r;
    if (np_probe(&r) != 0)
        return 0;

    /* the ROM goes under rom_path, as `rom native-tos.rom` */
    char rom_file[600] = "";
    if (r.rom_base) {
        const char *rp = sc_get(&c, "psctrl", "rom_path");
        char dir[560];
        expand_home(rp && *rp ? rp : "../roms", dir, sizeof dir);
        snprintf(rom_file, sizeof rom_file, "%s/native-tos.rom", dir);
        if (np_copy_rom(&r, rom_file) != 0)
            rom_file[0] = 0;
    }

    char stram[16];
    size_text(r.bank0 + r.bank1, stram, sizeof stram);

    if (sc_add_section(&c, "native") != 0 ||
        sc_set(&c, "native", "cpu", "68000") != 0 ||
        sc_set(&c, "native", "machine", r.machine) != 0 ||
        sc_set(&c, "native", "stram_size", stram) != 0 ||
        sc_set(&c, "native", "shifter", r.ste_shifter ? "ste" : "st") != 0 ||
        sc_set(&c, "native", "blitter", r.blitter ? "real" : "disabled") != 0 ||
        (rom_file[0] && sc_set(&c, "native", "rom", "native-tos.rom") != 0)) {
        fprintf(stderr, "[NATIVE] psctrl.cfg is full - [native] not added\n");
        return -1;
    }
    /* the first boot is the real machine: [psctrl] boot native, so the
     * page's countdown (and --no-setup) boots it until something else is
     * picked */
    if (sc_set(&c, "psctrl", "boot", "native") != 0) {
        fprintf(stderr, "[NATIVE] psctrl.cfg is full - boot not set\n");
        return -1;
    }
    if (sc_save(&c, cfg_path) != 0) {
        fprintf(stderr, "[NATIVE] cannot save %s: %s\n", cfg_path, sc_save_error(&c));
        return -1;
    }
    printf("[NATIVE] boot native (first start)\n");
    printf("[NATIVE] added [native]: cpu 68000, machine %s, stram_size %s, "
           "shifter %s, blitter %s, rom %s\n",
           r.machine, stram, r.ste_shifter ? "ste" : "st",
           r.blitter ? "real" : "disabled",
           rom_file[0] ? "native-tos.rom" : "(none - no ROM found)");
    return 1;
}
