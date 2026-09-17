#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Atom get_atom(Display *dpy, const char *name) {
    return XInternAtom(dpy, name, False);
}

static void send_command(Display *dpy, Window root, Atom cmd_atom, long arg) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = cmd_atom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = arg;
    ev.xclient.data.l[1] = 0;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &ev);
    XFlush(dpy);
}

static void print_workspace(Display *dpy, Window root) {
    Atom ws_atom = get_atom(dpy, "_ZELPY_CURRENT_WORKSPACE");
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, ws_atom, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) == Success && data) {
        if (nitems >= 1) printf("%d\n", (int)(*(long *)data));
        else printf("0\n");
        XFree(data);
    } else {
        printf("0\n");
    }
}

static void list_windows(Display *dpy, Window root) {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren))
        return;
    for (unsigned int i = 0; i < nchildren; i++) {
        char *name = NULL;
        XFetchName(dpy, children[i], &name);
        printf("0x%08lx  %s\n", children[i], name ? name : "(unnamed)");
        if (name) XFree(name);
    }
    if (children) XFree(children);
}

static void usage(void) {
    fprintf(stderr,
        "zelpyctl — control utility for zelpy\n"
        "\n"
        "Usage:\n"
        "  zelpyctl workspace            print current workspace\n"
        "  zelpyctl workspace <N>        switch to workspace N (1-9)\n"
        "  zelpyctl reload               reload zelpy config\n"
        "  zelpyctl retile               force retile\n"
        "  zelpyctl quit                 quit zelpy\n"
        "  zelpyctl list                 list top-level windows\n");
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "zelpyctl: cannot open display\n");
        return 1;
    }
    Window root = DefaultRootWindow(dpy);

    if (argc < 2) {
        usage();
        XCloseDisplay(dpy);
        return 1;
    }

    if (strcmp(argv[1], "workspace") == 0) {
        if (argc >= 3) {
            int ws = atoi(argv[2]);
            if (ws < 1 || ws > 9) {
                fprintf(stderr, "zelpyctl: workspace must be 1-9\n");
                XCloseDisplay(dpy);
                return 1;
            }
            send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_WORKSPACE"), ws);
        } else {
            print_workspace(dpy, root);
        }
    }
    else if (strcmp(argv[1], "reload") == 0) {
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_RELOAD"), 0);
    }
    else if (strcmp(argv[1], "retile") == 0) {
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_RETILE"), 0);
    }
    else if (strcmp(argv[1], "quit") == 0) {
        send_command(dpy, root, get_atom(dpy, "_ZELPY_CMD_QUIT"), 0);
    }
    else if (strcmp(argv[1], "list") == 0) {
        list_windows(dpy, root);
    }
    else {
        fprintf(stderr, "zelpyctl: unknown command '%s'\n", argv[1]);
        usage();
        XCloseDisplay(dpy);
        return 1;
    }

    XCloseDisplay(dpy);
    return 0;
}
