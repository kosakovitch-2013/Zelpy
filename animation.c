#include "animation.h"
#include <X11/Xlib.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

#define MAX_ANIMATIONS 64

typedef struct {
    Window win;
    int start_x, start_y, start_w, start_h;
    int target_x, target_y, target_w, target_h;
    int current_x, current_y, current_w, current_h;
    long start_time;
    int duration;
    int active;
} Animation;

static Animation animations[MAX_ANIMATIONS];
static int num_animations = 0;
static Display *anim_dpy = NULL;

static long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void animation_set_display(Display *d) {
    anim_dpy = d;
}

void animation_start(Window win, int x, int y, int w, int h, int duration_ms) {
    if (!anim_dpy || win == None) return;

    // Cancel existing animation for this window
    animation_cancel(win);

    if (num_animations >= MAX_ANIMATIONS) return;

    XWindowAttributes attr;
    if (!XGetWindowAttributes(anim_dpy, win, &attr)) return;

    Animation *a = &animations[num_animations++];
    a->win = win;
    a->start_x = attr.x;
    a->start_y = attr.y;
    a->start_w = attr.width;
    a->start_h = attr.height;
    a->target_x = x;
    a->target_y = y;
    a->target_w = w;
    a->target_h = h;
    a->current_x = a->start_x;
    a->current_y = a->start_y;
    a->current_w = a->start_w;
    a->current_h = a->start_h;
    a->start_time = get_time_ms();
    a->duration = duration_ms;
    a->active = 1;
}

void animation_cancel(Window win) {
    for (int i = 0; i < num_animations; i++) {
        if (animations[i].win == win && animations[i].active) {
            animations[i].active = 0;
            // Shift the rest of the array
            for (int j = i; j < num_animations - 1; j++) {
                animations[j] = animations[j+1];
            }
            num_animations--;
            i--; // recheck this index
        }
    }
}

static float ease_out_cubic(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

void animation_tick(long dt_ms) {
    (void)dt_ms; // we use our own timing via get_time_ms()
    if (!anim_dpy) return;

    long now = get_time_ms();
    for (int i = 0; i < num_animations; i++) {
        Animation *a = &animations[i];
        if (!a->active) continue;

        long elapsed = now - a->start_time;
        if (elapsed >= a->duration) {
            // Finish immediately
            XMoveResizeWindow(anim_dpy, a->win, a->target_x, a->target_y,
                              a->target_w, a->target_h);
            a->active = 0;
            // Remove from array
            for (int j = i; j < num_animations - 1; j++) {
                animations[j] = animations[j+1];
            }
            num_animations--;
            i--;
            continue;
        }

        float t = (float)elapsed / a->duration;
        float eased = ease_out_cubic(t);

        a->current_x = a->start_x + (a->target_x - a->start_x) * eased;
        a->current_y = a->start_y + (a->target_y - a->start_y) * eased;
        a->current_w = a->start_w + (a->target_w - a->start_w) * eased;
        a->current_h = a->start_h + (a->target_h - a->start_h) * eased;

        XMoveResizeWindow(anim_dpy, a->win, a->current_x, a->current_y,
                          a->current_w, a->current_h);
    }
    XFlush(anim_dpy);
}

int animation_is_active(Window win) {
    for (int i = 0; i < num_animations; i++) {
        if (animations[i].win == win && animations[i].active)
            return 1;
    }
    return 0;
}
