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
void files_test_place(int pane, int place);

#endif
