/* SPDX-License-Identifier: MIT
 *
 * pistorm_hugepage.h - 2 MB huge-page backing for the big, hot mappings.
 *
 * WHY: the JIT thread walks a ~150 MB working set (guest mirror 16 MB +
 * TT-RAM 128 MB, fVDI framebuffer, 8-16 MB translation cache) through the
 * Cortex-A72's L2 TLB, which with 4 KB pages reaches only ~4 MB. Every miss
 * is a page-table walk on the CPU core. Backing those regions with 2 MB
 * pages (the same block size Emu68 gets from its bare-metal MMU tables)
 * removes most of that cost without leaving Linux.
 *
 * Two backends, tried in order:
 *   1. MAP_HUGETLB  - explicit hugetlbfs pages. Needs a reservation
 *                     (hugepagesz=2M hugepages=N on cmdline.txt, or
 *                     /proc/sys/vm/nr_hugepages). Guaranteed 2 MB, never
 *                     split, no khugepaged. Opt-in: PISTORM_HUGETLB=1.
 *   2. THP/madvise  - anonymous mmap, 2 MB aligned, madvise(MADV_HUGEPAGE).
 *                     Works out of the box on Raspberry Pi OS 64-bit
 *                     (CONFIG_TRANSPARENT_HUGEPAGE_MADVISE). Default.
 * Either way the caller gets a plain pointer; nothing else changes.
 *
 * PISTORM_HUGEPAGE=0 disables both (plain mmap) for A/B testing.
 */
#ifndef PISTORM_HUGEPAGE_H
#define PISTORM_HUGEPAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PISTORM_HUGE_2M (2u << 20)

/* mmap() replacement for large anonymous regions. `extra_flags` is OR'd
 * into the MAP_ flags (e.g. MAP_NORESERVE); MAP_PRIVATE|MAP_ANONYMOUS are
 * implied. Returns MAP_FAILED on error, exactly like mmap(). Regions
 * smaller than 2 MB fall through to a plain mmap(). `tag` is for the log. */
void *pistorm_mmap_huge(size_t size, int prot, int extra_flags, const char *tag);

/* Print, once, how much of each tagged region actually landed on 2 MB pages
 * (AnonHugePages from /proc/self/smaps). Call after mlockall() has faulted
 * everything in - i.e. from cpu_task - so the numbers are final. */
void pistorm_hugepage_report(void);

#ifdef __cplusplus
}
#endif
#endif
