/*
 * bus_lock.c - "who owns the PiStorm bus" interlock.
 *
 * The emulator and ataritest both drive the real ST bus over the GPIO
 * header. Two of them at once is not merely wrong output, it is two
 * processes fighting over the same pins mid-cycle, so exactly one may
 * run at a time.
 *
 * WHY A FILE LOCK AND NOT A PROCESS SCAN
 *   The original check walked /proc looking for a process called
 *   "emulator". That has three failure modes, two of which bit us:
 *
 *     - A ZOMBIE matched. A zombie is a process-table entry whose
 *       process has already exited; it holds no memory, no file
 *       descriptors and no GPIO mapping, so it cannot possibly be using
 *       the bus. Worse, a zombie cannot be killed - only its parent
 *       reaping it clears it - so a stuck one locked ataritest out
 *       until reboot.
 *     - Any unreadable /proc entry was treated as "running", and /proc
 *       entries come and go constantly, so an unrelated process exiting
 *       mid-scan could refuse the run.
 *     - Matching by name is guesswork: it would trip over any binary
 *       that happened to be called "emulator", and miss ours if renamed
 *       or run through a wrapper.
 *
 *   flock() has none of these problems. The kernel releases the lock
 *   when the holding process dies, by ANY route - clean exit, crash,
 *   SIGKILL, or lingering as a zombie - because the release is tied to
 *   the file descriptor, not to the process table entry. There is no
 *   stale state to clean up, ever, and nothing to match by name.
 *
 * The lock file lives in /run, which is a tmpfs: it cannot survive a
 * reboot as stale state either. If /run is not writable (unusual, but
 * possible when not running as root) we fall back to /tmp, and if that
 * fails too we allow the run rather than block the user on a
 * diagnostic mechanism.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "bus_lock.h"

#define BUS_LOCK_PRIMARY  "/run/pistorm.bus.lock"
#define BUS_LOCK_FALLBACK "/tmp/pistorm.bus.lock"

/* Deliberately never closed while we hold the lock: the lock lives as
 * long as this descriptor does, and we want it to last the process. */
static int g_lock_fd = -1;

static int lock_open(const char *path)
{
    return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
}

int pistorm_bus_lock(const char *who)
{
    if (g_lock_fd >= 0)
        return 0;                        /* already ours */

    int fd = lock_open(BUS_LOCK_PRIMARY);
    if (fd < 0)
        fd = lock_open(BUS_LOCK_FALLBACK);
    if (fd < 0) {
        /* Cannot create a lock file at all. Do not stand in the user's
         * way over a diagnostic aid - warn and continue. */
        fprintf(stderr, "[bus] no lock file (%s) - proceeding unchecked\n",
                strerror(errno));
        return 0;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        /* Record who we are, so the loser can print something useful.
         * Purely informational - the LOCK is what enforces exclusion. */
        if (ftruncate(fd, 0) == 0) {
            char line[128];
            int n = snprintf(line, sizeof line, "%s pid %d\n",
                             who ? who : "?", (int)getpid());
            if (n > 0)
                (void)!write(fd, line, (size_t)n);
        }
        g_lock_fd = fd;                  /* keep open = keep the lock */
        return 0;
    }

    if (errno == EWOULDBLOCK) {
        char buf[128] = {0};
        ssize_t r;
        lseek(fd, 0, SEEK_SET);
        r = read(fd, buf, sizeof buf - 1);
        if (r <= 0)
            snprintf(buf, sizeof buf, "another process\n");
        fprintf(stderr, "[bus] already in use by %s", buf);
        close(fd);
        return -1;
    }

    fprintf(stderr, "[bus] flock failed (%s) - proceeding unchecked\n",
            strerror(errno));
    close(fd);
    return 0;
}

void pistorm_bus_unlock(void)
{
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}
