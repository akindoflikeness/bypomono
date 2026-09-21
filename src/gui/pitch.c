/* The pitch page of the bottom strip: a note sequencer on the clock. Three
   lanes share the sixteen columns: each step's pitch, whether it plays, and
   how hard. */
#include <math.h>
#include <stdio.h>

#include "app.h"

#define LABEL_W 44.0f
#define GATE_LANE_H 14.0f
#define VEL_LANE_H 30.0f
#define LANE_GAP 4.0f
#define DIM_LINE 90

/* ---------- the top row ---------- */

static bool controls(App *a, Ui *ui, Rct top) {
    FontId body = ui_font(12.0f);
    PitchSeqParams *p = &a->shadow_pitch;
    bool changed = false;
    Flow f = flow_in(top, ROW_H, GAP);

    const char *on = p->enabled ? "ON" : "OFF";
    if (chip_button(ui, ui_id("pitch on"),
                    flow_next(&f, fmaxf(text_width(body, on, 0.0f) + 2.0f * GAP, 44.0f)),
                    on, p->enabled)) {
        pitch_set_enabled(a, !p->enabled);
        return false; /* already sent */
    }

    char text[48];
    snprintf(text, sizeof text, "step %s", CHANDAS_DIVISIONS[p->division].name);
    int step = stepper(ui, ui_id("pitch division"), flow_next(&f, 120.0f), text, true);
    /* the divisions run long to short, so the left arrow moves down it */
    int d = p->division - step;
    if (step && d >= 0 && d < CHANDAS_DIVISIONS_LEN) {
        p->division = (int8_t)d;
        changed = true;
    }

    snprintf(text, sizeof text, "len %d", p->length);
    step = stepper(ui, ui_id("pitch length"), flow_next(&f, 84.0f), text, true);
    int len = p->length + step;
    if (step && len >= 1 && len <= PITCH_STEPS) {
        p->length = (uint8_t)len;
        changed = true;
    }

    char note[8];
    midi_note_name(p->root_midi, note, sizeof note);
    snprintf(text, sizeof text, "root %s", note);
    step = stepper(ui, ui_id("pitch root"), flow_next(&f, 104.0f), text, true);
    int root = p->root_midi + step;
    if (step && root >= 0 && root <= 127) {
        p->root_midi = (uint8_t)root;
        changed = true;
    }

    if (chip_button(ui, ui_id("pitch snap"),
                    flow_next(&f, text_width(body, "12-TET", 0.0f) + 2.0f * GAP),
                    "12-TET", p->snap)) {
        p->snap = !p->snap;
        changed = true;
    }

    snprintf(text, sizeof text, "%d%%", (int)roundf(p->gate_len * 100.0f));
    float span = 1.0f - PITCH_GATE_LEN_MIN;
    FaderAct act = fader_track(ui, ui_id("pitch gate len"), flow_next(&f, 170.0f),
                               "gate", text, (p->gate_len - PITCH_GATE_LEN_MIN) / span);
    if (act.kind == FADER_SET) {
        p->gate_len = PITCH_GATE_LEN_MIN + act.t * span;
        changed = true;
    } else if (act.kind == FADER_RESET) {
        p->gate_len = pitch_seq_params_default().gate_len;
        changed = true;
    }
    if (flow_bottom(&f) > top.y1 + 0.5f)
        layout_overflow("pitch controls", flow_bottom(&f) - top.y1);
    return changed;
}

/* ---------- the lanes ---------- */

static Rct column(Rct lane, int k) {
    float cw = rct_w(lane) / (float)PITCH_STEPS;
    return rct(roundf(lane.x0 + (float)k * cw) + 1.0f, lane.y0,
               roundf(lane.x0 + (float)(k + 1) * cw) - 1.0f, lane.y1);
}

static void lane_label(Canvas *c, Rct lane, const char *text) {
    text_draw(c, ui_font(11.0f), (P2){lane.x0 - LABEL_W, lane.y0}, ALIGN_LEFT_TOP,
              text, PAPER, 0.0f);
}

/* a cell's frame: inverted under the playhead, dotted past the length */
static uint8_t cell_frame(Canvas *c, Rct cell, bool head, bool played) {
    if (!played) {
        dotted_rect(c, cell, PAPER);
        return PAPER;
    }
    if (head) draw_rect_filled(c, cell, PAPER);
    draw_rect_stroke(c, cell, 1.0f, PAPER);
    return head ? INK_BLACK : PAPER;
}

static void pitch_text(const PitchSeqParams *p, int k, char *out, size_t cap) {
    if (p->snap) {
        midi_note_name(p->root_midi + (int)pitch_seq_semitones(p, k), out, cap);
    } else if (fabsf(p->pitch[k]) < 0.005f) {
        snprintf(out, cap, "0");
    } else {
        snprintf(out, cap, "%+.2f", (double)p->pitch[k]);
    }
}

static void draw_pitch_lane(App *a, Canvas *c, Rct lane, int head) {
    const PitchSeqParams *p = &a->shadow_pitch;
    FontId f = ui_font(10.0f);
    for (int k = 0; k < PITCH_STEPS; k++) {
        Rct cell = column(lane, k);
        bool played = k < p->length;
        uint8_t ink = cell_frame(c, cell, k == head, played);
        float mid = roundf(rct_center(cell).y);
        for (float x = cell.x0 + 2.0f; x < cell.x1 - 2.0f; x += 3.0f)
            draw_rect_filled(c, rct_xywh(x, mid, 1, 1),
                             k == head ? INK_BLACK : DIM_LINE);
        float t = (pitch_seq_semitones(p, k) + PITCH_RANGE_ST) / (2.0f * PITCH_RANGE_ST);
        float y = roundf(cell.y1 - t * rct_h(cell));
        draw_rect_filled(c, rct(cell.x0 + 3.0f, fmaxf(y - 1.0f, cell.y0),
                                cell.x1 - 3.0f, fminf(y + 1.0f, cell.y1)), ink);
        char text[12];
        pitch_text(p, k, text, sizeof text);
        float ty = t > 0.75f ? y + 4.0f : y - 3.0f - text_row_height(f);
        text_draw(c, f, (P2){rct_center(cell).x, ty}, ALIGN_CENTER_TOP, text, ink,
                  0.0f);
    }
}

static void draw_gate_lane(App *a, Canvas *c, Rct lane, int head) {
    const PitchSeqParams *p = &a->shadow_pitch;
    for (int k = 0; k < PITCH_STEPS; k++) {
        Rct cell = column(lane, k);
        uint8_t ink = cell_frame(c, cell, k == head, k < p->length);
        if (p->gate[k]) draw_rect_filled(c, rct_shrink(cell, 3.0f), ink);
    }
}

static void draw_velocity_lane(App *a, Canvas *c, Rct lane, int head) {
    const PitchSeqParams *p = &a->shadow_pitch;
    for (int k = 0; k < PITCH_STEPS; k++) {
        Rct cell = column(lane, k);
        uint8_t ink = cell_frame(c, cell, k == head, k < p->length);
        Rct in = rct_shrink(cell, 3.0f);
        float h = roundf(p->velocity[k] * rct_h(in));
        if (h > 0.0f) draw_rect_filled(c, rct(in.x0, in.y1 - h, in.x1, in.y1), ink);
    }
}

void draw_pitch_page(App *a, Ui *ui, Rct top, Rct body) {
    Canvas *c = ui->canvas;
    PitchSeqParams *p = &a->shadow_pitch;
    bool changed = controls(a, ui, top);

    Rct lanes = body;
    cut_left(&lanes, LABEL_W);
    Rct vel = cut_bottom(&lanes, VEL_LANE_H);
    cut_bottom(&lanes, LANE_GAP);
    Rct gate = cut_bottom(&lanes, GATE_LANE_H);
    cut_bottom(&lanes, LANE_GAP);
    Rct pitch = lanes;

    changed |= steps_paint(a, ui, ui_id("pitch paint"), pitch, p->pitch,
                           PITCH_STEPS, -PITCH_RANGE_ST, PITCH_RANGE_ST, 0.0f);
    changed |= steps_toggle(a, ui, ui_id("pitch gates"), gate, p->gate,
                            PITCH_STEPS);
    changed |= steps_paint(a, ui, ui_id("pitch velocity"), vel, p->velocity,
                           PITCH_STEPS, 0.0f, 1.0f, 1.0f);
    if (changed) pitch_send(a);

    int head = p->enabled ? atomic_load_explicit(&a->seq_meter.pitch_step,
                                                 memory_order_relaxed)
                          : -1;
    draw_pitch_lane(a, c, pitch, head);
    draw_gate_lane(a, c, gate, head);
    draw_velocity_lane(a, c, vel, head);
    lane_label(c, pitch, "pitch");
    lane_label(c, gate, "gate");
    lane_label(c, vel, "vel");
}
