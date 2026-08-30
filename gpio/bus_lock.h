/*
 * bus_lock.h - exclusive claim on the PiStorm bus. See bus_lock.c.
 *
 * Call pistorm_bus_lock() once, early in main(), BEFORE touching the
 * GPIO. Returns 0 if this process now owns the bus, -1 if another live
 * process holds it (a message naming the holder is printed).
 *
 * The lock is released automatically when the process exits, however it
 * exits - there is no cleanup to forget and no stale state to clear.
 * pistorm_bus_unlock() exists only for the rare case of wanting to hand
 * the bus over without exiting.
 */

#ifndef PISTORM_BUS_LOCK_H
#define PISTORM_BUS_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

int  pistorm_bus_lock(const char *who);
void pistorm_bus_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_BUS_LOCK_H */
