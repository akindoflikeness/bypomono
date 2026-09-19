#include <float.h>
#include <math.h>

#include "app.h"

#define FADER_ROW_EXTRA 6.0f

static bool fad_lin(Ui *ui, UiId id, Rct r, const char *label, float *v,
                    float lo, float hi, float def, const char *val) {
    float span = fabsf(hi - lo);
    if (span < FLT_EPSILON) span = FLT_EPSILON;
    float t = clampf((*v - lo) / span, 0.0f, 1.0f);
    FaderAct act = fader_track(ui, id, r, label, val, t);
    float next = *v;
    if (act.kind == FADER_SET) next = lo + act.t * span;
    else if (act.kind == FADER_RESET) next = def;
    else return false;
    if (next != *v) {
        *v = next;
        return true;
    }
    return false;
}

static bool fad_log(Ui *ui, UiId id, Rct r, const char *label, float *v,
                    float lo, float hi, float def, const char *val) {
    float t = position_of_log(*v, lo, hi);
    FaderAct act = fader_track(ui, id, r, label, val, t);
    float next = *v;
    if (act.kind == FADER_SET) next = log_position(act.t, lo, hi);
    else if (act.kind == FADER_RESET) next = def;
    else return false;
    if (next != *v) {
        *v = next;
        return true;
    }
    return false;
}

static bool fad_int(Ui *ui, UiId id, Rct r, const char *label, int *v, int lo,
                    int hi, int def, const char *val) {
    float span = (float)(hi - lo);
    if (span < 1.0f) span = 1.0f;
    float t = clampf((float)(*v - lo) / span, 0.0f, 1.0f);
    FaderAct act = fader_track(ui, id, r, label, val, t);
    int next = *v;
    if (act.kind == FADER_SET) {
        next = (int)roundf((float)lo + act.t * span);
        next = next < lo ? lo : (next > hi ? hi : next);
    } else if (act.kind == FADER_RESET) {
        next = def < lo ? lo : (def > hi ? hi : def);
    } else {
        return false;
    }
    if (next != *v) {
        *v = next;
        return true;
    }
    return false;
}

static float chip_h(float text_h) {
    float h = text_h + 2.0f * SNUG;
    return h < 21.0f ? 21.0f : h;
}

static void send_melody(App *a) {
    Event ev = {.kind = EV_SET_MELODY};
    ev.u.melody = a->shadow_melody;
    app_send(a, ev);
}

static void send_patch(App *a) {
    Event ev = {.kind = EV_SET_PATCH};
    ev.u.patch = a->shadow;
    app_send(a, ev);
}

static void send_verb(App *a) {
    Event ev = {.kind = EV_SET_VERB};
    ev.u.verb = a->shadow_verb;
    app_send(a, ev);
}

static void send_chandas(App *a) {
    Event ev = {.kind = EV_SET_CHANDAS};
    ev.u.chandas = a->shadow_chandas;
    app_send(a, ev);
}

static void send_warmth(App *a) {
    Event ev = {.kind = EV_SET_WARMTH};
    ev.u.f = a->shadow_warmth;
    app_send(a, ev);
}

static int changed_node(int ai, int bi) {
    AlgorithmId A = ALGORITHMS[ai], B = ALGORITHMS[bi];
    if (algorithm_distance(A, B) != 1) return -1;
    for (int k = 0; k < NUM_NODES; k++)
        if (algorithm_node(A, k) != algorithm_node(B, k)) return k;
    return -1;
}

static void tree_section(App *a, Ui *ui, Rct rect) {
    Canvas *c = ui->canvas;
    Resp resp = ui_interact(ui, ui_id("lrail.tree"), rect, 0.0f);
    float w = rct_w(rect), h = rct_h(rect);
    P2 nodes[NUM_NODES] = {
        {rect.x0 + 0.50f * w, rect.y0 + 0.10f * h},
        {rect.x0 + 0.30f * w, rect.y0 + 0.34f * h},
        {rect.x0 + 0.16f * w, rect.y0 + 0.58f * h},
        {rect.x0 + 0.74f * w, rect.y0 + 0.34f * h},
    };
    P2 leaves[NUM_OPS] = {
        {rect.x0 + 0.08f * w, rect.y0 + 0.86f * h},
        {rect.x0 + 0.26f * w, rect.y0 + 0.86f * h},
        {rect.x0 + 0.44f * w, rect.y0 + 0.62f * h},
        {rect.x0 + 0.64f * w, rect.y0 + 0.64f * h},
        {rect.x0 + 0.86f * w, rect.y0 + 0.64f * h},
    };
    struct { P2 from, to; float m_to; } edges[8] = {
        {nodes[0], nodes[1], 11.0f}, {nodes[0], nodes[3], 11.0f},
        {nodes[1], nodes[2], 11.0f}, {nodes[1], leaves[2], 9.0f},
        {nodes[2], leaves[0], 9.0f}, {nodes[2], leaves[1], 9.0f},
        {nodes[3], leaves[3], 9.0f}, {nodes[3], leaves[4], 9.0f},
    };
    for (int i = 0; i < 8; i++) {
        float dx = edges[i].to.x - edges[i].from.x;
        float dy = edges[i].to.y - edges[i].from.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        dx /= len;
        dy /= len;
        P2 ea = {edges[i].from.x + dx * 11.0f, edges[i].from.y + dy * 11.0f};
        P2 eb = {edges[i].to.x - dx * edges[i].m_to,
                 edges[i].to.y - dy * edges[i].m_to};
        bubble_chain(c, ui, ea, eb, true, a->shadow.index, ui->time);
    }

    int idx = algorithm_index_of(&a->shadow);
    int next_flip = idx + 1 < 8 ? changed_node(idx, idx + 1) : -1;
    int prev_flip = idx > 0 ? changed_node(idx, idx - 1) : -1;
    int fb_op = compile(a->shadow.algorithm).feedback_op;
    bool blink = (a->frame_count / 24) % 2 == 0;

    for (int node = 0; node < NUM_NODES; node++) {
        Combine choice = algorithm_node(a->shadow.algorithm, node);
        bool live = node == next_flip || node == prev_flip;
        float r = live ? 10.0f : 8.0f;
        draw_circle_filled(c, nodes[node], r, INK_BLACK);
        draw_circle_stroke(c, nodes[node], r, live && blink ? 2.0f : 1.0f,
                           PAPER);
        if (choice == COMBINE_FEEDBACK)
            draw_circle_stroke(c, nodes[node], r + 3.0f, 1.0f, PAPER);
        char glyph[2] = {combine_glyph(choice), '\0'};
        text_draw(c, ui_font(11.0f), nodes[node], ALIGN_CENTER_CENTER, glyph,
                  PAPER, 0.0f);
    }
    for (int i = 0; i < NUM_OPS; i++) {
        draw_circle_stroke(c, leaves[i], 7.0f, 1.0f, PAPER);
        if (i == fb_op)
            draw_circle_stroke(c, leaves[i], 10.0f, 1.0f, PAPER);
        dither_circle(c, leaves[i], 5.5f, fminf(a->env[i] * 1.6f, 1.0f), 2.0f);
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        text_draw(c, ui_font(10.0f), (P2){leaves[i].x, leaves[i].y + 13.0f},
                  ALIGN_CENTER_CENTER, num, PAPER, 0.0f);
    }
    if (resp.clicked) {
        for (int node = 0; node < NUM_NODES; node++) {
            float dx = resp.pointer.x - nodes[node].x;
            float dy = resp.pointer.y - nodes[node].y;
            if (sqrtf(dx * dx + dy * dy) < 12.0f) {
                if (node == next_flip) {
                    app_set_algorithm(a, idx + 1);
                } else if (node == prev_flip) {
                    app_set_algorithm(a, idx - 1);
                } else {
                    push_log(a, "node dormant. off-roster structures wake at "
                                "roster size 13.");
                }
                break;
            }
        }
    }
}

static void pentagram(App *a, Ui *ui, Rct rect) {
    Canvas *c = ui->canvas;
    P2 center = rct_center(rect);
    float radius = fminf(rct_w(rect), rct_h(rect)) * 0.40f;
    float haunt = a->shadow_verb.haunt;
    P2 pts[5];
    for (int i = 0; i < 5; i++) {
        float ang = -PI_F / 2.0f + TAU_F * (float)i / 5.0f;
        pts[i] = (P2){center.x + cosf(ang) * radius,
                      center.y + sinf(ang) * radius};
    }
    int chords = (int)(haunt * 21.0f);
    for (int k = 1; k <= chords; k++) {
        float ta = (float)k / 21.0f;
        float tb = (float)(k * 2 % 21) / 21.0f;
        float aa = -PI_F / 2.0f + TAU_F * ta;
        float ab = -PI_F / 2.0f + TAU_F * tb;
        P2 pa = {center.x + cosf(aa) * radius, center.y + sinf(aa) * radius};
        P2 pb = {center.x + cosf(ab) * radius, center.y + sinf(ab) * radius};
        draw_line(c, pa, pb, 1.0f, PAPER);
    }
    for (int i = 0; i < 5; i++) {
        P2 from = pts[i], to = pts[(i + 3) % 5];
        float dx = to.x - from.x, dy = to.y - from.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        dx /= len;
        dy /= len;
        bubble_chain(c, ui, (P2){from.x + dx * 9.0f, from.y + dy * 9.0f},
                     (P2){to.x - dx * 9.0f, to.y - dy * 9.0f}, haunt > 0.0f,
                     a->shadow.index, ui->time);
    }
    for (int i = 0; i < 5; i++) {
        draw_circle_filled(c, pts[i], 7.0f, INK_BLACK);
        draw_circle_stroke(c, pts[i], 7.0f, 1.0f, PAPER);
        dither_circle(c, pts[i], 5.5f, fminf(a->env[i] * haunt * 2.0f, 1.0f),
                      2.0f);
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        text_draw(c, ui_font(10.0f), pts[i], ALIGN_CENTER_CENTER, num, PAPER,
                  0.0f);
    }
}

static void warmth_fader(App *a, Ui *ui, float x, float w, float *py) {
    *py += GROUP;
    char val[32];
    snprintf(val, sizeof val, "%.2f", a->shadow_warmth);
    float warmth = a->shadow_warmth;
    if (fad_lin(ui, ui_id("rrail.warmth"), rct_xywh(x, *py, w, FADER_H),
                "warmth", &warmth, MIN_WARMTH, MAX_WARMTH, 0.0f, val)) {
        a->shadow_warmth = warmth;
        send_warmth(a);
    }
    *py += FADER_H;
    *py += FADER_ROW_EXTRA;
}

static void chandas_section(App *a, Ui *ui, float x, float w, float *py) {
    Canvas *c = ui->canvas;
    float y = *py;
    FontId body = ui_font(12.0f);
    float th = text_row_height(body);
    ChandasParams h = a->shadow_chandas;
    ChandasParams hd = chandas_params_default();
    bool changed = false;
    bool was_enabled = h.enabled;
    h.enabled = h.mix > 0.0f;
    changed |= h.enabled != was_enabled;
    (void)c;

    {
        float bw = text_width(body, "SYNC", 0.0f) + 2.0f * GAP;
        if (bw < 60.0f) bw = 60.0f;
        float bh = chip_h(th);
        if (chip_button(ui, ui_id("chandas.sync"), rct_xywh(x, y, bw, bh),
                        "SYNC", h.sync)) {
            h.sync = !h.sync;
            changed = true;
        }
        y += bh + GROUP;
    }
    if (h.sync) {
        int d = (int)h.division;
        if (d > CHANDAS_DIVISIONS_LEN - 1) d = CHANDAS_DIVISIONS_LEN - 1;
        const char *val = CHANDAS_DIVISIONS[d < 0 ? 0 : d].name;
        if (fad_int(ui, ui_id("chandas.time"), rct_xywh(x, y, w, FADER_H),
                    "time", &d, 0, CHANDAS_DIVISIONS_LEN - 1,
                    (int)hd.division, val)) {
            h.division = (size_t)d;
            changed = true;
        }
    } else {
        char val[32];
        snprintf(val, sizeof val, "%.2f", h.rate_hz);
        changed |= fad_log(ui, ui_id("chandas.rate"),
                           rct_xywh(x, y, w, FADER_H), "rate hz", &h.rate_hz,
                           0.1f, 8.0f, hd.rate_hz, val);
    }
    y += FADER_H + GROUP;
    y += FADER_ROW_EXTRA;

    char val[32];
    snprintf(val, sizeof val, "%.2f", h.mix);
    changed |= fad_lin(ui, ui_id("chandas.mix"), rct_xywh(x, y, w, FADER_H),
                       "mix", &h.mix, 0.0f, 1.0f, hd.mix, val);
    y += FADER_H + GROUP;
    snprintf(val, sizeof val, "%.2f", h.spread);
    changed |= fad_lin(ui, ui_id("chandas.spread"), rct_xywh(x, y, w, FADER_H),
                       "spread", &h.spread, 0.0f, 1.0f, hd.spread, val);
    y += FADER_H + GROUP;
    snprintf(val, sizeof val, "%.2fx", h.size);
    changed |= fad_lin(ui, ui_id("chandas.size"), rct_xywh(x, y, w, FADER_H),
                       "size", &h.size, CHANDAS_MIN_SIZE, CHANDAS_MAX_SIZE,
                       hd.size, val);
    y += FADER_H + GROUP;
    snprintf(val, sizeof val, "%.2f", h.warp);
    changed |= fad_lin(ui, ui_id("chandas.warp"), rct_xywh(x, y, w, FADER_H),
                       "warp", &h.warp, 0.0f, 1.0f, hd.warp, val);
    y += FADER_H + GROUP;
    snprintf(val, sizeof val, "%.2f", h.dimension);
    changed |= fad_lin(ui, ui_id("chandas.dimension"),
                       rct_xywh(x, y, w, FADER_H), "dimension", &h.dimension,
                       0.0f, 1.0f, hd.dimension, val);
    y += FADER_H + GROUP;
    snprintf(val, sizeof val, "%.2f", h.tail);
    changed |= fad_lin(ui, ui_id("chandas.tail"), rct_xywh(x, y, w, FADER_H),
                       "tail", &h.tail, 0.0f, 1.0f, hd.tail, val);
    y += FADER_H + GROUP;

    if (changed) {
        a->shadow_chandas = h;
        send_chandas(a);
    }
    *py = y;
}

void draw_left_rail(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    draw_rect_filled(c, r, INK_BLACK);
    Rct inner = rct_shrink(r, GROUP);
    Rct prev_clip = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(prev_clip, inner));

    static float content_h = 0.0f;
    float off = ui_scroll(ui, &a->left_scroll, inner, content_h);
    float x = inner.x0, w = rct_w(inner);
    float y = inner.y0 - off;
    float y_start = y;

    FontId body = ui_font(12.0f);
    float th = text_row_height(body);
    float strip_h = th + 2.0f * SNUG;

    inverted_strip(c, rct_xywh(x, y, w, strip_h), "THE TREE");
    y += strip_h + GROUP;
    tree_section(a, ui, rct_xywh(x, y, w, TREE_H));
    y += TREE_H + GROUP;
    y += GROUP;
    inverted_strip(c, rct_xywh(x, y, w, strip_h), "MELODY");
    y += strip_h + GROUP;

    bool driving = midi_driving(a);
    MelodyParams *m = &a->shadow_melody;
    bool changed = false, midi_click = false;

    {
        const char *lbl = m->enabled ? "S&H ON" : "S&H OFF";
        float bw = text_width(body, lbl, 0.0f) + 2.0f * GAP;
        if (bw < 70.0f) bw = 70.0f;
        float bh = chip_h(th);
        Rct br = rct_xywh(x, y, bw, bh);
        bool clicked = chip_button(ui, ui_id("lrail.sh"), br, lbl, m->enabled);
        if (driving) {
            dither_rect_ink(c, br, VEIL, 2.0f, INK_BLACK);
            midi_click |= clicked;
        } else if (clicked) {
            m->enabled = !m->enabled;
            changed = true;
        }
        y += bh + GROUP;
    }
    {
        float bx = x;
        float bh = chip_h(th);
        for (int s = 0; s < 2; s++) {
            const char *name = s == HOLD_GOLDEN_WEYL ? "golden" : "random";
            bool active = (int)m->source == s;
            float bw = text_width(body, name, 0.0f) + 2.0f * GAP;
            Rct br = rct_xywh(bx, y, bw, bh);
            bool clicked =
                chip_button(ui, ui_id_n("lrail.src", s), br, name, active);
            if (driving) {
                dither_rect_ink(c, br, VEIL, 2.0f, INK_BLACK);
                midi_click |= clicked;
            } else if (clicked && !active) {
                m->source = (HoldSource)s;
                changed = true;
            }
            bx += bw + GROUP;
        }
        y += bh + GROUP;
    }
    {
        float bx = x;
        float bh = chip_h(th);
        for (int t = 0; t < NUM_TUNINGS; t++) {
            const char *name = tuning_name((Tuning)t);
            bool active = (int)m->tuning == t;
            float bw = text_width(body, name, 0.0f) + 2.0f * GAP;
            if (bx > x && bx + bw > x + w) {
                bx = x;
                y += bh + GROUP;
            }
            Rct br = rct_xywh(bx, y, bw, bh);
            bool clicked =
                chip_button(ui, ui_id_n("lrail.tuning", t), br, name, active);
            if (driving) {
                dither_rect_ink(c, br, VEIL, 2.0f, INK_BLACK);
                midi_click |= clicked;
            } else if (clicked && !active) {
                m->tuning = (Tuning)t;
                changed = true;
            }
            bx += bw + GROUP;
        }
        y += bh + GROUP;
    }
    {
        Quantization q = tuning_quantization(m->tuning);
        bool snapping = driving || (m->enabled && quantization_is_quantized(q));
        const char *word = snapping ? "QUANTISED" : "FREE";
        const char *symbol = driving ? "12" : (m->enabled ? q.symbol : "");
        float row_h = th + 2.0f;
        float bx = x;
        text_draw(c, body, (P2){bx, y + row_h * 0.5f}, ALIGN_LEFT_CENTER,
                  "PITCH", PAPER, 0.0f);
        bx += text_width(body, "PITCH", 0.0f) + GROUP;
        float fw = text_width(body, word, 0.0f) + 2.0f * SNUG;
        Rct fr = rct_xywh(bx, y, fw, row_h);
        draw_rect_filled(c, fr, snapping ? PAPER : INK_BLACK);
        draw_rect_stroke(c, fr, 1.0f, PAPER);
        text_draw(c, body, rct_center(fr), ALIGN_CENTER_CENTER, word,
                  snapping ? INK_BLACK : PAPER, 0.0f);
        bx += fw + GROUP;
        if (symbol && symbol[0])
            text_draw(c, body, (P2){bx, y + row_h * 0.5f}, ALIGN_LEFT_CENTER,
                      symbol, PAPER, 0.0f);
        y += row_h + GROUP;
    }
    {
        float bx = x;
        float bh = chip_h(th);
        for (int s = 0; s < NUM_SCALES; s++) {
            const char *name = scale_name((Scale)s);
            bool active = m->tuning == TUNING_SCALE && (int)m->scale == s;
            float bw = text_width(body, name, 0.0f) + 2.0f * GAP;
            if (bx > x && bx + bw > x + w) {
                bx = x;
                y += bh + GROUP;
            }
            Rct br = rct_xywh(bx, y, bw, bh);
            bool clicked =
                chip_button(ui, ui_id_n("lrail.scale", s), br, name, active);
            if (driving) {
                dither_rect_ink(c, br, VEIL, 2.0f, INK_BLACK);
                midi_click |= clicked;
            } else if (clicked && !active) {
                m->scale = (Scale)s;
                m->tuning = TUNING_SCALE;
                changed = true;
            }
            bx += bw + GROUP;
        }
        y += bh + GROUP;
    }

    MelodyParams md = melody_params_default();
    {
        char val[32];
        snprintf(val, sizeof val, "%.2f", m->rate_hz);
        Rct fr = rct_xywh(x, y, w, FADER_H);
        if (driving) {
            FaderAct act =
                fader_track(ui, ui_id("lrail.rate"), fr, "rate hz", val,
                            position_of_log(m->rate_hz, 0.1f, 8.0f));
            dither_rect_ink(c, fr, VEIL, 2.0f, INK_BLACK);
            midi_click |= act.kind != FADER_NONE;
        } else {
            changed |= fad_log(ui, ui_id("lrail.rate"), fr, "rate hz",
                               &m->rate_hz, 0.1f, 8.0f, md.rate_hz, val);
        }
        y += FADER_H + GROUP;
    }
    {
        char val[32];
        int range = m->range_degrees;
        snprintf(val, sizeof val, "%d", range);
        Rct fr = rct_xywh(x, y, w, FADER_H);
        if (driving) {
            FaderAct act =
                fader_track(ui, ui_id("lrail.range"), fr, "range", val,
                            clampf((float)(range - 1) / 12.0f, 0.0f, 1.0f));
            dither_rect_ink(c, fr, VEIL, 2.0f, INK_BLACK);
            midi_click |= act.kind != FADER_NONE;
        } else if (fad_int(ui, ui_id("lrail.range"), fr, "range", &range, 1,
                           13, md.range_degrees, val)) {
            m->range_degrees = (uint8_t)range;
            changed = true;
        }
        y += FADER_H + GROUP;
    }
    {
        char val[32];
        int root = m->root_midi;
        snprintf(val, sizeof val, "%d", root);
        Rct fr = rct_xywh(x, y, w, FADER_H);
        if (driving) {
            FaderAct act =
                fader_track(ui, ui_id("lrail.root"), fr, "root", val,
                            clampf((float)(root - 24) / 33.0f, 0.0f, 1.0f));
            dither_rect_ink(c, fr, VEIL, 2.0f, INK_BLACK);
            midi_click |= act.kind != FADER_NONE;
        } else if (fad_int(ui, ui_id("lrail.root"), fr, "root", &root, 24, 57,
                           md.root_midi, val)) {
            m->root_midi = (uint8_t)root;
            changed = true;
        }
        y += FADER_H + GROUP;
    }

    if (midi_click)
        push_log(a, "midi is connected, so the instrument is configured for "
                    "midi — the melody sequencer is idle until it's "
                    "unplugged.");
    if (changed) send_melody(a);

    y += CELL_GUTTER;
    y += GROUP;
    {
        bool on = a->engaged;
        Rct br = rct_xywh(x, y, w / 3.0f, ENGAGE_H);
        Resp resp = ui_interact(ui, ui_id("lrail.drone"), br, 0.0f);
        draw_rect_filled(c, br, on ? PAPER : INK_BLACK);
        draw_rect_stroke(c, br, 1.0f, PAPER);
        text_draw(c, ui_font(16.0f), rct_center(br), ALIGN_CENTER_CENTER,
                  "DRONE", on ? INK_BLACK : PAPER, 0.0f);
        if (resp.clicked) app_set_engaged(a, !on);
        y += ENGAGE_H;
    }

    content_h = y - y_start;
    canvas_set_clip(c, prev_clip);
}

void draw_right_rail(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    draw_rect_filled(c, r, INK_BLACK);
    Rct inner = rct_shrink(r, GROUP);
    Rct prev_clip = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(prev_clip, inner));

    static float content_h = 0.0f;
    float off = ui_scroll(ui, &a->right_scroll, inner, content_h);
    float x = inner.x0, w = rct_w(inner);
    float y = inner.y0 - off;
    float y_start = y;

    FontId body = ui_font(12.0f);
    float th = text_row_height(body);
    float strip_h = th + 2.0f * SNUG;

    inverted_strip(c, rct_xywh(x, y, w, strip_h), "OPERATORS");
    y += strip_h + GROUP;
    y += TIGHT;

    Compiled compiled = compile(a->shadow.algorithm);
    Patch defaults = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    int toggled = -1;
    bool operator_changed = false;
    for (int i = 0; i < NUM_OPS; i++) {
        float row_h = th + 2.0f * SNUG;
        if (row_h < 22.0f) row_h = 22.0f;
        char lbl[8];
        if (a->shadow.ops[i].enabled) snprintf(lbl, sizeof lbl, "%d", i + 1);
        else snprintf(lbl, sizeof lbl, "\xC3\x97");
        float bw = text_width(body, lbl, 0.0f) + 2.0f * GAP;
        if (bw < 24.0f) bw = 24.0f;
        Rct br = rct_xywh(x, y, bw, row_h);
        if (chip_button(ui, ui_id_n("rrail.op", i), br, lbl,
                        a->shadow.ops[i].enabled))
            toggled = i;
        float bx = x + bw + GROUP;

        char ratio[32];
        snprintf(ratio, sizeof ratio, "\xC3\x97%-6.3f", a->shadow.ops[i].ratio);
        text_draw(c, body, (P2){bx, y + row_h * 0.5f}, ALIGN_LEFT_CENTER,
                  ratio, PAPER, 0.0f);
        bx += text_width(body, ratio, 0.0f) + GROUP;

        Rct mr = rct_xywh(bx, y + (row_h - 12.0f) * 0.5f, 70.0f, 12.0f);
        draw_rect_stroke(c, mr, 2.0f, PAPER);
        Rct mi = rct_shrink(mr, 2.0f);
        dither_rect(c,
                    rct_xywh(mi.x0, mi.y0,
                             rct_w(mi) * fminf(a->env[i] * 1.4f, 1.0f),
                             rct_h(mi)),
                    0.9f, 2.0f);
        bx += 70.0f + GROUP;

        bool carrier = (compiled.carriers >> i & 1) == 1;
        char tag[16];
        snprintf(tag, sizeof tag, "%-3s %-2s", carrier ? "car" : "mod",
                 compiled.feedback_op == i ? "fb" : "");
        text_draw(c, body, (P2){bx, y + row_h * 0.5f}, ALIGN_LEFT_CENTER, tag,
                  PAPER, 0.0f);
        y += row_h + GROUP;

        /* Ratio and level are direct operator controls. The graph above can
           turn this oscillator into a carrier or a modulator, but it never
           gets to replace either value. The logarithmic range gives useful
           resolution around 1x while retaining deliberately extreme FM. */
        char label[24], value[32];
        snprintf(label, sizeof label, "op %d ratio", i + 1);
        snprintf(value, sizeof value, "x%.4f", (double)a->shadow.ops[i].ratio);
        operator_changed |=
            fad_log(ui, ui_id_n("rrail.ratio", i), rct_xywh(x, y, w, FADER_H),
                    label, &a->shadow.ops[i].ratio, OP_RATIO_MIN, OP_RATIO_MAX,
                    defaults.ops[i].ratio, value);
        y += FADER_H + GROUP;

        snprintf(label, sizeof label, "op %d level", i + 1);
        snprintf(value, sizeof value, "%.3f", (double)a->shadow.ops[i].level);
        operator_changed |=
            fad_lin(ui, ui_id_n("rrail.level", i), rct_xywh(x, y, w, FADER_H),
                    label, &a->shadow.ops[i].level, 0.0f, 1.0f,
                    defaults.ops[i].level, value);
        y += FADER_H + GROUP;
    }
    if (toggled >= 0) {
        a->shadow.ops[toggled].enabled = !a->shadow.ops[toggled].enabled;
        operator_changed = true;
    }
    if (operator_changed) send_patch(a);
    y += GROUP;

    {
        static const char *const TABS[2] = {"THE ROOM", "CHANDAS"};
        FontId wf = ui_font(12.0f * 1.3f);
        float tab_h = text_row_height(wf) + 2.0f * 2.0f + 2.0f * SNUG;
        int t = wave_tabs(ui, ui_id("rrail.tabs"), rct_xywh(x, y, w, tab_h),
                          TABS, 2, a->ops_tab);
        if (t >= 0 && t != a->ops_tab) a->ops_tab = t;
        y += tab_h + GROUP;
    }
    y += TIGHT;

    pentagram(a, ui, rct_xywh(x, y, w, ROOM_ART_H));
    y += ROOM_ART_H + GROUP;

    if (a->ops_tab != 0) {
        chandas_section(a, ui, x, w, &y);
        warmth_fader(a, ui, x, w, &y);
        content_h = y - y_start;
        canvas_set_clip(c, prev_clip);
        return;
    }

    float tremble =
        ((float)(a->frame_count / 2 % 3) - 1.0f) * a->dread_level;
    VerbParams *v = &a->shadow_verb;
    VerbParams vd = verb_params_default();
    bool verb_changed = false;
    char val[32];

    snprintf(val, sizeof val, "%.2f", v->mix);
    verb_changed |= fad_lin(ui, ui_id("verb.mix"), rct_xywh(x, y, w, FADER_H),
                            "mix", &v->mix, 0.0f, 1.0f, vd.mix, val);
    y += FADER_H + GROUP + FADER_ROW_EXTRA;
    snprintf(val, sizeof val, "%.2f", v->ghost);
    verb_changed |= fad_lin(ui, ui_id("verb.ghost"),
                            rct_xywh(x, y, w, FADER_H), "ghost", &v->ghost,
                            0.0f, 1.0f, vd.ghost, val);
    y += FADER_H + GROUP + FADER_ROW_EXTRA;
    snprintf(val, sizeof val, "%.2f s", v->decay);
    verb_changed |= fad_log(ui, ui_id("verb.decay"),
                            rct_xywh(x, y, w, FADER_H), "decay s", &v->decay,
                            0.05f, 8.0f, vd.decay, val);
    y += FADER_H + GROUP + FADER_ROW_EXTRA;
    snprintf(val, sizeof val, "%.2f", v->damp);
    verb_changed |= fad_lin(ui, ui_id("verb.damp"), rct_xywh(x, y, w, FADER_H),
                            "damp", &v->damp, 0.0f, 0.99f, vd.damp, val);
    y += FADER_H + GROUP + FADER_ROW_EXTRA;
    {
        float shift = 2.0f + roundf(tremble);
        snprintf(val, sizeof val, "%.2f", v->haunt);
        verb_changed |= fad_lin(ui, ui_id("verb.haunt"),
                                rct_xywh(x + shift, y, w - shift, FADER_H),
                                "haunt", &v->haunt, 0.0f, 1.0f, vd.haunt, val);
        y += FADER_H + GROUP + FADER_ROW_EXTRA;
    }
    if (verb_changed) send_verb(a);

    warmth_fader(a, ui, x, w, &y);
    content_h = y - y_start;
    canvas_set_clip(c, prev_clip);
}
