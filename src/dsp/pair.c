#include "dsp.h"
#include <math.h>
#include <string.h>

#define CHUNK 128

void voice_pair_init(VoicePair *p, float sample_rate, Patch patch) {
    voice_init(&p->voices[0], sample_rate, patch);
    voice_init(&p->voices[1], sample_rate, patch);
    p->target = 0;
    p->blend = 0.0f;
    p->step = 0.0f;
    p->sample_rate = sample_rate;
}

void voice_pair_free(VoicePair *p) {
    voice_free(&p->voices[0]);
    voice_free(&p->voices[1]);
}

static int pair_live(const VoicePair *p) {
    return p->target;
}

bool voice_pair_crossing(const VoicePair *p) {
    return p->step != 0.0f;
}

State voice_pair_state(const VoicePair *p) {
    const Voice *v = &p->voices[pair_live(p)];
    State s;
    s.patch = v->patch;
    s.chain = v->chain;
    return s;
}

static void pair_cross_to(VoicePair *p, State next) {
    int incoming = p->blend < 0.5f ? 1 : 0;
    voice_set_patch(&p->voices[incoming], next.patch);
    voice_set_chain(&p->voices[incoming], next.chain);
    p->target = incoming;
    float want = (float)incoming;
    float samples = fmaxf(CROSSFADE_SECONDS * p->sample_rate, 1.0f);
    p->step = (want - p->blend) / samples;
}

void voice_pair_set_state(VoicePair *p, State next) {
    State cur = voice_pair_state(p);
    if (!state_is_structural_change(&cur, &next)) {
        for (int i = 0; i < 2; i++) {
            voice_set_patch(&p->voices[i], next.patch);
            voice_set_chain(&p->voices[i], next.chain);
        }
        return;
    }
    pair_cross_to(p, next);
}

void voice_pair_set_patch(VoicePair *p, Patch patch) {
    State s;
    s.patch = patch;
    s.chain = p->voices[pair_live(p)].chain;
    voice_pair_set_state(p, s);
}

const Patch *voice_pair_patch(const VoicePair *p) {
    return &p->voices[pair_live(p)].patch;
}

const Compiled *voice_pair_compiled(const VoicePair *p) {
    return &p->voices[pair_live(p)].compiled;
}

void voice_pair_set_op_enabled(VoicePair *p, int op, bool on) {
    for (int i = 0; i < 2; i++) {
        voice_set_op_enabled(&p->voices[i], op, on);
    }
}

void voice_pair_set_freq_hz(VoicePair *p, float hz) {
    for (int i = 0; i < 2; i++) {
        voice_set_freq_hz(&p->voices[i], hz);
    }
}

void voice_pair_glide_to_hz(VoicePair *p, float hz) {
    for (int i = 0; i < 2; i++) {
        voice_glide_to_hz(&p->voices[i], hz);
    }
}

void voice_pair_note_on(VoicePair *p, float hz, float velocity) {
    for (int i = 0; i < 2; i++) {
        voice_note_on(&p->voices[i], hz, velocity);
    }
}

bool voice_pair_note_sounding(const VoicePair *p) {
    return voice_note_sounding(&p->voices[pair_live(p)]);
}

const Chain *voice_pair_chain(const VoicePair *p) {
    return &p->voices[pair_live(p)].chain;
}

void voice_pair_set_bend_semitones(VoicePair *p, float semitones) {
    for (int i = 0; i < 2; i++) {
        voice_set_bend_semitones(&p->voices[i], semitones);
    }
}

void voice_pair_note_off(VoicePair *p) {
    for (int i = 0; i < 2; i++) {
        voice_note_off(&p->voices[i]);
    }
}

float voice_pair_target_hz(const VoicePair *p) {
    return voice_target_hz(&p->voices[pair_live(p)]);
}

static bool chain_eq(const Chain *a, const Chain *b) {
    if (a->amp.kind != b->amp.kind) return false;
    if (a->amp.kind == AMP_ENVELOPE) {
        return a->amp.env.attack_s == b->amp.env.attack_s &&
               a->amp.env.decay_s == b->amp.env.decay_s &&
               a->amp.env.release_s == b->amp.env.release_s &&
               a->amp.env.curve == b->amp.env.curve;
    }
    return true;
}

typedef struct {
    Frame *a_buf;
    size_t i;
} ACapture;

static void pair_a_emit(void *userdata, size_t n, const Frame *frame) {
    ACapture *c = userdata;
    c->a_buf[c->i] = *frame;
    c->i += 1;
}

typedef struct {
    const Frame *a_buf;
    size_t k;
    size_t done;
    float *blend;
    float *step;
    FrameEmit emit;
    void *userdata;
} BCapture;

static void pair_b_emit(void *userdata, size_t n, const Frame *b) {
    BCapture *c = userdata;
    float t = clampf(*c->blend, 0.0f, 1.0f);
    float ga = 1.0f - t;
    float gb = t;
    const Frame *a = &c->a_buf[c->k];
    Frame f = *a;
    for (int op = 0; op < NUM_OPS; op++) {
        f.ops[op] = a->ops[op] * ga + b->ops[op] * gb;
    }
    f.mix = a->mix * ga + b->mix * gb;
    f.master = a->master * ga + b->master * gb;
    f.field = a->field * ga + b->field * gb;
    f.base_hz = a->base_hz * ga + b->base_hz * gb;
    c->emit(c->userdata, c->done + c->k, &f);
    c->k += 1;
    if (*c->step != 0.0f) {
        *c->blend += *c->step;
        if ((*c->step > 0.0f && *c->blend >= 1.0f) ||
            (*c->step < 0.0f && *c->blend <= 0.0f)) {
            *c->blend = *c->step > 0.0f ? 1.0f : 0.0f;
            *c->step = 0.0f;
        }
    }
}

void voice_pair_render_frames(VoicePair *p, size_t count, FrameEmit emit, void *userdata) {
    size_t done = 0;
    Frame a_buf[CHUNK];
    memset(a_buf, 0, sizeof a_buf);
    while (done < count) {
        size_t run = CHUNK < count - done ? CHUNK : count - done;
        {
            ACapture ac = { a_buf, 0 };
            voice_render_frames(&p->voices[0], run, pair_a_emit, &ac);
            BCapture bc = { a_buf, 0, done, &p->blend, &p->step, emit, userdata };
            voice_render_frames(&p->voices[1], run, pair_b_emit, &bc);
        }
        if (p->step == 0.0f) {
            int t = p->target;
            Patch patch = p->voices[t].patch;
            Chain chain = p->voices[t].chain;
            int idle = 1 - t;
            bool stale = p->voices[idle].patch.ratio_mode != patch.ratio_mode ||
                         p->voices[idle].patch.algorithm != patch.algorithm ||
                         !chain_eq(&p->voices[idle].chain, &chain);
            if (stale) {
                voice_set_patch(&p->voices[idle], patch);
                voice_set_chain(&p->voices[idle], chain);
            }
        }
        done += run;
    }
}
