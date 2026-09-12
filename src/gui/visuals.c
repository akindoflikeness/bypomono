#include <float.h>
#include <math.h>
#include <stdint.h>

#include "app.h"

#define WHORLS 5
#define GROWTH 2.0f
#define TURNS 5.0f

static const uint32_t CHAMBERS[WHORLS] = {34, 21, 13, 8, 5};

#define SPAN_CAP 0.35f
#define RIB_SWEEP 0.55f
#define RIB_WAVE 0.22f
#define RIB_INNER 2.0f
#define RIB_MAX 3.0f
#define SUTURE_WAVE 0.035f
#define RIPPLE_AMP 0.60f
#define RIPPLE_WRAP 2.0f
#define ENGRAVE_REACH_MIN 0.30f
#define ENGRAVE_REACH_RANGE 0.45f
#define UMBILICUS_MAX 0.12f
#define MIN_PX 2.5f
#define STEP_PX 3.0f
#define FILL 0.97f
#define CULL_MARGIN 1.15f
#define GHOST_GAIN 0.92f
#define GHOST_MAX 5u
#define MAX_PERSPECTIVE 12.0f

static float hash01(uint64_t a, uint64_t b) {
    uint64_t h = a * 0x9E3779B97F4A7C15ULL ^ b * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 32;
    return (float)(h % 100000ULL) / 100000.0f;
}

float logalith_unit_r(float theta, float ph, float cycles) {
    float theta_max = TURNS * TAU_F;
    return powf(GROWTH, (theta - theta_max) / TAU_F) *
           (1.0f + SUTURE_WAVE * sinf(theta * cycles + ph));
}

static float rib_offset(float u, float span, float waviness, float along,
                        float phase) {
    float s = fminf(span, SPAN_CAP);
    float lean = RIB_SWEEP * s * (1.0f - u);
    float wave = RIB_WAVE * s * waviness * sinf(u * TAU_F);
    float ripple = RIPPLE_AMP * s * sinf(u * PI_F * along + phase) * sinf(u * PI_F);
    return -lean + wave + ripple;
}

static void unit_bounds(P2 *lo, P2 *hi) {
    float theta_max = TURNS * TAU_F;
    *lo = (P2){FLT_MAX, FLT_MAX};
    *hi = (P2){-FLT_MAX, -FLT_MAX};
    for (int k = 0; k <= 720; k++) {
        float theta = theta_max * (float)k / 720.0f;
        float r = powf(GROWTH, (theta - theta_max) / TAU_F) * (1.0f + SUTURE_WAVE);
        float px = cosf(theta) * r, py = sinf(theta) * r;
        lo->x = fminf(lo->x, px);
        lo->y = fminf(lo->y, py);
        hi->x = fmaxf(hi->x, px);
        hi->y = fmaxf(hi->y, py);
    }
}

static P2 project(float px, float py, float pitch, float yaw, float focal,
                  P2 pole) {
    if (pitch == 0.0f && yaw == 0.0f)
        return (P2){roundf(pole.x + px), roundf(pole.y + py)};
    float d = py * sinf(pitch) + px * sinf(yaw);
    float s = clampf(focal / (focal + d), 0.0f, MAX_PERSPECTIVE);
    return (P2){roundf(pole.x + px * cosf(yaw) * s),
                roundf(pole.y + py * cosf(pitch) * s)};
}

void logalith_fit(Rct rect, P2 *pole, float *base_r) {
    P2 lo, hi;
    unit_bounds(&lo, &hi);
    float r = fmaxf(
        fminf(rct_w(rect) / (hi.x - lo.x), rct_h(rect) / (hi.y - lo.y)) * FILL,
        1.0f);
    P2 ctr = rct_center(rect);
    pole->x = ctr.x - (lo.x + hi.x) * 0.5f * r;
    pole->y = ctr.y - (lo.y + hi.y) * 0.5f * r;
    *base_r = r;
}

typedef struct {
    Canvas *c;
    const Pose *pose;
    const Look *look;
    uint8_t ink;
} Ctx;

static float r_at(const Ctx *x, float t) {
    return x->pose->max_r *
           logalith_unit_r(t, x->look->suture_ph, x->look->cycles);
}

static P2 at_point(const Ctx *x, float t, float r) {
    float a = t + x->pose->tilt;
    return project(cosf(a) * r, sinf(a) * r, x->pose->pitch, x->pose->yaw,
                   x->pose->focal, x->pose->pole);
}

#define RUN_CAP 4096

typedef struct {
    P2 buf[RUN_CAP];
    int n;
    P2 prev;
    bool have_prev;
    uint64_t seed, seg;
    float w;
} Run;

static void run_start(Run *r, uint64_t seed, float w) {
    r->n = 0;
    r->have_prev = false;
    r->seed = seed;
    r->seg = 0;
    r->w = w;
}

static void run_push(const Ctx *x, Run *r, P2 p) {
    if (x->look->stipple <= 0.001f) {
        if (r->n == RUN_CAP) {
            draw_polyline(x->c, r->buf, (size_t)r->n, r->w, x->ink);
            r->buf[0] = r->buf[r->n - 1];
            r->n = 1;
        }
        r->buf[r->n++] = p;
    } else {
        if (r->have_prev) {
            if (hash01(r->seed ^ 0x109A11ULL, r->seg) >= x->look->stipple)
                draw_line(x->c, r->prev, p, r->w, x->ink);
            r->seg++;
        }
        r->prev = p;
        r->have_prev = true;
    }
}

static void run_end(const Ctx *x, Run *r) {
    if (x->look->stipple <= 0.001f && r->n >= 2)
        draw_polyline(x->c, r->buf, (size_t)r->n, r->w, x->ink);
    r->n = 0;
    r->have_prev = false;
    r->seg = 0;
}

static void rib_draw(const Ctx *x, float t, float r_inner, float r_outer,
                     float w, float span, float waviness, uint64_t seed) {
    float ph =
        x->look->ripple_ph + t * RIPPLE_WRAP * (r_outer / x->pose->max_r);
    P2 pts[15];
    for (int k = 0; k <= 14; k++) {
        float u = (float)k / 14.0f;
        pts[k] = at_point(x, t + rib_offset(u, span, waviness, x->look->along, ph),
                          r_inner + (r_outer - r_inner) * u);
    }
    if (x->look->stipple <= 0.001f) {
        draw_polyline(x->c, pts, 15, w, x->ink);
    } else {
        for (int k = 0; k < 14; k++)
            if (hash01(seed ^ 0x109A11ULL, (uint64_t)k) >= x->look->stipple)
                draw_line(x->c, pts[k], pts[k + 1], w, x->ink);
    }
}

void logalith_draw(Canvas *c, const Pose *pose, const Look *look, uint8_t ink) {
    if (look->grown <= 0.001f || look->stipple >= 0.999f || pose->max_r < 4.0f)
        return;
    float tau = TAU_F;
    float theta_max = TURNS * tau;
    P2 pole = pose->pole;
    float tilt = pose->tilt, max_r = pose->max_r, bore_r = pose->bore_r;
    float pitch = pose->pitch, yaw = pose->yaw, focal = pose->focal;
    Ctx ctx = {c, pose, look, ink};
    const Ctx *x = &ctx;

    float eye = fminf(MIN_PX + bore_r * UMBILICUS_MAX * (1.0f - look->index),
                      max_r * 0.9f);

    Rct clip = canvas_clip(c);
    float dx = fmaxf(fabsf(clip.x0 - pole.x), fabsf(clip.x1 - pole.x));
    float dy = fmaxf(fabsf(clip.y0 - pole.y), fabsf(clip.y1 - pole.y));
    float reach = sqrtf(dx * dx + dy * dy) * CULL_MARGIN + 12.0f;
    float far_r = (pitch == 0.0f && yaw == 0.0f) ? reach : reach * MAX_PERSPECTIVE;
    float t_cull =
        (far_r >= max_r || !isfinite(far_r))
            ? theta_max
            : clampf(theta_max + tau * logf(far_r / max_r) / logf(GROWTH), 0.0f,
                     theta_max);
    float t_far = fminf(t_cull, theta_max * clampf(look->grown, 0.0f, 1.0f));

    {
        Run run;
        uint64_t key = 0;
        bool run_heavy = false;
        run_start(&run, key, 1.0f);
        float t = 0.0f;
        while (t <= t_far) {
            float r = r_at(x, t);
            bool heavy = t > theta_max - tau;
            if (r < eye || heavy != run_heavy) {
                run_end(x, &run);
                key += 1;
                run_heavy = heavy;
                run_start(&run, key, run_heavy ? 2.0f : 1.0f);
            }
            if (r >= eye) run_push(x, &run, at_point(x, t, r));
            t += clampf(STEP_PX / fmaxf(r, 1.0f), 0.004f, 0.30f);
        }
        run_end(x, &run);
    }

    uint32_t ghosts = look->ghosts < GHOST_MAX ? look->ghosts : GHOST_MAX;
    for (uint32_t p = 1; p <= ghosts; p++) {
        float turn = (float)p * PI_F / 5.0f * look->ghost_spread;
        float step = STEP_PX / powi_f(GHOST_GAIN, (int)p * 4);
        float gt = 0.0f;
        while (gt <= t_far) {
            float r = r_at(x, gt);
            if (r >= eye) {
                float a = gt + tilt + turn;
                P2 q = project(cosf(a) * r, sinf(a) * r, pitch, yaw, focal, pole);
                draw_rect_filled(c, rct(q.x - 0.5f, q.y - 0.5f, q.x + 0.5f, q.y + 0.5f),
                                 ink);
            }
            gt += clampf(step / fmaxf(r, 1.0f), 0.004f, 0.30f);
        }
    }

    uint32_t chamber_index = 0;
    for (int whorl = 0; whorl < WHORLS; whorl++) {
        uint32_t chambers = CHAMBERS[whorl];
        float outer_t = theta_max - (float)whorl * tau;
        if (outer_t - tau > t_far) {
            chamber_index += chambers;
            continue;
        }
        float span = tau / (float)chambers;
        for (uint32_t j = 0; j < chambers; j++) {
            float t = outer_t - (float)j * span;
            chamber_index += 1;
            if (t < 0.0f || t > t_far) continue;
            float r_in = fmaxf(r_at(x, t - tau), eye);
            float r_out = r_at(x, t);
            if (r_out < eye + MIN_PX) continue;
            rib_draw(x, t, fmaxf(r_in, 1.0f), r_out, 1.0f, span, 1.0f,
                     0x5E00ULL + (uint64_t)chamber_index);
            uint32_t ribs =
                1 + (uint32_t)roundf(clampf(look->level, 0.0f, 1.0f) * RIB_MAX);
            while (ribs > 1 && span * r_out / (float)ribs < 3.0f) ribs -= 1;
            float engrave_reach =
                ENGRAVE_REACH_MIN +
                ENGRAVE_REACH_RANGE * clampf(look->level, 0.0f, 1.0f);
            for (uint32_t m = 1; m < ribs; m++) {
                float rt = t - span * (float)m / (float)ribs;
                if (rt < 0.0f || rt > t_far) continue;
                float ri = fmaxf(r_at(x, rt - tau), eye);
                float ro = r_at(x, rt);
                if (ro < eye + MIN_PX) continue;
                rib_draw(x, rt, ro - (ro - ri) * engrave_reach, ro, 1.0f, span,
                         RIB_INNER,
                         0xB100ULL + (uint64_t)chamber_index * 8 + (uint64_t)m);
            }
        }
    }

    if (theta_max <= t_far)
        rib_draw(x, theta_max, r_at(x, theta_max - tau), r_at(x, theta_max),
                 2.0f, tau / (float)CHAMBERS[0], 1.0f, 0xA9E9ULL);
}

#define SPLASH_DURATION_S 2.0f
#define SPLASH_HOLD_S 0.2f
#define SPLASH_TOTAL_S (SPLASH_DURATION_S + SPLASH_HOLD_S)
#define SPLASH_CURVE 0.05f
#define SCALE_FROM 0.21f
#define SCALE_TO 1.4f
#define BORE_LOCK 1.0f
#define POLE_X_FROM 0.0f
#define POLE_X_TO 0.5f
#define POLE_Y_FROM 0.5f
#define POLE_Y_TO 0.5f
#define PITCH_FROM (-0.6f)
#define PITCH_TO (-0.02f)
#define YAW_FROM 0.55f
#define YAW_TO 0.0f
#define FOCAL_K 0.15f
#define GROWN_FROM 0.0f
#define GROWN_TO 1.0f
#define SPIN_TURNS (-2.1f)
#define TILT_REST 1.55f
#define STIPPLE_FROM 0.64f
#define STIPPLE_TO 0.0f
#define LEVEL_FROM 0.0f
#define LEVEL_TO 0.93f
#define INDEX_FROM 0.125f
#define INDEX_TO 1.0f
#define SPLASH_CYCLES 79.0f
#define SPLASH_ALONG 6.2f
#define SPLASH_GHOSTS 5u
#define SPREAD_FROM 0.8f
#define SPREAD_TO 6.0f
#define SPLASH_GREY 224
#define SUTURE_RATE 0.6f
#define RIPPLE_RATE 0.9f

static float lerpf(float a, float b, float k) { return a + (b - a) * k; }

bool splash_draw(App *a, Canvas *c, Rct rect, float elapsed) {
    if (a->splash_over) return false;
    if (elapsed >= SPLASH_TOTAL_S) {
        a->splash_over = true;
        return false;
    }
    if (fminf(rct_w(rect), rct_h(rect)) < 16.0f) return true;

    draw_rect_filled(c, rect, 0);

    float k = powf(clampf(elapsed / SPLASH_DURATION_S, 0.0f, 1.0f), SPLASH_CURVE);
    P2 fitted;
    float base_r;
    logalith_fit(rect, &fitted, &base_r);
    float max_r = base_r * lerpf(SCALE_FROM, SCALE_TO, k);
    float dx = (lerpf(POLE_X_FROM, POLE_X_TO, k) - 0.5f) * rct_w(rect);
    float dy = (lerpf(POLE_Y_FROM, POLE_Y_TO, k) - 0.5f) * rct_h(rect);
    Pose pose = {
        .pole = {fitted.x + dx, fitted.y + dy},
        .tilt = TILT_REST + SPIN_TURNS * TAU_F * (1.0f - k),
        .max_r = max_r,
        .bore_r = lerpf(base_r, max_r, BORE_LOCK),
        .pitch = lerpf(PITCH_FROM, PITCH_TO, k),
        .yaw = lerpf(YAW_FROM, YAW_TO, k),
        .focal = fmaxf(max_r * FOCAL_K, 1.0f),
    };
    Look look = {
        .suture_ph = elapsed * SUTURE_RATE,
        .ripple_ph = elapsed * RIPPLE_RATE,
        .cycles = SPLASH_CYCLES,
        .along = SPLASH_ALONG,
        .level = lerpf(LEVEL_FROM, LEVEL_TO, k),
        .grown = lerpf(GROWN_FROM, GROWN_TO, k),
        .index = lerpf(INDEX_FROM, INDEX_TO, k),
        .ghosts = SPLASH_GHOSTS,
        .ghost_spread = lerpf(SPREAD_FROM, SPREAD_TO, k),
        .stipple = lerpf(STIPPLE_FROM, STIPPLE_TO, k),
    };
    logalith_draw(c, &pose, &look, SPLASH_GREY);
    return true;
}
