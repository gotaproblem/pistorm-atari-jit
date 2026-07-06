/*
 * platform_atari_fdd.c - PiStorm-Atari platform glue for FDD emulator
 */

#include "platform_atari_fdd.h"
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

void platform_fdd_init(char *image)
{
    bool wp = 0;

    fdd_init();

    if (fdd_insert_disk (0, image, wp) != 0)
       // fprintf(stderr, "[FDD] Drive A: %s%s\n", image, wp ? " (WP)" : " RW");
    //else
        fprintf(stderr, "[FDD] Drive A: failed to mount %s\n", image);

    //fprintf(stderr, "[FDD] Platform init complete\n");
}

#ifdef __cplusplus
}
#endif