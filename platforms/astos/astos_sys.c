/*
 * astos_sys.c - the Pi's figures for the taskbar. Phase 0: temperature
 * and the clock. The full status monitor (VideoCore mailbox: voltages,
 * clocks, throttling and under-voltage flags) is phase 1.
 */
#include <stdio.h>
#include <time.h>
#include "astos.h"

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
