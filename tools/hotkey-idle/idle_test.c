/* Host test for kbd_hotkey_idle_poll (platforms/atari/kbd_usb.c): the
 * native hotkeys while the guest is not reading the keyboard.
 *
 * Links the real kbd_usb.c against a modelled 6850 receiver (one data
 * register, overrun shown after the good byte is read, as the MC6850
 * data sheet has it) fed with IKBD bytes at their real 1.28 ms spacing,
 * a fake clock, and a guest that either never reads or reads each byte
 * 50 us after it arrives. The poll runs every 10 ms, as emulator.c's main
 * loop calls it.
 *
 *   sh tools/hotkey-idle/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void kbd_hotkey_idle_poll(unsigned long beam_n);
static unsigned long beam_n;          /* beam-timed handlers so far */
static int rasters;                   /* the guest runs one a frame */

/* ---- clock ---- */
static uint64_t t_us;
int __wrap_clock_gettime(clockid_t c, struct timespec *ts)
{
    (void)c;
    ts->tv_sec = (time_t)(t_us / 1000000u);
    ts->tv_nsec = (long)(t_us % 1000000u) * 1000;
    return 0;
}

/* ---- the 6850 receiver ---- */
static uint8_t rdr;
static int rdrf, lost, ovr_shown;
static unsigned taken;
volatile unsigned ps_acia_kbd_guest_access;

static void arrive(uint8_t b)
{
#ifdef OVR_ACCEPTS      /* the other reading of the data sheet: a byte arriving
                         * while the overrun is shown still lands in the register */
    if (ovr_shown) { rdr = b; return; }
#endif
    if (!rdrf && !ovr_shown) { rdr = b; rdrf = 1; }
    else lost = 1;
}
static uint8_t status(void) { return (uint8_t)((rdrf ? 1 : 0) | (ovr_shown ? 0x20 : 0)); }
static uint8_t data_read(void)
{
    uint8_t v = rdr;
    if (ovr_shown) { ovr_shown = 0; rdrf = 0; return v; }
    rdrf = 0;
    if (lost) { lost = 0; ovr_shown = 1; rdrf = 1; }
    return v;
}

/* as gpio/ps_protocol.c (no FIFO here: nothing rescues in these runs) */
int ps_acia_kbd_idle_look(unsigned access, int take, uint8_t *v)
{
    int r = 0;
    if (ps_acia_kbd_guest_access != access)
        return -1;
    uint8_t st = status();
    if (st & 0x01) r |= 1;
    if (st & 0x20) r |= 2;
    if (take && (st & 0x21)) {
        uint8_t d = data_read();
        if (st & 0x01) { *v = d; r |= 4; taken++; }
    }
    return r;
}

/* ---- what the hotkeys do ---- */
static int req_next, req_eject, req_exit;
void fdd_hotkey_request(int eject) { if (eject) req_eject++; else req_next++; }
void pistorm_request_exit(int mode) { (void)mode; req_exit++; }

/* ---- the IKBD stream ---- */
#define MAXB 200000
static struct { uint64_t t; uint8_t b; } wire[MAXB];
static int nw;
static void put(uint64_t t, const uint8_t *p, int n)
{
    for (int i = 0; i < n && nw < MAXB; i++) {
        wire[nw].t = t + (uint64_t)i * 1280u;
        wire[nw].b = p[i];
        nw++;
    }
}
static void key(uint64_t t, uint8_t sc) { put(t, &sc, 1); }
static int cmpw(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static uint32_t rng = 1;
static uint8_t rnd(void) { rng = rng * 1103515245u + 12345u; return (uint8_t)(rng >> 16); }
/* the movement bytes are the hotkeys' own scancodes: Ctrl, Alt, Insert,
 * Clr/Home, Undo - a byte taken out of its packet would be a hotkey */
static uint8_t mv(void)
{
    static const uint8_t k[5] = { 0x1D, 0x38, 0x52, 0x47, 0x61 };
    return k[rnd() % 5];
}
static void mouse(uint64_t from, uint64_t to, uint64_t every)
{
    for (uint64_t t = from; t < to; t += every) {
        uint8_t p[3] = { (uint8_t)(0xF8 | (rnd() & 3)), mv(), mv() };
        put(t, p, 3);
    }
}

/* run: listening = the guest reads every byte 50 us after it arrives */
static void run(uint64_t end, int listening)
{
    int wi = 0;
    uint64_t next_poll = 10000, guest_due = 0;
    int guest_pending = 0;
    static uint64_t base;
    qsort(wire, (size_t)nw, sizeof wire[0], cmpw);
    rdrf = lost = ovr_shown = 0;
    ps_acia_kbd_guest_access++;       /* the guest's last keyboard read: t = 0 */
    for (uint64_t t = 0; t <= end; t += 10) {
        t_us = base + t;
        while (wi < nw && wire[wi].t <= t) {
            arrive(wire[wi].b);
            wi++;
            if (listening && !guest_pending) { guest_pending = 1; guest_due = t + 50; }
        }
        if (guest_pending && t >= guest_due) {
            ps_acia_kbd_guest_access++;          /* status read */
            if (status() & 0x21) { ps_acia_kbd_guest_access++; (void)data_read(); }
            guest_pending = (status() & 0x01) != 0;
            guest_due = t + 50;
        }
        if (rasters && t % 20000 == 0) beam_n++;
        if (t >= next_poll) { kbd_hotkey_idle_poll(beam_n); next_poll += 10000; }
    }
    base += end + 1000000;
}
static void reset(void) { nw = 0; req_next = req_eject = req_exit = 0; taken = 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    setenv("PISTORM_HOTKEYS", "1", 1);
    /* 1. silent guest, Ctrl+Alt+Insert held and let go */
    reset();
    key(2000000, 0x1D); key(2300000, 0x38); key(2600000, 0x52);
    key(3000000, 0xD2); key(3050000, 0xB8); key(3100000, 0x9D);
    run(4000000, 0);
    CHECK(req_next == 1 && req_eject == 0, "silent guest: Ctrl+Alt+Insert gave %d next, %d eject", req_next, req_eject);

    /* 2. ...twice (the disk after that one) */
    reset();
    key(2000000, 0x1D); key(2300000, 0x38); key(2600000, 0x52); key(2700000, 0xD2);
    key(3600000, 0x52); key(3700000, 0xD2); key(4000000, 0xB8); key(4050000, 0x9D);
    run(5000000, 0);
    CHECK(req_next == 2, "silent guest: Insert twice with Ctrl+Alt held gave %d", req_next);

    /* 3. Clr/Home and Undo */
    reset();
    key(2000000, 0x1D); key(2300000, 0x38); key(2600000, 0x47); key(2700000, 0xC7);
    key(2900000, 0x61);
    run(4000000, 0);
    CHECK(req_eject == 1 && req_exit == 1, "silent guest: Clr/Home %d, Undo %d", req_eject, req_exit);

    /* 4. a moving mouse never makes a hotkey, whatever its bytes */
    for (int seed = 1; seed <= 300; seed++) {
        reset(); rng = (uint32_t)seed * 2654435761u;
        mouse(1500000, 6000000, 7000 + (uint64_t)(seed % 23) * 1000);
        run(6500000, 0);
        CHECK(req_next == 0 && req_eject == 0 && req_exit == 0,
              "mouse only, seed %d: next %d eject %d exit %d", seed, req_next, req_eject, req_exit);
        if (req_next || req_eject || req_exit) break;
    }

    /* 5. mouse moved, then let go, then the hotkey */
    for (int seed = 1; seed <= 100; seed++) {
        reset(); rng = (uint32_t)seed * 40503u;
        mouse(1200000, 2500000, 10000);
        key(3000000, 0x1D); key(3300000, 0x38); key(3600000, 0x52);
        key(4000000, 0xD2); key(4050000, 0xB8); key(4100000, 0x9D);
        run(5000000, 0);
        CHECK(req_next == 1, "mouse then hotkey, seed %d: %d next", seed, req_next);
        if (req_next != 1) break;
    }

    /* 6. a guest that reads the keyboard: nothing is taken from it */
    reset();
    mouse(1500000, 3000000, 10000);
    key(4000000, 0x1D); key(4300000, 0x38); key(4600000, 0x52);
    key(5000000, 0xD2); key(5050000, 0xB8); key(5100000, 0x9D);
    run(6000000, 1);
    CHECK(taken == 0 && req_next == 0, "listening guest: %u bytes taken, %d next", taken, req_next);

    /* 7. guest silent for under a second: hands off */
    reset();
    key(500000, 0x1D); key(600000, 0x38); key(700000, 0x52);
    run(900000, 0);
    CHECK(taken == 0 && req_next == 0, "under a second of silence: %u taken", taken);

    /* 8. a guest drawing beam-timed rasters is left alone (Jim Power) */
    reset(); rasters = 1;
    key(2000000, 0x1D); key(2300000, 0x38); key(2600000, 0x52);
    run(4000000, 0);
    rasters = 0;
    CHECK(taken == 0 && req_next == 0, "beam-timed rasters running: %u taken, %d next", taken, req_next);

    printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
