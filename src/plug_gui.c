/* Platform-independent half of the CLAP editor: it owns the App and renders
   the fixed DESIGN_W x DESIGN_H canvas. X11 magnifies that into out_px; Win32
   and Cocoa scale the canvas themselves. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plug_gui.h"

#define TIMER_MS 16

#if defined(_WIN32)
/* <windows.h> cannot come in here: it spells enumerator names the engine
   headers also use, so the two counters are declared by hand. */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *freq);

static double now_s(void) {
    static double period = 0.0;
    if (period == 0.0) {
        long long f = 0;
        QueryPerformanceFrequency(&f);
        period = f > 0 ? 1.0 / (double)f : 1e-7;
    }
    long long c = 0;
    QueryPerformanceCounter(&c);
    return (double)c * period;
}
#else
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

static Gui *gui_of(const clap_plugin_t *pl) {
    return ((Plug *)pl->plugin_data)->gui_state;
}

/* The most magnification the editor takes from the host's display density.
   The 1180x780 design at a 200% laptop density is 2360x1560 device pixels,
   nearly the whole screen; 1.5 keeps it to 1770x1170. Lower it if the window
   is still too big for you, raise it towards WINDOW_SCALE_MAX for a big
   monitor. Only Windows reports density this way; X11 and Cocoa are unchanged. */
#if defined(_WIN32)
#define HOST_SCALE_CAP 1.5f
#else
#define HOST_SCALE_CAP WINDOW_SCALE_MAX
#endif

static float compose_scale(const Gui *g) {
    float hs = g->s.host_scale;
    if (hs > HOST_SCALE_CAP) hs = HOST_SCALE_CAP;
    return snap_scale(g->fit * hs);
}

GuiSurface *gui_surface(Gui *g) { return &g->s; }

/* ---------- frame stats ---------- */
/* BYPO_GUI_STATS=1 writes to stderr, any other value names a file to append
   to. Nothing here runs unless the editor was opened with it set. */

static void stats_open(Gui *g) {
    const char *env = getenv("BYPO_GUI_STATS");
    if (!env || !*env || strcmp(env, "0") == 0) return;
    if (strcmp(env, "1") == 0) {
        g->stats.out = stderr;
        return;
    }
    g->stats.out = fopen(env, "a");
    if (g->stats.out) g->stats.own_out = true;
}

static void stats_close(Gui *g) {
    if (g->stats.out && g->stats.own_out) fclose(g->stats.out);
    memset(&g->stats, 0, sizeof g->stats);
}

/* frame and fp run on every tick, mag and present only on a tick that
   reaches the screen, so each is averaged over the count it belongs to */
static void stats_emit(Gui *g) {
    GuiStats *st = &g->stats;
    double per_tick = st->ticks ? 1.0 / (double)st->ticks : 0.0;
    double per_present = st->presents ? 1.0 / (double)st->presents : 0.0;
    double gap_avg = st->gaps ? st->gap_s / (double)st->gaps : 0.0;
    fprintf(st->out,
            "gui: ticks=%ld presents=%ld frame=%.2fms fp=%.2fms mag=%.2fms "
            "present=%.2fms tick_gap=%.2f/%.2f ms src=%s scale=%.2f win=%dx%d\n",
            st->ticks, st->presents, st->frame_s * per_tick * 1e3,
            st->fp_s * per_tick * 1e3, st->mag_s * per_present * 1e3,
            st->present_s * per_present * 1e3, gap_avg * 1e3, st->gap_max * 1e3,
            g->native_timer ? "native" : "host", g->scale, g->s.win_w,
            g->s.win_h);
    fflush(st->out);
}

/* close the reporting window if a second has passed, then book this tick */
static void stats_tick(Gui *g, double now) {
    GuiStats *st = &g->stats;
    if (st->win_t0 == 0.0) {
        st->win_t0 = now;
    } else if (now - st->win_t0 >= 1.0) {
        stats_emit(g);
        st->win_t0 = now;
        st->ticks = st->presents = st->gaps = 0;
        st->frame_s = st->fp_s = st->mag_s = st->present_s = 0.0;
        st->gap_s = st->gap_max = 0.0;
    }
    if (st->last_tick > 0.0) {
        double gap = now - st->last_tick;
        st->gap_s += gap;
        st->gaps++;
        if (gap > st->gap_max) st->gap_max = gap;
    }
    st->last_tick = now;
    st->ticks++;
}

/* ---------- shadows <- host params ---------- */

static void refresh_shadows(App *a, Plug *p) {
    Session s = plug_session_of_vals(p);
    a->shadow = s.patch;
    a->shadow_verb = s.verb;
    a->shadow_melody = s.melody;
    a->shadow_chandas = s.chandas;
    a->shadow_warmth = s.warmth;
    a->shadow_limiter_enabled = s.limiter_enabled;
    a->shadow_limiter_ceiling_db = s.limiter_ceiling_db;
    a->shadow_output_gain_db = s.output_gain_db;
    a->drone_hz = s.drone_hz;
    a->shadow_attack_s = s.attack_s;
    a->shadow_decay_s = s.decay_s;
    a->shadow_sustain = s.sustain;
    a->shadow_release_s = s.release_s;
    a->engaged = plug_getv(p, P_DRONE) > 0.5;
}

/* ---------- input ---------- */

void gui_in_motion(Gui *g, int px, int py) {
    g->pending.mouse.x = (float)px / g->scale;
    g->pending.mouse.y = (float)py / g->scale;
    g->pending.mouse_in_window = true;
}

void gui_in_button(Gui *g, int button, bool down) {
    if (button != 1) return; /* the editor reads the left button only */
    UiInput *in = &g->pending;
    if (!down) {
        in->down = false;
        in->released = true;
        return;
    }
    double t = now_s();
    in->down = true;
    in->pressed = true;
    if (t - g->last_click_time < 0.4
        && fabsf(in->mouse.x - g->last_click_pos.x) < 6.0f
        && fabsf(in->mouse.y - g->last_click_pos.y) < 6.0f)
        in->double_clicked = true;
    g->last_click_time = t;
    g->last_click_pos = in->mouse;
}

void gui_in_wheel(Gui *g, float delta) { g->pending.wheel += delta; }

void gui_in_inside(Gui *g, bool inside) {
    g->pending.mouse_in_window = inside;
}

void gui_in_key(Gui *g, int scancode, bool down) {
    if (scancode < 0 || scancode >= 512) return;
    if (down) {
        g->pending.key_pressed[scancode] = true;
        g->pending.key_down[scancode] = true;
        if (scancode == KEY_BACKSPACE) g->pending.backspace_repeat = true;
    } else {
        g->pending.key_down[scancode] = false;
    }
}

void gui_in_release_button(Gui *g) {
    if (!g->pending.down) return;
    g->pending.down = false;
    g->pending.released = true;
}

void gui_in_cancel(Gui *g) {
    gui_in_release_button(g);
    g->pending.mouse_in_window = false;
    memset(g->pending.key_down, 0, sizeof g->pending.key_down);
    memset(g->pending.key_pressed, 0, sizeof g->pending.key_pressed);
}

void gui_in_text(Gui *g, const char *utf8, int n) {
    if (n <= 0) return;
    size_t cur = strlen(g->pending.text);
    if (cur + (size_t)n >= sizeof g->pending.text) return;
    memcpy(g->pending.text + cur, utf8, (size_t)n);
    g->pending.text[cur + (size_t)n] = '\0';
}

/* ---------- output surface ---------- */

static void free_surface(Gui *g) {
    free(g->s.out_px);
    g->s.out_px = NULL;
    g->s.src_px = NULL;
    free(g->xmap);
    g->xmap = NULL;
}

static bool build_surface(Gui *g) {
    GuiSurface *s = &g->s;
    free_surface(g);
    s->win_w = (int)lroundf(DESIGN_W * g->scale);
    s->win_h = (int)lroundf(DESIGN_H * g->scale);
    s->src_px = g->canvas.px;
    s->src_w = g->canvas.w;
    s->src_h = g->canvas.h;
    g->have_hash = false; /* nothing on screen matches the new surface */
    if (backend_scales_itself()) return true;

    s->out_px = malloc((size_t)s->win_w * s->win_h * 4);
    if (!s->out_px) return false;
    g->xmap = malloc((size_t)s->win_w * sizeof *g->xmap);
    if (!g->xmap) {
        free_surface(g);
        return false;
    }
    for (int x = 0; x < s->win_w; x++) {
        int sx = (int)((float)x / g->scale);
        g->xmap[x] = sx < g->canvas.w ? sx : g->canvas.w - 1;
    }
    return true;
}

static void magnify(Gui *g) {
    if (!g->s.out_px) return;
    const uint32_t *src = g->canvas.px;
    uint32_t *dst = g->s.out_px;
    int dw = g->s.win_w, dh = g->s.win_h;
    if (g->scale == 1.0f) {
        memcpy(dst, src, (size_t)dw * dh * 4);
        return;
    }
    for (int y = 0; y < dh; y++) {
        int sy = (int)((float)y / g->scale);
        if (sy >= g->canvas.h) sy = g->canvas.h - 1;
        const uint32_t *row = src + (size_t)sy * g->canvas.w;
        uint32_t *orow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) orow[x] = row[g->xmap[x]];
    }
}

void gui_invalidate(Gui *g) { g->have_hash = false; }

/* Content fingerprint, so a frame that redraws to the same pixels costs the
   host nothing. Four independent lanes so the multiplies pipeline. */
static uint64_t canvas_fingerprint(const Canvas *c) {
    const uint64_t *p = (const uint64_t *)(const void *)c->px;
    size_t n = (size_t)c->w * (size_t)c->h / 2;
    uint64_t a = 0x243f6a8885a308d3ull, b = 0x13198a2e03707344ull;
    uint64_t d = 0xa4093822299f31d0ull, e = 0x082efa98ec4e6c89ull;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a = (a ^ p[i]) * 0x100000001b3ull;
        b = (b ^ p[i + 1]) * 0x100000001b3ull;
        d = (d ^ p[i + 2]) * 0x100000001b3ull;
        e = (e ^ p[i + 3]) * 0x100000001b3ull;
    }
    for (; i < n; i++) a = (a ^ p[i]) * 0x100000001b3ull;
    return a ^ (b + 0x9e3779b97f4a7c15ull) ^ (d << 17) ^ (e >> 13);
}

/* ---------- one editor frame ---------- */

static void gui_frame(Plug *p, Gui *g) {
    if (!g->created || !g->parented) return;
    GuiStats *st = g->stats.out ? &g->stats : NULL;
    if (st) stats_tick(g, now_s());
    backend_pump(g);
    if (!g->shown) {
        /* nothing to draw into; keep the clock current so the first frame
           back does not see one huge dt */
        g->last_time = now_s() - g->t0;
        return;
    }

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
        /* where the button went down, not where the pointer is by now */
        ui->last_press_pos = g->last_click_pos;
        ui->last_press_time = t;
    }

    if (atomic_exchange_explicit(&p->host_touched, false, memory_order_relaxed))
        refresh_shadows(g->app, p);

    double t_frame = st ? now_s() : 0.0;
    app_frame(g->app, ui);
    double t_fp = st ? now_s() : 0.0;
    ui->drag_prev = ui->in.mouse;
    g->app->quit = false; /* nothing in a plugin may end the host */

    /* An animated page (the shell, a blinking caret) already knows the
       pixels changed. Hashing the whole canvas just to confirm that reads
       every pixel and then presents anyway. */
    double t_mag = st ? now_s() : 0.0;
    if (!ui->repaint_soon) {
        uint64_t h = canvas_fingerprint(&g->canvas);
        if (st) st->fp_s += now_s() - t_mag;
        t_mag = st ? now_s() : 0.0;
        if (g->have_hash && h == g->last_hash) {
            if (st) st->frame_s += t_fp - t_frame;
            return;
        }
        g->last_hash = h;
        g->have_hash = true;
    } else {
        g->have_hash = false;
    }
    if (st) st->frame_s += t_fp - t_frame;
    if (!backend_scales_itself()) magnify(g);
    double t_present = st ? now_s() : 0.0;
    backend_present(g);
    if (st) {
        st->mag_s += t_present - t_mag;
        st->present_s += now_s() - t_present;
        st->presents++;
    }
}

/* Both timers land here. A frame that overruns its interval leaves the next
   one to be dropped rather than nested. */
static void gui_tick(Plug *p, Gui *g) {
    if (g->in_tick) return;
    g->in_tick = true;
    gui_frame(p, g);
    g->in_tick = false;
}

static void gui_frame_cb(Gui *g) {
    if (g->plug) gui_tick(g->plug, g);
}

static void drop_host_timer(Gui *g) {
    if (!g->timer_on || !g->plug) return;
    Plug *p = g->plug;
    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (ht) ht->unregister_timer(p->host, g->timer_id);
    g->timer_on = false;
}

static void ensure_host_timer(Gui *g) {
    if (g->timer_on || !g->plug) return;
    Plug *p = g->plug;
    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->register_timer(p->host, TIMER_MS, &g->timer_id))
        g->timer_on = true;
}

static void start_native_timer(Gui *g) {
    if (g->native_timer || !g->parented) return;
    g->native_timer = backend_start_frame_timer(g, gui_frame_cb);
    /* the host timer was the fallback. Once a native one is running, leave
       the host's message thread with a single 16 ms source. */
    if (g->native_timer) drop_host_timer(g);
}

static void stop_native_timer(Gui *g) {
    if (!g->native_timer) return;
    backend_stop_frame_timer(g);
    g->native_timer = false;
}

/* ---------- clap gui extension ---------- */

static bool gui_is_api_supported(const clap_plugin_t *pl, const char *api,
                                 bool is_floating) {
    return strcmp(api, BACKEND_WINDOW_API) == 0 && !is_floating;
}

static bool gui_get_preferred_api(const clap_plugin_t *pl, const char **api,
                                  bool *is_floating) {
    *api = BACKEND_WINDOW_API;
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
        g->plug = p;
        g->fit = WINDOW_SCALE_MIN;
        g->s.host_scale = 1.0f;
        g->scale = WINDOW_SCALE_MIN;
        g->fd = -1;
    }
    if (g->created) return true;
    stats_open(g);

    static bool fonts_ready = false;
    if (!fonts_ready) {
        prepare_preset_dir();
        text_init(asset_dir()); /* reports the source it loaded from itself */
        fonts_ready = true;
    }

    if (!backend_open(g)) return false;

    /* settled before the host can ask for a size */
    int avail_w = 0, avail_h = 0;
    backend_usable_screen(g, &avail_w, &avail_h);
    g->fit = pick_display_scale(avail_w, avail_h);
    g->scale = compose_scale(g);

    if (!g->app) {
        g->app = calloc(1, sizeof *g->app);
        if (!g->app) return false;
        app_init_defaults(g->app);
        g->app->restored = true;
        g->app->hosted = true;
        preset_rescan(g->app);
    }
    g->app->sample_rate = (float)p->sr;
    g->app->channels = 2;
    refresh_shadows(g->app, p);
    g->app->mods = p->mods_main;
    g->app->shadow_pitch = p->pitch_main;

    canvas_init(&g->canvas, (int)DESIGN_W, (int)DESIGN_H);
    memset(&g->ui, 0, sizeof g->ui);
    g->ui.canvas = &g->canvas;
    g->t0 = now_s();
    g->last_time = 0.0;
    memset(&g->pending, 0, sizeof g->pending);

    /* the fallback until a backend timer takes over on parent. X11 has no
       native timer, so this one stays. */
    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->register_timer(p->host, TIMER_MS, &g->timer_id))
        g->timer_on = true;
    int fd = backend_event_fd(g);
    if (fd >= 0) {
        const clap_host_posix_fd_support_t *hf =
            p->host->get_extension(p->host, CLAP_EXT_POSIX_FD_SUPPORT);
        if (hf && hf->register_fd(p->host, fd, CLAP_POSIX_FD_READ)) {
            g->fd_on = true;
            g->fd = fd;
        }
    }

    if (g->stats.out) {
        fprintf(g->stats.out,
                "gui: backend=%s scales_itself=%d px_per_point=%.2f fit=%.2f "
                "host_scale=%.2f scale=%.2f timer=%s/%dms\n",
                BACKEND_WINDOW_API, backend_scales_itself() ? 1 : 0,
                backend_px_per_point(g), g->fit, g->s.host_scale, g->scale,
                g->timer_on ? "on" : "off", TIMER_MS);
        fflush(g->stats.out);
    }

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
    plug_publish_bank(p, &g->app->mods, &g->app->shadow_pitch);
    stop_native_timer(g);
    const clap_host_timer_support_t *ht =
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (g->timer_on && ht) ht->unregister_timer(p->host, g->timer_id);
    g->timer_on = false;
    if (g->fd_on) {
        const clap_host_posix_fd_support_t *hf =
            p->host->get_extension(p->host, CLAP_EXT_POSIX_FD_SUPPORT);
        if (hf) hf->unregister_fd(p->host, g->fd);
        g->fd_on = false;
        g->fd = -1;
    }
    backend_close(g);
    free_surface(g);
    canvas_free(&g->canvas);
    stats_close(g);
    g->created = g->parented = g->shown = false;
}

static bool gui_get_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h);

static bool gui_set_scale(const clap_plugin_t *pl, double scale) {
    Plug *p = pl->plugin_data;
    Gui *g = p->gui_state;
    if (!g) return false;
    g->s.host_scale = scale > 0.0 ? (float)scale : 1.0f;
    float s = compose_scale(g);
    if (s == g->scale) return true;
    g->scale = s;
    if (!g->parented) return true;

    if (!build_surface(g) || !backend_resize(g)) return false;
    const clap_host_gui_t *hg = p->host->get_extension(p->host, CLAP_EXT_GUI);
    uint32_t w, h;
    gui_get_size(pl, &w, &h);
    if (hg && hg->request_resize) hg->request_resize(p->host, w, h);
    return true;
}

static bool gui_get_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h) {
    Gui *g = gui_of(pl);
    float s = g ? g->scale : 1.0f;
    /* Cocoa reports logical points, X11 and Win32 device pixels */
    float pp = g ? backend_px_per_point(g) : 1.0f;
    if (pp <= 0.0f) pp = 1.0f;
    *w = (uint32_t)lroundf(DESIGN_W * s / pp);
    *h = (uint32_t)lroundf(DESIGN_H * s / pp);
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
    if (!g || !g->created) return false;

    /* attach first: the backend may learn the monitor's scale from the parent */
    if (!backend_attach(g, window)) return false;
    g->scale = compose_scale(g);
    if (!build_surface(g) || !backend_resize(g)) return false;

    backend_show(g);
    g->parented = true;
    g->shown = true;
    start_native_timer(g);
    if (g->stats.out) {
        int hw = 0, hh = 0;
        char host[32];
        if (backend_host_size(g, &hw, &hh))
            snprintf(host, sizeof host, "%dx%d", hw, hh);
        else
            snprintf(host, sizeof host, "unknown");
        fprintf(g->stats.out,
                "gui: parented win=%dx%d host=%s scale=%.2f src=%s\n",
                g->s.win_w, g->s.win_h, host, g->scale,
                g->native_timer ? "native" : "host");
        fflush(g->stats.out);
    }
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
    backend_show(g);
    g->shown = true;
    g->have_hash = false; /* the window may have come back empty */
    start_native_timer(g);
    if (!g->native_timer) ensure_host_timer(g);
    return true;
}

static bool gui_hide(const clap_plugin_t *pl) {
    Gui *g = gui_of(pl);
    if (!g || !g->parented) return false;
    stop_native_timer(g);
    drop_host_timer(g); /* hidden: neither timer keeps the host thread awake */
    backend_hide(g);
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
    if (!g || !g->timer_on || timer_id != g->timer_id) return;
    if (g->native_timer) return; /* the backend timer renders instead */
    gui_tick(p, g);
}

const clap_plugin_timer_support_t PLUG_EXT_TIMER = {on_timer};

#if BYPO_GUI_POSIX_FD
static void on_fd(const clap_plugin_t *pl, int fd, clap_posix_fd_flags_t flags) {
    Gui *g = gui_of(pl);
    if (g && g->created) backend_pump(g);
}

const clap_plugin_posix_fd_support_t PLUG_EXT_FD = {on_fd};
#endif
