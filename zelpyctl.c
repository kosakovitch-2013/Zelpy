#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Atom get_atom(Display *dpy, const char *name) {
    return XInternAtom(dpy, name, False);
}

static void send_command(Display *dpy, Window root, Atom cmd_atom, long arg) {
    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = cmd_atom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = arg;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void print_workspace(Display *dpy, Window root) {
    Atom a = get_atom(dpy, "_ZELPY_CURRENT_WORKSPACE");
    Atom type; int fmt; unsigned long n, b; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &n, &b, &data) == Success && data) {
        if (n >= 1) printf("%d\n", (int)(*(long *)data));
        else printf("0\n");
        XFree(data);
    } else printf("0\n");
}

static void print_layout(Display *dpy, Window root) {
    Atom a = get_atom(dpy, "_ZELPY_CURRENT_LAYOUT");
    Atom type; int fmt; unsigned long n, b; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &n, &b, &data) == Success && data) {
        if (n >= 1) {
            const char *names[] = { "horizontal", "vertical", "dwindle", "master" };
            int m = (int)(*(long *)data);
            printf("%s\n", (m >= 0 && m <= 3) ? names[m] : "unknown");
        } else printf("unknown\n");
        XFree(data);
    } else printf("unknown\n");
}

static void list_windows(Display *dpy, Window root) {
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &ch, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        char *name = NULL;
        XFetchName(dpy, ch[i], &name);
        printf("0x%08lx  %s\n", ch[i], name ? name : "(unnamed)");
        if (name) XFree(name);
    }
    if (ch) XFree(ch);
}

static void usage(void) {
    fprintf(stderr,
        "zelpyctl — control utility for zelpy\n"
        "\n"
        "Usage:\n"
        "  zelpyctl workspace              print current workspace\n"
        "  zelpyctl workspace <N>          switch to workspace N (1-9)\n"
        "  zelpyctl layout                 print current layout\n"
        "  zelpyctl layout horizontal      set layout to horizontal\n"
        "  zelpyctl layout vertical        set layout to vertical\n"
        "  zelpyctl layout dwindle         set layout to dwindle\n"
        "  zelpyctl layout master          set layout to master\n"
        "  zelpyctl layout toggle          cycle layout\n"
        "  zelpyctl reload                 reload zelpy config\n"
        "  zelpyctl retile                 force retile\n"
        "  zelpyctl quit                   quit zelpy\n"
        "  zelpyctl list                   list top-level windows\n");
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "zelpyctl: cannot open display\n"); return 1; }
    Window root = DefaultRootWindow(dpy);

    if (argc < 2) { usage(); XCloseDisplay(dpy); return 1; }

    if (!strcmp(argv[1], "workspace")) {
        if (argc >= 3) {
            int ws = atoi(argv[2]);
            if (ws < 1 || ws > 9) { fprintf(stderr, "workspace 1-9\n"); return 1; }
            send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_WORKSPACE"), ws);
        } else print_workspace(dpy, root);
    }
    else if (!strcmp(argv[1], "layout")) {
        if (argc >= 3) {
            long mode = -1;
            if      (!strcmp(argv[2], "horizontal")) mode = 0;
            else if (!strcmp(argv[2], "vertical"))   mode = 1;
            else if (!strcmp(argv[2], "dwindle"))    mode = 2;
            else if (!strcmp(argv[2], "master"))     mode = 3;
            else if (!strcmp(argv[2], "toggle"))
                send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_LAYOUT_TOGGLE"), 0);
            if (mode >= 0)
                send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_LAYOUT"), mode);
            if (mode < 0 && strcmp(argv[2], "toggle") != 0) {
                fprintf(stderr, "layout: horizontal|vertical|dwindle|master|toggle\n");
                return 1;
            }
        } else print_layout(dpy, root);
    }
    else if (!strcmp(argv[1], "reload"))
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_RELOAD"), 0);
    else if (!strcmp(argv[1], "retile"))
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_RETILE"), 0);
    else if (!strcmp(argv[1], "quit"))
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_QUIT"), 0);
    else if (!strcmp(argv[1], "list"))
        list_windows(dpy, root);
    else { fprintf(stderr, "unknown: %s\n", argv[1]); usage(); XCloseDisplay(dpy); return 1; }

    XCloseDisplay(dpy);
    return 0;
}
