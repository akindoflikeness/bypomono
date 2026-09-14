/* X11 backend: a child window of the host's, an XImage over the editor's
   output buffer, and events read off the display's fd. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>

#include "plug_gui_backend.h"

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XImage *img;
} X11Back;

static X11Back *back_of(Gui *g) { return gui_surface(g)->back; }

static void drop_image(X11Back *b) {
    if (!b->img) return;
    b->img->data = NULL; /* the buffer belongs to plug_gui.c */
    XDestroyImage(b->img);
    b->img = NULL;
}

bool backend_open(Gui *g) {
    X11Back *b = calloc(1, sizeof *b);
    if (!b) return false;
    b->dpy = XOpenDisplay(NULL);
    if (!b->dpy) {
        free(b);
        return false;
    }
    gui_surface(g)->back = b;
    return true;
}

void backend_close(Gui *g) {
    X11Back *b = back_of(g);
    if (!b) return;
    if (b->dpy) {
        drop_image(b);
        if (b->gc) XFreeGC(b->dpy, b->gc);
        if (b->win) XDestroyWindow(b->dpy, b->win);
        XCloseDisplay(b->dpy);
    }
    free(b);
    gui_surface(g)->back = NULL;
}

/* Usable desktop of the default screen, which spans every head, not one. */
void backend_usable_screen(Gui *g, int *w, int *h) {
    Display *dpy = back_of(g)->dpy;
    int scr = DefaultScreen(dpy);
    *w = DisplayWidth(dpy, scr);
    *h = DisplayHeight(dpy, scr);

    Atom prop = XInternAtom(dpy, "_NET_WORKAREA", True);
    if (prop == None) return;
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, RootWindow(dpy, scr), prop, 0, 4, False,
                           XA_CARDINAL, &type, &fmt, &n, &after, &data)
        != Success)
        return;
    if (data && type == XA_CARDINAL && fmt == 32 && n >= 4) {
        const long *a = (const long *)(const void *)data;
        if (a[2] > 0 && a[2] < *w) *w = (int)a[2];
        if (a[3] > 0 && a[3] < *h) *h = (int)a[3];
    }
    if (data) XFree(data);
}

float backend_px_per_point(Gui *g) { return 1.0f; }

bool backend_attach(Gui *g, const clap_window_t *window) {
    X11Back *b = back_of(g);
    if (!b || !b->dpy || b->win) return false;
    int screen = DefaultScreen(b->dpy);
    b->win = XCreateSimpleWindow(b->dpy, (Window)window->x11, 0, 0, 1, 1, 0,
                                 BlackPixel(b->dpy, screen),
                                 BlackPixel(b->dpy, screen));
    if (!b->win) return false;
    XSelectInput(b->dpy, b->win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask
                     | PointerMotionMask | KeyPressMask | KeyReleaseMask
                     | EnterWindowMask | LeaveWindowMask);
    b->gc = XCreateGC(b->dpy, b->win, 0, NULL);
    return true;
}

bool backend_resize(Gui *g) {
    X11Back *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->win || !s->out_px) return false;
    int screen = DefaultScreen(b->dpy);
    drop_image(b);
    b->img = XCreateImage(b->dpy, DefaultVisual(b->dpy, screen),
                          (unsigned)DefaultDepth(b->dpy, screen), ZPixmap, 0,
                          (char *)s->out_px, (unsigned)s->win_w,
                          (unsigned)s->win_h, 32, 0);
    if (!b->img) return false;
    XResizeWindow(b->dpy, b->win, (unsigned)s->win_w, (unsigned)s->win_h);
    XFlush(b->dpy);
    return true;
}

void backend_present(Gui *g) {
    X11Back *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->win || !b->img) return;
    XPutImage(b->dpy, b->win, b->gc, b->img, 0, 0, 0, 0, (unsigned)s->win_w,
              (unsigned)s->win_h);
    XFlush(b->dpy);
}

void backend_show(Gui *g) {
    X11Back *b = back_of(g);
    if (!b || !b->win) return;
    XMapWindow(b->dpy, b->win);
    XFlush(b->dpy);
}

void backend_hide(Gui *g) {
    X11Back *b = back_of(g);
    if (!b || !b->win) return;
    XUnmapWindow(b->dpy, b->win);
    XFlush(b->dpy);
}

int backend_event_fd(Gui *g) {
    X11Back *b = back_of(g);
    return b && b->dpy ? ConnectionNumber(b->dpy) : -1;
}

static void x_key(Gui *g, XKeyEvent *ev, bool down) {
    char buf[8] = {0};
    KeySym ks = 0;
    int n = XLookupString(ev, buf, sizeof buf - 1, &ks, NULL);
    int sc = -1;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: sc = KEY_RETURN; break;
    case XK_Escape: sc = KEY_ESCAPE; break;
    case XK_Tab: sc = KEY_TAB; break;
    case XK_BackSpace: sc = KEY_BACKSPACE; break;
    case XK_F2: sc = KEY_F2; break;
    default: break;
    }
    if (sc >= 0) gui_in_key(g, sc, down);
    if (down && n > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127)
        gui_in_text(g, buf, n);
}

void backend_pump(Gui *g) {
    X11Back *b = back_of(g);
    if (!b || !b->dpy) return;
    while (XPending(b->dpy)) {
        XEvent e;
        XNextEvent(b->dpy, &e);
        switch (e.type) {
        case MotionNotify:
            gui_in_motion(g, e.xmotion.x, e.xmotion.y);
            break;
        case ButtonPress:
            gui_in_motion(g, e.xbutton.x, e.xbutton.y);
            if (e.xbutton.button == Button1) {
                gui_in_button(g, 1, true);
                XSetInputFocus(b->dpy, b->win, RevertToParent, CurrentTime);
            } else if (e.xbutton.button == Button3) {
                gui_in_button(g, 3, true);
            } else if (e.xbutton.button == Button4) {
                gui_in_wheel(g, -1.0f);
            } else if (e.xbutton.button == Button5) {
                gui_in_wheel(g, 1.0f);
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1) gui_in_button(g, 1, false);
            else if (e.xbutton.button == Button3) gui_in_button(g, 3, false);
            break;
        case KeyPress: x_key(g, &e.xkey, true); break;
        case KeyRelease: x_key(g, &e.xkey, false); break;
        case EnterNotify: gui_in_inside(g, true); break;
        case LeaveNotify: gui_in_inside(g, false); break;
        default: break;
        }
    }
}
