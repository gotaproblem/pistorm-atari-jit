/*
 * astos_sys.c - the Pi's figures for the taskbar. Phase 0: temperature
 * and the clock. The full status monitor (VideoCore mailbox: voltages,
 * clocks, throttling and under-voltage flags) is phase 1.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "astos.h"

/* The firmware's throttle word - what `vcgencmd get_throttled` prints -
 * read through the VideoCore mailbox (/dev/vcio, as vcgencmd itself
 * does), tag GET_THROTTLED $00030046. Bits 0-3 are now: under-voltage,
 * ARM frequency capped, throttled, soft temperature limit; bits 16-19
 * the same since boot. The request value is 0: a non-zero one is a mask
 * of the since-boot bits to CLEAR, which is not ours to do. -1 when there
 * is no mailbox (not a Pi). PISTORM_ASTOS_THROTTLED=0x... fakes a value,
 * for trying the taskbar out. */
#define MBOX_PROPERTY _IOWR(100, 0, char *)
int sys_throttled(void)
{
    static int fd = -2;
    const char *fake = getenv("PISTORM_ASTOS_THROTTLED");
    if (fake && *fake)
        return (int)strtol(fake, NULL, 0);
    if (fd == -2)
        fd = open("/dev/vcio", O_RDWR);
    if (fd < 0)
        return -1;
    uint32_t b[7] __attribute__((aligned(16))) = {
        7 * 4, 0x00000000,          /* size, request                 */
        0x00030046, 4, 0,           /* tag, value buffer, request len */
        0,                          /* value: 0 = clear nothing       */
        0x00000000,                 /* end tag                        */
    };
    if (ioctl(fd, MBOX_PROPERTY, b) < 0 || b[1] != 0x80000000u)
        return -1;
    return (int)(b[5] & 0x000F000Fu);
}

int sys_temp_c10(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f)
        return -1;
    long mc = -1;
    if (fscanf(f, "%ld", &mc) != 1)
        mc = -1;
    fclose(f);
    return mc < 0 ? -1 : (int)(mc / 100);
}

void sys_clock(char *buf, int n, int h12)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (h12)
        strftime(buf, (size_t)n, "%I:%M %p", &tm);
    else
        strftime(buf, (size_t)n, "%H:%M", &tm);
    if (h12 && buf[0] == '0')                 /* 09:41 PM -> 9:41 PM */
        for (int i = 0; buf[i]; i++)
            buf[i] = buf[i + 1];
}

void sys_date(char *buf, int n, int mdy)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, (size_t)n, mdy ? "%m/%d/%Y" : "%d/%m/%Y", &tm);
}
