#include "dsp.h"

EnvParams env_params_default(void) {
    EnvParams p = {0.008f, 2.0f, 2.0f, 0.5f, 1.0f};
    return p;
}

float velocity_for_level(float position) {
    float p = clampf(position, 0.0f, 1.0f);
    return p / (VELOCITY_CEILING + (1.0f - VELOCITY_CEILING) * p);
}

/* A setting moved mid-note: hold the level where it is and move the clock
   to the point the new shape reaches it, so nothing steps. */
static void env_reanchor(Envelope *e, const EnvParams *p) {
    float curve = curve_exponent(p->curve);
    if (e->stage == ENV_HELD) {
        float a_old = fmaxf(e->seen.attack_s, ENV_ATTACK_MIN);
        float a_new = fmaxf(p->attack_s, ENV_ATTACK_MIN);
        if (e->t < a_old) {
            float span = e->peak - e->from;
            float u = fabsf(span) > 1e-9f ? (e->level - e->from) / span : 1.0f;
            e->t = clampf(u, 0.0f, 1.0f) * a_new;
            return;
        }
        float sustain = e->sustain * clampf(p->sustain, 0.0f, 1.0f);
        float span = e->peak - sustain;
        if (span > 1e-6f && e->level > sustain) {
            float remaining =
                powf(clampf((e->level - sustain) / span, 0.0f, 1.0f), 1.0f / curve);
            e->t = a_new + (1.0f - remaining) * fmaxf(p->decay_s, 1e-4f);
        } else {
            /* the sustain has come up past the level: rise to it from here */
            e->peak = e->level;
            e->t = a_new;
        }
    } else if (e->stage == ENV_RELEASED) {
        if (e->from > 1e-9f) {
            float remaining = powf(clampf(e->level / e->from, 0.0f, 1.0f), 1.0f / curve);
            e->t = (1.0f - remaining) * fmaxf(p->release_s, 1e-4f);
        } else {
            e->t = 0.0f;
        }
    }
}

static bool env_params_moved(const EnvParams *a, const EnvParams *b) {
    return a->attack_s != b->attack_s || a->decay_s != b->decay_s
           || a->release_s != b->release_s || a->curve != b->curve
           || a->sustain != b->sustain;
}

void envelope_init(Envelope *e, float sample_rate) {
    e->seen = env_params_default();
    e->stage = ENV_IDLE;
    e->t = 0.0f;
    e->from = 0.0f;
    e->level = 0.0f;
    e->peak = 0.0f;
    e->sustain = 0.0f;
    e->sample_rate = fmaxf(sample_rate, 1.0f);
}

void envelope_note_on(Envelope *e, float velocity) {
    float v = clampf(velocity, 0.0f, 1.0f);
    e->from = e->level;
    e->stage = ENV_HELD;
    e->t = 0.0f;
    e->peak = v;
    e->sustain = v * v;
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

float envelope_amplitude(const Envelope *e, float trim) {
    return e->level * (VELOCITY_CEILING + (1.0f - VELOCITY_CEILING) * clampf(trim, 0.0f, 1.0f));
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
            float d = fmaxf(p->decay_s, 1e-4f);
            float remaining = 1.0f - clampf((e->t - a) / d, 0.0f, 1.0f);
            float sustain = e->sustain * clampf(p->sustain, 0.0f, 1.0f);
            e->level = sustain + (e->peak - sustain) * powf(remaining, curve_exponent(p->curve));
        }
        e->t += dt;
        break;
    }
    case ENV_RELEASED: {
        float r = fmaxf(p->release_s, 1e-4f);
        float remaining = 1.0f - clampf(e->t / r, 0.0f, 1.0f);
        e->level = e->from * powf(remaining, curve_exponent(p->curve));
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
