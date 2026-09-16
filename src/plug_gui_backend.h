#ifndef BYPO_PLUG_GUI_BACKEND_H
#define BYPO_PLUG_GUI_BACKEND_H

/* Everything a platform backend needs, and nothing else: the engine and app
   headers spell names the system frameworks also use (Cocoa has its own
   Delay), so no backend may see them. Gui stays opaque here; the fields a
   backend touches live in GuiSurface. */

#include <stdbool.h>
#include <stdint.h>

#include <clap/clap.h>

typedef struct Gui Gui;

/* one embedded window API per platform */
#if defined(_WIN32)
#define BACKEND_WINDOW_API CLAP_WINDOW_API_WIN32
#elif defined(__APPLE__)
#define BACKEND_WINDOW_API CLAP_WINDOW_API_COCOA
#else
#define BACKEND_WINDOW_API CLAP_WINDOW_API_X11
#endif

/* SDL scancode values for the keys the editor reads; UiInput is indexed by
   them and frame.c/panes.c spell them SDL_SCANCODE_*. Spelt out here so no
   backend has to include an SDL header. */
enum {
    KEY_RETURN = 40,
    KEY_ESCAPE = 41,
    KEY_BACKSPACE = 42,
    KEY_TAB = 43,
    KEY_F2 = 59,
    KEY_HOME = 74,
    KEY_PAGEUP = 75,
    KEY_END = 77,
    KEY_PAGEDOWN = 78,
    KEY_RIGHT = 79,
    KEY_LEFT = 80,
    KEY_DOWN = 81,
    KEY_UP = 82
};

/* the part of the editor state a backend reads and writes */
typedef struct {
    void *back;       /* platform window state, owned by the backend */
    uint32_t *out_px; /* the canvas magnified by scale, win_w * win_h;
                         NULL when the backend magnifies it itself */
    int win_w, win_h; /* window size, always device pixels */
    uint32_t *src_px; /* the design canvas at 1:1, src_w * src_h */
    int src_w, src_h;
    float host_scale; /* the host's factor, 1.0 until it says otherwise */
} GuiSurface;

GuiSurface *gui_surface(Gui *g);

/* ---------- backend: one implementation per platform ---------- */

/* true when the backend magnifies the design canvas itself (a compositor or
   the GPU), so the core hands it src_px and skips the CPU copy into out_px */
bool backend_scales_itself(void);
bool backend_open(Gui *g);  /* connect to the window system */
void backend_close(Gui *g); /* release everything the backend holds */
/* usable desktop in device pixels, for pick_display_scale */
void backend_usable_screen(Gui *g, int *w, int *h);
/* device pixels per logical point; 1.0 outside Cocoa */
float backend_px_per_point(Gui *g);
/* the host's own window in device pixels, false when it cannot be read;
   only the stats log asks, so it may be as slow as it likes */
bool backend_host_size(Gui *g, int *w, int *h);
/* create the child window inside the host's; it is sized by backend_resize */
bool backend_attach(Gui *g, const clap_window_t *window);
/* match the child window and any platform image to win_w/win_h/out_px */
bool backend_resize(Gui *g);
void backend_present(Gui *g); /* put out_px on screen now */
void backend_show(Gui *g);
void backend_hide(Gui *g);
int backend_event_fd(Gui *g); /* fd the host should poll, or -1 */
void backend_pump(Gui *g);    /* drain queued events into pending */

/* Drive one frame per display refresh from a platform timer on the main
   thread, because a host's CLAP timer can run far slower than it was asked
   for. False when the backend has none and the host timer must keep driving;
   cb is then never called. */
bool backend_start_frame_timer(Gui *g, void (*cb)(Gui *g));
void backend_stop_frame_timer(Gui *g);

/* ---------- input: fed by the backends, main thread ---------- */

void gui_in_motion(Gui *g, int px, int py);        /* window device pixels */
void gui_in_button(Gui *g, int button, bool down); /* 1 left, 3 right */
void gui_in_wheel(Gui *g, float delta);            /* + is down, 1 per notch */
void gui_in_inside(Gui *g, bool inside);
void gui_in_key(Gui *g, int scancode, bool down);
void gui_in_text(Gui *g, const char *utf8, int n);

/* the window lost its content: present again even if the editor did not
   change anything (X11 Expose) */
void gui_invalidate(Gui *g);

#endif
