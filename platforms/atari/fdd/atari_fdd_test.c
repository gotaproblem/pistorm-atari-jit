/*
 * atari_fdd_test.c - Standalone test for the Atari ST FDD emulator
 *
 * Creates a minimal 720KB .st image, then drives the FDC through the same
 * register sequence EmuTOS uses in floppy.c to verify read/write/seek.
 *
 * Build: gcc -O2 -pthread -Wall -o fdd_test atari_fdd.c atari_fdd_test.c
 * Run:   ./fdd_test [optional_disk.st]
 */

#include "atari_fdd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

/* =========================================================================
 * Minimal 16MB RAM buffer (stands in for pistorm's mmap'd Atari RAM)
 * ========================================================================= */
#define TEST_RAM_SIZE (16 * 1024 * 1024)
static uint8_t test_ram[TEST_RAM_SIZE];

/* =========================================================================
 * Register access helpers - mirror what EmuTOS assembler does
 * ========================================================================= */

static void write_psg(uint8_t reg, uint8_t val)
{
    fdd_io_write(PSG_REG_SELECT, reg, 1);
    fdd_io_write(PSG_REG_WRITE,  val, 1);
}

/* Select DMA FDC register index and write a value */
static void fdc_select_reg(int reg_idx)
{
    /* Set A1:A0 in bits [2:1] of DMA mode, clear SCREG, set R/W for read */
    uint16_t mode = (uint16_t)((reg_idx & 3) << 1) | DMA_MODE_RW;
    fdd_io_write(DMA_MODE_REG, mode, 2);
}

static void fdc_write_reg(int reg_idx, uint8_t val)
{
    /* A1:A0 in bits [2:1], SCREG=0, R/W=0 (write to disk direction) */
    uint16_t mode = (uint16_t)((reg_idx & 3) << 1);
    fdd_io_write(DMA_MODE_REG, mode, 2);
    fdd_io_write(FDC_DATA_REG, val, 2);
}

static uint8_t fdc_read_reg(int reg_idx)
{
    fdc_select_reg(reg_idx);
    return (uint8_t)(fdd_io_read(FDC_DATA_REG, 2) & 0xFF);
}

/* Set DMA sector count - must write with SCREG mode bit set */

/* Set DMA base address */
static void dma_set_addr(uint32_t addr)
{
    fdd_io_write(DMA_BASE_HIGH, (addr >> 16) & 0x3F, 2);
    fdd_io_write(DMA_BASE_MID,  (addr >>  8) & 0xFF, 2);
    fdd_io_write(DMA_BASE_LOW,  (addr      ) & 0xFF, 2);
}

/* Poll until FDC not busy (simulates MFP GPIP polling in EmuTOS) */
static int wait_fdc_done(int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10; i++) {
        uint8_t st = fdc_read_reg(FDC_REG_CMD_STATUS);
        if (!(st & FDC_STATUS_BUSY)) return 0;
        usleep(100);
    }
    fprintf(stderr, "[TEST] FDC timeout!\n");
    return -1;
}

/* Issue a Type I command (seek/restore) */
static int fdc_type1(uint8_t cmd, uint8_t data)
{
    fdc_write_reg(FDC_REG_DATA, data);
    fdc_write_reg(FDC_REG_CMD_STATUS, cmd);
    return wait_fdc_done(500);
}

/* =========================================================================
 * Full emulated BIOS read sequence (matches EmuTOS flopio)
 * ========================================================================= */
static int bios_read_sectors(int track, int side, int sector,
                              int count, uint32_t ram_addr)
{
    /* 1. Select drive A, set side */
    uint8_t porta = 0xFF;
    porta &= ~PSG_DRIVE0_SEL;                /* drive A selected */
    if (side == 0) porta |= PSG_SIDE_SEL;    /* bit2=1 -> side 0 */
    else           porta &= ~PSG_SIDE_SEL;   /* bit2=0 -> side 1 */
    write_psg(PSG_PORT_A, porta);

    /* 2. Seek to track */
    fdc_type1(FDC_CMD_SEEK, (uint8_t)track);

    /* 3. Set DMA base address */
    dma_set_addr(ram_addr);

    /* 4. Set sector count and direction (read from disk) */
    /* Clear DMA status by toggling R/W */
    uint16_t rmode = DMA_MODE_RW | (FDC_REG_CMD_STATUS << 1);
    fdd_io_write(DMA_MODE_REG, rmode ^ DMA_MODE_RW, 2);
    fdd_io_write(DMA_MODE_REG, rmode, 2);

    /* Set sector count with SCREG bit */
    fdd_io_write(DMA_MODE_REG, DMA_MODE_SCREG | DMA_MODE_RW, 2);
    fdd_io_write(FDC_DATA_REG, (uint32_t)count, 2);

    /* 5. Switch to FDC command register, set sector, issue read command */
    fdd_io_write(DMA_MODE_REG, rmode, 2);                 /* FDC reg 0 mode, R/W=read */
    fdc_write_reg(FDC_REG_SECTOR, (uint8_t)sector);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC_M);

    /* 6. Wait for completion */
    if (wait_fdc_done(2000) != 0) return -1;

    /* 7. Check DMA error status */
    uint16_t dstat = (uint16_t)fdd_io_read(DMA_MODE_REG, 2);
    if (!(dstat & DMA_STATUS_OK)) {
        fprintf(stderr, "[TEST] DMA error on read (dstat=0x%04X)\n", dstat);
        return -1;
    }
    return 0;
}

static int bios_write_sectors(int track, int side, int sector,
                               int count, uint32_t ram_addr)
{
    uint8_t porta = 0xFF;
    porta &= ~PSG_DRIVE0_SEL;
    if (side == 0) porta |= PSG_SIDE_SEL;
    else           porta &= ~PSG_SIDE_SEL;
    write_psg(PSG_PORT_A, porta);

    fdc_type1(FDC_CMD_SEEK, (uint8_t)track);
    dma_set_addr(ram_addr);

    /* Direction = write to disk: DMA_MODE_RW = 0 */
    uint16_t wmode = (FDC_REG_CMD_STATUS << 1);  /* R/W=0 = write */
    fdd_io_write(DMA_MODE_REG, wmode ^ DMA_MODE_RW, 2);
    fdd_io_write(DMA_MODE_REG, wmode, 2);
    fdd_io_write(DMA_MODE_REG, DMA_MODE_SCREG, 2);
    fdd_io_write(FDC_DATA_REG, (uint32_t)count, 2);
    fdd_io_write(DMA_MODE_REG, wmode, 2);

    fdc_write_reg(FDC_REG_SECTOR, (uint8_t)sector);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_WRITE_SEC_M);

    if (wait_fdc_done(2000) != 0) return -1;

    uint16_t dstat = (uint16_t)fdd_io_read(DMA_MODE_REG, 2);
    if (!(dstat & DMA_STATUS_OK)) {
        fprintf(stderr, "[TEST] DMA error on write (dstat=0x%04X)\n", dstat);
        return -1;
    }
    return 0;
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, expr) do { \
    if (expr) { printf("  PASS: %s\n", name); tests_passed++; } \
    else       { printf("  FAIL: %s\n", name); tests_failed++; } \
} while(0)

static const char *TMP_IMAGE = "/tmp/test_floppy.st";

static void create_test_image(void)
{
    /* Create a minimal 720KB DD image filled with a known pattern */
    FILE *f = fopen(TMP_IMAGE, "wb");
    assert(f);
    uint8_t sector[512];
    int sectors = 80 * 2 * 9;  /* 80 tracks, 2 sides, 9 sectors */
    for (int i = 0; i < sectors; i++) {
        memset(sector, (uint8_t)(i & 0xFF), sizeof(sector));
        /* First sector: put a recognisable bootsector marker */
        if (i == 0) {
            sector[0] = 0xEB;  /* JMP short (like real boot sector) */
            sector[1] = 0x60;
            sector[2] = 0x90;
            memcpy(sector + 3, "PISTORM", 7);
        }
        fwrite(sector, 1, sizeof(sector), f);
    }
    fclose(f);
    printf("[TEST] Created test image %s (%d KB)\n", TMP_IMAGE, sectors / 2);
}

static void test_basic_seek(void)
{
    printf("\n--- Test: Type I Seek / Restore ---\n");
    write_psg(PSG_PORT_A, (uint8_t)(~PSG_DRIVE0_SEL | PSG_SIDE_SEL) & 0xFF);

    /* Restore to track 0 */
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_RESTORE);
    wait_fdc_done(500);
    uint8_t tr = fdc_read_reg(FDC_REG_TRACK);
    TEST("Restore sets track reg to 0", tr == 0);

    uint8_t st = fdc_read_reg(FDC_REG_CMD_STATUS);
    TEST("Restore sets TRACK0 bit", st & FDC_STATUS_TRACK0);

    /* Seek to track 39 */
    fdc_write_reg(FDC_REG_DATA, 39);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_SEEK);
    wait_fdc_done(500);
    tr = fdc_read_reg(FDC_REG_TRACK);
    TEST("Seek track 39 correct", tr == 39);

    /* Seek back to 0 */
    fdc_write_reg(FDC_REG_DATA, 0);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_SEEK);
    wait_fdc_done(500);
    tr = fdc_read_reg(FDC_REG_TRACK);
    TEST("Seek track 0 correct", tr == 0);
    st = fdc_read_reg(FDC_REG_CMD_STATUS);
    TEST("TRACK0 bit set after seek to 0", st & FDC_STATUS_TRACK0);
}

static void test_read_sectors(void)
{
    printf("\n--- Test: Read Sectors ---\n");

    /* Target area in emulated RAM: start at 4KB */
    uint32_t ram_dest = 0x1000;

    /* Read track 0, side 0, sector 1 (boot sector) */
    int ret = bios_read_sectors(0, 0, 1, 1, ram_dest);
    TEST("Read sector 1 returns 0", ret == 0);

    /* Verify boot sector magic */
    TEST("Boot sector byte 0 = 0xEB", test_ram[ram_dest] == 0xEB);
    TEST("Boot sector bytes 3-9 = PISTORM",
         memcmp(test_ram + ram_dest + 3, "PISTORM", 7) == 0);

    /* Read track 0, side 0, sector 2 (should have pattern 0x01) */
    ram_dest = 0x2000;
    ret = bios_read_sectors(0, 0, 2, 1, ram_dest);
    TEST("Read sector 2 returns 0", ret == 0);
    TEST("Sector 2 pattern correct", test_ram[ram_dest] == 0x01);

    /* Read multiple sectors: track 0 side 0 sectors 1-9 */
    ram_dest = 0x3000;
    ret = bios_read_sectors(0, 0, 1, 9, ram_dest);
    TEST("Read 9 sectors (full track side 0) returns 0", ret == 0);

    /* Verify all 9 sectors present: sector i has pattern i-1 */
    bool multi_ok = true;
    for (int i = 1; i < 9; i++) {
        if (test_ram[ram_dest + i * 512] != (uint8_t)i) {
            multi_ok = false;
            fprintf(stderr, "  sector %d: expected 0x%02X got 0x%02X\n",
                    i + 1, i, test_ram[ram_dest + i * 512]);
        }
    }
    TEST("Multi-sector read data correct", multi_ok);
}

static void test_write_sectors(void)
{
    printf("\n--- Test: Write Sectors ---\n");

    /* Write a known pattern to track 5, side 1, sector 3 */
    uint32_t ram_src = 0x5000;
    for (int i = 0; i < 512; i++)
        test_ram[ram_src + i] = (uint8_t)(0xA5 ^ i);

    int ret = bios_write_sectors(5, 1, 3, 1, ram_src);
    TEST("Write sector returns 0", ret == 0);

    /* Read it back and compare */
    uint32_t ram_dst = 0x6000;
    memset(test_ram + ram_dst, 0, 512);
    ret = bios_read_sectors(5, 1, 3, 1, ram_dst);
    TEST("Read-back after write returns 0", ret == 0);

    bool match = (memcmp(test_ram + ram_src, test_ram + ram_dst, 512) == 0);
    TEST("Read-back data matches written data", match);
}

static void test_write_protect(void)
{
    printf("\n--- Test: Write Protect ---\n");

    fdd_set_write_protect(0, true);

    uint32_t ram_src = 0x7000;
    memset(test_ram + ram_src, 0xBB, 512);
    int ret = bios_write_sectors(0, 0, 1, 1, ram_src);
    TEST("Write to WP disk fails", ret != 0);

    uint8_t st = fdc_read_reg(FDC_REG_CMD_STATUS);
    TEST("WRTPROT bit set in status", st & FDC_STATUS_WRTPROT);

    fdd_set_write_protect(0, false);
}

static void test_read_address(void)
{
    printf("\n--- Test: Read Address (Type III) ---\n");

    /* Seek to track 10 */
    fdc_write_reg(FDC_REG_DATA, 10);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_SEEK);
    wait_fdc_done(500);

    uint32_t ram_addr = 0x8000;
    dma_set_addr(ram_addr);

    /* Select drive A, side 0 */
    uint8_t porta = 0xFF;
    porta &= ~PSG_DRIVE0_SEL;
    porta |= PSG_SIDE_SEL;
    write_psg(PSG_PORT_A, porta);

    /* Issue Read Address command */
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_READ_ADDR);
    wait_fdc_done(500);

    uint8_t *addr = test_ram + ram_addr;
    TEST("Read Address: track byte = 10", addr[0] == 10);
    TEST("Read Address: side byte = 0",   addr[1] == 0);
    TEST("Read Address: size code = 2",   addr[3] == 2);   /* 512 bytes */
}

static void test_force_interrupt(void)
{
    printf("\n--- Test: Force Interrupt ---\n");

    /* Start a seek then immediately force interrupt */
    fdc_write_reg(FDC_REG_DATA, 40);
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_SEEK);

    /* Force interrupt with $D8 (immediate interrupt condition) */
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_FORCE_INT | 0x08);

    uint8_t st = fdc_read_reg(FDC_REG_CMD_STATUS);
    TEST("Force interrupt clears BUSY", !(st & FDC_STATUS_BUSY));
    TEST("Force interrupt raises IRQ",  fdd_irq_active());

    /* $D0 = no interrupt condition */
    fdc_write_reg(FDC_REG_CMD_STATUS, FDC_CMD_FORCE_INT);
    st = fdc_read_reg(FDC_REG_CMD_STATUS);   /* reading status clears IRQ */
    TEST("Read status after $D0 clears IRQ", !fdd_irq_active());
}

static void test_eject_insert(void)
{
    printf("\n--- Test: Eject / Re-insert ---\n");

    fdd_eject_disk(0);

    /* Deselect drive (all bits high = no drive selected) */
    write_psg(PSG_PORT_A, 0xFF);

    /* Attempt to read from empty drive - drive is selected via PSG but no disk */
    write_psg(PSG_PORT_A, (uint8_t)(~PSG_DRIVE0_SEL | PSG_SIDE_SEL) & 0xFF);
    uint32_t ram_dst = 0x9000;
    int ret = bios_read_sectors(0, 0, 1, 1, ram_dst);
    TEST("Read from empty drive fails", ret != 0);

    /* Re-insert */
    ret = fdd_insert_disk(0, TMP_IMAGE, false);
    TEST("Re-insert image succeeds", ret == 0);

    ret = bios_read_sectors(0, 0, 1, 1, ram_dst);
    TEST("Read after re-insert succeeds", ret == 0);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    printf("=== Atari ST FDD Emulator Test ===\n");
    printf("    (pistorm-atari / Musashi target)\n\n");

    /* Point the FDD core at our test RAM */
    fdd_atari_ram = test_ram;

    /* Create or use provided image */
    const char *image = (argc > 1) ? argv[1] : TMP_IMAGE;
    if (argc <= 1) create_test_image();

    /* Initialise emulator */
    fdd_init();

    if (fdd_insert_disk(0, image, false) != 0) {
        fprintf(stderr, "Failed to mount image '%s'\n", image);
        return 1;
    }

    printf("\nDrive status:\n");
    fdd_status();

    /* Run tests */
    test_basic_seek();
    test_read_sectors();
    test_write_sectors();
    test_write_protect();
    test_read_address();
    test_force_interrupt();
    test_eject_insert();

    /* Summary */
    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    fdd_shutdown();
    if (argc <= 1) unlink(TMP_IMAGE);   /* clean up temp image */

    return tests_failed ? 1 : 0;
}
