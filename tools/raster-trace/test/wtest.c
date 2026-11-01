/* End-to-end test of the Timer B window record (W) - patch 0037.
 * Drives the REAL bus lock path (gpio/ps_protocol.c, included so its
 * static inline lock functions are callable) and the REAL raster_trace.c
 * through 400 synthetic Timer B bands with known injected delays, dumps the
 * trace, and analyse.py must blame each late band on the injected cause.
 * aarch64 only (arch timer): run on the Pi, or under qemu-aarch64-static.
 *   sh tools/raster-trace/test/wtest.sh */
#include "gpio/ps_protocol.c"
#include <pthread.h>
#include "platforms/atari/raster_trace.h"

static uint64_t clocks;
uint64_t pistorm_guest_clocks(void) { return clocks; }

static void spin_us(double us)
{
    uint64_t f, t0 = ps_ticks();
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    while ((double)(ps_ticks() - t0) * 1e6 / (double)f < us)
        ;
}

/* another thread holding the bus: what ipl_task's IPL samples do */
static volatile double other_us;
static void *other(void *a)
{
    (void)a;
    for (;;) {
        if (other_us > 0) {
            ps_bus_req = 1;
            spin_us(other_us);
            ps_bus_req = 0;
            other_us = 0;
        }
    }
    return NULL;
}

static void bus(double us) { ps_lock_bus(); spin_us(us); ps_unlock_bus(); }

int main(void)
{
    pthread_t th;
    pthread_create(&th, NULL, other, NULL);
    ps_tls_cpu = 1;                         /* this thread is the CPU thread */
    FILE *causes = fopen("/tmp/wtest.causes", "w");
    srand(1);
    int want[4] = {0, 0, 0, 0};             /* none, bus wait, governor, compute */
    for (int band = 0; band < 400; band++) {
        int cause = band % 10 == 3 ? 1 : band % 10 == 6 ? 2 : band % 10 == 8 ? 3 : 0;
        want[cause]++;
        if (band % 10 == 0) {
            rt_irq(4);                      /* a VBL now and then */
            spin_us(5);
        }
        rt_ipl(6);                          /* L: the line ended */
        spin_us(0.3);
        rt_irq(6);                          /* I 6 */
        spin_us(1.0);                       /* the IACK */
        rt_irq_vector(0x48);                /* window opens */
        for (int i = 0; i < 4; i++) {       /* the handler: frame, movem, ... */
            if (cause == 1 && i == 2) {
                other_us = 40.0;            /* another thread wants the bus */
                while (!ps_bus_req)
                    ;
            }
            bus(0.5);
            clocks += 40;
        }
        if (cause == 2) {
            rt_gov(40000, 0);               /* a governor pause, taken */
            spin_us(40.0);
        }
        if (cause == 3)
            spin_us(40.0);                  /* the CPU thread not running */
        rt_palette(8, 0x123);               /* W + P */
        fprintf(causes, "%d\n", cause);
        spin_us(30);
    }
    fclose(causes);
    rt_dump();
    printf("injected: %d on time, %d bus-wait, %d governor, %d compute\n",
           want[0], want[1], want[2], want[3]);
    return 0;
}
