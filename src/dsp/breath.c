#include "dsp.h"

#define REST_FRACTION (1.0f / (PHI * PHI))
#define GESTURE_FRACTION (1.0f / PHI)
#define LONE_NOTE_S 0.987f
#define MIN_INTERVAL_S 0.05f
#define MAX_INTERVAL_S 8.0f
#define RELEASE_BOOST (1.0f / PHI)

float curve_exponent(float curve) {
    return powf(PHI, BREATH_CURVE_RANGE * (clampf(curve, 0.0f, 1.0f) - 0.5f));
}

static void signature(RatioMode mode, float out[5]) {
    uint32_t raw[5];
    switch (mode) {
    case RATIO_HARMONIC:
        raw[0] = 30; raw[1] = 24; raw[2] = 18; raw[3] = 12; raw[4] = 6;
        break;
    case RATIO_FIBONACCI:
        raw[0] = 34; raw[1] = 21; raw[2] = 13; raw[3] = 8; raw[4] = 5;
        break;
    case RATIO_GOLDEN:
        raw[0] = 29; raw[1] = 18; raw[2] = 11; raw[3] = 7; raw[4] = 4;
        break;
    case RATIO_GOLDEN_MIRROR:
        raw[0] = 4; raw[1] = 7; raw[2] = 11; raw[3] = 18; raw[4] = 29;
        break;
    case RATIO_PLASTIC:
    default:
        raw[0] = 28; raw[1] = 21; raw[2] = 16; raw[3] = 12; raw[4] = 9;
        break;
    }
    uint32_t max = raw[0];
    for (int i = 1; i < 5; i++) {
        if (raw[i] > max) {
            max = raw[i];
        }
    }
    for (int i = 0; i < 5; i++) {
        out[i] = (float)raw[i] / (float)max;
    }
}

void breath_init(Breath *b, float sample_rate, RatioMode mode) {
    b->sample_rate = fmaxf(sample_rate, 1.0f);
    for (int i = 0; i < 5; i++) {
        b->phase[i] = fract_pos((float)i / PHI);
    }
    b->mode = mode;
    b->has_trigger = false;
    b->since_trigger = 0.0f;
    b->interval = LONE_NOTE_S;
    b->last_amount = 0.0f;
    b->declick_from = 0.0f;
    b->declick_left = 0.0f;
    b->boost_level = 1.0f;
}

void breath_set_mode(Breath *b, RatioMode mode) {
    b->mode = mode;
}

static void breath_begin_arrival(Breath *b) {
    b->declick_from = b->last_amount;
    b->declick_left = BREATH_DECLICK_S;
}

static void breath_begin(Breath *b) {
    if (b->has_trigger) {
        float elapsed = b->since_trigger;
        if (elapsed >= MIN_INTERVAL_S && b->boost_level >= 1.0f) {
            b->interval = fminf(elapsed, MAX_INTERVAL_S);
        }
    }
    for (int i = 0; i < 5; i++) {
        b->phase[i] = 0.25f;
    }
    b->has_trigger = true;
    b->since_trigger = 0.0f;
    b->boost_level = 1.0f;
    breath_begin_arrival(b);
}

void breath_trigger(Breath *b) {
    breath_begin(b);
}

static float breath_gesture_s(const Breath *b);

void breath_drift(Breath *b) {
    if (b->has_trigger && b->since_trigger < breath_gesture_s(b)) return;
    breath_begin(b);
}

static float breath_gesture_s(const Breath *b) {
    return clampf(b->interval * GESTURE_FRACTION, MIN_INTERVAL_S * GESTURE_FRACTION, LONE_NOTE_S);
}

static float breath_boost(const Breath *b, float curve) {
    if (!b->has_trigger) {
        return 0.0f;
    }
    float remaining = 1.0f - clampf(b->since_trigger / breath_gesture_s(b), 0.0f, 1.0f);
    return b->boost_level * powf(remaining, curve_exponent(curve));
}

void breath_release(Breath *b) {
    if (breath_boost(b, 0.5f) < RELEASE_BOOST) {
        b->has_trigger = true;
        b->since_trigger = 0.0f;
        b->boost_level = RELEASE_BOOST;
        breath_begin_arrival(b);
    }
}

float breath_rate_hz(float freq) {
    return clampf(fmaxf(freq, 0.0f) / powi_f(PHI, BREATH_RATE_RUNGS), BREATH_RATE_MIN_HZ,
                  BREATH_RATE_MAX_HZ);
}

Field breath_tick(Breath *b, float freq, float field, float floor_, float curve) {
    field = clampf(field, 0.0f, 1.0f);
    floor_ = clampf(floor_, 0.0f, 1.0f);
    float f = breath_rate_hz(freq);
    float sig[5];
    signature(b->mode, sig);

    float sum = 0.0f;
    for (int i = 0; i < 5; i++) {
        b->phase[i] += f * sig[i] / b->sample_rate;
        b->phase[i] = fract_pos(b->phase[i]);
        sum += lfo_sin(b->phase[i]);
    }
    float swing = (sum / 5.0f + 1.0f) * 0.5f;

    float boost = breath_boost(b, curve);
    float depth = field * (REST_FRACTION + (1.0f - REST_FRACTION) * boost);
    if (b->has_trigger) {
        b->since_trigger += 1.0f / b->sample_rate;
    }

    float amount = depth * swing;
    if (b->declick_left > 0.0f) {
        float travelled = 1.0f - clampf(b->declick_left / BREATH_DECLICK_S, 0.0f, 1.0f);
        float w = 0.5f - 0.5f * cosf(travelled * PI_F);
        b->declick_left = fmaxf(b->declick_left - 1.0f / b->sample_rate, 0.0f);
        amount = b->declick_from + (amount - b->declick_from) * w;
    }
    b->last_amount = amount;
    /* the wave opens the room's damp filter. The note's level and pitch stay. */
    Field out;
    out.amount = amount;
    out.gain = floor_;
    out.pitch = 1.0f;
    return out;
}

void breath_reset(Breath *b) {
    breath_init(b, b->sample_rate, b->mode);
}
