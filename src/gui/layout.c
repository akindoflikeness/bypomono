#include "layout.h"

#include <math.h>

static float span(float want, float have) {
    if (want < 0.0f) return 0.0f;
    return want < have ? want : (have > 0.0f ? have : 0.0f);
}

Rct cut_top(Rct *r, float h) {
    h = span(h, rct_h(*r));
    Rct out = rct(r->x0, r->y0, r->x1, r->y0 + h);
    r->y0 += h;
    return out;
}

Rct cut_bottom(Rct *r, float h) {
    h = span(h, rct_h(*r));
    Rct out = rct(r->x0, r->y1 - h, r->x1, r->y1);
    r->y1 -= h;
    return out;
}

Rct cut_left(Rct *r, float w) {
    w = span(w, rct_w(*r));
    Rct out = rct(r->x0, r->y0, r->x0 + w, r->y1);
    r->x0 += w;
    return out;
}

Rct cut_right(Rct *r, float w) {
    w = span(w, rct_w(*r));
    Rct out = rct(r->x1 - w, r->y0, r->x1, r->y1);
    r->x1 -= w;
    return out;
}

/* ---------- stack ---------- */

Stack stack_in(Rct area, float gap, const char *name) {
    return (Stack){area, area.y0, gap, name};
}

Rct stack_row(Stack *s, float h) {
    Rct r = rct(s->area.x0, s->y, s->area.x1, s->y + h);
    s->y += h + s->gap;
    return r;
}

void stack_space(Stack *s, float h) { s->y += h; }

Rct stack_rest(const Stack *s) {
    float y = s->y < s->area.y1 ? s->y : s->area.y1;
    return rct(s->area.x0, y, s->area.x1, s->area.y1);
}

void stack_close(const Stack *s) {
    /* the gap after the last row is not content */
    float over = s->y - s->gap - s->area.y1;
    if (over > 0.5f) layout_overflow(s->name, over);
}

/* ---------- flow ---------- */

Flow flow_in(Rct area, float row_h, float gap) {
    return (Flow){area, area.x0, area.y0, row_h, gap};
}

Rct flow_next(Flow *f, float w) {
    if (f->x > f->area.x0 && f->x + w > f->area.x1) {
        f->x = f->area.x0;
        f->y += f->row_h + f->gap;
    }
    Rct r = rct_xywh(f->x, f->y, w, f->row_h);
    f->x += w + f->gap;
    return r;
}

float flow_bottom(const Flow *f) { return f->y + f->row_h; }

/* ---------- overflow ---------- */

static int s_overflows;
static const char *s_first_name;
static float s_first_px;

void layout_frame_begin(void) {
    s_overflows = 0;
    s_first_name = 0;
}

void layout_overflow(const char *name, float px) {
    if (s_overflows++ == 0) {
        s_first_name = name;
        s_first_px = px;
    }
}

int layout_overflow_count(void) { return s_overflows; }

const char *layout_overflow_first(float *px) {
    if (px) *px = s_first_px;
    return s_first_name;
}

/* ---------- the screen ---------- */

Screen screen_layout(Rct full, float footer_h) {
    Screen s;
    Rct r = full;
    s.footer = cut_bottom(&r, footer_h);
    s.header = cut_top(&r, HEADER_H);
    cut_top(&r, PANEL_GAP);
    s.melody = cut_bottom(&r, MELODY_H);
    cut_bottom(&r, PANEL_GAP);
    s.sound = cut_left(&r, SOUND_W);
    cut_left(&r, PANEL_GAP);
    s.fm = cut_left(&r, FM_W);
    cut_left(&r, PANEL_GAP);
    s.space = cut_right(&r, SPACE_W);
    cut_right(&r, PANEL_GAP);
    s.display = r;
    return s;
}
