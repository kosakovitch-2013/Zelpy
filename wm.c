#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
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

// Config globals
static char focus_color[64] = "#ebbcba";
static char unfocus_color[64] = "#444444";
static char terminal_cmd[256] = "st";
static int border_width = BORDER_WIDTH_DEFAULT;

// Startup commands
static char startup_commands[MAX_STARTUP_CMDS][256];
static int num_startup_commands = 0;

// Variables
static char var_names[MAX_VARS][64];
static char var_values[MAX_VARS][256];
static int num_vars = 0;

// Opacity rules
typedef struct {
    char pattern[128];
    double opacity;
} OpacityRule;
static OpacityRule opacity_rules[MAX_OPACITY_RULES];
static int num_opacity_rules = 0;

// Keybind structure
enum { ACTION_NONE, ACTION_SPAWN, ACTION_CLOSE, ACTION_TOGGLE_FLOATING, ACTION_TOGGLE_FULLSCREEN, ACTION_QUIT, ACTION_WORKSPACE };
typedef struct {
    unsigned int mod;
    KeySym keysym;
    int action;
    char command[256];
    int workspace_num;
} Keybind;

static Keybind keybinds[MAX_KEYBINDS];
static int num_keybinds = 0;

static Display *dpy;
static Window root;
static int screen_width, screen_height;
static Window focused_window = None;
static Window fullscreen_window = None;
static int dragging = 0;
static int drag_start_x, drag_start_y;
static Window drag_window;

static int resizing = 0;
static int resize_direction = 0;
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

static Atom wm_protocols = None;
static Atom wm_take_focus = None;
static Atom wm_delete_window = None;
static Atom net_wm_state = None;
static Atom net_wm_state_fullscreen = None;
static Atom net_active_window = None;
static Atom net_wm_window_type = None;
static Atom net_wm_window_type_dock = None;
static Atom net_wm_window_opacity = None;

// Forward declarations
void tile_windows(void);
int window_exists(Window w);
void update_focus_from_pointer(void);
void raise_floating_windows(void);
int is_floating(Window w);
void add_managed_window(Window w);
void remove_managed_window(Window w);
void spawn_startup_commands(void);
void switch_workspace(int ws);
const char *get_var_value(const char *name);
void expand_variables(const char *input, char *output, size_t out_size);
void apply_opacity_rules(Window w);

int xerrorhandler(Display *disp, XErrorEvent *ev) { (void)disp; (void)ev; return 0; }

void set_border_color(Window w, const char *color) {
    if (!window_exists(w)) return;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor xcolor;
    XParseColor(dpy, cmap, color, &xcolor);
    XAllocColor(dpy, cmap, &xcolor);
    XSetWindowBorder(dpy, w, xcolor.pixel);
    XSetWindowBorderWidth(dpy, w, border_width);
}

void set_border_width(Window w, int width) {
    if (!window_exists(w)) return;
    XSetWindowBorderWidth(dpy, w, width);
}

Window get_toplevel(Window w) {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    Window top = w;
    while (1) {
        if (!XQueryTree(dpy, top, &root_ret, &parent_ret, &children, &nchildren))
            return w;
        if (children) XFree(children);
        if (parent_ret == root || parent_ret == None)
            return top;
        top = parent_ret;
        if (top == root || top == None)
            return w;
    }
}

int window_exists(Window w) {
    if (w == None) return 0;
    XWindowAttributes attr;
    XErrorHandler old = XSetErrorHandler(xerrorhandler);
    int r = XGetWindowAttributes(dpy, w, &attr);
    XSetErrorHandler(old);
    return r == 1;
}

int is_managed_window(Window w) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return 0;
    if (attr.override_redirect) return 0;
    if (attr.map_state != IsViewable) return 0;

    // Check for dock window type
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, net_wm_window_type, 0, 1, False,
                           XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) == Success && data) {
        Atom type = *(Atom *)data;
        XFree(data);
        if (type == net_wm_window_type_dock)
            return 0;
    }

    Window top = get_toplevel(w);
    if (top != w && top != root && top != None) return 0;
    return 1;
}

void add_managed_window(Window w) {
    if (!is_managed_window(w)) return;
    for (int i = 0; i < num_managed; i++) {
        if (managed_windows[i] == w) return;
    }
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
            break;
        }
    }
}

Window get_window_under_cursor(void) {
    Window root_ret, child_ret;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (XQueryPointer(dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) {
        if (child_ret != None)
            return get_toplevel(child_ret);
    }
    return None;
}

void update_highlight(void) {
    Window cursor_win = get_window_under_cursor();
    if (cursor_win != None && cursor_win != root && window_exists(cursor_win)) {
        focused_window = cursor_win;
    }
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren))
        return;
    for (unsigned int i = 0; i < nchildren; i++) {
        if (!window_exists(children[i])) continue;
        XWindowAttributes attr;
        if (XGetWindowAttributes(dpy, children[i], &attr)) {
            if (attr.map_state == IsViewable && !attr.override_redirect) {
                Window top = get_toplevel(children[i]);
                if (top != None && window_exists(top)) {
                    if (top == focused_window)
                        set_border_color(top, focus_color);
                    else
                        set_border_color(top, unfocus_color);
                }
            }
        }
    }
    if (children) XFree(children);
}

void send_wm_take_focus(Window w) {
    Atom *protocols;
    int num;
    if (XGetWMProtocols(dpy, w, &protocols, &num)) {
        for (int i = 0; i < num; i++) {
            if (protocols[i] == wm_take_focus) {
                XEvent ev;
                memset(&ev, 0, sizeof(ev));
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
}

void set_focus_to_window(Window w) {
    if (w == None || w == root || !window_exists(w)) return;
    send_wm_take_focus(w);
    XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
    XRaiseWindow(dpy, w);
    XFlush(dpy);

    if (is_floating(w)) {
        for (int i = 0; i < num_floating_stack; i++) {
            if (floating_stack[i] == w) {
                for (int j = i; j < num_floating_stack - 1; j++)
                    floating_stack[j] = floating_stack[j+1];
                floating_stack[num_floating_stack - 1] = w;
                break;
            }
        }
    } else {
        raise_floating_windows();
    }

    focused_window = w;
    update_highlight();
}

void update_focus_from_pointer(void) {
    Window win = get_window_under_cursor();
    if (win != None && win != root && window_exists(win)) {
        if (win != focused_window) {
            set_focus_to_window(win);
        }
    }
}

int is_floating(Window w) {
    for (int i = 0; i < num_floating; i++)
        if (floating_windows[i] == w) return 1;
    return 0;
}

void add_floating(Window w) {
    if (!is_floating(w) && num_floating < 128) {
        floating_windows[num_floating++] = w;
        if (num_floating_stack < 128) {
            floating_stack[num_floating_stack++] = w;
        }
    }
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
    for (int i = 0; i < num_floating_stack; i++) {
        if (window_exists(floating_stack[i])) {
            XRaiseWindow(dpy, floating_stack[i]);
        }
    }
    XFlush(dpy);
}

void enter_fullscreen(Window w) {
    if (w == None || w == root || !window_exists(w)) return;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return;

    fullscreen_orig_x = attr.x;
    fullscreen_orig_y = attr.y;
    fullscreen_orig_w = attr.width;
    fullscreen_orig_h = attr.height;
    fullscreen_orig_was_floating = is_floating(w);

    fullscreen_window = w;

    animation_start(w, 0, 0, screen_width, screen_height, ANIMATION_DURATION);
    set_border_width(w, 0);
    XRaiseWindow(dpy, w);
}

void exit_fullscreen(Window w) {
    if (w == None || w == root || !window_exists(w)) return;
    fullscreen_window = None;

    set_border_width(w, border_width);

    if (fullscreen_orig_was_floating) {
        animation_start(w, fullscreen_orig_x, fullscreen_orig_y, fullscreen_orig_w, fullscreen_orig_h, ANIMATION_DURATION);
        add_floating(w);
        raise_floating_windows();
    } else {
        tile_windows();
    }
    update_highlight();
}

void toggle_fullscreen(void) {
    Window w = get_window_under_cursor();
    if (w == None || w == root || !window_exists(w)) return;
    if (fullscreen_window == w) {
        exit_fullscreen(w);
    } else {
        enter_fullscreen(w);
    }
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (fullscreen_window == w) ? 1 : 0;
    ev.xclient.data.l[1] = net_wm_state_fullscreen;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void close_window(Window w) {
    w = get_toplevel(w);
    if (w == None || w == root || !window_exists(w)) return;
    Atom *protocols;
    int num;
    int has_delete = 0;
    if (XGetWMProtocols(dpy, w, &protocols, &num)) {
        for (int i = 0; i < num; i++) {
            if (protocols[i] == wm_delete_window) {
                has_delete = 1;
                break;
            }
        }
        XFree(protocols);
    }
    if (has_delete) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
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
    if (w == None || w == root || !window_exists(w)) return;
    if (is_floating(w)) {
        remove_floating(w);
        tile_windows();
        update_highlight();
    } else {
        add_floating(w);
        int fw = screen_width / 3;
        int fh = screen_height / 3;
        int fx = (screen_width - fw) / 2;
        int fy = (screen_height - fh) / 2;
        animation_start(w, fx, fy, fw, fh, ANIMATION_DURATION);
        set_border_color(w, focus_color);
        set_border_width(w, border_width);
        fullscreen_window = None;
        raise_floating_windows();
    }
}

void start_drag(Window w, int x, int y) {
    w = get_toplevel(w);
    if (w == None || w == root || !window_exists(w)) return;
    animation_cancel(w);
    dragging = 1;
    drag_window = w;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) {
        dragging = 0;
        return;
    }
    drag_start_x = attr.x - x;
    drag_start_y = attr.y - y;
    XRaiseWindow(dpy, w);
    if (is_floating(w)) {
        for (int i = 0; i < num_floating_stack; i++) {
            if (floating_stack[i] == w) {
                for (int j = i; j < num_floating_stack - 1; j++)
                    floating_stack[j] = floating_stack[j+1];
                floating_stack[num_floating_stack - 1] = w;
                break;
            }
        }
        raise_floating_windows();
    }
}

void do_drag(int x, int y) {
    if (!dragging || !window_exists(drag_window)) {
        dragging = 0;
        drag_window = None;
        return;
    }
    XMoveWindow(dpy, drag_window, drag_start_x + x, drag_start_y + y);
}

void stop_drag(void) {
    dragging = 0;
    drag_window = None;
}

int get_resize_direction(Window w, int root_x, int root_y) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return 0;
    int win_x = attr.x;
    int win_y = attr.y;
    int win_w = attr.width;
    int win_h = attr.height;
    int threshold = RESIZE_THRESHOLD;

    int near_left  = (root_x - win_x <= threshold) && (root_x >= win_x);
    int near_right = ((win_x + win_w) - root_x <= threshold) && (root_x <= (win_x + win_w));
    int near_top   = (root_y - win_y <= threshold) && (root_y >= win_y);
    int near_bottom= ((win_y + win_h) - root_y <= threshold) && (root_y <= (win_y + win_h));

    if (near_left && near_top) return 1;
    if (near_right && near_top) return 3;
    if (near_left && near_bottom) return 6;
    if (near_right && near_bottom) return 8;
    if (near_left) return 4;
    if (near_right) return 5;
    if (near_top) return 2;
    if (near_bottom) return 7;
    return 0;
}

void start_resize(Window w, int root_x, int root_y) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return;
    animation_cancel(w);
    resizing = 1;
    resize_window = w;
    resize_direction = get_resize_direction(w, root_x, root_y);
    resize_start_x = root_x;
    resize_start_y = root_y;
    resize_start_xpos = attr.x;
    resize_start_ypos = attr.y;
    resize_start_w = attr.width;
    resize_start_h = attr.height;
    XRaiseWindow(dpy, w);
    if (is_floating(w)) {
        for (int i = 0; i < num_floating_stack; i++) {
            if (floating_stack[i] == w) {
                for (int j = i; j < num_floating_stack - 1; j++)
                    floating_stack[j] = floating_stack[j+1];
                floating_stack[num_floating_stack - 1] = w;
                break;
            }
        }
        raise_floating_windows();
    }
}

void do_resize(int root_x, int root_y) {
    if (!resizing || resize_window == None) return;
    int dx = root_x - resize_start_x;
    int dy = root_y - resize_start_y;
    int new_x = resize_start_xpos;
    int new_y = resize_start_ypos;
    int new_w = resize_start_w;
    int new_h = resize_start_h;

    switch (resize_direction) {
        case 1: new_x += dx; new_y += dy; new_w -= dx; new_h -= dy; break;
        case 2: new_y += dy; new_h -= dy; break;
        case 3: new_y += dy; new_w += dx; new_h -= dy; break;
        case 4: new_x += dx; new_w -= dx; break;
        case 5: new_w += dx; break;
        case 6: new_x += dx; new_w -= dx; new_h += dy; break;
        case 7: new_h += dy; break;
        case 8: new_w += dx; new_h += dy; break;
    }

    if (new_w < MIN_WIDTH) new_w = MIN_WIDTH;
    if (new_h < MIN_HEIGHT) new_h = MIN_HEIGHT;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x + new_w > screen_width) new_w = screen_width - new_x;
    if (new_y + new_h > screen_height) new_h = screen_height - new_y;

    if (window_exists(resize_window)) {
        XMoveResizeWindow(dpy, resize_window, new_x, new_y, new_w, new_h);
        XFlush(dpy);
    }
}

void stop_resize(void) {
    resizing = 0;
    resize_window = None;
    resize_direction = 0;
}

// Config variables

const char *get_var_value(const char *name) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(var_names[i], name) == 0)
            return var_values[i];
    }
    return NULL;
}

void expand_variables(const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) return;
    size_t in_len = strlen(input);
    size_t out_pos = 0;
    for (size_t i = 0; i < in_len && out_pos < out_size - 1; i++) {
        if (input[i] == '@') {
            size_t j = i + 1;
            while (j < in_len && (isalnum(input[j]) || input[j] == '_'))
                j++;
            if (j > i + 1) {
                char name[64];
                size_t len = j - (i + 1);
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                strncpy(name, input + i + 1, len);
                name[len] = '\0';
                const char *val = get_var_value(name);
                if (val) {
                    size_t val_len = strlen(val);
                    if (out_pos + val_len < out_size - 1) {
                        strcpy(output + out_pos, val);
                        out_pos += val_len;
                    }
                    i = j - 1;
                    continue;
                }
            }
        }
        output[out_pos++] = input[i];
    }
    output[out_pos] = '\0';
}

unsigned int parse_mod(const char *modstr) {
    unsigned int mod = 0;
    if (strstr(modstr, "Mod1")) mod |= Mod1Mask;
    if (strstr(modstr, "Mod4")) mod |= Mod4Mask;
    if (strstr(modstr, "Shift")) mod |= ShiftMask;
    if (strstr(modstr, "Ctrl")) mod |= ControlMask;
    return mod;
}

int parse_action(const char *act) {
    if (strcmp(act, "spawn") == 0) return ACTION_SPAWN;
    if (strcmp(act, "close") == 0) return ACTION_CLOSE;
    if (strcmp(act, "toggle_floating") == 0) return ACTION_TOGGLE_FLOATING;
    if (strcmp(act, "toggle_fullscreen") == 0) return ACTION_TOGGLE_FULLSCREEN;
    if (strcmp(act, "quit") == 0) return ACTION_QUIT;
    if (strcmp(act, "workspace") == 0) return ACTION_WORKSPACE;
    return ACTION_NONE;
}

void apply_opacity_rules(Window w) {
    if (!window_exists(w)) return;

    // Get window title
    char *title = NULL;
    XFetchName(dpy, w, &title);

    // Get window class (optional)
    XClassHint class_hint;
    int has_class = XGetClassHint(dpy, w, &class_hint);
    char *class_name = has_class ? class_hint.res_name : NULL;
    char *class_class = has_class ? class_hint.res_class : NULL;

    for (int i = 0; i < num_opacity_rules; i++) {
        int match = 0;
        if (title && strcasestr(title, opacity_rules[i].pattern))
            match = 1;
        if (!match && class_name && strcasestr(class_name, opacity_rules[i].pattern))
            match = 1;
        if (!match && class_class && strcasestr(class_class, opacity_rules[i].pattern))
            match = 1;

        if (match) {
            unsigned long opacity_val = (unsigned long)(opacity_rules[i].opacity * 0xffffffffUL);
            XChangeProperty(dpy, w, net_wm_window_opacity, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char *)&opacity_val, 1);
            XFlush(dpy);
            break;
        }
    }

    if (title) XFree(title);
    if (has_class) {
        if (class_hint.res_name) XFree(class_hint.res_name);
        if (class_hint.res_class) XFree(class_hint.res_class);
    }
}

void parse_config_line(char *line) {
    line[strcspn(line, "\n")] = 0;
    if (line[0] == '#' || line[0] == '\0') return;

    char *first_space = strchr(line, ' ');
    if (!first_space) return;
    *first_space = '\0';
    char *name = line;
    char *value = first_space + 1;
    while (*value == ' ') value++;

    if (strcmp(name, "border_width") == 0) {
        border_width = atoi(value);
    } else if (strcmp(name, "focus_color") == 0) {
        strncpy(focus_color, value, 63);
    } else if (strcmp(name, "unfocus_color") == 0) {
        strncpy(unfocus_color, value, 63);
    } else if (strcmp(name, "terminal") == 0) {
        if (num_vars < MAX_VARS) {
            strncpy(var_names[num_vars], "terminal", 63);
            strncpy(var_values[num_vars], value, 255);
            num_vars++;
        }
        strncpy(terminal_cmd, value, 255);
    } else if (strcmp(name, "execute") == 0) {
        size_t len = strlen(value);
        if (len >= 2 && value[0] == '"' && value[len-1] == '"') {
            value[len-1] = '\0';
            value++;
        }
        if (num_startup_commands < MAX_STARTUP_CMDS) {
            strncpy(startup_commands[num_startup_commands], value, 255);
            num_startup_commands++;
        }
    } else if (strcmp(name, "opacity") == 0) {
        // Format: opacity "pattern" 0.8
        char *pattern_start = strchr(value, '"');
        if (!pattern_start) return;
        pattern_start++;
        char *pattern_end = strchr(pattern_start, '"');
        if (!pattern_end) return;
        *pattern_end = '\0';
        char *opacity_str = pattern_end + 1;
        while (*opacity_str == ' ') opacity_str++;
        double opacity = atof(opacity_str);
        if (opacity > 1.0) opacity = 1.0;
        if (opacity < 0.0) opacity = 0.0;
        if (num_opacity_rules < MAX_OPACITY_RULES) {
            strncpy(opacity_rules[num_opacity_rules].pattern, pattern_start, 127);
            opacity_rules[num_opacity_rules].pattern[127] = '\0';
            opacity_rules[num_opacity_rules].opacity = opacity;
            num_opacity_rules++;
        }
    } else if (strcmp(name, "keybind") == 0) {
        char *combo = strtok(value, " ");
        if (!combo) return;
        char *action_str = strtok(NULL, " ");
        if (!action_str) return;
        char *args = strtok(NULL, "");

        char *dash = strchr(combo, '-');
        if (!dash) return;
        *dash = '\0';
        char *modstr = combo;
        char *keystr = dash + 1;

        unsigned int mod = parse_mod(modstr);
        if (mod == 0) return;

        char key_lower[64];
        strncpy(key_lower, keystr, 63);
        key_lower[63] = '\0';
        for (int i = 0; key_lower[i]; i++) key_lower[i] = tolower(key_lower[i]);

        KeySym keysym = XStringToKeysym(key_lower);
        if (keysym == NoSymbol) {
            keysym = XStringToKeysym(keystr);
        }
        if (keysym == NoSymbol) {
            fprintf(stderr, "Zelpy: Unknown key '%s'\n", keystr);
            return;
        }

        int action = parse_action(action_str);
        if (action == ACTION_NONE) {
            fprintf(stderr, "Zelpy: Unknown action '%s'\n", action_str);
            return;
        }

        if (num_keybinds < MAX_KEYBINDS) {
            keybinds[num_keybinds].mod = mod;
            keybinds[num_keybinds].keysym = keysym;
            keybinds[num_keybinds].action = action;
            keybinds[num_keybinds].workspace_num = 0;
            keybinds[num_keybinds].command[0] = '\0';

            if (action == ACTION_SPAWN) {
                if (args) {
                    char expanded[256];
                    expand_variables(args, expanded, sizeof(expanded));
                    strncpy(keybinds[num_keybinds].command, expanded, 255);
                }
            } else if (action == ACTION_WORKSPACE) {
                if (args) {
                    int ws = atoi(args);
                    if (ws >= 1 && ws <= MAX_WORKSPACES) {
                        keybinds[num_keybinds].workspace_num = ws;
                    }
                }
            }
            num_keybinds++;
        }
    } else {
        // Variable definition
        if (num_vars < MAX_VARS) {
            strncpy(var_names[num_vars], name, 63);
            strncpy(var_values[num_vars], value, 255);
            num_vars++;
        }
    }
}

void load_config(void) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.config/zelpy/config", home);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/zelpy", home);
    mkdir(dir, 0755);

    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (!f) {
            fprintf(stderr, "Zelpy: Cannot create config file %s\n", path);
            return;
        }
        fprintf(f,
            "# Zelpy default config\n"
            "border_width 2\n"
            "focus_color #ebbcba\n"
            "unfocus_color #444444\n"
            "terminal st\n"
            "\n"
            "execute \"feh --bg-fill ~/wallpaper.jpg\"\n"
            "execute \"(sleep 2 && xcompmgr -c -s) &\"\n"
            "\n"
            "keybind Mod1-Return spawn @terminal\n"
            "keybind Mod1-t close\n"
            "keybind Mod1-v toggle_floating\n"
            "keybind Mod1-f toggle_fullscreen\n"
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

    num_keybinds = 0;
    num_startup_commands = 0;
    num_vars = 0;
    num_opacity_rules = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        parse_config_line(line);
    }
    fclose(f);

    fprintf(stderr, "Zelpy: Loaded %d keybinds, %d startup commands, %d vars, %d opacity rules\n",
            num_keybinds, num_startup_commands, num_vars, num_opacity_rules);
}

void spawn_startup_commands(void) {
    for (int i = 0; i < num_startup_commands; i++) {
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "/bin/sh", "-c", startup_commands[i], NULL);
            exit(0);
        }
    }
}

void grab_keys(void) {
    for (int i = 0; i < num_keybinds; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keybinds[i].keysym);
        if (code) {
            XGrabKey(dpy, code, keybinds[i].mod, root, True, GrabModeAsync, GrabModeAsync);
        } else {
            fprintf(stderr, "Zelpy: No keycode for keysym %lu\n", keybinds[i].keysym);
        }
    }
}

void apply_config(void) {
    grab_keys();
    // Apply opacity rules to all existing managed windows
    for (int i = 0; i < num_managed; i++) {
        if (window_exists(managed_windows[i]))
            apply_opacity_rules(managed_windows[i]);
    }
}

void sighup_handler(int sig) {
    (void)sig;
    load_config();
    apply_config();
    update_highlight();
}

// Workspace handling

void switch_workspace(int ws) {
    if (ws < 1 || ws > MAX_WORKSPACES) return;
    if (ws == current_workspace) return;
    current_workspace = ws;

    for (int i = 0; i < num_managed; i++) {
        Window w = managed_windows[i];
        if (!window_exists(w)) continue;
        if (managed_workspace[i] != current_workspace) {
            XUnmapWindow(dpy, w);
        } else {
            XMapWindow(dpy, w);
        }
    }
    XFlush(dpy);
    tile_windows();
    update_focus_from_pointer();
    raise_floating_windows();
}

// Setup and run

void setup(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }
    XSetErrorHandler(xerrorhandler);
    root = DefaultRootWindow(dpy);
    screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_height = DisplayHeight(dpy, DefaultScreen(dpy));

    animation_set_display(dpy);

    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_wm_window_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);

    Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, cursor);
    XFreeCursor(dpy, cursor);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 FocusChangeMask);

    load_config();
    apply_config();
    spawn_startup_commands();

    XGrabButton(dpy, Button1, SUPERKEY, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, SUPERKEY, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    signal(SIGHUP, sighup_handler);
}

void tile_windows(void) {
    Window visible[128];
    int count = 0;

    for (int i = 0; i < num_managed; i++) {
        Window w = managed_windows[i];
        if (!window_exists(w)) continue;
        if (managed_workspace[i] != current_workspace) continue;
        if (w == fullscreen_window) continue;
        if (is_floating(w)) continue;
        XWindowAttributes attr;
        if (XGetWindowAttributes(dpy, w, &attr)) {
            if (attr.map_state == IsViewable && !attr.override_redirect) {
                visible[count++] = w;
            }
        }
    }

    if (count > 0) {
        int width = screen_width / count;
        for (int i = 0; i < count; i++) {
            int target_x = i * width;
            int target_y = 0;
            int target_w = width - 2*border_width;
            int target_h = screen_height - 2*border_width;
            animation_start(visible[i], target_x, target_y, target_w, target_h, ANIMATION_DURATION);
            set_border_width(visible[i], border_width);
        }
    }
    raise_floating_windows();
}

void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        exit(0);
    }
}

void run(void) {
    XEvent ev;
    struct timeval tv;
    fd_set fds;
    int xfd = ConnectionNumber(dpy);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long last_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    while (1) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case KeyPress: {
                KeySym keysym = XKeycodeToKeysym(dpy, ev.xkey.keycode, 0);
                for (int i = 0; i < num_keybinds; i++) {
                    if (keybinds[i].keysym == keysym && keybinds[i].mod == (ev.xkey.state & ~(LockMask|Mod2Mask))) {
                        switch (keybinds[i].action) {
                            case ACTION_SPAWN:
                                spawn(keybinds[i].command[0] ? keybinds[i].command : terminal_cmd);
                                break;
                            case ACTION_CLOSE: {
                                Window w = get_window_under_cursor();
                                if (w != None && w != root && window_exists(w))
                                    close_window(w);
                                break;
                            }
                            case ACTION_TOGGLE_FLOATING:
                                toggle_floating();
                                break;
                            case ACTION_TOGGLE_FULLSCREEN:
                                toggle_fullscreen();
                                break;
                            case ACTION_QUIT:
                                exit(0);
                                break;
                            case ACTION_WORKSPACE:
                                switch_workspace(keybinds[i].workspace_num);
                                break;
                        }
                        break;
                    }
                }
                break;
            }
            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    Window w = get_window_under_cursor();
                    if (w != None && w != root && window_exists(w)) {
                        if (ev.xbutton.state & SUPERKEY) {
                            if (is_floating(w)) {
                                start_drag(w, ev.xbutton.x_root, ev.xbutton.y_root);
                            }
                        }
                    }
                } else if (ev.xbutton.button == Button3) {
                    Window w = get_window_under_cursor();
                    if (w != None && w != root && (ev.xbutton.state & SUPERKEY)) {
                        if (is_floating(w)) {
                            start_resize(w, ev.xbutton.x_root, ev.xbutton.y_root);
                        }
                    }
                }
                break;
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
                raise_floating_windows();
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
                break;
            case ClientMessage:
                if (ev.xclient.message_type == net_wm_state) {
                    if ((Atom)ev.xclient.data.l[1] == net_wm_state_fullscreen ||
                        (Atom)ev.xclient.data.l[2] == net_wm_state_fullscreen) {
                        Window w = ev.xclient.window;
                        if (w != None && window_exists(w)) {
                            if (ev.xclient.data.l[0] == 1) {
                                enter_fullscreen(w);
                            } else {
                                exit_fullscreen(w);
                            }
                        }
                    }
                }
                break;
            case DestroyNotify:
                if (ev.xdestroywindow.window == focused_window) focused_window = None;
                if (ev.xdestroywindow.window == drag_window) { dragging = 0; drag_window = None; }
                if (ev.xdestroywindow.window == resize_window) { resizing = 0; resize_window = None; }
                if (ev.xdestroywindow.window == fullscreen_window) fullscreen_window = None;
                remove_managed_window(ev.xdestroywindow.window);
                remove_floating(ev.xdestroywindow.window);
                animation_cancel(ev.xdestroywindow.window);
                break;
            case UnmapNotify:
                if (ev.xunmap.window == fullscreen_window) fullscreen_window = None;
                // Do NOT remove managed/floating state here, because UnmapNotify
                // is also sent when we hide windows during workspace switching.
                tile_windows();
                break;
            }
        }
        update_focus_from_pointer();

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long now = now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
        long dt = now - last_time;
        last_time = now;
        animation_tick(dt);

        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
}

int main(void) {
    setup();
    run();
    XCloseDisplay(dpy);
    return 0;
}
