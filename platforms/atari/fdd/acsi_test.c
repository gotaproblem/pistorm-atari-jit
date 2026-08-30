/*
 * acsi_test.c - host-side test of the emulated ACSI target THROUGH the
 * real dispatch: links the actual acsi.c AND the actual atari_fdd.c
 * routing, with the bus and ST-RAM stubbed. Drives the register-level
 * sequences a period driver (AHDI/ICD/Spectre) performs, so the CDB
 * handshake, IRQ pacing, DMA pointer arithmetic and status/sense
 * behaviour are all exercised on the same code the guest will hit.
 *
 * NOTE ataritest cannot test this subsystem: it talks to the REAL bus
 * from its own process; the emulated targets exist only inside the
 * emulator's dispatch. This standalone test is the harness instead.
 *
 * Build & run (any host):
 *   cc -DACSI_HOST_TEST -I. -o /tmp/acsi_test \
 *      platforms/atari/fdd/acsi_test.c platforms/atari/fdd/acsi.c \
 *      platforms/atari/fdd/atari_fdd.c && /tmp/acsi_test
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "atari_fdd.h"
#include "acsi.h"

/* ---- stubs: bus + ST-RAM ---------------------------------------------- */

static uint8_t ram[0x400000];
static long bus_hdc_writes;          /* HDC bytes that went to the "real bus" */

bool FDD_enabled = false;            /* ACSI-only configuration under test */

void ps_write_8(uint32_t a, uint16_t v)  { if (a < sizeof ram) ram[a] = (uint8_t)v;
                                           if (a == 0xFF8604u) bus_hdc_writes++; }
void ps_write_16(uint32_t a, uint16_t v) { if (a + 1 < sizeof ram) { ram[a] = v >> 8; ram[a+1] = (uint8_t)v; }
                                           if (a == 0xFF8604u) bus_hdc_writes++; }
void ps_write_32(uint32_t a, uint32_t v) { ps_write_16(a, v >> 16); ps_write_16(a + 2, (uint16_t)v); }
uint16_t ps_read_16(uint32_t a) { return a + 1 < sizeof ram ? (uint16_t)((ram[a] << 8) | ram[a+1]) : 0xFFFF; }
uint8_t  ps_read_8(uint32_t a)  { return a < sizeof ram ? ram[a] : 0xFF; }

void pistorm_dma_to_stram(uint32_t a, const uint8_t *b, uint32_t n)
{ if (a + n <= sizeof ram) memcpy(ram + a, b, n); }
void pistorm_dma_from_stram(uint32_t a, uint8_t *b, uint32_t n)
{ if (a + n <= sizeof ram) memcpy(b, ram + a, n); }
void stram_dma_write(uint32_t a, const uint8_t *b, unsigned n)
{ if (a + n <= sizeof ram) memcpy(ram + a, b, n); }

/* ---- checks ------------------------------------------------------------ */

static int checks, fails;
#define CHECK(c, msg) do { checks++; \
    if (!(c)) { fails++; printf("FAIL  %s\n", msg); } } while (0)

/* ---- driver-side helpers (the sequences AHDI performs) ------------------ */

static void dma_set_base(uint32_t a)
{
    fdd_io_write(DMA_BASE_LOW,  a & 0xFF, 1);
    fdd_io_write(DMA_BASE_MID,  (a >> 8) & 0xFF, 1);
    fdd_io_write(DMA_BASE_HIGH, (a >> 16) & 0xFF, 1);
}

static void dma_set_count(uint16_t n)
{
    fdd_io_write(DMA_MODE_REG, 0x098, 2);      /* HDC + SCREG          */
    fdd_io_write(FDC_DATA_REG, n, 2);
}

static int gpip_irq(void)                       /* 1 = GPIP5 asserted   */
{
    return (fdd_gpip(0xFF) & (1u << 5)) == 0;
}

/* send a full CDB; returns 0 on handshake failure */
static int send_cdb(const uint8_t *c, int n)
{
    fdd_io_write(DMA_MODE_REG, 0x088, 2);      /* HDC, A-bits low: 1st */
    fdd_io_write(FDC_DATA_REG, c[0], 2);
    for (int i = 1; i < n; i++) {
        if (!gpip_irq()) return 0;             /* target must request  */
        fdd_io_write(DMA_MODE_REG, 0x08A, 2);  /* HDC + A0: rest       */
        fdd_io_write(FDC_DATA_REG, c[i], 2);
    }
    return 1;
}

/* P.Putnik autoboot-loader pattern: every CDB byte is a LONG write to
 * $FF8604, so the data lands first and the mode for the NEXT byte lands
 * after - the mode change lags one byte, and this loader leaves $0088
 * (A1 LOW) in the first long, so CDB byte 1 arrives still looking like
 * a "new command". A real target keeps counting; so must we. */
static int send_cdb_pp(const uint8_t *c, int n)
{
    fdd_io_write(DMA_MODE_REG, 0x088, 2);          /* mode := first    */
    for (int i = 0; i < n; i++) {
        /* move.l #(data<<16)|nextmode,(a5) - decomposed as the bus does */
        fdd_io_write(FDC_DATA_REG, c[i], 2);       /* data, mode as-is  */
        if (i + 1 < n && !gpip_irq()) return 0;    /* per-byte IRQ      */
        /* the mode the loader puts in the low word: $0088 after the
         * FIRST byte (the bug trigger), $008A thereafter */
        fdd_io_write(DMA_MODE_REG, i == 0 ? 0x088 : 0x08A, 2);
    }
    return 1;
}

static uint8_t read_status(void)
{
    fdd_io_write(DMA_MODE_REG, 0x08A, 2);
    return (uint8_t)fdd_io_read(FDC_DATA_REG, 2);
}

static uint16_t read_count(void)
{
    fdd_io_write(DMA_MODE_REG, 0x098, 2);
    return (uint16_t)fdd_io_read(FDC_DATA_REG, 2);
}

static uint32_t dma_ptr(void)
{
    return ((fdd_io_read(DMA_BASE_HIGH, 1) & 0xFF) << 16) |
           ((fdd_io_read(DMA_BASE_MID, 1) & 0xFF) << 8) |
            (fdd_io_read(DMA_BASE_LOW, 1) & 0xFF);
}

/* ---- image scaffolding -------------------------------------------------- */

static const char *make_image(const char *path, uint32_t sectors, uint8_t seed)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    uint8_t sec[512];
    for (uint32_t s = 0; s < sectors; s++) {
        memset(sec, (uint8_t)(seed + s), sizeof sec);
        write(fd, sec, sizeof sec);
    }
    close(fd);
    return path;
}

int main(void)
{
    const char *img = make_image("/tmp/acsi_t.img", 2048, 0x10); /* 1MB  */
    const char *hfs = make_image("/tmp/acsi_t.hfs", 4096, 0x80); /* 2MB  */

    CHECK(acsi_attach(img) == 0, "attach .img at ID 0");
    CHECK(acsi_attach(hfs) == 1, "attach .hfs at ID 1");
    CHECK(acsi_enabled(), "subsystem enabled");
    fdd_init();

    /* ---- INQUIRY, ID 0 ------------------------------------------------ */
    {
        uint8_t c[6] = { 0x12, 0, 0, 0, 32, 0 };   /* ID 0 | INQUIRY     */
        dma_set_base(0x10000);
        dma_set_count(1);
        CHECK(send_cdb(c, 6), "INQUIRY: per-byte IRQ handshake");
        CHECK(gpip_irq(), "INQUIRY: completion IRQ");
        CHECK(read_status() == 0x00, "INQUIRY: good status");
        CHECK(!gpip_irq(), "status read clears IRQ");
        CHECK(memcmp(ram + 0x10008, "PiStorm ", 8) == 0,
              "INQUIRY data (vendor) landed at DMA base");
        CHECK(dma_ptr() == 0x10000 + 32, "DMA pointer advanced by 32");
        CHECK(read_count() == 0, "residual count 0");
    }

    /* ---- WRITE(6) / READ(6) round trip, ID 0 -------------------------- */
    {
        uint8_t c_wr[6] = { 0x0A, 0, 0, 5, 2, 0 }; /* write 2 blks @ 5   */
        uint8_t c_rd[6] = { 0x08, 0, 0, 5, 2, 0 };
        for (int i = 0; i < 1024; i++) ram[0x20000 + i] = (uint8_t)(i * 7);
        dma_set_base(0x20000);
        dma_set_count(2);
        CHECK(send_cdb(c_wr, 6), "WRITE6 handshake");
        CHECK(read_status() == 0x00, "WRITE6 good");
        memset(ram + 0x30000, 0, 1024);
        dma_set_base(0x30000);
        dma_set_count(2);
        CHECK(send_cdb(c_rd, 6), "READ6 handshake");
        CHECK(read_status() == 0x00, "READ6 good");
        CHECK(memcmp(ram + 0x20000, ram + 0x30000, 1024) == 0,
              "READ6 returns what WRITE6 stored");
        CHECK(dma_ptr() == 0x30000 + 1024, "DMA pointer advanced 2 sectors");
    }

    /* ---- .hfs wrapper, ID 1 ------------------------------------------- */
    {
        uint8_t c0[6] = { 0x28, 0, 0, 0, 1, 0 };   /* ID 1 | READ6 LBA 0 */
        c0[0] = (1 << 5) | 0x08;
        dma_set_base(0x40000);
        dma_set_count(1);
        CHECK(send_cdb(c0, 6), "hfs: READ LBA0 handshake");
        CHECK(read_status() == 0x00, "hfs: LBA0 good");
        uint8_t *r = ram + 0x40000;
        uint32_t hd_siz = (r[0x1C2] << 24) | (r[0x1C3] << 16) |
                          (r[0x1C4] << 8) | r[0x1C5];
        CHECK(hd_siz == 4097, "hfs root: hd_siz = file sectors + 1");
        CHECK(r[0x1C6] == 0x01 && r[0x1C7] == 'M' && r[0x1C8] == 'A' &&
              r[0x1C9] == 'C', "hfs root: MAC partition flagged");
        CHECK(r[0x1CD] == 1, "hfs root: partition starts at sector 1");

        uint8_t c1[6] = { (1 << 5) | 0x08, 0, 0, 1, 1, 0 };  /* LBA 1   */
        dma_set_base(0x41000);
        dma_set_count(1);
        CHECK(send_cdb(c1, 6), "hfs: READ LBA1 handshake");
        CHECK(read_status() == 0x00, "hfs: LBA1 good");
        CHECK(ram[0x41000] == 0x80, "hfs: LBA1 = file offset 0 (seed)");
    }

    /* ---- lagging-mode CDB (P.Putnik autoboot loader) ------------------ */
    {
        /* exactly what the loader on a PP autoboot disk issues:
         * READ(6) 11 blocks from LBA 2, every byte a long write */
        uint8_t c[6] = { 0x08, 0x00, 0x00, 0x02, 0x0B, 0x00 };
        memset(ram + 0x70000, 0, 11 * 512);
        dma_set_base(0x70000);
        dma_set_count(11);
        CHECK(send_cdb_pp(c, 6), "PP loader: lagging-mode CDB handshake");
        CHECK(gpip_irq(), "PP loader: completion IRQ");
        CHECK(read_status() == 0x00, "PP loader: good status");
        CHECK(read_count() == 0, "PP loader: residual 0");
        CHECK(dma_ptr() == 0x70000 + 11 * 512,
              "PP loader: DMA pointer advanced 11 sectors");
        /* the whole point: data must actually be there. Sector n of the
         * backing image is filled with byte (seed + n). */
        CHECK(ram[0x70000] == (uint8_t)(0x10 + 2) &&
              ram[0x70000 + 10 * 512] == (uint8_t)(0x10 + 12),
              "PP loader: 11 sectors from LBA 2 really landed in RAM");
    }

    /* ---- truncated CDB must NOT report the previous good status ------- */
    {
        uint8_t ok[6] = { 0x12, 0, 0, 0, 32, 0 };      /* INQUIRY: good  */
        dma_set_base(0x80000);
        dma_set_count(1);
        CHECK(send_cdb(ok, 6), "stale-status: priming command handshake");
        CHECK(read_status() == 0x00, "stale-status: priming command good");

        /* now start a command and stop half way through the CDB */
        fdd_io_write(DMA_MODE_REG, 0x088, 2);
        fdd_io_write(FDC_DATA_REG, 0x08, 2);           /* opcode only    */
        fdd_io_write(DMA_MODE_REG, 0x08A, 2);
        fdd_io_write(FDC_DATA_REG, 0x00, 2);
        CHECK(read_status() == 0x02,
              "stale-status: incomplete CDB reports CHECK CONDITION");
    }

    /* ---- REPORT LUNS via the ICD wrapper (HDDRIVER probes with it) ---- */
    {
        /* $1F escape, then a 12-byte CDB: A0 .. alloc=16 .. */
        uint8_t c[13] = { 0x1F, 0xA0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0 };
        memset(ram + 0x60000, 0xAA, 32);
        dma_set_base(0x60000);
        dma_set_count(1);
        CHECK(send_cdb(c, 13), "REPORT LUNS handshake (12-byte ICD CDB)");
        CHECK(read_status() == 0x00, "REPORT LUNS: good status");
        CHECK(ram[0x60000] == 0 && ram[0x60001] == 0 &&
              ram[0x60002] == 0 && ram[0x60003] == 8,
              "REPORT LUNS: list length 8 (one LUN)");
        for (int i = 8; i < 16; i++)
            CHECK(ram[0x60000 + i] == 0, "REPORT LUNS: LUN 0 entry is zero");
    }

    /* ---- error path: bad LBA then REQUEST SENSE ----------------------- */
    {
        uint8_t bad[6] = { 0x08, 0x1F, 0xFF, 0xFF, 1, 0 };  /* LBA 2M   */
        dma_set_base(0x50000);
        dma_set_count(1);
        CHECK(send_cdb(bad, 6), "bad READ handshake");
        CHECK(read_status() == 0x02, "bad READ: check condition");
        uint8_t rs[6] = { 0x03, 0, 0, 0, 4, 0 };
        dma_set_base(0x50000);
        CHECK(send_cdb(rs, 6), "REQUEST SENSE handshake");
        CHECK(read_status() == 0x00, "REQUEST SENSE good");
        CHECK(ram[0x50000] == 0x21, "sense code: invalid block address");
    }

    /* ---- ICD wrapper: READ CAPACITY(10) ------------------------------- */
    {
        uint8_t c[11] = { 0x1F, 0x25, 0,0,0,0,0,0,0,0,0 };
        dma_set_base(0x60000);
        CHECK(send_cdb(c, 11), "ICD READ CAPACITY handshake");
        CHECK(read_status() == 0x00, "ICD READ CAPACITY good");
        uint32_t last = (ram[0x60000] << 24) | (ram[0x60001] << 16) |
                        (ram[0x60002] << 8) | ram[0x60003];
        CHECK(last == 2047, "capacity: last LBA of the 1MB image");
        CHECK(ram[0x60006] == 2 && ram[0x60007] == 0, "block size 512");
    }

    /* ---- LOADER-PATTERN STRESS: long chained sequential reads ---------
     * Spectre/AHDI load big files as a CHAIN of multi-sector READ(6)
     * commands with varying counts and a moving DMA base. The original
     * harness only did 2-sector transfers; an addressing/ordering bug
     * that only shows at scale would corrupt a loaded program image and
     * present as "guest jumps into garbage" (field suspicion:
     * macimage.fil). Checksum every byte against the backing file. */
    {
        static const int chain[] = { 1, 2, 7, 16, 32, 255, 64, 3, 128, 9 };
        uint32_t lba = 100;
        uint32_t base = 0x08000;
        int ok = 1;
        for (unsigned ci = 0; ci < sizeof chain / sizeof chain[0]; ci++) {
            uint32_t cnt = (uint32_t)chain[ci];
            uint8_t c_rd[6] = { 0x08,
                (uint8_t)((lba >> 16) & 0x1F), (uint8_t)(lba >> 8),
                (uint8_t)lba, (uint8_t)(cnt == 256 ? 0 : cnt), 0 };
            dma_set_base(base);
            dma_set_count((uint16_t)cnt);
            if (!send_cdb(c_rd, 6)) { ok = 0; printf("chain[%u]: handshake fail\n", ci); break; }
            if (read_status() != 0) { ok = 0; printf("chain[%u]: bad status\n", ci); break; }
            if (dma_ptr() != base + cnt * 512) {
                ok = 0;
                printf("chain[%u]: DMA ptr %06X expected %06X\n",
                       ci, dma_ptr(), base + cnt * 512);
                break;
            }
            /* verify against the raw image file */
            {
                FILE *f = fopen("/tmp/acsi_t.img", "rb");
                static uint8_t fbuf[256 * 512];
                fseek(f, (long)lba * 512, SEEK_SET);
                fread(fbuf, 1, cnt * 512, f);
                fclose(f);
                if (memcmp(ram + base, fbuf, cnt * 512) != 0) {
                    ok = 0;
                    for (uint32_t i = 0; i < cnt * 512; i++)
                        if (ram[base + i] != fbuf[i]) {
                            printf("chain[%u]: first mismatch at byte %u "
                                   "(lba %u) got %02X want %02X\n",
                                   ci, i, lba + i / 512, ram[base + i], fbuf[i]);
                            break;
                        }
                    break;
                }
            }
            lba += cnt;
            base += cnt * 512;
        }
        CHECK(ok, "loader-pattern chained reads: every byte matches the image");
        CHECK(lba == 100 + 1+2+7+16+32+255+64+3+128+9,
              "loader-pattern chain completed all links");
    }

    /* ---- non-emulated ID passes to the real bus ----------------------- */
    {
        long before = bus_hdc_writes;
        fdd_io_write(DMA_MODE_REG, 0x088, 2);
        fdd_io_write(FDC_DATA_REG, (5 << 5) | 0x00, 2);   /* ID 5: TUR  */
        CHECK(bus_hdc_writes > before,
              "ID 5 command byte forwarded to the real bus");
        CHECK(!acsi_owns_dma(), "emulated side released ownership");
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
