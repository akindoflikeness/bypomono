#ifndef BYPO_CANVAS_H
#define BYPO_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the two inks */
#define PAPER 227
#define INK_BLACK 0

typedef struct { float x, y; } P2;
typedef struct { float x0, y0, x1, y1; } Rct;

static inline Rct rct(float x0, float y0, float x1, float y1) {
    return (Rct){x0, y0, x1, y1};
}
static inline Rct rct_xywh(float x, float y, float w, float h) {
    return (Rct){x, y, x + w, y + h};
}
static inline float rct_w(Rct r) { return r.x1 - r.x0; }
static inline float rct_h(Rct r) { return r.y1 - r.y0; }
static inline P2 rct_center(Rct r) {
    return (P2){0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)};
}
static inline bool rct_contains(Rct r, P2 p) {
    return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1;
}
static inline Rct rct_shrink(Rct r, float d) {
    return (Rct){r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d};
}
static inline Rct rct_expand(Rct r, float d) { return rct_shrink(r, -d); }
Rct rct_intersect(Rct a, Rct b);

/* software framebuffer, XRGB8888, design-space pixels */
typedef struct {
    uint32_t *px;
    int w, h;
    Rct clip;
} Canvas;

void canvas_init(Canvas *c, int w, int h);
void canvas_free(Canvas *c);
void canvas_fill(Canvas *c, uint8_t gray);
/* clip is absolute; callers save/restore the previous value themselves */
void canvas_set_clip(Canvas *c, Rct clip);
Rct canvas_clip(const Canvas *c);

void draw_rect_filled(Canvas *c, Rct r, uint8_t gray);
/* stroke inside the rect, like egui StrokeKind::Inside */
void draw_rect_stroke(Canvas *c, Rct r, float width, uint8_t gray);
void draw_line(Canvas *c, P2 a, P2 b, float width, uint8_t gray);
void draw_polyline(Canvas *c, const P2 *pts, size_t n, float width, uint8_t gray);
void draw_circle_filled(Canvas *c, P2 center, float radius, uint8_t gray);
void draw_circle_stroke(Canvas *c, P2 center, float radius, float width, uint8_t gray);
/* filled convex polygon + its outline (bookmark tabs) */
void draw_convex_poly(Canvas *c, const P2 *pts, size_t n, int fill_gray,
                      float stroke_w, int stroke_gray); /* gray<0 = skip */
void draw_dot(Canvas *c, P2 at, uint8_t gray); /* 1x1 px */

/* 4x4 Bayer ordered dither; cell px per dither cell.
   ink PAPER uses cell-1 leaving a hairline gap, matching the Rust. */
void dither_rect(Canvas *c, Rct r, float density, float cell);
void dither_rect_ink(Canvas *c, Rct r, float density, float cell, uint8_t ink);
void dither_circle(Canvas *c, P2 center, float radius, float density, float cell);
void dither_circle_ink(Canvas *c, P2 center, float radius, float density,
                       float cell, uint8_t ink);

/* blend an 8-bit coverage bitmap in `ink` over the canvas (glyphs) */
void canvas_blit_a8(Canvas *c, int x, int y, const uint8_t *a8, int aw, int ah,
                    int pitch, uint8_t ink);

#endif
