/*
 * astos_host_stub.c - lets ASTOS build and draw on any Linux with no
 * PiStorm (make astos-host): the bus, DRM and the [native] probe are
 * stubbed. Only for --no-board --shot runs; never linked into ./astos.
 */
#include <stdint.h>
#include "shifter_setup.h"

uint8_t  fc;
uint16_t ps_fw_rev = 0x421;

uint8_t  ps_read_8(uint32_t a)               { (void)a; return 0; }
uint16_t ps_read_16(uint32_t a)              { (void)a; return 0; }
uint32_t ps_read_32(uint32_t a)              { (void)a; return 0; }
void     ps_write_8(uint32_t a, uint16_t d)  { (void)a; (void)d; }
void     ps_write_16(uint32_t a, uint16_t d) { (void)a; (void)d; }
void     ps_write_32(uint32_t a, uint32_t d) { (void)a; (void)d; }
void     ps_setup_protocol(void)             { }
void     ps_get_firmware_revision(void)      { }
void     ps_pulse_halt(void)                 { }
void     ps_pulse_reset(void)                { }
void     ps_reset_state_machine(void)        { }
void     ps_power_wait(void)                 { }
int      ps_power_poll(void)                 { return 0; }
int      ps_st_reset_pin(void)               { return -1; }
int      pistorm_bus_lock(const char *w)     { (void)w; return 0; }
void     pistorm_bus_unlock(void)            { }
int      np_ensure_native_section(const char *p) { (void)p; return 0; }

int  sh_open(void)                           { return -1; }
int  sh_active(void)                         { return 0; }
void sh_present(const struct ss_screen *s)   { (void)s; }
void sh_close(void)                          { }
void sh_set_palette(const uint32_t rgb[4])   { (void)rgb; }
