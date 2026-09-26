#include "params.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define PHI4 (PHI * PHI * PHI * PHI)

const Control PARAMS[PARAM_COUNT] = {
    [PARAM_INDEX] = {"index", "INDEX", 0, 1, CURVE_POWER, PHI, false, "%.3f",
                     NULL, NULL, PG_PATCH},
    [PARAM_RIP] = {"rip", "RIP", 0, 1, CURVE_LINEAR, 1, false, "%.2f", NULL,
                   NULL, PG_PATCH},
    [PARAM_FB] = {"fb", "fb", 0, 1, CURVE_LINEAR, 1, false, "%.2f", NULL, NULL,
                  PG_PATCH},
    [PARAM_GLIDE] = {"glide", "glide s", 0, 2, CURVE_POWER, PHI4, false,
                     "%.3f", "s", "seconds", PG_PATCH},
    [PARAM_LEVEL] = {"level", "level", 0, 1, CURVE_LINEAR, 1, false, "%.2f",
                     NULL, NULL, PG_PATCH},
    /* squared, so the narrow beating end gets most of the travel */
    [PARAM_DETUNE] = {"detune", "detune", 0, UNISON_DETUNE_MAX, CURVE_POWER,
                      2, false, "%.1f ct", "cents", "cent", PG_PATCH},
    [PARAM_DRONE_HZ] = {"hz", "drone hz", 27.5f, 440, CURVE_LOG, 1, false,
                        "%.1f", "hz", NULL, PG_DRONE},
    [PARAM_MIX] = {"mix", "mix", 0, 1, CURVE_LINEAR, 1, false, "%.2f", NULL,
                   NULL, PG_VERB},
    [PARAM_GHOST] = {"ghost", "ghost", 0, 1, CURVE_LINEAR, 1, false, "%.2f",
                     NULL, NULL, PG_VERB},
    [PARAM_VERB_DECAY] = {"reverb_decay", "decay s", 0.05f, 8, CURVE_LOG, 1,
                          false, "%.2f s", "s", "seconds", PG_VERB},
    [PARAM_DAMP] = {"damp", "damp", 0, 0.99f, CURVE_LINEAR, 1, false, "%.2f",
                    NULL, NULL, PG_VERB},
    [PARAM_HAUNT] = {"haunt", "haunt", 0, 1, CURVE_LINEAR, 1, false, "%.2f",
                     NULL, NULL, PG_VERB},
    [PARAM_ATTACK] = {"attack", "attack", ENV_ATTACK_MIN, ENV_TIME_MAX,
                      CURVE_ENV_TIME, 1, false, NULL, "s", "seconds", PG_ENV},
    [PARAM_ENV_DECAY] = {"env_decay", "decay", 0, ENV_TIME_MAX, CURVE_ENV_TIME,
                         1, false, NULL, "s", "seconds", PG_ENV},
    [PARAM_SUSTAIN] = {"sustain", "sustain", 0, 1, CURVE_LINEAR, 1, false,
                       "%.2f", NULL, NULL, PG_ENV},
    [PARAM_RELEASE] = {"release", "release", ENV_RELEASE_MIN, ENV_TIME_MAX,
                       CURVE_ENV_TIME, 1, false, NULL, "s", "seconds", PG_ENV},
    [PARAM_CEILING] = {"ceiling", "ceiling", LIMITER_CEILING_DB_MIN,
                       LIMITER_CEILING_DB_MAX, CURVE_LINEAR, 1, false,
                       "%.1f dBTP", "dbtp", NULL, PG_LIMITER},
    [PARAM_OUTPUT] = {"output", "output", OUTPUT_GAIN_DB_MIN, OUTPUT_GAIN_DB_MAX,
                      CURVE_LINEAR, 1, false, "%+.1f dB", "db", NULL,
                      PG_LIMITER},
    [PARAM_MEL_RATE] = {"mel rate", "rate hz", 0.1f, 8, CURVE_LOG, 1, false,
                        "%.2f", "hz", NULL, PG_MELODY},
    [PARAM_MEL_RANGE] = {"mel range", "range", 1, 13, CURVE_LINEAR, 1, true,
                         "%.0f", NULL, NULL, PG_MELODY},
    [PARAM_MEL_ROOT] = {"mel root", "root", 24, 57, CURVE_LINEAR, 1, true,
                        "%.0f", NULL, NULL, PG_MELODY},
    [PARAM_CH_RATE] = {"chandas rate", "rate hz", 0.1f, 8, CURVE_LOG, 1, false,
                       "%.2f", "hz", NULL, PG_CHANDAS},
    [PARAM_CH_MIX] = {"chandas mix", "mix", 0, 1, CURVE_LINEAR, 1, false,
                      "%.2f", NULL, NULL, PG_CHANDAS},
    [PARAM_CH_SPREAD] = {"chandas spread", "spread", 0, 1, CURVE_LINEAR, 1,
                         false, "%.2f", NULL, NULL, PG_CHANDAS},
    [PARAM_CH_SIZE] = {"chandas size", "size", CHANDAS_MIN_SIZE,
                       CHANDAS_MAX_SIZE, CURVE_LINEAR, 1, false, "%.2fx", NULL,
                       NULL, PG_CHANDAS},
    [PARAM_CH_WARP] = {"chandas warp", "warp", 0, 1, CURVE_LINEAR, 1, false,
                       "%.2f", NULL, NULL, PG_CHANDAS},
    [PARAM_CH_DIM] = {"chandas dim", "dimension", 0, 1, CURVE_LINEAR, 1, false,
                      "%.2f", NULL, NULL, PG_CHANDAS},
    [PARAM_CH_TAIL] = {"chandas tail", "tail", 0, 1, CURVE_LINEAR, 1, false,
                       "%.2f", NULL, NULL, PG_CHANDAS},
};

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

int param_find(const char *name) {
    for (int i = 0; i < PARAM_COUNT; i++)
        if (strcmp(PARAMS[i].name, name) == 0) return i;
    return -1;
}

float param_get(const App *a, ParamId id) {
    switch (id) {
    case PARAM_INDEX: return a->shadow.index;
    case PARAM_RIP: return a->shadow.rip;
    case PARAM_FB: return a->shadow.feedback;
    case PARAM_GLIDE: return a->shadow.glide_seconds;
    case PARAM_LEVEL: return a->shadow.master_level;
    case PARAM_DETUNE: return a->shadow.unison_detune;
    case PARAM_DRONE_HZ: return a->drone_hz;
    case PARAM_MIX: return a->shadow_verb.mix;
    case PARAM_GHOST: return a->shadow_verb.ghost;
    case PARAM_VERB_DECAY: return a->shadow_verb.decay;
    case PARAM_DAMP: return a->shadow_verb.damp;
    case PARAM_HAUNT: return a->shadow_verb.haunt;
    case PARAM_ATTACK: return a->shadow_attack_s;
    case PARAM_ENV_DECAY: return a->shadow_decay_s;
    case PARAM_SUSTAIN: return a->shadow_sustain;
    case PARAM_RELEASE: return a->shadow_release_s;
    case PARAM_CEILING: return a->shadow_limiter_ceiling_db;
    case PARAM_OUTPUT: return a->shadow_output_gain_db;
    case PARAM_MEL_RATE: return a->shadow_melody.rate_hz;
    case PARAM_MEL_RANGE: return (float)a->shadow_melody.range_degrees;
    case PARAM_MEL_ROOT: return (float)a->shadow_melody.root_midi;
    case PARAM_CH_RATE: return a->shadow_chandas.rate_hz;
    case PARAM_CH_MIX: return a->shadow_chandas.mix;
    case PARAM_CH_SPREAD: return a->shadow_chandas.spread;
    case PARAM_CH_SIZE: return a->shadow_chandas.size;
    case PARAM_CH_WARP: return a->shadow_chandas.warp;
    case PARAM_CH_DIM: return a->shadow_chandas.dimension;
    case PARAM_CH_TAIL: return a->shadow_chandas.tail;
    case PARAM_COUNT: break;
    }
    return 0.0f;
}

void param_set(App *a, ParamId id, float v) {
    const Control *s = &PARAMS[id];
    if (!(v >= s->lo)) v = s->lo;
    if (v > s->hi) v = s->hi;
    if (s->integer) v = roundf(v);
    switch (id) {
    case PARAM_INDEX: a->shadow.index = v; break;
    case PARAM_RIP: a->shadow.rip = v; break;
    case PARAM_FB: a->shadow.feedback = v; break;
    case PARAM_GLIDE: a->shadow.glide_seconds = v; break;
    case PARAM_LEVEL: a->shadow.master_level = v; break;
    case PARAM_DETUNE: a->shadow.unison_detune = v; break;
    case PARAM_DRONE_HZ: a->drone_hz = v; break;
    case PARAM_MIX: a->shadow_verb.mix = v; break;
    case PARAM_GHOST: a->shadow_verb.ghost = v; break;
    case PARAM_VERB_DECAY: a->shadow_verb.decay = v; break;
    case PARAM_DAMP: a->shadow_verb.damp = v; break;
    case PARAM_HAUNT: a->shadow_verb.haunt = v; break;
    case PARAM_ATTACK: a->shadow_attack_s = v; break;
    case PARAM_ENV_DECAY: a->shadow_decay_s = v; break;
    case PARAM_SUSTAIN: a->shadow_sustain = v; break;
    case PARAM_RELEASE: a->shadow_release_s = v; break;
    case PARAM_CEILING: a->shadow_limiter_ceiling_db = v; break;
    case PARAM_OUTPUT: a->shadow_output_gain_db = v; break;
    case PARAM_MEL_RATE: a->shadow_melody.rate_hz = v; break;
    case PARAM_MEL_RANGE: a->shadow_melody.range_degrees = (uint8_t)v; break;
    case PARAM_MEL_ROOT: a->shadow_melody.root_midi = (uint8_t)v; break;
    case PARAM_CH_RATE: a->shadow_chandas.rate_hz = v; break;
    case PARAM_CH_MIX:
        /* mix at zero is the same as bypass */
        a->shadow_chandas.mix = v;
        a->shadow_chandas.enabled = v > 0.0f;
        break;
    case PARAM_CH_SPREAD: a->shadow_chandas.spread = v; break;
    case PARAM_CH_SIZE: a->shadow_chandas.size = v; break;
    case PARAM_CH_WARP: a->shadow_chandas.warp = v; break;
    case PARAM_CH_DIM: a->shadow_chandas.dimension = v; break;
    case PARAM_CH_TAIL: a->shadow_chandas.tail = v; break;
    case PARAM_COUNT: break;
    }
}

float param_default(const App *a, ParamId id) {
    /* patch defaults follow the loaded algorithm and ratio mode */
    Patch p = patch_init(a->shadow.algorithm, a->shadow.ratio_mode);
    VerbParams v = verb_params_default();
    EnvParams e = env_params_default();
    MelodyParams m = melody_params_default();
    ChandasParams h = chandas_params_default();
    switch (id) {
    case PARAM_INDEX: return p.index;
    case PARAM_RIP: return p.rip;
    case PARAM_FB: return p.feedback;
    case PARAM_GLIDE: return p.glide_seconds;
    case PARAM_LEVEL: return p.master_level;
    case PARAM_DETUNE: return p.unison_detune;
    case PARAM_DRONE_HZ: return START_HZ;
    case PARAM_MIX: return v.mix;
    case PARAM_GHOST: return v.ghost;
    case PARAM_VERB_DECAY: return v.decay;
    case PARAM_DAMP: return v.damp;
    case PARAM_HAUNT: return v.haunt;
    case PARAM_ATTACK: return e.attack_s;
    case PARAM_ENV_DECAY: return e.decay_s;
    case PARAM_SUSTAIN: return e.sustain;
    case PARAM_RELEASE: return e.release_s;
    case PARAM_CEILING: return LIMITER_CEILING_DB_DEFAULT;
    case PARAM_OUTPUT: return OUTPUT_GAIN_DB_DEFAULT;
    case PARAM_MEL_RATE: return m.rate_hz;
    case PARAM_MEL_RANGE: return (float)m.range_degrees;
    case PARAM_MEL_ROOT: return (float)m.root_midi;
    case PARAM_CH_RATE: return h.rate_hz;
    case PARAM_CH_MIX: return h.mix;
    case PARAM_CH_SPREAD: return h.spread;
    case PARAM_CH_SIZE: return h.size;
    case PARAM_CH_WARP: return h.warp;
    case PARAM_CH_DIM: return h.dimension;
    case PARAM_CH_TAIL: return h.tail;
    case PARAM_COUNT: break;
    }
    return 0.0f;
}

float param_pos(ParamId id, float v) {
    const Control *s = &PARAMS[id];
    float span = s->hi - s->lo;
    if (span < FLT_EPSILON) return 0.0f;
    switch (s->curve) {
    case CURVE_LOG: return position_of_log(v, s->lo, s->hi);
    case CURVE_ENV_TIME: return env_time_pos(v, s->lo);
    case CURVE_POWER:
        return powf(clamp01((v - s->lo) / span), 1.0f / s->shape);
    case CURVE_LINEAR: break;
    }
    return clamp01((v - s->lo) / span);
}

float param_at(ParamId id, float pos) {
    const Control *s = &PARAMS[id];
    pos = clamp01(pos);
    float v;
    switch (s->curve) {
    case CURVE_LOG: v = log_position(pos, s->lo, s->hi); break;
    case CURVE_ENV_TIME: v = env_time_at(pos, s->lo); break;
    case CURVE_POWER: v = s->lo + (s->hi - s->lo) * powf(pos, s->shape); break;
    default: v = s->lo + (s->hi - s->lo) * pos; break;
    }
    return s->integer ? roundf(v) : v;
}

void param_text(const App *a, ParamId id, char *out, size_t cap) {
    float v = param_get(a, id);
    if (PARAMS[id].fmt)
        snprintf(out, cap, PARAMS[id].fmt, (double)v);
    else if (v < 0.1f)
        snprintf(out, cap, "%.0f ms", (double)(v * 1000.0f));
    else
        snprintf(out, cap, "%.2f s", (double)v);
}

void params_send(App *a, int groups) {
    if (groups & PG_PATCH)
        app_send(a, (Event){.kind = EV_SET_PATCH, .u.patch = a->shadow});
    if (groups & PG_VERB)
        app_send(a, (Event){.kind = EV_SET_VERB, .u.verb = a->shadow_verb});
    if (groups & PG_MELODY)
        app_send(a, (Event){.kind = EV_SET_MELODY, .u.melody = a->shadow_melody});
    if (groups & PG_CHANDAS)
        app_send(a,
                 (Event){.kind = EV_SET_CHANDAS, .u.chandas = a->shadow_chandas});
    if (groups & PG_WARMTH)
        app_send(a, (Event){.kind = EV_SET_WARMTH, .u.f = a->shadow_warmth});
    if (groups & PG_LIMITER)
        app_send(a, (Event){.kind = EV_SET_LIMITER,
                            .u.limiter = {a->shadow_limiter_enabled,
                                          a->shadow_limiter_ceiling_db,
                                          a->shadow_output_gain_db}});
    if (groups & PG_DRONE)
        app_send(a, (Event){.kind = EV_GLIDE_TO, .u.f = a->drone_hz});
    if (groups & PG_ENV) gui_sync_adsr(a);
}

float log_position(float p, float lo, float hi) {
    if (lo < 1e-6f) lo = 1e-6f;
    float ratio = hi / lo;
    if (ratio < 1.0f + FLT_EPSILON) ratio = 1.0f + FLT_EPSILON;
    return lo * powf(ratio, clamp01(p));
}

float position_of_log(float v, float lo, float hi) {
    if (lo < 1e-6f) lo = 1e-6f;
    float ratio = hi / lo;
    if (ratio < 1.0f + FLT_EPSILON) ratio = 1.0f + FLT_EPSILON;
    float q = v / lo;
    if (q < 1e-6f) q = 1e-6f;
    return clamp01(logf(q) / logf(ratio));
}

float env_time_at(float t, float lo) {
    float floor = fmaxf(ENV_TIME_FLOOR, lo);
    t = clamp01(t);
    if (t <= 0.0f) return lo;
    return floor * powf(ENV_TIME_MAX / floor, t);
}

float env_time_pos(float v, float lo) {
    float floor = fmaxf(ENV_TIME_FLOOR, lo);
    if (!(v > floor)) return 0.0f;
    return clamp01(logf(v / floor) / logf(ENV_TIME_MAX / floor));
}

static bool adsr_same(const EnvParams *a, const EnvParams *b) {
    return a->attack_s == b->attack_s && a->decay_s == b->decay_s
           && a->release_s == b->release_s && a->sustain == b->sustain;
}

void gui_sync_adsr(App *a) {
    EnvParams want = env_params_default();
    want.attack_s = a->shadow_attack_s;
    want.decay_s = a->shadow_decay_s;
    want.sustain = a->shadow_sustain;
    want.release_s = a->shadow_release_s;
    if (adsr_same(&want, &a->adsr_sent)) return;
    a->adsr_sent = want;
    app_send(a, (Event){.kind = EV_SET_ADSR, .u.adsr = want});
}

