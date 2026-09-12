#ifndef BYPO_UI_H
#define BYPO_UI_H

#include "canvas.h"
#include "text.h"

/* widget identity: any stable string, hashed */
typedef uint64_t UiId;
UiId ui_id(const char *s);
UiId ui_id_n(const char *s, int n);

typedef enum { CURSOR_DEFAULT, CURSOR_RESIZE_H, CURSOR_RESIZE_V, CURSOR_POINTER,
               CURSOR_TEXT } UiCursor;

typedef struct {
    /* per-frame input, design-space coordinates */
    P2 mouse;
    bool mouse_in_window;
    bool down;          /* left button held */
    bool pressed;       /* went down this frame */
    bool released;      /* went up this frame */
    bool double_clicked;/* second press within 0.4 s and 6 px */
    float wheel;        /* scroll delta, +down */
    char text[64];      /* UTF-8 text typed this frame */
    bool key_pressed[512];   /* SDL scancodes */
    bool key_down[512];
    bool backspace_repeat;
} UiInput;

typedef struct {
    Canvas *canvas;
    UiInput in;
    double time;         /* seconds since start */
    float dt;
    UiId active;         /* widget being dragged / held */
    UiId hot;            /* hovered this frame (set during walk) */
    UiId focus;          /* text field with keyboard focus */
    UiCursor cursor;
    bool repaint_soon;   /* a widget asked for a quick repaint */
    /* double-click bookkeeping (filled by the SDL layer) */
    double last_press_time;
    P2 last_press_pos;
    P2 drag_prev; /* mouse position last frame, for per-frame drag deltas */
    /* animate_bool storage */
    struct { UiId id; float v; } anim[128];
    int anim_count;
} Ui;

/* response mirroring the egui subset the widgets use */
typedef struct {
    Rct rect;
    bool hovered;
    bool clicked;
    bool double_clicked;
    bool dragged;
    bool drag_started;
    bool pressed_on;    /* pointer button currently down on it */
    P2 drag_delta;
    P2 pointer;         /* interact position */
} Resp;

/* hover + click sensing on a rect; slop expands the hover zone only */
Resp ui_interact(Ui *ui, UiId id, Rct r, float hover_slop);
/* click_and_drag sensing; drag_delta is this frame's mouse movement */
Resp ui_interact_drag(Ui *ui, UiId id, Rct r, float hover_slop);

/* egui animate_bool_with_time equivalent */
float ui_animate_bool(Ui *ui, UiId id, bool target, float seconds);

/* simple vertical scroll region: returns the y offset to subtract when
   drawing content; call with the region rect and total content height */
typedef struct { float offset; } UiScroll;
float ui_scroll(Ui *ui, UiScroll *s, Rct viewport, float content_h);

/* single-line text field state (caret at end; block caret drawn by caller) */
typedef struct {
    char text[256];
    int len;
} UiText;
/* applies this frame's typed text/backspace when focused; true if changed */
bool ui_text_edit(Ui *ui, UiId id, UiText *t);

#endif
