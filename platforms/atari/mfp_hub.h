/*
 * mfp_hub.h — the ONE home for virtual MFP interrupt state.
 *
 * WHY THIS EXISTS. Virtual interrupt sources grew organically and each
 * carried a private copy of the same machinery: kbd_usb shadowed
 * IERB/IMRB (later ISRB/VR), dmasnd_capture shadowed IERA/IMRA/IPRA/
 * ISRA/VR with its own IACK-vector function, and intlev_ack hand-ordered
 * the two ahead of the real-bus IACK. Each copy implemented a DIFFERENT
 * subset of real MFP semantics, and the missing pieces were not
 * theoretical: the keyboard channel ignored in-service and nested the
 * guest's handler until the supervisor stack ran through the sysvars
 * (Petra/Paula corruption crashes), and dmasnd set its in-service shadow
 * but never CHECKED it before re-raising (Paula's hang after the kbd fix).
 *
 * THE MODEL. MFP channels are numbered 0..15; register set A (IERA/IPRA/
 * ISRA/IMRA, odd addresses $FFFA07/0B/0F/13) covers channels 8-15, set B
 * ($FFFA09/0D/11/15) channels 0-7. Delivery rule (68901 datasheet): a
 * channel requests an interrupt iff
 *     enabled && pending && mask-bit set && channel > highest in-service
 * The last clause is what makes hardware interrupt nesting safe: a
 * handler may lower the CPU IPL mid-service and only HIGHER MFP channels
 * may then preempt it. In software-EOI mode (VR bit 3 set - what TOS and
 * EmuTOS use) the in-service bit is set at IACK and cleared only by the
 * handler writing a 0 to it; in auto-EOI mode in-service is never held.
 *
 * SCOPE. The hub tracks VIRTUAL channels only - interrupts synthesised
 * host-side (USB keyboard bytes, dmasnd frame events). Real MFP
 * interrupts keep their real IACK cycle; the guest's writes to the real
 * chip pass through untouched (we only snoop them to keep the shadows
 * honest). The hub cannot see the REAL chip's in-service state, matching
 * the old per-source behaviour - the ordering guarantee is among the
 * virtual set, which is where every observed failure lived.
 *
 * Current virtual channels:
 *   ch  6 (GPIP4,  set B bit 6) keyboard/mouse injection - LEVEL source
 *   ch 13 (TimerA, set A bit 5) dmasnd frame counter     - EVENT source
 *   ch 15 (GPIP7,  set A bit 7) dmasnd XSINT pulse       - EVENT source
 *
 * Threading: writers are the CPU thread (register snoops, IACK) and
 * ipl_task (arbiter poll, event raises from the frame clock). All state
 * is C11 atomics with relaxed ordering - same discipline the per-source
 * copies used; cross-field consistency is not required, only per-field
 * coherence (worst case is one poll's delay, as on the real 4 MHz chip).
 */

#ifndef MFP_HUB_H
#define MFP_HUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snoop EVERY guest write into the $FFFAxx page (byte or word; word
 * writes put the register byte on the odd address). Replaces the
 * kbd_usb_mfp_snoop + dmasnd_mfp_snoop pair at all dispatcher sites. */
void mfp_hub_write_snoop(uint32_t addr, uint32_t value, int is_word);

/* Merge virtual pending/in-service bits into guest READS of IPRA/IPRB/
 * ISRA/ISRB, so handlers that introspect the chip ("is this interrupt
 * mine?") see the virtual channels too. Pass every $FFFAxx byte read
 * through this; non-shimmed registers return `real` unchanged.
 * (dmasnd had this for set A only, on ONE of the two read dispatchers;
 * the keyboard had it on neither.) */
uint8_t mfp_hub_read_shim(uint32_t addr, uint8_t real);

/* EVENT source: latch a pending request for the channel (edge - stays
 * pending until delivered or cleared by the guest/enable rules). */
void mfp_hub_raise(int ch);

/* LEVEL source registration: `poll` is consulted at arbitration time;
 * nonzero return = the source is asserting. The channel stays deliverable
 * for as long as the level holds (a delivered level re-requests after
 * EOI while still asserted - real GPIP behaviour). One poll per channel. */
void mfp_hub_register_level(int ch, int (*poll)(void));

/* Timer A event-count mode (TACR=8): count one external event; raises
 * channel 13 when the programmed count expires. The TACR/TADR shadows
 * and countdown live in the hub - callers just report events. */
void mfp_hub_timer_a_event(void);

/* Arbiter, for ipl_task: nonzero iff some virtual channel is deliverable
 * RIGHT NOW under the full rule (enabled, pending/asserting, masked-in,
 * outranks every virtual in-service channel). */
int mfp_hub_irq_wanted(void);

/* IACK, for intlev_ack (CPU thread): pick the highest deliverable
 * virtual channel; clear its pending latch (event sources), set its
 * in-service bit (software-EOI mode), and return the vector
 * ((VR & 0xF0) | channel). Returns -1 if nothing is deliverable -
 * caller falls through to the real-bus IACK. */
int mfp_hub_iack(void);

/* Cold/warm reset: clear pending + in-service latches and restore the
 * power-on register shadows (enables/masks all set, VR software-EOI -
 * matching the old per-source boot defaults so an early interrupt
 * before TOS programs the chip is not lost). */
void mfp_hub_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MFP_HUB_H */
