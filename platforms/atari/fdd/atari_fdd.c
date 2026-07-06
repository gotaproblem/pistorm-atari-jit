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

#include <stdio.h>
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
#define FDD_DBG(fmt, ...)  //fprintf(stderr, "[FDD] " fmt "\n", ##__VA_ARGS__)

/* =========================================================================
 * Global state
 * ========================================================================= */
static fdc_controller_t fdc;
static int motor_ticks[FDD_MAX_DRIVES];

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
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

#ifdef __cplusplus
}
#endif

static void dma_copy_to_ram(uint32_t addr, const uint8_t *buf, size_t count)
{
    for (size_t i = 0; i < count; i++)
        ps_write_8(addr + i, buf[i]);                  /* real ST-RAM — video shifter / bus masters */

    pistorm_dma_to_stram(addr, buf, (uint32_t)count);  /* JIT natmem mirror + SMC invalidate */
}

static void dma_copy_from_ram(uint32_t addr, uint8_t *buf, size_t count)
{
    for (int i = 0; i < count; i++ ) {
        *buf++     = ps_read_8 (addr + i);
    }
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

void fdd_init(void)
{
    memset(&fdc, 0, sizeof(fdc));
    pthread_mutex_init(&fdc.lock, NULL);

    fdc.status         = 0x00;
    fdc.dma_status     = DMA_STATUS_OK;
    fdc.psg_porta      = 0xFF;
    fdc.selected_drive = -1;
    fdc.sector_reg     = 1;     /* WD1772 power-on default */

    for (int i = 0; i < FDD_MAX_DRIVES; i++) {
        fdc.drives[i].fd            = -1;
        fdc.drives[i].current_track = 0;
        fdc.drives[i].num_sides     = 2;
        motor_ticks[i]              = 0;
    }

    FDD_LOG("Initialised");
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

int fdd_insert_disk(int drive, const char *image_path, bool write_protect)
{
    if (drive < 0 || drive >= FDD_MAX_DRIVES) return -1;

    fdd_drive_t *drv = &fdc.drives[drive];

   // pthread_mutex_lock(&fdc.lock);

    if (drv->fd >= 0) {
        close(drv->fd);
        drv->fd = -1;
    }

    int flags = write_protect ? O_RDONLY : O_RDWR;
    int fd = open(image_path, flags);
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

            FDD_LOG("Drive %c: BPB geometry: %d tracks %d spt %d sides",
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
    //bool irq = fdd_irq_active();

     /* Only assert FDD IRQ if a disk is inserted and active */
    if (fdc.selected_drive < 0 || 
        !fdc.drives[fdc.selected_drive].disk_inserted) {
        return other_gpip | (1u << 5);  /* IRQ inactive */
    }
    
    if (fdd_irq_active())
        return other_gpip & ~(1u << 5);
    else
        return other_gpip |  (1u << 5);
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
        fdd_drive_t *d = &fdc.drives[i];
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
 * ========================================================================= */

uint32_t fdd_io_read(uint32_t addr, int size)
{
    uint32_t val;

    //fprintf(stderr, "[IO] READ  addr=0x%06X size=%d\n", addr, size);

    /* Handle longword reads as two word reads */
    if (size == 4) {
        uint32_t hi = fdd_io_read(addr,     2);
        uint32_t lo = fdd_io_read(addr + 2, 2);
        return (hi << 16) | lo;
    }

    //pthread_mutex_lock(&fdc.lock);

    if (addr >= 0xFF8600u && addr <= 0xFF860Fu) {
        if (addr == FDC_DATA_REG)
            val = fdc_read_addr(addr, size);
        else
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
    //fprintf(stderr, "[IO] WRITE addr=0x%06X val=0x%04X size=%d\n", addr, val, size);

    /* Handle longword writes as two word writes */
    if (size == 4) {
        fdd_io_write(addr,     (val >> 16) & 0xFFFF, 2);
        fdd_io_write(addr + 2, val & 0xFFFF,          2);
        return;
    }

    //pthread_mutex_lock(&fdc.lock);

    if (addr >= 0xFF8600u && addr <= 0xFF860Fu) {
        if (addr == FDC_DATA_REG)
            fdc_write_addr(addr, val, size);
        else
            dma_write_addr(addr, val, size);
    } else if (addr >= 0xFF8800u && addr <= 0xFF8803u) {
        psg_write_addr(addr, val, size);
    }

    //pthread_mutex_unlock(&fdc.lock);
}

/* =========================================================================
 * PSG Port A - drive and side selection
 *
 * Atari ST hardware wiring (from official schematics):
 *   PSG port A bit 1 (/DRIVE_A) = 0 → drive A selected
 *   PSG port A bit 0 (/DRIVE_B) = 0 → drive B selected
 *   PSG port A bit 2 (/SIDE1)   = 0 → side 1, = 1 → side 0
 * ========================================================================= */

static uint32_t psg_read_addr(uint32_t addr, int size)
{
    (void)size;
    if (addr == PSG_REG_SELECT && fdc.psg_reg_sel == PSG_PORT_A_REG)
        return fdc.psg_porta;
    return 0xFFu;
}

static void psg_write_addr(uint32_t addr, uint32_t val, int size)
{
    (void)size;
    uint8_t v = val & 0xFF;

    if (addr == PSG_REG_SELECT) {
        fdc.psg_reg_sel = v & 0x0F;
        //FDD_DBG("PSG: select reg %d", fdc.psg_reg_sel);
    } else if (addr == PSG_REG_WRITE) {
        //FDD_DBG("PSG: write reg%d = 0x%02X", fdc.psg_reg_sel, v);
        if (fdc.psg_reg_sel == PSG_PORT_A_REG) {
            fdc.psg_porta = v;
            fdc_decode_drive_side();
        }
        /* All other PSG registers (mixer, tone, envelope etc) silently accepted */
    }
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
    case DMA_BASE_HIGH: val = (fdc.dma_addr >> 16) & 0x3Fu; break;
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

    case DMA_BASE_HIGH:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0x00FFFFu) | ((uint32_t)(v8 & 0x3Fu) << 16);
        FDD_DBG("DMA base addr: 0x%06X", fdc.dma_base_addr);
        break;
    case DMA_BASE_MID:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0xFF00FFu) | ((uint32_t)v8 << 8);
        break;
    case DMA_BASE_LOW:
        fdc.dma_base_addr = (fdc.dma_base_addr & 0xFFFF00u) | v8;
        //FDD_DBG("DMA base addr: 0x%06X", fdc.dma_base_addr);
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

            /* Start motor */
            //fprintf (stderr, "[SXB] Motor running\n");
            motor_ticks[fdc.selected_drive] = MOTOR_OFF_TICKS;
            fdc.status |= FDC_STATUS_MOTORON;

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
    static int read_count = 0;
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
    int sector = fdc.sector_reg;
    int count  = fdc.dma_sector_count ? fdc.dma_sector_count : 1;

    //FDD_LOG("Read address: drive=%d track=%d side=%d", drv, track, side);

    if (drv < 0 || drv >= FDD_MAX_DRIVES || !fdc.drives[drv].disk_inserted) {
        fdc.status |= FDC_STATUS_RECNF;
        fdc.status &= ~FDC_STATUS_BUSY;
        fdc_raise_irq_immediate();
        return;
    }

    fdd_drive_t *d = &fdc.drives[drv];
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
