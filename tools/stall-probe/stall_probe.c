/* stall_probe.c - does a core lose time with nothing running on it?
 *
 * Pins itself to one core (default 2, the emulator's CPU-thread core),
 * SCHED_FIFO, mlockall, then spins reading the arch timer for N seconds.
 * Every gap between two reads longer than the threshold is a stall: the
 * core did not run this loop for that long. Prints the stalls per second,
 * a histogram, and every stall with its CLOCK_MONOTONIC_RAW time so
 * they can be lined up with tools/ftrace-emu-cores.sh (kernel or not).
 *
 *   gcc -O2 -o stall_probe tools/stall-probe/stall_probe.c
 *   sudo ./stall_probe [cpu=2] [seconds=60] [threshold_ns=1000] > out.txt
 *
 * Run with the emulator NOT running (the core must be free).
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static inline uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
static inline uint64_t freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

#define MAXS 200000
static uint64_t st_t[MAXS], st_d[MAXS];

int main(int argc, char **argv)
{
    int cpu = argc > 1 ? atoi(argv[1]) : 2;
    double secs = argc > 2 ? atof(argv[2]) : 60.0;
    uint64_t thr_ns = argc > 3 ? strtoull(argv[3], 0, 0) : 1000;
    uint64_t hz = freq();
    uint64_t thr = thr_ns * hz / 1000000000ull;
    cpu_set_t cs;
    struct sched_param sp = { .sched_priority = 50 };
    struct timespec ts;
    uint64_t n = 0, t0, tend, prev, now, c0;
    long long m0;
    unsigned hist[8] = { 0 };
    static const unsigned edge_us[7] = { 2, 3, 5, 10, 20, 50, 100 };

    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    if (sched_setaffinity(0, sizeof cs, &cs) != 0) { perror("affinity"); return 1; }
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) perror("SCHED_FIFO (run with sudo)");
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall");
    memset(st_t, 0, sizeof st_t);
    memset(st_d, 0, sizeof st_d);

    c0 = ticks();
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    m0 = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    t0 = prev = ticks();
    tend = t0 + (uint64_t)(secs * (double)hz);
    while ((now = ticks()) < tend) {
        uint64_t d = now - prev;
        if (d > thr) {
            if (n < MAXS) { st_t[n] = prev; st_d[n] = d; }
            n++;
        }
        prev = now;
    }

    printf("# stall_probe cpu %d, %.0f s, threshold %llu ns, timer %llu Hz\n",
           cpu, secs, (unsigned long long)thr_ns, (unsigned long long)hz);
    printf("# clocks cntvct %llu mono_raw_ns %lld\n", (unsigned long long)c0, m0);
    printf("stalls: %llu (%.2f/s)\n", (unsigned long long)n, (double)n / secs);
    for (uint64_t i = 0; i < n && i < MAXS; i++) {
        double us = (double)st_d[i] * 1e6 / (double)hz;
        int b = 0;
        while (b < 7 && us >= edge_us[b]) b++;
        hist[b]++;
    }
    printf("histogram (us): <2 %u | 2-3 %u | 3-5 %u | 5-10 %u | 10-20 %u | 20-50 %u | 50-100 %u | >=100 %u\n",
           hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    printf("# first stalls: mono_raw_ns_at_start duration_ns\n");
    for (uint64_t i = 0; i < n && i < MAXS; i++) {
        long long m = m0 + (long long)(((double)st_t[i] - (double)c0) * 1e9 / (double)hz);
        printf("S %lld %.0f\n", m, (double)st_d[i] * 1e9 / (double)hz);
    }
    return 0;
}
