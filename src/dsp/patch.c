#include "dsp.h"

float midi_to_hz(uint8_t note) {
    return 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
}

float index_response(uint8_t depth, float index) {
    return powf(clampf(index, 0.0f, 1.0f), powi_f(PHI, (int)depth - 2));
}

float master_gain(float position) {
    return powf(clampf(position, 0.0f, 1.0f), PHI) * powf(10.0f, HEADROOM_DB / 20.0f);
}

float ratio_mode_ratio(RatioMode mode, int op) {
    static const float harmonic[NUM_OPS] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    static const float fibonacci[NUM_OPS] = {1.0f, 1.0f, 2.0f, 3.0f, 5.0f};
    switch (mode) {
    case RATIO_HARMONIC:
        return harmonic[op];
    case RATIO_FIBONACCI:
        return fibonacci[op];
    case RATIO_GOLDEN:
        return powi_f(PHI, op);
    case RATIO_GOLDEN_MIRROR:
        return powi_f(PHI, op - 2);
    case RATIO_PLASTIC:
        return powi_f(PLASTIC, op);
    default:
        return 1.0f;
    }
}

Patch patch_init(AlgorithmId algorithm, RatioMode ratio_mode) {
    Compiled compiled = compile(algorithm);
    Patch p;
    p.algorithm = algorithm;
    p.ratio_mode = ratio_mode;
    for (int i = 0; i < NUM_OPS; i++) {
        p.ops[i].enabled = true;
        p.ops[i].ratio = ratio_mode_ratio(ratio_mode, i);
        p.ops[i].detune_cents = 0.0f;
        p.ops[i].level = 1.0f;
        if (compiled.depth[i] > 1) {
            p.ops[i].level = powi_f(PHI, -((int)compiled.depth[i] - 1));
        }
    }
    p.feedback = 0.5f;
    p.index = 1.0f;
    p.rip = 0.0f;
    p.master_level = 0.8f;
    p.glide_seconds = 0.05f;
    p.field = 0.0f;
    p.curve = 0.5f;
    return p;
}
