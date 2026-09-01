/*
 * mfp_hub.c — single home for virtual MFP interrupt state.
 * See mfp_hub.h for the why and the model. Register/channel map:
 *
 *   ch 15..8 : set A - IERA $FFFA07, IPRA $0B, ISRA $0F, IMRA $13
 *   ch  7..0 : set B - IERB $FFFA09, IPRB $0D, ISRB $11, IMRB $15
 *   VR $FFFA17 (bit3 = software-EOI), TACR $FFFA19, TADR $FFFA1F
 *
 * All 16-bit channel masks here use bit N == channel N.
 */

#include "mfp_hub.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* PISTORM_MFP_HUB_DEBUG=1: budgeted trace of raises/IACKs/EOIs plus a
 * once-per-1024-arbitrations state line. Free when off (cached getenv). */
static int hub_dbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_MFP_HUB_DEBUG");
        v = (e && *e == '1') ? 1 : 0;
    }
    return v;
}

/* ---- register shadows (guest-programmed) ------------------------------ */
/* Boot defaults: everything enabled/unmasked, software EOI - the old
 * per-source shadows booted the same way so injected events before TOS
 * programs the MFP are not silently dropped. */
static _Atomic uint16_t g_ier = 0xFFFF;      /* enable   (A<<8 | B)      */
static _Atomic uint16_t g_imr = 0xFFFF;      /* mask     (A<<8 | B)      */
static _Atomic uint16_t g_vpend = 0;         /* virtual pending latches  */
static _Atomic uint16_t g_visr  = 0;         /* virtual in-service bits  */
static _Atomic uint8_t  g_vr    = 0x48;      /* vector reg, S=1 (sw EOI) */

/* Timer A event-count mode (dmasnd frame counter feeds it) */
static _Atomic uint8_t  g_tacr  = 0;
static _Atomic uint32_t g_tadr  = 256;       /* 0 counts as 256          */
static _Atomic uint32_t g_tacnt = 256;

/* level sources: polled at arbitration; NULL = event channel */
static int (*g_level_poll[16])(void);

void mfp_hub_register_level(int ch, int (*poll)(void))
{
    if (ch >= 0 && ch < 16)
        g_level_poll[ch] = poll;
}

/* ---- guest write snoop ------------------------------------------------ */

static void reg_write(uint32_t a, uint8_t b)
{
    switch (a) {
    case 0x00FFFA07u: {                      /* IERA (ch 15..8)          */
        uint16_t ier = (uint16_t)((atomic_load(&g_ier) & 0x00FF) | (b << 8));
        atomic_store(&g_ier, ier);
        /* 68901: disabling a channel also clears its pending latch */
        atomic_fetch_and(&g_vpend, ier);
        break;
    }
    case 0x00FFFA09u: {                      /* IERB (ch 7..0)           */
        uint16_t ier = (uint16_t)((atomic_load(&g_ier) & 0xFF00) | b);
        atomic_store(&g_ier, ier);
        atomic_fetch_and(&g_vpend, ier);
        break;
    }
    case 0x00FFFA0Bu:                        /* IPRA: written 0s clear   */
        atomic_fetch_and(&g_vpend, (uint16_t)((b << 8) | 0x00FF));
        break;
    case 0x00FFFA0Du:                        /* IPRB                     */
        atomic_fetch_and(&g_vpend, (uint16_t)(0xFF00 | b));
        break;
    case 0x00FFFA0Fu:                        /* ISRA: written 0s = EOI   */
        atomic_fetch_and(&g_visr, (uint16_t)((b << 8) | 0x00FF));
        break;
    case 0x00FFFA11u:                        /* ISRB                     */
        atomic_fetch_and(&g_visr, (uint16_t)(0xFF00 | b));
        break;
    case 0x00FFFA13u:                        /* IMRA                     */
        atomic_store(&g_imr, (uint16_t)((atomic_load(&g_imr) & 0x00FF) | (b << 8)));
        break;
    case 0x00FFFA15u:                        /* IMRB                     */
        atomic_store(&g_imr, (uint16_t)((atomic_load(&g_imr) & 0xFF00) | b));
        break;
    case 0x00FFFA17u:                        /* VR                       */
        atomic_store(&g_vr, b);
        if ((b & 0x08u) == 0)                /* auto-EOI: nothing held   */
            atomic_store(&g_visr, 0);
        break;
    case 0x00FFFA19u:                        /* TACR: reprogram re-arms  */
        atomic_store(&g_tacr, (uint8_t)(b & 0x0Fu));
        atomic_store(&g_tacnt, atomic_load(&g_tadr));
        break;
    case 0x00FFFA1Fu:                        /* TADR: 0 counts as 256    */
        atomic_store(&g_tadr, b ? b : 256u);
        atomic_store(&g_tacnt, b ? b : 256u);
        break;
    default:
        break;
    }
}

void mfp_hub_write_snoop(uint32_t addr, uint32_t value, int is_word)
{
    uint32_t a = addr & 0x00FFFFFFu;
    if (a < 0x00FFFA00u || a > 0x00FFFA2Fu)
        return;
    if (is_word) {
        /* register byte is on the odd address; a word write covers it */
        reg_write(a | 1u, (uint8_t)(value & 0xFF));
    } else {
        reg_write(a, (uint8_t)(value & 0xFF));
    }
}

/* ---- guest read shim -------------------------------------------------- */

/* pending picture including live level sources (a level channel is
 * pending exactly while its line asserts - it never latches) */
static uint16_t pending_now(void)
{
    uint16_t pend = atomic_load(&g_vpend);
    for (int ch = 0; ch < 16; ch++) {
        if (g_level_poll[ch] && g_level_poll[ch]())
            pend |= (uint16_t)(1u << ch);
    }
    return (uint16_t)(pend & atomic_load(&g_ier));
}

uint8_t mfp_hub_read_shim(uint32_t addr, uint8_t real)
{
    switch (addr & 0x00FFFFFFu) {
    case 0x00FFFA0Bu:                        /* IPRA                     */
        return (uint8_t)(real | (pending_now() >> 8));
    case 0x00FFFA0Du:                        /* IPRB                     */
        return (uint8_t)(real | (pending_now() & 0xFF));
    case 0x00FFFA0Fu:                        /* ISRA                     */
        return (uint8_t)(real | (atomic_load(&g_visr) >> 8));
    case 0x00FFFA11u:                        /* ISRB                     */
        return (uint8_t)(real | (atomic_load(&g_visr) & 0xFF));
    default:
        return real;
    }
}

/* ---- sources ---------------------------------------------------------- */

void mfp_hub_raise(int ch)
{
    if (ch < 0 || ch > 15)
        return;
    uint16_t bit = (uint16_t)(1u << ch);
    /* only latch if the channel is enabled - 68901 drops events on a
     * disabled channel rather than queueing them */
    if (atomic_load(&g_ier) & bit)
        atomic_fetch_or(&g_vpend, bit);
}

void mfp_hub_timer_a_event(void)
{
    if (atomic_load(&g_tacr) != 8u) {        /* only event-count mode    */
        if (hub_dbg()) {
            static int shown;
            if (shown < 4) {
                shown++;
                fprintf(stderr, "[mfphub] timerA event IGNORED: tacr=%02X "
                        "(not event-count mode 8)\n", atomic_load(&g_tacr));
            }
        }
        return;
    }
    uint32_t c = atomic_load(&g_tacnt);
    if (c <= 1u) {
        atomic_store(&g_tacnt, atomic_load(&g_tadr));
        if (hub_dbg()) {
            static int shown;
            if (shown < 16) {
                shown++;
                fprintf(stderr, "[mfphub] timerA count expired -> raise ch13 "
                        "(ier=%04X imr=%04X vpend=%04X visr=%04X)\n",
                        atomic_load(&g_ier), atomic_load(&g_imr),
                        atomic_load(&g_vpend), atomic_load(&g_visr));
            }
        }
        mfp_hub_raise(13);
    } else {
        atomic_store(&g_tacnt, c - 1u);
    }
}

/* ---- arbitration ------------------------------------------------------ */

/* deliverable-channel mask under the full rule; also refreshes level
 * sources into the pending picture (without latching them - a level
 * source is pending exactly while it asserts) */
static uint16_t deliverable(void)
{
    uint16_t pend = pending_now();
    pend &= atomic_load(&g_imr);
    if (!pend)
        return 0;

    /* priority: nothing at or below the highest in-service channel may
     * interrupt (this is what makes handler-side IPL games safe on the
     * real chip - only HIGHER channels preempt) */
    uint16_t isr = atomic_load(&g_visr);
    if (isr) {
        int top = 15;
        while (top >= 0 && !(isr & (1u << top)))
            top--;
        /* keep channels strictly above `top` */
        pend &= (uint16_t)~((1u << (top + 1)) - 1u);
    }
    return pend;
}

int mfp_hub_irq_wanted(void)
{
    return deliverable() != 0;
}

int mfp_hub_iack(void)
{
    uint16_t pend = deliverable();
    if (!pend)
        return -1;

    int ch = 15;
    while (!(pend & (1u << ch)))
        ch--;

    uint16_t bit = (uint16_t)(1u << ch);
    atomic_fetch_and(&g_vpend, (uint16_t)~bit);  /* pending -> taken    */
    if (atomic_load(&g_vr) & 0x08u)              /* software-EOI mode   */
        atomic_fetch_or(&g_visr, bit);

    if (hub_dbg()) {
        static int shown;
        if (shown < 16) {
            shown++;
            fprintf(stderr, "[mfphub] IACK ch%d -> vec %02X (visr now %04X)\n",
                    ch, (unsigned)((atomic_load(&g_vr) & 0xF0u) | (unsigned)ch),
                    atomic_load(&g_visr));
        }
    }
    return (int)((atomic_load(&g_vr) & 0xF0u) | (unsigned)ch);
}

/* ---- reset ------------------------------------------------------------ */

void mfp_hub_reset(void)
{
    atomic_store(&g_vpend, 0);
    atomic_store(&g_visr, 0);
    atomic_store(&g_ier, 0xFFFF);
    atomic_store(&g_imr, 0xFFFF);
    atomic_store(&g_vr, 0x48);
    atomic_store(&g_tacr, 0);
    atomic_store(&g_tadr, 256);
    atomic_store(&g_tacnt, 256);
}
