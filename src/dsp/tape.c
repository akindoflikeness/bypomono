#include "dsp.h"
#include <math.h>

#define DRIVE_DB 20.0f
#define DRIVE_CURVE 1.4f

#define EVEN 0.9f
#define EVEN_HZ 320.0f

#define BUMP_DB 2.2f
#define BUMP_HZ 70.0f

#define TOP_OPEN_HZ 22000.0f
#define TOP_CLOSED_HZ 8000.0f

#define COMP_AT_FULL 1.0f

#define TETHER_ATT_MS 0.5f
#define TETHER_REL_MS 30.0f

float tape_saturate(float x) {
    return soft_clip(x);
}

static void shelf_set(Shelf *s, float hz, float gain_db, float sr) {
    s->k = 1.0f - expf(-TAU_F * hz / fmaxf(sr, 1.0f));
    s->gain = powf(10.0f, gain_db / 20.0f) - 1.0f;
}

static float shelf_process(Shelf *s, float x) {
    s->lp += (x - s->lp) * s->k;
    return x + s->lp * s->gain;
}

static void shelf_clear(Shelf *s) {
    s->lp = 0.0f;
}

static void top_set(Top *t, float hz, float sr) {
    hz = fminf(hz, sr * 0.45f);
    t->k = 1.0f - expf(-TAU_F * hz / fmaxf(sr, 1.0f));
}

static float top_process(Top *t, float x) {
    t->lp += (x - t->lp) * t->k;
    return t->lp;
}

static void top_clear(Top *t) {
    t->lp = 0.0f;
}

static void hp4_set(Hp4 *h, float hz, float sr) {
    biquad_highpass(&h->a, hz, sr);
    biquad_highpass(&h->b, hz, sr);
}

static float hp4_process(Hp4 *h, float x) {
    return biquad_process(&h->b, biquad_process(&h->a, x));
}

static void hp4_clear(Hp4 *h) {
    biquad_clear(&h->a);
    biquad_clear(&h->b);
}

static float tether_band_follow(TetherBand *b, float x, float att, float rel) {
    float k = x > b->env ? att : rel;
    b->env = x + (b->env - x) * k;
    return b->env;
}

static void tether_band_clear(TetherBand *b) {
    hp4_clear(&b->l);
    hp4_clear(&b->r);
    b->env = 0.0f;
}

float tether_lift_db(float env, float lift) {
    if (lift <= 0.0f) {
        return 0.0f;
    }
    float x = 20.0f * log10f(fmaxf(env, 1e-9f));
    float slope = 1.0f - 1.0f / TETHER_RATIO;
    float below = fmaxf(TETHER_THRESHOLD_DB - x, 0.0f);
    float k = fminf(TETHER_KNEE_DB, lift / slope);
    float raw;
    if (below <= k) {
        raw = slope * below * below / (2.0f * fmaxf(k, 1e-6f));
    } else {
        raw = slope * (below - 0.5f * k);
    }
    return fminf(raw, lift);
}

static void tape_design(Tape *t, float w) {
    shelf_set(&t->bump_l, BUMP_HZ, BUMP_DB * w, t->sample_rate);
    shelf_set(&t->bump_r, BUMP_HZ, BUMP_DB * w, t->sample_rate);
    float top = TOP_OPEN_HZ + (TOP_CLOSED_HZ - TOP_OPEN_HZ) * w;
    top_set(&t->top_l, top, t->sample_rate);
    top_set(&t->top_r, top, t->sample_rate);
    t->makeup = 1.0f;
    t->designed = w;
}

void tape_init(Tape *t, float sample_rate) {
    t->sample_rate = sample_rate;
    t->warmth = 0.0f;
    t->warmth_s = 0.0f;
    t->glide = expf(-1.0f / (0.12f * fmaxf(sample_rate, 1.0f)));
    t->bump_l = (Shelf){0};
    t->bump_r = (Shelf){0};
    t->top_l = (Top){0};
    t->top_r = (Top){0};
    t->even_l = (Top){0};
    t->even_r = (Top){0};
    t->dc_l0 = 0.0f;
    t->dc_l1 = 0.0f;
    t->dc_r0 = 0.0f;
    t->dc_r1 = 0.0f;
    t->env_fast = 0.0f;
    t->env_slow = 0.0f;
    t->gr = 1.0f;
    t->gr_k = 0.0f;
    t->atk = 0.0f;
    t->rel_fast = 0.0f;
    t->rel_slow = 0.0f;
    t->band_a = (TetherBand){0};
    t->band_b = (TetherBand){0};
    t->ap_l = (Biquad){0};
    t->ap_r = (Biquad){0};
    t->act = 0.0f;
    t->act_k = 0.0f;
    t->ap_hz = 0.0f;
    t->t_att = 0.0f;
    t->t_rel = 0.0f;
    t->makeup = 1.0f;
    t->designed = -1.0f;
    t->atk = expf(-1.0f / (COMP_ATTACK_MS * 0.001f * fmaxf(sample_rate, 1.0f)));
    t->gr_k = expf(-1.0f / (0.003f * fmaxf(sample_rate, 1.0f)));
    t->rel_fast = expf(-1.0f / (COMP_RELEASE_FAST_MS * 0.001f * fmaxf(sample_rate, 1.0f)));
    t->rel_slow = expf(-1.0f / (COMP_RELEASE_SLOW_MS * 0.001f * fmaxf(sample_rate, 1.0f)));
    top_set(&t->even_l, EVEN_HZ, sample_rate);
    top_set(&t->even_r, EVEN_HZ, sample_rate);
    hp4_set(&t->band_a.l, TETHER_HZ, sample_rate);
    hp4_set(&t->band_a.r, TETHER_HZ, sample_rate);
    hp4_set(&t->band_b.l, TETHER_AIR_HZ, sample_rate);
    hp4_set(&t->band_b.r, TETHER_AIR_HZ, sample_rate);
    biquad_allpass(&t->ap_l, TETHER_AP_LO_HZ, sample_rate);
    biquad_allpass(&t->ap_r, TETHER_AP_LO_HZ, sample_rate);
    t->ap_hz = TETHER_AP_LO_HZ;
    t->t_att = expf(-1.0f / (TETHER_ATT_MS * 0.001f * fmaxf(sample_rate, 1.0f)));
    t->t_rel = expf(-1.0f / (TETHER_REL_MS * 0.001f * fmaxf(sample_rate, 1.0f)));
    t->act_k = 1.0f - expf(-1.0f / (0.021f * fmaxf(sample_rate, 1.0f)));
    tape_design(t, 0.0f);
}

void tape_set(Tape *t, float warmth) {
    t->warmth = clampf(warmth, MIN_WARMTH, MAX_WARMTH);
}

float tape_warmth(const Tape *t) {
    return t->warmth;
}

void tape_clear(Tape *t) {
    shelf_clear(&t->bump_l);
    shelf_clear(&t->bump_r);
    top_clear(&t->top_l);
    top_clear(&t->top_r);
    top_clear(&t->even_l);
    top_clear(&t->even_r);
    t->dc_l0 = 0.0f;
    t->dc_l1 = 0.0f;
    t->dc_r0 = 0.0f;
    t->dc_r1 = 0.0f;
    t->env_fast = 0.0f;
    t->env_slow = 0.0f;
    t->gr = 1.0f;
    tether_band_clear(&t->band_a);
    tether_band_clear(&t->band_b);
    biquad_clear(&t->ap_l);
    biquad_clear(&t->ap_r);
    t->act = 0.0f;
}

static float tape_drive(float w) {
    return powf(10.0f, DRIVE_DB * powf(w, DRIVE_CURVE) / 20.0f);
}

static float tape_reduction(float env) {
    float db = 20.0f * log10f(fmaxf(env, 1e-9f));
    float over = db - COMP_THRESHOLD_DB;
    float k = COMP_KNEE_DB;
    float cut;
    if (over <= -k) {
        cut = 0.0f;
    } else if (over >= k) {
        cut = over * (1.0f - 1.0f / COMP_RATIO);
    } else {
        float t = over + k;
        cut = (1.0f - 1.0f / COMP_RATIO) * t * t / (4.0f * k);
    }
    return powf(10.0f, -fminf(cut, COMP_MAX_REDUCTION_DB) / 20.0f);
}

static float tape_block(float *s0, float *s1, float x) {
    float y = x - *s0 + 0.9995f * *s1;
    *s0 = x;
    *s1 = y;
    return y;
}

static Stereo tape_tether(Tape *t, float l, float r, float w) {
    float lift = TETHER_LIFT_DB * w;
    float att = t->t_att;
    float rel = t->t_rel;
    float hl = 0.0f, hr = 0.0f, sum = 0.0f;
    TetherBand *bands[2] = {&t->band_a, &t->band_b};
    for (int i = 0; i < 2; i++) {
        TetherBand *band = bands[i];
        float bl = hp4_process(&band->l, l);
        float br = hp4_process(&band->r, r);
        float env = tether_band_follow(band, fmaxf(fabsf(bl), fabsf(br)), att, rel);
        float db = tether_lift_db(env, lift);
        sum += db;
        float d = powf(10.0f, db / 20.0f) - 1.0f;
        hl += bl * d;
        hr += br * d;
    }
    float want = lift > 0.0f ? clampf(0.5f * sum / lift, 0.0f, 1.0f) : 0.0f;
    t->act += (want - t->act) * t->act_k;
    float hz = TETHER_AP_LO_HZ + (TETHER_AP_HI_HZ - TETHER_AP_LO_HZ) * (t->act * w);
    if (fabsf(hz - t->ap_hz) > 8.0f) {
        biquad_allpass(&t->ap_l, hz, t->sample_rate);
        biquad_allpass(&t->ap_r, hz, t->sample_rate);
        t->ap_hz = hz;
    }
    return (Stereo){biquad_process(&t->ap_l, l) + hl, biquad_process(&t->ap_r, r) + hr};
}

Stereo tape_process(Tape *t, Stereo x) {
    t->warmth_s += (t->warmth - t->warmth_s) * (1.0f - t->glide);
    if (t->warmth <= 0.0f && t->warmth_s < 1e-4f) {
        t->warmth_s = 0.0f;
        tape_clear(t);
        return x;
    }
    float w = t->warmth_s;
    if (fabsf(w - t->designed) > 1e-3f) {
        tape_design(t, w);
    }
    float d = tape_drive(w);

    float l = shelf_process(&t->bump_l, x.l);
    float r = shelf_process(&t->bump_r, x.r);
    l *= d;
    r *= d;

    float bl = top_process(&t->even_l, l);
    float br = top_process(&t->even_r, r);
    float el = tape_block(&t->dc_l0, &t->dc_l1, bl * bl);
    float er = tape_block(&t->dc_r0, &t->dc_r1, br * br);
    float a = EVEN * w;
    l += el * a;
    r += er * a;

    l = top_process(&t->top_l, l);
    r = top_process(&t->top_r, r);

    float peak = fmaxf(fabsf(l), fabsf(r));
    float ka = t->atk;
    float kf = peak > t->env_fast ? ka : t->rel_fast;
    float ks = peak > t->env_slow ? ka : t->rel_slow;
    t->env_fast = peak + (t->env_fast - peak) * kf;
    t->env_slow = peak + (t->env_slow - peak) * ks;
    float env = fmaxf(t->env_fast, t->env_slow);
    float target = tape_reduction(env);
    target = 1.0f + (target - 1.0f) * w * COMP_AT_FULL;
    t->gr = target + (t->gr - target) * t->gr_k;
    float g = t->gr * t->makeup;
    l *= g;
    r *= g;

    Stereo th = tape_tether(t, l, r, w);

    return (Stereo){tape_saturate(th.l), tape_saturate(th.r)};
}
