#ifndef BYPO_LAYOUT_H
#define BYPO_LAYOUT_H

#include "canvas.h"

/* Rect cutting: each cut takes a slice off one side of *r and returns it.
   A slice never goes past what is left, so *r can end up empty. */
Rct cut_top(Rct *r, float h);
Rct cut_bottom(Rct *r, float h);
Rct cut_left(Rct *r, float w);
Rct cut_right(Rct *r, float w);

/* Rows handed out top to bottom inside an area. Rows past the bottom are
   still handed out, and counted as overflow when the stack is closed. */
typedef struct {
    Rct area;
    float y, gap;
    const char *name;
} Stack;
Stack stack_in(Rct area, float gap, const char *name);
Rct stack_row(Stack *s, float h);
void stack_space(Stack *s, float h);
/* what is left below the last row */
Rct stack_rest(const Stack *s);
void stack_close(const Stack *s);

/* Chips laid left to right, wrapping onto a new row when one will not fit. */
typedef struct {
    Rct area;
    float x, y, row_h, gap;
} Flow;
Flow flow_in(Rct area, float row_h, float gap);
Rct flow_next(Flow *f, float w);
/* the y just below the last row */
float flow_bottom(const Flow *f);

/* Overflow is counted per frame; the screenshot mode reports it. */
void layout_frame_begin(void);
void layout_overflow(const char *name, float px);
int layout_overflow_count(void);
const char *layout_overflow_first(float *px);

/* ---------- the screen ---------- */

#define HEADER_H 30.0f
#define BOTTOM_STRIP_H 190.0f
#define SOUND_W 270.0f
#define FM_W 330.0f
#define SPACE_W 222.0f
#define PANEL_GAP 5.0f

typedef struct {
    Rct header, sound, fm, display, space, strip, footer;
} Screen;

/* the fixed arrangement of the window; footer_h is the console bar */
Screen screen_layout(Rct full, float footer_h);

#endif
