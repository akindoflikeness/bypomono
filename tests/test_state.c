#include "../src/dsp/dsp.h"
#include "test.h"

static State base_state(void) {
    return state_new(patch_init(ALGORITHMS[0], RATIO_GOLDEN));
}

static void envelope_settings_are_knobs_not_a_crossfade(void) {
    State a = base_state();
    State b = base_state();
    b.adsr.decay_s = 0.4f;
    b.adsr.sustain = 0.2f;
    CHECK(!state_is_structural_change(&a, &b), "envelope settings crossed");
}

static void mut_ratio_mode(State *s) { s->patch.ratio_mode = RATIO_PLASTIC; }
static void mut_algorithm(State *s) { s->patch.algorithm = ALGORITHMS[3]; }
static void mut_op_ratio(State *s) { s->patch.ops[2].ratio *= 1.5f; }

static void mut_index(State *s) { s->patch.index = 0.9f; }
static void mut_feedback(State *s) { s->patch.feedback = 0.1f; }
static void mut_field(State *s) { s->patch.field = 0.3f; }
static void mut_curve(State *s) { s->patch.curve = 0.2f; }
static void mut_master_level(State *s) { s->patch.master_level = 0.4f; }
static void mut_glide(State *s) { s->patch.glide_seconds = 1.0f; }
static void mut_rip(State *s) { s->patch.rip = 0.7f; }
static void mut_op_level(State *s) { s->patch.ops[1].level = 0.3f; }
static void mut_op_detune(State *s) { s->patch.ops[1].detune_cents = 12.0f; }

static void the_shape_of_the_patch_is_structural_and_the_rest_is_not(void) {
    State a = base_state();

    void (*structural[])(State *) = {mut_ratio_mode, mut_algorithm};
    for (size_t i = 0; i < sizeof structural / sizeof structural[0]; i++) {
        State b = base_state();
        structural[i](&b);
        CHECK(state_is_structural_change(&a, &b), "should cross: structural case %zu", i);
    }

    void (*knobs[])(State *) = {mut_index, mut_feedback, mut_field,
                                mut_curve, mut_master_level, mut_glide,
                                mut_rip, mut_op_level, mut_op_detune, mut_op_ratio};
    for (size_t i = 0; i < sizeof knobs / sizeof knobs[0]; i++) {
        State b = base_state();
        knobs[i](&b);
        CHECK(!state_is_structural_change(&a, &b), "should not cross: knob case %zu", i);
    }
}

void test_state(void) {
    envelope_settings_are_knobs_not_a_crossfade();
    the_shape_of_the_patch_is_structural_and_the_rest_is_not();
}
