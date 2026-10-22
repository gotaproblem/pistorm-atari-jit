/*
 * atari_fdd.c - Atari ST FDD Emulator implementation
 *
 * All fixes applied:
 *   - PSG bit 1 = drive A, bit 0 = drive B (correct Atari HW wiring)
 *   - Restore command sets sector_reg = 1 (WD1772 hardware behaviour)
 *   - Type I commands assert IRQ immediately (irq_ready=true, no latch delay)
 *   - Type II commands use one-poll IRQ latch (irq_ready=false initially)
 *   - dma_sector_count set to 0 on transfer completion (EmuTOS checks this)
 *   - DMA transfers use ps_write_16/ps_read_16 for real ST RAM access
 *   - Geometry read from BPB in boot sector, file size as fallback
 *   - Longword (size=4) accesses split into two word accesses
 *   - dma_status reset at start of each transfer, not on R/W toggle
 *   - Force interrupt always clears irq before optionally re-raising
 */

#include "atari_fdd.h"
#include "gpio/ps_protocol.h"
#include "platforms/atari/mfp_hub.h"

#include <stdio.h>
#include "platforms/atari/psctrl/psctrl_tunables.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>

/* =========================================================================
 * Logging
 * ========================================================================= */
#define FDD_LOG(fmt, ...)  fprintf(stderr, "[FDD] " fmt "\n", ##__VA_ARGS__)
#define FDD_INFO(fmt, ...) PS_INFO("[FDD] " fmt "\n", ##__VA_ARGS__)   /* only with `debug verbose` */
#define FDD_DBG(fmt, ...)  //fprintf(stderr, "[FDD] " fmt "\n", ##__VA_ARGS__)

/* PISTORM_ACSI_DEBUG=1: trace the first accesses to the DMA-port window
 * so "guest never sent anything" and "sent but misrouted" are separable
 * from one boot log. Bounded; silent when unset. */
static int acsi_dbg_on(void)
{
    /* This used to test == '1' while acsi.c tested != '0', so
     * PISTORM_ACSI_DEBUG=2 turned one trace on and not the other. */
    return pst_dbg_acsi;
}
static int acsi_dbg_count;
#define ACSI_DBG_LIMIT 2000


#include "acsi.h"
#include <time.h>

/* =========================================================================
 * PISTORM_HDC_TRACE=1 - one line per REAL-ACSI command (passthrough path)
 *
 * Question it answers: when the guest reads the ACSI status byte, has the
 * device actually finished? It logs, per command:
 *   - the CDB as the guest sent it
 *   - how long after the last CDB byte the guest read the status byte
 *   - the first GPIP5-low the guest saw after the last CDB byte, how long
 *     after it, and WHO pulled it:  R = the real line (the device's /IRQ),
 *     F = emulated floppy INTRQ, A = emulated ACSI target, I = emulated IDE
 *   - the first DMA mode write ($8606) after the last CDB byte (a driver
 *     reprogramming the DMA chip ends any transfer still in progress)
 *   - how far the DMA address counter actually moved by the status read
 * PISTORM_HDC_TRACE=2 limits it to write commands; also logs real-FDC
 * pulls. Unbounded; silent and free when unset.
 * ========================================================================= */
static int hdc_trc = -1;

static int hdc_trace_on(void)
{
    if (hdc_trc < 0) {
        const char *e = getenv("PISTORM_HDC_TRACE");
        hdc_trc = (!e || !*e || *e == '0') ? 0 : (*e == '2') ? 2 : 1;
        if (hdc_trc)
            FDD_LOG("HDC trace on (PISTORM_HDC_TRACE=%d: %s)", hdc_trc,
                    hdc_trc == 2 ? "write commands only" : "everything");
    }
    return hdc_trc;
}

static uint64_t ht_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#define HT_MAX 1000000u          /* effectively unbounded: a boot alone
                                    used the old 200-line cap */
static unsigned ht_lines;
static struct {
    int      live;          /* first CDB byte seen, status not read yet */
    unsigned n;             /* CDB bytes so far                         */
    uint8_t  cdb[17];
    uint64_t t_last;        /* most recent CDB byte                     */
    uint32_t base;          /* DMA base when the command started        */
    uint64_t t_low;         /* first GPIP5 low after t_last (0 = none)  */
    char     src;
    unsigned polls;         /* GPIP reads since t_last                  */
    uint64_t t_mode;        /* first $8606 write since t_last (0 = none)*/
    int      wr;            /* this command writes (direction or opcode) */
    uint64_t t_poll;        /* most recent GPIP read since t_last       */
    unsigned tc_reads;      /* Timer C data ($FFFA23) reads since t_last */
    unsigned tc_falls;      /* ... bit 7 going 1 -> 0 between two reads  */
    unsigned tc_berr;       /* ... reads that came back with a bus error */
    int      tc_prev;       /* last bit 7 seen (-1 = none yet)          */
} ht;

/* " poll-end +Xus  TimerC n rd / k b7-falls / e berr" for the line */
static void ht_loop(char *out, size_t sz)
{
    char pe[24];
    if (ht.t_poll)
        snprintf(pe, sizeof pe, "+%lluus",
                 (unsigned long long)(ht.t_poll - ht.t_last));
    else
        snprintf(pe, sizeof pe, "none");
    snprintf(out, sz, "  last-poll %s  TimerC %u rd/%u b7-falls/%u berr",
             pe, ht.tc_reads, ht.tc_falls, ht.tc_berr);
}

/* PISTORM_HDC_TRACE=1: every command. =2: write commands only. */
#define HT_ON() (hdc_trc == 1 || (hdc_trc == 2 && ht.wr))

static void ht_cdb(const char *tag)
{
    char buf[3 * 17 + 1];
    int  k = 0;
    for (unsigned i = 0; i < ht.n && i < sizeof ht.cdb; i++)
        k += snprintf(buf + k, sizeof buf - k, "%02X ", ht.cdb[i]);
    buf[k ? k - 1 : 0] = 0;
    FDD_LOG("HDC %s [%s]", tag, buf);
}

static void hdc_trace_cmd_byte(uint8_t v, bool first, uint32_t base)
{
    if (!hdc_trace_on())
        return;
    if (first) {
        if (ht.live && HT_ON() && ht_lines < HT_MAX) {
            /* what the abandoned command got before the guest gave up:
             * time since its last CDB byte (= the driver's timeout or
             * retry point), and whether the device ever pulled GPIP5.
             * No "DMA moved": the guest has already loaded the next
             * command's DMA base by now. */
            char tag[300], low[48], loop[96];
            uint64_t now = ht_now_us();
            ht_loop(loop, sizeof loop);
            if (ht.t_low)
                snprintf(low, sizeof low, "%c +%lluus/%upolls", ht.src,
                         (unsigned long long)(ht.t_low - ht.t_last), ht.polls);
            else
                snprintf(low, sizeof low, "never (%u polls)", ht.polls);
            snprintf(tag, sizeof tag,
                     "ABANDONED - new command $%02X @0x%06X +%lluus after last "
                     "CDB byte, no status read; GPIP5-low %s  mode-wr %s%s",
                     v, base, (unsigned long long)(now - ht.t_last), low,
                     ht.t_mode ? "yes" : "none", loop);
            ht_lines++;
            ht_cdb(tag);
        }
        ht.live = 1;
        ht.n    = 0;
        ht.wr   = (v & 0x1F) == 0x0A;               /* WRITE(6) */
        ht.base = base;
    }
    if (!ht.live)
        return;
    if (ht.n == 1 && (ht.cdb[0] & 0x1F) == 0x1F && v == 0x2A)
        ht.wr = 1;                                  /* ICD WRITE(10) */
    if (ht.n < sizeof ht.cdb)
        ht.cdb[ht.n] = v;
    ht.n++;
    ht.t_last = ht_now_us();
    ht.t_low  = 0;
    ht.t_mode = 0;
    ht.polls  = 0;
    ht.t_poll   = 0;
    ht.tc_reads = 0;
    ht.tc_falls = 0;
    ht.tc_berr  = 0;
    ht.tc_prev  = -1;
}

static void hdc_trace_gpip(int low, char src)
{
    if (!hdc_trc || !ht.live)
        return;
    ht.polls++;
    ht.t_poll = ht_now_us();
    if (low && !ht.t_low) {
        ht.t_low = ht_now_us();
        ht.src   = src;
    }
}

/* called from pistorm_natmem.cpp's io_bget for every $FFFA23 (Timer C
 * data) read. HDDriver times its ACSI waits by counting bit-7 falls of
 * this register (one per Timer C period, 5 ms at TOS's 200 Hz), so the
 * count per command says how long the GUEST thought it waited. */
void hdc_trace_tcdr(uint8_t v, int berr)
{
    if (!hdc_trc || !ht.live)
        return;
    ht.tc_reads++;
    if (berr) {
        ht.tc_berr++;
        return;
    }
    int b7 = (v >> 7) & 1;
    if (ht.tc_prev == 1 && b7 == 0)
        ht.tc_falls++;
    ht.tc_prev = b7;
}

/* called from pistorm_natmem.cpp's GPIP shim when the emulated IDE INTRQ
 * pulls GPIP5 (that is applied after fdd_gpip, so fdd_gpip cannot see it) */
void hdc_trace_gpip_ide(void)
{
    hdc_trace_gpip(1, 'I');
}

static void hdc_trace_mode_write(void)
{
    if (hdc_trc && ht.live && !ht.t_mode)
        ht.t_mode = ht_now_us();
}

static void hdc_trace_sector(const uint8_t *b);

static void hdc_trace_status(uint32_t status)
{
    if (!hdc_trc || !ht.live)
        return;
    ht.live = 0;
    if (!HT_ON() || ht_lines >= HT_MAX)
        return;
    ht_lines++;

    uint64_t now  = ht_now_us();
    uint32_t addr = ((uint32_t)ps_read_8(0xFF8609u) << 16) |
                    ((uint32_t)ps_read_8(0xFF860Bu) << 8)  |
                     (uint32_t)ps_read_8(0xFF860Du);
    char low[48], mode[32];

    if (ht.t_low)
        snprintf(low, sizeof low, "%c +%lluus/%upolls", ht.src,
                 (unsigned long long)(ht.t_low - ht.t_last), ht.polls);
    else
        snprintf(low, sizeof low, "never (%u polls)", ht.polls);
    if (ht.t_mode)
        snprintf(mode, sizeof mode, "+%lluus",
                 (unsigned long long)(ht.t_mode - ht.t_last));
    else
        snprintf(mode, sizeof mode, "none");

    char tag[300], loop[96];
    ht_loop(loop, sizeof loop);
    snprintf(tag, sizeof tag,
             "st=%02X read +%lluus  GPIP5-low %s  mode-wr %s  DMA moved %d B%s",
             status & 0xFFu, (unsigned long long)(now - ht.t_last), low, mode,
             (int)(addr - ht.base), loop);
    ht_cdb(tag);

    /* write commands: decode the first sector as it sits in REAL RAM -
     * what the DMA chip sent - so a write to LBA 0 shows the partition
     * table that went to the disk */
    if (ht.wr && (int)(addr - ht.base) >= 512) {
        uint8_t b[512];
        for (unsigned i = 0; i < 512u; i++)
            b[i] = ps_read_8(ht.base + i);
        hdc_trace_sector(b);
    }
}

/* When the floppy itself is NOT emulated (no "fdd" cfg lines) but
 * emulated ACSI targets are attached, this file still owns the DMA-port
 * window - FDC-selected traffic must then PASS THROUGH to the real bus
 * (a real drive/Gotek may be connected), while HDC-selected traffic is
 * routed between emulated targets and the real ACSI bus per ID. */
extern bool FDD_enabled;

/* Real-device passthrough: see fdd_init_passthrough(). */
static bool fdd_passthrough;

/* =========================================================================
 * Global state
 * ========================================================================= */
static fdc_controller_t fdc;
static int motor_ticks[FDD_MAX_DRIVES];

/* Who ran the DMA last: 0 = emulated FDC, 1 = real HDC/ACSI. Decides which
 * side answers reads of the shared registers (status, count, counters). */
static int hdc_active;

/* A sector-count write is anonymous (same mode bits for floppy and ACSI);
 * it becomes an ACSI transfer's count only when ACSI command bytes follow.
 * Latched at count-write, claimed (or discarded) by the next command. */
static int hdc_count_fresh;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
static void     rfdc_gpip(uint8_t gpip);
static void     fdc_decode_drive_side(void);
static void     fdc_exec_command(uint8_t cmd);
static void     fdc_do_type1(uint8_t cmd);
static void     fdc_do_read_sectors(void);
static void     fdc_do_write_sectors(void);
static void     fdc_do_read_address(void);
static void     fdc_raise_irq_immediate(void);
static void     fdc_raise_irq_latched(void);
static void     fdc_clear_irq(void);
static uint32_t fdc_read_addr(uint32_t addr, int size);
static uint32_t dma_read_addr(uint32_t addr, int size);
static uint32_t psg_read_addr(uint32_t addr, int size);
static void     fdc_write_addr(uint32_t addr, uint32_t val, int size);
static void     dma_write_addr(uint32_t addr, uint32_t val, int size);
static void     psg_write_addr(uint32_t addr, uint32_t val, int size);
static int      fdd_image_read(int drive, int track, int side,
                               int sector, int count, uint8_t *buf);
static int      fdd_image_write(int drive, int track, int side,
                                int sector, int count, const uint8_t *buf);
static off_t    sector_offset(fdd_drive_t *d, int track, int side,
                              int sector);

/* =========================================================================
 * DMA transfer to/from real Atari ST RAM via ps_protocol
 *
 * ps_write_16 / ps_read_16 use little-endian byte order:
 *   ps_write_16(addr, val) where val = (buf[i+1] << 8) | buf[i]
 * ========================================================================= */

extern void stram_dma_write(uint32_t addr, const uint8_t *buf, unsigned int count);

#ifdef __cplusplus
extern "C" {
#endif

void pistorm_dma_to_stram(uint32_t, const uint8_t*, uint32_t);
void pistorm_dma_from_stram(uint32_t, uint8_t*, uint32_t);

#ifdef __cplusplus
}
#endif

/* falcon_stram 14M: the DMA address counter is 24 bits (a Falcon's is),
 * not the ST's 22, or a buffer at $C1xxxx lands at $01xxxx - over the
 * guest's low RAM. The board's real RAM ends at 4MB at most: above that
 * the transfer goes to the natmem copy only. */
uint32_t pistorm_stram_top(void);   /* pistorm_natmem.cpp */
static inline uint32_t dma_hi_mask(void)
{
    return pistorm_stram_top() > 0x400000u ? 0xFFu : 0x3Fu;
}

static void dma_copy_to_ram(uint32_t addr, const uint8_t *buf, size_t count)
{
    for (size_t i = 0; i < count && addr + i < 0x400000u; i++)
        ps_write_8(addr + i, buf[i]);                  /* real ST-RAM — video shifter / bus masters */

    pistorm_dma_to_stram(addr, buf, (uint32_t)count);  /* JIT natmem mirror + SMC invalidate */
}

static void dma_copy_from_ram(uint32_t addr, uint8_t *buf, size_t count)
{
    /* Snapshot the CPU's *authoritative* memory (the natmem mirror), NOT the
     * real bus. CPU stores to ST-RAM always land in natmem but are only
     * conditionally written through to the bus (stram_needs_bus_write), so
     * reading ps_read_8() here captures STALE bus bytes -> the FDC then writes
     * garbage sectors to the .st image, progressively corrupting the FAT /
     * directory / files. Read natmem so we write exactly what the guest wrote. */
    pistorm_dma_from_stram(addr, buf, (uint32_t)count);
}

/* =========================================================================
 * Services for the emulated ACSI targets (acsi.c). The DMA chip state -
 * base pointer, mode direction, status - is one set of registers shared
 * by the FDC and the ACSI port, and it lives here.
 * ========================================================================= */

/* the LIVE address counter (fdc.dma_addr) - it is what transfers read
 * and advance, and what the guest sees on $8609/0B/0D readback; the
 * programmed base (dma_base_addr) stays put, as on real hardware */
uint32_t fdd_dma_base(void)               { return fdc.dma_addr; }
void     fdd_dma_base_advance(uint32_t n) { fdc.dma_addr += n; }
int      fdd_dma_dir_is_write(void)
{
    return (fdc.dma_mode & DMA_MODE_RW) != 0;
}
void fdd_dma_note_ok(int count_zero)
{
    fdc.dma_status = DMA_STATUS_OK | (count_zero ? DMA_STATUS_SCZERO : 0);
}
void fdd_dma_copy_to_ram(uint32_t addr, const uint8_t *buf, uint32_t count)
{
    dma_copy_to_ram(addr, buf, count);
}
void fdd_dma_copy_from_ram(uint32_t addr, uint8_t *buf, uint32_t count)
{
    dma_copy_from_ram(addr, buf, count);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

bool fdd_owns_address(uint32_t addr)
{
    if (addr >= 0xFF8600u && addr <= 0xFF860Fu) return true;
    if (addr >= 0xFF8800u && addr <= 0xFF8803u) return true;
    return false;
}

/* Dispatcher gate: route this address into fdd_io_*?
 *
 * Floppy emulated: the full window incl. the PSG shadow (drive/side
 * select lives there). ACSI-only: JUST the DMA-port window - the PSG
 * must stay on its normal path or the local shadow would swallow real
 * PSG accesses. Neither: nothing routes here. */
bool fdd_route_address(uint32_t addr)
{
    if (FDD_enabled)
        return fdd_owns_address(addr);
    if (acsi_enabled() || fdd_passthrough)
        return addr >= 0xFF8600u && addr <= 0xFF860Fu;
    return false;
}

/* Is the DMA-port window handled by this file at all (any of: emulated
 * floppy, emulated ACSI targets, or real-device passthrough)? The gate the
 * dispatchers test before fdd_route_address / fdd_gpip. */
bool fdd_dma_routed(void)
{
    return FDD_enabled || acsi_enabled() || fdd_passthrough;
}

/* No "fdd" lines and no emulated ACSI target: real floppy (drive, Gotek)
 * and real ACSI devices only. The DMA-port window still routes through
 * fdd_io_* so that real bus-master DMA keeps the emulator's RAM image
 * coherent: the ACSI pull/push below, and the real-FDC pull (rfdc_*).
 * Without this a real floppy read landed in real RAM only and the guest
 * read the stale image - and real ACSI fell back to the old dma_snoop. */
void fdd_init_passthrough(void)
{
    fdd_init();
    fdd_passthrough = true;
}

/* MFP channel 7 = GPIP5 = the shared WD1772 INTRQ / ACSI /IRQ line.
 *
 * FINDING (19 Sep): the emulated FDC and the emulated ACSI targets have
 * never been able to raise an actual interrupt. fdd_gpip() pulls bit 5
 * low in the GPIP BYTE, which serves a guest that POLLS $FFFA01 - what
 * TOS and EmuTOS do, and why floppy I/O works - but the real MFP sits on
 * the real board with its real GPIP5 pin, and nothing we emulate can
 * move that pin. Software that waits on the FDC interrupt instead of
 * polling therefore waits forever.
 *
 * mfp_hub already knows how to be an MFP channel, and a GPIP line is a
 * LEVEL source (it asserts while the condition holds and re-requests
 * after EOI while still asserted), so the whole fix is to hand it this
 * poll. The hub applies the 68901 delivery rule - enabled in IERB,
 * unmasked in IMRB, outranks every virtual in-service channel - so a
 * guest that has not asked for the interrupt is not given one.
 *
 * Off by default (pst_fdd_mfp_irq): every configuration that works today
 * works by polling, and turning a new interrupt source on under a
 * working system is exactly the kind of change that costs a day. */
static int fdd_mfp_level_poll(void)
{
    if (!pst_fdd_mfp_irq)
        return 0;
    if (acsi_irq_active())
        return 1;
    /* Side-effect free: fdd_irq_active() ARMS irq_ready on its first
     * call, which is the GPIP poll path's one-read delay. Reading the
     * two flags here must not consume that. */
    return (fdc.selected_drive >= 0 &&
            fdc.drives[fdc.selected_drive].disk_inserted &&
            fdc.irq_pending && fdc.irq_ready) ? 1 : 0;
}

void fdd_init(void)
{
    memset(&fdc, 0, sizeof(fdc));
    pthread_mutex_init(&fdc.lock, NULL);

    fdc.status         = 0x00;
    fdc.dma_status     = DMA_STATUS_OK;
    fdc.psg_porta      = 0xFF;
    fdc.selected_drive = -1;
    fdc.sector_reg     = 1;     /* WD1772 power-on default */
    hdc_active         = 0;     /* DMA starts floppy-owned */
    hdc_count_fresh    = 0;

    for (int i = 0; i < FDD_MAX_DRIVES; i++) {
        fdc.drives[i].fd            = -1;
        fdc.drives[i].current_track = 0;
        fdc.drives[i].num_sides     = 2;
        motor_ticks[i]              = 0;
    }

    mfp_hub_register_level(7, fdd_mfp_level_poll);   /* GPIP5 - see above */

    /* Quieten the REAL WD1772 once. With the floppy emulated, nothing ever
     * talks to the chip on the board, and a 1772 holds INTRQ asserted from
     * power-on until its status register is read (or a command is written).
     * INTRQ is wired to GPIP5 together with ACSI /IRQ, so every ACSI
     * driver that polls GPIP5 - TOS, EmuTOS, HDDriver - saw "device ready"
     * on its FIRST poll of every command, read a status byte off a bus the
     * device had not yet driven, and never waited for the data phase.
     *
     * Reproduced 19 Oct 2026 (cold power-up, fdd 0: mounted, EmuTOS): IDs
     * with no device answered st=FF on 1 poll, ID 0's INQUIRY moved 0 bytes
     * on 1 poll; with the floppy not emulated the same INQUIRY took 23 polls
     * and moved 32 bytes. Removing the fdd line, or any guest floppy access
     * that reached the real chip, cleared it - which is why it only showed
     * after a full power-off. One real status read, FDC selected (mode bit 3
     * = 0, A1:A0 = 00), done before the guest runs. The value is discarded.
     * Real-floppy configurations are unaffected (the guest talks to the chip
     * itself there). PISTORM_FDC_NO_INTRQ_CLEAR=1 skips it, for A/B. */
    {
        const char *e = getenv("PISTORM_FDC_NO_INTRQ_CLEAR");
        if (!(e && *e == '1')) {
            ps_write_16(DMA_MODE_REG, 0x0080);        /* FDC selected, status reg, read */
            uint16_t st = ps_read_16(FDC_DATA_REG);   /* WD1772 status read clears INTRQ */
            ps_write_16(DMA_MODE_REG, 0x0080);
            FDD_INFO("real WD1772 status read once at init (INTRQ cleared): $%02X", st & 0xFF);
        }
    }

    FDD_INFO("Initialised");
}

void fdd_shutdown(void)
{
    //pthread_mutex_lock(&fdc.lock);
    for (int i = 0; i < FDD_MAX_DRIVES; i++) {
        if (fdc.drives[i].fd >= 0) {
            close(fdc.drives[i].fd);
            fdc.drives[i].fd = -1;
        }
    }
    //pthread_mutex_unlock(&fdc.lock);
    //pthread_mutex_destroy(&fdc.lock);
    FDD_LOG("Shutdown");
}

/* =========================================================================
 * MSA: Magic Shadow Archiver images (magic $0E0F, big-endian header,
 * per-track data blocks, RLE with $E5 marker - same format STBOX
 * decodes). Detected by MAGIC, not suffix. Decoded once at insert time
 * into an anonymous unlinked temp file, so the rest of the FDC model
 * only ever sees a raw .ST layout. The decoded copy is throwaway: MSA
 * mounts are forced read-only, there is no write-back into the archive.
 *
 * Returns: decoded fd (>= 0), -1 = not an MSA (caller opens it raw),
 * -2 = is an MSA but corrupt (caller must fail the mount).
 * ========================================================================= */
static int msa_open_decoded(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw = NULL, *img = NULL;

    if (!f)
        return -1;                          /* let the raw open() report */

    uint8_t magic[2];
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 0x0E || magic[1] != 0x0F) {
        fclose(f);
        return -1;                          /* plain .ST / raw image     */
    }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 12 || fsz > 2 * 1024 * 1024) goto bad;
    raw = malloc((size_t)fsz);
    if (!raw || fread(raw, 1, (size_t)fsz, f) != (size_t)fsz) goto bad;
    fclose(f); f = NULL;

    uint16_t spt    = (uint16_t)((raw[2] << 8) | raw[3]);
    uint16_t sides  = (uint16_t)(((raw[4] << 8) | raw[5]) + 1);
    uint16_t tstart = (uint16_t)((raw[6] << 8) | raw[7]);
    uint16_t tend   = (uint16_t)((raw[8] << 8) | raw[9]);
    uint32_t tlen   = 512u * spt;
    uint32_t total  = tlen * sides * (uint32_t)(tend - tstart + 1);
    if (!spt || spt > 12 || sides > 2 || tend < tstart ||
        total > 2048u * 1024) goto bad;
    img = calloc(1, total);
    if (!img) goto bad;

    uint32_t src = 10, dst = 0;
    for (uint32_t t = 0; t < (uint32_t)(tend - tstart + 1) * sides; t++) {
        if (src + 2 > (uint32_t)fsz) goto bad;
        uint32_t dlen = (uint32_t)((raw[src] << 8) | raw[src + 1]);
        src += 2;
        if (src + dlen > (uint32_t)fsz || dst + tlen > total) goto bad;
        if (dlen == tlen) {
            memcpy(img + dst, raw + src, tlen);
        } else {                                    /* RLE */
            uint32_t o = dst, e = src + dlen, i = src;
            while (i < e && o < dst + tlen) {
                uint8_t b = raw[i++];
                if (b != 0xE5) { img[o++] = b; continue; }
                if (i + 3 > e) goto bad;
                uint8_t v = raw[i];
                uint32_t n = (uint32_t)((raw[i + 1] << 8) | raw[i + 2]);
                i += 3;
                while (n-- && o < dst + tlen) img[o++] = v;
            }
        }
        src += dlen;
        dst += tlen;
    }
    free(raw); raw = NULL;

    char tmpl[] = "/tmp/pistorm_msa_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) goto bad;
    unlink(tmpl);                           /* anonymous: gone on close  */
    if (write(fd, img, total) != (ssize_t)total) { close(fd); goto bad; }
    lseek(fd, 0, SEEK_SET);
    free(img);
    FDD_LOG("MSA '%s' decoded: %u tracks %u spt %u sides (%u KB, read-only)",
            path, (unsigned)(tend - tstart + 1), spt, sides, total / 1024);
    return fd;

bad:
    FDD_LOG("MSA '%s': corrupt or unsupported archive", path);
    if (f) fclose(f);
    free(raw);
    free(img);
    return -2;
}

int fdd_insert_disk(int drive, const char *image_path, bool write_protect)
{
    if (drive < 0 || drive >= FDD_MAX_DRIVES) return -1;

    fdd_drive_t *drv = &fdc.drives[drive];

   // pthread_mutex_lock(&fdc.lock);

    if (drv->fd >= 0) {
        close(drv->fd);
        drv->fd = -1;
    }

    int fd = msa_open_decoded(image_path);
    if (fd == -2) {
        //pthread_mutex_unlock(&fdc.lock);
        return -1;                          /* MSA but broken            */
    }
    if (fd >= 0) {
        write_protect = true;               /* no write-back into MSA    */
    } else {
        int flags = write_protect ? O_RDONLY : O_RDWR;
        fd = open(image_path, flags);
    }
    if (fd < 0) {
        FDD_LOG("Failed to open '%s': %s", image_path, strerror(errno));
        //pthread_mutex_unlock(&fdc.lock);
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    drv->image_size = (uint32_t)st.st_size;
    drv->fd         = fd;

    /* Read geometry from BPB in boot sector */
    uint8_t boot[512];
    bool bpb_ok = false;
    
    if (pread(fd, boot, 512, 0) == 512) {
        uint16_t bps   = (uint16_t)(boot[0x0B] | (boot[0x0C] << 8));
        uint16_t spt   = (uint16_t)(boot[0x18] | (boot[0x19] << 8));
        uint16_t sides = (uint16_t)(boot[0x1A] | (boot[0x1B] << 8));
        uint16_t total = (uint16_t)(boot[0x13] | (boot[0x14] << 8));

        if (bps == 512 && spt >= 1 && spt <= 18 &&
            sides >= 1 && sides <= 2 && total > 0) {
            drv->sectors_per_track = spt;
            drv->num_tracks        = total / (spt * sides);
            drv->num_sides         = sides;
            drv->type = (spt > 9) ? FDD_TYPE_HD : FDD_TYPE_DD;
            bpb_ok = true;

            /* Scan for actual FAT location - some images have extra boot
            * sector copies before the FAT despite BPB saying res=1 */
            uint16_t bpb_res = (uint16_t)(boot[0x0E] | (boot[0x0F] << 8));
            drv->reserved_sectors = bpb_res;
            uint8_t scan[4];
            for (int s = 1; s < spt * 2; s++) {
                
                if (pread(fd, scan, 4, (off_t)s * 512) == 4) {
                    //fprintf (stderr, "[SXB] s = %d, bpb_res = %d, scan[1] 0x%X, scan[2] 0x%X, scan[0] 0x%X\n", 
                    //        s, bpb_res, scan[1], scan[2], scan[0]);
                    if (scan[1] == 0xFF && scan[2] == 0xFF &&
                        scan[0] >= 0xF8) {
                        if (s != bpb_res) {
                            FDD_LOG("Drive %c: FAT at sector %d (BPB says %d), adjusting",
                                    'A' + drive, s + 1, bpb_res + 1);
                            drv->reserved_sectors = s;
                        }
                        break;
                    }
                }
            }

            FDD_INFO("Drive %c: BPB geometry: %d tracks %d spt %d sides",
                    'A' + drive, drv->num_tracks, spt, sides);
        }
    }

    if (!bpb_ok) {
        /* Fallback: detect from file size */
        if (drv->image_size <= FDD_IMAGE_SIZE_SS) {
            drv->type              = FDD_TYPE_DD;
            drv->sectors_per_track = FDD_DD_SECTORS;
            drv->num_sides         = 1;
            drv->num_tracks        = drv->image_size / (FDD_SECTOR_SIZE * drv->sectors_per_track);
            drv->reserved_sectors  = 1;
        } else if (drv->image_size <= FDD_IMAGE_SIZE_DD) {
            drv->type              = FDD_TYPE_DD;
            drv->sectors_per_track = FDD_DD_SECTORS;
            drv->num_sides         = 2;
            drv->num_tracks        = FDD_TRACKS_80;
            drv->reserved_sectors  = 1;
        } else {
            drv->type              = FDD_TYPE_HD;
            drv->sectors_per_track = FDD_HD_SECTORS;
            drv->num_sides         = 2;
            drv->num_tracks        = FDD_TRACKS_80;
        }
        FDD_LOG("Drive %c: size-based geometry: %d tracks %d spt %d sides",
                'A' + drive, drv->num_tracks, drv->sectors_per_track, drv->num_sides);
    }

    drv->write_protected = write_protect;
    drv->disk_inserted   = true;
    drv->media_changed   = false;
    strncpy(drv->image_path, image_path, sizeof(drv->image_path) - 1);

    FDD_LOG("Drive %c: mounted '%s' (%s, %d tracks, %d spt, %d sides, %s)",
            'A' + drive, image_path,
            drv->type == FDD_TYPE_HD ? "HD" : "DD",
            drv->num_tracks, drv->sectors_per_track, drv->num_sides,
            write_protect ? "WP" : "RW");

    //pthread_mutex_unlock(&fdc.lock);
    return 0;
}

void fdd_eject_disk(int drive)
{
    if (drive < 0 || drive >= FDD_MAX_DRIVES) return;
   // pthread_mutex_lock(&fdc.lock);
    fdd_drive_t *drv = &fdc.drives[drive];
    if (drv->fd >= 0) { close(drv->fd); drv->fd = -1; }
    drv->disk_inserted  = false;
    drv->media_changed  = true;
    drv->write_protected = false;
    drv->num_sides = 2;
    FDD_LOG("Drive %c: ejected", 'A' + drive);
    //pthread_mutex_unlock(&fdc.lock);
}

/* Runtime eject / re-insert toggle (F11 on the USB keyboard). Spectre
 * asks for the boot floppy to be removed before switching to Mac mode
 * and waits for the WP-sensor change; this is the only way to oblige an
 * emulated drive. Pressing again re-mounts the same image. */
void fdd_toggle_disk(int drive)
{
    static bool last_wp[FDD_MAX_DRIVES];

    if (drive < 0 || drive >= FDD_MAX_DRIVES) return;
    fdd_drive_t *drv = &fdc.drives[drive];

    if (drv->disk_inserted) {
        last_wp[drive] = drv->write_protected;
        fdd_eject_disk(drive);
        printf("[FDD] F11: drive %c ejected (F11 re-inserts)\n", 'A' + drive);
    } else if (drv->image_path[0]) {
        char path[sizeof(drv->image_path)];
        strncpy(path, drv->image_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        if (fdd_insert_disk(drive, path, last_wp[drive]) == 0)
            printf("[FDD] F11: drive %c re-inserted '%s'\n", 'A' + drive, path);
        else
            printf("[FDD] F11: drive %c re-insert of '%s' FAILED\n",
                   'A' + drive, path);
    }
}

/*
 * Report a drive to anything outside this file. Everything interesting is
 * inside the file-static fdc, so before this there was no way for the
 * settings NatFeat to say what is in drive A: - fdd_status() is exported
 * but every fprintf in it is commented out.
 *
 * Returns 1 with a disk in, 0 empty, -1 for a bad index. busy is the
 * FDC's own BUSY bit: transfers here are synchronous on the CPU thread,
 * so it is essentially never set when asked from elsewhere, but a swap
 * refuses on it rather than assuming.
 */
int fdd_query(int drive, char *path, int pathlen, int *wp, int *busy)
{
    fdd_drive_t *d;

    if (drive < 0 || drive >= FDD_MAX_DRIVES)
        return -1;
    d = &fdc.drives[drive];
    if (wp)
        *wp = d->write_protected ? 1 : 0;
    if (busy)
        *busy = (fdc.status & FDC_STATUS_BUSY) ? 1 : 0;
    if (path && pathlen > 0) {
        strncpy(path, d->disk_inserted ? d->image_path : "", (size_t)pathlen - 1);
        path[pathlen - 1] = '\0';
    }
    return d->disk_inserted ? 1 : 0;
}

/* Arm the write-protect flicker described in fdc_read_addr. Call after
 * any host-side insert or eject. */
void fdd_pulse_media(int drive)
{
    if (drive < 0 || drive >= FDD_MAX_DRIVES)
        return;
    fdc.drives[drive].media_changed = true;
}

void fdd_set_write_protect(int drive, bool wp)
{
    if (drive < 0 || drive >= FDD_MAX_DRIVES) return;
   // pthread_mutex_lock(&fdc.lock);
    fdc.drives[drive].write_protected = wp;
    //pthread_mutex_unlock(&fdc.lock);
}

bool fdd_irq_active(void)
{
    if (!fdc.irq_pending)
        return false;
    if (!fdc.irq_ready) {
        fdc.irq_ready = true;
        return false;
    }
    return true;
}

uint8_t fdd_gpip(uint8_t other_gpip)
{
    /* GPIP bit 5 is the SHARED disk interrupt: WD1772 INTRQ and the ACSI
     * port's /IRQ are combined onto it. The emulated FDC may only ADD its
     * own assertion (pull the bit low); it must never force the bit high,
     * because that overwrites what a real ACSI device is signalling and its
     * driver then never sees command completion - which broke ACSI boot.
     * When our FDC is idle, the real line passes through untouched (the
     * real WD1772 receives no commands from us, so its INTRQ stays quiet;
     * anything else on the line is real and belongs to the guest). */
    if (acsi_irq_active()) {
        hdc_trace_gpip(1, 'A');
        return other_gpip & ~(1u << 5);   /* emulated ACSI target asserts */
    }

    if (fdc.selected_drive >= 0 &&
        fdc.drives[fdc.selected_drive].disk_inserted &&
        fdd_irq_active()) {
        hdc_trace_gpip(1, (other_gpip & (1u << 5)) ? 'F' : 'R');
        return other_gpip & ~(1u << 5);   /* our FDC asserts */
    }

    hdc_trace_gpip(!(other_gpip & (1u << 5)), 'R');
    if (!FDD_enabled)
        rfdc_gpip(other_gpip);            /* real FDC finished a read? */
    return other_gpip;                    /* real line as-is (ACSI visible) */
}

void fdd_vbl(void)
{
    //pthread_mutex_lock(&fdc.lock);

    for (int i = 0; i < FDD_MAX_DRIVES; i++) {
        if (motor_ticks[i] > 0) {
            if (--motor_ticks[i] == 0) {
                if (fdc.selected_drive == i)
                    fdc.status &= ~FDC_STATUS_MOTORON;

                FDD_DBG("Drive %c: motor off", 'A' + i);
            }
        }
    }

   // pthread_mutex_unlock(&fdc.lock);
}

void fdd_status(void)
{
   // pthread_mutex_lock(&fdc.lock);
    //fprintf(stderr, "[FDD] status=0x%02X track=%d sector=%d data=0x%02X\n",
    //        fdc.status, fdc.track_reg, fdc.sector_reg, fdc.data_reg);
    //fprintf(stderr, "[FDD] dma_mode=0x%04X dma_addr=0x%06X scount=%d dstat=0x%02X\n",
    //        fdc.dma_mode, fdc.dma_addr, fdc.dma_sector_count, fdc.dma_status);
    for (int i = 0; i < FDD_MAX_DRIVES; i++) {
        //fprintf(stderr, "[FDD] Drive %c: %s track=%d%s%s\n",
        //        'A' + i,
        //        d->disk_inserted ? d->image_path : "(no disk)",
        //        d->current_track,
        //        d->write_protected ? " WP" : "",
        //        d->media_changed   ? " MC" : "");
    }
   // pthread_mutex_unlock(&fdc.lock);
}

/* =========================================================================
 * Main memory dispatch
 *
 * FDC / HDC (ACSI) ROUTING. The $FF8600-$FF860F window is the DMA
 * controller, and it is SHARED: the WD1772 floppy controller and the ACSI
 * hard-disk port both sit behind it. On real hardware the GLUE routes the
 * $FF8604 data port between them with DMA mode register bit 3 (0 = FDC,
 * 1 = HDC: floppy modes are $80-$86/$90, ACSI modes $88/$8A). Before this
 * existed, the emulation claimed the whole window for
 * the emulated FDC, which made ACSI unreachable: TOS's boot-time ACSI scan
 * landed in the emulated floppy chip (ACSI command bytes even executed as
 * WD1772 commands), so booting from a real ACSI device was impossible with
 * fdd enabled.
 *
 * So route exactly as the GLUE does:
 *   - Bit 3 selects which chip answers at $8604 ONLY when bit 4 (SCREG) is
 *     clear. With SCREG set, $8604 is the DMA chip's own sector-count
 *     register REGARDLESS of bit 3 - drivers write the count for ACSI
 *     transfers with mode $90/$190, bit 3 clear, exactly as for floppy.
 *     (The first routing cut keyed count writes off bit 3 and sent ACSI
 *     counts into the emulated floppy; the real DMA was never armed and
 *     every ACSI data phase transferred nothing.)
 *   - $8604 with SCREG clear: bit 3 = 1 -> real bus (ACSI command/status
 *     bytes), bit 3 = 0 -> emulated FDC, exactly as before.
 *   - The count is SHARED plumbing like the mode and address registers:
 *     written to both sides. Which side a READ of the shared registers
 *     (count, $8606 status, address counters) should come from cannot be
 *     decided by mode bits - drivers use bit3=0 modes for those reads after
 *     ACSI transfers too - so ownership is tracked by the last command
 *     activity: an ACSI command byte marks the DMA as HDC-owned, an
 *     emulated FDC command takes it back.
 *   - Offsets not modelled here ($8600/02, STE density at $860E, ...) pass
 *     through to the real bus instead of being swallowed.
 * ========================================================================= */



static int hdc_selected(void)
{
    return (fdc.dma_mode & DMA_MODE_FDC_HDC) != 0;
}

static uint32_t bus_read(uint32_t addr, int size)
{
    return (size == 2) ? ps_read_16(addr) : ps_read_8(addr);
}

static void bus_write(uint32_t addr, uint32_t val, int size)
{
    if (size == 2)
        ps_write_16(addr, (uint16_t)val);
    else
        ps_write_8(addr, (uint8_t)val);
}

/* ---- HDC (ACSI) mirror coherence ---------------------------------------
 * A real ACSI transfer is real-bus DMA: the DMA chip moves data between the
 * device and REAL ST-RAM. The emulated CPU, however, reads ST-RAM from the
 * natmem mirror, and its writes only conditionally reach the real bus. So
 * both directions need a sync or the guest and the device each see stale
 * data:
 *   device -> RAM: after the transfer, pull the window from the real bus
 *     into the mirror. Trigger: the guest's read of the ACSI status byte at
 *     $8604 - the mandatory end-of-command handshake every driver performs
 *     (it releases the device IRQ), and always before the data is touched.
 *   RAM -> device: before the transfer, push the window from the mirror to
 *     the real bus (write-through only covers low RAM and the screen).
 *     Trigger: the HDC sector-count write, which drivers program after
 *     filling the buffer and before sending the command block.
 * The window is known because we latch the DMA base and the HDC sector
 * count as the guest programs them. */

#define HDC_MAX_SYNC_SECTORS 256u        /* 128 KB bound per sync pass */

extern unsigned int pistorm_stram_phys_top(void);   /* pistorm_natmem.cpp */

/* The window a real bus master may be synced through. The guest's model
 * is a flat 4MB; the board may have far less, and above what it has
 * there is nothing to read. Pulling that nothing into the natmem mirror
 * overwrites memory the guest is using - vectors and sysvars included,
 * which is an illegal instruction a moment later. */
static int hdc_window_ok(uint32_t base, uint32_t len)
{
    uint32_t top = pistorm_stram_phys_top();

    if (!len || base >= 0x400000u || base + len > 0x400000u)
        return 0;
    if (top && base + len > top) {
        static int shown;
        if (shown < 8) {
            shown++;
            FDD_LOG("DMA window 0x%06X+%u is past the board's %uK of real "
                    "RAM - not syncing (the transfer itself cannot have "
                    "landed either)", base, len, top >> 10);
        }
        return 0;
    }
    return 1;
}

/* A driver's sector count is a CEILING (HDDriver and TOS program 255 for
 * a 1-sector READ(6)), so count*512 can run past the end of ST-RAM when
 * the buffer sits near the top. hdc_window_ok() then refused the WHOLE
 * window and nothing was synced: field case 21 Oct, HDDriver under TOS
 * 2.06 reads its FAT sectors into $3F0000; $3F0000 + 255*512 = $40FE00,
 * so the FAT never reached the guest, the cluster chain returned the
 * same cluster for ever (LBA 524628 x N into consecutive buffers, drive
 * LED flashing). Clamp to the end of real RAM instead; 0 = base itself
 * is past it. */
static uint32_t hdc_clamp(uint32_t base, uint32_t len)
{
    uint32_t end = 0x400000u;
    uint32_t top = pistorm_stram_phys_top();

    if (top && top < end)
        end = top;
    if (base >= end)
        return 0;
    return len > end - base ? end - base : len;
}

/* The DMA chip's address counter ($FF8609/0B/0D, real bus). */
static uint32_t hdc_dma_counter(void)
{
    return ((uint32_t)ps_read_8(0xFF8609u) << 16) |
           ((uint32_t)ps_read_8(0xFF860Bu) << 8)  |
            (uint32_t)ps_read_8(0xFF860Du);
}

/* Let the DMA chip finish writing RAM before the mirror is pulled.
 *
 * The device raises /IRQ (status phase) the moment its LAST byte has gone
 * into the DMA chip's FIFO. The chip then still has to win the bus and
 * flush that 16-byte chunk to RAM - a few microseconds. A 68000 driver
 * never notices: it reads the status byte and only touches the buffer much
 * later. We pull the window into the mirror AT the status read, so without
 * this the last chunk is taken stale - field trace: "DMA moved 496 B" on
 * every 512-byte read, i.e. the boot sector's $55AA signature missing, and
 * EmuTOS showing the partition as an empty drive.
 *
 * Done = the counter reaches base+len, or stops moving for ~15 us (a
 * transfer shorter than the window - INQUIRY, READ CAPACITY - never
 * reaches the end, and its tail under 16 bytes stays in the FIFO for good;
 * the ST cannot move it either). Bounded at 2 ms. */
static void hdc_wait_dma_done(uint32_t base, uint32_t len)
{
    uint64_t t0    = ht_now_us();
    uint32_t end   = base + len;
    uint32_t prev  = hdc_dma_counter();
    uint32_t first = prev;
    int      still = 0;

    while (prev != end) {
        uint64_t t = ht_now_us();
        while (ht_now_us() - t < 5u)
            ;                                   /* ~5 us between samples */
        uint32_t now = hdc_dma_counter();
        if (now == prev) {
            if (++still >= 3)
                break;                          /* quiet ~15 us: done   */
        } else
            still = 0;
        prev = now;
        if (ht_now_us() - t0 > 2000u) {
            static int shown;
            if (shown < 8) {
                shown++;
                FDD_LOG("HDC pull: DMA counter still moving after 2 ms "
                        "(0x%06X -> 0x%06X, want 0x%06X) - pulling anyway",
                        first, prev, end);
            }
            break;
        }
    }
    if (HT_ON() && ht_lines < HT_MAX) {
        ht_lines++;
        FDD_LOG("HDC pull waited %lluus: counter +%d -> +%d of %u",
                (unsigned long long)(ht_now_us() - t0),
                (int)(first - base), (int)(prev - base), len);
    }
}

/* PISTORM_HDC_TRACE: decode the first sector of each pull the way a
 * driver would - MBR partition table for LBA 0, BPB for anything that looks
 * like a FAT boot sector - so "is the disk really empty, or did the sector
 * arrive damaged" can be answered from the log. */
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

static void hdc_trace_sector(const uint8_t *b)
{
    /* sectors only: READ(6) $08 / WRITE(6) $0A or ICD-wrapped READ(10)
     * $1F $28 / WRITE(10) $1F $2A. INQUIRY, REQUEST SENSE etc. are pulled
     * too but are not disk sectors. */
    int icd = (ht.cdb[0] & 0x1F) == 0x1F;
    uint8_t op6 = ht.cdb[0] & 0x1F;
    if (!((!icd && (op6 == 0x08 || op6 == 0x0A)) ||
          (icd && (ht.cdb[1] == 0x28 || ht.cdb[1] == 0x2A))))
        return;
    if (ht_lines >= HT_MAX)
        return;
    ht_lines++;

    /* LBA and target from the CDB the trace saw (6-byte, or ICD $1F + 10) */
    uint32_t lba = 0, tgt = ht.cdb[0] >> 5;
    if (icd && ht.n >= 7)
        lba = ((uint32_t)ht.cdb[3] << 24) | ((uint32_t)ht.cdb[4] << 16) |
              ((uint32_t)ht.cdb[5] << 8) | ht.cdb[6];
    else if (ht.n >= 4)
        lba = ((uint32_t)(ht.cdb[1] & 0x1F) << 16) |
              ((uint32_t)ht.cdb[2] << 8) | ht.cdb[3];

    FDD_LOG("HDC sector t%u LBA %u: %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X  sig %02X%02X",
            tgt, lba, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
            b[510], b[511]);

    /* the partitioning sectors in full: LBA 0 (MBR / Atari root sector),
     * LBA 1, and the top of the 21-bit READ(6) range that HDDRUTIL reads
     * and writes - so every byte a partitioner looks at is in the log */
    if (lba <= 1 || lba >= 0x1FFFFFu)
        for (int r = 0; r < 512; r += 16)
            FDD_LOG("HDC   %03X: %02X %02X %02X %02X %02X %02X %02X %02X  "
                    "%02X %02X %02X %02X %02X %02X %02X %02X", r,
                    b[r], b[r + 1], b[r + 2], b[r + 3], b[r + 4], b[r + 5],
                    b[r + 6], b[r + 7], b[r + 8], b[r + 9], b[r + 10],
                    b[r + 11], b[r + 12], b[r + 13], b[r + 14], b[r + 15]);

    if (lba == 0 && b[510] == 0x55 && b[511] == 0xAA)
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = b + 0x1BE + 16 * i;
            if (e[4])
                FDD_LOG("HDC   MBR part %d: type %02X boot %02X start %u "
                        "size %u", i, e[4], e[0], le32(e + 8), le32(e + 12));
        }

    /* BPB: 512-byte sectors and a sane cluster size = boot sector */
    uint32_t bps = le16(b + 0x0B), spc = b[0x0D];
    if (bps && !(bps & 0x1FF) && spc && !(spc & (spc - 1))) {
        uint32_t tot = le16(b + 0x13) ? le16(b + 0x13) : le32(b + 0x20);
        FDD_LOG("HDC   BPB: bps %u spc %u res %u fats %u rootents %u "
                "total %u fatsz %u media %02X  oem '%.8s'",
                bps, spc, le16(b + 0x0E), b[0x10], le16(b + 0x11),
                tot, le16(b + 0x16), b[0x15], (const char *)b + 3);
    }
}

static void hdc_sync_pull(void)
{
    fdc.hdc_pending = false;
    uint32_t want = hdc_clamp(fdc.hdc_base, (uint32_t)fdc.hdc_count * 512u);
    if (!want) {
        hdc_window_ok(fdc.hdc_base, (uint32_t)fdc.hdc_count * 512u);   /* logs */
        return;
    }
    if (!hdc_window_ok(fdc.hdc_base, want))
        return;
    hdc_wait_dma_done(fdc.hdc_base, want);

    /* Pull only what the DMA chip actually wrote - the counter's travel -
     * not the whole sector-count window. Drivers routinely program the
     * count as a ceiling: TOS 3.06 sets 255 for a 1-sector READ(6), the
     * device stops after 512 bytes, and the rest of the window in REAL RAM
     * is whatever was there before. Low ST-RAM is mirror-only (CPU writes
     * never reach the real chips below $10000), so copying 130560 stale
     * bytes from $1654 back over the mirror wiped TOS's live variables -
     * field case 17 Oct: the screen size at $2908-$2917 became text, the
     * desktop's screen clear ran 31 MB, and TOS 3.06 crashed. rfdc_pull()
     * (real floppy) has always limited itself the same way. */
    uint32_t moved = (hdc_dma_counter() - fdc.hdc_base) & 0xFFFFFFu;
    uint32_t len = moved < want ? moved : want;

    uint8_t buf[512];
    for (uint32_t off = 0; off < len; off += 512u) {
        uint32_t n = len - off < 512u ? len - off : 512u;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = ps_read_8(fdc.hdc_base + off + i);
        pistorm_dma_to_stram(fdc.hdc_base + off, buf, n);

        if (HT_ON() && off == 0 && n == 512u)
            hdc_trace_sector(buf);
    }
    FDD_DBG("HDC sync pull: %u sectors @ 0x%06X", fdc.hdc_count, fdc.hdc_base);

    /* PISTORM_HDC_TRACE: fingerprint what was pulled, and read the real
     * window a second time to catch reads that do not repeat. A boot that
     * works and one that crashes can then be compared transfer by transfer:
     * same sum = same data reached the guest. */
    if (HT_ON() && ht_lines < HT_MAX) {
        uint32_t sum = 2166136261u, diff = 0, first = 0xFFFFFFFFu;
        uint8_t  v[512];
        for (uint32_t off = 0; off < len; off += 512u) {
            uint32_t n = len - off < 512u ? len - off : 512u;
            pistorm_dma_from_stram(fdc.hdc_base + off, v, n);  /* mirror */
            for (uint32_t i = 0; i < n; i++) {
                sum = (sum ^ v[i]) * 16777619u;
                if (ps_read_8(fdc.hdc_base + off + i) != v[i]) {
                    if (first == 0xFFFFFFFFu)
                        first = off + i;
                    diff++;
                }
            }
        }
        ht_lines++;
        if (diff)
            FDD_LOG("HDC pull check: %u B @0x%06X sum %08X  REREAD DIFFERS in "
                    "%u bytes (first at +%u)", len, fdc.hdc_base, sum, diff, first);
        else
            FDD_LOG("HDC pull check: %u B @0x%06X sum %08X  reread ok "
                    "(window %u B)", len, fdc.hdc_base, sum, want);

        /* Short transfers (INQUIRY, MODE SENSE, REQUEST SENSE): dump the first
         * 64 bytes of REAL RAM at the base, so "DMA moved 48 B" against a
         * 16-byte allocation length can be read for what it is - 48 bytes of
         * device data (the device ignored the allocation length), or 16 bytes
         * of data and 32 untouched (the DMA counter ran on, FIFO granularity,
         * RAM beyond +16 is what was there before). 19 Oct 2026: HDDriver's
         * INQUIRY buffers sit on its stack and the return address 24 bytes
         * past the buffer was overwritten with code bytes. */
        if (len && len <= 64u) {
            uint8_t w[64];
            char hex[64 * 3 + 1];
            uint32_t k = 0;
            for (uint32_t i = 0; i < 64u; i++) {
                w[i] = ps_read_8(fdc.hdc_base + i);
                k += (uint32_t)snprintf(hex + k, sizeof hex - k, "%02X%s", w[i],
                                        (i & 15) == 15 ? " " : "");
            }
            FDD_LOG("HDC real RAM @0x%06X +0..63: %s", fdc.hdc_base, hex);
        }
    }
}

/* Copy [base, base+count*512) from the emulator's RAM image to real ST RAM.
 * Byte writes, as before - same bus path the push always used. */
static void hdc_push_window(uint32_t base, uint32_t count)
{
    uint32_t len = hdc_clamp(base, count * 512u);
    if (!len) {
        hdc_window_ok(base, count * 512u);   /* logs "past real RAM" */
        return;
    }
    if (!hdc_window_ok(base, len))
        return;
    uint8_t buf[512];
    for (uint32_t off = 0; off < len; off += 512u) {
        uint32_t n = len - off < 512u ? len - off : 512u;
        pistorm_dma_from_stram(base + off, buf, n);
        for (uint32_t i = 0; i < n; i++)
            ps_write_8(base + off + i, buf[i]);
    }
    FDD_DBG("HDC sync push: %u sectors @ 0x%06X", count, base);
}

/* The window pushed early, at the sector-count write (see
 * hdc_note_sector_count) - the push at the first command byte skips it. */
static uint32_t hdc_prepush_base, hdc_prepush_count;
static int      hdc_prepushed;

static void hdc_sync_push(void)
{
    if (hdc_prepushed && hdc_prepush_base == fdc.hdc_base &&
        hdc_prepush_count >= fdc.hdc_count) {
        hdc_prepushed = 0;
        return;                 /* already on the real bus, before the DMA
                                   chip could fetch any of it */
    }
    hdc_prepushed = 0;
    hdc_push_window(fdc.hdc_base, fdc.hdc_count);
}

/* =========================================================================
 * Real FDC passthrough (no "fdd" lines): a real drive or Gotek on the real
 * WD1772. Same coherence problem as real ACSI - the DMA chip writes REAL
 * RAM, the guest reads the emulator's image - so a read-type command arms
 * a window and the image is pulled when the command has completed.
 *
 * "Completed" = the first of:
 *   - the guest reads the FDC status register and BUSY is clear (TOS does
 *     this after every command, and an interrupt handler always does), or
 *   - the guest polls GPIP and sees GPIP5 (INTRQ) low.
 * Either is only believed RFDC_SETTLE_US after the command was written:
 * the WD1772 needs a few microseconds to raise BUSY / drop INTRQ, and the
 * Pi can read faster than that.
 *
 * Writes need no completion step: the buffer goes to real RAM at the
 * sector-count write (hdc_note_sector_count, write direction), before the
 * DMA chip can prefetch it; rfdc_command repeats it if the count came
 * first and the base was moved afterwards.
 * ========================================================================= */
#define RFDC_SETTLE_US 200u

static struct {
    int      armed;         /* read-type command issued, not pulled yet */
    uint32_t base;          /* DMA base at the command write            */
    uint32_t count;         /* sector count in force (0 = not given)    */
    uint8_t  cmd;
    uint64_t t_cmd;
} rfdc;

static unsigned rfdc_logged;

static void rfdc_pull(const char *why)
{
    rfdc.armed = 0;

    uint32_t want = rfdc.count * 512u;
    hdc_wait_dma_done(rfdc.base, want);
    uint32_t moved = (hdc_dma_counter() - rfdc.base) & 0xFFFFFFu;

    /* What actually reached RAM is what the counter passed; a FIFO tail
     * under 16 bytes never leaves the chip (the ST cannot see it either). */
    uint32_t len = moved;
    if (want && len > want)
        len = want;
    if (len > HDC_MAX_SYNC_SECTORS * 512u)
        len = HDC_MAX_SYNC_SECTORS * 512u;
    if (!hdc_window_ok(rfdc.base, len))
        return;

    uint8_t buf[512];
    for (uint32_t off = 0; off < len; off += 512u) {
        uint32_t n = len - off < 512u ? len - off : 512u;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = ps_read_8(rfdc.base + off + i);
        pistorm_dma_to_stram(rfdc.base + off, buf, n);
    }

    if (hdc_trace_on() && rfdc_logged < 200u) {
        rfdc_logged++;
        FDD_LOG("FDC real cmd $%02X: pulled %u B @0x%06X (count %u, DMA moved "
                "%u) at %s", rfdc.cmd, len, rfdc.base, rfdc.count, moved, why);
    }
}

static int rfdc_settled(void)
{
    return ht_now_us() - rfdc.t_cmd >= RFDC_SETTLE_US;
}

/* A command for the real WD1772 ($8604, FDC selected, A1:A0 = 00). */
static void rfdc_command(uint8_t cmd)
{
    uint8_t t = cmd & 0xF0u;

    hdc_count_fresh = 0;            /* the count was the floppy's */
    fdc.hdc_pending = false;

    if (t == FDC_CMD_FORCE_INT)
        return;                     /* aborts the command in progress: keep
                                       its window, the next completion
                                       pulls whatever did arrive */

    rfdc.armed = 0;
    if (t == 0x80u || t == 0x90u || t == FDC_CMD_READ_ADDR ||
        t == FDC_CMD_READ_TRACK) {
        rfdc.armed = 1;
        rfdc.base  = fdc.dma_base_addr;
        rfdc.count = fdc.hdc_count;
        rfdc.cmd   = cmd;
        rfdc.t_cmd = ht_now_us();
    } else if (t == FDC_CMD_WRITE_SEC || t == 0xB0u ||
               t == FDC_CMD_WRITE_TRACK) {
        if (!(hdc_prepushed && hdc_prepush_base == fdc.dma_base_addr &&
              hdc_prepush_count >= fdc.hdc_count))
            hdc_push_window(fdc.dma_base_addr, fdc.hdc_count);
    }
    hdc_prepushed = 0;
}

/* Guest read of a real FDC register: status (A1:A0 = 00) with BUSY clear
 * completes an armed read. */
static void rfdc_status_read(uint32_t val)
{
    if (rfdc.armed && !(fdc.dma_mode & (DMA_MODE_A0 | DMA_MODE_A1)) &&
        !(val & FDC_STATUS_BUSY) && rfdc_settled())
        rfdc_pull("status read");
}

/* Guest GPIP poll: INTRQ (GPIP5) low completes an armed read. */
static void rfdc_gpip(uint8_t gpip)
{
    if (rfdc.armed && !(gpip & (1u << 5)) && rfdc_settled())
        rfdc_pull("GPIP5 low");
}

/* Sector-count write seen ($8604, SCREG set - mode bits cannot say whether
 * it is for the floppy or for ACSI). Stash it; if ACSI command bytes follow,
 * hdc_claim_count() turns it into a transfer window. */
static void hdc_note_sector_count(uint32_t val)
{
    uint16_t n = (uint16_t)(val & 0xFFu);
    if (n > HDC_MAX_SYNC_SECTORS) {
        FDD_LOG("HDC transfer of %u sectors exceeds sync bound (%u) - "
                "mirror sync capped", n, HDC_MAX_SYNC_SECTORS);
        n = HDC_MAX_SYNC_SECTORS;
    }
    fdc.hdc_count   = n;
    hdc_count_fresh = 1;

    /* WRITE direction: push the buffer to real RAM NOW, before the count
     * reaches the real DMA chip. In write direction the chip starts filling
     * its 32-byte FIFO from real RAM the moment it has a count - before the
     * first command byte, which is where the push used to happen. With
     * stram_cache on (CPU writes stay in the emulator's image) the first 32
     * bytes of a sector then went to disk stale: field trace showed GEMDOS
     * FAT/dir buffers differing at +0, i.e. corrupted FAT and directory
     * sectors. We cannot tell yet whether this count is for the HDC or the
     * floppy; pushing for a floppy write is harmless (the emulated FDC
     * reads the image anyway). */
    hdc_prepushed = 0;
    if ((fdc.dma_mode & DMA_MODE_RW) && n) {
        hdc_push_window(fdc.dma_base_addr, n);
        hdc_prepush_base  = fdc.dma_base_addr;
        hdc_prepush_count = n;
        hdc_prepushed     = 1;
    }
}

/* First ACSI command byte after a count write: the count was for the HDC.
 * Snapshot the window HERE (the base could legally be programmed after the
 * count - only the final command byte starts the transfer) and prepare the
 * coherence sync for the direction in force. */
/* PISTORM_HDC_TRACE, write commands: before the push, how many bytes of
 * the window differ between the emulator's RAM image and real ST RAM. The
 * DMA chip reads REAL RAM, and in write direction it starts filling its
 * 32-byte FIFO as soon as the mode and count are programmed - before the
 * first command byte, i.e. before the push. A difference here in the first
 * 32 bytes reaches the disk whatever the push does afterwards. */
static void hdc_trace_write_window(void)
{
    if (!HT_ON() || ht_lines >= HT_MAX)
        return;
    uint32_t len = hdc_clamp(fdc.hdc_base, (uint32_t)fdc.hdc_count * 512u);
    if (!len || !hdc_window_ok(fdc.hdc_base, len))
        return;
    uint32_t diff = 0, first = 0xFFFFFFFFu;
    uint8_t  m[512];
    for (uint32_t off = 0; off < len; off += 512u) {
        uint32_t n = len - off < 512u ? len - off : 512u;
        pistorm_dma_from_stram(fdc.hdc_base + off, m, n);
        for (uint32_t i = 0; i < n; i++)
            if (ps_read_8(fdc.hdc_base + off + i) != m[i]) {
                if (first == 0xFFFFFFFFu)
                    first = off + i;
                diff++;
            }
    }
    ht_lines++;
    if (diff)
        FDD_LOG("HDC WRITE %u sector(s) @0x%06X mode %03X: image and real RAM "
                "DIFFER in %u bytes (first at +%u) before push",
                fdc.hdc_count, fdc.hdc_base, fdc.dma_mode, diff, first);
    else
        FDD_LOG("HDC WRITE %u sector(s) @0x%06X mode %03X: image == real RAM",
                fdc.hdc_count, fdc.hdc_base, fdc.dma_mode);
}

static void hdc_claim_count(bool first)
{
    hdc_active = 1;
    if (!hdc_count_fresh) {
        /* No sector-count write since the last command - the driver is
         * issuing another command under the count it already set, which
         * the DMA chip allows. Field case 22 Oct, HDDRUTIL scanning real
         * ACSI: INQUIRY (count written), then four 4-byte REQUEST SENSEs
         * with no count write. The chip's FIFO only goes to RAM in 16-byte
         * chunks, so the four together fill one chunk ("DMA moved 16 B"
         * on the third) - but nothing was pulled, the guest read a stale
         * sense buffer, and HDDRUTIL listed no devices on either ACSI
         * target (no READ CAPACITY ever sent).
         *
         * So on the FIRST byte of such a command, read direction, arm a
         * pull from where the chip's address counter is now. The pull
         * takes only what the counter moved (hdc_sync_pull), bounded by
         * the count in force, so a command that moves nothing pulls
         * nothing. Write direction is unchanged: its data went out at
         * the count write. */
        if (first && !(fdc.dma_mode & DMA_MODE_RW)) {
            fdc.hdc_base      = hdc_dma_counter();
            fdc.hdc_dir_write = false;
            if (!fdc.hdc_count)
                fdc.hdc_count = 1;
            fdc.hdc_pending   = true;
        }
        return;
    }
    hdc_count_fresh   = 0;
    fdc.hdc_base      = fdc.dma_base_addr;
    fdc.hdc_dir_write = (fdc.dma_mode & DMA_MODE_RW) != 0;
    if (fdc.hdc_dir_write)
        ht.wr = 1;
    if (HT_ON() && ht_lines < HT_MAX) {
        ht_lines++;
        /* opcode $0A = WRITE(6); a write opcode taken as "read" means the
         * driver set write direction AFTER the command bytes - no push */
        uint8_t op = ht.cdb[0] & 0x1F;
        FDD_LOG("HDC claim: %s, %u sector(s) @0x%06X, mode %03X at first "
                "command byte (opcode $%02X%s)",
                fdc.hdc_dir_write ? "WRITE (push)" : "read (pull)",
                fdc.hdc_count, fdc.hdc_base, fdc.dma_mode, op,
                (op == 0x0A && !fdc.hdc_dir_write) ? " - WRITE OPCODE BUT READ "
                "DIRECTION: no push" : "");
    }
    if (fdc.hdc_dir_write) {
        hdc_trace_write_window();
        hdc_sync_push();                 /* buffer must be on the real bus
                                            before the device reads it */
        fdc.hdc_pending = false;
    } else {
        fdc.hdc_pending = (fdc.hdc_count != 0);   /* pull after completion */
    }
}

uint32_t fdd_io_read(uint32_t addr, int size)
{
    uint32_t val;

    if (acsi_dbg_on() && addr >= 0xFF8600u && addr <= 0xFF860Fu &&
        acsi_dbg_count < ACSI_DBG_LIMIT) {
        acsi_dbg_count++;
        FDD_LOG("dbg R %06X sz%d (mode %03X)", addr, size, fdc.dma_mode);
    }

    /* Handle longword reads as two word reads */
    if (size == 4) {
        uint32_t hi = fdd_io_read(addr,     2);
        uint32_t lo = fdd_io_read(addr + 2, 2);
        return (hi << 16) | lo;
    }

    //pthread_mutex_lock(&fdc.lock);

    if (addr >= 0xFF8600u && addr <= 0xFF860Fu) {
        if (addr == FDC_DATA_REG) {
            if (fdc.dma_mode & DMA_MODE_SCREG)
                /* DMA chip's own count register (bit 3 is not consulted by
                 * the GLUE here): whoever ran the DMA last answers */
                val = acsi_owns_dma() ? acsi_residual_count()
                    : hdc_active      ? bus_read(addr, size)
                                      : fdc_read_addr(addr, size);
            else if (hdc_selected()) {
                if (acsi_owns_dma())
                    val = acsi_status_read();  /* emulated status byte */
                else {
                    val = bus_read(addr, size);   /* ACSI status byte */
                    hdc_trace_status(val);
                    if (fdc.hdc_pending) {
                        /* end-of-command handshake: bring the mirror up
                         * to date - unless the device reports CHECK
                         * CONDITION / BUSY (status bits 1-4): no valid
                         * data then, and the driver will not use it */
                        if (val & 0x1Eu)
                            fdc.hdc_pending = false;
                        else
                            hdc_sync_pull();
                    }
                }
            } else if (FDD_enabled)
                val = fdc_read_addr(addr, size);
            else {
                val = bus_read(addr, size);   /* real WD1772 register */
                rfdc_status_read(val);
            }
        } else if (addr == DMA_MODE_REG ||
                   addr == DMA_BASE_HIGH || addr == DMA_BASE_MID ||
                   addr == DMA_BASE_LOW) {
            /* shared: DMA status and address counters follow ownership */
            val = hdc_active ? bus_read(addr, size)
                             : dma_read_addr(addr, size);
        } else
            val = dma_read_addr(addr, size);
    } else if (addr >= 0xFF8800u && addr <= 0xFF8803u) {
        val = psg_read_addr(addr, size);
    } else {
        val = 0xFFu;
    }

    //pthread_mutex_unlock(&fdc.lock);
    return val;
}

void fdd_io_write(uint32_t addr, uint32_t val, int size)
{
    if (acsi_dbg_on() && addr >= 0xFF8600u && addr <= 0xFF860Fu &&
        acsi_dbg_count < ACSI_DBG_LIMIT) {
        acsi_dbg_count++;
        FDD_LOG("dbg W %06X = %0*X sz%d (mode %03X)",
                addr, size * 2, val, size, fdc.dma_mode);
    }

    /* Handle longword writes as two word writes */
    if (size == 4) {
        fdd_io_write(addr,     (val >> 16) & 0xFFFF, 2);
        fdd_io_write(addr + 2, val & 0xFFFF,          2);
        return;
    }

    //pthread_mutex_lock(&fdc.lock);

    if (addr >= 0xFF8600u && addr <= 0xFF860Fu) {
        if (addr == DMA_MODE_REG ||
            addr == DMA_BASE_HIGH || addr == DMA_BASE_MID ||
            addr == DMA_BASE_LOW) {
            /* shared plumbing: latch locally AND program the real chip */
            if (addr == DMA_MODE_REG)
                hdc_trace_mode_write();
            dma_write_addr(addr, val, size);
            bus_write(addr, val, size);
        } else if (addr == FDC_DATA_REG) {
            if (fdc.dma_mode & DMA_MODE_SCREG) {
                /* sector count: the DMA chip's own register, shared by both
                 * devices and written with bit3=0 modes even for ACSI. Arm
                 * both sides; the next command decides whose it was. */
                hdc_note_sector_count(val);
                fdc_write_addr(addr, val, size);  /* local latch (floppy) */
                bus_write(addr, val, size);       /* real chip (ACSI) */
            } else if (hdc_selected()) {
                /* ACSI command byte. Emulated targets get first refusal;
                 * the mode register's A-bits mark the FIRST byte of a CDB
                 * (TOS writes mode $88 for it, $8A for the rest). */
                bool first = (fdc.dma_mode &
                              (DMA_MODE_A0 | DMA_MODE_A1)) == 0;
                if (acsi_cmd_byte((uint8_t)val, first)) {
                    /* consumed by an emulated target: the count that was
                     * programmed belongs to it, not to a real device */
                    hdc_count_fresh = 0;
                    fdc.hdc_pending = false;
                } else {
                    hdc_trace_cmd_byte((uint8_t)val, first, fdc.dma_base_addr);
                    hdc_claim_count(first);       /* real ACSI device  */
                    bus_write(addr, val, size);
                    /* Pause after each command byte on the real bus.
                     *
                     * 19 Oct 2026: with the floppy emulated, a cold-booted
                     * machine saw no ACSI drives - unless PISTORM_HDC_TRACE=1
                     * was on, whose only effect inside the CDB phase is the
                     * fprintf of the claim line between byte 1 and byte 2,
                     * tens of microseconds. A 68000 driver spaces CDB bytes
                     * by ~4-6 us of instruction time; the Pi delivers them
                     * as fast as the CPLD cycles. The RP2040 HDC polls for
                     * the CS edge ("12 us to assert IRQ") and misses a byte
                     * that follows the previous one too closely. Sweep on
                     * that controller: 0 fails, 2 works, 5 works. Default 5
                     * = real-68000 spacing with margin; ~30 us per command.
                     * PISTORM_ACSI_CDB_GAP_US=n overrides (0 = old timing).
                     *
                     * 21 Oct 2026: 5 us is not enough for TOS 2.06, whose
                     * code from "IRQ seen" to the next byte is shorter than
                     * 1.04's: the HDC (ACSI ID 1) never acked byte 1 and ID 0
                     * got a phantom ack. At 20 us TOS 2.06 read ID 1's LBA 0
                     * (status 00, 512 B). Default 20; ~120 us per 6-byte CDB. */
                    {
                        static int gap = -1;
                        if (gap < 0) {
                            const char *e = getenv("PISTORM_ACSI_CDB_GAP_US");
                            gap = e ? atoi(e) : 20;
                            if (gap < 0) gap = 0;
                            if (gap > 1000) gap = 1000;
                        }
                        if (gap) {
                            uint64_t t0 = ht_now_us();
                            while (ht_now_us() - t0 < (uint64_t)gap)
                                ;
                        }
                    }
                }
            } else if (!FDD_enabled) {
                /* floppy not emulated (ACSI-only config): FDC commands
                 * belong to a real drive on the real bus */
                acsi_release();
                hdc_active = 1;   /* real device owns the DMA regs now */
                if (!(fdc.dma_mode & (DMA_MODE_A0 | DMA_MODE_A1)))
                    rfdc_command((uint8_t)val);   /* command register */
                bus_write(addr, val, size);
            } else {
                /* emulated FDC command: the DMA is floppy's again */
                acsi_release();
                hdc_active = 0;
                hdc_count_fresh = 0;
                fdc.hdc_pending = false;
                fdc_write_addr(addr, val, size);
            }
        } else {
            /* unmodelled offset: the real chip may still care ($860E...) */
            bus_write(addr, val, size);
        }
    } else if (addr >= 0xFF8800u && addr <= 0xFF8803u) {
        psg_write_addr(addr, val, size);
    }

    //pthread_mutex_unlock(&fdc.lock);
}

/* =========================================================================
 * PSG Port A - drive and side selection
 *
 * Atari ST hardware wiring (from official schematics):
 *   PSG port A bit 0 (/SIDE1)   = 0 → side 1, = 1 → side 0
 *   PSG port A bit 1 (/DRIVE_A) = 0 → drive A selected
 *   PSG port A bit 2 (/DRIVE_B) = 0 → drive B selected
 * ========================================================================= */

/*
 * The floppy emulation needs exactly ONE thing from the PSG: port A's
 * drive and side select bits. It is not the chip's owner and must not
 * behave like one. Three rules, each of them a bug that was paid for:
 *
 * 1. EVERY write reaches the real chip, exactly as it does when no image
 *    is mounted. With no image, natmem's HW_PAGE_PSG case is
 *    ym2149_snoop* followed by hw_bus_*put - the real YM2149 is
 *    programmed. Swallowing the writes here (mixer, tone, envelope all
 *    dropped on the floor) is why `ym2149 disabled` gave no sound at all
 *    while a floppy image was mounted.
 *
 *    Gating the pass-through on ym2149_active() is NOT the fix. It
 *    leaves the real chip unprogrammed while rule 2 still reads FROM it,
 *    and a register read-modify-write - select 7, read the mixer, OR in
 *    a channel, write it back, which is what TOS and most players do -
 *    then folds a stale chip's bits into the emulated one. Wrong mixer
 *    bits are noise channels left open: that is the noise.
 *
 * 2. Reads come from the real chip, with our emulated drive-select bits
 *    laid back over port A. Returning $FF, as this used to, is a
 *    different answer from the one the identical read gets when no image
 *    is mounted.
 *
 * 3. Byte lane. The PSG sits on D8-D15, so a byte access carries the
 *    value and a WORD access carries it in the HIGH byte
 *    (move.w #$0E00,$FF8800). This took the low byte of both, so every
 *    word-form PSG write - a great deal of ST software - selected
 *    register 0 and wrote 0, and drive/side select was never decoded for
 *    those programs at all. Long writes arrive here already split into
 *    two words by fdd_io_write().
 *
 * Drive select: a drive whose disk we are serving from an image must not
 * also spin its real mechanism, so ITS select bit (and only its) is held
 * high on the way to the chip. A drive with no image is untouched, so a
 * real drive B still works beside an image in A. Port A's other bits -
 * RS232 RTS/DTR, printer strobe - always pass through.
 */

/* Select bits to hold high on the real chip: the drives we are serving. */
static uint8_t psg_served_bits(void)
{
    uint8_t m = 0;
    if (fdc.drives[0].disk_inserted) m |= PSG_DRIVE_A_SEL;
    if (fdc.drives[1].disk_inserted) m |= PSG_DRIVE_B_SEL;
    return m;
}

static uint32_t psg_read_addr(uint32_t addr, int size)
{
    uint32_t real = bus_read(addr, size);
    uint8_t  m    = psg_served_bits();

    if (addr == PSG_REG_SELECT && fdc.psg_reg_sel == PSG_PORT_A_REG && m) {
        uint8_t hi = (uint8_t)(size == 2 ? (real >> 8) : real);
        hi = (uint8_t)((hi & (uint8_t)~m) | (fdc.psg_porta & m));
        return size == 2 ? (((uint32_t)hi << 8) | (real & 0xFFu)) : hi;
    }
    return real;
}

static void psg_write_addr(uint32_t addr, uint32_t val, int size)
{
    uint8_t v = (uint8_t)(size == 2 ? (val >> 8) : val);

    if (addr == PSG_REG_SELECT) {
        fdc.psg_reg_sel = (uint8_t)(v & 0x0F);
    } else if (addr == PSG_REG_WRITE && fdc.psg_reg_sel == PSG_PORT_A_REG) {
        uint8_t m = psg_served_bits();
        fdc.psg_porta = v;
        fdc_decode_drive_side();
        v = (uint8_t)(v | m);              /* served drives stay deselected */
        val = (size == 2) ? (((uint32_t)v << 8) | (val & 0xFFu)) : v;
    }
    bus_write(addr, val, size);            /* always - see rule 1 above */
}

static void fdc_decode_drive_side(void)
{
    uint8_t p = fdc.psg_porta;

    /* bit 1 low = drive A selected, bit 0 low = drive B selected */
    bool drvA = !(p & PSG_DRIVE_A_SEL);
    bool drvB = !(p & PSG_DRIVE_B_SEL);

    /* bit 2: 1=side 0, 0=side 1 */
    fdc.selected_side = (p & PSG_SIDE_SEL) ? 0 : 1;

    //if      (drvA && !drvB) fdc.selected_drive = 0;
    //else if (!drvA && drvB) fdc.selected_drive = 1;
    //else                    fdc.selected_drive = -1;
    if (drvA)       fdc.selected_drive = 0;   /* drive A takes priority */
    else if (drvB)  fdc.selected_drive = 1;
    else            fdc.selected_drive = -1;

    /* Start motor */
    //if (fdc.selected_drive >= 0) {
    //    fprintf (stderr, "[SXB] Motor running\n");
    //    motor_ticks[fdc.selected_drive] = MOTOR_OFF_TICKS;
    //    fdc.status |= FDC_STATUS_MOTORON;
    //}

    FDD_DBG("PSG: drive=%d side=%d (porta=0x%02X)",
            fdc.selected_drive, fdc.selected_side, p);
}

/* =========================================================================
 * DMA Controller Registers ($FF8606, $FF8609, $FF860B, $FF860D)
 * ========================================================================= */

static uint32_t dma_read_addr(uint32_t addr, int size)
{
    (void)size;
    uint32_t val = 0;
    switch (addr) {
    case DMA_MODE_REG:
        /* DMA status register read */
        val = fdc.dma_status;
     //   fprintf(stderr, "[DMA] READ $FF8606 -> 0x%04X\n", val);
        break;
    case DMA_BASE_HIGH: val = (fdc.dma_addr >> 16) & dma_hi_mask(); break;
    case DMA_BASE_MID:  val = (fdc.dma_addr >>  8) & 0xFFu; break;
    case DMA_BASE_LOW:  val = (fdc.dma_addr      ) & 0xFFu; break;
    default:            val = 0xFFu;                          break;
    }
    return val;
}

static void dma_write_addr(uint32_t addr, uint32_t val, int size)
{
    uint16_t v16 = val & 0xFFFFu;
    uint8_t  v8  = val & 0xFFu;

    switch (addr) {
    case DMA_MODE_REG:
        FDD_DBG("DMA mode write: 0x%04X", v16);
        fdc.dma_mode = v16;
        break;

    /* The real DMA chip has ONE address counter: writing $8609/0B/0D
     * loads it directly, transfers advance it, reads return it. Keep
     * the programmed copy too (the floppy path re-latches from it at
     * transfer start - now a no-op, behaviour unchanged), but the live
     * counter must follow writes or an ACSI transfer that runs without
     * a floppy-style latch starts from a stale counter. */
    case DMA_BASE_HIGH:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0x00FFFFu) | ((uint32_t)(v8 & dma_hi_mask()) << 16);
        fdc.dma_addr      = (fdc.dma_addr      & 0x00FFFFu) | ((uint32_t)(v8 & dma_hi_mask()) << 16);
        FDD_DBG("DMA base addr: 0x%06X", fdc.dma_base_addr);
        break;
    case DMA_BASE_MID:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0xFF00FFu) | ((uint32_t)v8 << 8);
        fdc.dma_addr      = (fdc.dma_addr      & 0xFF00FFu) | ((uint32_t)v8 << 8);
        break;
    case DMA_BASE_LOW:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0xFFFF00u) | v8;
        fdc.dma_addr      = (fdc.dma_addr      & 0xFFFF00u) | v8;
        break;

    default:
        break;
    }
    (void)size;
}

/* =========================================================================
 * FDC Register Read/Write ($FF8604)
 *
 * DMA mode register determines which WD1772 register is accessed:
 *   bits [2:1] = A1:A0 → 0=cmd/status, 1=track, 2=sector, 3=data
 *   bit  [4]   = SCREG → sector count register (not FDC register)
 * ========================================================================= */

static int fdc_reg_index(void)
{
    return (fdc.dma_mode >> 1) & 0x03;
}

static uint32_t fdc_read_addr(uint32_t addr, int size)
{
    (void)addr; (void)size;
    uint32_t val = 0;

    if (fdc.dma_mode & DMA_MODE_SCREG) {
        /*
         * EmuTOS get_dma_status() reads $FF8604 in SCREG mode to get
         * the sector count. After a successful transfer this must be 0.
         * 0 = transfer complete (DMA decremented to zero).
         */
        val = fdc.dma_sector_count;
        FDD_DBG("FDC SCREG read (sector count) -> %d", val);
        return val;
    }

    switch (fdc_reg_index()) {
    case FDC_REG_CMD_STATUS:
        val = fdc.status;

        /* Type I status bit 6 mirrors the drive's WP sensor LIVE on a
         * real WD1772 - and with no disk in the drive the sensor reads
         * "protected". That live bit is the media-change signal TOS and
         * Spectre poll to notice a disk being removed; without it an
         * eject is invisible to the guest. Type II/III writes set the
         * bit explicitly at command time, so only Type I is composed
         * here ((cmd & 0x80) == 0). */
        if ((fdc.command & 0x80u) == 0 &&
            fdc.selected_drive >= 0 && fdc.selected_drive < FDD_MAX_DRIVES) {
            fdd_drive_t *sd = &fdc.drives[fdc.selected_drive];
            if (!sd->disk_inserted || sd->write_protected)
                val |= FDC_STATUS_WRTPROT;
            else
                val &= ~FDC_STATUS_WRTPROT;
            /*
             * Media change. The ST has no disk-change line: TOS decides a
             * disk was swapped when flopvbl sees the write-protect state
             * TOGGLE, and only then does GEMDOS Mediach() answer 2
             * ("definitely changed") instead of serving the old disk's
             * cached FAT and directory - which is what corrupts the image
             * on the first write after a swap.
             *
             * media_changed has existed in this struct since the start
             * and was read by nothing (set in fdd_insert_disk and
             * fdd_eject_disk, and grep found no consumer). This is that
             * consumer: the first Type I status read after a swap reports
             * the WP bit inverted, and the flag clears, so the next read
             * reports the truth. One toggle is what flopvbl is looking
             * for.
             *
             * UNVERIFIED ON HARDWARE. TOS 1.02 / 1.62 / 2.06 / EmuTOS all
             * differ slightly here and this is the one part of the floppy
             * work that cannot be reasoned to confidence from source. The
             * front end also calls Getbpb() and walks the root directory
             * after a swap, which is the belt to this braces.
             */
            if (sd->media_changed) {
                val ^= FDC_STATUS_WRTPROT;
                sd->media_changed = false;
            }
        }

        //fprintf(stderr, "[FDC] READ status -> 0x%02X\n", val);
        /* Reading status register clears the IRQ (WD1772 hardware) */
        fdc_clear_irq();
        break;
    case FDC_REG_TRACK:
        val = fdc.track_reg;
        FDD_DBG("FDC track read -> %d", val);
        break;
    case FDC_REG_SECTOR:
        val = fdc.sector_reg;
        FDD_DBG("FDC sector read -> %d", val);
        break;
    case FDC_REG_DATA:
        val = fdc.data_reg;
        FDD_DBG("FDC data read -> 0x%02X", val);
        break;
    }
    return val;
}

static void fdc_write_addr(uint32_t addr, uint32_t val, int size)
{
    (void)addr; (void)size;

    //fprintf(stderr, "[FDC] fdc_write_addr addr=0x%06X val=0x%04X size=%d mode=0x%04X reg=%d screg=%d\n",
    //        addr, val, size, fdc.dma_mode, fdc_reg_index(),
    //        !!(fdc.dma_mode & DMA_MODE_SCREG));

    uint8_t v = val & 0xFFu;

    if (fdc.dma_mode & DMA_MODE_SCREG) {
        /* Writing sector count register */
        fdc.dma_sector_count = (uint16_t)v;
        fdc.dma_addr = fdc.dma_base_addr;   /* latch address at start of transfer */
        FDD_DBG("FDC SCREG write (sector count) = %d", v);
        return;
    }

    switch (fdc_reg_index()) {
        case FDC_REG_CMD_STATUS:
            FDD_DBG("FDC command: 0x%02X drive=%d", v, fdc.selected_drive);

            /* Start motor. Guard the drive index: a command with no drive
             * selected (probe-time FORCE INTERRUPT, say) used to write
             * motor_ticks[-1] - an underflow that corrupted whatever static
             * the linker placed before the array. */
            if (fdc.selected_drive >= 0 && fdc.selected_drive < FDD_MAX_DRIVES) {
                motor_ticks[fdc.selected_drive] = MOTOR_OFF_TICKS;
                fdc.status |= FDC_STATUS_MOTORON;
            }

            fdc_exec_command(v);
            usleep (2000);
            break;
        case FDC_REG_TRACK:
            fdc.track_reg = v;
            FDD_DBG("FDC track set: %d", v);
            break;
        case FDC_REG_SECTOR:
            fdc.sector_reg = v;
            FDD_DBG("FDC sector set: %d", v);
            break;
        case FDC_REG_DATA:
            fdc.data_reg = v;
            FDD_DBG("FDC data set: 0x%02X", v);
            break;
    }
}

/* =========================================================================
 * IRQ helpers
 *
 * Type I commands (seek/restore/step): assert IRQ immediately.
 *   Real WD1772 asserts /INT within microseconds of command completion.
 *   EmuTOS flop_detect_drive() calls irq_delay() then polls immediately.
 *
 * Type II commands (read/write sector): use one-poll latch.
 *   Gives EmuTOS time to exit the command write bus cycle before seeing IRQ.
 * ========================================================================= */

static void fdc_raise_irq_immediate(void)
{
    fdc.irq_pending = true;
    fdc.irq_ready   = true;    /* visible immediately on first GPIP poll */
    //FDD_DBG("IRQ raised (immediate)");
}

static void fdc_raise_irq_latched(void)
{
    fdc.irq_pending = true;
    fdc.irq_ready   = false;   /* visible on second GPIP poll */
    //FDD_DBG("IRQ raised (latched)");
}

static void fdc_clear_irq(void)
{
    fdc.irq_pending = false;
    fdc.irq_ready   = false;
    //FDD_DBG("IRQ cleared");
}

/* =========================================================================
 * Command execution
 * ========================================================================= */

static void fdc_exec_command(uint8_t cmd)
{
    fdc.command = cmd;
    fdc.status |= FDC_STATUS_BUSY;
    fdc.status &= ~(FDC_STATUS_INDEX   | FDC_STATUS_CRCERR   |
                    FDC_STATUS_SEEKERR | FDC_STATUS_WRTPROT  |
                    FDC_STATUS_LOSTDATA);
    fdc_clear_irq();

    /*
     * WD1772 command class identification:
     *
     * Force Interrupt: top nibble = 0xD  (0xD0..0xDF)
     *
     * Type I:
     *   Restore:  top nibble = 0x0  (0x00..0x0F)
     *   Seek:     top nibble = 0x1  (0x10..0x1F)
     *   Step:     top 3 bits = 001  (0x20..0x3F)  <- lower nibble = rate/flags
     *   Step In:  top 3 bits = 010  (0x40..0x5F)
     *   Step Out: top 3 bits = 011  (0x60..0x7F)
     *
     * Type II:
     *   Read Sector:  top 3 bits = 100  (0x80..0x9F)
     *   Write Sector: top 3 bits = 101  (0xA0..0xBF)
     *
     * Type III:
     *   Read Address:  top nibble = 0xC  (0xC0..0xCF)
     *   Read Track:    top nibble = 0xE  (0xE0..0xEF)
     *   Write Track:   top nibble = 0xF  (0xF0..0xFF)
     *
     * IMPORTANT: Step/Step-In/Step-Out use top 3 bits, not top 4.
     * EmuTOS sends commands like 0x32, 0x3F, 0x52, 0x5F with flag bits
     * in the lower nibble — these are valid Step variants, not unknown commands.
     */

    uint8_t top4 = cmd & 0xF0u;
    uint8_t top3 = cmd & 0xE0u;

    /* Force Interrupt */
    if (top4 == 0xD0u) {
        fdc.status &= ~FDC_STATUS_BUSY;
        //FDD_LOG("Force interrupt (0x%02X)", cmd);
        if (cmd & 0x08u)
            fdc_raise_irq_immediate();
        return;
    }

    /* Type I: Restore (0x00..0x0F) */
    if (top4 == 0x00u) { fdc_do_type1(cmd); return; }

    /* Type I: Seek (0x10..0x1F) */
    if (top4 == 0x10u) { fdc_do_type1(cmd); return; }

    /* Type I: Step (0x20..0x3F) */
    if (top3 == 0x20u) { fdc_do_type1(cmd); return; }

    /* Type I: Step In (0x40..0x5F) */
    if (top3 == 0x40u) { fdc_do_type1(cmd); return; }

    /* Type I: Step Out (0x60..0x7F) */
    if (top3 == 0x60u) { fdc_do_type1(cmd); return; }

    /* Type II: Read Sector (0x80..0x9F) */
    if (top3 == 0x80u) { fdc_do_read_sectors();  return; }

    /* Type II: Write Sector (0xA0..0xBF) */
    if (top3 == 0xA0u) { fdc_do_write_sectors(); return; }

    /* Type III: Read Address (0xC0..0xCF) */
    if (top4 == 0xC0u) { fdc_do_read_address(); return; }

    /* Type III: Read Track (0xE0..0xEF) - not needed for normal disk access */
    if (top4 == 0xE0u) {
       // FDD_LOG("Read Track not implemented (cmd=0x%02X) -> error", cmd);
        fdc.status |= FDC_STATUS_RECNF;
        fdc.status &= ~FDC_STATUS_BUSY;
        fdc_raise_irq_immediate();
        return;
    }

    /* Type III: Write Track / Format (0xF0..0xFF) - not needed for normal access */
    if (top4 == 0xF0u) {
        //FDD_LOG("Write Track not implemented (cmd=0x%02X) -> error", cmd);
        fdc.status |= FDC_STATUS_RECNF;
        fdc.status &= ~FDC_STATUS_BUSY;
        fdc_raise_irq_immediate();
        return;
    }

    //FDD_LOG("Unknown command 0x%02X", cmd);
    fdc.status &= ~FDC_STATUS_BUSY;
    fdc_raise_irq_immediate();
}

/* =========================================================================
 * Type I: Restore / Seek / Step / Step In / Step Out
 *
 * cmd & 0xF0 identifies Restore (0x00) and Seek (0x10).
 * cmd & 0xE0 identifies Step (0x20), Step In (0x40), Step Out (0x60).
 * Lower nibble contains step rate and flag bits — ignore for dispatch,
 * but preserve cmd so flag bits can be checked if needed.
 * ========================================================================= */

static void fdc_do_type1(uint8_t cmd)
{
    int drv = fdc.selected_drive;
    int max_tracks;
    int target;

    if (drv < 0 || drv >= FDD_MAX_DRIVES) {
        fdc.status &= ~FDC_STATUS_BUSY;
        fdc.status |= FDC_STATUS_SEEKERR;
        fdc_raise_irq_immediate();
        return;
    }

    fdd_drive_t *d = &fdc.drives[drv];
    max_tracks = d->disk_inserted ? d->num_tracks : FDD_TRACKS_80;

    uint8_t top4 = cmd & 0xF0u;
    uint8_t top3 = cmd & 0xE0u;

    if (top4 == 0x00u) {
        /* Restore: move head to track 0 */
        d->current_track = 0;
        fdc.track_reg    = 0;
        //fdc.sector_reg   = 1;   /* WD1772 resets sector register to 1 */
        //FDD_LOG("Drive %c: Restore -> track 0", 'A' + drv);

    } else if (top4 == 0x10u) {
        /* Seek: move to track in data register */
        target = fdc.data_reg;
        if (target < 0 || target >= max_tracks) {
            fdc.status |= FDC_STATUS_SEEKERR;
          //  FDD_LOG("Drive %c: Seek %d -> SEEK ERROR (max %d)",
          //          'A' + drv, target, max_tracks - 1);
        } else {
            d->current_track = target;
            fdc.track_reg    = (uint8_t)target;
          //  FDD_LOG("Drive %c: Seek -> track %d", 'A' + drv, target);
        }

    } else if (top3 == 0x20u) {
        /* Step: repeat last direction - we track no last-direction state,
         * so treat as nop. EmuTOS uses this only as a timing/verify step. */
       // FDD_LOG("Drive %c: Step (nop, cmd=0x%02X)", 'A' + drv, cmd);

    } else if (top3 == 0x40u) {
        if (d->current_track < max_tracks - 1) {
            d->current_track++;
        }
        if (cmd & 0x10u) {
            fdc.track_reg = (uint8_t)d->current_track;
        }
        //FDD_LOG("Drive %c: Step In -> track %d (cmd=0x%02X)",
        //    'A' + drv, d->current_track, cmd);

    } else if (top3 == 0x60u) {
        if (d->current_track > 0) {
            d->current_track--;
        }
        if (cmd & 0x10u) {
            fdc.track_reg = (uint8_t)d->current_track;
        }
        //FDD_LOG("Drive %c: Step Out -> track %d (cmd=0x%02X)",
        //        'A' + drv, d->current_track, cmd);
    }

#if defined(ATARI_LAT_DIAG) || defined(ATARI_IDE_DIAG)
    fprintf(stderr, "[FDC-T1] cmd=%02X drv=%d data=%u -> trk=%d\n",
            cmd, drv, fdc.data_reg, d->current_track);
#endif

    /* Update Track 0 status bit */
    if (d->current_track == 0)
        fdc.status |= FDC_STATUS_TRACK0;
    else
        fdc.status &= ~FDC_STATUS_TRACK0;

    /* Simulate step time for Step/Step-In/Step-Out.
     * TOS 1.62 calibration measures step timing and loops if it
     * completes too fast. 3ms matches WD1772 default 6ms/step rate. */
    if (top3 == 0x20u || top3 == 0x40u || top3 == 0x60u) {
        //pthread_mutex_unlock(&fdc.lock);
        usleep(2500);
        //pthread_mutex_lock(&fdc.lock);
    }

    fdc.status &= ~FDC_STATUS_BUSY;
    fdc_raise_irq_immediate();
}

/* =========================================================================
 * Type II: Read Sectors
 *
 * After transfer:
 *   dma_sector_count = 0  (EmuTOS reads this via SCREG mode to check success)
 *   dma_status = DMA_STATUS_OK | DMA_STATUS_SCZERO
 *   IRQ raised (latched - visible on second GPIP poll)
 * ========================================================================= */

static void fdc_do_read_sectors(void)
{
    //fprintf(stderr, "[READ] entry: drv=%d track=%d side=%d sec=%d dma=0x%06X\n",
    //        fdc.selected_drive,
    //        (fdc.selected_drive >= 0 && fdc.selected_drive < FDD_MAX_DRIVES)
    //            ? fdc.drives[fdc.selected_drive].current_track : -1,
    //        fdc.selected_side,
    //        fdc.sector_reg,
    //        fdc.dma_addr);
            
    /* Reset DMA status at start of transfer */
    fdc.dma_status = DMA_STATUS_OK;

    int drv    = fdc.selected_drive;

    //fprintf(stderr, "[READ2] disk_inserted=%d fd=%d\n",
    //    fdc.drives[drv].disk_inserted, fdc.drives[drv].fd);

    /* Use current physical track position, not the track register.
     * The track register can be corrupted by calibration sequences.
     * EmuTOS seeks to the correct track before reading; current_track
     * is updated by fdc_do_type1() and is authoritative. */
    int track  = fdc.drives[drv].current_track;//(drv >= 0 && drv < FDD_MAX_DRIVES)
                // ? fdc.drives[drv].current_track
                // : fdc.track_reg;
    int side   = fdc.selected_side;
    int sector = fdc.sector_reg;
    int count  = fdc.dma_sector_count ? fdc.dma_sector_count : 1;

    //FDD_LOG("Read: T%d S%d Sec%d count=%d dma=0x%06X drive=%d",
     //       track, side, sector, count, fdc.dma_addr, drv);

    //static int read_count = 0;
    //fprintf(stderr, "[COUNT] Read #%d T%d S%d Sec%d\n",
    //    ++read_count, track, side, sector);

    if (drv < 0 || drv >= FDD_MAX_DRIVES || !fdc.drives[drv].disk_inserted) {
       // FDD_LOG("Read: no disk in drive %d", drv);
        fdc.status    |= FDC_STATUS_RECNF;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;     /* error */
        fdc_raise_irq_latched();
        return;
    }

    uint8_t *buf = malloc((size_t)count * FDD_SECTOR_SIZE);
    if (!buf) {
        fdc.status    |= FDC_STATUS_LOSTDATA;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        return;
    }

    int img_ret = fdd_image_read(drv, track, side, sector, count, buf);
#if defined(ATARI_LAT_DIAG) || defined(ATARI_IDE_DIAG)
    fprintf(stderr, "[FDC-RD] T%d S%d sec%d n%d dma=%06X treg=%u %s %02X %02X\n",
            track, side, sector, count, fdc.dma_addr, fdc.track_reg,
            img_ret < 0 ? "FAIL" : "ok",
            img_ret < 0 ? 0 : buf[0], img_ret < 0 ? 0 : buf[1]);
#endif
    //fprintf(stderr, "[COUNT] Read #%d T%d S%d Sec%d %s\n",
    //        ++read_count, track, side, sector,
    //        img_ret < 0 ? "FAIL" : "OK");
    if (img_ret < 0) {
        free(buf);
        fdc.status    |= FDC_STATUS_RECNF;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        return;
    }

    /*f (track == 0 && side == 0 && sector == 2) {
        fprintf(stderr, "[FAT] %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                buf[0],  buf[1],  buf[2],  buf[3],
                buf[4],  buf[5],  buf[6],  buf[7],
                buf[8],  buf[9],  buf[10], buf[11],
                buf[12], buf[13], buf[14], buf[15]);
    }*/

    /* Transfer to real ST RAM via GPIO bus */
    dma_copy_to_ram(fdc.dma_addr, buf, (size_t)count * FDD_SECTOR_SIZE);

    //fprintf(stderr, "[READ3] dma_copy_to_ram done, addr=0x%06X\n", fdc.dma_addr);
    //fflush(stderr);

    free(buf);

    //fdc.dma_addr        += (uint32_t)(count * FDD_SECTOR_SIZE);
    //fdc.sector_reg       = (uint8_t)(sector + count);
    fdc.dma_sector_count = 0;                              /* 0 = success */
    fdc.dma_status       = DMA_STATUS_OK | DMA_STATUS_SCZERO;
    fdc.status          &= ~FDC_STATUS_BUSY;
    fdc_raise_irq_latched();

    //fprintf(stderr, "[READ4] IRQ raised, pending=%d ready=%d\n",
    //    fdc.irq_pending, fdc.irq_ready);
    //fflush(stderr);

    //fprintf(stderr, "[STATUS] after read: status=0x%02X dma_status=0x%02X dma_sector_count=%d\n",
    //    fdc.status, fdc.dma_status, fdc.dma_sector_count);
}

/* =========================================================================
 * Type II: Write Sectors
 * ========================================================================= */

static void fdc_do_write_sectors(void)
{
    fdc.dma_status = DMA_STATUS_OK;

    int drv    = fdc.selected_drive;
    /* Use current physical track position, not the track register.
     * The track register can be corrupted by calibration sequences.
     * EmuTOS seeks to the correct track before reading; current_track
     * is updated by fdc_do_type1() and is authoritative. */
    int track  = fdc.drives[drv].current_track;//(drv >= 0 && drv < FDD_MAX_DRIVES)
                 //? fdc.drives[drv].current_track
                // : fdc.track_reg;
    int side   = fdc.selected_side;
    int sector = fdc.sector_reg;
    int count  = fdc.dma_sector_count ? fdc.dma_sector_count : 1;

    //FDD_LOG("Write: T%d S%d Sec%d count=%d dma=0x%06X drive=%d",
     //       track, side, sector, count, fdc.dma_addr, drv);

    if (drv < 0 || drv >= FDD_MAX_DRIVES || !fdc.drives[drv].disk_inserted) {
        fdc.status    |= FDC_STATUS_RECNF;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        return;
    }

    if (fdc.drives[drv].write_protected) {
        fdc.status    |= FDC_STATUS_WRTPROT;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        FDD_LOG("Drive %c: write protected", 'A' + drv);
        return;
    }

    uint8_t *buf = malloc((size_t)count * FDD_SECTOR_SIZE);
    if (!buf) {
        fdc.status    |= FDC_STATUS_LOSTDATA;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        return;
    }

    /* Read from real ST RAM via GPIO bus */
    dma_copy_from_ram(fdc.dma_addr, buf, (size_t)count * FDD_SECTOR_SIZE);

    if (fdd_image_write(drv, track, side, sector, count, buf) < 0) {
        free(buf);
        fdc.status    |= FDC_STATUS_RECNF;
        fdc.status    &= ~FDC_STATUS_BUSY;
        fdc.dma_status = 0;
        fdc_raise_irq_latched();
        return;
    }
    free(buf);

    //fdc.dma_addr        += (uint32_t)(count * FDD_SECTOR_SIZE);
    //fdc.sector_reg       = (uint8_t)(sector + count);
    fdc.dma_sector_count = 0;
    fdc.dma_status       = DMA_STATUS_OK | DMA_STATUS_SCZERO;
    fdc.status          &= ~FDC_STATUS_BUSY;
    fdc_raise_irq_latched();

    //FDD_LOG("Write OK: %d sector(s) drive %c T%d S%d",
   //         count, 'A' + drv, track, fdc.sector_reg);
}

/* =========================================================================
 * Type III: Read Address
 *
 * Returns 6-byte ID field: track, side, sector, size_code, crc1, crc2
 * EmuTOS uses this during geometry detection.
 * ========================================================================= */

static void fdc_do_read_address(void)
{
    int drv    = fdc.selected_drive;
    /* Use current physical track position, not the track register.
     * The track register can be corrupted by calibration sequences.
     * EmuTOS seeks to the correct track before reading; current_track
     * is updated by fdc_do_type1() and is authoritative. */
    int track  = (drv >= 0 && drv < FDD_MAX_DRIVES)
                 ? fdc.drives[drv].current_track
                 : fdc.track_reg;
    int side   = fdc.selected_side;

    //FDD_LOG("Read address: drive=%d track=%d side=%d", drv, track, side);

    if (drv < 0 || drv >= FDD_MAX_DRIVES || !fdc.drives[drv].disk_inserted) {
        fdc.status |= FDC_STATUS_RECNF;
        fdc.status &= ~FDC_STATUS_BUSY;
        fdc_raise_irq_immediate();
        return;
    }

    uint8_t id[6];
    id[0] = (uint8_t)track;
    id[1] = (uint8_t)side;
    id[2] = fdc.sector_reg ? fdc.sector_reg : 1;
    id[3] = 2;      /* sector size code: 2 = 512 bytes */
    id[4] = 0;      /* CRC1 */
    id[5] = 0;      /* CRC2 */

    dma_copy_to_ram(fdc.dma_addr, id, 6);

    fdc.track_reg  = (uint8_t)track;
    fdc.sector_reg = id[2];
    fdc.status    &= ~FDC_STATUS_BUSY;
    fdc_raise_irq_immediate();

    //FDD_LOG("Read address OK: T%d S%d sec=%d spt=%d",
     //       track, side, id[2], d->sectors_per_track);
}

/* =========================================================================
 * Image I/O
 *
 * Atari ST .st image layout (linear sector dump):
 *   track 0 side 0 sectors 1..spt
 *   track 0 side 1 sectors 1..spt
 *   track 1 side 0 ...
 *
 * Byte offset = ((track * sides + side) * spt + (sector-1)) * 512
 * Sectors are 1-based (sector 1 = first sector on track).
 * ========================================================================= */

static off_t sector_offset(fdd_drive_t *d, int track, int side,
                            int sector)
{
    int num_sides = d->num_sides ? d->num_sides : 2;

    //int logical = ((track * num_sides) + side) * d->sectors_per_track
     //             + (sector - 1);

    /* If image has extra reserved sectors (boot sector copies) before
     * the FAT, remap sectors beyond sector 1 on track 0 side 0 */
    //if (track == 0 && side == 0 && sector > 1 && d->reserved_sectors > 1) {
    //    logical += (d->reserved_sectors - 1);
    //}

    int bytes_per_track = FDD_SECTOR_SIZE * d->sectors_per_track;
    int offset = bytes_per_track * side;
    offset += (bytes_per_track * num_sides) * track;
    offset += (FDD_SECTOR_SIZE * (sector - 1));

    return offset;//(off_t)logical * FDD_SECTOR_SIZE;
}


static int fdd_image_read(int drive, int track, int side,
                           int sector, int count, uint8_t *buf)
{
    fdd_drive_t *d = &fdc.drives[drive];
    int sides = d->num_sides ? d->num_sides : 2;

   // fprintf(stderr, "[IMG] request: T%d S%d sec=%d count=%d spt=%d\n",
    //    track, side, sector, count, d->sectors_per_track);
    //fflush(stderr);

    if (d->fd < 0 || !d->disk_inserted) {
        FDD_LOG("Read: no disk drive %c", 'A' + drive);
        return -1;
    }

    if (track  < 0 || track  >= d->num_tracks        ||
        side   < 0 || side   >= sides                 ||
        sector < 1 || sector + count - 1 > d->sectors_per_track) {
        FDD_LOG("Read: out of range T%d S%d Sec%d+%d (max %d tracks, %d spt, %d sides)",
                track, side, sector, count, d->num_tracks, d->sectors_per_track, sides);
        return -1;
    }

    off_t off = sector_offset(d, track, side, sector);

    //fprintf(stderr, "[IMG] about to pread: fd=%d off=%ld count=%zu\n",
    //    d->fd, (long)off, (size_t)count * FDD_SECTOR_SIZE);
    //fflush(stderr);

    ssize_t n = pread(d->fd, buf, (size_t)count * FDD_SECTOR_SIZE, off);

    //fprintf(stderr, "[IMG] pread returned: %zd\n", n);
    //fflush(stderr);

    if (n != (ssize_t)(count * FDD_SECTOR_SIZE)) {
        FDD_LOG("Read: pread failed at offset %ld: %s",
                (long)off, strerror(errno));
        return -1;
    }
    return 0;
}

static int fdd_image_write(int drive, int track, int side,
                            int sector, int count, const uint8_t *buf)
{
    fdd_drive_t *d = &fdc.drives[drive];
    int sides = d->num_sides ? d->num_sides : 2;

    if (d->fd < 0 || !d->disk_inserted) return -1;
    if (d->write_protected) {
        fprintf (stderr, "[SXB] fdd_image_write image write-protected\n");    
        return -1;
    }

    if (track  < 0 || track  >= d->num_tracks        ||
        side   < 0 || side   >= sides                 ||
        sector < 1 || sector + count - 1 > d->sectors_per_track) {
        FDD_LOG("Write: out of range T%d S%d Sec%d+%d (max %d tracks, %d spt, %d sides)",
                track, side, sector, count, d->num_tracks, d->sectors_per_track, sides);
        return -1;
    }

    off_t off = sector_offset(d, track, side, sector);
    ssize_t n = pwrite(d->fd, buf, (size_t)count * FDD_SECTOR_SIZE, off);
    if (n != (ssize_t)(count * FDD_SECTOR_SIZE)) {
        FDD_LOG("Write: pwrite failed at offset %ld: %s",
                (long)off, strerror(errno));
        return -1;
    }

    fsync (d->fd);

    //fprintf(stderr, "[WRITE] pwrite OK: T%d S%d sec=%d off=%ld\n",
    //    track, side, sector, (long)off);
    //fflush(stderr);

    return 0;
}
