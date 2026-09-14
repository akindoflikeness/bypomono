#include "../src/dsp/dsp.h"
#include "test.h"

static void sanitize_rejects_an_unknown_algorithm(void) {
    Session s = session_default();
    s.patch.algorithm = (AlgorithmId)14;
    Session out = session_sanitize(s);
    CHECK(out.patch.algorithm == ALGORITHMS[0], "algorithm %d survived sanitize",
          (int)out.patch.algorithm);
}

static void sanitize_clamps_the_documented_ranges(void) {
    Session s = session_default();
    s.patch.index = 4.0f;
    s.verb.decay = 99.0f;
    s.drone_hz = 1.0f;
    Session out = session_sanitize(s);
    CHECK(out.patch.index == 1.0f, "index %g", out.patch.index);
    CHECK(out.verb.decay == 8.0f, "decay %g", out.verb.decay);
    CHECK(out.drone_hz == 27.5f, "drone_hz %g", out.drone_hz);
}

static void sanitize_pulls_chandas_back_into_range(void) {
    Session s = session_default();
    s.chandas.division = 9999;
    s.chandas.size = 0.0f;
    s.tempo_bpm = 0.0f;
    Session out = session_sanitize(s);
    CHECK(out.chandas.division == (size_t)(CHANDAS_DIVISIONS_LEN - 1), "division %zu",
          out.chandas.division);
    CHECK(out.chandas.size == CHANDAS_MIN_SIZE, "size %g", out.chandas.size);
    CHECK(out.tempo_bpm == CHANDAS_MIN_BPM, "tempo %g", out.tempo_bpm);
    CHECK(out.tempo_bpm > 0.0f, "tempo %g not positive", out.tempo_bpm);
}

void test_session(void) {
    sanitize_rejects_an_unknown_algorithm();
    sanitize_clamps_the_documented_ranges();
    sanitize_pulls_chandas_back_into_range();
}
