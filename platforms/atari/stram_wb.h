/* ST-RAM write-behind for interrupt handlers - see stram_wb.c */
#ifndef STRAM_WB_H
#define STRAM_WB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

extern int      stram_wb_on;        /* PISTORM_WRBEHIND=1 */
extern unsigned stram_wb_n;         /* entries waiting   */
extern unsigned long stram_wb_deferred, stram_wb_direct_after, stram_wb_pushed,
                     stram_wb_flushes, stram_wb_max;

void stram_wb_init(void);
/* a write-through of the ST-RAM bank: size 1 (v = the byte) or 2 */
void stram_wb_slow(uint32_t bus_a, uint32_t v, int size, uint8_t wfc, int in_handler);
void stram_wb_drain(void);          /* until empty or an interrupt is due */
void stram_wb_flush_slow(void);     /* until empty                         */

static inline void stram_wb_flush(void)
{
    if (__builtin_expect(stram_wb_n != 0, 0))
        stram_wb_flush_slow();
}

#ifdef __cplusplus
}
#endif
#endif
