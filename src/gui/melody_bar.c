/* The melody page of the bottom strip: the sample-and-hold note source, its
   tuning and scale, and its rate, range and root. */
#include <math.h>
#include <stdio.h>

#include "app.h"

#define STEPPER_W 170.0f

static const char *const MIDI_OWNS =
    "midi is connected, so the instrument is configured for midi — the "
    "melody sequencer is idle until it's unplugged.";

static int wrap(int v, int n) { return ((v % n) + n) % n; }

/* PITCH and whether notes land on a grid, with the grid's symbol */
static void pitch_readout(App *a, Ui *ui, Flow *flow, bool driving) {
    Canvas *c = ui->canvas;
    FontId body = ui_font(12.0f);
    const MelodyParams *m = &a->shadow_melody;
    Quantization q = tuning_quantization(m->tuning);
    bool snapping = driving || (m->enabled && quantization_is_quantized(q));
    const char *word = snapping ? "QUANTISED" : "FREE";
    const char *symbol = driving ? "12" : (m->enabled ? q.symbol : "");

    Rct label = flow_next(flow, text_width(body, "PITCH", 0.0f));
    text_draw(c, body, (P2){label.x0, rct_center(label).y}, ALIGN_LEFT_CENTER,
              "PITCH", PAPER, 0.0f);
    Rct fr = flow_next(flow, text_width(body, word, 0.0f) + 2.0f * SNUG);
    draw_rect_filled(c, fr, snapping ? PAPER : INK_BLACK);
    draw_rect_stroke(c, fr, 1.0f, PAPER);
    text_draw(c, body, rct_center(fr), ALIGN_CENTER_CENTER, word,
              snapping ? INK_BLACK : PAPER, 0.0f);
    if (symbol && symbol[0]) {
        Rct sr = flow_next(flow, text_width(body, symbol, 0.0f));
        text_draw(c, body, (P2){sr.x0, rct_center(sr).y}, ALIGN_LEFT_CENTER,
                  symbol, PAPER, 0.0f);
    }
}

/* SYNC puts the notes on the clock, one per division; otherwise rate hz.
   Returns true when a press was refused because midi owns the notes. */
static bool rate_control(App *a, Ui *ui, Rct r, bool driving) {
    MelodyParams *m = &a->shadow_melody;
    FontId body = ui_font(12.0f);
    Rct chip = cut_left(&r, text_width(body, "SYNC", 0.0f) + 2.0f * GAP);
    cut_left(&r, GROUP);
    if (driving) {
        chip_button(ui, ui_id("melody sync"), chip, "SYNC", m->sync);
        return param_fader_veiled(a, ui, r, PARAM_MEL_RATE) && press_on(ui, r);
    }
    if (chip_button(ui, ui_id("melody sync"), chip, "SYNC", m->sync)) {
        m->sync = !m->sync;
        params_send(a, PG_MELODY);
    }
    if (!m->sync) {
        param_fader(a, ui, r, PARAM_MEL_RATE);
        return false;
    }
    char text[32];
    snprintf(text, sizeof text, "note %s", CHANDAS_DIVISIONS[m->division].name);
    int step = stepper(ui, ui_id("melody division"), r, text, true);
    /* the divisions run long to short, so the left arrow moves down it */
    int d = m->division - step;
    if (step && d >= 0 && d < CHANDAS_DIVISIONS_LEN) {
        m->division = (int8_t)d;
        params_send(a, PG_MELODY);
    }
    return false;
}

void draw_melody_page(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    FontId body = ui_font(12.0f);
    MelodyParams *m = &a->shadow_melody;
    bool driving = midi_driving(a);
    bool changed = false, refused = false;

    Rct inner = r;
    Rct top = cut_top(&inner, ROW_H);
    cut_top(&inner, GROUP);
    Rct faders = cut_top(&inner, FADER_H);

    Flow flow = flow_in(top, ROW_H, GROUP);
    {
        const char *lbl = m->enabled ? "S&H ON" : "S&H OFF";
        Rct br = flow_next(&flow, fmaxf(text_width(body, lbl, 0.0f) + 2.0f * GAP,
                                        70.0f));
        if (chip_button(ui, ui_id("melody sh"), br, lbl, m->enabled)) {
            if (driving) refused = true;
            else melody_set_enabled(a, !m->enabled);
        }
    }
    for (int src = 0; src < 2; src++) {
        const char *name = src == HOLD_GOLDEN_WEYL ? "golden" : "random";
        bool active = (int)m->source == src;
        Rct br = flow_next(&flow, text_width(body, name, 0.0f) + 2.0f * GAP);
        if (chip_button(ui, ui_id_n("melody src", src), br, name, active) && !active) {
            if (driving) refused = true;
            else m->source = (HoldSource)src, changed = true;
        }
    }
    {
        char text[64];
        snprintf(text, sizeof text, "tuning: %s", tuning_name(m->tuning));
        Rct sr = flow_next(&flow, STEPPER_W);
        int step = stepper(ui, ui_id("melody tuning"), sr, text, !driving);
        if (step) {
            m->tuning = (Tuning)wrap((int)m->tuning + step, NUM_TUNINGS);
            changed = true;
        }
        refused |= driving && press_on(ui, sr);
    }
    {
        /* picking a scale also puts the tuning back on the scale */
        bool on_scale = m->tuning == TUNING_SCALE;
        char text[64];
        snprintf(text, sizeof text, "scale: %s", on_scale ? scale_name(m->scale) : "off");
        Rct sr = flow_next(&flow, STEPPER_W);
        int step = stepper(ui, ui_id("melody scale"), sr, text, !driving);
        if (step) {
            m->scale = on_scale ? (Scale)wrap((int)m->scale + step, NUM_SCALES)
                                : m->scale;
            m->tuning = TUNING_SCALE;
            changed = true;
        }
        refused |= driving && press_on(ui, sr);
    }
    flow.x += GROUP;
    pitch_readout(a, ui, &flow, driving);
    if (flow_bottom(&flow) > top.y1 + 0.5f)
        layout_overflow("melody", flow_bottom(&flow) - top.y1);
    if (changed) params_send(a, PG_MELODY);

    static const ParamId FADERS[] = {PARAM_MEL_RATE, PARAM_MEL_RANGE,
                                     PARAM_MEL_ROOT};
    const int n = (int)(sizeof FADERS / sizeof FADERS[0]);
    float w = floorf((rct_w(faders) - (float)(n - 1) * GROUP) / (float)n);
    for (int i = 0; i < n; i++) {
        Rct fr = rct_xywh(faders.x0 + (float)i * (w + GROUP), faders.y0, w,
                          FADER_H);
        if (i == 0) {
            refused |= rate_control(a, ui, fr, driving);
            continue;
        }
        if (!driving) param_fader(a, ui, fr, FADERS[i]);
        else refused |= param_fader_veiled(a, ui, fr, FADERS[i]) && press_on(ui, fr);
    }
    if (driving) {
        dither_rect_ink(c, top, VEIL, 2.0f, INK_BLACK);
        if (refused) push_log(a, "%s", MIDI_OWNS);
    }
}
