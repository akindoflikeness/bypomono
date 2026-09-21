/* The FM column: the algorithm and the five operators it wires together. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define OP_CHIP_W 25.0f
#define ENV_METER_W 40.0f

static int changed_node(int ai, int bi) {
    AlgorithmId A = ALGORITHMS[ai], B = ALGORITHMS[bi];
    if (algorithm_distance(A, B) != 1) return -1;
    for (int k = 0; k < NUM_NODES; k++)
        if (algorithm_node(A, k) != algorithm_node(B, k)) return k;
    return -1;
}

/* The junction tree. The nodes one step away along the roster blink and
   step there when clicked. */
static void tree(App *a, Ui *ui, Rct rect) {
    Canvas *c = ui->canvas;
    Resp resp = ui_interact(ui, ui_id("fm tree"), rect, 0.0f);
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
    if (!resp.clicked) return;
    for (int node = 0; node < NUM_NODES; node++) {
        float dx = resp.pointer.x - nodes[node].x;
        float dy = resp.pointer.y - nodes[node].y;
        if (sqrtf(dx * dx + dy * dy) >= 12.0f) continue;
        if (node == next_flip)
            app_set_algorithm(a, idx + 1);
        else if (node == prev_flip)
            app_set_algorithm(a, idx - 1);
        else
            push_log(a, "node dormant. off-roster structures wake at roster "
                        "size 13.");
        break;
    }
}

static void roster(App *a, Ui *ui, Rct row) {
    int idx = algorithm_index_of(&a->shadow);
    float slot = floorf((rct_w(row) - 7.0f * TIGHT) / 8.0f);
    for (int i = 0; i < 8; i++) {
        Rct b = rct_xywh(row.x0 + (float)i * (slot + TIGHT), row.y0, slot,
                         rct_h(row));
        if (pane_button(ui, ui_id_n("fm alg", i), b, ROMAN[i], i == idx)
            && i != idx)
            app_set_algorithm(a, i);
    }
}

/* ratio palettes: an explicit load, never a side effect of topology */
static void ratio_modes(App *a, Ui *ui, Stack *s) {
    FontId f = ui_font(12.0f);
    Flow flow = flow_in(stack_rest(s), ROW_H, GROUP);
    int picked = -1;
    for (int m = 0; m < RATIO_MODE_COUNT; m++) {
        const char *name = mode_name_of((RatioMode)m);
        Rct chip = flow_next(&flow, text_width(f, name, 0.0f) + 2.0f * GAP);
        bool active = a->shadow.ratio_mode == (RatioMode)m;
        if (chip_button(ui, ui_id_n("fm mode", m), chip, name, active) && !active)
            picked = m;
    }
    stack_row(s, flow_bottom(&flow) - flow.area.y0);
    if (picked < 0) return;
    patch_apply_ratio_mode(&a->shadow, (RatioMode)picked);
    params_send(a, PG_PATCH);
    char ratios[128];
    size_t at = 0;
    for (int i = 0; i < NUM_OPS; i++)
        at += (size_t)snprintf(ratios + at, sizeof ratios - at,
                               i ? " %.3f" : "%.3f",
                               (double)a->shadow.ops[i].ratio);
    push_log(a, "ratio palette %s loaded. op ratios: %s.",
             mode_name_of((RatioMode)picked), ratios);
}

static void level_text(char *out, size_t cap, float value) {
    snprintf(out, cap, "%.2f", (double)clampf(value, 0.0f, 1.0f));
    if (out[0] == '0') memmove(out, out + 1, strlen(out));
}

/* one row per operator: on/off, ratio, level and its live envelope */
static void operators(App *a, Ui *ui, Stack *s) {
    Canvas *c = ui->canvas;
    FontId heading = ui_font(9.0f);
    Rct head = stack_row(s, text_row_height(heading));
    float x_ratio = head.x0 + OP_CHIP_W + GAP;
    float x_meter = head.x1 - ENV_METER_W;
    float controls_w = x_meter - GAP - x_ratio;
    float ratio_w = floorf((controls_w - GAP) * GOLDEN_MAJOR);
    float x_level = x_ratio + ratio_w + GAP;
    float level_w = x_meter - GAP - x_level;
    text_draw(c, heading, (P2){head.x0, head.y0}, ALIGN_LEFT_TOP, "OP", PAPER, 1.0f);
    text_draw(c, heading, (P2){x_ratio, head.y0}, ALIGN_LEFT_TOP, "RATIO", PAPER, 1.0f);
    text_draw(c, heading, (P2){x_level, head.y0}, ALIGN_LEFT_TOP, "LEVEL", PAPER, 1.0f);
    text_draw(c, heading, (P2){x_meter, head.y0}, ALIGN_LEFT_TOP, "ENV", PAPER, 1.0f);

    Patch defaults = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    bool changed = false;
    for (int i = 0; i < NUM_OPS; i++) {
        OpParams *op = &a->shadow.ops[i];
        Rct row = stack_row(s, FADER_H);
        char text[32];
        snprintf(text, sizeof text, "%d", i + 1);
        if (chip_button(ui, ui_id_n("fm op", i),
                        rct_xywh(row.x0, row.y0, OP_CHIP_W, FADER_H), text,
                        op->enabled)) {
            op->enabled = !op->enabled;
            changed = true;
        }

        snprintf(text, sizeof text, "%.2f", (double)op->ratio);
        FaderAct ratio = fader_track(
            ui, ui_id_n("fm ratio", i), rct_xywh(x_ratio, row.y0, ratio_w, FADER_H),
            "", text, position_of_log(op->ratio, OP_RATIO_MIN, OP_RATIO_MAX));
        float next = op->ratio;
        if (ratio.kind == FADER_SET)
            next = log_position(ratio.t, OP_RATIO_MIN, OP_RATIO_MAX);
        else if (ratio.kind == FADER_RESET)
            next = defaults.ops[i].ratio;
        if (next != op->ratio) {
            op->ratio = next;
            changed = true;
        }

        level_text(text, sizeof text, op->level);
        FaderAct level = fader_track(
            ui, ui_id_n("fm level", i), rct_xywh(x_level, row.y0, level_w, FADER_H),
            "", text, clampf(op->level, 0.0f, 1.0f));
        /* level is relative operator gain, so its reset is unity whatever the
           algorithm's starting depth */
        next = op->level;
        if (level.kind == FADER_SET) next = level.t;
        else if (level.kind == FADER_RESET) next = 1.0f;
        if (next != op->level) {
            op->level = next;
            changed = true;
        }

        Rct meter = rct(x_meter, row.y0 + 5.0f, head.x1, row.y1 - 5.0f);
        draw_rect_stroke(c, meter, 1.0f, PAPER);
        Rct mi = rct_shrink(meter, 2.0f);
        dither_rect(c,
                    rct_xywh(mi.x0, mi.y0,
                             rct_w(mi) * fminf(a->env[i] * 1.4f, 1.0f),
                             rct_h(mi)),
                    0.9f, 2.0f);
    }
    if (changed) params_send(a, PG_PATCH);
}

void draw_fm_column(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    Rct content = rct_shrink(r, 6.0f);
    int idx = algorithm_index_of(&a->shadow);
    Rct below = window_chrome_tagged(c, content, "FM",
                                     idx < 5 ? "the unfolding" : "the folding");
    Stack s = stack_in(below, GROUP, "fm");
    stack_space(&s, GROUP);
    tree(a, ui, stack_row(&s, TREE_H));
    roster(a, ui, stack_row(&s, ROW_H));
    stack_space(&s, GROUP);
    inverted_strip(c, stack_row(&s, text_row_height(ui_font(12.0f)) + 2.0f * SNUG),
                   "RATIO MODE");
    ratio_modes(a, ui, &s);
    stack_space(&s, GROUP);
    operators(a, ui, &s);
    stack_close(&s);
}
