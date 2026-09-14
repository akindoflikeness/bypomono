#include <string.h>

#include "../src/dsp/dsp.h"
#include "test.h"

static void feedback_moves_the_tap_and_leaves_the_routing_alone(void) {
    for (uint8_t raw = 0; raw < (uint8_t)ALGORITHM_SPACE; raw++) {
        AlgorithmId id = raw;
        AlgorithmId plain = id;
        for (int k = 0; k < NUM_NODES; k++) {
            if (algorithm_node(id, k) == COMBINE_FEEDBACK) {
                plain = algorithm_with_node(plain, k, COMBINE_SERIES);
            }
        }
        Compiled a = compile(id);
        Compiled b = compile(plain);
        CHECK(memcmp(a.modulators, b.modulators, sizeof a.modulators) == 0,
              "id %u rerouted", (unsigned)raw);
        CHECK(a.carriers == b.carriers, "id %u changed carriers", (unsigned)raw);
        CHECK(memcmp(a.depth, b.depth, sizeof a.depth) == 0,
              "id %u changed depths", (unsigned)raw);
    }
}

static void a_feedback_node_never_buries_the_tap_deeper(void) {
    for (uint8_t raw = 0; raw < (uint8_t)ALGORITHM_SPACE; raw++) {
        AlgorithmId id = raw;
        Compiled c = compile(id);
        bool has_f = false;
        for (int k = 0; k < NUM_NODES; k++) {
            if (algorithm_node(id, k) == COMBINE_FEEDBACK) {
                has_f = true;
            }
        }
        int deepest = c.eval_order[0];
        if (has_f) {
            CHECK(c.depth[c.feedback_op] <= c.depth[deepest],
                  "id %u: F put the tap at depth %u against %u", (unsigned)raw,
                  (unsigned)c.depth[c.feedback_op], (unsigned)c.depth[deepest]);
        } else {
            CHECK(c.feedback_op == deepest, "id %u: tap moved with no F",
                  (unsigned)raw);
        }
    }
}

static void the_roster_is_a_single_click_path(void) {
    for (int i = 0; i + 1 < 8; i++) {
        char ga[NUM_NODES + 1], gb[NUM_NODES + 1];
        algorithm_to_glyphs(ALGORITHMS[i], ga);
        algorithm_to_glyphs(ALGORITHMS[i + 1], gb);
        CHECK(algorithm_distance(ALGORITHMS[i], ALGORITHMS[i + 1]) == 1,
              "%s to %s is not one click", ga, gb);
    }
    for (int i = 0; i + 1 < 8; i++) {
        int k = -1;
        for (int kk = 0; kk < NUM_NODES; kk++) {
            if (algorithm_node(ALGORITHMS[i], kk) !=
                algorithm_node(ALGORITHMS[i + 1], kk)) {
                k = kk;
                break;
            }
        }
        CHECK(k >= 0, "checked above");
        char gb[NUM_NODES + 1];
        algorithm_to_glyphs(ALGORITHMS[i + 1], gb);
        CHECK(combine_next(algorithm_node(ALGORITHMS[i], k)) ==
                  algorithm_node(ALGORITHMS[i + 1], k),
              "step to %s turns node %d backwards", gb, k);
    }

    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            CHECK(ALGORITHMS[i] != ALGORITHMS[j], "the roster repeats itself");
        }
    }
}

static void the_roster_matches_its_documented_table(void) {
    static const struct {
        const char *glyphs;
        int carriers, max_depth, fb_op, fb_depth;
    } table[8] = {
        {"SSSS", 1, 4, 1, 4}, {"SSSP", 2, 4, 1, 4}, {"PSSP", 3, 3, 1, 3},
        {"PPSP", 4, 2, 1, 2}, {"PPPP", 5, 1, 1, 1}, {"PPPF", 4, 2, 5, 1},
        {"PPFF", 3, 2, 2, 1}, {"PFFF", 2, 3, 3, 1},
    };
    for (int i = 0; i < 8; i++) {
        AlgorithmId id = ALGORITHMS[i];
        Compiled c = compile(id);
        char glyphs[NUM_NODES + 1];
        algorithm_to_glyphs(id, glyphs);
        CHECK(strcmp(glyphs, table[i].glyphs) == 0, "algorithm %d", i + 1);
        CHECK((int)c.carrier_count == table[i].carriers, "algorithm %d", i + 1);
        int max_depth = 0;
        for (int op = 0; op < NUM_OPS; op++) {
            if (c.depth[op] > max_depth) {
                max_depth = c.depth[op];
            }
        }
        CHECK(max_depth == table[i].max_depth, "algorithm %d", i + 1);
        CHECK(c.feedback_op + 1 == table[i].fb_op, "algorithm %d", i + 1);
        CHECK((int)c.depth[c.feedback_op] == table[i].fb_depth, "algorithm %d",
              i + 1);
    }
}

static void the_feedback_algorithms_tap_a_carrier(void) {
    for (int i = 5; i < 8; i++) {
        Compiled c = compile(ALGORITHMS[i]);
        CHECK(c.depth[c.feedback_op] == 1, "algorithm %d taps depth %u", i + 1,
              (unsigned)c.depth[c.feedback_op]);
    }
    for (int i = 0; i < 5; i++) {
        Compiled c = compile(ALGORITHMS[i]);
        CHECK(c.feedback_op == c.eval_order[0], "algorithm %d tap", i + 1);
    }
}

static void the_old_bitmask_still_means_what_it_meant(void) {
    static const struct {
        uint8_t bits;
        int expect;
    } cases[5] = {
        {0u, 0}, {0x8, 1}, {0x9, 2}, {0xB, 3}, {0xF, 4},
    };
    for (int i = 0; i < 5; i++) {
        CHECK(algorithm_from_legacy_bits(cases[i].bits) ==
                  ALGORITHMS[cases[i].expect],
              "legacy %u migrated wrong", (unsigned)cases[i].bits);
    }
    for (uint8_t bits = 0; bits < 16; bits++) {
        AlgorithmId id = algorithm_from_legacy_bits(bits);
        bool all_non_f = true;
        for (int k = 0; k < NUM_NODES; k++) {
            if (algorithm_node(id, k) == COMBINE_FEEDBACK) {
                all_non_f = false;
            }
        }
        CHECK(all_non_f, "legacy %u produced feedback", (unsigned)bits);
    }
}

static void glyphs_round_trip(void) {
    for (uint8_t raw = 0; raw < (uint8_t)ALGORITHM_SPACE; raw++) {
        AlgorithmId id = raw;
        char text[NUM_NODES + 1];
        algorithm_to_glyphs(id, text);
        AlgorithmId back;
        bool ok = algorithm_from_glyphs(text, &back);
        CHECK(ok && back == id, "%s", text);
    }
    char g7[NUM_NODES + 1];
    algorithm_to_glyphs(ALGORITHMS[7], g7);
    CHECK(strcmp(g7, "PFFF") == 0, "ALGORITHMS[7] is %s", g7);
    AlgorithmId out;
    CHECK(!algorithm_from_glyphs("nope", &out), "nope parsed");
    CHECK(!algorithm_from_glyphs("SSS", &out), "SSS parsed");
}

static void full_space_compiles_to_valid_routings(void) {
    for (uint8_t raw = 0; raw < (uint8_t)ALGORITHM_SPACE; raw++) {
        Compiled c = compile(raw);

        for (int op = 0; op < NUM_OPS; op++) {
            CHECK(c.depth[op] >= 1, "id %u: unreachable op", (unsigned)raw);
        }

        CHECK(c.carrier_count >= 1, "id %u: no carriers", (unsigned)raw);
        for (int op = 0; op < NUM_OPS; op++) {
            CHECK(((c.carriers >> op & 1) == 1) == (c.depth[op] == 1), "id %u",
                  (unsigned)raw);
        }

        int pos[NUM_OPS];
        for (int i = 0; i < NUM_OPS; i++) {
            pos[c.eval_order[i]] = i;
        }
        for (int target = 0; target < NUM_OPS; target++) {
            for (int m = 0; m < NUM_OPS; m++) {
                if ((c.modulators[target] >> m & 1) == 1) {
                    CHECK(pos[m] < pos[target], "id %u: bad eval order",
                          (unsigned)raw);
                    CHECK(c.depth[m] == c.depth[target] + 1, "id %u",
                          (unsigned)raw);
                }
            }
        }

        for (int m = 0; m < NUM_OPS; m++) {
            int fan_out = 0;
            for (int t = 0; t < NUM_OPS; t++) {
                if ((c.modulators[t] >> m & 1) == 1) {
                    fan_out++;
                }
            }
            CHECK(fan_out <= 1, "id %u: op %d modulates %d targets",
                  (unsigned)raw, m, fan_out);
        }
    }
}

static void the_unfolding_half_is_unchanged(void) {
    static const int max_depths[5] = {4, 4, 3, 2, 1};
    for (int i = 0; i < 5; i++) {
        Compiled c = compile(ALGORITHMS[i]);
        CHECK((int)c.carrier_count == i + 1, "algorithm %d", i + 1);
        int max_depth = 0;
        for (int op = 0; op < NUM_OPS; op++) {
            if (c.depth[op] > max_depth) {
                max_depth = c.depth[op];
            }
        }
        CHECK(max_depth == max_depths[i], "algorithm %d", i + 1);
        CHECK(c.feedback_op == 0, "algorithm %d", i + 1);
    }
}

static void the_folding_half_closes_the_tree(void) {
    for (int i = 5; i < 8; i++) {
        Compiled c = compile(ALGORITHMS[i]);
        CHECK((int)c.carrier_count == 9 - i, "algorithm %d has %u carriers",
              i + 1, (unsigned)c.carrier_count);
    }
}

static void node_trit_round_trip(void) {
    AlgorithmId id;
    CHECK(algorithm_from_glyphs("PSSP", &id), "PSSP parses");
    CHECK(algorithm_node(id, 0) == COMBINE_PARALLEL, "node 0");
    CHECK(algorithm_node(id, 1) == COMBINE_SERIES, "node 1");
    CHECK(algorithm_node(id, 3) == COMBINE_PARALLEL, "node 3");
    AlgorithmId ppsp, sssp;
    CHECK(algorithm_from_glyphs("PPSP", &ppsp), "PPSP parses");
    CHECK(algorithm_from_glyphs("SSSP", &sssp), "SSSP parses");
    CHECK(algorithm_with_node(id, 1, COMBINE_PARALLEL) == ppsp, "with_node 1 P");
    CHECK(algorithm_with_node(id, 0, COMBINE_SERIES) == sssp, "with_node 0 S");
    CHECK(algorithm_node(algorithm_with_node(id, 2, COMBINE_FEEDBACK), 2) ==
              COMBINE_FEEDBACK,
          "with_node 2 F");
    CHECK(combine_next(COMBINE_SERIES) == COMBINE_PARALLEL, "S next");
    CHECK(combine_next(COMBINE_PARALLEL) == COMBINE_FEEDBACK, "P next");
    CHECK(combine_next(COMBINE_FEEDBACK) == COMBINE_SERIES, "F next");
    static const Combine choices[3] = {COMBINE_SERIES, COMBINE_PARALLEL,
                                       COMBINE_FEEDBACK};
    for (int k = 0; k < NUM_NODES; k++) {
        for (int ci = 0; ci < 3; ci++) {
            Combine c = choices[ci];
            AlgorithmId once = algorithm_with_node(id, k, c);
            CHECK(algorithm_with_node(once, k, c) == once,
                  "with_node idempotent k=%d", k);
            CHECK(once < (uint8_t)ALGORITHM_SPACE, "with_node in range k=%d",
                  k);
        }
    }
}

void test_algorithm(void) {
    feedback_moves_the_tap_and_leaves_the_routing_alone();
    a_feedback_node_never_buries_the_tap_deeper();
    the_roster_is_a_single_click_path();
    the_roster_matches_its_documented_table();
    the_feedback_algorithms_tap_a_carrier();
    the_old_bitmask_still_means_what_it_meant();
    glyphs_round_trip();
    full_space_compiles_to_valid_routings();
    the_unfolding_half_is_unchanged();
    the_folding_half_closes_the_tree();
    node_trit_round_trip();
}
