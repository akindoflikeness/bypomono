#include "app.h"

#include <SDL2/SDL_scancode.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define FADER_TEXT 13.0f
#define FADER_HOVER_SLOP 2.0f
#define MARGIN_TICK SECTION

#define WAVE_AMP 2.0f
#define WAVE_RATE_HZ 0.9f
#define WAVE_EASE_S 0.18f
#define WAVE_SCALE 1.3f
#define WAVE_TRACKING 1.6f

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float tab_width(float available, float gap, int n) {
    float nf = (float)(n < 1 ? 1 : n);
    float w = floorf((available - gap * (nf - 1.0f)) / nf);
    return w < 0.0f ? 0.0f : w;
}

void draw_margin_bar(Canvas *c, Rct bar, bool inner_edge_on_left) {
    float ox = inner_edge_on_left ? bar.x1 - MARGIN_TICK : bar.x0;
    float ys[2] = {bar.y0, bar.y1 - 1.0f};
    for (int i = 0; i < 2; i++)
        draw_rect_filled(c, rct_xywh(ox, ys[i], MARGIN_TICK, 1.0f), PAPER);
}

void hard_rect(Canvas *c, Rct r, float width) {
    draw_rect_stroke(c, r, width, PAPER);
}

void bubble_chain(Canvas *c, Ui *ui, P2 a, P2 b, bool active, float index,
                  double time) {
    (void)ui;
    (void)index;
    (void)time;
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 4.0f) return;
    float spacing = active ? 7.0f : 10.0f;
    int n = (int)ceilf(len / spacing);
    if (n < 2) n = 2;
    for (int k = 0; k <= n; k++) {
        float t = (float)k / (float)n;
        P2 p = {a.x + dx * t, a.y + dy * t};
        if (active) {
            float r = 1.3f +
                      1.4f * (0.5f + 0.5f * sinf((float)k * 0.9f + len * 0.13f));
            draw_circle_stroke(c, p, r, 1.0f, PAPER);
        } else {
            draw_rect_filled(c, rct(p.x - 0.75f, p.y - 0.75f, p.x + 0.75f,
                                    p.y + 0.75f),
                             PAPER);
        }
    }
}

/* ---------- faders ---------- */

static void backed_text(Canvas *c, P2 at, bool right, const char *s, FontId f) {
    if (!s || !s[0]) return;
    float w = text_width(f, s, 0.0f);
    float h = text_row_height(f);
    P2 min;
    if (right)
        min = (P2){at.x - w - 2.0f, at.y - h * 0.5f - 1.0f};
    else
        min = (P2){at.x - 2.0f, at.y - h * 0.5f - 1.0f};
    draw_rect_filled(c, rct_xywh(min.x, min.y, w + 4.0f, h + 2.0f), INK_BLACK);
    text_draw(c, f, at, right ? ALIGN_RIGHT_CENTER : ALIGN_LEFT_CENTER, s,
              PAPER, 0.0f);
}

FaderAct fader_track(Ui *ui, UiId id, Rct r, const char *label,
                     const char *value, float t) {
    Resp resp = ui_interact_drag(ui, id, r, FADER_HOVER_SLOP);
    t = clamp01(t);
    Canvas *c = ui->canvas;
    Rct inner = rct_shrink(r, 2.0f);
    float iw = rct_w(inner), ih = rct_h(inner);
    dither_rect(c, rct_xywh(inner.x0, inner.y0, iw * t, ih), 0.7f, 3.0f);
    if (t > 0.0f) {
        float x = roundf(inner.x0 + iw * t);
        if (x > inner.x1 - 1.0f) x = inner.x1 - 1.0f;
        draw_rect_filled(c, rct_xywh(x, inner.y0, 1.0f, ih), PAPER);
    }
    hard_rect(c, r, resp.hovered ? 2.0f : 1.0f);
    FontId lf = ui_font(FADER_TEXT);
    FontId vf = readout_font(FADER_TEXT);
    float cy = 0.5f * (inner.y0 + inner.y1);
    backed_text(c, (P2){inner.x0 + 4.0f, cy}, false, label, lf);
    backed_text(c, (P2){inner.x1 - 3.0f, cy}, true, value, vf);
    FaderAct act = {FADER_NONE, 0.0f};
    if (resp.double_clicked && resp.hovered) {
        /* Reset happens on the second press. Consume the active drag now so
           its later release cannot arrive as a normal click and overwrite
           the default with the pointer position. */
        if (ui->active == id) ui->active = 0;
        act.kind = FADER_RESET;
    } else if (resp.dragged || resp.clicked) {
        float denom = iw > 1.0f ? iw : 1.0f;
        act.kind = FADER_SET;
        act.t = clamp01((resp.pointer.x - inner.x0) / denom);
    }
    return act;
}

/* prototypes carry no per-param default: FADER_RESET is a no-op in these
   wrappers; call fader_track directly where reset-to-default is needed */
bool fader(Ui *ui, UiId id, Rct r, const char *label, float *v, float lo,
           float hi) {
    float span = fabsf(hi - lo);
    if (span < FLT_EPSILON) span = FLT_EPSILON;
    float t = clamp01((*v - lo) / span);
    char value[32];
    snprintf(value, sizeof value, "%.2f", (double)*v);
    FaderAct act = fader_track(ui, id, r, label, value, t);
    if (act.kind == FADER_SET) {
        float next = lo + act.t * span;
        if (next != *v) {
            *v = next;
            return true;
        }
    }
    return false;
}

float log_position(float p, float lo, float hi) {
    if (lo < 1e-6f) lo = 1e-6f;
    float ratio = hi / lo;
    if (ratio < 1.0f + FLT_EPSILON) ratio = 1.0f + FLT_EPSILON;
    return lo * powf(ratio, clamp01(p));
}

float position_of_log(float v, float lo, float hi) {
    if (lo < 1e-6f) lo = 1e-6f;
    float ratio = hi / lo;
    if (ratio < 1.0f + FLT_EPSILON) ratio = 1.0f + FLT_EPSILON;
    float q = v / lo;
    if (q < 1e-6f) q = 1e-6f;
    return clamp01(logf(q) / logf(ratio));
}

bool fader_log(Ui *ui, UiId id, Rct r, const char *label, float *v, float lo,
               float hi, const char *suffix) {
    float t = position_of_log(*v, lo, hi);
    char value[48];
    snprintf(value, sizeof value, "%.2f%s", (double)*v, suffix ? suffix : "");
    FaderAct act = fader_track(ui, id, r, label, value, t);
    if (act.kind == FADER_SET) {
        float next = log_position(act.t, lo, hi);
        if (next != *v) {
            *v = next;
            return true;
        }
    }
    return false;
}

bool fader_int(Ui *ui, UiId id, Rct r, const char *label, int *v, int lo,
               int hi) {
    float span = (float)(hi - lo < 1 ? 1 : hi - lo);
    float t = clamp01((float)(*v - lo) / span);
    char value[32];
    snprintf(value, sizeof value, "%d", *v);
    FaderAct act = fader_track(ui, id, r, label, value, t);
    if (act.kind == FADER_SET) {
        int next = (int)lroundf((float)lo + act.t * span);
        if (next < lo) next = lo;
        if (next > hi) next = hi;
        if (next != *v) {
            *v = next;
            return true;
        }
    }
    return false;
}

/* ---------- knobs ---------- */

#define KNOB_A0 (0.75f * PI_F)
#define KNOB_SWEEP (1.5f * PI_F)
#define KNOB_TEXT 11.0f

float env_time_at(float t, float lo) {
    float floor = fmaxf(ENV_TIME_FLOOR, lo);
    t = clamp01(t);
    if (t <= 0.0f) return lo;
    return floor * powf(ENV_TIME_MAX / floor, t);
}

float env_time_pos(float v, float lo) {
    float floor = fmaxf(ENV_TIME_FLOOR, lo);
    if (!(v > floor)) return 0.0f;
    return clamp01(logf(v / floor) / logf(ENV_TIME_MAX / floor));
}

static P2 on_circle(P2 c, float r, float a) {
    return (P2){c.x + cosf(a) * r, c.y + sinf(a) * r};
}

FaderAct knob_track(Ui *ui, UiId id, Rct r, const char *label,
                    const char *value, float t) {
    Resp resp = ui_interact_drag(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    t = clamp01(t);
    FontId lf = ui_font(KNOB_TEXT);
    FontId vf = ui_font(KNOB_TEXT);
    float lh = text_row_height(lf), vh = text_row_height(vf);
    float radius = floorf(fminf(rct_w(r) * 0.5f - 4.0f,
                                (rct_h(r) - lh - vh - 8.0f) * 0.5f));
    if (radius < 6.0f) radius = 6.0f;
    P2 ce = {roundf(rct_center(r).x), roundf(r.y0 + lh + 4.0f + radius)};

    text_draw(c, lf, (P2){ce.x, r.y0}, ALIGN_CENTER_TOP, label, PAPER, 0.0f);
    /* the track is dotted so the value arc reads as the solid part */
    int dots = (int)(KNOB_SWEEP * radius / 4.0f);
    for (int i = 0; i <= dots; i++) {
        P2 p = on_circle(ce, radius, KNOB_A0 + KNOB_SWEEP * (float)i / (float)dots);
        draw_dot(c, (P2){roundf(p.x), roundf(p.y)}, PAPER);
    }
    if (t > 0.001f) {
        P2 pts[64];
        int n = 2 + (int)(KNOB_SWEEP * t * radius / 3.0f);
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++)
            pts[i] = on_circle(ce, radius, KNOB_A0 + KNOB_SWEEP * t * (float)i / (float)(n - 1));
        draw_polyline(c, pts, (size_t)n, 2.0f, PAPER);
    }
    float a = KNOB_A0 + KNOB_SWEEP * t;
    draw_line(c, on_circle(ce, radius * 0.3f, a), on_circle(ce, radius - 4.0f, a), 2.0f, PAPER);
    if (resp.hovered) draw_circle_stroke(c, ce, radius + 3.0f, 1.0f, PAPER);
    text_draw(c, vf, (P2){ce.x, r.y1}, ALIGN_CENTER_BOTTOM, value, PAPER, 0.0f);

    FaderAct act = {FADER_NONE, t};
    if (resp.hovered) ui->cursor = CURSOR_RESIZE_V;
    if (resp.double_clicked && resp.hovered) {
        /* As with a fader, the release belongs to the reset gesture. */
        if (ui->active == id) ui->active = 0;
        act.kind = FADER_RESET;
    } else if (resp.dragged && resp.drag_delta.y != 0.0f) {
        bool fine = ui->in.key_down[SDL_SCANCODE_LSHIFT]
                    || ui->in.key_down[SDL_SCANCODE_RSHIFT];
        act.kind = FADER_SET;
        act.t = clamp01(t - resp.drag_delta.y / KNOB_DRAG_PX
                                * (fine ? KNOB_FINE : 1.0f));
    }
    return act;
}

/* ---------- scope furniture ---------- */

void draw_graticule(Canvas *c, Rct r, int cols, int rows) {
    float w = rct_w(r), h = rct_h(r);
    for (int cc = 1; cc < cols; cc++) {
        float x = r.x0 + w * (float)cc / (float)cols;
        float step = (cc * 2 == cols) ? 5.0f : 9.0f;
        for (float y = r.y0; y < r.y1; y += step)
            draw_dot(c, (P2){x, y}, PAPER);
    }
    for (int rr = 1; rr < rows; rr++) {
        float y = r.y0 + h * (float)rr / (float)rows;
        float step = (rr * 2 == rows) ? 5.0f : 9.0f;
        for (float x = r.x0; x < r.x1; x += step)
            draw_dot(c, (P2){x, y}, PAPER);
    }
    float cy = 0.5f * (r.y0 + r.y1);
    for (int cc = 0; cc <= cols; cc++) {
        float x = r.x0 + w * (float)cc / (float)cols;
        draw_line(c, (P2){x, cy - 2.0f}, (P2){x, cy + 2.0f}, 1.0f, PAPER);
    }
    float cx = 0.5f * (r.x0 + r.x1);
    for (int rr = 0; rr <= rows; rr++) {
        float y = r.y0 + h * (float)rr / (float)rows;
        draw_line(c, (P2){cx - 2.0f, y}, (P2){cx + 2.0f, y}, 1.0f, PAPER);
    }
}

void beam_segment(Canvas *c, P2 a, P2 b, int k, bool decayed) {
    if (decayed) {
        if (k % 2 == 0) draw_line(c, a, b, 1.0f, PAPER);
        return;
    }
    draw_line(c, a, b, 2.0f, PAPER);
    P2 a_dn = {a.x, a.y + 2.0f}, b_dn = {b.x, b.y + 2.0f};
    P2 a_up = {a.x, a.y - 2.0f}, b_up = {b.x, b.y - 2.0f};
    if (k % 2 == 0) draw_line(c, a_dn, b_dn, 1.0f, PAPER);
    if (k % 3 == 0) draw_line(c, a_up, b_up, 1.0f, PAPER);
}

/* ---------- buttons & chrome ---------- */

void dotted_rect(Canvas *c, Rct r, uint8_t ink) {
    const float STEP = 4.0f, DOT = 2.0f;
    for (float x = r.x0; x <= r.x1 - DOT; x += STEP) {
        draw_rect_filled(c, rct_xywh(roundf(x), roundf(r.y0), DOT, DOT), ink);
        draw_rect_filled(c, rct_xywh(roundf(x), roundf(r.y1 - DOT), DOT, DOT),
                         ink);
    }
    for (float y = r.y0; y <= r.y1 - DOT; y += STEP) {
        draw_rect_filled(c, rct_xywh(roundf(r.x0), roundf(y), DOT, DOT), ink);
        draw_rect_filled(c, rct_xywh(roundf(r.x1 - DOT), roundf(y), DOT, DOT),
                         ink);
    }
}

bool pane_button(Ui *ui, UiId id, Rct r, const char *text, bool armed) {
    Resp resp = ui_interact(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    bool pressed = armed && resp.pressed_on;
    uint8_t bg = pressed ? PAPER : INK_BLACK;
    uint8_t fg = pressed ? INK_BLACK : PAPER;
    draw_rect_filled(c, r, bg);
    if (armed)
        dotted_rect(c, r, fg);
    else
        draw_rect_stroke(c, r, resp.hovered ? 2.0f : 1.0f, PAPER);
    text_draw(c, ui_font(12.0f), rct_center(r), ALIGN_CENTER_CENTER, text, fg,
              0.0f);
    return resp.clicked;
}

bool chip_button(Ui *ui, UiId id, Rct r, const char *text, bool selected) {
    Resp resp = ui_interact(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    uint8_t bg = selected ? PAPER : INK_BLACK;
    uint8_t fg = selected ? INK_BLACK : PAPER;
    draw_rect_filled(c, r, bg);
    hard_rect(c, r, resp.hovered ? 2.0f : 1.0f);
    text_draw(c, ui_font(12.0f), rct_center(r), ALIGN_CENTER_CENTER, text, fg,
              0.0f);
    return resp.clicked;
}

void inverted_strip(Canvas *c, Rct r, const char *text) {
    draw_rect_filled(c, r, PAPER);
    text_draw(c, ui_font(12.0f), (P2){r.x0 + SNUG, 0.5f * (r.y0 + r.y1)},
              ALIGN_LEFT_CENTER, text, INK_BLACK, 0.0f);
}

Rct window_chrome_tagged(Canvas *c, Rct r, const char *title, const char *tag) {
    FontId heading = ui_font(12.0f);
    float h = text_row_height(heading) + 2.0f * SNUG;
    Rct strip = rct(r.x0, r.y0, r.x1, r.y0 + h);
    draw_rect_filled(c, strip, PAPER);
    float title_w = text_width(heading, title, 0.0f);
    text_draw(c, heading, (P2){strip.x0 + SNUG, strip.y0 + SNUG},
              ALIGN_LEFT_TOP, title, INK_BLACK, 0.0f);
    if (tag) {
        char tagged[160];
        snprintf(tagged, sizeof tagged, "[%s]", tag);
        float from = strip.x0 + SNUG + title_w + GROUP;
        if (from > strip.x1) from = strip.x1;
        Rct prev = canvas_clip(c);
        canvas_set_clip(c, rct_intersect(prev,
                                         rct(from, strip.y0, strip.x1,
                                             strip.y1)));
        text_draw(c, ui_font(12.0f),
                  (P2){strip.x1 - SNUG, 0.5f * (strip.y0 + strip.y1)},
                  ALIGN_RIGHT_CENTER, tagged, INK_BLACK, 0.0f);
        canvas_set_clip(c, prev);
    }
    return rct(r.x0, strip.y1, r.x1, r.y1);
}

bool bookmark(Ui *ui, UiId id, Rct r, const char *label, bool selected) {
    const float POINT = SNUG;
    const float INSET = TIGHT;
    Resp resp = ui_interact(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    uint8_t ink = selected ? INK_BLACK : PAPER;
    int fill = selected ? PAPER : INK_BLACK;
    float w = resp.hovered ? 2.0f : 1.0f;
    FontId f = ui_font(11.0f);
    float cy = 0.5f * (r.y0 + r.y1);

    P2 outer[5] = {
        {roundf(r.x0), roundf(r.y0)},
        {roundf(r.x1 - POINT), roundf(r.y0)},
        {roundf(r.x1), roundf(cy)},
        {roundf(r.x1 - POINT), roundf(r.y1)},
        {roundf(r.x0), roundf(r.y1)},
    };
    draw_convex_poly(c, outer, 5, fill, w, PAPER);
    Rct ir = rct_shrink(r, INSET + 1.0f);
    float icy = 0.5f * (ir.y0 + ir.y1);
    P2 innerp[5] = {
        {roundf(ir.x0), roundf(ir.y0)},
        {roundf(ir.x1 - POINT), roundf(ir.y0)},
        {roundf(ir.x1), roundf(icy)},
        {roundf(ir.x1 - POINT), roundf(ir.y1)},
        {roundf(ir.x0), roundf(ir.y1)},
    };
    draw_convex_poly(c, innerp, 5, -1, 1.0f, ink);
    float th = text_row_height(f);
    text_draw(c, f, (P2){roundf(r.x0 + GROUP), roundf(cy - th * 0.5f)},
              ALIGN_LEFT_TOP, label, ink, 0.0f);
    return resp.clicked;
}

/* ---------- wave tabs ---------- */

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = *p;
    int extra = 0;
    if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    p++;
    while (extra-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}

static int utf8_count(const char *s) {
    int n = 0;
    while (*s) {
        utf8_next(&s);
        n++;
    }
    return n;
}

static bool wave_tab(Ui *ui, UiId id, Rct r, const char *text, bool active) {
    uint8_t fill = active ? PAPER : INK_BLACK;
    uint8_t ink = active ? INK_BLACK : PAPER;
    Resp resp = ui_interact(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    FontId wf = ui_font(12.0f);
    wf.px *= WAVE_SCALE;
    draw_rect_filled(c, r, fill);
    if (!active) hard_rect(c, r, 1.0f);
    float lift = ui_animate_bool(ui, id, resp.hovered, WAVE_EASE_S);
    if (lift > 0.0f) ui->repaint_soon = true;
    float t = (float)ui->time;
    int n = utf8_count(text);
    if (n < 1) n = 1;
    float text_h = text_row_height(wf);
    float x = r.x0 + SNUG;
    float y = 0.5f * (r.y0 + r.y1) - text_h * 0.5f;
    const char *s = text;
    int i = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        float phase = (float)i / (float)n * TAU_F;
        float dy = lift * WAVE_AMP * sinf(TAU_F * WAVE_RATE_HZ * t + phase);
        P2 at = {roundf(x), roundf(y + dy)};
        float adv = text_draw_glyph(c, wf, at, cp, ink);
        text_draw_glyph(c, wf, (P2){at.x + 1.0f, at.y}, cp, ink);
        x += adv + WAVE_TRACKING;
        i++;
    }
    return resp.clicked;
}

int wave_tabs(Ui *ui, UiId id, Rct r, const char *const *labels, int n,
              int active) {
    float gap = CELL_GUTTER;
    float w = tab_width(rct_w(r), gap, n);
    int hit = -1;
    float x = r.x0;
    for (int i = 0; i < n; i++) {
        UiId tid = id + (UiId)(i + 1) * 0x9E3779B97F4A7C15ULL;
        Rct tr = rct(x, r.y0, x + w, r.y1);
        if (wave_tab(ui, tid, tr, labels[i], i == active)) hit = i;
        x += w + gap;
    }
    return hit;
}

/* ---------- icons ---------- */

const char *const ICON_SAVE[9] = {
    "#########",
    "#.......#",
    "#.:::::.#",
    "#.:::::.#",
    "#.......#",
    "#.#####.#",
    "#.#...#.#",
    "#.#...#.#",
    "#########",
};
const char *const ICON_DELETE[9] = {
    "...###...",
    ".#######.",
    ".........",
    ".#######.",
    ".#:#:#:#.",
    ".#:#:#:#.",
    ".#:#:#:#.",
    ".#:#:#:#.",
    "..#####..",
};
const char *const ICON_FOLDER[9] = {
    ".........",
    "####.....",
    "#...####.",
    "#.......#",
    "#.:::::.#",
    "#.:::::.#",
    "#.:::::.#",
    "#.:::::.#",
    "#########",
};
const char *const ICON_RECORD[9] = {
    ".........",
    "..#####..",
    ".#######.",
    "#########",
    "#########",
    "#########",
    ".#######.",
    "..#####..",
    ".........",
};

void draw_icon(Canvas *c, const char *const rows[9], P2 at, uint8_t ink,
               float k) {
    for (int y = 0; y < 9; y++) {
        const char *row = rows[y];
        for (int x = 0; row[x]; x++) {
            bool lit = row[x] == '#' ||
                       (row[x] == ':' && (x + y) % 2 == 0);
            if (lit)
                draw_rect_filled(c,
                                 rct_xywh(at.x + (float)x * k,
                                          at.y + (float)y * k, k, k),
                                 ink);
        }
    }
}

bool icon_button(Ui *ui, UiId id, Rct r, const char *const rows[9],
                 const char *label, bool armed, float k) {
    Resp resp = ui_interact(ui, id, r, 0.0f);
    Canvas *c = ui->canvas;
    uint8_t fill = armed ? PAPER : INK_BLACK;
    uint8_t ink = armed ? INK_BLACK : PAPER;
    draw_rect_filled(c, r, fill);
    hard_rect(c, r, 1.0f);
    float side = 9.0f * k;
    float cy = 0.5f * (r.y0 + r.y1);
    draw_icon(c, rows, (P2){roundf(r.x0 + GAP), roundf(cy - side * 0.5f)}, ink,
              k);
    if (label) {
        FontId f = ui_font(11.0f);
        float th = text_row_height(f);
        text_draw(c, f,
                  (P2){roundf(r.x0 + GAP + side + GAP), roundf(cy - th * 0.5f)},
                  ALIGN_LEFT_TOP, label, ink, 0.0f);
    }
    return resp.clicked;
}

/* ---------- caret & chips ---------- */

void draw_block_caret(Canvas *c, Ui *ui, FontId f, P2 text_pos,
                      const char *text, uint8_t bg) {
    const double BLINK_S = 0.53;
    ui->repaint_soon = true;
    if ((int64_t)(ui->time / BLINK_S) % 2 != 0) return;
    float w = glyph_width(f, '0');
    if (w < 1.0f) w = 1.0f;
    float h = roundf(text_row_height(f));
    if (h < 9.0f) h = 9.0f;
    float x = roundf(text_pos.x + text_width(f, text, 0.0f));
    float y = roundf(text_pos.y);
    uint8_t ink = (bg == INK_BLACK) ? PAPER : INK_BLACK;
    draw_rect_filled(c, rct_xywh(x, y, roundf(w), h), ink);
}
