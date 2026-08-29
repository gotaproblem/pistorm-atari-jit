/*
 * acsi.c - emulated ACSI hard-disk target(s) on the Atari DMA port
 *
 * See acsi.h / ACSI-DESIGN.md. Command semantics follow the Adaptec
 * ACB-4000 bridge flavour that period drivers (AHDI, ICD, PP) and
 * Spectre talk to; cross-checked against Hatari's hdc.c, which is the
 * proven reference for this dialect.
 *
 * Threading: everything here runs on the CPU thread inside the guest's
 * own register accesses (the same context atari_fdd.c runs in), so no
 * locking is needed. File I/O is synchronous - the guest's clock is
 * stopped while we work, so driver timeouts cannot fire early.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "acsi.h"

/* ---- services provided by atari_fdd.c --------------------------------- */
extern uint32_t fdd_dma_base(void);
extern void     fdd_dma_base_advance(uint32_t bytes);
extern int      fdd_dma_dir_is_write(void);            /* mode bit 8 */
extern void     fdd_dma_note_ok(int count_zero);
extern void     fdd_dma_copy_to_ram(uint32_t addr, const uint8_t *buf,
                                    uint32_t count);
extern void     fdd_dma_copy_from_ram(uint32_t addr, uint8_t *buf,
                                      uint32_t count);

#define ACSI_LOG(...) do { fprintf(stderr, "[ACSI] " __VA_ARGS__); \
                           fprintf(stderr, "\n"); } while (0)

/* ---- targets ----------------------------------------------------------- */

typedef struct {
    int      fd;                 /* -1 = no target at this ID            */
    char     path[256];
    bool     hfs;                /* bare HFS volume: synthesized sector 0 */
    uint32_t total_lba;          /* as seen by the guest                  */
    uint8_t  root[512];          /* synthesized AHDI root (hfs only)      */
    uint8_t  sense;              /* pending sense code (0 = none)         */
    uint32_t sense_lba;
} acsi_target_t;

static acsi_target_t tgt[ACSI_MAX_TARGETS];
static int n_targets;
static bool tgt_inited;

static void tgt_init_once(void)
{
    if (tgt_inited)
        return;
    for (int i = 0; i < ACSI_MAX_TARGETS; i++)
        tgt[i].fd = -1;
    tgt_inited = true;
}

/* ---- bus/command state ------------------------------------------------- */

typedef enum { PH_IDLE, PH_CDB, PH_STATUS } acsi_phase_t;

static acsi_phase_t phase = PH_IDLE;
static int      cur_id = -1;
static uint8_t  cdb[16];
static int      cdb_len;             /* bytes expected  */
static int      cdb_got;             /* bytes collected */
static int      icd;                 /* $1F extended-command wrapper */
static uint8_t  status_byte;
static bool     irq;                 /* GPIP5 assert (active low line) */
static bool     owns;                /* DMA-port registers answer here  */
static uint16_t residual;            /* SCREG readback after commands   */

bool acsi_enabled(void)      { return n_targets > 0; }
bool acsi_owns_dma(void)     { return owns; }
bool acsi_irq_active(void)   { return irq; }
uint16_t acsi_residual_count(void) { return residual; }

void acsi_release(void)
{
    owns = false;
    irq = false;
    phase = PH_IDLE;
}

/* ---- images ------------------------------------------------------------ */

static void synth_root(acsi_target_t *t, uint32_t hfs_sectors)
{
    uint8_t *r = t->root;
    memset(r, 0, 512);
    uint32_t total = hfs_sectors + 1;
    /* AHDI root sector: hd_siz at $1C2 (BE32), partition table at $1C6,
     * entries of {flag, id[3], start BE32, size BE32}. One MAC-type
     * partition starting at sector 1 - exactly what Spectre's own
     * formatter lays down, minus the Atari partitions it would add. */
    r[0x1C2] = (uint8_t)(total >> 24); r[0x1C3] = (uint8_t)(total >> 16);
    r[0x1C4] = (uint8_t)(total >> 8);  r[0x1C5] = (uint8_t)total;
    r[0x1C6] = 0x01;                             /* exists              */
    r[0x1C7] = 'M'; r[0x1C8] = 'A'; r[0x1C9] = 'C';
    r[0x1CA] = 0; r[0x1CB] = 0; r[0x1CC] = 0; r[0x1CD] = 1;   /* start */
    r[0x1CE] = (uint8_t)(hfs_sectors >> 24);
    r[0x1CF] = (uint8_t)(hfs_sectors >> 16);
    r[0x1D0] = (uint8_t)(hfs_sectors >> 8);
    r[0x1D1] = (uint8_t)hfs_sectors;
}

int acsi_attach_at(int id, const char *path)
{
    tgt_init_once();
    if (id < 0) {
        /* auto: lowest free ID */
        for (id = 0; id < ACSI_MAX_TARGETS && tgt[id].fd >= 0; id++)
            ;
    }
    if (id >= ACSI_MAX_TARGETS) {
        ACSI_LOG("attach %s: no free ID (max %d)", path, ACSI_MAX_TARGETS);
        return -1;
    }
    if (tgt[id].fd >= 0) {
        ACSI_LOG("attach %s: ID %d already has %s", path, id, tgt[id].path);
        return -1;
    }
    acsi_target_t *t = &tgt[id];

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        ACSI_LOG("attach %s: cannot open (%s)", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 512) {
        ACSI_LOG("attach %s: unusable size", path);
        close(fd);
        return -1;
    }

    const char *dot = strrchr(path, '.');
    t->hfs = (dot && strcasecmp(dot, ".hfs") == 0);
    uint32_t file_sectors = (uint32_t)(st.st_size / 512);

    t->fd = fd;
    snprintf(t->path, sizeof t->path, "%s", path);
    if (t->hfs) {
        t->total_lba = file_sectors + 1;     /* +synthesized root      */
        synth_root(t, file_sectors);
    } else {
        t->total_lba = file_sectors;
    }
    t->sense = 0;
    n_targets++;
    ACSI_LOG("ID %d: %s - %u sectors (%u MB)%s", id, path,
             t->total_lba, t->total_lba / 2048,
             t->hfs ? ", bare HFS volume (AHDI root synthesized)" : "");
    return id;
}

int acsi_attach(const char *path)
{
    /* optional ID prefix pins the target: "3:disk.img" or "3 disk.img";
     * otherwise the lowest free ID is used */
    if (path[0] >= '0' && path[0] <= '7' &&
        (path[1] == ':' || path[1] == ' ' || path[1] == '\t')) {
        int id = path[0] - '0';
        path += 2;
        while (*path == ' ' || *path == '\t')
            path++;
        return acsi_attach_at(id, path);
    }
    return acsi_attach_at(-1, path);
}

static int read_lba(acsi_target_t *t, uint32_t lba, uint8_t *buf)
{
    if (t->hfs) {
        if (lba == 0) { memcpy(buf, t->root, 512); return 0; }
        return pread(t->fd, buf, 512, (off_t)(lba - 1) * 512) == 512 ? 0 : -1;
    }
    return pread(t->fd, buf, 512, (off_t)lba * 512) == 512 ? 0 : -1;
}

static int write_lba(acsi_target_t *t, uint32_t lba, const uint8_t *buf)
{
    if (t->hfs) {
        if (lba == 0) {
            /* the synthesized root is not backed by the file; period
             * partitioners rewriting it would destroy the wrapper illusion,
             * so it is read-only by design */
            ACSI_LOG("ID write to synthesized root sector ignored (%s)",
                     t->path);
            return 0;
        }
        return pwrite(t->fd, buf, 512, (off_t)(lba - 1) * 512) == 512 ? 0 : -1;
    }
    return pwrite(t->fd, buf, 512, (off_t)lba * 512) == 512 ? 0 : -1;
}

/* ---- completion -------------------------------------------------------- */

#define SENSE_NONE        0x00
#define SENSE_NOT_READY   0x04
#define SENSE_INVALID_OP  0x20
#define SENSE_INVALID_LBA 0x21
#define SENSE_INVALID_CDB 0x24
#define SENSE_INVALID_LUN 0x25

static void finish(uint8_t st, uint8_t sense, uint32_t lba)
{
    acsi_target_t *t = &tgt[cur_id];
    status_byte = st;
    t->sense = sense;
    t->sense_lba = lba;
    phase = PH_STATUS;
    irq = true;                       /* command complete               */
    fdd_dma_note_ok(residual == 0);
}

static void finish_good(void)  { finish(0x00, SENSE_NONE, 0); }
static void finish_check(uint8_t sense, uint32_t lba) { finish(0x02, sense, lba); }

/* ---- data helpers ------------------------------------------------------ */

static void dma_out(const uint8_t *buf, uint32_t len)   /* device -> RAM */
{
    if (!len) return;
    fdd_dma_copy_to_ram(fdd_dma_base(), buf, len);
    fdd_dma_base_advance(len);
}

static void dma_in(uint8_t *buf, uint32_t len)          /* RAM -> device */
{
    if (!len) return;
    fdd_dma_copy_from_ram(fdd_dma_base(), buf, len);
    fdd_dma_base_advance(len);
}

/* ---- command execution ------------------------------------------------- */

static void exec_read(acsi_target_t *t, uint32_t lba, uint32_t blocks)
{
    uint8_t buf[512];
    if (lba + blocks > t->total_lba || lba + blocks < lba) {
        finish_check(SENSE_INVALID_LBA, lba);
        return;
    }
    for (uint32_t i = 0; i < blocks; i++) {
        if (read_lba(t, lba + i, buf) < 0) {
            residual = (uint16_t)(blocks - i);
            finish_check(SENSE_INVALID_LBA, lba + i);
            return;
        }
        dma_out(buf, 512);
    }
    residual = 0;
    finish_good();
}

static void exec_write(acsi_target_t *t, uint32_t lba, uint32_t blocks)
{
    uint8_t buf[512];
    if (lba + blocks > t->total_lba || lba + blocks < lba) {
        finish_check(SENSE_INVALID_LBA, lba);
        return;
    }
    for (uint32_t i = 0; i < blocks; i++) {
        dma_in(buf, 512);
        if (write_lba(t, lba + i, buf) < 0) {
            residual = (uint16_t)(blocks - i);
            finish_check(SENSE_INVALID_LBA, lba + i);
            return;
        }
    }
    residual = 0;
    finish_good();
}

static void exec_inquiry(acsi_target_t *t, uint32_t alloc)
{
    uint8_t d[48];
    memset(d, 0, sizeof d);
    d[0] = 0x00;                     /* direct-access device            */
    d[1] = 0x00;                     /* not removable                   */
    d[2] = 0x01;                     /* ANSI level, bridge-era          */
    d[3] = 0x01;
    d[4] = 31;                       /* additional length               */
    memcpy(&d[8],  "PiStorm ", 8);   /* vendor  (8)                     */
    memcpy(&d[16], t->hfs ? "ACSI HFS DISK   "
                          : "ACSI DISK       ", 16);  /* product (16)   */
    memcpy(&d[32], "1.0 ", 4);       /* revision (4)                    */
    if (alloc > sizeof d) alloc = sizeof d;
    dma_out(d, alloc);
    residual = 0;
    finish_good();
}

static void exec_request_sense(acsi_target_t *t, uint32_t alloc)
{
    uint8_t d[16];
    memset(d, 0, sizeof d);
    if (alloc < 4) alloc = 4;
    if (alloc > sizeof d) alloc = sizeof d;
    /* classic 4-byte bridge sense: code + LBA of the failure */
    d[0] = t->sense;
    d[1] = (uint8_t)((t->sense_lba >> 16) & 0x1F);
    d[2] = (uint8_t)(t->sense_lba >> 8);
    d[3] = (uint8_t)t->sense_lba;
    dma_out(d, alloc);
    t->sense = SENSE_NONE;
    residual = 0;
    finish_good();
}

static void exec_mode_sense(acsi_target_t *t, uint8_t page, uint32_t alloc)
{
    uint8_t d[32];
    uint32_t n;
    uint32_t blocks = t->total_lba;
    memset(d, 0, sizeof d);

    if (page == 0x00) {
        /* header + block descriptor, ACB-4000 style */
        d[3] = 8;                                  /* descriptor length */
        d[5] = (uint8_t)(blocks >> 16);
        d[6] = (uint8_t)(blocks >> 8);
        d[7] = (uint8_t)blocks;
        d[9] = 0; d[10] = 2; d[11] = 0;            /* block size 512    */
        n = 12;
    } else if (page == 0x04) {
        /* rigid disk geometry - invented but self-consistent */
        uint32_t cyl = blocks / (16 * 32);
        if (cyl == 0) cyl = 1;
        d[0] = 22;
        d[4] = 0x04; d[5] = 0x12;
        d[6] = (uint8_t)(cyl >> 16); d[7] = (uint8_t)(cyl >> 8);
        d[8] = (uint8_t)cyl;
        d[9] = 16;                                 /* heads             */
        n = 24;
    } else {
        finish_check(SENSE_INVALID_CDB, 0);
        return;
    }
    if (alloc && alloc < n) n = alloc;
    dma_out(d, n);
    residual = 0;
    finish_good();
}

static void exec_command(void)
{
    acsi_target_t *t = &tgt[cur_id];
    const uint8_t *c = icd ? cdb + 1 : cdb;
    uint8_t op = icd ? c[0] : (uint8_t)(c[0] & 0x1F);

    /* 6-byte CDB fields (group 0) */
    uint32_t lba6 = (((uint32_t)c[1] & 0x1F) << 16) |
                    ((uint32_t)c[2] << 8) | c[3];
    uint32_t len6 = c[4] ? c[4] : 256;
    uint8_t  lun  = (uint8_t)(c[1] >> 5);

    if (!icd && lun != 0) {
        finish_check(SENSE_INVALID_LUN, 0);
        return;
    }

    switch (op) {
        case 0x00:                                    /* TEST UNIT READY */
        case 0x01:                                    /* REZERO          */
            residual = 0;
            finish_good();
            break;
        case 0x03:                                    /* REQUEST SENSE   */
            exec_request_sense(t, c[4] ? c[4] : 4);
            break;
        case 0x04:                                    /* FORMAT UNIT     */
            residual = 0;
            finish_good();
            break;
        case 0x08:                                    /* READ(6)         */
            exec_read(t, lba6, len6);
            break;
        case 0x0A:                                    /* WRITE(6)        */
            exec_write(t, lba6, len6);
            break;
        case 0x0B:                                    /* SEEK(6)         */
            if (lba6 < t->total_lba) { residual = 0; finish_good(); }
            else finish_check(SENSE_INVALID_LBA, lba6);
            break;
        case 0x12:                                    /* INQUIRY         */
            exec_inquiry(t, c[4] ? c[4] : 5);
            break;
        case 0x15: {                                  /* MODE SELECT     */
            /* accept and discard the parameter list the guest DMAs out */
            uint8_t junk[512];
            uint32_t n = c[4];
            while (n) {
                uint32_t chunk = n > sizeof junk ? (uint32_t)sizeof junk : n;
                dma_in(junk, chunk);
                n -= chunk;
            }
            residual = 0;
            finish_good();
            break;
        }
        case 0x1A:                                    /* MODE SENSE(6)   */
            exec_mode_sense(t, (uint8_t)(c[2] & 0x3F), c[4]);
            break;
        /* ---- 10-byte ops via the ICD $1F wrapper -------------------- */
        case 0x25:                                    /* READ CAPACITY   */
            if (icd) {
                uint8_t d[8];
                uint32_t last = t->total_lba - 1;
                d[0] = (uint8_t)(last >> 24); d[1] = (uint8_t)(last >> 16);
                d[2] = (uint8_t)(last >> 8);  d[3] = (uint8_t)last;
                d[4] = 0; d[5] = 0; d[6] = 2; d[7] = 0;   /* 512 */
                dma_out(d, 8);
                residual = 0;
                finish_good();
                break;
            }
            finish_check(SENSE_INVALID_OP, 0);
            break;
        case 0x28:                                    /* READ(10)        */
        case 0x2A: {                                  /* WRITE(10)       */
            if (!icd) { finish_check(SENSE_INVALID_OP, 0); break; }
            uint32_t lba10 = ((uint32_t)c[2] << 24) | ((uint32_t)c[3] << 16) |
                             ((uint32_t)c[4] << 8) | c[5];
            uint32_t len10 = ((uint32_t)c[7] << 8) | c[8];
            if (op == 0x28) exec_read(t, lba10, len10);
            else            exec_write(t, lba10, len10);
            break;
        }
        default:
            ACSI_LOG("ID %d: unsupported opcode $%02X%s", cur_id, op,
                     icd ? " (ICD)" : "");
            finish_check(SENSE_INVALID_OP, 0);
            break;
    }
}

/* ---- CDB byte handshake ------------------------------------------------ */

static int icd_cdb_len(uint8_t scsi_op)
{
    switch (scsi_op >> 5) {
        case 0:  return 1 + 6;
        case 1:
        case 2:  return 1 + 10;
        case 5:  return 1 + 12;
        default: return 1 + 6;
    }
}

bool acsi_cmd_byte(uint8_t v, bool first_byte)
{
    if (!n_targets)
        return false;
    if (first_byte) {
        int id = (v >> 5) & 7;
        if (tgt[id].fd < 0) {
            /* not one of ours: the real bus owns this command */
            acsi_release();
            return false;
        }
        cur_id = id;
        owns = true;
        phase = PH_CDB;
        icd = ((v & 0x1F) == 0x1F);
        cdb_got = 0;
        cdb_len = icd ? 2 : 6;         /* ICD: length known at 2nd byte */
        cdb[cdb_got++] = (uint8_t)(v & 0x1F);
        residual = 0;
        irq = true;                    /* request next byte             */
        return true;
    }

    if (!owns || phase != PH_CDB)
        return false;                  /* mid-command on the real bus   */

    cdb[cdb_got++] = v;
    if (icd && cdb_got == 2)
        cdb_len = icd_cdb_len(v);

    if (cdb_got < cdb_len) {
        irq = true;                    /* request next byte             */
        return true;
    }

    irq = false;
    exec_command();                    /* sets status, asserts IRQ      */
    return true;
}

uint8_t acsi_status_read(void)
{
    irq = false;
    if (phase == PH_STATUS)
        phase = PH_IDLE;
    /* ownership persists so the count/base readbacks that follow the
     * status still answer locally; the next non-emulated first byte or
     * FDC command releases it (acsi_release from atari_fdd.c) */
    return status_byte;
}
