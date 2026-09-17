#ifndef BYPO_CLI_VIEW_H
#define BYPO_CLI_VIEW_H

/* What a console line shows, independent of how it is drawn: text, and
   optionally a waveform strip. A pixel face draws the strip in dots, a
   terminal would draw it in braille. Nothing here knows about either. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRAPH_COLS 120
#define VIEW_TEXT 256
#define VIEW_LINES 40

typedef enum { GRAPH_NONE = 0, GRAPH_BELOW, GRAPH_RIGHT } GraphPlace;

/* one min/max span per column, in -127..127 (bottom..top); mark is the
   playhead column, or -1 */
typedef struct {
    uint8_t cols;
    int8_t lo[GRAPH_COLS], hi[GRAPH_COLS];
    int16_t mark;
} Graph;

typedef struct {
    char text[VIEW_TEXT];
    GraphPlace place;
    Graph graph;
} ViewLine;

typedef struct {
    ViewLine line[VIEW_LINES];
    int n;
} View;

/* f(x) for x in [0,1) gives -1..1; each column is sampled several times so
   steep edges fill their column. mark_x < 0 draws no playhead */
typedef float (*GraphFn)(float x, void *ud);
void graph_plot(Graph *g, int cols, GraphFn f, void *ud, float mark_x);

void view_clear(View *v);
/* appends a text line and returns it, or NULL when the view is full */
ViewLine *view_add(View *v, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
