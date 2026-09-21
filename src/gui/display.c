/* The display column: one page at a time under a row of tabs. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define ENV_KNOB_H 62.0f
#define FRAC_1_SQRT_2 0.70710678118654752440f

/* ---------- scope: phase + pentagram ---------- */

static void paint_phase(const App *a, Canvas *c, Rct rect) {
    draw_rect_stroke(c, rect, 2.0f, PAPER);
    Rct inner = rct_shrink(rect, 1.0f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(inner, saved));
    draw_graticule(c, inner, 6, 6);
    P2 ce = rct_center(inner);
    float scale = fminf(rct_w(inner), rct_h(inner)) * 0.6f;
    int len = a->lissa_len;
    int n = len > 2 ? len : 2;
    P2 prev = {0, 0};
    for (int i = 0; i < len; i++) {
        int idx = (a->lissa_head - len + i + 512) % 512;
        float l = a->lissa_x[idx], r = a->lissa_y[idx];
        float x = (l - r) * FRAC_1_SQRT_2;
        float y = (l + r) * FRAC_1_SQRT_2;
        P2 p = {ce.x + x * scale, ce.y - y * scale};
        if (i > 0) beam_segment(c, prev, p, i, i < n / 3);
        prev = p;
    }
    canvas_set_clip(c, saved);
}

static void labelled(Canvas *c, Rct *r, const char *text) {
    inverted_strip(c, cut_top(r, text_row_height(ui_font(12.0f)) + 2.0f * SNUG),
                   text);
    cut_top(r, GROUP);
}

static void draw_scope_page(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    r = rct_shrink(r, GAP);
    labelled(c, &r, "PHASE");
    float side = fminf(rct_w(r), rct_h(r));
    paint_phase(a, c, rct_xywh(roundf(rct_center(r).x - 0.5f * side),
                               roundf(rct_center(r).y - 0.5f * side), side, side));
}

/* ---------- envelope ---------- */

static EnvParams shadow_env(const App *a) {
    EnvParams p = env_params_default();
    p.attack_s = a->shadow_attack_s;
    p.decay_s = a->shadow_decay_s;
    p.sustain = a->shadow_sustain;
    p.release_s = a->shadow_release_s;
    return p;
}

/* the note held for a while then let go, at full velocity, with the newest
   voice's envelope riding it as a dot */
static void paint_envelope(const App *a, Canvas *c, Rct rect) {
    draw_rect_stroke(c, rect, 1.0f, PAPER);
    Rct inner = rct_shrink(rect, 3.0f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(inner, saved));
    EnvParams p = shadow_env(a);
    float at = fmaxf(p.attack_s, 1e-4f);
    float de = fmaxf(p.decay_s, 1e-4f);
    float re = fmaxf(p.release_s, 1e-4f);
    float hold = fmaxf(0.2f * (at + de + re), 0.05f);
    float span = at + de + hold + re;
    int steps = (int)rct_w(inner);
    if (steps < 2) steps = 2;
    float bottom = inner.y1 - 1.0f, height = rct_h(inner) - 2.0f;

    float marks[3] = {at, at + de, at + de + hold};
    for (int k = 0; k < 3; k++) {
        float x = roundf(inner.x0 + marks[k] / span * rct_w(inner));
        for (float yy = inner.y0; yy < inner.y1; yy += 3.0f)
            draw_dot(c, (P2){x, roundf(yy)}, PAPER);
    }

    Envelope e;
    envelope_init(&e, (float)steps / span);
    envelope_note_on(&e);
    P2 pts[1024];
    int n = steps + 1 < 1024 ? steps + 1 : 1024;
    bool released = false;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)steps * span;
        if (!released && t >= at + de + hold) {
            envelope_note_off(&e);
            released = true;
        }
        float level = envelope_tick(&e, &p);
        pts[i] = (P2){inner.x0 + (float)i, bottom - level * height};
    }
    draw_polyline(c, pts, (size_t)n, 2.0f, PAPER);

    if (a->chain.amp.kind == AMP_ENVELOPE) {
        uint32_t clock = atomic_load_explicit(&((App *)a)->env_clock,
                                              memory_order_relaxed);
        EnvStage stage = (EnvStage)(clock >> 30);
        float secs = (float)(clock & ((1u << 30) - 1)) / 1000.0f;
        float level = (float)atomic_load_explicit(&((App *)a)->env_level_q16,
                                                  memory_order_relaxed)
                      / 65536.0f;
        float t = -1.0f;
        if (stage == ENV_HELD) t = fminf(secs, at + de + hold);
        else if (stage == ENV_RELEASED) t = fminf(at + de + hold + secs, span);
        if (t >= 0.0f) {
            P2 dot = {roundf(inner.x0 + t / span * rct_w(inner)),
                      roundf(bottom - level * height)};
            draw_circle_filled(c, dot, 3.0f, PAPER);
            draw_circle_stroke(c, dot, 5.0f, 1.0f, PAPER);
        }
    }
    canvas_set_clip(c, saved);
}

static void envelope_knobs(App *a, Ui *ui, Rct r) {
    static const ParamId KNOBS[] = {PARAM_ATTACK, PARAM_ENV_DECAY,
                                    PARAM_SUSTAIN, PARAM_RELEASE};
    const int n = (int)(sizeof KNOBS / sizeof KNOBS[0]);
    bool reaches = a->chain.amp.kind == AMP_ENVELOPE || a->shadow_melody.enabled;
    float kw = rct_w(r) / (float)n;
    for (int k = 0; k < n; k++) {
        Rct kr = rct(roundf(r.x0 + kw * (float)k), r.y0,
                     roundf(r.x0 + kw * (float)(k + 1)), r.y1);
        bool live = reaches;
        param_knob(a, ui, kr, KNOBS[k], live);
        if (live || !press_on(ui, kr)) continue;
        if (midi_driving(a))
            push_log(a, "midi is connected, but the drone is still on "
                        "— turn it off to make notes the amplitude "
                        "authority and the envelope reachable.");
        else
            push_log(a, "the envelope only reaches anything once notes "
                        "raise the sound — connect midi or start the "
                        "sequencer, and switch the drone off.");
    }
}

static void draw_envelope_page(App *a, Ui *ui, Rct r) {
    r = rct_shrink(r, GAP);
    Rct knobs = cut_bottom(&r, fminf(ENV_KNOB_H, rct_h(r)));
    cut_bottom(&r, GROUP);
    paint_envelope(a, ui->canvas, r);
    envelope_knobs(a, ui, knobs);
}

/* ---------- presets, info ---------- */

static void draw_presets_page(App *a, Ui *ui, Rct r) {
    draw_presets_pane(a, ui, rct_shrink(r, GAP));
}

/* short enough to live in the instrument; the full licence text sits
   beside the binary in THIRD-PARTY-LICENSES.txt */
static void draw_info_page(App *a, Ui *ui, Rct r) {
    (void)a;
    Canvas *c = ui->canvas;
    FontId heading = ui_font(12.0f);
    FontId body = ui_font(11.0f);
    Rct content = rct_shrink(r, 6.0f);
    float y = content.y0;

    text_draw(c, heading, (P2){content.x0, y}, ALIGN_LEFT_TOP,
              "BYPO MONO C", PAPER, 0.0f);
    y += text_row_height(heading) + TIGHT;
    text_draw(c, body, (P2){content.x0, y}, ALIGN_LEFT_TOP,
              "design + audio architecture  AKOL", PAPER, 0.0f);
    y += text_row_height(body) + SECTION;

    text_draw(c, heading, (P2){content.x0, y}, ALIGN_LEFT_TOP, "CREDITS", PAPER,
              0.0f);
    y += text_row_height(heading) + TIGHT;
    static const char *const LINES[] = {
        "BYPOSerif, from Source Serif 4",
        "  Adobe  SIL OFL 1.1",
        "Unifont Ex Mono",
        "  stgiga / GNU Unifont  SIL OFL 1.1",
        "SDL2  zlib License",
        "FreeType  FTL",
        "",
        "BYPO source: MIT License",
        "full notices: THIRD-PARTY-LICENSES.txt",
    };
    for (size_t i = 0; i < sizeof LINES / sizeof LINES[0]; i++) {
        if (y + text_row_height(body) > content.y1) break;
        text_draw(c, body, (P2){content.x0, y}, ALIGN_LEFT_TOP, LINES[i], PAPER,
                  0.0f);
        y += text_row_height(body) + TIGHT;
    }
}

static void draw_shell_page(App *a, Ui *ui, Rct r) { draw_stage(a, ui, r); }

static const Tab DISPLAY[DISPLAY_TABS] = {
    [TAB_SHELL] = {"SHELL", draw_shell_page},
    [TAB_SCOPE] = {"SCOPE", draw_scope_page},
    [TAB_ENV] = {"ENV", draw_envelope_page},
    [TAB_PRE] = {"PRE", draw_presets_page},
    [TAB_INFO] = {"I", draw_info_page},
};

void draw_display_tabs(App *a, Ui *ui, Rct bar) {
    tab_strip(ui, "display tab", bar, DISPLAY, DISPLAY_TABS, &a->display_tab);
}

void draw_display_column(App *a, Ui *ui, Rct r) {
    tab_page(a, ui, rct_shrink(r, GAP), DISPLAY, DISPLAY_TABS, a->display_tab);
}
