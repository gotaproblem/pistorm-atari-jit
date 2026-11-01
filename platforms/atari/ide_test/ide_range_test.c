/* ide_range_test.c - host test for the IDE model's end-of-image refusal
 * (idedriver.c beyond_end). Builds the model on its own, attaches a
 * 64-sector image and drives it through the register interface:
 *   - reads/writes inside the image work and carry data
 *   - a read or write that starts past the end, or runs off it, is refused
 *     at once (ERR, error ID-not-found, no DRQ) and the image does not grow
 * Run from the repo root:  sh platforms/atari/ide_test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "platforms/atari/idedriver.h"

volatile int pst_dbg_ide = 0;
int emulator_falcon_tos(void) { return 0; }

#define NSECT 16384                     /* attach wants > 4 MB */
static int fails;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static void setup(struct ide_controller *c, unsigned lba, unsigned count, unsigned cmd)
{
    IDE_write8(c, IDE_sec_count, count);
    IDE_write8(c, IDE_lba_low, lba & 0xFF);
    IDE_write8(c, IDE_lba_mid, (lba >> 8) & 0xFF);
    IDE_write8(c, IDE_lba_hi, (lba >> 16) & 0xFF);
    IDE_write8(c, IDE_lba_top, 0xE0 | ((lba >> 24) & 0x0F));
    IDE_write8(c, IDE_command_w, cmd);
}

int main(void)
{
    const char *img = "/tmp/ide_range_test.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    unsigned char sec[512];
    for (int n = 0; n < NSECT; n++) {
        memset(sec, n & 0xFF, sizeof sec);
        if (write(fd, sec, 512) != 512) return 2;
    }
    struct ide_controller *c = IDE_allocate("test");
    if (ide_attach_hdf(c, 0, fd) != 0) { printf("attach failed\n"); return 2; }
    IDE_reset_begin(c);

    /* inside: last sector */
    setup(c, NSECT - 1, 1, 0x20);
    unsigned st = IDE_read8(c, IDE_status_r);
    unsigned w = IDE_read16(c, IDE_data);
    check((st & 0x09) == 0x08 && (w & 0xFF) == ((NSECT - 1) & 0xFF),
          "read of the last sector: DRQ, no ERR, its data");
    for (int i = 1; i < 256; i++) IDE_read16(c, IDE_data);

    /* past the end */
    setup(c, NSECT, 1, 0x20);
    st = IDE_read8(c, IDE_status_r);
    unsigned er = IDE_read8(c, IDE_error_r);
    check((st & 0x09) == 0x01 && (er & 0x10), "read past the end: ERR + IDNF, no DRQ");

    /* starts inside, runs off the end */
    setup(c, NSECT - 2, 4, 0x20);
    st = IDE_read8(c, IDE_status_r);
    check((st & 0x09) == 0x01, "read running off the end: refused up front");

    /* write past the end: refused, image does not grow */
    struct stat s0, s1;
    fstat(fd, &s0);
    setup(c, NSECT + 3, 1, 0x30);
    st = IDE_read8(c, IDE_status_r);
    for (int i = 0; i < 256; i++) IDE_write16(c, IDE_data, 0xAAAA);
    fstat(fd, &s1);
    check((st & 0x09) == 0x01, "write past the end: ERR, no DRQ");
    check(s1.st_size == s0.st_size, "write past the end: image did not grow");

    /* write inside still works */
    setup(c, 5, 1, 0x30);
    st = IDE_read8(c, IDE_status_r);
    for (int i = 0; i < 256; i++) IDE_write16(c, IDE_data, 0x5A5A);
    pread(fd, sec, 512, 5 * 512);
    check((st & 0x09) == 0x08 && sec[0] == 0x5A && sec[511] == 0x5A,
          "write inside the image: DRQ, data lands");

    unlink(img);
    printf("%s (%d failures)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails != 0;
}
