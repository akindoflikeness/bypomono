#include "dsp.h"
#include <math.h>

const float melody_fib_hz[10] = {
    34.0f, 55.0f, 89.0f, 144.0f, 233.0f, 377.0f, 610.0f, 987.0f, 1597.0f, 2584.0f,
};

static const uint8_t SCALE_PHRYGIAN_IV[] = {0, 1, 3, 5, 7, 8, 10};
static const uint8_t SCALE_PHRYGIAN_DOMINANT_IV[] = {0, 1, 4, 5, 7, 8, 10};
static const uint8_t SCALE_NATURAL_MINOR_IV[] = {0, 2, 3, 5, 7, 8, 10};
static const uint8_t SCALE_HARMONIC_MINOR_IV[] = {0, 2, 3, 5, 7, 8, 11};
static const uint8_t SCALE_NEAPOLITAN_MINOR_IV[] = {0, 1, 3, 5, 7, 8, 11};
static const uint8_t SCALE_BYZANTINE_IV[] = {0, 1, 4, 5, 7, 8, 11};
static const uint8_t SCALE_FIBONACCI_IV[] = {0, 1, 2, 3, 5, 8};
static const uint8_t SCALE_PHYLLOTAXIS_IV[] = {0, 1, 3, 6, 7, 8, 10};

const uint8_t *scale_intervals(Scale s, size_t *count) {
    switch (s) {
    case SCALE_PHRYGIAN:
        *count = sizeof SCALE_PHRYGIAN_IV;
        return SCALE_PHRYGIAN_IV;
    case SCALE_PHRYGIAN_DOMINANT:
        *count = sizeof SCALE_PHRYGIAN_DOMINANT_IV;
        return SCALE_PHRYGIAN_DOMINANT_IV;
    case SCALE_NATURAL_MINOR:
        *count = sizeof SCALE_NATURAL_MINOR_IV;
        return SCALE_NATURAL_MINOR_IV;
    case SCALE_HARMONIC_MINOR:
        *count = sizeof SCALE_HARMONIC_MINOR_IV;
        return SCALE_HARMONIC_MINOR_IV;
    case SCALE_NEAPOLITAN_MINOR:
        *count = sizeof SCALE_NEAPOLITAN_MINOR_IV;
        return SCALE_NEAPOLITAN_MINOR_IV;
    case SCALE_BYZANTINE:
        *count = sizeof SCALE_BYZANTINE_IV;
        return SCALE_BYZANTINE_IV;
    case SCALE_FIBONACCI:
        *count = sizeof SCALE_FIBONACCI_IV;
        return SCALE_FIBONACCI_IV;
    case SCALE_PHYLLOTAXIS:
    default:
        *count = sizeof SCALE_PHYLLOTAXIS_IV;
        return SCALE_PHYLLOTAXIS_IV;
    }
}

const char *scale_name(Scale s) {
    switch (s) {
    case SCALE_PHRYGIAN:
        return "phrygian";
    case SCALE_PHRYGIAN_DOMINANT:
        return "phrygdom";
    case SCALE_NATURAL_MINOR:
        return "nat minor";
    case SCALE_HARMONIC_MINOR:
        return "harm minor";
    case SCALE_NEAPOLITAN_MINOR:
        return "neapolitan";
    case SCALE_BYZANTINE:
        return "byzantine";
    case SCALE_FIBONACCI:
        return "fibonacci";
    case SCALE_PHYLLOTAXIS:
    default:
        return "phyllotaxis";
    }
}

const char *tuning_name(Tuning t) {
    switch (t) {
    case TUNING_SCALE:
        return "scale";
    case TUNING_FIBONACCI_HZ:
        return "fib hz";
    case TUNING_GOLDEN_POWERS:
        return "phi powers";
    case TUNING_PLASTIC_POWERS:
        return "rho powers";
    case TUNING_GOLDEN_WALK:
    default:
        return "phi walk";
    }
}

Quantization tuning_quantization(Tuning t) {
    Quantization q;
    switch (t) {
    case TUNING_SCALE:
        q.kind = QUANT_TWELVE_TET;
        q.symbol = "";
        break;
    case TUNING_FIBONACCI_HZ:
        q.kind = QUANT_GRID;
        q.symbol = "F";
        break;
    case TUNING_GOLDEN_POWERS:
        q.kind = QUANT_GRID;
        q.symbol = "φ";
        break;
    case TUNING_PLASTIC_POWERS:
        q.kind = QUANT_GRID;
        q.symbol = "ρ";
        break;
    case TUNING_GOLDEN_WALK:
    default:
        q.kind = QUANT_FREE;
        q.symbol = "φ";
        break;
    }
    return q;
}

bool quantization_is_quantized(Quantization q) {
    return q.kind != QUANT_FREE;
}

MelodyParams melody_params_default(void) {
    MelodyParams p;
    p.enabled = false;
    p.tuning = TUNING_SCALE;
    p.scale = SCALE_PHRYGIAN;
    p.root_midi = 45;
    p.range_degrees = 8;
    p.rate_hz = PHI;
    p.source = HOLD_GOLDEN_WEYL;
    return p;
}

static size_t melody_period(const Melody *m) {
    size_t p = (size_t)(m->sample_rate / fmaxf(m->params.rate_hz, 0.01f));
    return p < 1 ? 1 : p;
}

void melody_init(Melody *m, float sample_rate, MelodyParams params) {
    m->params = params;
    m->sample_rate = sample_rate;
    m->countdown = 0;
    m->weyl = 0.0f;
    m->rng = 0x9E3779B9u;
    m->walk_hz = midi_to_hz(params.root_midi);
    m->countdown = melody_period(m);
}

void melody_set_params(Melody *m, MelodyParams params) {
    bool was_disabled = !m->params.enabled;
    if (params.tuning != m->params.tuning || params.root_midi != m->params.root_midi) {
        m->walk_hz = midi_to_hz(params.root_midi);
    }
    m->params = params;
    size_t period = melody_period(m);
    m->countdown = m->countdown < period ? m->countdown : period;
    if (was_disabled && params.enabled) {
        m->countdown = 0;
    }
}

bool melody_samples_until_fire(const Melody *m, size_t *out) {
    if (!m->params.enabled) {
        return false;
    }
    *out = m->countdown;
    return true;
}

void melody_advance(Melody *m, size_t samples) {
    if (m->params.enabled) {
        m->countdown = samples >= m->countdown ? 0 : m->countdown - samples;
    }
}

static float melody_draw(Melody *m) {
    switch (m->params.source) {
    case HOLD_GOLDEN_WEYL:
        m->weyl = fract_pos(m->weyl + (PHI - 1.0f));
        return m->weyl;
    case HOLD_XORSHIFT:
    default:
        m->rng ^= m->rng << 13;
        m->rng ^= m->rng >> 17;
        m->rng ^= m->rng << 5;
        return (float)(m->rng >> 8) / (float)(1u << 24);
    }
}

float melody_fire(Melody *m) {
    m->countdown = melody_period(m);
    float x = melody_draw(m);
    size_t range = m->params.range_degrees < 1 ? 1 : (size_t)m->params.range_degrees;
    size_t degree = (size_t)(x * (float)range);
    degree = degree < range - 1 ? degree : range - 1;
    float root_hz = midi_to_hz(m->params.root_midi);
    switch (m->params.tuning) {
    case TUNING_SCALE: {
        size_t count;
        const uint8_t *intervals = scale_intervals(m->params.scale, &count);
        size_t midi = (size_t)m->params.root_midi + 12 * (degree / count)
                      + (size_t)intervals[degree % count];
        return midi_to_hz((uint8_t)(midi < 127 ? midi : 127));
    }
    case TUNING_FIBONACCI_HZ:
        return melody_fib_hz[degree < 9 ? degree : 9];
    case TUNING_GOLDEN_POWERS:
        return fminf(root_hz * powi_f(PHI, (int)degree), MELODY_PITCH_CAP_HZ);
    case TUNING_PLASTIC_POWERS:
        return fminf(root_hz * powi_f(PLASTIC, (int)degree), MELODY_PITCH_CAP_HZ);
    case TUNING_GOLDEN_WALK:
    default: {
        float degs = (float)(m->params.range_degrees < 1 ? 1 : m->params.range_degrees);
        float hi = root_hz * exp2f(degs / 12.0f);
        float hz = clampf(m->walk_hz, root_hz, hi);
        hz = x >= 0.5f ? hz * PHI : hz / PHI;
        if (hz > hi) {
            hz = hi * hi / hz;
        }
        if (hz < root_hz) {
            hz = root_hz * root_hz / hz;
        }
        hz = clampf(hz, root_hz, hi);
        m->walk_hz = hz;
        return hz;
    }
    }
}
