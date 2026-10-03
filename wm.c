#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include "animation.h"

#define MODKEY Mod1Mask
#define SUPERKEY Mod4Mask
#define BORDER_WIDTH_DEFAULT 2
#define RESIZE_THRESHOLD 10
#define MIN_WIDTH 50
#define MIN_HEIGHT 50
#define MAX_KEYBINDS 64
#define MAX_STARTUP_CMDS 64
#define MAX_VARS 64
#define MAX_WORKSPACES 9
#define MAX_OPACITY_RULES 32
#define ANIMATION_DURATION 200

#define LAYOUT_HORIZONTAL 0
#define LAYOUT_VERTICAL   1
#define LAYOUT_DWINDLE    2
#define LAYOUT_MASTER     3

// ---------- Config ----------
static char focus_color[64] = "#ebbcba";
static char unfocus_color[64] = "#444444";
static char terminal_cmd[256] = "st";
static int border_width = BORDER_WIDTH_DEFAULT;
static int gap_horizontal = 0;
static int gap_vertical = 0;
static int layout_mode = LAYOUT_HORIZONTAL;

static char startup_commands[MAX_STARTUP_CMDS][256];
static int num_startup_commands = 0;

static char var_names[MAX_VARS][64];
static char var_values[MAX_VARS][256];
static int num_vars = 0;

typedef struct { char pattern[128]; double opacity; } OpacityRule;
static OpacityRule opacity_rules[MAX_OPACITY_RULES];
static int num_opacity_rules = 0;

enum { ACTION_NONE, ACTION_SPAWN, ACTION_CLOSE, ACTION_TOGGLE_FLOATING,
       ACTION_TOGGLE_FULLSCREEN, ACTION_QUIT, ACTION_WORKSPACE, ACTION_LAYOUT_TOGGLE };

typedef struct {
    unsigned int mod;
    KeySym keysym;
    int action;
    char command[256];
    int workspace_num;
} Keybind;
static Keybind keybinds[MAX_KEYBINDS];
static int num_keybinds = 0;

// ---------- Globals ----------
static Display *dpy;
static Window root;
static int screen_width, screen_height;
static Window focused_window = None;
static Window fullscreen_window = None;

static int dragging = 0, drag_start_x, drag_start_y;
static Window drag_window;

static int resizing = 0, resize_direction = 0;
static Window resize_window = None;
static int resize_start_x, resize_start_y;
static int resize_start_w, resize_start_h, resize_start_xpos, resize_start_ypos;

static Window floating_windows[128];
static int num_floating = 0;
static Window floating_stack[128];
static int num_floating_stack = 0;

static Window managed_windows[128];
static int managed_workspace[128];
static int num_managed = 0;

static int fullscreen_orig_x, fullscreen_orig_y, fullscreen_orig_w, fullscreen_orig_h;
static int fullscreen_orig_was_floating = 0;
static int current_workspace = 1;
static int top_offset = 0;

// Window swallowing

// Dwindle tree
typedef struct DNode {
    Window win;
    struct DNode *parent, *left, *right;
    int split_horizontal;
} DNode;
static DNode *dwindle_roots[MAX_WORKSPACES + 1] = {0};
static Window dwindle_last_focused = None;

// ---------- Atoms ----------
static Atom wm_protocols, wm_take_focus, wm_delete_window;
static Atom net_wm_state, net_wm_state_fullscreen, net_active_window;
static Atom net_wm_window_type, net_wm_window_type_dock, net_wm_window_opacity;
static Atom net_wm_strut;
static Atom zelpy_workspace_atom, zelpy_layout_atom;
static Atom zelpy_cmd_workspace, zelpy_cmd_reload, zelpy_cmd_retile;
static Atom zelpy_cmd_quit, zelpy_cmd_layout, zelpy_cmd_layout_toggle;
static Atom net_supporting_wm_check, net_wm_name, utf8_string;

// ---------- Forward declarations ----------
void tile_windows(void);
void tile_dwindle(void);
int  window_exists(Window w);
void update_focus_from_pointer(void);
void raise_floating_windows(void);
int  is_floating(Window w);
int  is_dock_window(Window w);
void add_managed_window(Window w);
void remove_managed_window(Window w);
void spawn_startup_commands(void);
void switch_workspace(int ws);
const char *get_var_value(const char *name);
void expand_variables(const char *input, char *output, size_t out_size);
void apply_opacity_rules(Window w);
void update_struts(void);
void raise_docks(void);
void broadcast_workspace(void);
void broadcast_layout(void);
void toggle_layout(void);

int xerrorhandler(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

// ---------- Small helpers ----------

// Reads an ATOM property (single value) from window w.
static Atom get_atom_prop(Window w, Atom prop) {
    Atom actual_type; int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    Atom result = None;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) == Success && data) {
        if (nitems >= 1) result = *(Atom *)data;
        XFree(data);
    }
    return result;
}

int window_exists(Window w) {
    if (w == None) return 0;
    XWindowAttributes a;
    XErrorHandler old = XSetErrorHandler(xerrorhandler);
    int r = XGetWindowAttributes(dpy, w, &a);
    XSetErrorHandler(old);
    return r == 1;
}

int is_dock_window(Window w) {
    if (!window_exists(w)) return 0;
    return get_atom_prop(w, net_wm_window_type) == net_wm_window_type_dock;
}

Window get_toplevel(Window w) {
    Window rr, pr, *ch; unsigned int n;
    Window top = w;
    while (1) {
        if (!XQueryTree(dpy, top, &rr, &pr, &ch, &n)) return w;
        if (ch) XFree(ch);
        if (pr == root || pr == None) return top;
        top = pr;
        if (top == root || top == None) return w;
    }
}

void set_border_width(Window w, int width) {
    if (window_exists(w)) XSetWindowBorderWidth(dpy, w, width);
}

void set_border_color(Window w, const char *color) {
    if (!window_exists(w)) return;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor c;
    XParseColor(dpy, cmap, color, &c);
    XAllocColor(dpy, cmap, &c);
    XSetWindowBorder(dpy, w, c.pixel);
    XSetWindowBorderWidth(dpy, w, border_width);
}

Window get_window_under_cursor(void) {
    Window rr, cr; int rx, ry, wx, wy; unsigned int mask;
    if (XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) {
        if (cr != None) return get_toplevel(cr);
    }
    return None;
}

int is_managed_window(Window w) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return 0;
    if (a.override_redirect || a.map_state != IsViewable) return 0;
    if (is_dock_window(w)) return 0;
    Window top = get_toplevel(w);
    if (top != w && top != root && top != None) return 0;
    return 1;
}

// ---------- Windows / managed list ----------

void add_managed_window(Window w) {
    if (!is_managed_window(w)) return;
    for (int i = 0; i < num_managed; i++)
        if (managed_windows[i] == w) return;
    if (num_managed < 128) {
        managed_windows[num_managed] = w;
        managed_workspace[num_managed] = current_workspace;
        num_managed++;
        apply_opacity_rules(w);
    }
}

void remove_managed_window(Window w) {
    for (int i = 0; i < num_managed; i++) {
        if (managed_windows[i] == w) {
            for (int j = i; j < num_managed - 1; j++) {
                managed_windows[j] = managed_windows[j+1];
                managed_workspace[j] = managed_workspace[j+1];
            }
            num_managed--;
            return;
        }
    }
}

int is_floating(Window w) {
    for (int i = 0; i < num_floating; i++)
        if (floating_windows[i] == w) return 1;
    return 0;
}

void add_floating(Window w) {
    if (is_floating(w) || num_floating >= 128) return;
    floating_windows[num_floating++] = w;
    if (num_floating_stack < 128)
        floating_stack[num_floating_stack++] = w;
}

void remove_floating(Window w) {
    for (int i = 0; i < num_floating; i++) {
        if (floating_windows[i] == w) {
            for (int j = i; j < num_floating - 1; j++)
                floating_windows[j] = floating_windows[j+1];
            num_floating--;
            break;
        }
    }
    for (int i = 0; i < num_floating_stack; i++) {
        if (floating_stack[i] == w) {
            for (int j = i; j < num_floating_stack - 1; j++)
                floating_stack[j] = floating_stack[j+1];
            num_floating_stack--;
            break;
        }
    }
}

void raise_floating_windows(void) {
    for (int i = 0; i < num_floating_stack; i++)
        if (window_exists(floating_stack[i]))
            XRaiseWindow(dpy, floating_stack[i]);
    XFlush(dpy);
}

void float_stack_raise(Window w) {
    for (int i = 0; i < num_floating_stack; i++) {
        if (floating_stack[i] == w) {
            for (int j = i; j < num_floating_stack - 1; j++)
                floating_stack[j] = floating_stack[j+1];
            floating_stack[num_floating_stack - 1] = w;
            return;
        }
    }
}

void raise_docks(void) {
    if (fullscreen_window != None && window_exists(fullscreen_window)) return;
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &ch, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        if (!window_exists(ch[i])) continue;
        if (get_atom_prop(ch[i], net_wm_window_type) == net_wm_window_type_dock)
            XRaiseWindow(dpy, ch[i]);
    }
    if (ch) XFree(ch);
    XFlush(dpy);
}

void update_struts(void) {
    top_offset = 0;
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &ch, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        if (!window_exists(ch[i])) continue;
        Atom type; int fmt; unsigned long ni, ba; unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, ch[i], net_wm_strut, 0, 4, False,
                               XA_CARDINAL, &type, &fmt, &ni, &ba, &data) == Success && data) {
            if (ni >= 4) {
                unsigned long *s = (unsigned long *)data;
                if (s[2] > 0 && s[2] < (unsigned long)screen_height / 2
                    && (int)s[2] > top_offset)
                    top_offset = (int)s[2];
            }
            XFree(data);
        }
    }
    if (ch) XFree(ch);
}

// ---------- Focus / highlight ----------

void send_wm_take_focus(Window w) {
    Atom *protocols; int num;
    if (!XGetWMProtocols(dpy, w, &protocols, &num)) return;
    for (int i = 0; i < num; i++) {
        if (protocols[i] == wm_take_focus) {
            XEvent ev; memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = w;
            ev.xclient.message_type = wm_protocols;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = wm_take_focus;
            ev.xclient.data.l[1] = CurrentTime;
            XSendEvent(dpy, w, False, NoEventMask, &ev);
            XFlush(dpy);
            break;
        }
    }
    XFree(protocols);
}

void set_focus_to_window(Window w) {
    if (w == None || w == root || !window_exists(w)) return;
    if (is_dock_window(w)) return;
    send_wm_take_focus(w);
    XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
    XRaiseWindow(dpy, w);
    if (is_floating(w)) float_stack_raise(w);
    else raise_floating_windows();
    raise_docks();
    XFlush(dpy);
    focused_window = w;
    dwindle_last_focused = w;
}

void update_highlight(void) {
    Window cur = get_window_under_cursor();
    if (cur != None && cur != root && window_exists(cur))
        focused_window = cur;
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &ch, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        if (!window_exists(ch[i])) continue;
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, ch[i], &a)) continue;
        if (a.map_state != IsViewable || a.override_redirect) continue;
        Window top = get_toplevel(ch[i]);
        if (!window_exists(top)) continue;
        set_border_color(top, top == focused_window ? focus_color : unfocus_color);
    }
    if (ch) XFree(ch);
}

void update_focus_from_pointer(void) {
    Window w = get_window_under_cursor();
    if (w != None && w != root && window_exists(w) && w != focused_window) {
        set_focus_to_window(w);
        update_highlight();
    }
}

// ---------- Workspace / layout broadcast ----------

void broadcast_workspace(void) {
    long ws = current_workspace;
    XChangeProperty(dpy, root, zelpy_workspace_atom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&ws, 1);
    XFlush(dpy);
}

void broadcast_layout(void) {
    long m = layout_mode;
    XChangeProperty(dpy, root, zelpy_layout_atom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&m, 1);
    XFlush(dpy);
}

// ---------- Fullscreen ----------

void enter_fullscreen(Window w) {
    if (!window_exists(w) || is_dock_window(w)) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return;
    fullscreen_orig_x = a.x; fullscreen_orig_y = a.y;
    fullscreen_orig_w = a.width; fullscreen_orig_h = a.height;
    fullscreen_orig_was_floating = is_floating(w);
    fullscreen_window = w;
    animation_start(w, 0, 0, screen_width, screen_height, ANIMATION_DURATION);
    set_border_width(w, 0);
    XRaiseWindow(dpy, w);
    raise_docks();
}

void exit_fullscreen(Window w) {
    if (!window_exists(w)) return;
    fullscreen_window = None;
    raise_docks();
    set_border_width(w, border_width);
    if (fullscreen_orig_was_floating) {
        animation_start(w, fullscreen_orig_x, fullscreen_orig_y,
                        fullscreen_orig_w, fullscreen_orig_h, ANIMATION_DURATION);
        add_floating(w);
        raise_floating_windows();
    } else {
        tile_windows();
    }
    update_highlight();
}

void toggle_fullscreen(void) {
    Window w = get_window_under_cursor();
    if (!window_exists(w) || is_dock_window(w)) return;
    if (fullscreen_window == w) exit_fullscreen(w);
    else enter_fullscreen(w);

    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (fullscreen_window == w) ? 1 : 0;
    ev.xclient.data.l[1] = net_wm_state_fullscreen;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

// ---------- Close / float ----------

void close_window(Window w) {
    w = get_toplevel(w);
    if (!window_exists(w) || is_dock_window(w)) return;
    Atom *protocols; int num; int has_delete = 0;
    if (XGetWMProtocols(dpy, w, &protocols, &num)) {
        for (int i = 0; i < num; i++)
            if (protocols[i] == wm_delete_window) { has_delete = 1; break; }
        XFree(protocols);
    }
    if (has_delete) {
        XEvent ev; memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = wm_delete_window;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
        XFlush(dpy);
    } else {
        XKillClient(dpy, w);
        XFlush(dpy);
    }
}

void toggle_floating(void) {
    Window w = get_window_under_cursor();
    if (!window_exists(w) || is_dock_window(w)) return;
    if (is_floating(w)) {
        remove_floating(w);
        tile_windows();
        update_highlight();
    } else {
        add_floating(w);
        int fw = screen_width / 3, fh = screen_height / 3;
        int fx = (screen_width - fw) / 2;
        int fy = (screen_height - fh) / 2 + top_offset;
        animation_start(w, fx, fy, fw, fh, ANIMATION_DURATION);
        set_border_color(w, focus_color);
        set_border_width(w, border_width);
        fullscreen_window = None;
        raise_floating_windows();
    }
}

// ---------- Move / resize ----------

void start_drag(Window w, int x, int y) {
    w = get_toplevel(w);
    if (!window_exists(w) || is_dock_window(w)) return;
    animation_cancel(w);
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return;
    dragging = 1;
    drag_window = w;
    drag_start_x = a.x - x;
    drag_start_y = a.y - y;
    XRaiseWindow(dpy, w);
    if (is_floating(w)) { float_stack_raise(w); raise_floating_windows(); }
}

void do_drag(int x, int y) {
    if (!dragging || !window_exists(drag_window)) {
        dragging = 0; drag_window = None; return;
    }
    XMoveWindow(dpy, drag_window, drag_start_x + x, drag_start_y + y);
}

void stop_drag(void) { dragging = 0; drag_window = None; }

int get_resize_direction(Window w, int rx, int ry) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return 0;
    int near_left  = (rx - a.x <= RESIZE_THRESHOLD) && (rx >= a.x);
    int near_right = ((a.x + a.width) - rx <= RESIZE_THRESHOLD) && (rx <= a.x + a.width);
    int near_top   = (ry - a.y <= RESIZE_THRESHOLD) && (ry >= a.y);
    int near_bot   = ((a.y + a.height) - ry <= RESIZE_THRESHOLD) && (ry <= a.y + a.height);
    if (near_left && near_top) return 1;
    if (near_right && near_top) return 3;
    if (near_left && near_bot) return 6;
    if (near_right && near_bot) return 8;
    if (near_left) return 4;
    if (near_right) return 5;
    if (near_top) return 2;
    if (near_bot) return 7;
    return 0;
}

void start_resize(Window w, int rx, int ry) {
    if (is_dock_window(w)) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return;
    animation_cancel(w);
    resizing = 1;
    resize_window = w;
    resize_direction = get_resize_direction(w, rx, ry);
    resize_start_x = rx; resize_start_y = ry;
    resize_start_xpos = a.x; resize_start_ypos = a.y;
    resize_start_w = a.width; resize_start_h = a.height;
    XRaiseWindow(dpy, w);
    if (is_floating(w)) { float_stack_raise(w); raise_floating_windows(); }
}

void do_resize(int rx, int ry) {
    if (!resizing || resize_window == None) return;
    int dx = rx - resize_start_x, dy = ry - resize_start_y;
    int nx = resize_start_xpos, ny = resize_start_ypos;
    int nw = resize_start_w, nh = resize_start_h;
    switch (resize_direction) {
        case 1: nx += dx; ny += dy; nw -= dx; nh -= dy; break;
        case 2: ny += dy; nh -= dy; break;
        case 3: ny += dy; nw += dx; nh -= dy; break;
        case 4: nx += dx; nw -= dx; break;
        case 5: nw += dx; break;
        case 6: nx += dx; nw -= dx; nh += dy; break;
        case 7: nh += dy; break;
        case 8: nw += dx; nh += dy; break;
    }
    if (nw < MIN_WIDTH) nw = MIN_WIDTH;
    if (nh < MIN_HEIGHT) nh = MIN_HEIGHT;
    if (nx < 0) nx = 0;
    if (ny < top_offset) ny = top_offset;
    if (nx + nw > screen_width) nw = screen_width - nx;
    if (ny + nh > screen_height) nh = screen_height - ny;
    if (window_exists(resize_window)) {
        XMoveResizeWindow(dpy, resize_window, nx, ny, nw, nh);
        XFlush(dpy);
    }
}

void stop_resize(void) { resizing = 0; resize_window = None; resize_direction = 0; }

static DNode *dnode_create(Window win, int split_h) {
    DNode *n = calloc(1, sizeof(DNode));
    n->win = win; n->split_horizontal = split_h;
    return n;
}

static DNode *dnode_find(DNode *r, Window win) {
    if (!r) return NULL;
    if (r->win == win) return r;
    DNode *l = dnode_find(r->left, win);
    return l ? l : dnode_find(r->right, win);
}

static void dnode_free(DNode *n) {
    if (!n) return;
    dnode_free(n->left);
    dnode_free(n->right);
    free(n);
}

static void dnode_insert(DNode *tree, Window new_win, Window target_win) {
    DNode *t = dnode_find(tree, target_win);
    if (!t) return;
    int split_h = 1;
    XWindowAttributes a;
    if (XGetWindowAttributes(dpy, t->win, &a))
        split_h = (a.width >= a.height) ? 1 : 0;
    DNode *old_leaf = dnode_create(t->win, 0);
    DNode *new_leaf = dnode_create(new_win, 0);
    t->win = None;
    t->split_horizontal = split_h;
    t->left = old_leaf; old_leaf->parent = t;
    t->right = new_leaf; new_leaf->parent = t;
}

static void dnode_layout(DNode *n, int x, int y, int w, int h) {
    if (!n) return;
    if (n->win != None) {
        int tw = w - 2 * border_width;
        int th = h - 2 * border_width;
        if (tw < MIN_WIDTH) tw = MIN_WIDTH;
        if (th < MIN_HEIGHT) th = MIN_HEIGHT;
        animation_start(n->win, x, y, tw, th, ANIMATION_DURATION);
        set_border_width(n->win, border_width);
        return;
    }
    int gx = gap_horizontal, gy = gap_vertical;
    if (n->split_horizontal) {
        int half = (w - gx) / 2;
        dnode_layout(n->left,  x, y, half, h);
        dnode_layout(n->right, x + half + gx, y, w - half - gx, h);
    } else {
        int half = (h - gy) / 2;
        dnode_layout(n->left,  x, y, w, half);
        dnode_layout(n->right, x, y + half + gy, w, h - half - gy);
    }
}

static void dwindle_rebuild(void) {
    int ws = current_workspace;
    if (dwindle_roots[ws]) { dnode_free(dwindle_roots[ws]); dwindle_roots[ws] = NULL; }
    Window first = None;
    for (int i = 0; i < num_managed; i++) {
        Window w = managed_windows[i];
        if (!window_exists(w)) continue;
        if (managed_workspace[i] != ws) continue;
        if (w == fullscreen_window || is_floating(w)) continue;
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, w, &a)) continue;
        if (a.map_state != IsViewable || a.override_redirect) continue;

        if (first == None) {
            first = w;
            dwindle_roots[ws] = dnode_create(w, 0);
        } else {
            Window target = dwindle_last_focused;
            if (target == None || !dnode_find(dwindle_roots[ws], target))
                target = first;
            if (target == w) target = first;
            dnode_insert(dwindle_roots[ws], w, target);
        }
    }
}

void tile_dwindle(void) {
    int ws = current_workspace;
    if (!dwindle_roots[ws]) dwindle_rebuild();
    if (!dwindle_roots[ws]) return;
    int gx = gap_horizontal, gy = gap_vertical;
    dnode_layout(dwindle_roots[ws], gx, top_offset + gy,
                 screen_width - 2 * gx, screen_height - top_offset - 2 * gy);
}

// ---------- Tiling ----------

static void collect_visible(Window *out, int *count) {
    *count = 0;
    for (int i = 0; i < num_managed && *count < 128; i++) {
        Window w = managed_windows[i];
        if (!window_exists(w)) continue;
        if (managed_workspace[i] != current_workspace) continue;
        if (w == fullscreen_window || is_floating(w)) continue;
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, w, &a) && a.map_state == IsViewable
            && !a.override_redirect)
            out[(*count)++] = w;
    }
}

void tile_windows(void) {
    update_struts();
    raise_floating_windows();
    raise_docks();

    if (layout_mode == LAYOUT_DWINDLE) {
        dwindle_rebuild();
        tile_dwindle();
        return;
    }

    Window vis[128]; int count = 0;
    collect_visible(vis, &count);
    if (count == 0) return;

    int gx = gap_horizontal, gy = gap_vertical;
    int ax = gx, ay = top_offset + gy;
    int aw = screen_width - 2 * gx;
    int ah = screen_height - top_offset - 2 * gy;

    if (layout_mode == LAYOUT_HORIZONTAL) {
        int total_w = aw - (count - 1) * gx;
        int width = total_w / count;
        for (int i = 0; i < count; i++) {
            int x = ax + i * (width + gx);
            animation_start(vis[i], x, ay, width - 2*border_width,
                            ah - 2*border_width, ANIMATION_DURATION);
            set_border_width(vis[i], border_width);
        }
    } else if (layout_mode == LAYOUT_VERTICAL) {
        int total_h = ah - (count - 1) * gy;
        int height = total_h / count;
        for (int i = 0; i < count; i++) {
            int y = ay + i * (height + gy);
            animation_start(vis[i], ax, y, aw - 2*border_width,
                            height - 2*border_width, ANIMATION_DURATION);
            set_border_width(vis[i], border_width);
        }
    } else { // LAYOUT_MASTER
        if (count == 1) {
            animation_start(vis[0], ax, ay, aw - 2*border_width,
                            ah - 2*border_width, ANIMATION_DURATION);
            set_border_width(vis[0], border_width);
            return;
        }
        int master_w = (aw - gx) * 60 / 100;
        int stack_x = ax + master_w + gx;
        int stack_w = aw - master_w - gx;
        animation_start(vis[0], ax, ay, master_w - 2*border_width,
                        ah - 2*border_width, ANIMATION_DURATION);
        set_border_width(vis[0], border_width);

        int sc = count - 1;
        int total_stack_h = ah - (sc - 1) * gy;
        int stack_h = total_stack_h / sc;
        for (int i = 1; i < count; i++) {
            int y = ay + (i - 1) * (stack_h + gy);
            animation_start(vis[i], stack_x, y, stack_w - 2*border_width,
                            stack_h - 2*border_width, ANIMATION_DURATION);
            set_border_width(vis[i], border_width);
        }
    }
}

void toggle_layout(void) {
    layout_mode = (layout_mode + 1) % 4;
    broadcast_layout();
    tile_windows();
    const char *names[] = { "horizontal", "vertical", "dwindle", "master" };
    fprintf(stderr, "Zelpy: layout = %s\n", names[layout_mode]);
}

// ---------- Config parsing ----------

const char *get_var_value(const char *name) {
    for (int i = 0; i < num_vars; i++)
        if (strcmp(var_names[i], name) == 0) return var_values[i];
    return NULL;
}

void expand_variables(const char *in, char *out, size_t os) {
    if (!in || !out || !os) return;
    size_t o = 0, n = strlen(in);
    for (size_t i = 0; i < n && o < os - 1; i++) {
        if (in[i] == '@') {
            size_t j = i + 1;
            while (j < n && (isalnum(in[j]) || in[j] == '_')) j++;
            if (j > i + 1) {
                char name[64]; size_t len = j - i - 1;
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                strncpy(name, in + i + 1, len); name[len] = '\0';
                const char *v = get_var_value(name);
                if (v) {
                    size_t vl = strlen(v);
                    if (o + vl < os - 1) { strcpy(out + o, v); o += vl; }
                    i = j - 1;
                    continue;
                }
            }
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

unsigned int parse_mod(const char *m) {
    unsigned int mod = 0;
    if (strstr(m, "Mod1"))  mod |= Mod1Mask;
    if (strstr(m, "Mod4"))  mod |= Mod4Mask;
    if (strstr(m, "Shift")) mod |= ShiftMask;
    if (strstr(m, "Ctrl"))  mod |= ControlMask;
    return mod;
}

int parse_action(const char *a) {
    if (!strcmp(a, "spawn"))             return ACTION_SPAWN;
    if (!strcmp(a, "close"))             return ACTION_CLOSE;
    if (!strcmp(a, "toggle_floating"))   return ACTION_TOGGLE_FLOATING;
    if (!strcmp(a, "toggle_fullscreen")) return ACTION_TOGGLE_FULLSCREEN;
    if (!strcmp(a, "quit"))              return ACTION_QUIT;
    if (!strcmp(a, "workspace"))         return ACTION_WORKSPACE;
    if (!strcmp(a, "layout_toggle"))     return ACTION_LAYOUT_TOGGLE;
    return ACTION_NONE;
}

void apply_opacity_rules(Window w) {
    if (!window_exists(w)) return;
    char *title = NULL; XFetchName(dpy, w, &title);
    XClassHint ch; int has = XGetClassHint(dpy, w, &ch);
    char *cn = has ? ch.res_name : NULL;
    char *cc = has ? ch.res_class : NULL;

    for (int i = 0; i < num_opacity_rules; i++) {
        int m = 0;
        if (title && strcasestr(title, opacity_rules[i].pattern)) m = 1;
        if (!m && cn && strcasestr(cn, opacity_rules[i].pattern)) m = 1;
        if (!m && cc && strcasestr(cc, opacity_rules[i].pattern)) m = 1;
        if (m) {
            unsigned long v = (unsigned long)(opacity_rules[i].opacity * 0xffffffffUL);
            XChangeProperty(dpy, w, net_wm_window_opacity, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char *)&v, 1);
            XFlush(dpy);
            break;
        }
    }
    if (title) XFree(title);
    if (has) {
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
}

void parse_config_line(char *line) {
    line[strcspn(line, "\n")] = 0;
    if (line[0] == '#' || line[0] == '\0') return;
    char *sp = strchr(line, ' ');
    if (!sp) return;
    *sp = '\0';
    char *name = line;
    char *value = sp + 1;
    while (*value == ' ') value++;

    if (!strcmp(name, "border_width")) border_width = atoi(value);
    else if (!strcmp(name, "gap_horizontal")) gap_horizontal = atoi(value);
    else if (!strcmp(name, "gap_vertical")) gap_vertical = atoi(value);
    else if (!strcmp(name, "gap")) { int g = atoi(value); gap_horizontal = g; gap_vertical = g; }
    else if (!strcmp(name, "layout")) {
        if      (!strcmp(value, "vertical")) layout_mode = LAYOUT_VERTICAL;
        else if (!strcmp(value, "dwindle"))  layout_mode = LAYOUT_DWINDLE;
        else if (!strcmp(value, "master"))   layout_mode = LAYOUT_MASTER;
        else                                 layout_mode = LAYOUT_HORIZONTAL;
    }
    else if (!strcmp(name, "focus_color"))   strncpy(focus_color, value, 63);
    else if (!strcmp(name, "unfocus_color")) strncpy(unfocus_color, value, 63);
    else if (!strcmp(name, "terminal")) {
        if (num_vars < MAX_VARS) {
            strncpy(var_names[num_vars], "terminal", 63);
            strncpy(var_values[num_vars], value, 255);
            num_vars++;
        }
        strncpy(terminal_cmd, value, 255);
    }
    else if (!strcmp(name, "execute")) {
        size_t len = strlen(value);
        if (len >= 2 && value[0] == '"' && value[len-1] == '"') {
            value[len-1] = '\0'; value++;
        }
        if (num_startup_commands < MAX_STARTUP_CMDS)
            strncpy(startup_commands[num_startup_commands++], value, 255);
    }
    else if (!strcmp(name, "opacity")) {
        char *p1 = strchr(value, '"'); if (!p1) return;
        p1++;
        char *p2 = strchr(p1, '"'); if (!p2) return;
        *p2 = '\0';
        char *ops = p2 + 1; while (*ops == ' ') ops++;
        double o = atof(ops);
        if (o > 1.0) o = 1.0;
        if (o < 0.0) o = 0.0;
        if (num_opacity_rules < MAX_OPACITY_RULES) {
            strncpy(opacity_rules[num_opacity_rules].pattern, p1, 127);
            opacity_rules[num_opacity_rules].pattern[127] = '\0';
            opacity_rules[num_opacity_rules].opacity = o;
            num_opacity_rules++;
        }
    }
    else if (!strcmp(name, "keybind")) {
        char *combo = strtok(value, " ");   if (!combo) return;
        char *act_s = strtok(NULL, " ");    if (!act_s) return;
        char *args  = strtok(NULL, "");
        char *dash = strchr(combo, '-');    if (!dash) return;
        *dash = '\0';
        unsigned int mod = parse_mod(combo);
        if (!mod) return;
        char key[64];
        strncpy(key, dash + 1, 63); key[63] = '\0';
        for (int i = 0; key[i]; i++) key[i] = tolower(key[i]);
        KeySym ks = XStringToKeysym(key);
        if (ks == NoSymbol) ks = XStringToKeysym(dash + 1);
        if (ks == NoSymbol) { fprintf(stderr, "Zelpy: unknown key %s\n", dash+1); return; }
        int action = parse_action(act_s);
        if (action == ACTION_NONE) { fprintf(stderr, "Zelpy: unknown action %s\n", act_s); return; }
        if (num_keybinds >= MAX_KEYBINDS) return;
        Keybind *kb = &keybinds[num_keybinds++];
        kb->mod = mod; kb->keysym = ks; kb->action = action;
        kb->workspace_num = 0; kb->command[0] = '\0';
        if (action == ACTION_SPAWN && args) {
            char exp[256]; expand_variables(args, exp, sizeof(exp));
            snprintf(kb->command, sizeof(kb->command), "%s", exp);
        } else if (action == ACTION_WORKSPACE && args) {
            int ws = atoi(args);
            if (ws >= 1 && ws <= MAX_WORKSPACES) kb->workspace_num = ws;
        }
    }
    else {
        if (num_vars < MAX_VARS) {
            strncpy(var_names[num_vars], name, 63);
            strncpy(var_values[num_vars], value, 255);
            num_vars++;
        }
    }
}

void load_config(void) {
    char path[512];
    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.config/zelpy/config", home);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config", home); mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/zelpy", home); mkdir(dir, 0755);

    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (!f) return;
        fprintf(f,
            "# Zelpy default config\n"
            "border_width 2\n"
            "gap 0\n"
            "# layout: horizontal | vertical | dwindle | master\n"
            "layout horizontal\n"
            "focus_color #ebbcba\n"
            "unfocus_color #444444\n"
            "terminal st\n"
            "\n"
            "execute \"feh --bg-fill ~/wallpaper.jpg\"\n"
            "execute \"xcompmgr -c -s &\"\n"
            "execute \"zelpane &\"\n"
            "\n"
            "# opacity \"firefox\" 0.9\n"
            "\n"
            "keybind Mod1-Return spawn @terminal\n"
            "keybind Mod1-t close\n"
            "keybind Mod1-v toggle_floating\n"
            "keybind Mod1-f toggle_fullscreen\n"
            "keybind Mod1-l layout_toggle\n"
            "keybind Mod1-q quit\n"
            "keybind Mod1-space spawn rofi -show drun\n"
            "keybind Mod1-1 workspace 1\n"
            "keybind Mod1-2 workspace 2\n"
            "keybind Mod1-3 workspace 3\n"
            "keybind Mod1-4 workspace 4\n"
            "keybind Mod1-5 workspace 5\n"
            "keybind Mod1-6 workspace 6\n"
            "keybind Mod1-7 workspace 7\n"
            "keybind Mod1-8 workspace 8\n"
            "keybind Mod1-9 workspace 9\n"
        );
        fclose(f);
        f = fopen(path, "r");
        if (!f) return;
    }
    num_keybinds = 0; num_startup_commands = 0;
    num_vars = 0; num_opacity_rules = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) parse_config_line(line);
    fclose(f);
    fprintf(stderr, "Zelpy: %d keybinds, %d cmds, %d vars, %d opacity rules\n",
            num_keybinds, num_startup_commands, num_vars, num_opacity_rules);
}

// ---------- Startup / keys ----------

void spawn_startup_commands(void) {
    for (int i = 0; i < num_startup_commands; i++)
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "/bin/sh", "-c", startup_commands[i], NULL);
            exit(0);
        }
}

void grab_keys(void) {
    for (int i = 0; i < num_keybinds; i++) {
        KeyCode c = XKeysymToKeycode(dpy, keybinds[i].keysym);
        if (c) XGrabKey(dpy, c, keybinds[i].mod, root, True,
                        GrabModeAsync, GrabModeAsync);
        else fprintf(stderr, "Zelpy: no keycode for %lu\n", keybinds[i].keysym);
    }
}

void apply_config(void) {
    grab_keys();
    for (int i = 0; i < num_managed; i++)
        if (window_exists(managed_windows[i]))
            apply_opacity_rules(managed_windows[i]);
}

void sighup_handler(int sig) {
    (void)sig;
    load_config();
    apply_config();
    update_highlight();
    broadcast_layout();
    tile_windows();
}

// ---------- Workspaces ----------

void switch_workspace(int ws) {
    if (ws < 1 || ws > MAX_WORKSPACES || ws == current_workspace) return;
    current_workspace = ws;
    broadcast_workspace();
    dwindle_last_focused = None;
    for (int i = 0; i < num_managed; i++) {
        Window w = managed_windows[i];
        if (!window_exists(w)) continue;
        if (managed_workspace[i] != current_workspace) XUnmapWindow(dpy, w);
        else XMapWindow(dpy, w);
    }
    XFlush(dpy);
    tile_windows();
    update_focus_from_pointer();
    raise_floating_windows();
    raise_docks();
}

// ---------- Setup ----------

void setup(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); exit(1); }
    XSetErrorHandler(xerrorhandler);
    root = DefaultRootWindow(dpy);
    screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_height = DisplayHeight(dpy, DefaultScreen(dpy));
    animation_set_display(dpy);

    wm_protocols           = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_take_focus          = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    wm_delete_window       = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    net_wm_state           = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_state_fullscreen= XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    net_active_window      = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_window_type     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    net_wm_window_type_dock= XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_wm_window_opacity  = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    net_wm_strut           = XInternAtom(dpy, "_NET_WM_STRUT", False);
    zelpy_workspace_atom   = XInternAtom(dpy, "_ZELPY_CURRENT_WORKSPACE", False);
    zelpy_layout_atom      = XInternAtom(dpy, "_ZELPY_CURRENT_LAYOUT", False);
    zelpy_cmd_workspace    = XInternAtom(dpy, "_ZELPY_CMD_WORKSPACE", False);
    zelpy_cmd_reload       = XInternAtom(dpy, "_ZELPY_CMD_RELOAD", False);
    zelpy_cmd_retile       = XInternAtom(dpy, "_ZELPY_CMD_RETILE", False);
    zelpy_cmd_quit         = XInternAtom(dpy, "_ZELPY_CMD_QUIT", False);
    zelpy_cmd_layout       = XInternAtom(dpy, "_ZELPY_CMD_LAYOUT", False);
    zelpy_cmd_layout_toggle= XInternAtom(dpy, "_ZELPY_CMD_LAYOUT_TOGGLE", False);

    Cursor cur = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, cur); XFreeCursor(dpy, cur);

    XSelectInput(dpy, root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 FocusChangeMask);

    // EWMH: register zelpy as the WM for fastfetch/neofetch detection
    {
        Window wm_check = XCreateSimpleWindow(dpy, root, -100, -100, 1, 1, 0, 0, 0);
        net_supporting_wm_check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
        net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, root, net_supporting_wm_check, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&wm_check, 1);
        XChangeProperty(dpy, wm_check, net_supporting_wm_check, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&wm_check, 1);
        XChangeProperty(dpy, wm_check, net_wm_name, utf8_string, 8,
                        PropModeReplace, (unsigned char *)"zelpy", 5);
        XFlush(dpy);
    }

    load_config();
    apply_config();
    update_struts();
    broadcast_workspace();
    broadcast_layout();
    spawn_startup_commands();

    XGrabButton(dpy, Button1, SUPERKEY, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, SUPERKEY, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    signal(SIGHUP, sighup_handler);
}

void spawn(const char *cmd) {
    if (fork() == 0) { setsid(); execl("/bin/sh", "/bin/sh", "-c", cmd, NULL); exit(0); }
}

// ---------- Main loop ----------

void run(void) {
    XEvent ev;
    struct timeval tv; fd_set fds;
    int xfd = ConnectionNumber(dpy);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long last_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    while (1) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case KeyPress: {
                KeySym ks = XkbKeycodeToKeysym(dpy, ev.xkey.keycode, 0, 0);
                for (int i = 0; i < num_keybinds; i++) {
                    if (keybinds[i].keysym != ks) continue;
                    if (keybinds[i].mod != (ev.xkey.state & ~(LockMask|Mod2Mask))) continue;
                    switch (keybinds[i].action) {
                        case ACTION_SPAWN:
                            spawn(keybinds[i].command[0] ? keybinds[i].command : terminal_cmd);
                            break;
                        case ACTION_CLOSE: {
                            Window w = get_window_under_cursor();
                            if (window_exists(w)) close_window(w);
                            break;
                        }
                        case ACTION_TOGGLE_FLOATING: toggle_floating(); break;
                        case ACTION_TOGGLE_FULLSCREEN: toggle_fullscreen(); break;
                        case ACTION_LAYOUT_TOGGLE: toggle_layout(); break;
                        case ACTION_QUIT: exit(0);
                        case ACTION_WORKSPACE: switch_workspace(keybinds[i].workspace_num); break;
                    }
                    break;
                }
                break;
            }
            case ButtonPress: {
                Window w = get_window_under_cursor();
                if (!window_exists(w) || is_dock_window(w)) break;
                if (ev.xbutton.button == Button1 && (ev.xbutton.state & SUPERKEY)) {
                    if (is_floating(w)) start_drag(w, ev.xbutton.x_root, ev.xbutton.y_root);
                } else if (ev.xbutton.button == Button3 && (ev.xbutton.state & SUPERKEY)) {
                    if (is_floating(w)) start_resize(w, ev.xbutton.x_root, ev.xbutton.y_root);
                }
                break;
            }
            case MotionNotify:
                if (dragging) do_drag(ev.xmotion.x_root, ev.xmotion.y_root);
                else if (resizing) do_resize(ev.xmotion.x_root, ev.xmotion.y_root);
                break;
            case ButtonRelease:
                if (ev.xbutton.button == Button1) stop_drag();
                else if (ev.xbutton.button == Button3) stop_resize();
                break;
            case MapRequest:
                XMapWindow(dpy, ev.xmaprequest.window);
                add_managed_window(ev.xmaprequest.window);
                tile_windows();
                // try_swallow(ev.xmaprequest.window);
                break;
            case ConfigureRequest:
                XConfigureWindow(dpy, ev.xconfigurerequest.window,
                                 ev.xconfigurerequest.value_mask,
                                 &(XWindowChanges){
                                     .x = ev.xconfigurerequest.x,
                                     .y = ev.xconfigurerequest.y,
                                     .width = ev.xconfigurerequest.width,
                                     .height = ev.xconfigurerequest.height,
                                     .border_width = ev.xconfigurerequest.border_width,
                                     .sibling = ev.xconfigurerequest.above,
                                     .stack_mode = ev.xconfigurerequest.detail
                                 });
                raise_docks();
                break;
            case ClientMessage:
                if (ev.xclient.message_type == net_wm_state) {
                    if ((Atom)ev.xclient.data.l[1] == net_wm_state_fullscreen ||
                        (Atom)ev.xclient.data.l[2] == net_wm_state_fullscreen) {
                        Window w = ev.xclient.window;
                        if (window_exists(w) && !is_dock_window(w)) {
                            if (ev.xclient.data.l[0] == 1) enter_fullscreen(w);
                            else exit_fullscreen(w);
                        }
                    }
                }
                else if (ev.xclient.message_type == zelpy_cmd_workspace)
                    switch_workspace((int)ev.xclient.data.l[0]);
                else if (ev.xclient.message_type == zelpy_cmd_reload) {
                    load_config(); apply_config();
                    update_highlight(); broadcast_layout(); tile_windows();
                }
                else if (ev.xclient.message_type == zelpy_cmd_retile)
                    tile_windows();
                else if (ev.xclient.message_type == zelpy_cmd_quit)
                    exit(0);
                else if (ev.xclient.message_type == zelpy_cmd_layout) {
                    int m = (int)ev.xclient.data.l[0];
                    if (m >= 0 && m <= 3) {
                        layout_mode = m;
                        broadcast_layout();
                        tile_windows();
                    }
                }
                else if (ev.xclient.message_type == zelpy_cmd_layout_toggle)
                    toggle_layout();
                break;
            case DestroyNotify:
                if (ev.xdestroywindow.window == focused_window) focused_window = None;
                if (ev.xdestroywindow.window == drag_window) { dragging = 0; drag_window = None; }
                if (ev.xdestroywindow.window == resize_window) { resizing = 0; resize_window = None; }
                if (ev.xdestroywindow.window == fullscreen_window) fullscreen_window = None;
                // unswallow(ev.xdestroywindow.window);
                remove_managed_window(ev.xdestroywindow.window);
                remove_floating(ev.xdestroywindow.window);
                animation_cancel(ev.xdestroywindow.window);
                tile_windows();
                break;
            case UnmapNotify:
                if (ev.xunmap.window == fullscreen_window) fullscreen_window = None;
                tile_windows();
                break;
            }
        }

        update_focus_from_pointer();

        struct timespec nts;
        clock_gettime(CLOCK_MONOTONIC, &nts);
        long now = nts.tv_sec * 1000 + nts.tv_nsec / 1000000;
        long dt = now - last_time;
        last_time = now;
        animation_tick(dt);

        tv.tv_sec = 0; tv.tv_usec = 10000;
        FD_ZERO(&fds); FD_SET(xfd, &fds);
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
}

int main(void) {
    setup();
    run();
    XCloseDisplay(dpy);
    return 0;
}
