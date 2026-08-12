#ifndef _MACHINE_COOKIE_H
#define _MACHINE_COOKIE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side _MCH cookie forcing (cfg "machine st|ste|megaste|tt|falcon").
 * machine_cookie_set() arms the watcher with the desired _MCH value;
 * machine_cookie_tick() is polled from ipl_task's housekeeping slot
 * (bounded memory work only) and patches the guest's cookie jar once
 * the OS has built it - replacing SETMCH.PRG for emulator boots. */
void machine_cookie_set(uint32_t mch_value);
void machine_cookie_tick(void);

#ifdef __cplusplus
}
#endif

#endif
