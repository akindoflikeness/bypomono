/* Painting across a row of step cells, shared by the sequences and the pitch
   sequencer. */
#include <math.h>

#include "app.h"

int step_at(Rct cells, int n, float x) {
    int k = (int)floorf((x - cells.x0) / (rct_w(cells) / (float)n));
    return k < 0 ? 0 : (k >= n ? n - 1 : k);
}

static float value_at(Rct cells, float y, float lo, float hi) {
    float t = clampf((cells.y1 - y) / rct_h(cells), 0.0f, 1.0f);
    return lo + (hi - lo) * t;
}

/* every cell between the last point and this one takes the height the line
   between them has there, so a fast stroke leaves no gaps */
static void stroke(float *values, int n, Rct cells, P2 from, P2 to, float lo,
                   float hi) {
    int k0 = step_at(cells, n, from.x), k1 = step_at(cells, n, to.x);
    int a = k0 < k1 ? k0 : k1, b = k0 < k1 ? k1 : k0;
    for (int k = a; k <= b; k++) {
        float t = k1 == k0 ? 1.0f : (float)(k - k0) / (float)(k1 - k0);
        values[k] = value_at(cells, from.y + (to.y - from.y) * t, lo, hi);
    }
}

bool steps_paint(App *a, Ui *ui, UiId id, Rct cells, float *values, int n,
                 float lo, float hi, float reset) {
    Resp resp = ui_interact_drag(ui, id, cells, 0.0f);
    if (resp.hovered) ui->cursor = CURSOR_POINTER;
    P2 m = ui->in.mouse;
    if (resp.double_clicked) {
        values[step_at(cells, n, m.x)] = reset;
        a->paint_id = 0;
        return true;
    }
    if (ui->in.pressed && resp.hovered) {
        a->paint_id = id;
        a->paint_prev = m;
        stroke(values, n, cells, m, m, lo, hi);
        return true;
    }
    if (a->paint_id != id) return false;
    if (!ui->in.down) {
        a->paint_id = 0;
        return false;
    }
    stroke(values, n, cells, a->paint_prev, m, lo, hi);
    a->paint_prev = m;
    return true;
}

bool steps_toggle(App *a, Ui *ui, UiId id, Rct cells, bool *values, int n) {
    Resp resp = ui_interact_drag(ui, id, cells, 0.0f);
    if (resp.hovered) ui->cursor = CURSOR_POINTER;
    int k = step_at(cells, n, ui->in.mouse.x);
    if (ui->in.pressed && resp.hovered) {
        a->paint_id = id;
        a->paint_to = !values[k];
        values[k] = a->paint_to;
        return true;
    }
    if (a->paint_id != id) return false;
    if (!ui->in.down) {
        a->paint_id = 0;
        return false;
    }
    if (values[k] == a->paint_to) return false;
    values[k] = a->paint_to;
    return true;
}
