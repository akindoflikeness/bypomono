#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

/* ---------- shared app helpers ---------- */

const char *const ROMAN[8] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII"};

const char *mode_name_of(RatioMode m) {
    switch (m) {
    case RATIO_HARMONIC: return "harmonic";
    case RATIO_FIBONACCI: return "fibonacci";
    case RATIO_GOLDEN: return "golden";
    case RATIO_GOLDEN_MIRROR: return "mirror";
    default: return "plastic";
    }
}

int algorithm_index_of(const Patch *p) {
    for (int i = 0; i < 8; i++)
        if (ALGORITHMS[i] == p->algorithm) return i;
    return 0;
}

void app_set_algorithm(App *a, int idx) {
    if (idx == algorithm_index_of(&a->shadow)) return;
    /* The graph changes here; the five oscillators do not. In particular,
       a carrier becoming a modulator (or vice versa) keeps its ratio and
       level so the topology can be auditioned against a stable palette. */
    a->shadow.algorithm = ALGORITHMS[idx];
    Event ev = {.kind = EV_SET_PATCH, .u.patch = a->shadow};
    app_send(a, ev);
    Compiled compiled = compile(a->shadow.algorithm);
    char glyphs[NUM_NODES + 1];
    algorithm_to_glyphs(a->shadow.algorithm, glyphs);
    unsigned max_depth = 0;
    for (int i = 0; i < NUM_OPS; i++)
        if (compiled.depth[i] > max_depth) max_depth = compiled.depth[i];
    push_log(a,
             "algorithm %s compiled (%s). carriers: %u. max depth: %u. "
             "feedback on op %d at depth %u.",
             ROMAN[idx], glyphs, (unsigned)compiled.carrier_count, max_depth,
             compiled.feedback_op + 1,
             (unsigned)compiled.depth[compiled.feedback_op]);
}

void app_set_engaged(App *a, bool on) {
    if (a->engaged == on) return;
    a->engaged = on;
    Event ev = {.kind = EV_ENGAGE, .u.flag = on};
    app_send(a, ev);
    if (on)
        push_log(a, "drone on. it resumes at %.1f hz.", a->drone_hz);
    else
        push_log(a, "drone off. the shell keeps breathing in silence.");
}

/* ---------- shell / sky constants ---------- */

#define SHELL_WHORLS 5
#define SHELL_GROWTH 2.0f
#define SHELL_TURNS 5.0f
#define SHELL_SPAN_CAP 0.35f
#define SHELL_RIB_MAX 3.0f
#define ENGRAVE_REACH_MIN 0.30f
#define ENGRAVE_REACH_RANGE 0.45f
#define SHELL_RIB_SWEEP 0.55f
#define SHELL_RIB_WAVE 0.22f
#define SHELL_RIB_INNER 2.0f
#define SUTURE_WAVE 0.035f
#define SUTURE_SPEED 0.35f
#define SUTURE_CYCLES_OPEN 21.0f
#define SUTURE_CYCLES_DAMPED 5.0f
#define RIPPLE_ALONG_OPEN 3.0f
#define RIPPLE_ALONG_DAMPED 0.8f
#define ECHO_MAX 2.0f
#define ECHO_STEP 0.17f
#define GHOST_MAX 5.0f
#define GHOST_GAIN 0.92f
#define RIPPLE_AMP 0.60f
#define RIPPLE_WRAP 2.0f
#define RIPPLE_SPEED 0.9f
#define AGITATE_SPEED 2.0f
#define SHELL_TREMBLE_PX 2.0f
#define SHELL_SEIZE TAU_F
#define SCALE_MIN 0.55f
#define SCALE_MAX 1.45f
#define SCALE_SMOOTH_S 1.8f
#define UMBILICUS_MAX 0.12f
#define INDEX_SMOOTH_S 0.6f
#define SHELL_MIN_PX 2.5f
#define SUTURE_STEP_PX 3.0f
#define SHELL_FILL 0.97f
#define ROCK_PERIOD_S 13.0
#define ROCK_SWAY 0.060f
#define ROCK_SPIN_S 377.0f
#define ROCK_TILT 0.075f
#define ROCK_TILT_2 0.34f
#define DREAD_SMOOTH_S 0.9f
#define AGITATE_RISE_S 20.0f
#define AGITATE_FALL_S 45.0f
#define STAR_COUNT 170
#define SKY_DRIFT 0.0045f
#define SKY_DRIFT_ANGLE 2.3999632f
#define SKY_WOBBLE 0.010f
#define SKY_CURVE 0.09f
#define SKY_CURVE_PERIOD_S 89.0
#define SKY_LENS 0.055f
#define SKY_LENS_MAX 0.40f
#define SKY_WARP 0.014f
#define SKY_EDGE 0.24f
#define SKY_SHIMMER_DUTY 0.34f
#define EYE_TRACK 0.34f
#define CULL_MARGIN 1.15f
#define TAU_D 6.283185307179586476925286766559

#define FADER_TEXT 13.0f
#define FADER_HOVER_SLOP 2.0f

#define ENV_KNOB_H 62.0f
#define ENV_KNOBS 5

static const float BAYER[4][4] = {
    {0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f},
    {12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f},
    {3.0f / 16.0f, 11.0f / 16.0f, 1.0f / 16.0f, 9.0f / 16.0f},
    {15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f, 5.0f / 16.0f},
};

/* ---------- pre-layout integrators ---------- */

static float wrap_tau(float x) {
    x = fmodf(x, TAU_F);
    if (x < 0.0f) x += TAU_F;
    return x;
}

static float scale_for(float master) {
    return SCALE_MIN + (SCALE_MAX - SCALE_MIN) * clampf(master, 0.0f, 1.0f);
}

static float integrity(const App *a) {
    return 100.0f * (1.0f - fmaxf(a->shadow.rip, a->shadow_verb.haunt));
}

static void update_dread(App *a) {
    float in = integrity(a);
    switch (a->dread) {
    case DREAD_NORMAL:
        if (in < 47.5f) a->dread = DREAD_LOW;
        break;
    case DREAD_LOW:
        if (in > 52.5f) a->dread = DREAD_NORMAL;
        else if (in < 12.5f) a->dread = DREAD_CRITICAL;
        break;
    case DREAD_CRITICAL:
        if (in > 17.5f) a->dread = DREAD_LOW;
        break;
    }
}

void centre_prelayout(App *a, Ui *ui) {
    float dt = clampf(ui->dt, 0.0f, 0.1f);
    float rate = 1.0f + AGITATE_SPEED * clampf(a->shadow.rip, 0.0f, 1.0f);
    a->ripple_phase = wrap_tau(a->ripple_phase + dt * RIPPLE_SPEED * rate);
    a->suture_phase = wrap_tau(a->suture_phase + dt * SUTURE_SPEED * rate);
    float k = 1.0f - expf(-dt / SCALE_SMOOTH_S);
    a->shell_scale += (scale_for(a->shadow.master_level) - a->shell_scale) * k;
    float scale = fmaxf(a->shell_scale, 0.05f);
    for (int i = 0; i < 3; i++) {
        float r = (float)(TAU_D / ROCK_PERIOD_S) * powi_f(PHI, i) / scale;
        a->rock_phase[i] = wrap_tau(a->rock_phase[i] + dt * r);
    }
    a->spin = wrap_tau(a->spin + dt * (TAU_F / ROCK_SPIN_S) / scale);
    float ki = 1.0f - expf(-dt / INDEX_SMOOTH_S);
    a->index_smooth +=
        (clampf(a->shadow.index, 0.0f, 1.0f) - a->index_smooth) * ki;
    update_dread(a);
    float kd = 1.0f - expf(-dt / DREAD_SMOOTH_S);
    float target = a->dread == DREAD_CRITICAL ? 1.0f : 0.0f;
    a->dread_level += (target - a->dread_level) * kd;
    float subversion =
        clampf(fmaxf(a->shadow.rip, a->shadow_verb.haunt), 0.0f, 1.0f);
    a->agitation = clampf(a->agitation
                              + dt * (subversion / AGITATE_RISE_S
                                      - a->agitation / AGITATE_FALL_S),
                          0.0f, 1.0f);
}

/* ---------- shell geometry ---------- */

typedef struct {
    P2 pivot, pole;
    float tilt, max_r;
} ShellPose;

typedef struct {
    float suture_ph, ripple_ph, damp;
    uint32_t echoes, ghosts;
    float ghost_spread, charge, index, eye_open;
    RatioMode ratio_mode;
    float level[NUM_OPS];
    float grown, stipple;
} ShellDrive;

static float shell_unit_r(float theta, float ph, float cycles) {
    float theta_max = SHELL_TURNS * TAU_F;
    return powf(SHELL_GROWTH, (theta - theta_max) / TAU_F)
           * (1.0f + SUTURE_WAVE * sinf(theta * cycles + ph));
}

static void shell_unit_bounds(P2 *lo_out, P2 *hi_out) {
    static bool init;
    static P2 lo, hi;
    if (!init) {
        float theta_max = SHELL_TURNS * TAU_F;
        lo = (P2){3.4e38f, 3.4e38f};
        hi = (P2){-3.4e38f, -3.4e38f};
        for (int k = 0; k <= 720; k++) {
            float theta = theta_max * (float)k / 720.0f;
            float r = powf(SHELL_GROWTH, (theta - theta_max) / TAU_F)
                      * (1.0f + SUTURE_WAVE);
            P2 p = {cosf(theta) * r, sinf(theta) * r};
            lo.x = fminf(lo.x, p.x);
            lo.y = fminf(lo.y, p.y);
            hi.x = fmaxf(hi.x, p.x);
            hi.y = fmaxf(hi.y, p.y);
        }
        init = true;
    }
    *lo_out = lo;
    *hi_out = hi;
}

static float shell_fit_r(float w, float h) {
    P2 lo, hi;
    shell_unit_bounds(&lo, &hi);
    P2 span = {hi.x - lo.x, hi.y - lo.y};
    float fit = fminf(w / span.x, h / span.y) * SHELL_FILL;
    fit = fmaxf(fit, 1.0f);
    /* SHELL_REF_STAGE = 348 x 441 */
    float ref = fminf(348.0f / span.x, 441.0f / span.y) * SHELL_FILL;
    ref = fmaxf(ref, 1.0f);
    return fmaxf(fit, ref);
}

static float tremble(const App *a) {
    return ((float)(a->frame_count / 2 % 3) - 1.0f) * a->dread_level;
}

static ShellPose shell_pose(const App *a, Rct rect) {
    float p0 = a->rock_phase[0], p1 = a->rock_phase[1], p2 = a->rock_phase[2];
    float w1 = sinf(p0);
    float w2 = sinf(p1);
    float tilt = (sinf(p1 + PI_F / 5.0f) + ROCK_TILT_2 * sinf(p2))
                     / (1.0f + ROCK_TILT_2) * ROCK_TILT
                 + a->spin;

    float probe_r = shell_fit_r(rct_w(rect), rct_h(rect));
    P2 lo, hi;
    shell_unit_bounds(&lo, &hi);
    float s = a->shell_scale;
    /* charge is identically 1: charge_hold() == 0, so no lunge */
    P2 sway = {w1 * probe_r * s * s * ROCK_SWAY,
               w2 * probe_r * s * s * ROCK_SWAY};
    P2 half = {(hi.x - lo.x) * 0.5f * probe_r, (hi.y - lo.y) * 0.5f * probe_r};
    float swing = sinf(ROCK_TILT);
    P2 headroom = {half.y * swing + SHELL_TREMBLE_PX, half.x * swing};
    P2 stutter = {tremble(a) * SHELL_TREMBLE_PX, 0.0f};
    float fitted_r = shell_fit_r(rct_w(rect) - 2.0f * headroom.x,
                                 rct_h(rect) - 2.0f * headroom.y);
    float max_r = fitted_r * s;
    P2 centre = rct_center(rect);
    P2 pole = {centre.x - (lo.x + hi.x) * 0.5f * max_r,
               centre.y - (lo.y + hi.y) * 0.5f * max_r};
    ShellPose out;
    out.pivot = (P2){centre.x + sway.x + stutter.x, centre.y + sway.y + stutter.y};
    out.pole = (P2){pole.x + sway.x + stutter.x, pole.y + sway.y + stutter.y};
    out.tilt = tilt;
    out.max_r = max_r;
    return out;
}

static ShellDrive shell_drive(const App *a) {
    float seize = 0.0f;
    if (a->dread_level > 0.001f) {
        uint64_t h = (a->frame_count * 2654435761ull) % 10007ull;
        seize = ((float)h / 10007.0f - 0.5f) * SHELL_SEIZE * a->dread_level;
    }
    ShellDrive d;
    d.suture_ph = a->suture_phase + seize;
    d.ripple_ph = a->ripple_phase + seize;
    d.damp = clampf(a->shadow_verb.damp, 0.0f, 1.0f);
    d.echoes = (uint32_t)roundf(clampf(a->shadow.feedback, 0.0f, 1.0f) * ECHO_MAX);
    d.ghosts = (uint32_t)roundf(clampf(a->shadow_verb.haunt, 0.0f, 1.0f) * GHOST_MAX);
    d.ghost_spread = 1.0f;
    d.charge = 1.0f;
    d.index = a->index_smooth;
    d.eye_open = 0.0f;
    d.ratio_mode = a->shadow.ratio_mode;
    for (int i = 0; i < NUM_OPS; i++) d.level[i] = a->env[i];
    d.grown = 1.0f;
    d.stipple = 0.0f;
    return d;
}

static void chambers_for(RatioMode mode, uint32_t out[SHELL_WHORLS]) {
    static const uint32_t T[RATIO_MODE_COUNT][SHELL_WHORLS] = {
        {30, 24, 18, 12, 6}, {34, 21, 13, 8, 5}, {29, 18, 11, 7, 4},
        {4, 7, 11, 18, 29},  {28, 21, 16, 12, 9},
    };
    int m = (int)mode;
    if (m < 0 || m >= RATIO_MODE_COUNT) m = 0;
    memcpy(out, T[m], sizeof(uint32_t) * SHELL_WHORLS);
}

/* ---------- the eye ---------- */

static void paint_eye(const App *a, Canvas *c, P2 pole, float eye_r, float open) {
    if (open <= 0.01f || eye_r < 2.0f) return;
    P2 look = {0.0f, 0.0f};
    P2 d = {a->pointer.x - pole.x, a->pointer.y - pole.y};
    float n = sqrtf(d.x * d.x + d.y * d.y);
    if (n > 1.0f) {
        look.x = d.x / n * eye_r * EYE_TRACK * open;
        look.y = d.y / n * eye_r * EYE_TRACK * open;
    }
    P2 ce = {pole.x + look.x, pole.y + look.y};
    float rim = eye_r * 2.2f;
    float cell = 2.0f;
    int steps = (int)ceilf(rim * 2.0f / cell);
    if (steps < 1) steps = 1;
    for (int iy = 0; iy < steps; iy++) {
        for (int ix = 0; ix < steps; ix++) {
            P2 off = {(float)ix * cell - rim, (float)iy * cell - rim};
            float dd = sqrtf(off.x * off.x + off.y * off.y);
            if (dd < eye_r || dd > rim) continue;
            float k = 1.0f - (dd - eye_r) / (rim - eye_r);
            if (k * k * open > BAYER[iy & 3][ix & 3])
                draw_rect_filled(c,
                                 rct_xywh(ce.x + off.x, ce.y + off.y,
                                          cell - 1.0f, cell - 1.0f),
                                 PAPER);
        }
    }
    dither_circle_ink(c, ce, eye_r, open, 2.0f, INK_BLACK);
}

/* ---------- the ammonite ---------- */

typedef struct {
    Canvas *c;
    P2 pole;
    float tilt, max_r, suture_ph, cycles, along, ripple_clock, stipple;
    uint32_t echoes;
} MonoCtx;

static float mc_r_at(const MonoCtx *m, float t) {
    return m->max_r * shell_unit_r(t, m->suture_ph, m->cycles);
}

static P2 mc_at(const MonoCtx *m, float t, float r) {
    float a = t + m->tilt;
    return (P2){roundf(m->pole.x + cosf(a) * r),
                roundf(m->pole.y + sinf(a) * r)};
}

static void mc_line(const MonoCtx *m, const P2 *pts, int n, float w,
                    uint64_t seed) {
    if (n < 2) return;
    if (m->stipple <= 0.001f) {
        draw_polyline(m->c, pts, (size_t)n, w, PAPER);
        return;
    }
    for (int k = 0; k < n - 1; k++) {
        uint64_t h = seed * 0x9E3779B97F4A7C15ull
                     ^ (uint64_t)k * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        if ((float)(h % 100000ull) / 100000.0f >= m->stipple)
            draw_line(m->c, pts[k], pts[k + 1], w, PAPER);
    }
}

static float rib_offset(float u, float span, float waviness, float along,
                        float phase) {
    float s = fminf(span, SHELL_SPAN_CAP);
    float lean = SHELL_RIB_SWEEP * s * (1.0f - u);
    float wave = SHELL_RIB_WAVE * s * waviness * sinf(u * TAU_F);
    float ripple =
        RIPPLE_AMP * s * sinf(u * PI_F * along + phase) * sinf(u * PI_F);
    return -lean + wave + ripple;
}

static void mc_rib(const MonoCtx *m, float t, float r_inner, float r_outer,
                   float w, float span, float waviness, uint64_t seed) {
    float ph = m->ripple_clock + t * RIPPLE_WRAP * (r_outer / m->max_r);
    const int n = 14;
    P2 pts[15];
    for (int k = 0; k <= n; k++) {
        float u = (float)k / (float)n;
        pts[k] = mc_at(m, t + rib_offset(u, span, waviness, m->along, ph),
                       r_inner + (r_outer - r_inner) * u);
    }
    mc_line(m, pts, n + 1, w, seed);
    for (uint32_t e = 1; e <= m->echoes; e++) {
        float off = ECHO_STEP * fminf(span, SHELL_SPAN_CAP) * (float)e;
        P2 ep[15];
        for (int k = 0; k <= n; k++) {
            float u = (float)k / (float)n;
            ep[k] = mc_at(m, t + rib_offset(u, span, waviness, m->along, ph) + off,
                          r_inner + (r_outer - r_inner) * u);
        }
        for (int k = 0; k + 1 <= n; k += 2)
            draw_line(m->c, ep[k], ep[k + 1], 1.0f, PAPER);
    }
}

#define RUN_CAP 8192

static void draw_monolith(const App *a, Canvas *c, Rct rect,
                          const ShellPose *pose, const ShellDrive *drive) {
    if (fminf(rct_w(rect), rct_h(rect)) < 16.0f) return;
    float theta_max = SHELL_TURNS * TAU_F;
    float damp = drive->damp;
    MonoCtx m;
    m.c = c;
    m.pole = pose->pole;
    m.tilt = pose->tilt;
    m.max_r = pose->max_r;
    m.suture_ph = drive->suture_ph;
    m.cycles = SUTURE_CYCLES_OPEN + (SUTURE_CYCLES_DAMPED - SUTURE_CYCLES_OPEN) * damp;
    m.along = RIPPLE_ALONG_OPEN + (RIPPLE_ALONG_DAMPED - RIPPLE_ALONG_OPEN) * damp;
    m.ripple_clock = drive->ripple_ph;
    m.stipple = clampf(drive->stipple, 0.0f, 1.0f);
    m.echoes = drive->echoes;

    float bore_r = m.max_r / (1.0f + (drive->charge - 1.0f));
    float eye = fminf(SHELL_MIN_PX + bore_r * UMBILICUS_MAX * (1.0f - drive->index),
                      m.max_r * 0.9f);
    paint_eye(a, c, m.pole, eye, drive->eye_open);

    Rct clip = canvas_clip(c);
    float dx = fmaxf(fabsf(clip.x0 - m.pole.x), fabsf(clip.x1 - m.pole.x));
    float dy = fmaxf(fabsf(clip.y0 - m.pole.y), fabsf(clip.y1 - m.pole.y));
    float far = sqrtf(dx * dx + dy * dy) * CULL_MARGIN + 12.0f;
    float t_clip;
    if (far >= m.max_r || !isfinite(far))
        t_clip = theta_max;
    else
        t_clip = clampf(theta_max + TAU_F * logf(far / m.max_r) / logf(SHELL_GROWTH),
                        0.0f, theta_max);
    float t_far = fminf(t_clip, theta_max * clampf(drive->grown, 0.0f, 1.0f));

    static P2 run[RUN_CAP];
    int run_len = 0;
    bool run_heavy = false;
    uint64_t seam = 0;
    float t = 0.0f;
    while (t <= t_far) {
        float r = mc_r_at(&m, t);
        bool heavy = t > theta_max - TAU_F;
        if (r < eye || heavy != run_heavy) {
            mc_line(&m, run, run_len, run_heavy ? 2.0f : 1.0f, seam);
            run_len = 0;
            seam++;
            run_heavy = heavy;
        }
        if (r >= eye) {
            if (run_len == RUN_CAP) {
                mc_line(&m, run, run_len, run_heavy ? 2.0f : 1.0f, seam);
                run[0] = run[run_len - 1];
                run_len = 1;
            }
            run[run_len++] = mc_at(&m, t, r);
        }
        t += clampf(SUTURE_STEP_PX / fmaxf(r, 1.0f), 0.004f, 0.30f);
    }
    mc_line(&m, run, run_len, run_heavy ? 2.0f : 1.0f, seam);

    for (uint32_t p = 1; p <= drive->ghosts; p++) {
        float turn = (float)p * PI_F / 5.0f * drive->ghost_spread;
        float step = SUTURE_STEP_PX / powi_f(GHOST_GAIN, (int)p * 4);
        float gt = 0.0f;
        while (gt <= t_far) {
            float r = mc_r_at(&m, gt);
            if (r >= eye) {
                float ang = gt + m.tilt + turn;
                draw_dot(c,
                         (P2){roundf(m.pole.x + cosf(ang) * r),
                              roundf(m.pole.y + sinf(ang) * r)},
                         PAPER);
            }
            gt += clampf(step / fmaxf(r, 1.0f), 0.004f, 0.30f);
        }
    }

    uint32_t chamber_counts[SHELL_WHORLS];
    chambers_for(drive->ratio_mode, chamber_counts);
    uint32_t chamber_index = 0;
    for (int whorl = 0; whorl < SHELL_WHORLS; whorl++) {
        uint32_t chambers = chamber_counts[whorl];
        float outer_t = theta_max - (float)whorl * TAU_F;
        if (outer_t - TAU_F > t_far) {
            chamber_index += chambers;
            continue;
        }
        float span = TAU_F / (float)chambers;
        for (uint32_t j = 0; j < chambers; j++) {
            float ct = outer_t - (float)j * span;
            int op = (int)(chamber_index % NUM_OPS);
            chamber_index++;
            if (ct < 0.0f) continue;
            float r_in = fmaxf(mc_r_at(&m, ct - TAU_F), eye);
            float r_out = mc_r_at(&m, ct);
            if (r_out < eye + SHELL_MIN_PX) continue;
            mc_rib(&m, ct, fmaxf(r_in, 1.0f), r_out, 1.0f, span, 1.0f,
                   0x5E00ull + chamber_index);
            float level = fminf(drive->level[op], 1.0f);
            uint32_t ribs = 1 + (uint32_t)roundf(level * SHELL_RIB_MAX);
            while (ribs > 1 && span * r_out / (float)ribs < 3.0f) ribs--;
            float reach = ENGRAVE_REACH_MIN + ENGRAVE_REACH_RANGE * level;
            for (uint32_t mm = 1; mm < ribs; mm++) {
                float rt = ct - span * (float)mm / (float)ribs;
                if (rt < 0.0f) continue;
                float ri = fmaxf(mc_r_at(&m, rt - TAU_F), eye);
                float ro = mc_r_at(&m, rt);
                if (ro < eye + SHELL_MIN_PX) continue;
                mc_rib(&m, rt, ro - (ro - ri) * reach, ro, 1.0f, span,
                       SHELL_RIB_INNER,
                       0xB100ull + (uint64_t)chamber_index * 8 + mm);
            }
        }
    }

    if (theta_max - TAU_F <= t_far)
        mc_rib(&m, theta_max, mc_r_at(&m, theta_max - TAU_F),
               mc_r_at(&m, theta_max), 2.0f,
               TAU_F / (float)chamber_counts[0], 1.0f, 0xA9E9ull);
}

/* ---------- starfield ---------- */

static void draw_starfield(const App *a, Canvas *c, Rct rect,
                           const ShellPose *pose) {
    double t = a->last_frame_time;
    float tf = (float)t;
    float sw = rct_w(rect), sh = rct_h(rect);
    float curve = SKY_CURVE * (float)sin(t / SKY_CURVE_PERIOD_S * TAU_D);
    float drift_x = cosf(SKY_DRIFT_ANGLE), drift_y = sinf(SKY_DRIFT_ANGLE);
    P2 centre = rct_center(rect);

    for (int i = 0; i < STAR_COUNT; i++) {
        uint64_t h = (uint64_t)i * 0x9E3779B97F4A7C15ull;
        uint64_t kind = (h >> 5) & 31;
        float depth = (float)((h >> 21) & 3) / 3.0f;
        float near = 0.35f + 0.65f * depth;
        double phase = (double)((h >> 41) & 0xFFFF) / 65535.0 * TAU_D;

        float fx = fract_pos((float)((h >> 11) & 0xFFFF) / 65535.0f
                             + tf * SKY_DRIFT * near * drift_x);
        float fy = fract_pos((float)((h >> 27) & 0xFFFF) / 65535.0f
                             + tf * SKY_DRIFT * near * drift_y);
        float edge = fmaxf(fminf(1.0f - fabsf(fx - 0.5f) * 2.0f,
                                 1.0f - fabsf(fy - 0.5f) * 2.0f),
                           0.0f);
        edge = clampf(1.0f - edge / SKY_EDGE, 0.0f, 1.0f);

        fx += (float)sin(t / 34.0 * TAU_D + phase) * SKY_WOBBLE * near;
        fy += (float)sin(t / 21.0 * TAU_D + phase) * SKY_WOBBLE * near;

        P2 u = {fx - 0.5f, fy - 0.5f};
        float lensq = u.x * u.x + u.y * u.y;
        u.x *= 1.0f + curve * lensq;
        u.y *= 1.0f + curve * lensq;

        float nx = u.x * 6.283f, ny = u.y * 6.283f;
        u.x += sinf(nx * 1.0f + (float)(t * 0.11)) * cosf(ny * 1.3f - (float)(t * 0.07))
               * SKY_WARP;
        u.y += sinf(ny * 0.8f - (float)(t * 0.09)) * cosf(nx * 1.7f + (float)(t * 0.13))
               * SKY_WARP;

        P2 p = {centre.x + u.x * sw, centre.y + u.y * sh};

        P2 off = {p.x - pose->pivot.x, p.y - pose->pivot.y};
        float d = sqrtf(off.x * off.x + off.y * off.y);
        if (d > 0.5f) {
            float push = fminf(SKY_LENS * pose->max_r * pose->max_r / d,
                               SKY_LENS_MAX * pose->max_r);
            p.x += off.x / d * push;
            p.y += off.y / d * push;
        }

        if (edge > 0.0f) {
            double rate = 1.3 + 1.7 * (double)((h >> 17) & 15) / 15.0;
            float blink = 0.5f + 0.5f * (float)sin(t * rate + phase);
            if (blink < edge * SKY_SHIMMER_DUTY) continue;
        }

        p.x = roundf(p.x);
        p.y = roundf(p.y);
        if (kind == 0) {
            draw_rect_filled(c, rct(p.x - 1.0f, p.y - 1.0f, p.x + 1.0f, p.y + 1.0f),
                             PAPER);
            for (int di = 0; di < 2; di++) {
                float dd = di == 0 ? 2.0f : 3.0f;
                draw_dot(c, (P2){p.x + dd, p.y}, PAPER);
                draw_dot(c, (P2){p.x - dd, p.y}, PAPER);
                draw_dot(c, (P2){p.x, p.y + dd}, PAPER);
                draw_dot(c, (P2){p.x, p.y - dd}, PAPER);
            }
        } else if (kind <= 5) {
            draw_rect_filled(c, rct(p.x - 1.0f, p.y - 1.0f, p.x + 1.0f, p.y + 1.0f),
                             PAPER);
        } else {
            draw_dot(c, p, PAPER);
        }
    }
}

void draw_stage(App *a, Ui *ui, Rct r) {
    Rct stage = rct_shrink(r, 4.0f);
    ShellPose pose = shell_pose(a, stage);
    draw_starfield(a, ui->canvas, stage, &pose);
    ShellDrive drive = shell_drive(a);
    draw_monolith(a, ui->canvas, stage, &pose, &drive);
}

/* ---------- scopes ---------- */

#define FRAC_1_SQRT_2 0.70710678118654752440f

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

/* ---------- envelope ---------- */

static EnvParams shadow_env(const App *a) {
    EnvParams p = env_params_default();
    p.attack_s = a->shadow_attack_s;
    p.decay_s = a->shadow_decay_s;
    p.sustain = a->shadow_sustain;
    p.release_s = a->shadow_release_s;
    p.curve = a->shadow.curve;
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

static void time_text(char *out, size_t cap, float s) {
    if (s < 0.1f) snprintf(out, cap, "%.0f ms", (double)(s * 1000.0f));
    else snprintf(out, cap, "%.2f s", (double)s);
}

static void envelope_knobs(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    EnvParams def = env_params_default();
    Patch pd = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    bool reaches = a->chain.amp.kind == AMP_ENVELOPE || a->shadow_melody.enabled;
    float kw = rct_w(r) / (float)ENV_KNOBS;
    char val[32];
    for (int k = 0; k < ENV_KNOBS; k++) {
        Rct kr = rct(roundf(r.x0 + kw * (float)k), r.y0,
                     roundf(r.x0 + kw * (float)(k + 1)), r.y1);
        UiId id = ui_id_n("env knob", k);
        FaderAct act;
        switch (k) {
        case 0:
        case 1: {
            float *v = k == 0 ? &a->shadow_attack_s : &a->shadow_decay_s;
            float lo = k == 0 ? ENV_ATTACK_MIN : 0.0f;
            time_text(val, sizeof val, *v);
            act = knob_track(ui, id, kr, k == 0 ? "attack" : "decay", val,
                             env_time_pos(*v, lo));
            if (reaches && act.kind == FADER_SET) *v = env_time_at(act.t, lo);
            if (reaches && act.kind == FADER_RESET)
                *v = k == 0 ? def.attack_s : def.decay_s;
            break;
        }
        case 2:
            snprintf(val, sizeof val, "%.2f", (double)a->shadow_sustain);
            act = knob_track(ui, id, kr, "sustain", val, a->shadow_sustain);
            if (reaches && act.kind == FADER_SET) a->shadow_sustain = act.t;
            if (reaches && act.kind == FADER_RESET) a->shadow_sustain = def.sustain;
            break;
        case 3:
            time_text(val, sizeof val, a->shadow_release_s);
            act = knob_track(ui, id, kr, "release", val,
                             env_time_pos(a->shadow_release_s, ENV_RELEASE_MIN));
            if (reaches && act.kind == FADER_SET)
                a->shadow_release_s = env_time_at(act.t, ENV_RELEASE_MIN);
            if (reaches && act.kind == FADER_RESET) a->shadow_release_s = def.release_s;
            break;
        default: {
            /* curve also bends the field, so it answers under the drone too */
            float bend = a->shadow.curve * 2.0f - 1.0f;
            snprintf(val, sizeof val, "%+.2f", (double)bend);
            act = knob_track(ui, id, kr, "curve", val, a->shadow.curve);
            float next = a->shadow.curve;
            if (act.kind == FADER_SET) next = act.t;
            else if (act.kind == FADER_RESET) next = pd.curve;
            if (next != a->shadow.curve) {
                a->shadow.curve = next;
                app_send(a, (Event){.kind = EV_SET_PATCH, .u.patch = a->shadow});
            }
            continue;
        }
        }
        if (reaches) continue;
        dither_rect_ink(c, kr, VEIL, 2.0f, INK_BLACK);
        if (ui->in.pressed && ui->in.mouse_in_window && rct_contains(kr, ui->in.mouse)) {
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
}

static void draw_envelope_view(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, r);
    Rct content = rct_shrink(r, 6.0f);
    float knobs_h = fminf(ENV_KNOB_H, rct_h(content));
    float plot_bottom = fmaxf(content.y0, content.y1 - knobs_h - GROUP);
    paint_envelope(a, c, rct(content.x0, content.y0, content.x1, plot_bottom));
    envelope_knobs(a, ui,
                   rct(content.x0, plot_bottom + GROUP, content.x1,
                       content.y1));

    canvas_set_clip(c, saved);
}

/* ---------- contextual display ---------- */

static void display_level_value(char *out, size_t cap, float value) {
    snprintf(out, cap, "%.2f", (double)clampf(value, 0.0f, 1.0f));
    if (out[0] == '0') memmove(out, out + 1, strlen(out));
}

static void draw_operator_view(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, r);
    Rct content = rct_shrink(r, 6.0f);
    FontId heading = ui_font(9.0f);
    float y = content.y0;

    /* The algorithm bench proved that five oscillators are easier to read as
       one table. Here the table is part of the instrument, so its values are
       controls instead of diagnostics. */
    float op_w = 25.0f;
    float controls_x = content.x0 + op_w + GROUP;
    float controls_w = content.x1 - controls_x;
    float ratio_w = floorf((controls_w - GROUP) * GOLDEN_MAJOR);
    float level_w = controls_w - GROUP - ratio_w;

    text_draw(c, heading, (P2){content.x0, y}, ALIGN_LEFT_TOP, "OP", PAPER,
              1.0f);
    text_draw(c, heading, (P2){controls_x, y}, ALIGN_LEFT_TOP, "RATIO", PAPER,
              1.0f);
    text_draw(c, heading, (P2){controls_x + ratio_w + GROUP, y}, ALIGN_LEFT_TOP,
              "LEVEL", PAPER, 1.0f);
    y += text_row_height(heading) + TIGHT;

    Patch defaults = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    bool changed = false;
    for (int i = 0; i < NUM_OPS; i++) {
        bool enabled = a->shadow.ops[i].enabled;
        char op[8], value[32];
        snprintf(op, sizeof op, "%d", i + 1);
        if (chip_button(ui, ui_id_n("display.op", i),
                        rct_xywh(content.x0, y, op_w, FADER_H), op, enabled)) {
            a->shadow.ops[i].enabled = !enabled;
            changed = true;
        }

        Rct ratio_r = rct_xywh(controls_x, y, ratio_w, FADER_H);
        snprintf(value, sizeof value, "%.2f", (double)a->shadow.ops[i].ratio);
        FaderAct ratio = fader_track(ui, ui_id_n("display.ratio", i), ratio_r,
                                     "", value,
                                     position_of_log(a->shadow.ops[i].ratio,
                                                     OP_RATIO_MIN,
                                                     OP_RATIO_MAX));
        if (ratio.kind == FADER_SET) {
            float next = log_position(ratio.t, OP_RATIO_MIN, OP_RATIO_MAX);
            if (next != a->shadow.ops[i].ratio) {
                a->shadow.ops[i].ratio = next;
                changed = true;
            }
        } else if (ratio.kind == FADER_RESET
                   && a->shadow.ops[i].ratio != defaults.ops[i].ratio) {
            a->shadow.ops[i].ratio = defaults.ops[i].ratio;
            changed = true;
        }

        Rct level_r = rct_xywh(controls_x + ratio_w + GROUP, y, level_w,
                               FADER_H);
        display_level_value(value, sizeof value, a->shadow.ops[i].level);
        FaderAct level = fader_track(ui, ui_id_n("display.level", i), level_r,
                                     "", value,
                                     clampf(a->shadow.ops[i].level, 0.0f, 1.0f));
        if (level.kind == FADER_SET) {
            if (level.t != a->shadow.ops[i].level) {
                a->shadow.ops[i].level = level.t;
                changed = true;
            }
        } else if (level.kind == FADER_RESET
                   && a->shadow.ops[i].level != 1.0f) {
            /* Level is relative operator gain: its neutral/default value is
               unity, independent of the algorithm's initial depth trim. */
            a->shadow.ops[i].level = 1.0f;
            changed = true;
        }
        y += FADER_H + GROUP;
    }
    if (changed) {
        Event ev = {.kind = EV_SET_PATCH, .u.patch = a->shadow};
        app_send(a, ev);
    }
    canvas_set_clip(c, saved);
}

/* This is deliberately short enough to live in the instrument. Full licence
   text remains beside the binary in THIRD-PARTY-LICENSES.txt. */
static void draw_info_view(App *a, Ui *ui, Rct r) {
    (void)a;
    Canvas *c = ui->canvas;
    FontId heading = ui_font(12.0f);
    FontId body = ui_font(11.0f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, r);
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
        "BYPOSerif, from Source Serif 4  Adobe  SIL OFL 1.1",
        "European Teletext  Jayvee Enaguas  CC0 1.0",
        "Unifont Ex Mono  stgiga / GNU Unifont  SIL OFL 1.1",
        "SDL2  zlib License     FreeType  FTL",
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
    canvas_set_clip(c, saved);
}

void draw_display_cell(App *a, Ui *ui, Rct r) {
    static const char *const TABS[3] = {"OPERATORS", "ENVELOPE", "I"};
    float tab_h = text_row_height(ui_font(12.0f * 1.3f)) + 2.0f * 2.0f
                  + 2.0f * SNUG;
    int hit = wave_tabs(ui, ui_id("display.tabs"),
                        rct_xywh(r.x0, r.y0, rct_w(r), tab_h), TABS, 3,
                        a->display_tab);
    if (hit >= 0) a->display_tab = hit;

    Rct page = rct(r.x0, r.y0 + tab_h + CELL_GUTTER, r.x1, r.y1);
    if (a->display_tab == 0)
        draw_operator_view(a, ui, page);
    else if (a->display_tab == 1)
        draw_envelope_view(a, ui, page);
    else
        draw_info_view(a, ui, page);
}

/* ---------- the controls house ---------- */

static bool press_on(const Ui *ui, Rct r) {
    return ui->in.pressed && ui->in.mouse_in_window
           && rct_contains(r, ui->in.mouse);
}

void draw_controls_house(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, r);
    Rct content = rct_shrink(r, 6.0f);
    float x0 = content.x0, x1 = content.x1;
    float w = x1 - x0;

    int idx = algorithm_index_of(&a->shadow);
    Rct below = window_chrome_tagged(c, content, "PARAMETERS",
                                     idx < 5 ? "the unfolding" : "the folding");
    float y = below.y0 + GROUP;

    /* algorithm roster: 5 + 3 */
    float slot = fmaxf(floorf((w - 4.0f * GROUP) / 5.0f), 24.0f);
    float indent = slot + GROUP;
    int clicked = -1;
    for (int i = 0; i < 5; i++) {
        Rct b = rct_xywh(x0 + (float)i * (slot + GROUP), y, slot, 21.0f);
        if (pane_button(ui, ui_id_n("house alg", i), b, ROMAN[i], i == idx)
            && i != idx)
            clicked = i;
    }
    y += 21.0f + GROUP;
    for (int i = 5; i < 8; i++) {
        Rct b = rct_xywh(x0 + indent + (float)(i - 5) * (slot + GROUP), y, slot,
                         21.0f);
        if (pane_button(ui, ui_id_n("house alg", i), b, ROMAN[i], i == idx)
            && i != idx)
            clicked = i;
    }
    y += 21.0f + GROUP;
    if (clicked >= 0) app_set_algorithm(a, clicked);

    bool patch_changed = false;
    Patch pd = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    char val[48];

    /* INDEX: phi-warped position */
    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        float pos = powf(clampf(a->shadow.index, 0.0f, 1.0f), 1.0f / PHI);
        float def = powf(clampf(pd.index, 0.0f, 1.0f), 1.0f / PHI);
        snprintf(val, sizeof val, "%.3f", powf(pos, PHI));
        FaderAct act = fader_track(ui, ui_id("house INDEX"), row, "INDEX", val, pos);
        float next = pos;
        if (act.kind == FADER_SET) next = act.t;
        else if (act.kind == FADER_RESET) next = def;
        if (next != pos) {
            a->shadow.index = powf(next, PHI);
            patch_changed = true;
        }
        y += FADER_H + GROUP;
    }

    /* RIP: trembles sideways under dread */
    {
        float t = tremble(a);
        Rct row = rct(x0 + 2.0f + roundf(t), y, x1, y + FADER_H);
        snprintf(val, sizeof val, "%.2f", a->shadow.rip);
        FaderAct act = fader_track(ui, ui_id("house RIP"), row, "RIP", val,
                                   clampf(a->shadow.rip, 0.0f, 1.0f));
        float next = a->shadow.rip;
        if (act.kind == FADER_SET) next = act.t;
        else if (act.kind == FADER_RESET) next = pd.rip;
        if (next != a->shadow.rip) {
            a->shadow.rip = next;
            patch_changed = true;
        }
        y += FADER_H + GROUP;
    }

    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        snprintf(val, sizeof val, "%.2f", a->shadow.feedback);
        FaderAct act = fader_track(ui, ui_id("house fb"), row, "fb", val,
                                   clampf(a->shadow.feedback, 0.0f, 1.0f));
        float next = a->shadow.feedback;
        if (act.kind == FADER_SET) next = act.t;
        else if (act.kind == FADER_RESET) next = pd.feedback;
        if (next != a->shadow.feedback) {
            a->shadow.feedback = next;
            patch_changed = true;
        }
        y += FADER_H + GROUP;
    }

    /* glide s: t^(phi^4) over 0..2 s */
    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        float glide_exp = PHI * PHI * PHI * PHI;
        float pos = powf(clampf(a->shadow.glide_seconds, 0.0f, 2.0f) / 2.0f,
                         1.0f / glide_exp);
        float def = powf(clampf(pd.glide_seconds, 0.0f, 2.0f) / 2.0f,
                         1.0f / glide_exp);
        snprintf(val, sizeof val, "%.3f", 2.0f * powf(pos, glide_exp));
        FaderAct act =
            fader_track(ui, ui_id("house glide s"), row, "glide s", val, pos);
        float next = pos;
        if (act.kind == FADER_SET) next = act.t;
        else if (act.kind == FADER_RESET) next = def;
        if (next != pos) {
            a->shadow.glide_seconds = 2.0f * powf(next, glide_exp);
            patch_changed = true;
        }
        y += FADER_H + GROUP;
    }

    /* drone hz: log fader, veiled while midi or the sequencer own pitch */
    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        float hz = a->drone_hz;
        float t = position_of_log(hz, 27.5f, 440.0f);
        snprintf(val, sizeof val, "%.1f", hz);
        FaderAct act =
            fader_track(ui, ui_id("house drone hz"), row, "drone hz", val, t);
        bool pressed = press_on(ui, row);
        if (midi_driving(a)) {
            dither_rect_ink(c, row, VEIL, 2.0f, INK_BLACK);
            if (pressed)
                push_log(a, "midi is connected, so the instrument is "
                            "configured for midi — this control is idle until "
                            "it's unplugged.");
        } else if (a->shadow_melody.enabled) {
            dither_rect_ink(c, row, VEIL, 2.0f, INK_BLACK);
            if (pressed)
                push_log(a, "the sequencer owns the pitch while it runs — "
                            "switch it off to hand this slider back.");
        } else {
            float next = hz;
            if (act.kind == FADER_SET) next = log_position(act.t, 27.5f, 440.0f);
            else if (act.kind == FADER_RESET) next = START_HZ;
            a->drone_hz = next;
            if (next != hz) {
                Event ev = {.kind = EV_GLIDE_TO, .u.f = a->drone_hz};
                app_send(a, ev);
            }
        }
        y += FADER_H + GROUP;
    }

    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        snprintf(val, sizeof val, "%.2f", a->shadow.field);
        FaderAct act = fader_track(ui, ui_id("house field"), row, "field", val,
                                   clampf(a->shadow.field, 0.0f, 1.0f));
        float next = a->shadow.field;
        if (act.kind == FADER_SET) next = act.t;
        else if (act.kind == FADER_RESET) next = pd.field;
        if (next != a->shadow.field) {
            a->shadow.field = next;
            patch_changed = true;
        }
        y += FADER_H + GROUP;
    }

    float strip_h = text_row_height(ui_font(12.0f)) + 2.0f * SNUG;

    /* voices: mono or poly, unison on top of either */
    y += GROUP;
    inverted_strip(c, rct(x0, y, x1, y + strip_h), "VOICES");
    y += strip_h + GROUP;
    {
        static const char *const NAMES[3] = {"mono", "poly 4", "unison"};
        FontId f = ui_font(12.0f);
        float cx = x0;
        int hit = -1;
        for (int i = 0; i < 3; i++) {
            bool active = i == 0   ? a->shadow.voices <= 1
                          : i == 1 ? a->shadow.voices > 1
                                   : a->shadow.unison > 1;
            float cw = text_width(f, NAMES[i], 0.0f) + 2.0f * GAP;
            if (i == 2) cx += GROUP;
            if (cx > x0 && cx + cw > x1) {
                cx = x0;
                y += 21.0f + GROUP;
            }
            if (chip_button(ui, ui_id_n("house voices", i),
                            rct_xywh(cx, y, cw, 21.0f), NAMES[i], active)
                && (i == 2 || !active))
                hit = i;
            cx += cw + GROUP;
        }
        y += 21.0f + GROUP;
        bool drone_holds = a->chain.amp.kind != AMP_ENVELOPE;
        if (hit == 0) {
            a->shadow.voices = 1;
            patch_changed = true;
            push_log(a, "mono. one voice; a new note glides out of the last.");
        } else if (hit == 1) {
            a->shadow.voices = POLY_MAX;
            patch_changed = true;
            if (drone_holds)
                push_log(a, "poly 4 is set, but the drone is one voice. switch "
                            "it off and notes stack up to four.");
            else
                push_log(a, "poly 4. up to four notes at once; a fifth takes "
                            "the oldest.");
        } else if (hit == 2) {
            a->shadow.unison = a->shadow.unison > 1 ? 1 : UNISON_MAX;
            patch_changed = true;
            if (a->shadow.unison > 1)
                push_log(a, "unison. two voices a note, %.1f cents apart, "
                            "spread left and right.",
                         (double)a->shadow.unison_detune);
            else
                push_log(a, "unison off. one voice a note.");
        }
    }

    /* detune: squared, so the narrow beating end gets most of the travel */
    {
        Rct row = rct(x0, y, x1, y + FADER_H);
        float d = a->shadow.unison_detune;
        float pos = sqrtf(clampf(d / UNISON_DETUNE_MAX, 0.0f, 1.0f));
        snprintf(val, sizeof val, "%.1f ct", d);
        FaderAct act =
            fader_track(ui, ui_id("house detune"), row, "detune", val, pos);
        if (a->shadow.unison > 1) {
            float next = d;
            if (act.kind == FADER_SET)
                next = UNISON_DETUNE_MAX * act.t * act.t;
            else if (act.kind == FADER_RESET)
                next = pd.unison_detune;
            if (next != d) {
                a->shadow.unison_detune = next;
                patch_changed = true;
            }
        } else {
            dither_rect_ink(c, row, VEIL, 2.0f, INK_BLACK);
            if (press_on(ui, row))
                push_log(a, "detune spreads the unison pair. switch unison on "
                            "to hear it.");
        }
        y += FADER_H + GROUP;
    }

    if (patch_changed) {
        Event ev = {.kind = EV_SET_PATCH, .u.patch = a->shadow};
        app_send(a, ev);
    }

    /* ratio palettes: an explicit load, never a side effect of topology */
    y += GROUP;
    inverted_strip(c, rct(x0, y, x1, y + strip_h), "RATIO PALETTE");
    y += strip_h + GROUP;
    {
        FontId f = ui_font(12.0f);
        float cx = x0;
        int new_mode = -1;
        for (int m = 0; m < RATIO_MODE_COUNT; m++) {
            const char *name = mode_name_of((RatioMode)m);
            float cw = text_width(f, name, 0.0f) + 2.0f * GAP;
            if (cx > x0 && cx + cw > x1) {
                cx = x0;
                y += 21.0f + GROUP;
            }
            Rct chip = rct_xywh(cx, y, cw, 21.0f);
            bool active = a->shadow.ratio_mode == (RatioMode)m;
            if (chip_button(ui, ui_id_n("house mode", m), chip, name, active)
                && !active)
                new_mode = m;
            cx += cw + GROUP;
        }
        y += 21.0f + GROUP;
        if (new_mode >= 0) {
            RatioMode mode = (RatioMode)new_mode;
            patch_apply_ratio_mode(&a->shadow, mode);
            Event ev = {.kind = EV_SET_PATCH, .u.patch = a->shadow};
            app_send(a, ev);
            char ratios[128];
            size_t at = 0;
            for (int i = 0; i < NUM_OPS; i++)
                at += (size_t)snprintf(ratios + at, sizeof ratios - at,
                                       i ? " %.3f" : "%.3f",
                                       (double)a->shadow.ops[i].ratio);
            push_log(a, "ratio palette %s loaded. op ratios: %s.", mode_name_of(mode),
                     ratios);
        }
    }

    y += GROUP;
    inverted_strip(c, rct(x0, y, x1, y + strip_h), "PHASE");
    y += strip_h + GROUP;
    paint_phase(a, c, rct(x0 + GROUP, y, x1 - GROUP,
                          fmaxf(content.y1 - GROUP, y + 40.0f)));

    canvas_set_clip(c, saved);
}
