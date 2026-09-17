#include "dsp.h"
#include <math.h>
#include <string.h>

const ModTargetSpec MOD_TARGETS[MT_COUNT] = {
    [MT_NONE] = {"", 0.0f, 0.0f},
    [MT_INDEX] = {"index", 0.0f, 1.0f},
    [MT_RIP] = {"rip", 0.0f, 1.0f},
    [MT_FB] = {"fb", 0.0f, 1.0f},
    [MT_FIELD] = {"field", 0.0f, 1.0f},
    [MT_CURVE] = {"curve", 0.0f, 1.0f},
    [MT_LEVEL] = {"level", 0.0f, 1.0f},
    [MT_PITCH] = {"pitch", -MOD_PITCH_SEMITONES, MOD_PITCH_SEMITONES},
    [MT_MIX] = {"mix", 0.0f, 1.0f},
    [MT_GHOST] = {"ghost", 0.0f, 1.0f},
    [MT_DECAY] = {"decay", 0.05f, 8.0f},
    [MT_DAMP] = {"damp", 0.0f, 0.99f},
    [MT_HAUNT] = {"haunt", 0.0f, 1.0f},
    [MT_CH_MIX] = {"chandas mix", 0.0f, 1.0f},
    [MT_CH_RATE] = {"chandas rate", 0.1f, 8.0f},
    [MT_CH_SPREAD] = {"chandas spread", 0.0f, 1.0f},
    [MT_CH_SIZE] = {"chandas size", CHANDAS_MIN_SIZE, CHANDAS_MAX_SIZE},
    [MT_CH_WARP] = {"chandas warp", 0.0f, 1.0f},
    [MT_CH_DIM] = {"chandas dim", 0.0f, 1.0f},
    [MT_CH_TAIL] = {"chandas tail", 0.0f, 1.0f},
    [MT_MEL_RATE] = {"mel rate", 0.1f, 8.0f},
    [MT_WARMTH] = {"warmth", MIN_WARMTH, MAX_WARMTH},
};

static const char *const SHAPE_NAMES[LFO_SHAPE_COUNT] = {
    "sine", "tri", "saw", "ramp", "square", "exp", "sh", "drift"};
static const char *const MODE_NAMES[LFO_MODE_COUNT] = {"free", "retrig",
                                                       "once"};

const char *lfo_shape_name(LfoShape s) {
    return (unsigned)s < LFO_SHAPE_COUNT ? SHAPE_NAMES[s] : SHAPE_NAMES[0];
}

const char *lfo_mode_name(LfoMode m) {
    return (unsigned)m < LFO_MODE_COUNT ? MODE_NAMES[m] : MODE_NAMES[0];
}

LfoParams lfo_params_default(void) {
    LfoParams p;
    memset(&p, 0, sizeof p);
    p.used = true;
    p.shape = LFO_SINE;
    p.mode = LFO_FREE;
    p.division = -1;
    p.rate_hz = 1.0f;
    return p;
}

ModBank mod_bank_default(void) {
    ModBank b;
    memset(&b, 0, sizeof b);
    for (int i = 0; i < MOD_LFOS; i++) {
        b.lfo[i] = lfo_params_default();
        b.lfo[i].used = false;
    }
    return b;
}

ModBank mod_bank_sanitize(ModBank b) {
    for (int i = 0; i < MOD_LFOS; i++) {
        LfoParams *p = &b.lfo[i];
        if (p->shape >= LFO_SHAPE_COUNT) p->shape = LFO_SINE;
        if (p->mode >= LFO_MODE_COUNT) p->mode = LFO_FREE;
        if (p->division < -1 || p->division >= CHANDAS_DIVISIONS_LEN)
            p->division = -1;
        p->rate_hz = clampf(p->rate_hz, LFO_RATE_MIN_HZ, LFO_RATE_MAX_HZ);
        p->phase = clampf(p->phase, 0.0f, 1.0f);
    }
    for (int i = 0; i < MOD_ROUTES; i++) {
        ModRoute *r = &b.route[i];
        if (r->target >= MT_COUNT || r->lfo >= MOD_LFOS
            || !b.lfo[r->lfo].used) {
            memset(r, 0, sizeof *r);
            continue;
        }
        r->depth = clampf(r->depth, -1.0f, 1.0f);
    }
    return b;
}

int mod_bank_find_route(const ModBank *b, int lfo, ModTarget t) {
    for (int i = 0; i < MOD_ROUTES; i++)
        if (b->route[i].target == t && b->route[i].lfo == lfo) return i;
    return -1;
}

float lfo_effective_hz(const LfoParams *p, float bpm) {
    if (p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN) {
        float beats = CHANDAS_DIVISIONS[p->division].beats;
        return clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM) / 60.0f / beats;
    }
    return clampf(p->rate_hz, LFO_RATE_MIN_HZ, LFO_RATE_MAX_HZ);
}

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float rand_bipolar(uint32_t *s) {
    return (float)(xorshift(s) >> 8) / 8388607.5f - 1.0f;
}

/* bipolar value of the deterministic shapes */
static float shape_bipolar(LfoShape shape, float ph) {
    switch (shape) {
    case LFO_SINE: return sinf(2.0f * (float)M_PI * ph);
    case LFO_TRIANGLE:
        return ph < 0.25f ? 4.0f * ph
               : ph < 0.75f ? 2.0f - 4.0f * ph
                            : 4.0f * ph - 4.0f;
    case LFO_SAW: return 1.0f - 2.0f * ph;
    case LFO_RAMP: return 2.0f * ph - 1.0f;
    case LFO_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
    case LFO_EXP: return 2.0f * expf(-5.0f * ph) - 1.0f;
    default: return 0.0f;
    }
}

static float polarise(const LfoParams *p, float bipolar) {
    return p->unipolar ? 0.5f * (bipolar + 1.0f) : bipolar;
}

float lfo_shape_at(const LfoParams *p, float phase, uint32_t seed) {
    float ph = fract_pos(phase);
    if (p->shape == LFO_SH || p->shape == LFO_DRIFT) {
        /* four cycles of held values, the same four every drawing */
        uint32_t cycle = (uint32_t)floorf(phase);
        uint32_t s = seed * 2654435761u + cycle * 40503u + 1u;
        float a = rand_bipolar(&s);
        if (p->shape == LFO_SH) return polarise(p, a);
        uint32_t s2 = seed * 2654435761u + (cycle + 1u) * 40503u + 1u;
        float b = rand_bipolar(&s2);
        float t = 0.5f - 0.5f * cosf((float)M_PI * ph);
        return polarise(p, a + (b - a) * t);
    }
    return polarise(p, shape_bipolar((LfoShape)p->shape, ph));
}

void mod_init(Mod *m, float sample_rate) {
    memset(m, 0, sizeof *m);
    m->bank = mod_bank_default();
    m->sample_rate = sample_rate > 0.0f ? sample_rate : 48000.0f;
    for (int i = 0; i < MOD_LFOS; i++) m->st[i].rng = 0x51ED270Bu + (uint32_t)i;
}

static void lfo_restart(const LfoParams *p, LfoState *s) {
    s->phase = p->phase;
    s->done = false;
    s->from = s->to;
    s->to = rand_bipolar(&s->rng);
}

void mod_set_lfo(Mod *m, int slot, LfoParams p) {
    if (slot < 0 || slot >= MOD_LFOS) return;
    bool was = m->bank.lfo[slot].used;
    m->bank.lfo[slot] = p;
    if (p.used && !was) lfo_restart(&p, &m->st[slot]);
}

void mod_set_route(Mod *m, int slot, ModRoute r) {
    if (slot < 0 || slot >= MOD_ROUTES) return;
    if (r.target >= MT_COUNT || r.lfo >= MOD_LFOS) r.target = MT_NONE;
    m->bank.route[slot] = r;
}

void mod_note_on(Mod *m) {
    for (int i = 0; i < MOD_LFOS; i++) {
        const LfoParams *p = &m->bank.lfo[i];
        if (p->used && p->mode != LFO_FREE) lfo_restart(p, &m->st[i]);
    }
}

bool mod_any_lfo(const Mod *m) {
    for (int i = 0; i < MOD_LFOS; i++)
        if (m->bank.lfo[i].used) return true;
    return false;
}

void mod_advance(Mod *m, size_t samples, float bpm) {
    float dt = (float)samples / m->sample_rate;
    for (int i = 0; i < MOD_LFOS; i++) {
        const LfoParams *p = &m->bank.lfo[i];
        LfoState *s = &m->st[i];
        if (!p->used) continue;
        if (!s->done) {
            float next = s->phase + lfo_effective_hz(p, bpm) * dt;
            if (p->mode == LFO_ONCE && next >= p->phase + 1.0f) {
                next = p->phase + 1.0f;
                s->done = true;
            }
            if (floorf(next) != floorf(s->phase)) {
                s->from = s->to;
                s->to = rand_bipolar(&s->rng);
            }
            s->phase = p->mode == LFO_ONCE ? next : fract_pos(next);
        }
        float ph = fract_pos(s->phase);
        if (s->done) ph = 1.0f - 1e-6f;
        float bi;
        if (p->shape == LFO_SH) {
            bi = s->from;
        } else if (p->shape == LFO_DRIFT) {
            float t = 0.5f - 0.5f * cosf((float)M_PI * ph);
            bi = s->from + (s->to - s->from) * t;
        } else {
            bi = shape_bipolar((LfoShape)p->shape, ph);
        }
        s->value = polarise(p, bi);
    }
}

static int target_group(ModTarget t) {
    switch (t) {
    case MT_INDEX: case MT_RIP: case MT_FB: case MT_FIELD: case MT_CURVE:
    case MT_LEVEL: return MOD_G_PATCH;
    case MT_PITCH: return MOD_G_BEND;
    case MT_MIX: case MT_GHOST: case MT_DECAY: case MT_DAMP: case MT_HAUNT:
        return MOD_G_VERB;
    case MT_CH_MIX: case MT_CH_RATE: case MT_CH_SPREAD: case MT_CH_SIZE:
    case MT_CH_WARP: case MT_CH_DIM: case MT_CH_TAIL: return MOD_G_CHANDAS;
    case MT_MEL_RATE: return MOD_G_MELODY;
    case MT_WARMTH: return MOD_G_WARMTH;
    default: return 0;
    }
}

static float *target_slot(ModBase *b, ModTarget t) {
    switch (t) {
    case MT_INDEX: return &b->patch.index;
    case MT_RIP: return &b->patch.rip;
    case MT_FB: return &b->patch.feedback;
    case MT_FIELD: return &b->patch.field;
    case MT_CURVE: return &b->patch.curve;
    case MT_LEVEL: return &b->patch.master_level;
    case MT_PITCH: return &b->bend;
    case MT_MIX: return &b->verb.mix;
    case MT_GHOST: return &b->verb.ghost;
    case MT_DECAY: return &b->verb.decay;
    case MT_DAMP: return &b->verb.damp;
    case MT_HAUNT: return &b->verb.haunt;
    case MT_CH_MIX: return &b->chandas.mix;
    case MT_CH_RATE: return &b->chandas.rate_hz;
    case MT_CH_SPREAD: return &b->chandas.spread;
    case MT_CH_SIZE: return &b->chandas.size;
    case MT_CH_WARP: return &b->chandas.warp;
    case MT_CH_DIM: return &b->chandas.dimension;
    case MT_CH_TAIL: return &b->chandas.tail;
    case MT_MEL_RATE: return &b->melody.rate_hz;
    case MT_WARMTH: return &b->warmth;
    default: return NULL;
    }
}

int mod_apply(Mod *m, const ModBase *base, ModBase *out) {
    *out = *base;
    int groups = 0;
    for (int i = 0; i < MOD_ROUTES; i++) {
        const ModRoute *r = &m->bank.route[i];
        if (r->target == MT_NONE || r->target >= MT_COUNT) continue;
        if (!m->bank.lfo[r->lfo].used) continue;
        const ModTargetSpec *spec = &MOD_TARGETS[r->target];
        float *slot = target_slot(out, (ModTarget)r->target);
        if (!slot) continue;
        float span = r->target == MT_PITCH ? MOD_PITCH_SEMITONES
                                           : spec->max - spec->min;
        *slot += r->depth * m->st[r->lfo].value * span;
        groups |= target_group((ModTarget)r->target);
    }
    for (int t = MT_INDEX; t < MT_COUNT; t++) {
        if (t == MT_PITCH) continue;
        float *slot = target_slot(out, (ModTarget)t);
        if (slot) *slot = clampf(*slot, MOD_TARGETS[t].min, MOD_TARGETS[t].max);
    }
    int write = groups | m->groups_prev;
    m->groups_prev = groups;
    m->groups = groups;
    return write;
}
