/*
 * astos_hw.c - what the real machine is, for the "This machine" panel.
 *
 * Asked over the bus with the 68k halted, once when ASTOS starts:
 *   - np_probe (native_probe.c): ST-RAM banks, which chips answer (STE
 *     DMA sound, blitter, Mega STE control), the TOS in ROM - the same
 *     probe that writes the [native] build
 *   - IDE: does the IDE status register at $F0001D answer (a bus error
 *     on a plain ST; an IDE interface or a Falcon-style port answers)
 *   - ACSI: a TEST UNIT READY to each target 0-7 through the DMA chip,
 *     the way TOS's ACSI code sends a command: the command byte with A1
 *     low, then A1 high, each byte acknowledged by the HDC interrupt
 *     (MFP GPIP bit 5, active low). A target that never acknowledges the
 *     first byte is not there. No data phase, so no DMA transfer.
 * Nothing here is emulated - the emulator's IDE and ACSI images do not
 * exist yet when ASTOS runs.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "astos.h"
#include "native_probe.h"
#include "gpio/ps_protocol.h"

extern uint8_t fc;
extern volatile uint8_t g_buserr;
uint8_t ps_read_8_quiet(uint32_t addr, uint8_t fc_value, uint8_t *berr_out);

#define FC_SUPER_DATA 0x5
#define REG_DMA_DATA  0x00FF8604u
#define REG_DMA_MODE  0x00FF8606u
#define REG_MFP_GPIP  0x00FFFA01u
#define REG_IDE_STAT  0x00F0001Du
#define GPIP_HDC_IRQ  0x20            /* FDC/HDC interrupt, active low    */
#define MODE_HDC_A1_0 0x0088          /* HDC, A1 low: first command byte  */
#define MODE_HDC_A1_1 0x008A          /* HDC, A1 high: the rest           */
#define MODE_FDC      0x0080          /* back to the FDC                  */

/* the HDC acknowledged the byte: its interrupt line went low */
static int hdc_ack(int us)
{
    for (int t = 0; t < us; t += 50) {
        if (!(ps_read_8(REG_MFP_GPIP) & GPIP_HDC_IRQ))
            return 1;
        usleep(50);
    }
    return 0;
}

/* TEST UNIT READY to one target: 1 if it answered the command */
static int acsi_there(int id)
{
    int ok = 0;
    ps_write_16(REG_DMA_MODE, MODE_HDC_A1_0);
    ps_write_16(REG_DMA_DATA, (uint16_t)((id << 5) | 0x00));   /* opcode 0 */
    ps_write_16(REG_DMA_MODE, MODE_HDC_A1_1);
    if (!hdc_ack(3000))                       /* no one at this ID */
        goto out;
    for (int b = 1; b < 6; b++) {             /* the other 5 CDB bytes */
        ps_write_16(REG_DMA_DATA, 0x0000);
        ps_write_16(REG_DMA_MODE, MODE_HDC_A1_1);
        if (!hdc_ack(100000))
            goto out;
    }
    ps_write_16(REG_DMA_MODE, MODE_HDC_A1_1);
    (void)ps_read_16(REG_DMA_DATA);           /* the status byte */
    ok = 1;
out:
    ps_write_16(REG_DMA_MODE, MODE_FDC);
    return ok;
}

void hw_probe(struct astos_hw *hw)
{
    memset(hw, 0, sizeof *hw);
    hw->ok = np_probe(&hw->np) == 0;
    if (!hw->ok)
        return;

    uint8_t old_fc = fc;
    fc = FC_SUPER_DATA;                       /* the GLUE ignores FC 0 */
    uint8_t berr = 0;
    (void)ps_read_8_quiet(REG_IDE_STAT, FC_SUPER_DATA, &berr);
    hw->ide = !berr;
    /* the HDC interrupt must be quiet first, or every ID would look
     * present: a pending FDC interrupt shares the line - reading the FDC
     * status clears it. Still low: the line is stuck, so no answer. */
    if (!(ps_read_8(REG_MFP_GPIP) & GPIP_HDC_IRQ)) {
        ps_write_16(REG_DMA_MODE, MODE_FDC);
        (void)ps_read_16(REG_DMA_DATA);
        usleep(100);
    }
    if (ps_read_8(REG_MFP_GPIP) & GPIP_HDC_IRQ) {
        for (int id = 0; id < 8; id++)
            if (acsi_there(id))
                hw->acsi |= 1u << id;
    } else {
        hw->acsi_unknown = 1;
        printf("[ASTOS] ACSI: the HDC interrupt line is held low - not probed\n");
    }
    fc = old_fc;
    g_buserr = 0;                             /* nothing left latched */
    printf("[ASTOS] hardware: %s, IDE %s, ACSI targets 0x%02X\n",
           hw->np.machine, hw->ide ? "yes" : "no", hw->acsi);
}
