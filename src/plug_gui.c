#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <SDL2/SDL_scancode.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plug.h"

#define TIMER_MS 16

typedef struct {
    App *app;
    Ui ui;
    Canvas canvas;
    Display *dpy;
    Window win;
    GC gc;
    XImage *img;
    uint32_t *out_px; /* scaled output buffer owned by img */
    int *xmap;
    float scale;
    int win_w, win_h;
    bool created, parented, shown;
    clap_id timer_id;
    bool timer_on, fd_on;
    double t0, last_time;
    /* input accumulated between timer ticks */
    UiInput pending;
    double last_click_time;
    P2 last_click_pos;
    bool have_x_error;
} Gui;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static Gui *gui_of(const clap_plugin_t *pl) {
    return ((Plug *)pl->plugin_data)->gui_state;
}

/* ---------- shadows <- host params ---------- */

static void refresh_shadows(App *a, Plug *p) {
    Session s = plug_session_of_vals(p);
    a->shadow = s.patch;
    a->shadow_verb = s.verb;
    a->shadow_melody = s.melody;
    a->shadow_chandas = s.chandas;
    a->shadow_warmth = s.warmth;
    a->drone_hz = s.drone_hz;
    a->shadow_release_s = (float)plug_getv(p, P_RELEASE);
    a->engaged = plug_getv(p, P_DRONE) > 0.5;
}

/* ---------- input translation ---------- */

static void note_press(Gui *g, double t) {
    UiInput *in = &g->pending;
    in->down = true;
    in->pressed = true;
    if (t - g->last_click_time < 0.4 && fabsf(in->mouse.x - g->last_click_pos.x) < 6.0f
        && fabsf(in->mouse.y - g->last_click_pos.y) < 6.0f)
        in->double_clicked = true;
    g->last_click_time = t;
    g->last_click_pos = in->mouse;
}

static void x_key(Gui *g, XKeyEvent *ev, bool down) {
    char buf[8] = {0};
    KeySym ks = 0;
    int n = XLookupString(ev, buf, sizeof buf - 1, &ks, NULL);
    int sc = -1;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: sc = SDL_SCANCODE_RETURN; break;
    case XK_Escape: sc = SDL_SCANCODE_ESCAPE; break;
    case XK_Tab: sc = SDL_SCANCODE_TAB; break;
    case XK_BackSpace: sc = SDL_SCANCODE_BACKSPACE; break;
    case XK_F2: sc = SDL_SCANCODE_F2; break;
    default: break;
    }
    if (sc >= 0 && sc < 512) {
        if (down) {
            g->pending.key_pressed[sc] = true;
            g->pending.key_down[sc] = true;
        } else {
            g->pending.key_down[sc] = false;
        }
    }
    if (down) {
        if (ks == XK_BackSpace) g->pending.backspace_repeat = true;
        if (n > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
            size_t cur = strlen(g->pending.text);
            if (cur + (size_t)n < sizeof g->pending.text) {
                memcpy(g->pending.text + cur, buf, (size_t)n);
                g->pending.text[cur + (size_t)n] = '\0';
            }
        }
    }
}

static void pump_x(Gui *g) {
    if (!g->dpy) return;
    while (XPending(g->dpy)) {
        XEvent e;
        XNextEvent(g->dpy, &e);
        UiInput *in = &g->pending;
        switch (e.type) {
        case MotionNotify:
            in->mouse.x = (float)e.xmotion.x / g->scale;
            in->mouse.y = (float)e.xmotion.y / g->scale;
            in->mouse_in_window = true;
            break;
        case ButtonPress:
            in->mouse.x = (float)e.xbutton.x / g->scale;
            in->mouse.y = (float)e.xbutton.y / g->scale;
            if (e.xbutton.button == Button1) {
                note_press(g, now_s());
                XSetInputFocus(g->dpy, g->win, RevertToParent, CurrentTime);
            } else if (e.xbutton.button == Button4) {
                in->wheel -= 1.0f;
            } else if (e.xbutton.button == Button5) {
                in->wheel += 1.0f;
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1) {
                in->down = false;
                in->released = true;
            }
            break;
        case KeyPress: x_key(g, &e.xkey, true); break;
        case KeyRelease: x_key(g, &e.xkey, false); break;
        case EnterNotify: in->mouse_in_window = true; break;
        case LeaveNotify: in->mouse_in_window = false; break;
        default: break;
        }
    }
}

/* ---------- blit ---------- */

static void blit(Gui *g) {
    if (!g->dpy || !g->parented || !g->img) return;
    const uint32_t *src = g->canvas.px;
    uint32_t *dst = g->out_px;
    int dw = g->win_w, dh = g->win_h;
    if (g->scale == 1.0f) {
        memcpy(dst, src, (size_t)dw * dh * 4);
    } else {
        for (int y = 0; y < dh; y++) {
            int sy = (int)((float)y / g->scale);
            if (sy >= g->canvas.h) sy = g->canvas.h - 1;
            const uint32_t *row = src + (size_t)sy * g->canvas.w;
            uint32_t *orow = dst + (size_t)y * dw;
            for (int x = 0; x < dw; x++) orow[x] = row[g->xmap[x]];
        }
    }
    XPutImage(g->dpy, g->win, g->gc, g->img, 0, 0, 0, 0, (unsigned)dw,
              (unsigned)dh);
    XFlush(g->dpy);
}

/* ---------- one editor frame ---------- */

static void gui_tick(Plug *p, Gui *g) {
    if (!g->created || !g->parented) return;
    pump_x(g);

    Ui *ui = &g->ui;
    ui->in = g->pending;
    /* clear edge-triggered state for the next accumulation window */
    g->pending.pressed = g->pending.released = g->pending.double_clicked = false;
    g->pending.wheel = 0.0f;
    g->pending.text[0] = '\0';
    g->pending.backspace_repeat = false;
    memset(g->pending.key_pressed, 0, sizeof g->pending.key_pressed);

    double t = now_s() - g->t0;
    ui->time = t;
    ui->dt = (float)(t - g->last_time);
    g->last_time = t;
    ui->cursor = CURSOR_DEFAULT;
    ui->hot = 0;
    ui->repaint_soon = false;
    if (ui->in.pressed) {
        ui->last_press_pos = ui->in.mouse;
        ui->last_press_time = t;
    }

    if (atomic_exchange_explicit(&p->host_touched, false, memory_order_relaxed))
        refresh_shadows(g->app, p);

    app_frame(g->app, ui);
    ui->drag_prev = ui->in.mouse;
    g->app->quit = false; /* nothing in a plugin may end the host */
    blit(g);
}

/* ---------- clap gui extension ---------- */

static bool gui_is_api_supported(const clap_plugin_t *pl, const char *api,
                                 bool is_floating) {
    return strcmp(api, CLAP_WINDOW_API_X11) == 0 && !is_floating;
}

static bool gui_get_preferred_api(const clap_plugin_t *pl, const char **api,
                                  bool *is_floating) {
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

static bool gui_create(const clap_plugin_t *pl, const char *api,
                       bool is_floating) {
    if (!gui_is_api_supported(pl, api, is_floating)) return false;
    Plug *p = pl->plugin_data;
    Gui *g = p->gui_state;
    if (!g) {
        g = calloc(1, sizeof *g);
        if (!g) return false;
        p->gui_state = g;
        g->scale = 1.0f;
    }
    if (g->created) return true;

    static bool fonts_ready = false;
    if (!fonts_ready) {
        prepare_preset_dir();
        if (text_init(asset_dir()) != 0)
            fprintf(stderr, "bypo: fonts not found under %s/fonts\n",
                    asset_dir());
        fonts_ready = true;
    }

    g->dpy = XOpenDisplay(NULL);
    if (!g->dpy) return false;

    if (!g->app) {
        g->app = calloc(1, sizeof *g->app);
        if (!g->app) return false;
        app_init_defaults(g->app);
        g->app->have_shot = false;      /* env vars belong to the DAW */
        g->app->have_report_at = false;
        g->app->restored = true;
        g->app->hosted = true;
        preset_rescan(g->app);
    }
    g->app->sample_rate = (float)p->sr;
    g->app->channels = 2;
    refresh_shadows(g->app, p);
    g->app->splash_over = false;

    canvas_init(&g->canvas, (int)DESIGN_W, (int)DESIGN_H);
    memset(&g->ui, 0, sizeof g->ui);
    g->ui.canvas = &g->canvas;
    g->t0 = now_s();
    g->last_time = 0.0;
    memset(&g->pending, 0, sizeof g->pending);

    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->register_timer(p->host, TIMER_MS, &g->timer_id))
        g->timer_on = true;
    const clap_host_posix_fd_support_t *hf =
        p->host->get_extension(p->host, CLAP_EXT_POSIX_FD_SUPPORT);
    if (hf
        && hf->register_fd(p->host, ConnectionNumber(g->dpy),
                           CLAP_POSIX_FD_READ))
        g->fd_on = true;

    g->created = true;
    return true;
}

static void gui_destroy(const clap_plugin_t *pl) {
    Plug *p = pl->plugin_data;
    Gui *g = p->gui_state;
    if (!g || !g->created) return;
    /* unhook the audio thread first; the App itself stays allocated so the
       renderer can never race a free */
    atomic_store_explicit(&p->gui_app, NULL, memory_order_release);
    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (g->timer_on && ht) ht->unregister_timer(p->host, g->timer_id);
    g->timer_on = false;
    const clap_host_posix_fd_support_t *hf =
        p->host->get_extension(p->host, CLAP_EXT_POSIX_FD_SUPPORT);
    if (g->fd_on && hf && g->dpy)
        hf->unregister_fd(p->host, ConnectionNumber(g->dpy));
    g->fd_on = false;
    if (g->dpy) {
        if (g->img) {
            XDestroyImage(g->img); /* frees out_px */
            g->img = NULL;
            g->out_px = NULL;
        }
        if (g->gc) XFreeGC(g->dpy, g->gc);
        g->gc = NULL;
        if (g->win) XDestroyWindow(g->dpy, g->win);
        g->win = 0;
        XCloseDisplay(g->dpy);
        g->dpy = NULL;
    }
    free(g->xmap);
    g->xmap = NULL;
    canvas_free(&g->canvas);
    g->created = g->parented = g->shown = false;
}

static bool gui_set_scale(const clap_plugin_t *pl, double scale) {
    Gui *g = gui_of(pl);
    if (!g) return false;
    float s = (float)(round(scale * 4.0) / 4.0);
    g->scale = clampf(s, 1.0f, 2.0f);
    return true;
}

static bool gui_get_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h) {
    Gui *g = gui_of(pl);
    float s = g ? g->scale : 1.0f;
    *w = (uint32_t)lroundf(DESIGN_W * s);
    *h = (uint32_t)lroundf(DESIGN_H * s);
    return true;
}

static bool gui_can_resize(const clap_plugin_t *pl) { return false; }

static bool gui_get_resize_hints(const clap_plugin_t *pl,
                                 clap_gui_resize_hints_t *hints) {
    return false;
}

static bool gui_adjust_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h) {
    return gui_get_size(pl, w, h);
}

static bool gui_set_size(const clap_plugin_t *pl, uint32_t w, uint32_t h) {
    uint32_t ow, oh;
    gui_get_size(pl, &ow, &oh);
    return w == ow && h == oh;
}

static bool gui_set_parent(const clap_plugin_t *pl,
                           const clap_window_t *window) {
    Plug *p = pl->plugin_data;
    Gui *g = p->gui_state;
    if (!g || !g->created || !g->dpy) return false;

    g->win_w = (int)lroundf(DESIGN_W * g->scale);
    g->win_h = (int)lroundf(DESIGN_H * g->scale);

    int screen = DefaultScreen(g->dpy);
    g->win = XCreateSimpleWindow(g->dpy, (Window)window->x11, 0, 0,
                                 (unsigned)g->win_w, (unsigned)g->win_h, 0,
                                 BlackPixel(g->dpy, screen),
                                 BlackPixel(g->dpy, screen));
    XSelectInput(g->dpy, g->win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask
                     | PointerMotionMask | KeyPressMask | KeyReleaseMask
                     | EnterWindowMask | LeaveWindowMask);
    g->gc = XCreateGC(g->dpy, g->win, 0, NULL);

    g->out_px = malloc((size_t)g->win_w * g->win_h * 4);
    if (!g->out_px) return false;
    g->img = XCreateImage(g->dpy, DefaultVisual(g->dpy, screen),
                          (unsigned)DefaultDepth(g->dpy, screen), ZPixmap, 0,
                          (char *)g->out_px, (unsigned)g->win_w,
                          (unsigned)g->win_h, 32, 0);
    g->xmap = malloc((size_t)g->win_w * sizeof(int));
    for (int x = 0; x < g->win_w; x++) {
        int sx = (int)((float)x / g->scale);
        g->xmap[x] = sx < g->canvas.w ? sx : g->canvas.w - 1;
    }

    XMapWindow(g->dpy, g->win);
    XFlush(g->dpy);
    g->parented = true;
    /* the editor is real now: let the audio thread feed it */
    atomic_store_explicit(&p->gui_app, g->app, memory_order_release);
    return true;
}

static bool gui_set_transient(const clap_plugin_t *pl,
                              const clap_window_t *window) {
    return false;
}

static void gui_suggest_title(const clap_plugin_t *pl, const char *title) {}

static bool gui_show(const clap_plugin_t *pl) {
    Gui *g = gui_of(pl);
    if (!g || !g->parented) return false;
    XMapWindow(g->dpy, g->win);
    XFlush(g->dpy);
    g->shown = true;
    return true;
}

static bool gui_hide(const clap_plugin_t *pl) {
    Gui *g = gui_of(pl);
    if (!g || !g->parented) return false;
    XUnmapWindow(g->dpy, g->win);
    XFlush(g->dpy);
    g->shown = false;
    return true;
}

const clap_plugin_gui_t PLUG_EXT_GUI = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy,
    gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints,
    gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
    gui_suggest_title, gui_show, gui_hide,
};

/* ---------- host callbacks ---------- */

static void on_timer(const clap_plugin_t *pl, clap_id timer_id) {
    Plug *p = pl->plugin_data;
    Gui *g = p->gui_state;
    if (g && g->timer_on && timer_id == g->timer_id) gui_tick(p, g);
}

const clap_plugin_timer_support_t PLUG_EXT_TIMER = {on_timer};

static void on_fd(const clap_plugin_t *pl, int fd, clap_posix_fd_flags_t flags) {
    Gui *g = gui_of(pl);
    if (g && g->created) pump_x(g);
}

const clap_plugin_posix_fd_support_t PLUG_EXT_FD = {on_fd};
