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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dma_snoop.h"
#include "atari_fdd.h"          /* FDC_DATA_REG, DMA_MODE_*, DMA_STATUS_* */

extern uint8_t  ps_read_8 (uint32_t addr);
extern uint16_t ps_read_16(uint32_t addr);
extern uint32_t ps_read_status_reg(void);
extern void     ps_write_status_reg(uint16_t value);

/* Sticky "M68K_BR_n has been asserted since reset", added to the firmware
 * 2026-08-16 because it is the one thing the Pi could not otherwise observe
 * - whether the DMA controller is even ASKING for the bus.
 *
 * BIT POSITION, and why it was wrong until 2026-08-16 (second fix):
 *   Firmware:  assign PI_D = trigger ? {PI_IPL2, PI_IPL1, M68K_RESET_n,
 *                                       M68K_HALT_n, br_seen, fwrev} : 16'bz;
 *              -> fwrev is PI_D[10:0], br_seen is PI_D[11].
 *   Host:      ps_read_status_reg() returns the RAW GPLEV0 word, and the
 *              data bus sits at GPIO 8..23 - see ps_protocol.c:422
 *              (ps_io->data = status >> 8) and :693
 *              (fw = (ps_read_status_reg() >> 8) & 0x07FF).
 *   Therefore  br_seen = PI_D[11] = raw status bit 19, not bit 11.
 *
 * The old 0x0800 mask read raw bit 11 = PI_D[3] = bit 3 of fwrev. For
 * fwrev 0x246 that bit is 0, so "[BR seen: no]" was a constant, not a
 * measurement. Every BR conclusion drawn from it is void.
 *
 * Reads 0 on firmware older than 0.70a, which is indistinguishable from
 * "never requested", so check the revision printed at INIT if it says no. */
#define STATUS_BR_SEEN  0x00080000u     /* PI_D[11] -> GPIO19 */
#define STATUS_BGACK_SEEN 0x00040000u   /* PSP2 CSR bit10 - sticky, tells
                                           us whether the BR requester ever
                                           completed a 3-wire handshake */
extern void     ps_write_8(uint32_t addr, uint16_t value);
extern void     pistorm_dma_to_stram  (uint32_t addr, const uint8_t *src, uint32_t n);
extern void     pistorm_dma_from_stram(uint32_t addr, uint8_t *dst, uint32_t n);
extern uint32_t p2_dbg2_counters(void);   /* FW 0x2A {br_edges,bgack_edges} */
static uint32_t gnt_arm_snap;
static uint64_t win_arm_us;
uint8_t snoop_isr_fdc_status;   /* last FDC status the GUEST read (ISR)  */
uint8_t snoop_isr_fdc_valid;
/* Deferred sync: a chained window is pulled AFTER the next command is
 * issued, overlapping the FDC's seek/stream latency instead of sitting
 * in the inter-sector gap (>1.4ms there = a lost 200ms revolution -
 * measured as 2s/track instead of ~0.4s). */
static uint32_t stash_base, stash_count;
static int      stash_pending;
static uint64_t stash_us_t;
#include <time.h>
static uint64_t snoop_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
}

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
static int      fdc_saw_busy;   /* BUSY observed set since arming - see the
                                   busy-clear completion trigger */
static int      gpip_seen_high; /* GPIP5 seen deasserted since arming - the
                                   high-then-low guard for the GPIP trigger */

static uint8_t  poison_byte;    /* pattern written before a read transfer */
static int      poison_ok;      /* window was poisoned, delta is meaningful */

static uint8_t  last_pull[512];         /* previous sector, for re-read compare */
static uint32_t last_pull_base;
static int      last_pull_valid;

static unsigned pulls;
static unsigned traced;

static void xfer_complete(void);        /* defined below the sync helpers */

/* Level 3 - the raw sequence, no interpretation. Everything above this file
 * derives state from these writes; if the derivation is wrong the derived
 * logs are worthless, so this prints what actually went past. Bounded so it
 * cannot flood.
 *
 * THE BOUND WAS 240 AND THAT WAS THE WRONG PLACE TO PUT IT. The comment used
 * to say "the interesting part is the first few command sequences". It is
 * not. Two boots of the identical config produced byte-identical traces for
 * all 240 lines - because 240 lines is entirely INQUIRY and READ CAPACITY,
 * which always work - and then one boot pulled six sectors and the other
 * pulled none. The whole difference lived past the cut, so the trace was
 * showing precisely the region that carries no information.
 *
 * Rule: bound the log where it stops being decisive, not where it starts
 * being long. */
#define TRACE_MAX 4000u

static const char *regname(uint32_t a)
{
    switch (a)
    {
        case FDC_DATA_REG:   return "DATA/COUNT $FF8604";
        case DMA_MODE_REG:   return "MODE       $FF8606";
        case DMA_BASE_HIGH:  return "BASE_HI    $FF8609";
        case DMA_BASE_MID:   return "BASE_MID   $FF860B";
        case DMA_BASE_LOW:   return "BASE_LO    $FF860D";
        case 0xFF8800u:      return "PSG_SEL    $FF8800";
        case 0xFF8802u:      return "PSG_DATA   $FF8802";
        default:             return "?";
    }
}

static void trace_access(const char *dir, uint32_t a, uint32_t v, int size)
{
    if (traced >= TRACE_MAX)
        return;
    traced++;
    SNOOP_LOG("%s %s %s=0x%04X sz=%d  [mode=0x%04X screg=%d hdc=%d %s count=%u]",
              (traced == TRACE_MAX) ? "trc*" : "trc ",
              dir, regname(a), v, size,
              dma_mode,
              (dma_mode & DMA_MODE_SCREG)   ? 1 : 0,
              (dma_mode & DMA_MODE_FDC_HDC) ? 1 : 0,
              (dma_mode & DMA_MODE_RW) ? "WRITE" : "read ",
              dma_count);
}

int dma_snoop_active(void)
{
    if (snoop_env < 0)
    {
        const char *e = getenv("PISTORM_DMA_SNOOP");
        snoop_env = (e && *e >= '1' && *e <= '4') ? (*e - '0') : 0;
        if (snoop_env)
        {
            /* install the bus-census gate in ps_protocol */
            extern int (*ps_xfer_active_hook)(void);
            ps_xfer_active_hook = dma_snoop_xfer_active;
        }
        if (snoop_env)
            SNOOP_LOG("real bus-master DMA mirror sync enabled%s",
                      snoop_env == 4 ? " (level 4: poison the destination and "
                                       "count bytes that actually arrive)" :
                      snoop_env == 3 ? " (level 3: full register trace)" :
                      snoop_env == 2 ? " (verbose: probes the DMA registers)" : "");
    }
    return snoop_env;
}

/* ---- bus-occupancy yield ------------------------------------------------
 *
 * THE MEASURED FAULT (2026-08-17): the WD1772 reports LOST DATA (status bit
 * 2) on floppy transfers - its byte was not collected within 32us. The DMA
 * FIFO drains through MMU slots, and those slots only exist when the bus is
 * free. Each PiStorm bus access occupies the bus for 1-2us versus ~500ns
 * for a real 68000 cycle (see the governor comment in cpu/events.cpp), and
 * during a transfer EmuTOS polls MFP GPIP continuously - each poll a full
 * bus transaction. The bus is busiest exactly when the DMA needs it free.
 * A real 68000 polls the same way but occupies the bus less than half as
 * long per poll, which is why the same disk works with a real CPU fitted.
 *
 * So: while a transfer is armed, space the polls out. The guest loses
 * nothing - it is spinning on a status bit - and every microsecond of gap
 * is a microsecond of MMU slots for the FIFO.
 *
 * PISTORM_DMA_YIELD_US tunes the gap (default 150us, 0 disables). A whole
 * sector at 32us/byte is ~16ms, so a 150us poll spacing still samples
 * completion 100+ times per sector - latency cost is negligible.
 *
 * Failure mode if win_armed ever sticks: polls stay spaced at yield_us
 * until the next transfer re-arms. Visible as sluggish register polling,
 * not a hang, and bounded by the next count write clearing the arm. */
static int yield_us = -1;

int dma_snoop_xfer_active(void)
{
    return snoop_env > 0 && win_armed;
}

void dma_snoop_poll_yield(void)
{
    if (yield_us < 0)
    {
        const char *e = getenv("PISTORM_DMA_YIELD_US");
        yield_us = e ? atoi(e) : 150;
        if (yield_us < 0)    yield_us = 0;
        if (yield_us > 5000) yield_us = 5000;
        if (snoop_env > 0)
            SNOOP_LOG("poll yield during armed transfers: %dus%s",
                      yield_us, yield_us ? "" : " (disabled)");
    }

    if (yield_us && dma_snoop_xfer_active())
        usleep((useconds_t)yield_us);
}

int dma_snoop_owns(uint32_t addr)
{
    /* $FF8800-03 is the PSG, and PORT A is where the floppy drive select,
     * side select and motor live - bit 0 side, bit 1 /drive A, bit 2 /drive
     * B. None of the DMA state machine below touches it (the switch has no
     * case for it, so it falls through to default and changes nothing); it
     * is included purely so the level-3 trace SEES it.
     *
     * Without this the trace shows only $FF8600-0F, which is everything
     * that happens AFTER a drive has been selected. If EmuTOS never selects
     * a drive, or selects the wrong one, the old trace looked identical to
     * a healthy machine that simply had no disk in it. */
    return (addr >= 0xFF8600u && addr <= 0xFF860Fu) ||
           (addr >= 0xFF8800u && addr <= 0xFF8803u);
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

    /* SECTOR RE-READ COMPARATOR.
     *
     * Two pulls of the SAME address should hold identical bytes. Comparing
     * them is the only test here that needs no knowledge of what the data is
     * supposed to be.
     *
     * It also detects the specific failure the logs point at: content
     * appearing one byte shifted between reads. The WD1772 hands over a byte
     * every 32us and the DMA has to take each one; miss a single DRQ and
     * every byte after it lands one position early for the rest of the
     * sector. The data still looks real, because it is - but the boot
     * checksum can never come out at $1234 and no directory parses. A byte
     * shift and random corruption need completely different fixes, so it is
     * worth knowing which this is. */

    if (!len || base >= STRAM_LIMIT || base + len > STRAM_LIMIT)
    {
        SNOOP_LOG("pull SKIPPED: base=0x%06X len=%u  << the reason the "
                  "mirror has a hole here", base, len);
        return;
    }

    for (off = 0; off < len; off += 512u)
    {
        uint32_t i;
        /* WORD reads: half the bus transactions of the old byte loop.
         * The pull is the per-sector hot path - every microsecond here
         * delays the loader's next command and risks missing the next
         * sector header (a miss costs a full 200ms revolution). */
        {
            extern void ps_read_block(uint32_t, uint8_t *, uint32_t);
            ps_read_block(base + off, buf, 256u);
        }
        (void)i;

        /* STABLE READS - the measured fix for a measured fault.
         *
         * Two back-to-back reads of the same unchanging RAM differed by
         * 28-69 bytes of 512, every sector, every boot: the Pi<->CPLD read
         * path corrupts 5-13% of bytes per read. That one number explains
         * the whole history - FINDINGS.md's "IACK vector ~4-5% dirty" was
         * this same fault through a different window, as were the $18-for-
         * $08 CDB, the floating registers, the impossible address-counter
         * deltas, and every "corrupt" sector: the DMA wrote RAM correctly
         * and the SYNC corrupted it on the way into the mirror.
         *
         * FINDINGS.md also validated the countermeasure: double-read-
         * confirmed "stable reads" never disagreed across weeks. So: any
         * byte whose two reads disagree is re-read until two CONSECUTIVE
         * reads agree (bounded). Independent errors at p~0.1 make two
         * matching wrong values ~1% per retry pair and vanishing across
         * rounds; agreement is overwhelmingly the true value.
         *
         * The bench fix is the read-capture timing itself (FINDINGS.md:
         * "scope the bus capture timing") - this makes the machine usable
         * until that happens. */
        if (snoop_env >= 3)
        {
            uint32_t unstable = 0, gaveup = 0;
            uint8_t  b2;

            for (i = 0; i < 512u; i++)
            {
                b2 = ps_read_8(base + off + i);

                if (b2 != buf[i])
                {
                    uint8_t r1, r2;
                    int     tries;

                    unstable++;

                    for (tries = 0; tries < 8; tries++)
                    {
                        r1 = ps_read_8(base + off + i);
                        r2 = ps_read_8(base + off + i);

                        if (r1 == r2)
                            break;
                    }

                    if (tries == 8)
                        gaveup++;

                    buf[i] = r1;
                }
            }

            if (unstable)
                SNOOP_LOG("vrfy: %u unstable byte%s stabilised by re-read%s",
                          unstable, unstable == 1 ? "" : "s",
                          gaveup ? "  << SOME NEVER SETTLED" : "");
            else
            {
                /* Positive confirmation, bounded: silence was ambiguous
                 * between "clean" and "not running" during the RD_SETTLE
                 * sweep. First few sectors only. */
                static unsigned clean_said;
                if (clean_said < 4)
                {
                    clean_said++;
                    SNOOP_LOG("vrfy: clean - both reads of all 512 bytes "
                              "identical");
                }
            }
        }

        /* DIRECTION/COLLECTION DETECTOR: the mirror holds the window's
         * PRE-transfer state. If the bus now holds the same bytes, the
         * "read" wrote nothing into RAM - the exact signature of the DMA
         * chip running the window in the WRITE direction (mode toggle
         * not taken) while the FDC's bytes fell on the floor. */
        {
            uint8_t prior[512]; uint32_t same = 0, i2;
            pistorm_dma_from_stram(base + off, prior, 512u);
            for (i2 = 0; i2 < 512u; i2++)
                if (prior[i2] == buf[i2]) same++;
            if (same >= 460u)
                SNOOP_LOG("dirn: window 0x%06X UNCHANGED from prior RAM "
                          "(%u/512 identical) << NOTHING WAS WRITTEN - "
                          "transfer likely ran in the WRITE direction",
                          base + off, same);
        }
        pistorm_dma_to_stram(base + off, buf, 512u);

        /* First few only: enough of the sector to judge a boot sector or an
         * AHDI/MBR root by eye. Silence here means the DMA never ran, which
         * is a different fault from the data being wrong. */
        if ((snoop_env >= 2 || pulls < 6) && off == 0)   /* env>=2: never
                          cap - a capped print cost us three debug rounds */
        {
            uint32_t j;

            pulls++;

            SNOOP_LOG("pull #%u: %u sector(s) -> 0x%06X | "
                      "%02X %02X %02X %02X %02X %02X %02X %02X "
                      "%02X %02X %02X %02X %02X %02X %02X %02X ... "
                      "sig %02X%02X",
                      pulls, count, base,
                      buf[0], buf[1], buf[2],  buf[3],
                      buf[4], buf[5], buf[6],  buf[7],
                      buf[8], buf[9], buf[10], buf[11],
                      buf[12], buf[13], buf[14], buf[15],
                      buf[0x1FE], buf[0x1FF]);

            /* ATARI BOOT CHECKSUM - the one test that validates all 512
             * bytes with a single number.
             *
             * TOS executes a boot sector only if its 256 big-endian words
             * sum to exactly $1234 (mod 65536). So for a bootable disk -
             * a Gotek/FlashFloppy selector image, a game, anything that is
             * supposed to boot - this is a complete integrity check of the
             * whole sector. One wrong byte anywhere and it fails, and TOS
             * silently declines to boot, which is exactly the symptom.
             *
             * This replaces a BPB decode that was actively misleading: those
             * offsets only mean anything on sector 0, so every FAT,
             * directory and program sector was being reported as "NOT a
             * valid BPB" when nothing was wrong with it. Only sector 0 is
             * checked against $1234; the BPB is printed only when it is
             * plausible enough to be worth reading. */
            {
                uint32_t sum = 0;

                for (j = 0; j < 512u; j += 2)
                    sum += (uint32_t)((buf[j] << 8) | buf[j + 1]);

                sum &= 0xFFFFu;

                /* same address as last time? then we can compare */
            if (base == last_pull_base && last_pull_valid)
            {
                uint32_t diff = 0, k;
                int      shift, best = 0, best_match = -1;

                for (k = 0; k < 512u; k++)
                    if (buf[k] != last_pull[k])
                        diff++;

                if (diff)
                {
                    /* try aligning at +/-4 bytes and see if it snaps */
                    for (shift = -4; shift <= 4; shift++)
                    {
                        uint32_t m = 0;

                        for (k = 0; k < 512u; k++)
                        {
                            int32_t src = (int32_t)k + shift;

                            if (src >= 0 && src < 512 &&
                                buf[k] == last_pull[src])
                                m++;
                        }

                        if ((int)m > best_match)
                        {
                            best_match = (int)m;
                            best       = shift;
                        }
                    }
                }

                if (!diff)
                    SNOOP_LOG("      re-read of 0x%06X: IDENTICAL "
                              "(%u bytes) - the read is repeatable",
                              base, 512u);
                else if (best != 0 && best_match > 400)
                    SNOOP_LOG("      re-read of 0x%06X: %u bytes differ, but "
                              "%d of 512 match at a %+d byte SHIFT"
                              "   << dropped/duplicated DRQ, not corruption",
                              base, diff, best_match, best);
                else
                    SNOOP_LOG("      re-read of 0x%06X: %u of 512 bytes "
                              "differ, no shift alignment"
                              "   << random corruption, not slippage",
                              base, diff);
            }

            memcpy(last_pull, buf, 512u);
            last_pull_base  = base;
            last_pull_valid = 1;

            SNOOP_LOG("      boot checksum = $%04X%s", sum,
                          (sum == 0x1234u)
                            ? "   << $1234: bootable, all 512 bytes correct"
                            : "   (only $1234 boots; other values are normal"
                              " for non-boot sectors)");
            }

            if ((buf[11] | (buf[12] << 8)) == 512u)
                SNOOP_LOG("      bpb: sec/clus=%u reserved=%u fats=%u "
                          "rootent=%u total=%u media=$%02X sec/fat=%u "
                          "sec/trk=%u heads=%u",
                          (unsigned)buf[13],
                          (unsigned)(buf[14] | (buf[15] << 8)),
                          (unsigned)buf[16],
                          (unsigned)(buf[17] | (buf[18] << 8)),
                          (unsigned)(buf[19] | (buf[20] << 8)),
                          (unsigned)buf[21],
                          (unsigned)(buf[22] | (buf[23] << 8)),
                          (unsigned)(buf[24] | (buf[25] << 8)),
                          (unsigned)(buf[26] | (buf[27] << 8)));

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
    {
        SNOOP_LOG("pull SKIPPED: base=0x%06X len=%u  << the reason the "
                  "mirror has a hole here", base, len);
        return;
    }

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

    /* A LONGWORD access at $FF8604 spans TWO registers: the high word goes
     * to $FF8604 and the low word to $FF8606. That is the standard AHDI
     * idiom - write an ACSI command byte and the next DMA mode in one bus
     * cycle - and it is what the guest actually does. Treating it as a
     * single 32-bit write to the data register threw the mode half away and
     * left every derived mode/count value wrong from that point on. Split
     * it and handle each half as the hardware does. */
    if (size == 4)
    {
        dma_snoop_write(addr,     (val >> 16) & 0xFFFFu, 2);
        dma_snoop_write(addr + 2,  val        & 0xFFFFu, 2);
        return;
    }

    /* $FF8604 and $FF8606 are WORD registers - a byte access lands on the
     * odd half ($FF8605 / $FF8607). Normalise so the switch below sees the
     * register rather than the half. The base registers are already odd
     * byte addresses and need no adjustment. */
    if (addr == 0xFF8605u || addr == 0xFF8607u)
        addr &= ~1u;
    if (size == 2)
        val &= 0xFFFFu;

    if (snoop_env >= 3)     /* >= : level 4 is level 3 PLUS the poison
                             * check. Gating on equality meant the higher
                             * level showed LESS - a level-4 run printed no
                             * register trace at all. Third time this exact
                             * mistake has cost a boot in this file; the
                             * others were the arm: and done: lines. */
        trace_access("W", addr, val, size);

    /* PSG port A decode. Register 14 is port A; on the ST that is
     *   bit 0  side select     (0 = side 1, 1 = side 0)
     *   bit 1  /drive A select (0 = selected)
     *   bit 2  /drive B select (0 = selected)
     * A floppy access that never shows "drive A" here never reached the
     * drive at all, whatever the FDC registers afterwards look like. */
    if (snoop_env >= 2 && addr >= 0xFF8800u && addr <= 0xFF8803u)
    {
        static uint32_t psg_reg = 0xFFu;
        static uint32_t last_porta = 0xFFFFu;

        if (stash_pending)          /* load ended / idle: safe to sync */
        {
            stash_pending = 0;
            sync_pull(stash_base, stash_count);
        }

        if (addr == 0xFF8800u)
            psg_reg = val & 0x0Fu;

        else if (psg_reg == 14u && (val & 0xFFu) != last_porta)
        {
            last_porta = val & 0xFFu;
            SNOOP_LOG("psg : port A = 0x%02X  [%s%s side %d]",
                      (unsigned)last_porta,
                      (last_porta & 0x02u) ? "" : "drive A ",
                      (last_porta & 0x04u) ? "" : "drive B ",
                      (last_porta & 0x01u) ? 0 : 1);

            if ((last_porta & 0x06u) == 0x06u)
                SNOOP_LOG("psg : no drive selected");
        }

        return;         /* nothing below applies to the PSG */
    }

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

                /* A NEW COUNT while a READ window is still armed is
                 * ITSELF the completion signal - sync the old window before
                 * re-arming, do not just drop it.
                 *
                 * The Gotek menu loader proved why: it is INTERRUPT-driven.
                 * Between issuing READ SECTOR and programming the next
                 * sector there are no polls at all - no GPIP, no FDC
                 * status. Completion arrives as the MFP vector, the ISR
                 * sets up the next sector, and the only externally visible
                 * sign that sector N finished is sector N+1 being
                 * programmed. The old code cleared win_armed here without
                 * syncing, so every chained sector's data stayed stale in
                 * the mirror and the loader jumped into garbage. */
                if (win_armed && !win_is_write)
                {
                    /* xfer_complete() consumes dma_count - but the count
                     * this very write just latched belongs to the NEXT
                     * window.  Losing it here meant the following command
                     * byte found dma_count==0 and never armed: every
                     * OTHER sector went unsynced (the 0x400-stepped pull
                     * addresses), and count-only chained loads collapsed
                     * to no pulls at all (13 dry chn triggers, FF menu
                     * dead after 6 pulls). Preserve it across the sync. */
                    uint32_t next_count = dma_count;
                    if (snoop_env >= 2)
                        SNOOP_LOG("chn : next transfer programmed - window "
                                  "queued for overlapped sync");
                    /* DO NOT sync here: this is the loader's inter-sector
                     * gap. Queue it; the pull happens after the next
                     * command is already running (see arm branch). */
                    stash_base    = win_base;
                    stash_count   = win_count;
                    stash_pending = 1;
                    win_armed = 0;
                    dma_count = next_count;
                }
                win_armed = 0;
                poison_ok = 0;
            }
            /* !win_armed: arm ONCE per transfer, on the FIRST command byte.
             * A 6-byte ACSI CDB is six writes here with the count still
             * live, so this used to re-arm and re-probe on every one of
             * them - six extra REAL BUS reads injected into the middle of
             * every command block, from the code whose job is to observe it
             * without disturbing it. */
            else if (dma_count && !win_armed)
            {
                /* command byte with a live count - the transfer starts now.
                 * Snapshot the window here: the base may legally be written
                 * after the count, so only this moment is reliable. */
                /* Arm from the chip's LIVE counter: in chained loads the
                 * ISR often programs only the sector count and lets the
                 * address counter run on - the shadowed dma_base then
                 * points at a long-finished window. The live counter is
                 * correct in both styles. Falls back to the shadow if the
                 * probe returns something outside ST-RAM. */
                {
                    uint32_t live = read_dma_base();
                    win_base = (live && live < STRAM_LIMIT) ? live : dma_base;
                }
                win_count    = dma_count;
                win_is_write = (dma_mode & DMA_MODE_RW) != 0;
                win_armed    = 1;
                if (snoop_env >= 2) gnt_arm_snap = p2_dbg2_counters();
                win_arm_us   = snoop_us();
                if (snoop_env >= 2)
                    SNOOP_LOG("arm : base=0x%06X count=%u %s",
                              win_base, win_count,
                              win_is_write ? "WRITE" : "READ");
                fdc_saw_busy = 0;
                gpip_seen_high = 0;

                /* Overlapped sync: the FDC command for THIS window has
                 * already gone out (this hook runs after the bus write).
                 * The disk needs 8-20ms before any byte arrives; pull
                 * the PREVIOUS window now, hidden inside that latency. */
                if (stash_pending)
                {
                    stash_pending = 0;
                    sync_pull(stash_base, stash_count);
                }

                /* Re-arm the sticky BR flag so it means "BR during THIS
                 * transfer" instead of "BR at any point since reset".
                 *
                 * It was reading YES on every done: line, which is not
                 * evidence - the flag latches on the first assertion and
                 * TOS asserts plenty during boot, so a single early BR
                 * would pin it high for the rest of the session. The
                 * firmware exposes status[2] as a Pi-clearable re-arm
                 * exactly for this: write 0x0004 to clear, 0x0000 to
                 * release. status[1:0] stay 00 so HALT/RESET/INIT are
                 * untouched. */
                if (snoop_env >= 2)
                {
                    ps_write_status_reg(0x0004u);
                    ps_write_status_reg(0x0000u);
                }

                /* WRITE-PATH CHECK. The read path is fixed (RD_SETTLE), so
                 * readbacks are now trustworthy - use them to verify the
                 * WRITE direction, which the $08->$18 CDB corruption proved
                 * is also suspect. If the guest's DMA base register write
                 * was corrupted on the wire, the transfer lands at the
                 * wrong RAM address: completion looks normal, the
                 * programmed window reads back all zeros. That is exactly
                 * the all-zero sector seen after the lens was fixed. Read
                 * the base registers back and compare with what the guest
                 * wrote. */
                {
                    uint32_t hw = read_dma_base();

                    if (hw != win_base)
                        SNOOP_LOG("wchk: BASE MISMATCH - guest wrote "
                                  "0x%06X, hardware holds 0x%06X"
                                  "   << write-path corruption, transfer "
                                  "will land at the wrong address",
                                  win_base, hw);
                    else
                    {
                        static unsigned ok_said;
                        if (ok_said < 3)
                        {
                            ok_said++;
                            SNOOP_LOG("wchk: base verified 0x%06X", hw);
                        }
                    }
                }

                /* LEVEL 4 - poison the destination before a read.
                 *
                 * The address counter read back from $FF8609/0B/0D cannot be
                 * trusted. On one boot it reported a 1-sector transfer
                 * advancing by 6 bytes, by 7500, and by 38584 - the last two
                 * are impossible, a sector is 512. Those registers are being
                 * read as three separate byte cycles on a bus whose undriven
                 * halves hold stale values, and one "after" address was
                 * exactly the base a different transfer had used.
                 *
                 * So stop asking the chip and look at the RAM. Write a known
                 * pattern across the window first; afterwards, count how many
                 * bytes differ. Bytes that changed came from the device -
                 * nothing else writes there - and that is not inferable from
                 * any register.
                 *
                 * Level 4 only: it costs count*512 real bus writes per
                 * transfer, and it does overwrite the buffer the guest is
                 * about to be given. Diagnostic, not for normal running. */
                poison_ok = 0;

                if (snoop_env >= 4 && !win_is_write &&
                    win_count && win_base < STRAM_LIMIT &&
                    win_base + win_count * 512u <= STRAM_LIMIT)
                {
                    uint32_t i, n = win_count * 512u;

                    poison_byte = (uint8_t)(0xA5u ^ (win_base & 0xFFu));

                    for (i = 0; i < n; i++)
                        ps_write_8(win_base + i, poison_byte);

                    poison_ok = 1;
                }

                if (snoop_env >= 2)     /* was == 2: level 3 lost the arm
                                           line as well as the done line */
                {
                    /* $FF8606 read: only bits 0-2 exist and all three are
                     * ACTIVE LOW. Bits 3-7 are undefined bus float - masking
                     * 0xFF printed 0xF3 / 0x33 / 0x07 for what is really the
                     * same state three times, which made noise look like
                     * signal. Mask to the register.
                     *
                     *   bit 0 = 0 -> DMA error
                     *   bit 1 = 0 -> sector count IS zero
                     *   bit 2     -> FDC DRQ
                     *
                     * So "the count did not take" is bit 1 CLEAR. The old
                     * test was inverted and fired on every transfer. */
                    uint32_t st = ps_read_16(DMA_MODE_REG) & 0x07u;
                    SNOOP_LOG("arm : base=0x%06X count=%u %-5s status=0x%02X"
                              " (err=%s cnt=%s)%s",
                              win_base, win_count,
                              win_is_write ? "WRITE" : "READ", st,
                              (st & DMA_STATUS_OK)     ? "ok"   : "ERROR",
                              (st & DMA_STATUS_SCZERO) ? "live" : "zero",
                              !(st & DMA_STATUS_SCZERO)
                                ? "   << count reads ZERO at arm time - it did not take"
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


/* Completion tail, shared by every trigger. MUST run before the guest can
 * act on whatever told it the transfer finished - the Gotek panic was
 * exactly this ordering lost: the loader saw GPIP5 drop, jumped to its
 * load address, and executed stale mirror before any sync ran. Every
 * caller is inside a bus-access handler, so calling this before returning
 * the value to the guest restores the guarantee. */
/* Bus census collected by ps_protocol.c while the window was armed. */
extern volatile uint32_t ps_xfer_census[8];

static void xfer_complete(void)
{
    win_armed = 0;

    /* PRODUCTION (snoop=1): never sync inline - the caller may be the
     * loader's EOI write sitting in the inter-sector gap, where >1.4ms
     * costs a 200ms revolution. Queue; drained at the next command
     * (chains) or the next PSG write (load end / motor off). */
    if (snoop_env == 1 && !win_is_write)
    {
        if (stash_pending)                 /* two pending: flush older */
            sync_pull(stash_base, stash_count);
        stash_base = win_base; stash_count = win_count;
        stash_pending = 1; stash_us_t = snoop_us();
        dma_count = 0;
        return;
    }

    if (stash_pending)              /* older chained window still queued */
    {
        stash_pending = 0;
        sync_pull(stash_base, stash_count);
    }

    /* GRANT TELEMETRY (fw 0x2A): ~32 BR bursts move a healthy 512-byte
     * window. Dead window + ~32 grants = bus ran, data went the wrong
     * way. Dead window + 0 grants = the DMA never engaged its RAM side. */
    if (snoop_env >= 2)
    {
        uint32_t g = p2_dbg2_counters();
        /* The FDC verdict comes from the GUEST's OWN ISR status read
         * (snooped, zero extra bus traffic). The previous version did a
         * bare $8604 read here - with mode=$90 at chn time that read the
         * WRONG register through the mux ($FF float = fake RNF/CRC/LOST
         * on every window) and risked INTRQ. Instrument error, mine. */
        uint64_t dur = snoop_us() - win_arm_us;
        SNOOP_LOG("gnt : BR+%u BGACK+%u  isr-fdc=%s$%02X%s%s%s  dur=%ums",
                  ((g >> 8) - (gnt_arm_snap >> 8)) & 0xFFu,
                  (g - gnt_arm_snap) & 0xFFu,
                  snoop_isr_fdc_valid ? "" : "(none)",
                  snoop_isr_fdc_status,
                  (snoop_isr_fdc_status & 0x10u) ? " RNF"  : "",
                  (snoop_isr_fdc_status & 0x08u) ? " CRC"  : "",
                  (snoop_isr_fdc_status & 0x04u) ? " LOST" : "",
                  (unsigned)(dur / 1000u));
        snoop_isr_fdc_valid = 0; snoop_isr_fdc_status = 0;
    }

    {
        static const char *rn[8] =
            { "ram", "dma", "psg", "mfp", "acia", "vid", "rom", "oth" };
        uint32_t total = 0, i;
        char line[160];
        int  n = 0;

        for (i = 0; i < 8; i++)
            total += ps_xfer_census[i];

        /* The sync's own reads land in the census too - everything below
         * runs after this dump so the numbers are the TRANSFER's, not
         * ours. Anything nonzero here occupied the bus while the WD1772
         * was delivering a byte every 32us. */
        if (total)
        {
            /* PI_D[11] (fw 0.76a+): sticky "watchdog fired since clear",
             * cleared at arm by the same status[2] pulse that used to
             * re-arm BR. Set here = at least one bus cycle stalled ~256us
             * during THIS transfer = ~8 floppy bytes lost per firing.
             * On 0.70a-0.75a this bit is BR and reads 0 - harmless. */
            uint32_t sr = ps_read_status_reg();
            n += snprintf(line + n, sizeof line - (size_t)n,
                          "  WD=%s", (sr & STATUS_BR_SEEN) ? "FIRED" : "no");

            for (i = 0; i < 8; i++)
                if (ps_xfer_census[i])
                    n += snprintf(line + n, sizeof line - (size_t)n,
                                  " %s=%u", rn[i], ps_xfer_census[i]);

            SNOOP_LOG("bus : %u access%s during transfer:%s",
                      total, total == 1 ? "" : "es", line);
        }

        for (i = 0; i < 8; i++)
            ps_xfer_census[i] = 0;
    }

    /* >= 2, not == 2. Level 3 is level 2 plus the raw register trace,
     * so gating this on equality made the MORE verbose level report
     * LESS about completion. */
    if (snoop_env >= 2)
    {
        uint32_t after  = read_dma_base();
        uint32_t expect = win_base + win_count * 512u;
        uint32_t sr = ps_read_status_reg();
        SNOOP_LOG("done: address 0x%06X -> 0x%06X (expected 0x%06X)  %s  [BR:%s BGACK:%s]",
                  win_base, after, expect,
                  (after == win_base)
                    ? "<< DID NOT MOVE - no DMA took place"
                    : (after == expect ? "<< advanced correctly"
                                       : "<< advanced, but not by the expected amount"),
                  (sr & STATUS_BR_SEEN) ? "YES" : "no",
                  (sr & STATUS_BGACK_SEEN) ? "YES" : "no");
    }

    /* Did any byte actually arrive? Read the real bus, not a register. */
    if (poison_ok)
    {
        uint32_t i, n = win_count * 512u, moved = 0, first = 0xFFFFFFFFu;

        for (i = 0; i < n; i++)
        {
            if (ps_read_8(win_base + i) != poison_byte)
            {
                if (moved == 0)
                    first = i;

                moved++;
            }
        }

        SNOOP_LOG("data: %u of %u bytes changed in ST-RAM%s%s",
                  moved, n,
                  moved ? "" : "   << NOTHING ARRIVED",
                  ( moved && first != 0 ) ? "   << but not from offset 0"
                                          : "");
        poison_ok = 0;
    }

    if (!win_is_write)
    {
        /* Let the DMA's final FIFO chunk land before reading the window.
         *
         * The WD1772 raises INTRQ when the COMMAND completes; the DMA is
         * still flushing its last <=16-byte FIFO chunk into RAM at that
         * moment. A real 68000 needs tens of microseconds to reach the
         * ISR, so the flush always wins the race. This sync runs INSIDE
         * the very bus access that signalled completion - earlier than
         * any real CPU could - so without the settle it can capture a
         * sector whose tail bytes have not arrived yet. Forty clean-
         * looking sectors with a handful of stale tail bytes is exactly
         * an "Illegal Instruction mid-run" crash, which is what the
         * Gotek menu did after otherwise loading perfectly. */
        static int settle_us = -1;
        if (settle_us < 0)
        {
            const char *e = getenv("PISTORM_DMA_SETTLE_US");
            settle_us = e ? atoi(e) : 150;
            if (settle_us < 0)    settle_us = 0;
            if (settle_us > 5000) settle_us = 5000;
        }
        if (settle_us)
            usleep((useconds_t)settle_us);

        /* PULL WHAT THE HARDWARE MOVED, not what the count register said.
         * Bench (PSP2, FF menu loader): pulls stepped 0x400 apart with
         * "count=1" - every other sector was landing in a window the
         * shadow never described, and once the loader switched to
         * count-only chaining the pulls stopped entirely while 13 chn
         * triggers fired dry.  The DMA chip's live address counter is
         * ground truth for how many bytes reached RAM this window. */
        {
            uint32_t live = read_dma_base();
            uint32_t span_sec = win_count;
            if (live > win_base && ((live - win_base) & 511u) == 0u &&
                live - win_base <= MAX_SYNC_SECTORS * 512u)
            {
                span_sec = (live - win_base) / 512u;
                if (span_sec != win_count)
                    SNOOP_LOG("span: hw moved %u sector(s), count said %u - "
                              "pulling what actually landed", span_sec,
                              win_count);
            }
            sync_pull(win_base, span_sec);
        }
    }
    dma_count = 0;
}

/* GPIP poll while a transfer is armed. Called from the natmem MFP getters
 * with the RAW hardware byte (pre-shim: the IDE shim forces bit 5 low when
 * its own interrupt is pending, which would fake a completion). GPIP5 is
 * the FDC/HDC interrupt, active low: high-then-low since arming means the
 * transfer finished, and the mirror is synced HERE, inside the read, so
 * the guest cannot see "done" before the data it will jump to is real.
 * The high-first requirement stops a stale low at arm time firing it. */
/* MFP ISRB write ($FFFA11). An interrupt-driven disk ISR ends with a
 * software end-of-interrupt: clearing bit 7 (channel 7 = GPIP5 = FDC/HDC).
 * That write is the ISR saying "I have handled this disk interrupt" - the
 * one externally visible completion signal an interrupt-driven loader
 * gives for its LAST sector, which has no follow-on transfer to catch.
 * Bit 7 must be CLEAR in the written value (bclr / move.b #$7F idiom);
 * other channels' EOIs leave bit 7 set and are ignored. */
void dma_snoop_mfp_eoi(uint32_t val)
{
    if (win_armed && snoop_env > 0 && !(val & 0x80u))
    {
        /* GUARD, added after the trigger itself caused the corruption it
         * was meant to prevent. A bclr #n,$FFFA11 from ANY interrupt's
         * EOI writes back bit 7 as it currently reads - usually 0 - so
         * EmuTOS's 200Hz Timer C fired this mid-transfer, synced a
         * half-written sector, and disarmed the window so the real
         * completion never re-synced it. Whether a tick landed inside a
         * transfer varied per run: the Gotek menu loaded once in many
         * attempts, at every settle value, which is exactly a 200Hz
         * dice roll against a 16ms transfer.
         *
         * The DMA chip itself arbitrates: $FF8606 bit 1 is ACTIVE LOW
         * "sector count is zero". Count still live = transfer still
         * running = this EOI belongs to someone else. One bus read,
         * hardware-authoritative, no heuristics. */
        uint16_t st = ps_read_16(DMA_MODE_REG);

        if (st & DMA_STATUS_SCZERO)
        {
            if (snoop_env >= 3)
                SNOOP_LOG("eoi : ignored - DMA count still live "
                          "(another channel's EOI)");
            return;
        }

        SNOOP_LOG("eoi : MFP channel 7 end-of-interrupt (disk ISR done)");
        xfer_complete();
    }
}

void dma_snoop_gpip_poll(uint32_t raw)
{
    /* POLLED completion consumers (TOS boot, directory reads) check the
     * mirror immediately after seeing GPIP low, with no intervening I/O.
     * Their own poll is therefore the LAST safe drain point - and polls
     * never occur inside a chained loader's inter-sector gap, so this
     * costs chained loads nothing. (Deferring past this point broke
     * floppy boot: TOS checksummed a stale mirror and gave up.) */
    if (stash_pending)
    {
        stash_pending = 0;
        sync_pull(stash_base, stash_count);
    }
    if (!win_armed || snoop_env <= 0)
        return;

    /* Level 3: show the polls themselves, bounded. If the next panic log
     * has NO "gpip poll" lines between arming and the crash, the loader is
     * not polling GPIP at all - it is jumping on a counted delay, and the
     * fix is eager sync rather than another trigger. The absence of these
     * lines is the finding. */
    if (snoop_env >= 3)
    {
        static unsigned gpip_polls;
        if (gpip_polls < 40)
        {
            gpip_polls++;
            SNOOP_LOG("gpip poll: $%02X (bit5 %s)", (unsigned)(raw & 0xFFu),
                      (raw & 0x20u) ? "high" : "LOW");
        }
    }

    if (raw & 0x20u)
        gpip_seen_high = 1;
    else if (gpip_seen_high)
    {
        if (snoop_env >= 2) SNOOP_LOG("gpip: IRQ asserted (end-of-transfer)");
        xfer_complete();
    }
}

void dma_snoop_read(uint32_t addr, uint32_t val, int size)
{
    /* Any snooped I/O read (FDC/DMA status etc.) precedes data use in
     * every polled/checked flow - drain here too. Chained loaders issue
     * no reads between sectors, so their gap stays clear. */
    if (stash_pending)
    {
        stash_pending = 0;
        sync_pull(stash_base, stash_count);
    }
    (void) size;

    if (!dma_snoop_active())
        return;
    if (!win_armed && snoop_env < 3)
        return;

    /* A LONGWORD access at $FF8604 spans TWO registers: the high word goes
     * to $FF8604 and the low word to $FF8606. That is the standard AHDI
     * idiom - write an ACSI command byte and the next DMA mode in one bus
     * cycle - and it is what the guest actually does. Treating it as a
     * single 32-bit write to the data register threw the mode half away and
     * left every derived mode/count value wrong from that point on. Split
     * it and handle each half as the hardware does. */
    if (size == 4)
    {
        dma_snoop_read(addr,     (val >> 16) & 0xFFFFu, 2);
        dma_snoop_read(addr + 2,  val        & 0xFFFFu, 2);
        return;
    }

    /* same word-register normalisation as the write path */
    if (addr == 0xFF8605u || addr == 0xFF8607u)
        addr &= ~1u;

    if (snoop_env >= 3)     /* >= : level 4 is level 3 PLUS the poison
                             * check. Gating on equality meant the higher
                             * level showed LESS - a level-4 run printed no
                             * register trace at all. Third time this exact
                             * mistake has cost a boot in this file; the
                             * others were the arm: and done: lines. */
        trace_access("R", addr, val, size);

    if (addr >= 0xFF8800u && addr <= 0xFF8803u)
        return;

    /* ---- COMPLETION ------------------------------------------------------
     *
     * PRIMARY TRIGGER: the guest's read of the ACSI status byte at $FF8604
     * in HDC mode. A full untruncated trace of an EmuTOS boot contains SIX
     * of these and ZERO reads of $FF8606 - the driver never polls the DMA
     * status register at all. The old trigger waited on an access that does
     * not happen, which is why a boot could arm repeatedly and never pull.
     *
     * This is also what atari_fdd.c already keys off, calling it "the
     * mandatory end-of-command handshake every driver performs (it releases
     * the device IRQ), and always before the data is touched". Two
     * implementations of one idea; this one now agrees with the other.
     *
     * The high byte of that read is undriven bus - ACSI status is D0-D7 -
     * so it is not examined here.
     *
     * SECONDARY TRIGGER, below: sector count reached zero. Kept for the
     * WD1772 path and any driver that does poll $FF8606. */
    if (win_armed && addr == FDC_DATA_REG &&
        (dma_mode & DMA_MODE_FDC_HDC) && !(dma_mode & DMA_MODE_SCREG))
    {
        SNOOP_LOG("hs  : ACSI status read $FF8604=0x%02X (end-of-command)",
                  val & 0xFFu);
        xfer_complete();
        return;
    }

    /* WD1772 BUSY -> CLEAR : completion for loaders that are not EmuTOS.
     *
     * The two existing completion triggers are EmuTOS idioms: the ACSI
     * status-byte read, and the DMA sector-count readback. A boot-sector
     * loader uses NEITHER - it polls the WD1772 status register and gets on
     * with it. First seen with the Gotek menu: its boot sector arrived
     * byte-perfect (checksum $1234), TOS executed it, the loader pulled the
     * menu into high RAM over real DMA - and no trigger fired, so the
     * mirror was never synced, the guest executed stale bytes at $3F2800,
     * ran off the top of RAM and Line-F panicked at $3FFFFC.
     *
     * The WD1772's own completion signal is status bit 0 (BUSY) going
     * clear. Requiring BUSY to have been SEEN SET since arming prevents a
     * premature fire on a poll that lands before the chip has started. */
    if (win_armed && addr == FDC_DATA_REG &&
        !(dma_mode & DMA_MODE_FDC_HDC) && !(dma_mode & DMA_MODE_SCREG))
    {
        if (val & 0x01u)
            fdc_saw_busy = 1;
        else if (fdc_saw_busy)
        {
            SNOOP_LOG("fdc : BUSY cleared (end-of-transfer, status $%02X)",
                      (unsigned)(val & 0xFFu));
            xfer_complete();
            return;
        }
    }

    /* WD1772 ERROR BITS - only reported when one is actually set.
     *
     * A read of $FF8604 with the HDC bit clear and SCREG clear is an FDC
     * register read, and after a Type II command that is the status. The
     * chip records exactly the failure the data points at:
     *
     *   bit 2 LOST DATA - the DMA did not take a byte before the next one
     *                     arrived. The WD1772 delivers a byte every 32us and
     *                     will not wait. Miss one and every byte after it
     *                     lands one position early for the rest of the
     *                     sector - which is why a boot sector reads as
     *                     coherent 68000 code that still fails its checksum,
     *                     and why "60 34" turns up at offset 1 instead of 0.
     *   bit 3 CRC ERROR - the bytes arrived but one is wrong.
     *   bit 4 RNF       - the sector was never found.
     *
     * These three distinguish "too slow" from "corrupted" from "wrong
     * place", and they need different fixes. Silent means none were set. */
    if (addr == FDC_DATA_REG &&
        !(dma_mode & DMA_MODE_FDC_HDC) && !(dma_mode & DMA_MODE_SCREG))
    {
        extern uint8_t snoop_isr_fdc_status;   /* per-window, guest's view */
        extern uint8_t snoop_isr_fdc_valid;
        snoop_isr_fdc_status = (uint8_t)val;
        snoop_isr_fdc_valid  = 1;
    }
    if (addr == FDC_DATA_REG &&
        !(dma_mode & DMA_MODE_FDC_HDC) && !(dma_mode & DMA_MODE_SCREG) &&
        ((val & 0x1Cu) != 0))
    {
        SNOOP_LOG("fdc : status $%02X -%s%s%s", (unsigned)(val & 0xFFu),
                  (val & 0x04u) ? "  LOST DATA (DMA too slow - byte dropped,"
                                  " rest of sector shifts)" : "",
                  (val & 0x08u) ? "  CRC ERROR" : "",
                  (val & 0x10u) ? "  RECORD NOT FOUND" : "");
    }

    /* FLOPPY (real WD1772 / Gotek) end-of-transfer. The ACSI handshake above
     * does not exist on this path - there is no command-status byte - and
     * the boot trace shows the driver does not read $FF8606 either. What it
     * DOES do is read back the DMA sector count: $FF8604 with SCREG set,
     * and zero means the transfer finished. atari_fdd.c records the same
     * thing at its line 746 - "EmuTOS get_dma_status() reads $FF8604 in
     * SCREG mode to get the sector count".
     *
     * Non-zero is a poll of a transfer still in flight, so only zero
     * completes. Both real devices now report through one path. */
    if (win_armed && addr == FDC_DATA_REG &&
        (dma_mode & DMA_MODE_SCREG) && (val & 0xFFu) == 0)
    {
        SNOOP_LOG("sc0 : sector count read back zero (end-of-transfer)");
        xfer_complete();
        return;
    }

    /* Sector count zero. POLARITY: bits 0-2 of $FF8606-read are ACTIVE LOW,
     * exactly as this file's neighbour documents bit 0 - "1=ok, 0=error".
     * Bit 1 reads 0 WHEN THE COUNT IS ZERO, so completion is the bit being
     * CLEAR, not set.
     *
     * The old test had it inverted, which cost two separate wrong answers:
     * the arm-time probe reported "SCZERO ALREADY SET - the count did not
     * take" on every single transfer (bit 1 was set because the count HAD
     * taken), and completion fired while the count was still non-zero -
     * i.e. before the data arrived. That is the signature seen in the first
     * working run: pulls #1 and #2 at an identical address with identical
     * contents and #3-#6 all zeros. Those were not completed transfers,
     * they were pulls firing early and copying stale RAM. */
    if (win_armed && addr == DMA_MODE_REG && !(val & DMA_STATUS_SCZERO))
        xfer_complete();
}
