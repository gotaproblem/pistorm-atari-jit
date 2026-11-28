/*
 * astos_files.h - the file manager window (see astos_files.c). Drawn
 * over the desktop with files_draw(); keys and clicks go to it while
 * files_is_open(). files_key / files_click return 1 when it has closed.
 */
#ifndef ASTOS_FILES_H
#define ASTOS_FILES_H

#include "setup_input.h"

int  files_open(const char *cfg_path);
int  files_is_open(void);
void files_draw(void);
int  files_key(struct si_event k);
int  files_click(int x, int y);
int  files_hover(int x, int y);
/* drag and drop: every pointer move with the left button state, and the
 * left button's release; 1 = redraw */
int  files_motion(int x, int y, int held);
int  files_release(int x, int y);
/* the wheel over the window: n notches, + = up */
int  files_wheel(int x, int y, int n);
void files_test_place(int pane, int place);
/* 1 once after Files changed psctrl.cfg (Use in): reload the desktop */
int  files_cfg_changed(void);

#endif
