#ifndef ANIMATION_H
#define ANIMATION_H

#include <X11/Xlib.h>

void animation_set_display(Display *dpy);
void animation_start(Window win, int x, int y, int w, int h, int duration_ms);
void animation_cancel(Window win);
void animation_tick(long dt_ms);
int animation_is_active(Window win);

#endif
