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
};

static const char *const MODE_NAMES[SEQ_MODE_COUNT] = {"loop", "once"};
static const char *const FILL_NAMES[SEQ_FILL_COUNT] = {
    "flat", "sine", "saw", "ramp", "tri", "square", "random"};

const char *seq_mode_name(SeqMode m) {
    return (unsigned)m < SEQ_MODE_COUNT ? MODE_NAMES[m] : MODE_NAMES[0];
}

const char *seq_fill_name(SeqFill f) {
    return (unsigned)f < SEQ_FILL_COUNT ? FILL_NAMES[f] : FILL_NAMES[0];
}

SeqParams seq_params_default(void) {
    SeqParams p;
    memset(&p, 0, sizeof p);
    p.used = true;
    p.mode = SEQ_LOOP;
    p.smooth = true;
    p.division = SEQ_DEFAULT_DIVISION;
    p.length_s = 2.0f;
    for (int k = 0; k < SEQ_STEPS; k++) p.value[k] = 0.5f;
    return p;
}

ModBank mod_bank_default(void) {
    ModBank b;
    memset(&b, 0, sizeof b);
    for (int i = 0; i < SEQS; i++) {
        b.seq[i] = seq_params_default();
        b.seq[i].used = false;
    }
    return b;
}

ModBank mod_bank_sanitize(ModBank b) {
    for (int i = 0; i < SEQS; i++) {
        SeqParams *p = &b.seq[i];
        if (p->mode >= SEQ_MODE_COUNT) p->mode = SEQ_LOOP;
        if (p->division < -1 || p->division >= CHANDAS_DIVISIONS_LEN)
            p->division = -1;
        p->length_s = clampf(p->length_s, SEQ_LENGTH_MIN_S, SEQ_LENGTH_MAX_S);
        for (int k = 0; k < SEQ_STEPS; k++)
            p->value[k] = clampf(p->value[k], 0.0f, 1.0f);
    }
    for (int i = 0; i < MOD_ROUTES; i++) {
        ModRoute *r = &b.route[i];
        if (r->target >= MT_COUNT || r->seq >= SEQS || !b.seq[r->seq].used) {
            memset(r, 0, sizeof *r);
            continue;
        }
        r->depth = clampf(r->depth, -1.0f, 1.0f);
        if (r->target != MT_PITCH) r->snap = false;
    }
    return b;
}

int mod_bank_find_route(const ModBank *b, int seq, ModTarget t) {
    for (int i = 0; i < MOD_ROUTES; i++)
        if (b->route[i].target == t && b->route[i].seq == seq) return i;
    return -1;
}

bool seq_has_pitch_route(const ModBank *b, int seq) {
    return mod_bank_find_route(b, seq, MT_PITCH) >= 0;
}

float seq_step_seconds(const SeqParams *p, float bpm) {
    if (p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN)
        return CHANDAS_DIVISIONS[p->division].beats * 60.0f
               / clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
    return clampf(p->length_s, SEQ_LENGTH_MIN_S, SEQ_LENGTH_MAX_S)
           / (float)SEQ_STEPS;
}

/* ---------- the curve ---------- */

static float step_value(const SeqParams *p, int k) {
    if (p->mode == SEQ_LOOP) k = ((k % SEQ_STEPS) + SEQ_STEPS) % SEQ_STEPS;
    else k = k < 0 ? 0 : (k >= SEQ_STEPS ? SEQ_STEPS - 1 : k);
    return p->value[k];
}

/* Fritsch-Butland slope: zero at a peak or a valley, otherwise the harmonic
   mean of the two neighbouring slopes, which keeps each span monotone */
static float slope_at(const SeqParams *p, int k) {
    if (p->mode == SEQ_ONCE && (k <= 0 || k >= SEQ_STEPS - 1)) return 0.0f;
    float d0 = step_value(p, k) - step_value(p, k - 1);
    float d1 = step_value(p, k + 1) - step_value(p, k);
    if (d0 * d1 <= 0.0f) return 0.0f;
    return 2.0f * d0 * d1 / (d0 + d1);
}

float seq_value_at(const SeqParams *p, float pos) {
    if (!p->smooth) {
        int k = (int)floorf(pos);
        if (p->mode == SEQ_ONCE && pos >= (float)SEQ_STEPS) k = SEQ_STEPS - 1;
        return step_value(p, k);
    }
    /* the values sit at step centres */
    float x = pos - 0.5f;
    if (p->mode == SEQ_ONCE) {
        if (x <= 0.0f) return p->value[0];
        if (x >= (float)(SEQ_STEPS - 1)) return p->value[SEQ_STEPS - 1];
    }
    int k = (int)floorf(x);
    float t = x - (float)k;
    float y0 = step_value(p, k), y1 = step_value(p, k + 1);
    float m0 = slope_at(p, k), m1 = slope_at(p, k + 1);
    float t2 = t * t, t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * y0 + (t3 - 2.0f * t2 + t) * m0
           + (-2.0f * t3 + 3.0f * t2) * y1 + (t3 - t2) * m1;
}

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float fill_at(SeqFill f, float ph, uint32_t *rng) {
    switch (f) {
    case SEQ_FILL_SINE: return 0.5f + 0.5f * sinf(TAU_F * ph);
    case SEQ_FILL_SAW: return 1.0f - ph;
    case SEQ_FILL_RAMP: return ph;
    case SEQ_FILL_TRI: return ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph;
    case SEQ_FILL_SQUARE: return ph < 0.5f ? 1.0f : 0.0f;
    case SEQ_FILL_RANDOM: return (float)(xorshift(rng) >> 8) / 16777215.0f;
    default: return 0.5f;
    }
}

void seq_fill(SeqParams *p, SeqFill f, uint32_t seed) {
    uint32_t rng = seed * 2654435761u + 1u;
    for (int k = 0; k < SEQ_STEPS; k++)
        p->value[k] = fill_at(f, (float)k / (float)SEQ_STEPS, &rng);
}

/* ---------- running ---------- */

void mod_init(Mod *m, float sample_rate) {
    memset(m, 0, sizeof *m);
    m->bank = mod_bank_default();
    m->sample_rate = sample_rate > 0.0f ? sample_rate : 48000.0f;
}

static void seq_restart(SeqState *s) {
    s->pos = 0.0f;
    s->done = false;
}

void mod_set_seq(Mod *m, int slot, SeqParams p) {
    if (slot < 0 || slot >= SEQS) return;
    bool was = m->bank.seq[slot].used;
    m->bank.seq[slot] = p;
    if (p.used && !was) seq_restart(&m->st[slot]);
}

void mod_set_route(Mod *m, int slot, ModRoute r) {
    if (slot < 0 || slot >= MOD_ROUTES) return;
    if (r.target >= MT_COUNT || r.seq >= SEQS) r.target = MT_NONE;
    m->bank.route[slot] = r;
}

void mod_note_on(Mod *m) {
    for (int i = 0; i < SEQS; i++)
        if (m->bank.seq[i].used && m->bank.seq[i].mode == SEQ_ONCE)
            seq_restart(&m->st[i]);
}

bool mod_any_seq(const Mod *m) {
    for (int i = 0; i < SEQS; i++)
        if (m->bank.seq[i].used) return true;
    return false;
}

void mod_advance(Mod *m, size_t samples, float bpm) {
    float dt = (float)samples / m->sample_rate;
    for (int i = 0; i < SEQS; i++) {
        const SeqParams *p = &m->bank.seq[i];
        SeqState *s = &m->st[i];
        if (!p->used) continue;
        if (!s->done) {
            float next = s->pos + dt / seq_step_seconds(p, bpm);
            if (p->mode == SEQ_ONCE && next >= (float)SEQ_STEPS) {
                next = (float)SEQ_STEPS;
                s->done = true;
            }
            if (p->mode == SEQ_LOOP) next = fmodf(next, (float)SEQ_STEPS);
            s->pos = next;
        }
        s->value = 2.0f * seq_value_at(p, s->pos) - 1.0f;
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
    default: return NULL;
    }
}

int mod_apply(Mod *m, const ModBase *base, ModBase *out) {
    *out = *base;
    int groups = 0;
    for (int i = 0; i < MOD_ROUTES; i++) {
        const ModRoute *r = &m->bank.route[i];
        if (r->target == MT_NONE || r->target >= MT_COUNT) continue;
        if (!m->bank.seq[r->seq].used) continue;
        float *slot = target_slot(out, (ModTarget)r->target);
        if (!slot) continue;
        const ModTargetSpec *spec = &MOD_TARGETS[r->target];
        float span = r->target == MT_PITCH ? MOD_PITCH_SEMITONES
                                           : spec->max - spec->min;
        float add = r->depth * m->st[r->seq].value * span;
        if (r->snap && r->target == MT_PITCH) add = roundf(add);
        *slot += add;
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
