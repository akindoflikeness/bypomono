/* The bottom strip: MELODY, PITCH or SEQUENCES. A sequence is sixteen values
   painted across the step cells and pointed at controls through its routes. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define TABS_W 330.0f
#define LEFT_W 300.0f
#define ROUTES_W 270.0f
#define DIM_LINE 90

/* ---------- small pieces ---------- */

static const SeqParams *selected(const App *a) {
    return &a->mods.seq[a->seq_selected];
}

/* the first pitch route of seq, or NULL */
static const ModRoute *pitch_route(const ModBank *b, int seq) {
    int r = mod_bank_find_route(b, seq, MT_PITCH);
    return r >= 0 ? &b->route[r] : NULL;
}

/* the curve across r, value 0 at the bottom and 1 at the top */
static void draw_curve(Canvas *c, Rct r, const SeqParams *p, uint8_t ink,
                       bool dotted) {
    P2 prev = {0, 0};
    for (int x = 0; x <= (int)rct_w(r); x++) {
        float pos = (float)x / rct_w(r) * (float)SEQ_STEPS;
        float v = seq_value_at(p, fminf(pos, (float)SEQ_STEPS - 1e-3f));
        P2 at = {r.x0 + (float)x, roundf(r.y1 - 1.0f - v * (rct_h(r) - 2.0f))};
        if (dotted) {
            if (x % 3 == 0) draw_rect_filled(c, rct_xywh(at.x, at.y, 1, 1), ink);
        } else if (x > 0) {
            draw_line(c, prev, at, 1.0f, ink);
        }
        prev = at;
    }
}

/* ---------- the selectors: one small preview per sequence ---------- */

static void draw_selectors(App *a, Ui *ui, Rct row) {
    Canvas *c = ui->canvas;
    float w = fminf(floorf((rct_w(row) - 7.0f * GAP) / 8.0f), 64.0f);
    for (int s = 0; s < SEQS; s++) {
        Rct box = rct_xywh(row.x0 + (float)s * (w + GAP), row.y0, w, rct_h(row));
        const SeqParams *p = &a->mods.seq[s];
        bool sel = s == a->seq_selected;
        Resp r = ui_interact(ui, ui_id_n("seq pick", s), box, 0.0f);
        if (r.hovered) ui->cursor = CURSOR_POINTER;
        if (r.clicked) {
            a->seq_selected = s;
            if (!p->used) {
                ModBank next = a->mods;
                next.seq[s] = seq_params_default();
                mods_commit(a, &next);
            }
        }
        if (!p->used) {
            dotted_rect(c, box, PAPER);
        } else {
            if (sel) draw_rect_filled(c, box, PAPER);
            else draw_rect_stroke(c, box, 1.0f, PAPER);
            draw_curve(c, rct_shrink(box, 3.0f), p, sel ? INK_BLACK : PAPER,
                       !p->smooth);
        }
        char num[4];
        snprintf(num, sizeof num, "%d", s + 1);
        text_draw(c, ui_font(10.0f), (P2){box.x0 + 3.0f, box.y0 + 1.0f},
                  ALIGN_LEFT_TOP, num, sel && p->used ? INK_BLACK : PAPER, 0.0f);
    }
}

/* ---------- the left column: how the selected sequence plays ---------- */

static bool chip_at(Ui *ui, Flow *f, const char *id, int n, const char *text,
                    bool on) {
    FontId body = ui_font(12.0f);
    Rct r = flow_next(f, text_width(body, text, 0.0f) + 2.0f * GAP);
    return chip_button(ui, ui_id_n(id, n), r, text, on);
}

/* free length on a log fader, SEQ_LENGTH_MIN_S to SEQ_LENGTH_MAX_S */
static float length_pos(float s) {
    return logf(s / SEQ_LENGTH_MIN_S) / logf(SEQ_LENGTH_MAX_S / SEQ_LENGTH_MIN_S);
}

static float length_at(float t) {
    return SEQ_LENGTH_MIN_S * powf(SEQ_LENGTH_MAX_S / SEQ_LENGTH_MIN_S, t);
}

static void rate_row(App *a, Ui *ui, Rct row, SeqParams *p) {
    FontId body = ui_font(12.0f);
    bool synced = p->division >= 0;
    Rct chip = cut_left(&row, text_width(body, "SYNC", 0.0f) + 2.0f * GAP);
    cut_left(&row, GROUP);
    if (chip_button(ui, ui_id("seq sync"), chip, "SYNC", synced))
        p->division = synced ? -1 : SEQ_DEFAULT_DIVISION;
    if (p->division >= 0) {
        char text[48];
        snprintf(text, sizeof text, "step %s",
                 CHANDAS_DIVISIONS[p->division].name);
        int step = stepper(ui, ui_id("seq division"), row, text, true);
        /* the divisions run long to short, so the left arrow moves down it */
        int d = p->division - step;
        if (step && d >= 0 && d < CHANDAS_DIVISIONS_LEN) p->division = (int8_t)d;
    } else {
        char text[32];
        snprintf(text, sizeof text, "%.2f s", (double)p->length_s);
        FaderAct act = fader_track(ui, ui_id("seq length"), row, "all 16",
                                   text, length_pos(p->length_s));
        if (act.kind == FADER_SET) p->length_s = length_at(act.t);
        else if (act.kind == FADER_RESET) p->length_s = seq_params_default().length_s;
    }
    (void)a;
}

static void draw_controls(App *a, Ui *ui, Rct r) {
    ModBank next = a->mods;
    SeqParams *p = &next.seq[a->seq_selected];
    Stack st = stack_in(r, GAP, "seq controls");

    Flow f = flow_in(stack_row(&st, ROW_H), ROW_H, GAP);
    if (chip_at(ui, &f, "seq mode", 0, "loop", p->mode == SEQ_LOOP))
        p->mode = SEQ_LOOP;
    if (chip_at(ui, &f, "seq mode", 1, "once", p->mode == SEQ_ONCE))
        p->mode = SEQ_ONCE;
    f.x += GROUP;
    if (chip_at(ui, &f, "seq shape", 0, "smooth", p->smooth)) p->smooth = true;
    if (chip_at(ui, &f, "seq shape", 1, "steps", !p->smooth)) p->smooth = false;
    f.x += GROUP;
    if (chip_at(ui, &f, "seq rm", 0, "rm", false)) {
        mods_seq_remove(&next, a->seq_selected);
        mods_commit(a, &next);
        return;
    }

    rate_row(a, ui, stack_row(&st, ROW_H), p);

    static const SeqFill FILLS[] = {SEQ_FILL_SINE, SEQ_FILL_SAW, SEQ_FILL_TRI,
                                    SEQ_FILL_SQUARE, SEQ_FILL_RANDOM,
                                    SEQ_FILL_FLAT};
    static const char *const LABELS[] = {"sine", "saw", "tri", "sqr", "rnd",
                                         "flat"};
    f = flow_in(stack_row(&st, ROW_H), ROW_H, GAP);
    for (size_t i = 0; i < sizeof FILLS / sizeof FILLS[0]; i++)
        if (chip_at(ui, &f, "seq fill", (int)i, LABELS[i], false))
            seq_fill(p, FILLS[i], (uint32_t)(ui->time * 1000.0));
    stack_close(&st);
    mods_commit(a, &next);
}

/* ---------- the routes of the selected sequence ---------- */

static int next_free_target(const ModBank *b, int seq, int from, int dir) {
    for (int k = 1; k < MT_COUNT; k++) {
        int t = ((from - 1 + dir * k) % (MT_COUNT - 1) + (MT_COUNT - 1))
                    % (MT_COUNT - 1)
                + 1;
        if (mod_bank_find_route(b, seq, (ModTarget)t) < 0) return t;
    }
    return -1;
}

static void draw_routes(App *a, Ui *ui, Rct r) {
    ModBank next = a->mods;
    int s = a->seq_selected;
    Stack st = stack_in(r, GAP, "seq routes");
    for (int i = 0; i < MOD_ROUTES; i++) {
        ModRoute *rt = &next.route[i];
        if (rt->target == MT_NONE || rt->seq != s) continue;
        if (stack_rest(&st).y1 - st.y < ROW_H * 2.0f + GAP) break;
        Rct row = stack_row(&st, ROW_H);
        Rct x = cut_right(&row, ROW_H);
        cut_right(&row, GAP);
        if (chip_button(ui, ui_id_n("seq route rm", i), x, "x", false)) {
            memset(rt, 0, sizeof *rt);
            continue;
        }
        if (rt->target == MT_PITCH) {
            Rct snap = cut_right(&row, 44.0f);
            cut_right(&row, GAP);
            if (chip_button(ui, ui_id_n("seq route snap", i), snap, "snap",
                            rt->snap))
                rt->snap = !rt->snap;
        }
        Rct tgt = cut_left(&row, 112.0f);
        cut_left(&row, GAP);
        int step = stepper(ui, ui_id_n("seq route target", i), tgt,
                           MOD_TARGETS[rt->target].name, true);
        if (step) {
            int t = next_free_target(&next, s, rt->target, step);
            if (t > 0) {
                rt->target = (uint8_t)t;
                rt->snap = false;
            }
        }
        char depth[16];
        snprintf(depth, sizeof depth, "%+.2f", (double)rt->depth);
        FaderAct act = fader_track(ui, ui_id_n("seq route depth", i), row, "",
                                   depth, 0.5f * (rt->depth + 1.0f));
        if (act.kind == FADER_SET)
            rt->depth = roundf((act.t * 2.0f - 1.0f) * 100.0f) / 100.0f;
        else if (act.kind == FADER_RESET)
            rt->depth = 0.0f;
    }
    if (st.y + ROW_H <= r.y1) {
        Rct add = stack_row(&st, ROW_H);
        add.x1 = add.x0 + 80.0f;
        if (chip_button(ui, ui_id("seq route add"), add, "+ route", false)) {
            int t = MT_INDEX;
            while (t < MT_COUNT && mod_bank_find_route(&next, s, (ModTarget)t) >= 0)
                t++;
            if (t < MT_COUNT
                && !mods_route_set(&next, s, (ModTarget)t, 0.25f, false))
                push_log(a, "all %d routes are in use", MOD_ROUTES);
        }
    }
    stack_close(&st);
    mods_commit(a, &next);
}

/* ---------- the step cells ---------- */

static void semitone_text(const ModRoute *pr, float v, char *out, size_t cap) {
    float st = (2.0f * v - 1.0f) * pr->depth * MOD_PITCH_SEMITONES;
    if (pr->snap) snprintf(out, cap, "%+d", (int)roundf(st));
    else snprintf(out, cap, "%+.1f", (double)st);
    if (out[0] == '+' && (pr->snap ? roundf(st) == 0.0f : fabsf(st) < 0.05f))
        snprintf(out, cap, "0");
}

static void draw_cells(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    ModBank next = a->mods;
    int s = a->seq_selected;
    SeqParams *p = &next.seq[s];
    const ModRoute *pr = pitch_route(&next, s);
    Rct cells = r;
    float cw = rct_w(cells) / (float)SEQ_STEPS;
    steps_paint(a, ui, ui_id("seq paint"), cells, p->value, SEQ_STEPS, 0.0f,
                1.0f, 0.5f);

    int head = (int)floorf(seq_meter_pos(&a->seq_meter, s));
    FontId f = ui_font(10.0f);
    for (int k = 0; k < SEQ_STEPS; k++) {
        Rct cell = rct(roundf(cells.x0 + (float)k * cw) + 1.0f, cells.y0,
                       roundf(cells.x0 + (float)(k + 1) * cw) - 1.0f, cells.y1);
        bool on_head = k == head % SEQ_STEPS;
        uint8_t ink = on_head ? INK_BLACK : PAPER;
        if (on_head) draw_rect_filled(c, cell, PAPER);
        draw_rect_stroke(c, cell, 1.0f, PAPER);
        float mid = roundf(rct_center(cell).y);
        for (float x = cell.x0 + 2.0f; x < cell.x1 - 2.0f; x += 3.0f)
            draw_rect_filled(c, rct_xywh(x, mid, 1, 1), on_head ? INK_BLACK : DIM_LINE);
        float y = roundf(cell.y1 - p->value[k] * rct_h(cell));
        draw_rect_filled(c, rct(cell.x0 + 3.0f, fmaxf(y - 1.0f, cell.y0),
                                cell.x1 - 3.0f, fminf(y + 1.0f, cell.y1)), ink);
        if (pr) {
            char st[12];
            semitone_text(pr, p->value[k], st, sizeof st);
            float ty = p->value[k] > 0.8f ? y + 4.0f : y - 3.0f - text_row_height(f);
            text_draw(c, f, (P2){rct_center(cell).x, ty}, ALIGN_CENTER_TOP, st,
                      ink, 0.0f);
        }
    }
    draw_curve(c, rct(cells.x0 + 1.0f, cells.y0, cells.x1 - 1.0f, cells.y1), p,
               PAPER, !p->smooth);
    mods_commit(a, &next);
}

/* ---------- the page and the strip ---------- */

static void draw_sequences_page(App *a, Ui *ui, Rct top, Rct body) {
    if (a->seq_selected < 0 || a->seq_selected >= SEQS) a->seq_selected = 0;
    draw_selectors(a, ui, top);
    if (!selected(a)->used) {
        text_draw(ui->canvas, ui_font(12.0f), (P2){body.x0, body.y0},
                  ALIGN_LEFT_TOP,
                  "click a dotted box to start a sequence there", PAPER, 0.0f);
        return;
    }
    Rct left = cut_left(&body, LEFT_W);
    cut_left(&body, SECTION);
    Rct routes = cut_right(&body, ROUTES_W);
    cut_right(&body, SECTION);
    draw_controls(a, ui, left);
    if (!selected(a)->used) return; /* rm just took it */
    draw_routes(a, ui, routes);
    draw_cells(a, ui, body);
}

/* the strip draws its own pages: the sequences page also uses the row the
   tabs sit in */
static void no_page(App *a, Ui *ui, Rct r) {
    (void)a;
    (void)ui;
    (void)r;
}

static const Tab STRIP[STRIP_TABS] = {
    [TAB_MELODY] = {"MELODY", no_page},
    [TAB_PITCH] = {"PITCH", no_page},
    [TAB_SEQS] = {"SEQUENCES", no_page},
};

void draw_bottom_strip(App *a, Ui *ui, Rct r) {
    r = rct_shrink(r, 6.0f);
    Rct top = cut_top(&r, ROW_H);
    cut_top(&r, GAP);
    Rct tabs = cut_left(&top, TABS_W);
    cut_left(&top, SECTION);
    tab_strip(ui, "strip tab", tabs, STRIP, STRIP_TABS, &a->strip_tab);
    if (a->strip_tab == TAB_SEQS) draw_sequences_page(a, ui, top, r);
    else if (a->strip_tab == TAB_PITCH) draw_pitch_page(a, ui, top, r);
    else draw_melody_page(a, ui, r);
}
