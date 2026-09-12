#include "canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

Rct rct_intersect(Rct a, Rct b) {
    Rct r = {fmaxf(a.x0, b.x0), fmaxf(a.y0, b.y0), fminf(a.x1, b.x1),
             fminf(a.y1, b.y1)};
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}

static inline uint32_t gray32(uint8_t g) {
    return 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
}

void canvas_init(Canvas *c, int w, int h) {
    c->px = calloc((size_t)w * h, 4);
    c->w = w;
    c->h = h;
    c->clip = rct(0, 0, (float)w, (float)h);
}

void canvas_free(Canvas *c) {
    free(c->px);
    c->px = NULL;
}

void canvas_fill(Canvas *c, uint8_t gray) {
    uint32_t v = gray32(gray);
    for (size_t i = 0, n = (size_t)c->w * c->h; i < n; i++) c->px[i] = v;
}

void canvas_set_clip(Canvas *c, Rct clip) {
    c->clip = rct_intersect(clip, rct(0, 0, (float)c->w, (float)c->h));
}

Rct canvas_clip(const Canvas *c) { return c->clip; }

/* integer clip bounds of the current clip rect */
static void clip_i(const Canvas *c, int *x0, int *y0, int *x1, int *y1) {
    *x0 = (int)ceilf(c->clip.x0 - 0.5f);
    *y0 = (int)ceilf(c->clip.y0 - 0.5f);
    *x1 = (int)floorf(c->clip.x1 + 0.5f);
    *y1 = (int)floorf(c->clip.y1 + 0.5f);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > c->w) *x1 = c->w;
    if (*y1 > c->h) *y1 = c->h;
}

static void fill_span(Canvas *c, int x0, int x1, int y, uint32_t v) {
    if (y < 0 || y >= c->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > c->w) x1 = c->w;
    uint32_t *row = c->px + (size_t)y * c->w;
    for (int x = x0; x < x1; x++) row[x] = v;
}

void draw_rect_filled(Canvas *c, Rct r, uint8_t gray) {
    r = rct_intersect(r, c->clip);
    int x0 = (int)lroundf(r.x0), y0 = (int)lroundf(r.y0);
    int x1 = (int)lroundf(r.x1), y1 = (int)lroundf(r.y1);
    uint32_t v = gray32(gray);
    for (int y = y0; y < y1; y++) fill_span(c, x0, x1, y, v);
}

void draw_rect_stroke(Canvas *c, Rct r, float width, uint8_t gray) {
    float w = fmaxf(width, 1.0f);
    draw_rect_filled(c, rct(r.x0, r.y0, r.x1, r.y0 + w), gray);
    draw_rect_filled(c, rct(r.x0, r.y1 - w, r.x1, r.y1), gray);
    draw_rect_filled(c, rct(r.x0, r.y0 + w, r.x0 + w, r.y1 - w), gray);
    draw_rect_filled(c, rct(r.x1 - w, r.y0 + w, r.x1, r.y1 - w), gray);
}

void draw_dot(Canvas *c, P2 at, uint8_t gray) {
    int x = (int)lroundf(at.x), y = (int)lroundf(at.y);
    if (x < (int)c->clip.x0 || y < (int)c->clip.y0 || x >= (int)c->clip.x1
        || y >= (int)c->clip.y1 || x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    c->px[(size_t)y * c->w + x] = gray32(gray);
}

void draw_line(Canvas *c, P2 a, P2 b, float width, uint8_t gray) {
    /* axis-aligned lines become rects; others walk pixels (no AA anywhere) */
    if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(b.x) || !isfinite(b.y))
        return;
    float m = fmaxf(width, 1.0f);
    Rct cl = rct_expand(c->clip, m);
    if ((a.x < cl.x0 && b.x < cl.x0) || (a.x > cl.x1 && b.x > cl.x1)
        || (a.y < cl.y0 && b.y < cl.y0) || (a.y > cl.y1 && b.y > cl.y1))
        return;
    float dx = b.x - a.x, dy = b.y - a.y;
    float hw = fmaxf(width, 1.0f) * 0.5f;
    if (fabsf(dx) < 0.01f) {
        draw_rect_filled(
            c, rct(a.x - hw, fminf(a.y, b.y), a.x + hw, fmaxf(a.y, b.y)), gray);
        return;
    }
    if (fabsf(dy) < 0.01f) {
        draw_rect_filled(
            c, rct(fminf(a.x, b.x), a.y - hw, fmaxf(a.x, b.x), a.y + hw), gray);
        return;
    }
    /* Liang-Barsky clip to the (expanded) clip rect so huge segments
       only walk their visible span */
    float t0 = 0.0f, t1 = 1.0f;
    float p_[4] = {-dx, dx, -dy, dy};
    float q_[4] = {a.x - cl.x0, cl.x1 - a.x, a.y - cl.y0, cl.y1 - a.y};
    for (int e = 0; e < 4; e++) {
        if (p_[e] == 0.0f) {
            if (q_[e] < 0.0f) return;
        } else {
            float t = q_[e] / p_[e];
            if (p_[e] < 0.0f) {
                if (t > t1) return;
                if (t > t0) t0 = t;
            } else {
                if (t < t0) return;
                if (t < t1) t1 = t;
            }
        }
    }
    P2 ca = {a.x + dx * t0, a.y + dy * t0};
    float cdx = dx * (t1 - t0), cdy = dy * (t1 - t0);
    float len = sqrtf(cdx * cdx + cdy * cdy);
    int steps = (int)ceilf(len);
    if (steps > 4096) steps = 4096; /* clipped span can never need more */
    int iw = (int)lroundf(fmaxf(width, 1.0f));
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)(steps > 0 ? steps : 1);
        P2 p = {ca.x + cdx * t, ca.y + cdy * t};
        if (iw <= 1)
            draw_dot(c, p, gray);
        else
            draw_rect_filled(
                c, rct(p.x - hw, p.y - hw, p.x - hw + iw, p.y - hw + iw), gray);
    }
}

void draw_polyline(Canvas *c, const P2 *pts, size_t n, float width, uint8_t gray) {
    for (size_t i = 0; i + 1 < n; i++)
        draw_line(c, pts[i], pts[i + 1], width, gray);
}

void draw_circle_filled(Canvas *c, P2 center, float radius, uint8_t gray) {
    int x0, y0, x1, y1;
    clip_i(c, &x0, &y0, &x1, &y1);
    int cy0 = (int)floorf(center.y - radius), cy1 = (int)ceilf(center.y + radius);
    if (cy0 > y0) y0 = cy0;
    if (cy1 + 1 < y1) y1 = cy1 + 1;
    uint32_t v = gray32(gray);
    float r2 = radius * radius;
    for (int y = y0; y < y1; y++) {
        float fy = (float)y + 0.5f - center.y;
        float span = r2 - fy * fy;
        if (span <= 0.0f) continue;
        float half = sqrtf(span);
        int sx0 = (int)lroundf(center.x - half), sx1 = (int)lroundf(center.x + half);
        if (sx0 < x0) sx0 = x0;
        if (sx1 > x1) sx1 = x1;
        fill_span(c, sx0, sx1, y, v);
    }
}

void draw_circle_stroke(Canvas *c, P2 center, float radius, float width,
                        uint8_t gray) {
    int steps = (int)fmaxf(8.0f, radius * 6.2831853f);
    P2 prev = {center.x + radius, center.y};
    for (int i = 1; i <= steps; i++) {
        float a = 6.2831853f * (float)i / (float)steps;
        P2 p = {center.x + cosf(a) * radius, center.y + sinf(a) * radius};
        draw_line(c, prev, p, width, gray);
        prev = p;
    }
}

void draw_convex_poly(Canvas *c, const P2 *pts, size_t n, int fill_gray,
                      float stroke_w, int stroke_gray) {
    if (n < 3) return;
    if (fill_gray >= 0) {
        float ymin = pts[0].y, ymax = pts[0].y;
        for (size_t i = 1; i < n; i++) {
            ymin = fminf(ymin, pts[i].y);
            ymax = fmaxf(ymax, pts[i].y);
        }
        int y0 = (int)lroundf(ymin), y1 = (int)lroundf(ymax);
        uint32_t v = gray32((uint8_t)fill_gray);
        for (int y = y0; y < y1; y++) {
            float fy = (float)y + 0.5f;
            float xa = 1e9f, xb = -1e9f;
            for (size_t i = 0; i < n; i++) {
                P2 p = pts[i], q = pts[(i + 1) % n];
                if ((p.y <= fy && q.y > fy) || (q.y <= fy && p.y > fy)) {
                    float x = p.x + (q.x - p.x) * (fy - p.y) / (q.y - p.y);
                    xa = fminf(xa, x);
                    xb = fmaxf(xb, x);
                }
            }
            if (xb > xa) {
                int sx0 = (int)lroundf(fmaxf(xa, c->clip.x0));
                int sx1 = (int)lroundf(fminf(xb, c->clip.x1));
                if (y >= (int)c->clip.y0 && y < (int)c->clip.y1)
                    fill_span(c, sx0, sx1, y, v);
            }
        }
    }
    if (stroke_gray >= 0)
        for (size_t i = 0; i < n; i++)
            draw_line(c, pts[i], pts[(i + 1) % n], stroke_w,
                      (uint8_t)stroke_gray);
}

static const uint8_t BAYER4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void dither_cells(Canvas *c, Rct r, float density, float cell,
                         uint8_t ink, bool circle_mask, P2 center, float radius) {
    if (density <= 0.0f) return;
    if (density >= 1.0f && !circle_mask) {
        draw_rect_filled(c, r, ink);
        return;
    }
    if (density >= 1.0f && circle_mask) {
        draw_circle_filled(c, center, radius, ink);
        return;
    }
    float step = fmaxf(cell, 1.0f);
    float side = (ink == PAPER) ? step - 1.0f : step;
    if (side < 1.0f) side = 1.0f;
    int level = (int)lroundf(density * 16.0f);
    float r2 = radius * radius;
    int col0 = (int)floorf(r.x0 / step), col1 = (int)ceilf(r.x1 / step);
    int row0 = (int)floorf(r.y0 / step), row1 = (int)ceilf(r.y1 / step);
    for (int row = row0; row < row1; row++) {
        for (int col = col0; col < col1; col++) {
            if (BAYER4[row & 3][col & 3] >= level) continue;
            float x = (float)col * step, y = (float)row * step;
            if (x + side <= r.x0 || x >= r.x1 || y + side <= r.y0 || y >= r.y1)
                continue;
            if (circle_mask) {
                float cx = x + side * 0.5f - center.x;
                float cy = y + side * 0.5f - center.y;
                if (cx * cx + cy * cy > r2) continue;
            }
            Rct cellr = rct(fmaxf(x, r.x0), fmaxf(y, r.y0),
                            fminf(x + side, r.x1), fminf(y + side, r.y1));
            draw_rect_filled(c, cellr, ink);
        }
    }
}

void dither_rect_ink(Canvas *c, Rct r, float density, float cell, uint8_t ink) {
    dither_cells(c, r, density, cell, ink, false, (P2){0, 0}, 0.0f);
}

void dither_rect(Canvas *c, Rct r, float density, float cell) {
    dither_rect_ink(c, r, density, cell, PAPER);
}

void dither_circle_ink(Canvas *c, P2 center, float radius, float density,
                       float cell, uint8_t ink) {
    Rct r = rct(center.x - radius, center.y - radius, center.x + radius,
                center.y + radius);
    dither_cells(c, r, density, cell, ink, true, center, radius);
}

void dither_circle(Canvas *c, P2 center, float radius, float density, float cell) {
    dither_circle_ink(c, center, radius, density, cell, PAPER);
}

void canvas_blit_a8(Canvas *c, int x, int y, const uint8_t *a8, int aw, int ah,
                    int pitch, uint8_t ink) {
    int cx0, cy0, cx1, cy1;
    clip_i(c, &cx0, &cy0, &cx1, &cy1);
    for (int j = 0; j < ah; j++) {
        int py = y + j;
        if (py < cy0 || py >= cy1) continue;
        const uint8_t *src = a8 + (size_t)j * pitch;
        uint32_t *row = c->px + (size_t)py * c->w;
        for (int i = 0; i < aw; i++) {
            int px = x + i;
            if (px < cx0 || px >= cx1) continue;
            uint8_t a = src[i];
            if (a == 0) continue;
            uint8_t d = row[px] & 0xFF;
            uint8_t g = (uint8_t)((ink * a + d * (255 - a)) / 255);
            row[px] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
        }
    }
}
