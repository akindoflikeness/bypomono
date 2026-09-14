#ifndef BYPO_PLUG_GUI_H
#define BYPO_PLUG_GUI_H

#include "plug.h"
#include "plug_gui_backend.h"

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
};

#endif
