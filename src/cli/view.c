#include "view.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SUBSAMPLES 6

static int8_t level(float v) {
    if (isnan(v)) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int8_t)lroundf(v * 127.0f);
}

void graph_plot(Graph *g, int cols, GraphFn f, void *ud, float mark_x) {
    if (cols < 1) cols = 1;
    if (cols > GRAPH_COLS) cols = GRAPH_COLS;
    g->cols = (uint8_t)cols;
    float prev = f(0.0f, ud);
    for (int c = 0; c < cols; c++) {
        /* start from the last sample of the previous column, so a jump
           between columns draws as a joined edge instead of a gap */
        float lo = prev, hi = prev;
        for (int k = 0; k <= SUBSAMPLES; k++) {
            float x = ((float)c + (float)k / SUBSAMPLES) / (float)cols;
            if (x >= 1.0f) x = 1.0f - 1e-6f;
            float v = f(x, ud);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            prev = v;
        }
        g->lo[c] = level(lo);
        g->hi[c] = level(hi);
    }
    g->mark = mark_x < 0.0f ? -1
                            : (int16_t)(fmodf(mark_x, 1.0f) * (float)cols);
    if (g->mark >= cols) g->mark = (int16_t)(cols - 1);
}

void graph_from_samples(Graph *g, const float *v, int n, int cols,
                        float scale) {
    if (cols < 1) cols = 1;
    if (cols > GRAPH_COLS) cols = GRAPH_COLS;
    g->cols = (uint8_t)cols;
    g->mark = -1;
    if (n <= 0) {
        memset(g->lo, 0, (size_t)cols);
        memset(g->hi, 0, (size_t)cols);
        return;
    }
    for (int c = 0; c < cols; c++) {
        int from = n * c / cols, to = n * (c + 1) / cols;
        if (to <= from) to = from + 1;
        if (to > n) to = n;
        /* the sample before this column, so a step draws as one riser and
           does not smear along the rest of the strip */
        float lo = v[from > 0 ? from - 1 : from] * scale, hi = lo;
        for (int i = from; i < to; i++) {
            float x = v[i] * scale;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        g->lo[c] = level(lo);
        g->hi[c] = level(hi);
    }
}

void view_clear(View *v) { v->n = 0; }

ViewLine *view_add(View *v, const char *fmt, ...) {
    if (v->n >= VIEW_LINES) return NULL;
    ViewLine *l = &v->line[v->n++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof l->text, fmt, ap);
    va_end(ap);
    l->place = GRAPH_NONE;
    return l;
}
