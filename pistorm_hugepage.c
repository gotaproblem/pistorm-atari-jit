/* SPDX-License-Identifier: MIT
 * pistorm_hugepage.c - see pistorm_hugepage.h */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include "pistorm_hugepage.h"

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define MAX_REGIONS 8
static struct {
    const char *tag;
    void *base;
    size_t size;
    const char *how;
} g_regions[MAX_REGIONS];
static int g_nregions;

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return (e && *e) ? atoi(e) : def;
}

/* THP policy lives in sysfs. Raspberry Pi OS ships 'madvise' for both
 * knobs, which is exactly what we need; if someone has set 'never', flip
 * it to 'madvise' (we run as root for the GPIO bus anyway). 'always' is
 * left alone - it also works. */
static void thp_ensure_madvise(const char *knob)
{
    char path[128], cur[128] = {0};
    snprintf(path, sizeof path, "/sys/kernel/mm/transparent_hugepage/%s", knob);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(cur, sizeof cur, f)) cur[0] = 0;
    fclose(f);
    if (!strstr(cur, "[never]"))
        return;
    f = fopen(path, "w");
    if (f) {
        fputs("madvise", f);
        fclose(f);
        fprintf(stderr, "[HUGEPAGE] transparent_hugepage/%s was 'never' - set to 'madvise'\n", knob);
    } else {
        fprintf(stderr, "[HUGEPAGE] transparent_hugepage/%s is 'never' and could not be changed - "
                "no 2M pages will form\n", knob);
    }
}

static void note(const char *tag, void *base, size_t size, const char *how)
{
    if (g_nregions < MAX_REGIONS) {
        g_regions[g_nregions].tag = tag;
        g_regions[g_nregions].base = base;
        g_regions[g_nregions].size = size;
        g_regions[g_nregions].how = how;
        g_nregions++;
    }
    fprintf(stderr, "[HUGEPAGE] %-10s %8zu KB at %p - %s\n",
            tag, size >> 10, base, how);
}

void *pistorm_mmap_huge(size_t size, int prot, int extra_flags, const char *tag)
{
    const int base_flags = MAP_PRIVATE | MAP_ANONYMOUS | extra_flags;
    const int enabled = env_int("PISTORM_HUGEPAGE", 1);
    const int want_hugetlb = env_int("PISTORM_HUGETLB", 0);

    if (!enabled || size < PISTORM_HUGE_2M) {
        void *p = mmap(NULL, size, prot, base_flags, -1, 0);
        if (p != MAP_FAILED && size >= PISTORM_HUGE_2M)
            note(tag, p, size, "4K pages (PISTORM_HUGEPAGE=0)");
        return p;
    }

    /* 1. hugetlbfs - only if asked for; an unreserved pool just fails ENOMEM */
    if (want_hugetlb) {
        size_t rsz = (size + PISTORM_HUGE_2M - 1) & ~(size_t)(PISTORM_HUGE_2M - 1);
        /* MAP_NORESERVE is meaningless for hugetlb and hugetlb pages are
         * always resident, so drop the caller's extra flags here. */
        void *p = mmap(NULL, rsz, prot,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
        if (p != MAP_FAILED) {
            note(tag, p, rsz, "hugetlbfs 2M pages");
            return p;
        }
        fprintf(stderr, "[HUGEPAGE] %s: MAP_HUGETLB failed (%s) - reserve with "
                "'hugepagesz=2M hugepages=N' in cmdline.txt; falling back to THP\n",
                tag, strerror(errno));
    }

    /* 2. THP: over-allocate by one huge page, trim to a 2 MB-aligned window,
     *    then ask for huge pages. Alignment matters: THP only forms 2 MB
     *    pages on 2 MB-aligned virtual ranges. */
    {
        static int thp_checked;
        if (!thp_checked) {
            thp_checked = 1;
            thp_ensure_madvise("enabled");
            thp_ensure_madvise("defrag");
        }
        size_t rsz = (size + PISTORM_HUGE_2M - 1) & ~(size_t)(PISTORM_HUGE_2M - 1);
        size_t span = rsz + PISTORM_HUGE_2M;
        /* Map PROT_NONE first. cpu_task runs mlockall(MCL_FUTURE), and the
         * JIT cache is allocated from the main thread while that may already
         * be in force: a readable mapping would then be populated at mmap()
         * time, BEFORE madvise() marks the VMA VM_HUGEPAGE, and every page
         * would be a 4 KB one (khugepaged may or may not collapse them
         * later). PROT_NONE cannot be populated, so the first touch after
         * mprotect() below faults with the hint in place and gets 2 MB. */
        uint8_t *raw = mmap(NULL, span, PROT_NONE, base_flags, -1, 0);
        if (raw == MAP_FAILED)
            return MAP_FAILED;
        uintptr_t a = ((uintptr_t)raw + PISTORM_HUGE_2M - 1) & ~(uintptr_t)(PISTORM_HUGE_2M - 1);
        uint8_t *aligned = (uint8_t *)a;
        size_t head = aligned - raw;
        size_t tail = span - head - rsz;
        if (head)
            munmap(raw, head);
        if (tail)
            munmap(aligned + rsz, tail);
        int thp_ok = (madvise(aligned, rsz, MADV_HUGEPAGE) == 0);
        if (mprotect(aligned, rsz, prot) != 0) {
            int e = errno;
            munmap(aligned, rsz);
            errno = e;
            return MAP_FAILED;
        }
        if (!thp_ok)
            note(tag, aligned, rsz, "THP unavailable (madvise failed) - 4K pages");
        else
            note(tag, aligned, rsz, "THP madvise(MADV_HUGEPAGE), 2M aligned");
        return aligned;
    }
}

/* Walk /proc/self/smaps once and sum AnonHugePages for each noted region. */
void pistorm_hugepage_report(void)
{
    static int done;
    if (done || g_nregions == 0)
        return;
    done = 1;

    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) {
        fprintf(stderr, "[HUGEPAGE] no /proc/self/smaps - cannot verify\n");
        return;
    }
    size_t huge_kb[MAX_REGIONS] = {0};
    size_t rss_kb[MAX_REGIONS] = {0};
    char line[512];
    uintptr_t lo = 0, hi = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx ", &a, &b) == 2) {
            lo = a; hi = b;
            continue;
        }
        unsigned long kb;
        int is_huge = (strncmp(line, "AnonHugePages:", 14) == 0);
        int is_rss = (strncmp(line, "Rss:", 4) == 0);
        if (!is_huge && !is_rss)
            continue;
        if (sscanf(line + (is_huge ? 14 : 4), " %lu kB", &kb) != 1)
            continue;
        for (int i = 0; i < g_nregions; i++) {
            uintptr_t rb = (uintptr_t)g_regions[i].base;
            uintptr_t re = rb + g_regions[i].size;
            if (lo < re && hi > rb) {
                if (is_huge) huge_kb[i] += kb; else rss_kb[i] += kb;
            }
        }
    }
    fclose(f);

    for (int i = 0; i < g_nregions; i++) {
        size_t sz_kb = g_regions[i].size >> 10;
        int pct = rss_kb[i] ? (int)(huge_kb[i] * 100 / rss_kb[i]) : 0;
        fprintf(stderr, "[HUGEPAGE] %-10s %8zu KB mapped, %8zu KB resident, "
                "%8zu KB on 2M pages (%d%% of resident)%s\n",
                g_regions[i].tag, sz_kb, rss_kb[i], huge_kb[i], pct,
                (strstr(g_regions[i].how, "hugetlb") ? " [hugetlb: not counted "
                 "in AnonHugePages, always 2M]" : ""));
    }
    fprintf(stderr, "[HUGEPAGE] tip: if the 2M column is ~0, check "
            "/sys/kernel/mm/transparent_hugepage/enabled is 'madvise' or "
            "'always', and .../defrag is not 'never'\n");
}
