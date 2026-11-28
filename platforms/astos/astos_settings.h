/*
 * astos_settings.h - the settings window for one environment (see
 * astos_settings.c). Drawn over the desktop with set_draw(); keys and
 * clicks go to it while set_is_open(). set_key / set_click return 1 when
 * the window has closed (Close, or Esc - twice with unsaved changes).
 */
#ifndef ASTOS_SETTINGS_H
#define ASTOS_SETTINGS_H

#include "setup_input.h"

int  set_open(const char *cfg_path, const char *section, const char *title);
int  set_is_open(void);
void set_draw(void);
int  set_key(struct si_event k);
int  set_click(int x, int y);
int  set_hover(int x, int y);
int  set_release(int x, int y);       /* left button up; 1 = redraw */
const char *set_section(void);

#endif
