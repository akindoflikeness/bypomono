#include "dsp.h"

EnvParams env_params_default(void) {
    EnvParams p = {0.008f, 2.0f, 2.0f, 1.0f};
    return p;
}

/* Decay and release fall a fixed number of dB per second, the way an analog
   envelope does. A stage's time is how long its gap takes to close by 80 dB,
   which is where ENV_FLOOR ends a release, so the release time is the time
   to silence. Attack is a straight line. */
#define ENV_STAGE_DB 80.0f

/* the fraction of the gap left after t seconds of a stage lasting secs */
static float env_fall(float t, float secs) {
    return powf(10.0f, -(ENV_STAGE_DB / 20.0f) * t / fmaxf(secs, 1e-4f));
}

/* inverse of env_fall: the time at which that fraction is left */
static float env_fall_time(float remaining, float secs) {
    remaining = clampf(remaining, 1e-9f, 1.0f);
    return -log10f(remaining) * 20.0f / ENV_STAGE_DB * fmaxf(secs, 1e-4f);
}

/* A setting moved mid-note: hold the level where it is and move the clock
   to the point the new shape reaches it, so nothing steps. */
static void env_reanchor(Envelope *e, const EnvParams *p) {
    if (e->stage == ENV_HELD) {
        float a_old = fmaxf(e->seen.attack_s, ENV_ATTACK_MIN);
        float a_new = fmaxf(p->attack_s, ENV_ATTACK_MIN);
        if (e->t < a_old) {
            float span = e->peak - e->from;
            float u = fabsf(span) > 1e-9f ? (e->level - e->from) / span : 1.0f;
            e->t = clampf(u, 0.0f, 1.0f) * a_new;
            return;
        }
        float sustain = clampf(p->sustain, 0.0f, 1.0f);
        float span = e->peak - sustain;
        if (span > 1e-6f && e->level > sustain) {
            e->t = a_new + env_fall_time((e->level - sustain) / span, p->decay_s);
        } else {
            /* the sustain has come up past the level: rise to it from here */
            e->peak = e->level;
            e->t = a_new;
        }
    } else if (e->stage == ENV_RELEASED) {
        e->t = e->from > 1e-9f ? env_fall_time(e->level / e->from, p->release_s)
                               : 0.0f;
    }
}

static bool env_params_moved(const EnvParams *a, const EnvParams *b) {
    return a->attack_s != b->attack_s || a->decay_s != b->decay_s
           || a->release_s != b->release_s || a->sustain != b->sustain;
}

void envelope_init(Envelope *e, float sample_rate) {
    e->seen = env_params_default();
    e->stage = ENV_IDLE;
    e->t = 0.0f;
    e->from = 0.0f;
    e->level = 0.0f;
    e->peak = 0.0f;
    e->sample_rate = fmaxf(sample_rate, 1.0f);
}

void envelope_note_on(Envelope *e) {
    e->from = e->level;
    e->stage = ENV_HELD;
    e->t = 0.0f;
    e->peak = 1.0f;
}

void envelope_note_off(Envelope *e) {
    if (e->stage == ENV_HELD) {
        e->from = e->level;
        e->stage = ENV_RELEASED;
        e->t = 0.0f;
    }
}

bool envelope_active(const Envelope *e) {
    return e->stage != ENV_IDLE;
}

float envelope_level(const Envelope *e) {
    return e->level;
}

float envelope_tick(Envelope *e, const EnvParams *p) {
    if (env_params_moved(p, &e->seen)) {
        env_reanchor(e, p);
        e->seen = *p;
    }
    float dt = 1.0f / e->sample_rate;
    switch (e->stage) {
    case ENV_IDLE:
        e->level = 0.0f;
        break;
    case ENV_HELD: {
        float a = fmaxf(p->attack_s, ENV_ATTACK_MIN);
        if (e->t < a) {
            float x = e->t / a;
            e->level = e->from + (e->peak - e->from) * x;
        } else {
            float sustain = clampf(p->sustain, 0.0f, 1.0f);
            e->level = sustain + (e->peak - sustain) * env_fall(e->t - a, p->decay_s);
        }
        e->t += dt;
        break;
    }
    case ENV_RELEASED: {
        e->level = e->from * env_fall(e->t, p->release_s);
        e->t += dt;
        if (e->level < ENV_FLOOR) {
            e->level = 0.0f;
            e->stage = ENV_IDLE;
        }
        break;
    }
    }
    return e->level;
}
