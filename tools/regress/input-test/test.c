/* Host test for platforms/atari/input_script.c:  sh tools/regress/input-test/run.sh
 * Plays a short script against stubs that log what reaches the injection
 * layer, with their times, and checks order, timing and the stop. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "platforms/atari/input_script.h"

static struct timespec t0;
static double now(void)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (n.tv_sec - t0.tv_sec) + (n.tv_nsec - t0.tv_nsec) * 1e-9;
}
static char got[64][64];
static double at[64];
static int ng;
static volatile int stopped;
void joy_usb_script_set(int port, uint8_t s) { snprintf(got[ng], 64, "joy%d %02X", port, s); at[ng++] = now(); }
void kbd_usb_script_key(uint8_t k, int d) { snprintf(got[ng], 64, "key %02X %d", k, d); at[ng++] = now(); }
static int rt_started;
void rt_start(void) { rt_started++; }
void pistorm_request_exit(int m) { snprintf(got[ng], 64, "exit %d", m); at[ng++] = now(); stopped = 1; for (;;) pause(); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    FILE *f = fopen("/tmp/input-test.in", "w");
    fprintf(f, "# test\nstart C:\\GAMES\\JIM\\START.TOS\nseconds 1.2\nshots 4\n"
               "0.1 key 0x39   # space\n0.3 fire\n0.6 joy1 right fire\n0.9 joy1 -\n"
               "0.5 mark play here\n1.0 keydown 0x1D\n1.1 keyup 0x1D\n");
    fclose(f);
    setenv("PISTORM_INPUT_SCRIPT", "/tmp/input-test.in", 1);
    CHECK(input_script_wanted(), "wanted");
    CHECK(input_script_init() == 1, "init");
    CHECK(input_script_armed(), "armed");
    input_script_pexec("C:\\AUTO\\LAUNCH.PRG");          /* not it */
    usleep(300000);
    CHECK(ng == 0, "started on the wrong Pexec (%d events)", ng);
    CHECK(input_script_armed(), "still armed");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    input_script_pexec("d:\\jimpower\\start.tos");      /* any path, any case */
    CHECK(!input_script_armed(), "disarmed");
    for (int i = 0; i < 300 && !stopped; i++)
        usleep(10000);
    usleep(50000);
    static const char *want[] = { "key 39 1", "key 39 0", "joy1 80", "joy1 00", "joy1 88",
                                  "joy1 00", "key 1D 1", "key 1D 0", "joy1 00", "exit 0" };
    static const double wt[] = { 0.1, 0.2, 0.3, 0.5, 0.6, 0.9, 1.0, 1.1, 1.2, 1.2 };
    const int nw = sizeof want / sizeof *want;
    CHECK(ng == nw, "%d events, want %d", ng, nw);
    for (int i = 0; i < ng && i < nw; i++) {
        CHECK(!strcmp(got[i], want[i]), "event %d: %s, want %s", i, got[i], want[i]);
        CHECK(at[i] >= wt[i] - 0.01 && at[i] < wt[i] + 0.05, "event %d (%s) at %.3f s, want %.1f",
              i, got[i], at[i], wt[i]);
    }
    CHECK(rt_started == 1, "rt_start called %d times at mark play, want 1", rt_started);
    printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
