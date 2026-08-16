/*
 * dma_snoop.c - keep the JIT mirror coherent with REAL bus-master DMA.
 * See dma_snoop.h for why this is needed.
 *
 * Scope, deliberately narrow: this fixes the DEVICE -> GUEST direction,
 * which is the broken one. Guest -> device already works in the common
 * configuration because stram_needs_bus_write() returns 1 unconditionally
 * when "stram_cache" is disabled, so guest writes are already on the real
 * bus. With stram_cache ENABLED that is no longer true, so the push side
 * is implemented too.
 *
 * This does not touch the bus arbitration problem. Both faults sit on the
 * same path and fixing either alone shows no improvement: arbitration
 * broken means no DMA happens to sync, coherency broken means the DMA that
 * did happen stays invisible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dma_snoop.h"
#include "atari_fdd.h"          /* FDC_DATA_REG, DMA_MODE_*, DMA_STATUS_* */

extern uint8_t  ps_read_8 (uint32_t addr);
extern void     ps_write_8(uint32_t addr, uint16_t value);
extern void     pistorm_dma_to_stram  (uint32_t addr, const uint8_t *src, uint32_t n);
extern void     pistorm_dma_from_stram(uint32_t addr, uint8_t *dst, uint32_t n);

/* NOTE: this file deliberately does NOT reference FDD_enabled. It is called
 * only from the branch where the emulator is NOT handling the registers
 * itself, so the condition is already established by the call site - and a
 * `bool` shared between this (C) and emulator.c (C++) is an ABI question
 * not worth having. */

#define SNOOP_LOG(fmt, ...) fprintf(stderr, "[DMA] " fmt "\n", ##__VA_ARGS__)

#define STRAM_LIMIT         0x400000u
/* Same bound the emulated path uses: 128 KB per sync pass. A transfer
 * larger than this is reported rather than silently half-copied. */
#define MAX_SYNC_SECTORS    256u

static int      snoop_env = -1;         /* -1 = not yet read; 2 = verbose */

static uint32_t dma_base;               /* $FF8609/0B/0D */
static uint32_t dma_mode;               /* $FF8606        */
static uint32_t dma_count;              /* $FF8604 when SCREG selected */

static uint32_t win_base;               /* armed transfer window */
static uint32_t win_count;
static int      win_is_write;           /* RAM -> device */
static int      win_armed;

static unsigned pulls;

int dma_snoop_active(void)
{
    if (snoop_env < 0)
    {
        const char *e = getenv("PISTORM_DMA_SNOOP");
        snoop_env = (e && *e == '2') ? 2 : ((e && *e == '1') ? 1 : 0);
        if (snoop_env)
            SNOOP_LOG("real bus-master DMA mirror sync enabled%s",
                      snoop_env == 2 ? " (verbose: probes the DMA registers)" : "");
    }
    return snoop_env;
}

int dma_snoop_owns(uint32_t addr)
{
    return addr >= 0xFF8600u && addr <= 0xFF860Fu;
}

/* The ST's DMA address counter INCREMENTS as the controller transfers. So
 * reading it back after the fact answers "did any DMA actually happen?"
 * without depending on how the status register is interpreted. Only used at
 * verbose level: these are extra reads of live hardware registers, and the
 * guest is polling them too. */
static uint32_t read_dma_base(void)
{
    uint32_t h = ps_read_8(DMA_BASE_HIGH);
    uint32_t m = ps_read_8(DMA_BASE_MID);
    uint32_t l = ps_read_8(DMA_BASE_LOW);
    return ((h & 0xFFu) << 16) | ((m & 0xFFu) << 8) | (l & 0xFFu);
}

/* ---- the sync itself ------------------------------------------------- */

/* Device wrote real ST-RAM. Copy that window into the mirror so the guest
 * can see it. Read the REAL bus here - that is the whole point. */
static void sync_pull(uint32_t base, uint32_t count)
{
    uint8_t  buf[512];
    uint32_t len, off;

    if (count > MAX_SYNC_SECTORS)
    {
        SNOOP_LOG("transfer of %u sectors exceeds the %u-sector sync bound "
                  "- mirror sync capped, guest will see only the first %u KB",
                  count, MAX_SYNC_SECTORS, (MAX_SYNC_SECTORS * 512u) / 1024u);
        count = MAX_SYNC_SECTORS;
    }

    len = count * 512u;
    if (!len || base >= STRAM_LIMIT || base + len > STRAM_LIMIT)
        return;

    for (off = 0; off < len; off += 512u)
    {
        uint32_t i;
        for (i = 0; i < 512u; i++)
            buf[i] = ps_read_8(base + off + i);
        pistorm_dma_to_stram(base + off, buf, 512u);

        /* First few only: enough of the sector to judge a boot sector or an
         * AHDI/MBR root by eye. Silence here means the DMA never ran, which
         * is a different fault from the data being wrong. */
        if (pulls < 6 && off == 0)
        {
            pulls++;
            SNOOP_LOG("pull #%u: %u sector(s) -> 0x%06X | "
                      "%02X %02X %02X %02X %02X %02X %02X %02X ... sig %02X%02X",
                      pulls, count, base,
                      buf[0], buf[1], buf[2], buf[3],
                      buf[4], buf[5], buf[6], buf[7],
                      buf[0x1FE], buf[0x1FF]);
        }
    }
}

/* Guest buffer -> device. Only necessary when stram_cache is enabled, since
 * otherwise every guest write already went through to the bus. Reads the
 * MIRROR (the CPU's authoritative copy) and puts it on the real bus. */
static void sync_push(uint32_t base, uint32_t count)
{
    uint8_t  buf[512];
    uint32_t len, off;

    if (count > MAX_SYNC_SECTORS)
        count = MAX_SYNC_SECTORS;

    len = count * 512u;
    if (!len || base >= STRAM_LIMIT || base + len > STRAM_LIMIT)
        return;

    for (off = 0; off < len; off += 512u)
    {
        uint32_t i;
        pistorm_dma_from_stram(base + off, buf, 512u);
        for (i = 0; i < 512u; i++)
            ps_write_8(base + off + i, buf[i]);
    }
}

/* ---- register snooping ----------------------------------------------- */

void dma_snoop_write(uint32_t addr, uint32_t val, int size)
{
    if (!dma_snoop_active() || !dma_snoop_owns(addr))
        return;

    /* $FF8604 and $FF8606 are WORD registers - a byte access lands on the
     * odd half ($FF8605 / $FF8607). Normalise so the switch below sees the
     * register rather than the half. The base registers are already odd
     * byte addresses and need no adjustment. */
    if (addr == 0xFF8605u || addr == 0xFF8607u)
        addr &= ~1u;
    if (size == 2)
        val &= 0xFFFFu;

    switch (addr)
    {
        case DMA_BASE_HIGH:
            dma_base = (dma_base & 0x00FFFFu) | ((val & 0xFFu) << 16);
            break;
        case DMA_BASE_MID:
            dma_base = (dma_base & 0xFF00FFu) | ((val & 0xFFu) << 8);
            break;
        case DMA_BASE_LOW:
            dma_base = (dma_base & 0xFFFF00u) | (val & 0xFFu);
            break;

        case DMA_MODE_REG:
            dma_mode = val;
            break;

        case FDC_DATA_REG:
            if (dma_mode & DMA_MODE_SCREG)
            {
                /* sector count written: the next command byte starts it */
                dma_count = val & 0xFFu;
            }
            else if (dma_count)
            {
                /* command byte with a live count - the transfer starts now.
                 * Snapshot the window here: the base may legally be written
                 * after the count, so only this moment is reliable. */
                win_base     = dma_base;
                win_count    = dma_count;
                win_is_write = (dma_mode & DMA_MODE_RW) != 0;
                win_armed    = 1;

                if (snoop_env == 2)
                {
                    /* If SCZERO is ALREADY set at the moment we arm, the
                     * count never reached the hardware - and any later pull
                     * is firing on a stale zero rather than on a completed
                     * transfer. That distinction decides whether the DMA is
                     * running at all. */
                    uint32_t st = ps_read_8(0xFF8607u);
                    SNOOP_LOG("arm : base=0x%06X count=%u %-5s status=0x%02X%s",
                              win_base, win_count,
                              win_is_write ? "WRITE" : "READ", st,
                              (st & DMA_STATUS_SCZERO)
                                ? "   << SCZERO ALREADY SET - the count did not take"
                                : "");
                }

                if (win_is_write)
                    sync_push(win_base, win_count);
            }
            break;

        default:
            break;
    }
}

void dma_snoop_read(uint32_t addr, uint32_t val, int size)
{
    (void) size;

    if (!dma_snoop_active() || !win_armed)
        return;

    /* same word-register normalisation as the write path */
    if (addr == 0xFF8605u || addr == 0xFF8607u)
        addr &= ~1u;

    /* Completion: the DMA status register reports sector count zero. This is
     * how the hardware itself signals done, so it works for both the WD1772
     * and an ACSI device, and it fires once rather than on every status poll
     * during the transfer. */
    if (addr == DMA_MODE_REG && (val & DMA_STATUS_SCZERO))
    {
        win_armed = 0;

        if (snoop_env == 2)
        {
            uint32_t after  = read_dma_base();
            uint32_t expect = win_base + win_count * 512u;
            SNOOP_LOG("done: address 0x%06X -> 0x%06X (expected 0x%06X)  %s",
                      win_base, after, expect,
                      (after == win_base)
                        ? "<< DID NOT MOVE - no DMA took place"
                        : (after == expect ? "<< advanced correctly"
                                           : "<< advanced, but not by the expected amount"));
        }

        if (!win_is_write)
            sync_pull(win_base, win_count);
        dma_count = 0;
    }
}
