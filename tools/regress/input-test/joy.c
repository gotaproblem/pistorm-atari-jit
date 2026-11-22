/* Host test: joy_usb.c's scripted joystick (joy_usb_script_set) through
 * joy_usb_tick with no pad attached - every change, press AND release,
 * must reach send_joy, and the right-button mirror must follow fire. */
#include <stdio.h>
#include <string.h>
#include "platforms/atari/joy_usb.h"
static int sent[64], ns, rb[64], nr;
static int send_joy(int port, uint8_t st, uint8_t pad) { (void)pad; if (port == 1) sent[ns++] = st; return 1; }
static void set_rb(int d) { if (!nr || rb[nr - 1] != d) rb[nr++] = d; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
    static const joy_usb_emit_hooks h = { .send_joy = send_joy, .set_joy_rbutton = set_rb };
    joy_usb_set_hooks(&h);
    const uint8_t seq[] = { 0x80, 0x00, 0x80, 0x00, 0x88, 0x08, 0x00, 0x80, 0x00 };
    for (unsigned k = 0; k < sizeof seq; k++) {
        joy_usb_script_set(1, seq[k]);
        for (int t = 0; t < 5; t++)          /* several ticks per state */
            joy_usb_tick();
    }
    /* each state once, in order (repeats while held are allowed only as
     * the same value) */
    int dedup[64], nd = 0;
    for (int i = 0; i < ns; i++)
        if (!nd || dedup[nd - 1] != sent[i]) dedup[nd++] = sent[i];
    CHECK(nd == (int)sizeof seq, "%d distinct states sent, want %d", nd, (int)sizeof seq);
    for (int i = 0; i < nd && i < (int)sizeof seq; i++)
        CHECK(dedup[i] == seq[i], "state %d: $%02X, want $%02X", i, dedup[i], seq[i]);
    CHECK(joy_usb_state(1) == 0, "joy_usb_state after the last release: $%02X", joy_usb_state(1));
    CHECK(nr >= 8 && rb[nr - 1] == 0, "right-button mirror: %d changes, last %d", nr, nr ? rb[nr - 1] : -1);
    printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
