// SPDX-License-Identifier: MIT
/* See falcon_tos.h. */
#include "falcon_tos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* MC146818 RTC / NVRAM                                                 */
/* ------------------------------------------------------------------ */
#define RB_DM    0x04      /* reg B: binary (1) or BCD (0) data        */
#define RB_24H   0x02      /* reg B: 24-hour clock                     */
#define RB_SET   0x80      /* reg B: clock frozen while it is set      */
#define RA_UIP   0x80      /* reg A: update in progress                */
#define RD_VRT   0x80      /* reg D: RAM and time valid (battery good) */
#define NV_FIRST 14        /* NVRAM 14..63; checksum over 14..61       */
#define NV_CKS   62

static uint8_t  g_rtc[64];
static uint8_t  g_idx;
static int      g_loaded;
static long     g_offset;          /* guest time - host time, seconds  */
static uint8_t  g_set[10];         /* time fields written under SET    */

static const char *nv_path(void)
{
    const char *e = getenv("PISTORM_FALCON_NVRAM");
    return (e && *e) ? e : "falcon_nvram.bin";
}

static void nv_checksum(void)
{
    uint8_t s = 0;
    for (int i = NV_FIRST; i < NV_CKS; i++)
        s += g_rtc[i];
    g_rtc[NV_CKS] = (uint8_t)~s;
    g_rtc[NV_CKS + 1] = s;
}

static void nv_save(void)
{
    FILE *f = fopen(nv_path(), "wb");
    if (!f)
        return;
    fwrite(g_rtc + NV_FIRST, 1, 64 - NV_FIRST, f);
    fclose(f);
}

/* First use: the saved NVRAM, else a Falcon's defaults - UK language
 * and keyboard, 24h DD/MM/YY with '/', VGA 640x480 16 colours ($001A). */
static void rtc_init(void)
{
    if (g_loaded)
        return;
    g_loaded = 1;
    memset(g_rtc, 0, sizeof g_rtc);
    g_rtc[10] = 0x26;                  /* 32.768 kHz base, 1024 Hz rate  */
    g_rtc[11] = RB_DM | RB_24H;
    g_rtc[13] = RD_VRT;
    FILE *f = fopen(nv_path(), "rb");
    if (f && fread(g_rtc + NV_FIRST, 1, 64 - NV_FIRST, f) == 64 - NV_FIRST) {
        fclose(f);
        fprintf(stderr, "[FALCON] NVRAM from %s\n", nv_path());
        return;
    }
    if (f)
        fclose(f);
    memset(g_rtc + NV_FIRST, 0, 64 - NV_FIRST);
    g_rtc[20] = 3;                     /* language: UK                   */
    g_rtc[21] = 3;                     /* keyboard: UK                   */
    g_rtc[22] = 0x11;                  /* 24h, DD/MM/YY                  */
    g_rtc[23] = '/';                   /* date separator                 */
    g_rtc[24] = 0x20;                  /* boot delay                     */
    g_rtc[25] = 1;
    g_rtc[26] = 0xFF;
    g_rtc[28] = 0x00;                  /* boot video mode $001A:         */
    g_rtc[29] = 0x1A;                  /* VGA 640x480x16                 */
    g_rtc[30] = 0x87;                  /* SCSI id 7, no arbitration      */
    nv_checksum();
    nv_save();
    fprintf(stderr, "[FALCON] NVRAM: Falcon defaults written to %s\n", nv_path());
}

static struct tm *rtc_now(void)
{
    time_t t = time(NULL) + g_offset;
    return localtime(&t);
}

static uint8_t rtc_out(int v)
{
    if (g_rtc[11] & RB_DM)
        return (uint8_t)v;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int rtc_in(uint8_t v)
{
    if (g_rtc[11] & RB_DM)
        return v;
    return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
}

static uint8_t rtc_read(uint8_t i)
{
    rtc_init();
    struct tm *t = rtc_now();
    switch (i) {
    case 0: return rtc_out(t->tm_sec);
    case 2: return rtc_out(t->tm_min);
    case 4:
        if (g_rtc[11] & RB_24H)
            return rtc_out(t->tm_hour);
        {
            int h = t->tm_hour % 12;
            return (uint8_t)(rtc_out(h ? h : 12) | (t->tm_hour >= 12 ? 0x80 : 0));
        }
    case 6: return rtc_out(t->tm_wday + 1);
    case 7: return rtc_out(t->tm_mday);
    case 8: return rtc_out(t->tm_mon + 1);
    case 9: return rtc_out(t->tm_year - 68);      /* years since 1968   */
    case 10:
        /* UIP: toggled so a program waiting for an update sees one */
        g_rtc[10] = (g_rtc[11] & RB_SET) ? (uint8_t)(g_rtc[10] & ~RA_UIP)
                                         : (uint8_t)(g_rtc[10] ^ RA_UIP);
        return g_rtc[10];
    case 12: {
        uint8_t v = g_rtc[12];
        g_rtc[12] = 0;                 /* flags clear on read            */
        return v;
    }
    case 13: return RD_VRT;
    default: return g_rtc[i & 63];
    }
}

/* the clock fields written under SET become the guest's time when SET
 * is cleared: kept as an offset from the Pi's clock */
static void rtc_commit_set(void)
{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_sec  = rtc_in(g_set[0]);
    t.tm_min  = rtc_in(g_set[2]);
    int h = rtc_in((uint8_t)(g_set[4] & 0x7F));
    if (!(g_rtc[11] & RB_24H))
        h = (h % 12) + ((g_set[4] & 0x80) ? 12 : 0);
    t.tm_hour = h;
    t.tm_mday = rtc_in(g_set[7]);
    t.tm_mon  = rtc_in(g_set[8]) - 1;
    t.tm_year = rtc_in(g_set[9]) + 68;
    t.tm_isdst = -1;
    time_t g = mktime(&t);
    if (g != (time_t)-1)
        g_offset = (long)(g - time(NULL));
}

static void rtc_write(uint8_t i, uint8_t v)
{
    rtc_init();
    i &= 63;
    if (i <= 9) {
        if (i == 0 || i == 2 || i == 4 || i == 6 || i == 7 || i == 8 || i == 9) {
            if (!(g_rtc[11] & RB_SET)) {
                /* a write without SET: take the clock's current fields
                 * as the base, change this one, apply at once */
                struct tm *t = rtc_now();
                g_set[0] = rtc_out(t->tm_sec);  g_set[2] = rtc_out(t->tm_min);
                g_set[4] = rtc_out(t->tm_hour); g_set[7] = rtc_out(t->tm_mday);
                g_set[8] = rtc_out(t->tm_mon + 1); g_set[9] = rtc_out(t->tm_year - 68);
                g_set[i] = v;
                rtc_commit_set();
            } else {
                g_set[i] = v;
            }
        } else {
            g_rtc[i] = v;                          /* alarms             */
        }
        return;
    }
    switch (i) {
    case 10: g_rtc[10] = (uint8_t)((g_rtc[10] & RA_UIP) | (v & 0x7F)); return;
    case 11: {
        int was_set = g_rtc[11] & RB_SET;
        if ((v & RB_SET) && !was_set) {            /* freeze: snapshot   */
            struct tm *t = rtc_now();
            g_set[0] = rtc_out(t->tm_sec);  g_set[2] = rtc_out(t->tm_min);
            g_set[4] = rtc_out(t->tm_hour); g_set[7] = rtc_out(t->tm_mday);
            g_set[8] = rtc_out(t->tm_mon + 1); g_set[9] = rtc_out(t->tm_year - 68);
        }
        g_rtc[11] = v;
        if (was_set && !(v & RB_SET))
            rtc_commit_set();
        return;
    }
    case 12: case 13: return;                      /* read-only          */
    default:
        if (g_rtc[i] != v) {
            g_rtc[i] = v;
            nv_save();                             /* settings persist   */
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Z85C30 SCC: there, but nothing behind it                             */
/* ------------------------------------------------------------------ */
static uint8_t g_scc_ptr[2];

static uint8_t scc_read(uint32_t o)
{
    int ch = (o >> 2) & 1;                         /* $81/$83 A, $85/$87 B */
    if (o & 2)                                     /* data               */
        return 0;
    uint8_t r = g_scc_ptr[ch];
    g_scc_ptr[ch] = 0;
    /* RR0: Tx buffer empty, DCD/CTS up; nothing received */
    return r == 0 ? 0x2C : 0;
}

static void scc_write(uint32_t o, uint8_t v)
{
    int ch = (o >> 2) & 1;
    if (o & 2)
        return;                                    /* data: dropped      */
    if (g_scc_ptr[ch] == 0)
        g_scc_ptr[ch] = (uint8_t)(v & 7) | (((v >> 3) & 7) == 1 ? 8 : 0);
    else
        g_scc_ptr[ch] = 0;                         /* register written   */
}

/* ------------------------------------------------------------------ */
/* NCR 5380 SCSI: an empty bus                                          */
/* ------------------------------------------------------------------ */
/* The Falcon's DMA chip routes $FF8604 to the 5380 when the mode word
 * has bit 3 set and bit 4 clear (bits 2-0 = the 5380 register), as the
 * ST routes it to ACSI. No target is fitted. Modelled on Hatari's
 * ncr5380.c with no devices, register for register - a driver checks
 * its own writes and the chip's handshakes, not just "BSY never rises":
 *   0 CSD   the bus data: what we drive while arbitrating, else 0
 *   1 ICR   reads back as written, with AIP (6) / LA (5) owned by the
 *           chip; RST (7) resets the chip (ICR = $80, IRQ set)
 *   2 MODE  reads back; arbitrate (bit 0) rising sets AIP, falling
 *           clears it - an empty bus is always won at once
 *   3 TCR   reads back
 *   4 CSBS  the bus lines: no target, so only RST (from ICR bit 7)
 *   5 BSR   IRQ (4); phase match never (the bus is free), no DRQ
 *   6 IDR   0
 *   7 reading clears the IRQ
 * Writes to 5-7 start DMA: nothing to talk to. HDDRIVER arbitrates
 * before every selection; with AIP never set (everything read 0) each
 * one ran to its timeout and the scan crawled, then hung. The mode
 * word itself still goes to the real chip: the floppy shares it. */
static uint16_t g_dmamode;
static int      g_scsi_logged;
static uint8_t  g_ncr[8];
static int      g_ncr_irq;

static int scsi_selected(void)
{
    return (g_dmamode & 0x18u) == 0x08u;
}

static void ncr_reset(void)
{
    memset(g_ncr, 0, sizeof g_ncr);
    g_ncr[1] = 0x80;
    g_ncr_irq = 1;
}

static uint8_t ncr_read(int reg)
{
    switch (reg) {
    case 0: return (g_ncr[2] & 1) ? g_ncr[0] : 0;     /* arbitrating     */
    case 1: return g_ncr[1];
    case 2: return g_ncr[2];
    case 3: return g_ncr[3];
    case 4: return (uint8_t)(g_ncr[1] & 0x80);         /* RST only        */
    case 5: return g_ncr_irq ? 0x10 : 0x00;
    case 6: return 0;
    default:                                           /* 7: reset IRQ    */
        g_ncr_irq = 0;
        return g_ncr[7];
    }
}

static void ncr_write(int reg, uint8_t v)
{
    uint8_t old = g_ncr[reg];
    switch (reg) {
    case 1:
        g_ncr[1] = (uint8_t)((v & ~0x60u) | (old & 0x60u));   /* AIP, LA */
        if (v & 0x80)
            ncr_reset();
        break;
    case 2:
        g_ncr[2] = v;
        if ((v & 1) && !(old & 1))
            g_ncr[1] = (uint8_t)((g_ncr[1] | 0x40u) & ~0x20u);   /* AIP  */
        else if (!(v & 1) && (old & 1))
            g_ncr[1] &= (uint8_t)~0x40u;
        break;
    case 5:
        break;                         /* start DMA send: keeps BSR     */
    default:
        g_ncr[reg] = v;
        break;
    }
}

void falcon_tos_snoop(uint32_t a, uint32_t v, int size)
{
    if (!emulator_falcon_tos())
        return;
    a &= 0x00FFFFFFu;
    if (size == 2 && a == 0xFF8606u)
        g_dmamode = (uint16_t)v;
    else if (size == 1 && a == 0xFF8607u)
        g_dmamode = (uint16_t)((g_dmamode & 0xFF00u) | (v & 0xFFu));
    /* a long at $FF8604 is split into its two words by the caller, so
     * the data byte meets the 5380 register the OLD mode selected */
}

static void scsi_note(void)
{
    if (!g_scsi_logged) {
        g_scsi_logged = 1;
        fprintf(stderr, "[FALCON] SCSI: NCR 5380 probed - empty bus "
                        "(no targets), no ACSI traffic\n");
    }
}

/* ------------------------------------------------------------------ */
int falcon_tos_owns(uint32_t a)
{
    if (!emulator_falcon_tos())
        return 0;
    a &= 0x00FFFFFFu;
    return (a >= 0xFF8960u && a < 0xFF8964u) ||
           (a >= 0xFF8C80u && a < 0xFF8C88u) ||
           (a == 0xFF860Eu || a == 0xFF860Fu) ||
           ((a == 0xFF8604u || a == 0xFF8605u) && scsi_selected());
}

static uint8_t read8(uint32_t a)
{
    if (a == 0xFF8604u) return 0;      /* high byte of the data word    */
    if (a == 0xFF8605u) { scsi_note(); return ncr_read(g_dmamode & 7); }
    if (a == 0xFF8961u) return g_idx;
    if (a == 0xFF8963u) return rtc_read(g_idx);
    if (a >= 0xFF8C80u && a < 0xFF8C88u) return scc_read(a & 7u);
    return 0;
}

static void write8(uint32_t a, uint8_t v)
{
    if (a == 0xFF8604u) return;
    if (a == 0xFF8605u) { scsi_note(); ncr_write(g_dmamode & 7, v); return; }
    if (a == 0xFF8961u) { g_idx = v & 63; return; }
    if (a == 0xFF8963u) { rtc_write(g_idx, v); return; }
    if (a >= 0xFF8C80u && a < 0xFF8C88u) { scc_write(a & 7u, v); return; }
}

uint32_t falcon_tos_read(uint32_t a, int size)
{
    uint32_t v = 0;
    a &= 0x00FFFFFFu;
    for (int i = 0; i < size; i++)
        v = (v << 8) | read8(a + (uint32_t)i);
    return v;
}

void falcon_tos_write(uint32_t a, uint32_t v, int size)
{
    a &= 0x00FFFFFFu;
    for (int i = 0; i < size; i++)
        write8(a + (uint32_t)i, (uint8_t)(v >> (8 * (size - 1 - i))));
}

void falcon_tos_reset(void)
{
    g_rtc[11] &= (uint8_t)~0x78;       /* SQWE and interrupt enables     */
    g_rtc[12] = 0;
    g_idx = 0;
    g_scc_ptr[0] = g_scc_ptr[1] = 0;
    g_dmamode = 0;
    memset(g_ncr, 0, sizeof g_ncr);    /* RESET line: the 5380 too      */
    g_ncr_irq = 0;
}
