#include "dsp.h"

Chain chain_default(void) {
    Chain c;
    c.amp.kind = AMP_DRONE;
    c.amp.env.attack_s = 0.0f;
    c.amp.env.decay_s = 0.0f;
    c.amp.env.release_s = 0.0f;
    c.amp.env.curve = 0.0f;
    return c;
}

bool chain_is_structural_change(const Chain *a, const Chain *b) {
    return a->amp.kind != b->amp.kind;
}

State state_new(Patch patch) {
    State s;
    s.patch = patch;
    s.chain = chain_default();
    return s;
}

bool state_is_structural_change(const State *a, const State *b) {
    if (chain_is_structural_change(&a->chain, &b->chain)) {
        return true;
    }
    if (a->patch.algorithm != b->patch.algorithm) {
        return true;
    }
    if (a->patch.ratio_mode != b->patch.ratio_mode) {
        return true;
    }
    for (int i = 0; i < NUM_OPS; i++) {
        if (a->patch.ops[i].ratio != b->patch.ops[i].ratio) {
            return true;
        }
    }
    return false;
}
