/*
 * platform_atari_fdd.c - PiStorm-Atari platform glue for FDD emulator
 */

#include "platform_atari_fdd.h"
#include "platforms/atari/psctrl/psctrl_tunables.h"   /* PS_INFO */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * VBL timer thread - 50Hz
 * ========================================================================= */

void *fdd_vbl_thread(void *arg)
{
    (void)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20ms */
    while (1) {
        nanosleep (&ts, NULL);
        fdd_vbl ();
    }
    return NULL;
}

/* =========================================================================
 * Platform init
 * ========================================================================= */

/* Both drives, in one place. `image_b` may be NULL or empty - drive B
 * then has no image and its select bits are left alone, so a real drive
 * B on the ST still answers beside an image in A. */
void platform_fdd_init(const char *image_a, const char *image_b)
{
    const bool wp = 0;
    static const char *const nm[2] = { "A:", "B:" };
    const char *img[2] = { image_a, image_b };

    fdd_init();

    for (int d = 0; d < 2; d++) {
        if (!img[d] || !img[d][0])
            continue;
        if (fdd_insert_disk (d, img[d], wp) != 0)
            fprintf(stderr, "[FDD] Drive %s failed to mount %s\n", nm[d], img[d]);
        else
            PS_INFO("[FDD] Drive %s %s%s\n", nm[d], img[d], wp ? " (WP)" : " RW");
    }
}

#ifdef __cplusplus
}
#endif