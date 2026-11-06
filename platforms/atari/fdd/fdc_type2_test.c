/*
 * fdc_type2_test.c - WD1772 Type II commands against a real .st image
 *
 * Checks what a loader sees, register by register, as on the real chip:
 *   - a read without the m bit moves ONE sector, whatever the DMA count
 *   - the DMA address counter ($8609/0B/0D) advances by what was moved
 *   - with m, sectors run on to the DMA count or the track end (then RNF)
 *   - EmuTOS's way (address + count 1 per sector) is unchanged
 *
 * Build and run from the repo root:
 *   sh platforms/atari/fdd/fdc_type2_run.sh
 */

#include "atari_fdd.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- the emulator around atari_fdd.c, stubbed -------------------------- */

static uint8_t ram[0x400000];

bool FDD_enabled = true;
int  pst_dbg_acsi, pst_fdd_mfp_irq, pst_verbose;
void pistorm_dma_to_stram(uint32_t a, const uint8_t *b, uint32_t n)
{ if (a + n <= sizeof ram) memcpy(ram + a, b, n); }
void pistorm_dma_from_stram(uint32_t a, uint8_t *b, uint32_t n)
{ if (a + n <= sizeof ram) memcpy(b, ram + a, n); }
void pistorm_dma_write_stram(uint32_t a, const uint8_t *b, uint32_t n)
{ if (a + n <= sizeof ram) memcpy(ram + a, b, n); }
uint32_t pistorm_stram_top(void)      { return 0x400000u; }
uint32_t pistorm_stram_phys_top(void) { return 0x400000u; }
void     ps_write_8(uint32_t a, uint32_t v)  { (void)a; (void)v; }
void     ps_write_16(uint32_t a, uint32_t v) { (void)a; (void)v; }
uint32_t ps_read_8(uint32_t a)  { (void)a; return 0xFF; }
uint32_t ps_read_16(uint32_t a) { (void)a; return 0xFFFF; }
void mfp_hub_register_level(void *f) { (void)f; }
bool acsi_enabled(void) { return false; }
bool acsi_owns_dma(void) { return false; }
bool acsi_irq_active(void) { return false; }
void acsi_cmd_byte(uint8_t b, int first) { (void)b; (void)first; }
void acsi_dma_fifo_reset(void) {}
void acsi_release(void) {}
uint32_t acsi_residual_count(void) { return 0; }
uint8_t acsi_status_read(void) { return 0; }

/* ---- register access, as a 68000 program does it ------------------------ */

static void psg(uint8_t reg, uint8_t v)
{
	fdd_io_write(PSG_REG_SELECT, reg, 1);
	fdd_io_write(PSG_REG_WRITE, v, 1);
}

static void fdc_set(int reg, uint8_t v)
{
	fdd_io_write(DMA_MODE_REG, (uint16_t) ((reg & 3) << 1), 2);
	fdd_io_write(FDC_DATA_REG, v, 2);
}

static uint8_t fdc_get(int reg)
{
	fdd_io_write(DMA_MODE_REG, (uint16_t) ((reg & 3) << 1), 2);
	return (uint8_t) fdd_io_read(FDC_DATA_REG, 2);
}

static void dma_addr_set(uint32_t a)
{
	fdd_io_write(DMA_BASE_LOW, a & 0xFF, 1);
	fdd_io_write(DMA_BASE_MID, (a >> 8) & 0xFF, 1);
	fdd_io_write(DMA_BASE_HIGH, (a >> 16) & 0xFF, 1);
}

static uint32_t dma_addr_get(void)
{
	return (fdd_io_read(DMA_BASE_HIGH, 1) & 0xFF) << 16 |
	       (fdd_io_read(DMA_BASE_MID, 1) & 0xFF) << 8 |
	       (fdd_io_read(DMA_BASE_LOW, 1) & 0xFF);
}

static void dma_count_set(int n)
{
	fdd_io_write(DMA_MODE_REG, DMA_MODE_SCREG | DMA_MODE_RW, 2);
	fdd_io_write(DMA_MODE_REG, DMA_MODE_SCREG, 2);
	fdd_io_write(FDC_DATA_REG, (uint32_t) n, 2);
}

static int dma_count_get(void)
{
	fdd_io_write(DMA_MODE_REG, DMA_MODE_SCREG, 2);
	return (int) (fdd_io_read(FDC_DATA_REG, 2) & 0xFF);
}

/* ---- checks -------------------------------------------------------------- */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SPT 10
#define SIDES 2
#define TRACKS 80

/* every byte of a sector says where it came from */
static uint8_t tag(int t, int s, int sec) { return (uint8_t) (t * 7 + s * 3 + sec * 11); }

static int sector_is(uint32_t addr, int t, int s, int sec)
{
	for (int i = 0; i < 512; i++)
		if (ram[addr + i] != tag(t, s, sec))
			return 0;
	return 1;
}

static void seek(int t)
{
	fdc_set(FDC_REG_DATA, (uint8_t) t);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_SEEK);
}

int main(void)
{
	char img[] = "/tmp/fdc2XXXXXX";
	int fd = mkstemp(img);
	uint8_t sec[512];

	/* an 800K image, 80 x 10 x 2, with a real BPB in the boot sector */
	for (int t = 0; t < TRACKS; t++)
		for (int s = 0; s < SIDES; s++)
			for (int n = 1; n <= SPT; n++) {
				memset(sec, tag(t, s, n), 512);
				if (t == 0 && s == 0 && n == 1) {
					memset(sec, 0, 32);
					sec[0x0B] = 0x00; sec[0x0C] = 0x02;    /* 512 bps   */
					sec[0x13] = (TRACKS * SIDES * SPT) & 0xFF;
					sec[0x14] = (TRACKS * SIDES * SPT) >> 8;
					sec[0x18] = SPT;  sec[0x1A] = SIDES;
				}
				if (write(fd, sec, 512) != 512)
					return 2;
			}
	close(fd);

	fdd_init();
	CHECK(fdd_insert_disk(0, img, false) == 0, "insert");
	psg(PSG_PORT_A_REG, (uint8_t) (0xFF & ~PSG_DRIVE_A_SEL));  /* side 0 = bit set */

	/* 1. EmuTOS: address + count 1 for each sector */
	seek(3);
	for (int n = 1; n <= SPT; n++) {
		uint32_t a = 0x10000 + (uint32_t) (n - 1) * 512;

		fdc_set(FDC_REG_SECTOR, (uint8_t) n);
		dma_addr_set(a);
		dma_count_set(1);
		fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC);
		CHECK(!(fdc_get(FDC_REG_CMD_STATUS) & FDC_STATUS_RECNF), "emutos: RNF on sector %d", n);
		CHECK(sector_is(a, 3, 0, n), "emutos: sector %d data", n);
		CHECK(dma_count_get() == 0, "emutos: count %d after sector %d", dma_count_get(), n);
	}

	/* 2. Shadow of the Beast II: base once, count 14, one sector per
	 *    command, poll the address counter */
	memset(ram + 0x20000, 0, 0x4000);
	seek(59);
	dma_addr_set(0x20000);
	dma_count_set(14);
	for (int n = 1; n <= SPT; n++) {
		fdc_set(FDC_REG_SECTOR, (uint8_t) n);
		fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC);
		CHECK(!(fdc_get(FDC_REG_CMD_STATUS) & FDC_STATUS_RECNF), "loader: RNF on sector %d", n);
		CHECK(dma_addr_get() == 0x20000u + (uint32_t) n * 512,
		      "loader: address %06X after sector %d", dma_addr_get(), n);
	}
	for (int n = 1; n <= SPT; n++)
		CHECK(sector_is(0x20000 + (uint32_t) (n - 1) * 512, 59, 0, n), "loader: sector %d data", n);
	CHECK(dma_count_get() == 4, "loader: count %d, wanted 4", dma_count_get());

	/* 3. m bit, count past the track end: 10 sectors, then RNF */
	seek(10);
	dma_addr_set(0x30000);
	dma_count_set(14);
	fdc_set(FDC_REG_SECTOR, 1);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC | 0x10);
	CHECK(fdc_get(FDC_REG_CMD_STATUS) & FDC_STATUS_RECNF, "m: no RNF at the track end");
	CHECK(dma_addr_get() == 0x30000u + SPT * 512, "m: address %06X", dma_addr_get());
	for (int n = 1; n <= SPT; n++)
		CHECK(sector_is(0x30000 + (uint32_t) (n - 1) * 512, 10, 0, n), "m: sector %d data", n);

	/* 4. m bit inside the track: DMA count ends it, no RNF */
	dma_addr_set(0x40000);
	dma_count_set(5);
	fdc_set(FDC_REG_SECTOR, 3);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC | 0x10);
	CHECK(!(fdc_get(FDC_REG_CMD_STATUS) & FDC_STATUS_RECNF), "m5: RNF");
	CHECK(dma_addr_get() == 0x40000u + 5 * 512, "m5: address %06X", dma_addr_get());
	CHECK(fdc_get(FDC_REG_SECTOR) == 8, "m5: sector register %d, wanted 8", fdc_get(FDC_REG_SECTOR));
	CHECK(sector_is(0x40000, 10, 0, 3) && sector_is(0x40000 + 4 * 512, 10, 0, 7), "m5: data");

	/* 5. a sector that is not on the track */
	dma_addr_set(0x50000);
	dma_count_set(1);
	fdc_set(FDC_REG_SECTOR, 11);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC);
	CHECK(fdc_get(FDC_REG_CMD_STATUS) & FDC_STATUS_RECNF, "sector 11: no RNF");
	CHECK(dma_addr_get() == 0x50000u, "sector 11: address moved");

	/* 6. writes: one sector without m, address advances */
	memset(ram + 0x60000, 0x5A, 1024);
	seek(20);
	dma_addr_set(0x60000);
	dma_count_set(2);
	fdc_set(FDC_REG_SECTOR, 4);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_WRITE_SEC);
	CHECK(dma_addr_get() == 0x60200u, "write: address %06X", dma_addr_get());
	dma_addr_set(0x70000);
	dma_count_set(2);
	fdc_set(FDC_REG_SECTOR, 4);
	fdc_set(FDC_REG_CMD_STATUS, FDC_CMD_READ_SEC | 0x10);
	CHECK(ram[0x70000] == 0x5A && ram[0x701FF] == 0x5A, "write: sector 4 not written");
	CHECK(sector_is(0x70200, 20, 0, 5), "write: sector 5 was written too");

	/* 7. next disk of a set - Steve's real names, plus their disk 2s */
	{
		char dir[] = "/tmp/fddsetXXXXXX", p[600], out[600];
		const char *f[] = {
			"Shadow of the Beast (1990)(Psygnosis)(Disk 1 of 2)[cr Replicants][t +2][a].st",
			"Shadow of the Beast (1990)(Psygnosis)(Disk 1 of 2)[cr Replicants][t].st",
			"Shadow of the Beast (1990)(Psygnosis)(Disk 2 of 2)[cr Replicants].st",
			"Shadow of the Beast II (1991)(Psygnosis)(Disk 1 of 2)[cr Elite][t][b].st",
			"Shadow of the Beast II (1991)(Psygnosis)(Disk 1 of 2)[cr Vmax][t].st",
			"Shadow of the Beast II (1991)(Psygnosis)(Disk 2 of 2)[cr Vmax].st",
			"Shadow of the Beast II (1991)(Psygnosis)(Disk 2 of 2)[cr Elite].st",
			"Lone (1990)(X)(Disk 1 of 3).st",
			"game1.st", "game2.st", "solo3.st" };
		const struct { int from; const char *to; } want[] = {
			{ 0, f[2] }, { 1, f[2] }, { 2, f[1] },  /* 2 -> 1: best tail, then name */
			{ 3, f[6] }, { 4, f[5] }, { 5, f[4] },
			{ 6, NULL /* f[3] or f[4]: either is disk 1 */ },
			{ 7, "" },  { 8, f[9] }, { 9, f[8] }, { 10, "" } };

		if (!mkdtemp(dir)) { CHECK(0, "set: mkdtemp"); }
		else {
			for (unsigned k = 0; k < sizeof f / sizeof f[0]; k++) {
				snprintf(p, sizeof p, "%s/%s", dir, f[k]);
				FILE *h = fopen(p, "w"); if (h) fclose(h);
			}
			for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) {
				int got;

				snprintf(p, sizeof p, "%s/%s", dir, f[want[k].from]);
				got = fdd_next_in_set(p, out, sizeof out);
				if (!want[k].to)
					CHECK(got && strstr(out, "(Disk 1 of 2)[cr "), "set: %s -> %s",
					      f[want[k].from], got ? out : "(none)");
				else if (!want[k].to[0])
					CHECK(!got, "set: %s should have no next, got %s", f[want[k].from], out);
				else
					CHECK(got && !strcmp(strrchr(out, '/') + 1, want[k].to),
					      "set: %s -> %s, wanted %s", f[want[k].from],
					      got ? strrchr(out, '/') + 1 : "(none)", want[k].to);
			}

			/* the keys: eject, then Insert puts in the disk after it */
			snprintf(p, sizeof p, "%s/%s", dir, f[3]);
			{	/* a real 800K image under that name */
				FILE *src = fopen(img, "rb"), *dst = fopen(p, "wb");
				int c;
				while (src && dst && (c = fgetc(src)) != EOF) fputc(c, dst);
				if (src) fclose(src);
				if (dst) fclose(dst);
				snprintf(out, sizeof out, "%s/%s", dir, f[6]);
				src = fopen(p, "rb"); dst = fopen(out, "wb");
				while (src && dst && (c = fgetc(src)) != EOF) fputc(c, dst);
				if (src) fclose(src);
				if (dst) fclose(dst);
			}
			CHECK(fdd_insert_disk(0, p, false) == 0, "keys: insert disk 1");
			fdd_hotkey_eject(0);
			CHECK(fdd_query(0, NULL, 0, NULL, NULL) == 0, "keys: not ejected");
			fdd_hotkey_next(0);
			out[0] = 0;
			CHECK(fdd_query(0, out, sizeof out, NULL, NULL) == 1 && strstr(out, "(Disk 2 of 2)[cr Elite]"),
			      "keys: after eject + Insert drive has '%s'", out);
			fdd_hotkey_next(0);       /* Insert with a disk in: straight to the next */
			out[0] = 0;
			CHECK(fdd_query(0, out, sizeof out, NULL, NULL) == 1 && strstr(out, "(Disk 1 of 2)"),
			      "keys: Insert again should wrap to disk 1, has '%s'", out);
			snprintf(p, sizeof p, "rm -rf '%s'", dir);
			if (system(p)) {}
		}
	}

	/* 8. no BPB: the geometry is the one the size is exactly */
	{
		const struct { int t, s, n; } g[] = {
			{ 80, 2, 10 }, { 80, 2, 9 }, { 80, 1, 9 }, { 80, 2, 11 },
			{ 81, 2, 10 }, { 82, 2, 10 }, { 80, 2, 18 } };
		char raw[] = "/tmp/fdc2rawXXXXXX";

		for (unsigned k = 0; k < sizeof g / sizeof g[0]; k++) {
			int rfd = mkstemp(raw);
			long sz = (long) g[k].t * g[k].s * g[k].n * 512;
			int nt = 0, ns = 0, spt = 0;

			memset(sec, 0xE5, 512);                    /* no BPB anywhere */
			for (long o = 0; o < sz; o += 512)
				if (write(rfd, sec, 512) != 512) return 2;
			close(rfd);
			CHECK(fdd_insert_disk(1, raw, false) == 0, "raw insert");
			fdd_geometry(1, &nt, &ns, &spt);
			CHECK(nt == g[k].t && ns == g[k].s && spt == g[k].n,
			      "raw %ld bytes: %d x %d x %d, wanted %d x %d x %d", sz,
			      nt, ns, spt, g[k].t, g[k].s, g[k].n);
			unlink(raw);
			strcpy(raw, "/tmp/fdc2rawXXXXXX");
		}
	}

	unlink(img);
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
