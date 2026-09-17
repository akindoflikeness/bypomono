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

/* Which table entry each op plays: every carrier takes the fundamental (the
   first entry that is 1.0), and the modulators take the remaining entries in
   table order, ranked by depth and then by op index, so a note sounds at its
   own pitch in every algorithm. */
static void ratio_slots(const Compiled *compiled, RatioMode mode, int slot[NUM_OPS]) {
    int fundamental = 0;
    for (int k = 0; k < NUM_OPS; k++) {
        if (ratio_mode_ratio(mode, k) == 1.0f) {
            fundamental = k;
            break;
        }
    }
    int order[NUM_OPS];
    int n = 0;
    for (int op = 0; op < NUM_OPS; op++) {
        if ((compiled->carriers >> op & 1) == 1) {
            slot[op] = fundamental;
            continue;
        }
        int j = n;
        while (j > 0 &&
               (compiled->depth[order[j - 1]] > compiled->depth[op] ||
                (compiled->depth[order[j - 1]] == compiled->depth[op] && order[j - 1] > op))) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = op;
        n++;
    }
    int next = 0;
    for (int i = 0; i < n; i++) {
        if (next == fundamental) next++;
        slot[order[i]] = next++;
    }
}

Patch patch_init(AlgorithmId algorithm, RatioMode ratio_mode) {
    Compiled compiled = compile(algorithm);
    int slot[NUM_OPS];
    ratio_slots(&compiled, ratio_mode, slot);
    Patch p;
    p.algorithm = algorithm;
    p.ratio_mode = ratio_mode;
    for (int i = 0; i < NUM_OPS; i++) {
        p.ops[i].enabled = true;
        p.ops[i].ratio = ratio_mode_ratio(ratio_mode, slot[i]);
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
    p.voices = 1;
    p.unison = 1;
    p.unison_detune = UNISON_DETUNE_DEFAULT;
    return p;
}
