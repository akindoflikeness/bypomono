#ifndef BYPO_PLUG_GUI_H
#define BYPO_PLUG_GUI_H

#include "plug.h"
#include "plug_gui_backend.h"

/* Where an editor frame spends its time, summed over one second. Off unless
   BYPO_GUI_STATS is set, and then out is the only thing a tick tests. */
typedef struct {
    FILE *out;    /* NULL when off */
    bool own_out; /* out is a file this opened, not stderr */
    double win_t0;
    double last_tick;
    long ticks, presents, gaps;
    double frame_s, fp_s, mag_s, present_s; /* summed over the window */
    double gap_s, gap_max;                  /* interval between timer calls */
} GuiStats;

struct Gui {
    GuiSurface s;     /* the only part a backend may touch */
    App *app;
    Ui ui;
    Canvas canvas;
    int *xmap;        /* out_px column -> canvas column */
    float fit;        /* largest scale this display fits */
    float scale;      /* fit * host_scale, snapped */
    bool created, parented, shown;
    clap_id timer_id;
    bool timer_on, fd_on;
    int fd; /* what fd_on registered */
    double t0, last_time;
    uint64_t last_hash; /* canvas fingerprint last presented */
    bool have_hash;
    /* input accumulated between timer ticks */
    UiInput pending;
    double last_click_time;
    P2 last_click_pos;
    GuiStats stats;
};

#endif
