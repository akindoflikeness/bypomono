#include "dsp.h"
#include <math.h>
#include <stdlib.h>

#define BASE_DELAY_SECONDS 0.029f
#define MAX_DELAY_SECONDS 0.08f
#define ALLPASS_G (1.0f / PHI)
#define COMB_INPUT_GAIN 0.06f
#define WET_MAKEUP_GAIN 25.0f
#define MAX_FB 0.985f
#define WET_HEADROOM (PHI * PHI)
#define LOOP_DRIVE PHI
#define PARAM_SMOOTH 0.0014f
#define FIELD_TO_DAMP 0.5f
#define FIELD_TO_MIX 0.1f
#define MOD_BASE_HZ 0.31f

#define FC_MAX_HZ 18000.0f
#define FC_MIN_HZ 20.0f
#define FC_MAX_NYQUIST_FRACTION 0.45f
#define FC_GOLDEN_STEPS 10.0f
#define LOOP_DAMP_SCALE (1.0f / (PHI * PHI))

/* delay lengths and coefficients travel rather than jump: a spliced read or
   a rewritten filter coefficient is a click */
#define LINE_GLIDE_S 0.04f
#define GHOST_STEP 3
#define GHOST_DELAY_FACTOR PHI
#define GHOST_MAX_FB 0.92f

static const float ALLPASS_SECONDS[2] = { 0.0051f, 0.0051f / PHI };

float verb_room_hp_hz(float base_hz) {
    return clampf(base_hz / PHI, VERB_ROOM_HP_MIN_HZ, VERB_ROOM_HP_MAX_HZ);
}

float verb_damp_q(float damp) {
    float knee = clampf((damp - VERB_Q_KNEE) / (1.0f - VERB_Q_KNEE), 0.0f, 1.0f);
    return VERB_Q_BASE + (powi_f(PHI, 4) - VERB_Q_BASE) * powf(knee, PHI);
}

VerbParams verb_params_default(void) {
    VerbParams p = { 0.35f, 0.4f, 4.0f, 0.4f, 0.0f };
    return p;
}

static float loop_sat(float v) {
    float d = LOOP_DRIVE;
    if (d <= 0.0f) {
        return v;
    }
    return tanhf(v * d) / d;
}

static void comb_new(Comb *c, size_t len, float lfo_inc, float lfo_phase) {
    c->buf = (float *)calloc(len, sizeof(float));
    c->len = len;
    c->write = 0;
    c->delay = 1;
    c->delay_f = 0.0f;
    c->delay_to = 1.0f;
    c->delay_seconds = 0.0f;
    c->fb = 0.0f;
    c->fb_target = 0.0f;
    c->lp = 0.0f;
    c->lfo_phase = lfo_phase;
    c->lfo_inc = lfo_inc;
}

static void comb_set_delay(Comb *c, float seconds, float sample_rate) {
    size_t d = (size_t)(seconds * sample_rate);
    c->delay_seconds = seconds;
    c->delay = d < 1 ? 1 : (d > c->len - 1 ? c->len - 1 : d);
    c->delay_to = (float)c->delay;
    if (c->delay_f <= 0.0f) c->delay_f = c->delay_to; /* born on its length */
}

static void comb_set_decay(Comb *c, float decay) {
    c->fb_target = fminf(powf(0.001f, c->delay_seconds / fmaxf(decay, 0.05f)), MAX_FB);
}

static float comb_process(Comb *c, float x, float damp, float glide) {
    c->fb += PARAM_SMOOTH * (c->fb_target - c->fb);
    c->delay_f = glide_to(c->delay_f, c->delay_to, glide);
    c->lfo_phase += c->lfo_inc;
    if (c->lfo_phase >= 1.0f) {
        c->lfo_phase -= 1.0f;
    }
    float wobble = sinf(TAU_F * c->lfo_phase) * VERB_MOD_DEPTH_SAMPLES;
    size_t len = c->len;
    float d = clampf(c->delay_f + wobble, 1.0f, (float)(len - 2));
    size_t di = (size_t)d;
    float frac = d - (float)di;
    size_t i0 = (c->write + len - di) % len;
    size_t i1 = (c->write + len - di - 1) % len;
    float y = c->buf[i0] * (1.0f - frac) + c->buf[i1] * frac;
    c->lp = y + damp * (c->lp - y);
    c->buf[c->write] = x + loop_sat(c->lp * c->fb);
    c->write = (c->write + 1) % c->len;
    return y;
}

static void ghost_new(GhostLine *g, size_t len, float ap_a) {
    g->buf = (float *)calloc(len, sizeof(float));
    g->len = len;
    g->write = 0;
    g->delay = 1;
    phase_rotator_init(&g->rot, ap_a);
    g->rot_to = ap_a;
    g->delay_f = 0.0f;
    g->delay_to = 1.0f;
    g->fb = 0.0f;
}

static void ghost_set_delay(GhostLine *g, float seconds, float sample_rate) {
    size_t d = (size_t)(seconds * sample_rate);
    g->delay = d < 1 ? 1 : (d > g->len - 1 ? g->len - 1 : d);
    g->delay_to = (float)g->delay;
    if (g->delay_f <= 0.0f) g->delay_f = g->delay_to;
}

static float ghost_process(GhostLine *g, float x) {
    float d = clampf(g->delay_f, 1.0f, (float)(g->len - 2));
    size_t di = (size_t)d;
    float frac = d - (float)di;
    size_t i0 = (g->write + g->len - di) % g->len;
    size_t i1 = (g->write + g->len - di - 1) % g->len;
    float y = phase_rotator_process(&g->rot, g->buf[i0] * (1.0f - frac) + g->buf[i1] * frac);
    g->buf[g->write] = x + y * g->fb;
    g->write = (g->write + 1) % g->len;
    return y;
}

static void allpass_new(Allpass *a, float seconds, float sample_rate) {
    size_t delay = (size_t)(seconds * sample_rate);
    if (delay < 1) {
        delay = 1;
    }
    a->buf = (float *)calloc(delay + 1, sizeof(float));
    a->len = delay + 1;
    a->write = 0;
    a->delay = delay;
}

static float allpass_process(Allpass *a, float x) {
    size_t read = (a->write + a->len - a->delay) % a->len;
    float d = a->buf[read];
    float y = d - ALLPASS_G * x;
    a->buf[a->write] = x + ALLPASS_G * d;
    a->write = (a->write + 1) % a->len;
    return y;
}

void verb_init(StereoVerb *v, float sample_rate) {
    size_t len = (size_t)(MAX_DELAY_SECONDS * sample_rate) + 2;
    size_t ghost_len = (size_t)(MAX_DELAY_SECONDS * GHOST_DELAY_FACTOR * sample_rate) + 2;
    float ghost_a = allpass_coeff_for(START_HZ, sample_rate, PHASE_PER_PASS);
    VerbParams defaults = verb_params_default();
    v->sample_rate = sample_rate;
    v->params = defaults;
    v->mix_s = defaults.mix;
    v->ghost_s = defaults.ghost;
    v->haunt_s = defaults.haunt;
    v->damp_s = defaults.damp;
    v->configured = false;
    for (int i = 0; i < NUM_OPS; i++) {
        v->send[i] = 1.0f;
        v->send_to[i] = 1.0f;
        v->carrier[i] = 1.0f;
        v->carrier_to[i] = 1.0f;
        v->is_carrier[i] = true;
        float rate = MOD_BASE_HZ * powf(PHI, -0.5f * (float)i);
        comb_new(&v->combs_l[i], len, rate / sample_rate, 0.19f * (float)i);
        comb_new(&v->combs_r[i], len, rate / sample_rate, 0.19f * (float)i + 0.37f);
        ghost_new(&v->ghosts[i], ghost_len, ghost_a);
        dc_block_clear(&v->dc[i]);
    }
    for (int i = 0; i < 2; i++) {
        allpass_new(&v->ap_l[i], ALLPASS_SECONDS[i], sample_rate);
        allpass_new(&v->ap_r[i], ALLPASS_SECONDS[i], sample_rate);
        v->svf_l[i].ic1 = 0.0f;
        v->svf_l[i].ic2 = 0.0f;
        v->svf_r[i].ic1 = 0.0f;
        v->svf_r[i].ic2 = 0.0f;
    }
    dc_block_clear(&v->wet_dc_l);
    dc_block_clear(&v->wet_dc_r);
    v->room_hp_l.ic1 = 0.0f;
    v->room_hp_l.ic2 = 0.0f;
    v->room_hp_r.ic1 = 0.0f;
    v->room_hp_r.ic2 = 0.0f;
    v->room_hp_g = tanf(PI_F * verb_room_hp_hz(START_HZ) / sample_rate);
    v->room_hp_for_hz = START_HZ;
    v->ghost_hz = START_HZ;
}

/* solving for the coefficient is a bisection, so only pay for it when the
   drone pitch actually moved */
void verb_set_drone_hz(StereoVerb *v, float hz) {
    if (hz == v->ghost_hz) return;
    v->ghost_hz = hz;
    float a = allpass_coeff_for(hz, v->sample_rate, PHASE_PER_PASS);
    for (int i = 0; i < NUM_OPS; i++) v->ghosts[i].rot_to = a;
}

void verb_free(StereoVerb *v) {
    for (int i = 0; i < NUM_OPS; i++) {
        free(v->combs_l[i].buf);
        free(v->combs_r[i].buf);
        free(v->ghosts[i].buf);
        v->combs_l[i].buf = NULL;
        v->combs_r[i].buf = NULL;
        v->ghosts[i].buf = NULL;
    }
    for (int i = 0; i < 2; i++) {
        free(v->ap_l[i].buf);
        free(v->ap_r[i].buf);
        v->ap_l[i].buf = NULL;
        v->ap_r[i].buf = NULL;
    }
}

void verb_configure(StereoVerb *v, const Patch *patch, const Compiled *compiled) {
    for (int i = 0; i < NUM_OPS; i++) {
        float ratio = fmaxf(patch->ops[i].ratio, 1e-3f);
        float left = fminf(BASE_DELAY_SECONDS / sqrtf(ratio), MAX_DELAY_SECONDS / sqrtf(PHI));
        comb_set_delay(&v->combs_l[i], left, v->sample_rate);
        comb_set_delay(&v->combs_r[i], left * sqrtf(PHI), v->sample_rate);
        comb_set_decay(&v->combs_l[i], v->params.decay);
        comb_set_decay(&v->combs_r[i], v->params.decay);
        ghost_set_delay(&v->ghosts[i], left * GHOST_DELAY_FACTOR, v->sample_rate);
        bool carrier = ((compiled->carriers >> i) & 1) == 1;
        v->is_carrier[i] = carrier;
        v->carrier_to[i] = carrier ? 1.0f : 0.0f;
        v->send_to[i] = carrier ? 1.0f : powi_f(PHI, -((int)compiled->depth[i] - 1));
        if (!v->configured) {
            v->send[i] = v->send_to[i];
            v->carrier[i] = v->carrier_to[i];
        }
    }
    v->configured = true;
}

void verb_set_params(StereoVerb *v, VerbParams params) {
    v->params = params;
    for (int i = 0; i < NUM_OPS; i++) {
        comb_set_decay(&v->combs_l[i], params.decay);
    }
    for (int i = 0; i < NUM_OPS; i++) {
        comb_set_decay(&v->combs_r[i], params.decay);
    }
}

VerbParams verb_params(const StereoVerb *v) {
    return v->params;
}

Stereo verb_process(StereoVerb *v, const Frame *frame) {
    v->mix_s += PARAM_SMOOTH * (v->params.mix - v->mix_s);
    v->ghost_s += PARAM_SMOOTH * (v->params.ghost - v->ghost_s);
    v->haunt_s += PARAM_SMOOTH * (v->params.haunt - v->haunt_s);
    v->damp_s += PARAM_SMOOTH * (v->params.damp - v->damp_s);

    float damp = v->damp_s - frame->field * FIELD_TO_DAMP;
    float wet_duck = fmaxf(1.0f - frame->field * FIELD_TO_MIX, 0.0f);

    float glide = glide_k(LINE_GLIDE_S, v->sample_rate);
    float rot_k = glide_k(GATE_GLIDE_S, v->sample_rate);
    /* the ghost lines travel even while they are silent, so haunt coming
       back does not find them mid-jump */
    for (int i = 0; i < NUM_OPS; i++) {
        v->ghosts[i].delay_f = glide_to(v->ghosts[i].delay_f, v->ghosts[i].delay_to, glide);
        v->ghosts[i].rot.a = glide_to(v->ghosts[i].rot.a, v->ghosts[i].rot_to, rot_k);
    }
    float sends[NUM_OPS];
    for (int i = 0; i < NUM_OPS; i++) {
        /* which ops feed the room is part of the algorithm, so it moves when
           the algorithm does */
        v->send[i] = glide_to(v->send[i], v->send_to[i], PARAM_SMOOTH);
        v->carrier[i] = glide_to(v->carrier[i], v->carrier_to[i], PARAM_SMOOTH);
        float bleed = v->carrier[i] + (1.0f - v->carrier[i]) * v->ghost_s;
        sends[i] = frame->ops[i] * frame->master * v->send[i] * bleed;
    }
    float haunted[NUM_OPS] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (v->haunt_s > 1e-4f) {
        for (int i = 0; i < NUM_OPS; i++) {
            v->ghosts[i].fb = GHOST_MAX_FB * v->haunt_s;
            haunted[i] = ghost_process(&v->ghosts[i], sends[i]);
        }
    }
    float wet_l = 0.0f;
    float wet_r = 0.0f;
    float loop_damp = fmaxf(damp, 0.0f) * LOOP_DAMP_SCALE;
    for (int i = 0; i < NUM_OPS; i++) {
        int from = (i + NUM_OPS - GHOST_STEP) % NUM_OPS;
        float x = dc_block_process(&v->dc[i], sends[i] + v->haunt_s * haunted[from]) * COMB_INPUT_GAIN;
        wet_l += comb_process(&v->combs_l[i], x, loop_damp, glide);
        wet_r += comb_process(&v->combs_r[i], x, loop_damp, glide);
    }
    float scale = WET_MAKEUP_GAIN / (float)NUM_OPS;
    wet_l *= scale;
    wet_r *= scale;
    for (int i = 0; i < 2; i++) {
        wet_l = allpass_process(&v->ap_l[i], wet_l);
    }
    for (int i = 0; i < 2; i++) {
        wet_r = allpass_process(&v->ap_r[i], wet_r);
    }
    float fc = clampf(FC_MAX_HZ * powf(PHI, -FC_GOLDEN_STEPS * damp), FC_MIN_HZ,
                      fminf(FC_MAX_HZ, FC_MAX_NYQUIST_FRACTION * v->sample_rate));
    float g = tanf(PI_F * fc / v->sample_rate);
    float k1 = 1.0f / verb_damp_q(damp);
    wet_l = svf_process(&v->svf_l[0], wet_l, g, k1);
    wet_l = svf_process(&v->svf_l[1], wet_l, g, 2.0f);
    wet_r = svf_process(&v->svf_r[0], wet_r, g, k1);
    wet_r = svf_process(&v->svf_r[1], wet_r, g, 2.0f);
    wet_l = dc_block_process(&v->wet_dc_l, soft_clip_to(wet_l, WET_HEADROOM));
    wet_r = dc_block_process(&v->wet_dc_r, soft_clip_to(wet_r, WET_HEADROOM));
    if (fabsf(frame->base_hz - v->room_hp_for_hz) > v->room_hp_for_hz * 1e-4f) {
        v->room_hp_for_hz = frame->base_hz;
        v->room_hp_g = tanf(PI_F * verb_room_hp_hz(frame->base_hz) / v->sample_rate);
    }
    wet_l = svf_process_hp(&v->room_hp_l, wet_l, v->room_hp_g, VERB_ROOM_HP_K);
    wet_r = svf_process_hp(&v->room_hp_r, wet_r, v->room_hp_g, VERB_ROOM_HP_K);
    float dry = frame->mix * (1.0f - v->mix_s);
    float side = frame->side * (1.0f - v->mix_s);
    Stereo out = { dry + side + wet_l * v->mix_s * wet_duck, dry - side + wet_r * v->mix_s * wet_duck };
    return out;
}
