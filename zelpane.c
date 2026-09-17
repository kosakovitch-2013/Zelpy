#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#define DEFAULT_HEIGHT 24
#define DEFAULT_BG "#1a1b26"
#define DEFAULT_FG "#c0caf5"

static int panel_height = DEFAULT_HEIGHT;
static char bg_color[32] = DEFAULT_BG;
static char fg_color[32] = DEFAULT_FG;

static unsigned long parse_color(Display *dpy, int screen, const char *hex) {
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor color;
    if (!XParseColor(dpy, cmap, hex, &color)) {
        fprintf(stderr, "zelpane: bad color %s\n", hex);
        return BlackPixel(dpy, screen);
    }
    XAllocColor(dpy, cmap, &color);
    return color.pixel;
}

static int get_current_workspace(Display *dpy, Window root) {
    Atom ws_atom = XInternAtom(dpy, "_ZELPY_CURRENT_WORKSPACE", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int ws = 1;
    if (XGetWindowProperty(dpy, root, ws_atom, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) == Success && data) {
        if (nitems >= 1) {
            ws = (int)(*(long *)data);
        }
        XFree(data);
    }
    return ws;
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/zelpane/config", home);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/zelpane", home);
    mkdir(dir, 0755);

    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (!f) return;
        fprintf(f,
            "# Zelpane config\n"
            "height 24\n"
            "bg #1a1b26\n"
            "fg #c0caf5\n"
        );
        fclose(f);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *val = sp + 1;
        while (*val == ' ') val++;
        if (strcmp(line, "height") == 0) {
            panel_height = atoi(val);
            if (panel_height < 8) panel_height = 8;
            if (panel_height > 200) panel_height = 200;
        } else if (strcmp(line, "bg") == 0) {
            strncpy(bg_color, val, sizeof(bg_color) - 1);
        } else if (strcmp(line, "fg") == 0) {
            strncpy(fg_color, val, sizeof(fg_color) - 1);
        }
    }
    fclose(f);
}

int main(void) {
    load_config();

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "zelpane: cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);

    unsigned long bg_pix = parse_color(dpy, screen, bg_color);
    unsigned long fg_pix = parse_color(dpy, screen, fg_color);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, sw, panel_height, 0,
                                     bg_pix, bg_pix);

    XStoreName(dpy, win, "zelpane");

    // Set dock type
    Atom wm_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, win, wm_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&dock_type, 1);

    // _NET_WM_STRUT
    Atom strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    long strut_data[4] = { 0, 0, (long)panel_height, 0 };
    XChangeProperty(dpy, win, strut, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut_data, 4);

    // _NET_WM_STRUT_PARTIAL
    Atom strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    long partial[12] = {
        0, 0, (long)panel_height, 0,
        0, 0,
        0, 0,
        0, (long)(sw - 1),
        0, 0
    };
    XChangeProperty(dpy, win, strut_partial, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)partial, 12);

    XSelectInput(dpy, win, ExposureMask);
    // Also watch root for workspace changes
    XSelectInput(dpy, root, PropertyChangeMask);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    Atom zelpy_ws_atom = XInternAtom(dpy, "_ZELPY_CURRENT_WORKSPACE", False);

    time_t last_sec = 0;
    while (1) {
        time_t now = time(NULL);
        if (now != last_sec) {
            last_sec = now;
            struct tm *tm_info = localtime(&now);
            char buf[64];
            strftime(buf, sizeof(buf), "%a %b %d  %H:%M:%S", tm_info);

            XSetForeground(dpy, gc, bg_pix);
            XFillRectangle(dpy, win, gc, 0, 0, sw, panel_height);

            XSetForeground(dpy, gc, fg_pix);

            int font_ascent = font ? font->ascent : 8;
            int font_descent = font ? font->descent : 2;
            int baseline = (panel_height + font_ascent - font_descent) / 2;

            const char *left = " zelpane";
            XDrawString(dpy, win, gc, 6, baseline, left, strlen(left));

            int tw = font ? XTextWidth(font, buf, strlen(buf)) : (int)strlen(buf) * 6;
            XDrawString(dpy, win, gc, (sw - tw) / 2, baseline, buf, strlen(buf));

            int ws_num = get_current_workspace(dpy, root);
            char right[32];
            snprintf(right, sizeof(right), " ws %d ", ws_num);
            int rw = font ? XTextWidth(font, right, strlen(right)) : (int)strlen(right) * 6;
            XDrawString(dpy, win, gc, sw - rw - 6, baseline, right, strlen(right));

            XFlush(dpy);
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose && ev.xexpose.count == 0) {
                last_sec = 0;
            } else if (ev.type == PropertyNotify && ev.xproperty.atom == zelpy_ws_atom) {
                last_sec = 0;  // force redraw immediately
            }
        }

        struct timeval tv = { 0, 200000 };  // 200ms poll for responsiveness
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
    }

    XCloseDisplay(dpy);
    return 0;
}
