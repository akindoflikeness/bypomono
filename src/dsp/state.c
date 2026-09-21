#include "dsp.h"

State state_new(Patch patch) {
    State s;
    s.patch = patch;
    s.adsr = env_params_default();
    return s;
}

/* the envelope never needs a crossfade: a voice's envelope takes new
   settings mid-note without stepping */
bool state_is_structural_change(const State *a, const State *b) {
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
