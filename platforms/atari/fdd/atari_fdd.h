/*
 * atari_fdd.h - Atari ST Floppy Disk Drive Emulator for PiStorm/Musashi
 *
 * Emulates the WD1772 FDC + ST DMA chip as seen by TOS/EmuTOS.
 * DMA transfers use ps_write_16/ps_read_16 to access real Atari ST RAM
 * directly via the GPIO/CPLD bus - no local RAM buffer needed.
 *
 * Hardware register map:
 *   $FF8604  r/w  FDC register data (muxed via DMA mode register)
 *   $FF8606  r    DMA status / w DMA mode control
 *   $FF8609  r/w  DMA base address high byte [23:16]
 *   $FF860B  r/w  DMA base address mid  byte [15:8]
 *   $FF860D  r/w  DMA base address low  byte [7:0]
 *   $FF8800  r/w  PSG register select
 *   $FF8802  w    PSG register data
 *
 * PSG Port A (register 14) drive/side select:
 *   bit 0: /DRIVE_B  0=drive B selected
 *   bit 1: /DRIVE_A  0=drive A selected  (yes, A is bit 1 - Atari HW quirk)
 *   bit 2: /SIDE1    0=side 1, 1=side 0
 *
 * EmuTOS flopio() DMA sequence:
 *   1. PSG reg14: select drive and side
 *   2. Write DMA base address ($FF8609/B/D)
 *   3. Toggle DMA R/W bit to clear status ($FF8606)
 *   4. Set SCREG mode, write sector count to $FF8604
 *   5. Clear SCREG, issue FDC command to $FF8604
 *   6. Poll MFP GPIP bit 5 ($FFFA01) until low (IRQ asserted)
 *   7. Read FDC status from $FF8604
 *   8. Read DMA sector count from $FF8604 (SCREG mode) - 0 = success
 */

#ifndef ATARI_FDD_H
#define ATARI_FDD_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* =========================================================================
 * Hardware Register Addresses
 * ========================================================================= */
#define FDC_DATA_REG        0xFF8604u
#define DMA_MODE_REG        0xFF8606u
#define DMA_BASE_HIGH       0xFF8609u
#define DMA_BASE_MID        0xFF860Bu
#define DMA_BASE_LOW        0xFF860Du
#define PSG_REG_SELECT      0xFF8800u
#define PSG_REG_WRITE       0xFF8802u
#define PSG_PORT_A_REG      14u
#define MFP_GPIP            0xFFFA01u

/* =========================================================================
 * DMA Mode Register ($FF8606 write)
 * ========================================================================= */
#define DMA_MODE_A0         (1u << 1)   /* FDC register A0 */
#define DMA_MODE_A1         (1u << 2)   /* FDC register A1 */
#define DMA_MODE_SCREG      (1u << 4)   /* 1=sector count, 0=FDC register */
#define DMA_MODE_RW         (1u << 8)   /* 1=read from disk, 0=write to disk */

/* DMA Status Register ($FF8606 read) */
#define DMA_STATUS_OK       (1u << 0)   /* 1=ok, 0=error */
#define DMA_STATUS_SCZERO   (1u << 1)   /* sector count is zero */
#define DMA_STATUS_DATARQ   (1u << 2)   /* FDC DRQ active */

/* =========================================================================
 * PSG Port A Bits
 * ========================================================================= */
/* Real Atari ST YM2149 port A wiring:
 *   bit 0 = floppy side select (0 -> side 1, 1 -> side 0)
 *   bit 1 = /drive A select    (0 -> drive A selected)
 *   bit 2 = /drive B select    (0 -> drive B selected)
 * (side and drive-B were previously swapped, which forced every access to
 *  side 0 whenever drive A was in use -> side-1 sectors read garbage.) */
#define PSG_SIDE_SEL        (1u << 0)   /* 0=side 1, 1=side 0 */
#define PSG_DRIVE_A_SEL     (1u << 1)   /* 0=drive A selected */
#define PSG_DRIVE_B_SEL     (1u << 2)   /* 0=drive B selected */

/* =========================================================================
 * WD1772 FDC Register Indices (DMA mode A1:A0)
 * ========================================================================= */
#define FDC_REG_CMD_STATUS  0u
#define FDC_REG_TRACK       1u
#define FDC_REG_SECTOR      2u
#define FDC_REG_DATA        3u

/* =========================================================================
 * WD1772 Commands (top nibble; lower nibble = flags/rate)
 * ========================================================================= */
#define FDC_CMD_RESTORE     0x00u
#define FDC_CMD_SEEK        0x10u
#define FDC_CMD_STEP        0x20u
#define FDC_CMD_STEP_IN     0x40u
#define FDC_CMD_STEP_OUT    0x60u
#define FDC_CMD_READ_SEC    0x80u
#define FDC_CMD_WRITE_SEC   0xA0u
#define FDC_CMD_READ_ADDR   0xC0u
#define FDC_CMD_FORCE_INT   0xD0u
#define FDC_CMD_READ_TRACK  0xE0u
#define FDC_CMD_WRITE_TRACK 0xF0u

/* =========================================================================
 * WD1772 Status Register Bits
 * ========================================================================= */
#define FDC_STATUS_BUSY     (1u << 0)
#define FDC_STATUS_INDEX    (1u << 1)   /* Type I: index pulse */
#define FDC_STATUS_DRQ      (1u << 1)   /* Type II/III: data request */
#define FDC_STATUS_TRACK0   (1u << 2)   /* Type I: head at track 0 */
#define FDC_STATUS_LOSTDATA (1u << 2)   /* Type II/III: lost data */
#define FDC_STATUS_CRCERR   (1u << 3)
#define FDC_STATUS_SEEKERR  (1u << 4)   /* Type I: seek error */
#define FDC_STATUS_RECNF    (1u << 4)   /* Type II/III: record not found */
#define FDC_STATUS_HEADLOAD (1u << 5)   /* Type I */
#define FDC_STATUS_RECTYPE  (1u << 5)   /* Type II: record type (deleted) */
#define FDC_STATUS_WRTPROT  (1u << 6)
#define FDC_STATUS_MOTORON  (1u << 7)

/* =========================================================================
 * Disk Geometry Constants
 * ========================================================================= */
#define FDD_MAX_DRIVES      2
#define FDD_MAX_SIDES       2
#define FDD_SECTOR_SIZE     512
#define FDD_TRACKS_80       80
#define FDD_DD_SECTORS      9
#define FDD_HD_SECTORS      18
#define FDD_IMAGE_SIZE_SS   (80 * 1 * 9 * 512)
#define FDD_IMAGE_SIZE_DD   (80 * 2 * 9 * 512)
#define FDD_IMAGE_SIZE_HD   (80 * 2 * 18 * 512)

/* Motor off timeout: ticks at fdd_vbl() call rate (50Hz = 100 ticks = 2s) */
#define MOTOR_OFF_TICKS     90 // 1.8 seconds

/* =========================================================================
 * Drive State Structure
 * ========================================================================= */
typedef enum {
    FDD_TYPE_NONE = 0,
    FDD_TYPE_DD,
    FDD_TYPE_HD
} fdd_type_t;

typedef struct {
    int         fd;
    char        image_path[256];
    bool        write_protected;
    bool        disk_inserted;
    bool        media_changed;
    fdd_type_t  type;
    int         current_track;
    int         sectors_per_track;
    int         num_tracks;
    int         num_sides;
    uint32_t    image_size;
    int         reserved_sectors;   /* actual reserved sectors from image scan */
} fdd_drive_t;

/* =========================================================================
 * FDC / DMA Controller State Structure
 * ========================================================================= */
typedef struct {
    /* WD1772 registers */
    uint8_t         status;
    uint8_t         command;
    uint8_t         track_reg;
    uint8_t         sector_reg;
    uint8_t         data_reg;

    /* DMA chip registers */
    uint16_t        dma_mode;
    uint32_t        dma_addr;
    uint16_t        dma_sector_count;
    uint8_t         dma_status;
    uint32_t        dma_base_addr;

    /* PSG port A shadow */
    uint8_t         psg_porta;
    uint8_t         psg_reg_sel;

    /* Decoded drive/side from PSG port A */
    int             selected_drive;     /* 0=A, 1=B, -1=none */
    int             selected_side;      /* 0 or 1 */

    /* IRQ state */
    bool            irq_pending;        /* command complete, IRQ asserted */
    bool            irq_ready;          /* IRQ now visible on GPIP polls */

    /* Drives */
    fdd_drive_t     drives[FDD_MAX_DRIVES];

    pthread_mutex_t lock;
} fdc_controller_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise - call once at startup */
void     fdd_init(void);

/* Shutdown - call at exit */
void     fdd_shutdown(void);

/* Mount/unmount disk images */
int      fdd_insert_disk(int drive, const char *image_path, bool write_protect);
void     fdd_eject_disk(int drive);
void     fdd_set_write_protect(int drive, bool wp);

/* Call from MFP GPIP read handler to get FDC interrupt state.
 * Returns other_gpip with bit 5 set correctly:
 *   bit 5 low  = FDC interrupt asserted
 *   bit 5 high = no interrupt */
uint8_t  fdd_gpip(uint8_t other_gpip);

/* Call from 50Hz VBL timer for motor-off timeout */
void     fdd_vbl(void);

/* Debug dump to stderr */
void     fdd_status(void);

bool     fdd_irq_active(void);

/* Memory dispatch - call from platform read/write handlers
 * when fdd_owns_address(addr) is true */
uint32_t fdd_io_read(uint32_t addr, int size);
void     fdd_io_write(uint32_t addr, uint32_t val, int size);

bool fdd_owns_address(uint32_t addr);

#ifdef __cplusplus
}
#endif
#endif /* ATARI_FDD_H */
