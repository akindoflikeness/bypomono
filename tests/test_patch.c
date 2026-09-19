#include "../src/dsp/dsp.h"
#include "test.h"

static void golden_ratios_are_phi_powers(void) {
    for (int op = 0; op < NUM_OPS; op++) {
        float expected = powi_f(PHI, op);
        CHECK(fabsf(ratio_mode_ratio(RATIO_GOLDEN, op) - expected) < 1e-4f,
              "golden ratio op %d: %g vs %g", op, ratio_mode_ratio(RATIO_GOLDEN, op), expected);
    }
    CHECK(fabsf(PHI * PHI - (PHI + 1.0f)) < 1e-4f, "phi*phi != phi+1");
}

static void palettes_belong_to_operator_numbers(void) {
    for (int mode = 0; mode < RATIO_MODE_COUNT; mode++) {
        for (int alg = 0; alg < 8; alg++) {
            Patch patch = patch_init(ALGORITHMS[alg], (RatioMode)mode);
            for (int op = 0; op < NUM_OPS; op++) {
                float want = ratio_mode_ratio((RatioMode)mode, op);
                CHECK_NEAR(patch.ops[op].ratio, want, 1e-6f,
                           "mode %d algorithm %d op %d ratio %g vs %g", mode,
                           alg + 1, op + 1, (double)patch.ops[op].ratio,
                           (double)want);
            }
        }
    }
}

static void loading_a_palette_only_replaces_ratios(void) {
    Patch patch = patch_init(ALGORITHMS[4], RATIO_HARMONIC);
    for (int op = 0; op < NUM_OPS; op++) {
        patch.ops[op].enabled = (op & 1) == 0;
        patch.ops[op].detune_cents = (float)(op - 2) * 17.0f;
        patch.ops[op].level = 0.1f * (float)(op + 1);
    }
    patch_apply_ratio_mode(&patch, RATIO_PLASTIC);
    CHECK(patch.algorithm == ALGORITHMS[4], "palette changed the algorithm");
    CHECK(patch.ratio_mode == RATIO_PLASTIC, "palette mode %d",
          (int)patch.ratio_mode);
    for (int op = 0; op < NUM_OPS; op++) {
        CHECK_NEAR(patch.ops[op].ratio,
                   ratio_mode_ratio(RATIO_PLASTIC, op), 1e-6f,
                   "palette op %d ratio", op + 1);
        CHECK(patch.ops[op].enabled == ((op & 1) == 0),
              "palette changed op %d enable", op + 1);
        CHECK_NEAR(patch.ops[op].detune_cents, (float)(op - 2) * 17.0f, 1e-6f,
                   "palette changed op %d detune", op + 1);
        CHECK_NEAR(patch.ops[op].level, 0.1f * (float)(op + 1), 1e-6f,
                   "palette changed op %d level", op + 1);
    }
}

static void init_levels_follow_golden_decay(void) {
    Patch patch = patch_init(ALGORITHMS[0], RATIO_HARMONIC);
    Compiled compiled = compile(ALGORITHMS[0]);
    for (int i = 0; i < NUM_OPS; i++) {
        if ((compiled.carriers >> i & 1) == 1) {
            CHECK(patch.ops[i].level == 1.0f, "carrier %d level %g", i, patch.ops[i].level);
        } else {
            float expected = powi_f(PHI, -((int)compiled.depth[i] - 1));
            CHECK(fabsf(patch.ops[i].level - expected) < 1e-6f,
                  "op %d level %g vs %g", i, patch.ops[i].level, expected);
            CHECK(patch.ops[i].level < 1.0f, "op %d level %g not < 1", i, patch.ops[i].level);
        }
    }
}

static void plastic_number_satisfies_its_cubic(void) {
    CHECK(fabsf(powi_f(PLASTIC, 3) - (PLASTIC + 1.0f)) < 1e-4f, "plastic cubic");
    CHECK(fabsf(ratio_mode_ratio(RATIO_PLASTIC, 4) - powi_f(PLASTIC, 4)) < 1e-4f,
          "plastic ratio op 4");
}

static void golden_mirror_is_symmetric_around_the_fundamental(void) {
    CHECK(fabsf(ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 2) - 1.0f) < 1e-6f,
          "center op is the fundamental");
    CHECK(fabsf(ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 0) * ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 4) - 1.0f) < 1e-4f,
          "0*4 mirror");
    CHECK(fabsf(ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 1) * ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 3) - 1.0f) < 1e-4f,
          "1*3 mirror");
    CHECK(ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 0) < 1.0f && ratio_mode_ratio(RATIO_GOLDEN_MIRROR, 1) < 1.0f,
          "two partials below base");
}

static void index_response_curves_are_ordered_by_depth(void) {
    for (uint8_t depth = 1; depth <= 4; depth++) {
        CHECK(index_response(depth, 0.0f) == 0.0f, "depth %d at zero", depth);
        CHECK(fabsf(index_response(depth, 1.0f) - 1.0f) < 1e-6f, "depth %d at one", depth);
    }
    CHECK(fabsf(index_response(2, 0.3f) - 0.3f) < 1e-6f, "depth 2 is linear");
    float x = 0.5f;
    CHECK(index_response(1, x) > index_response(2, x), "1 > 2 at %g", x);
    CHECK(index_response(2, x) > index_response(3, x), "2 > 3 at %g", x);
    CHECK(index_response(3, x) > index_response(4, x), "3 > 4 at %g", x);
    CHECK(index_response(4, 0.8f) > index_response(4, 0.4f), "monotone in index");
}

static void mode_roster_count_is_fibonacci(void) {
    CHECK(RATIO_MODE_COUNT == 5, "mode count %d", (int)RATIO_MODE_COUNT);
    CHECK(session_default().patch.ratio_mode == RATIO_GOLDEN, "default mode is Golden");
}

static void midi_reference_pitches(void) {
    CHECK(fabsf(midi_to_hz(69) - 440.0f) < 1e-3f, "A4 %g", midi_to_hz(69));
    CHECK(fabsf(midi_to_hz(45) - 110.0f) < 1e-3f, "A2 %g", midi_to_hz(45));
}

void test_patch(void) {
    golden_ratios_are_phi_powers();
    palettes_belong_to_operator_numbers();
    loading_a_palette_only_replaces_ratios();
    init_levels_follow_golden_decay();
    plastic_number_satisfies_its_cubic();
    golden_mirror_is_symmetric_around_the_fundamental();
    index_response_curves_are_ordered_by_depth();
    mode_roster_count_is_fibonacci();
    midi_reference_pitches();
}
