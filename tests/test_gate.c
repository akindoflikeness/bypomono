#include "../src/dsp/dsp.h"
#include "test.h"

#define SR 48000.0f

static void born_closed_is_exact_zero_forever(void) {
    EngageGate g;
    engage_gate_init(&g, SR, false);
    for (size_t i = 0; i < (size_t)SR; i++) {
        CHECK(engage_gate_next(&g, false) == 0.0f, "gain %g at sample %zu", g.gain, i);
    }
}

static void born_open_is_exact_unity(void) {
    EngageGate g;
    engage_gate_init(&g, SR, true);
    CHECK(engage_gate_next(&g, true) == 1.0f, "gain %g", g.gain);
}

static void travel_never_steps(void) {
    EngageGate g;
    engage_gate_init(&g, SR, false);
    float k = 1.0f - expf(-1.0f / (GATE_GLIDE_S * SR));
    float prev = 0.0f;
    for (size_t i = 0; i < (size_t)SR; i++) {
        bool open = (i / 4800) % 2 == 0;
        float x = engage_gate_next(&g, open);
        CHECK(x >= 0.0f && x <= 1.0f, "gain %g out of range", x);
        CHECK(fabsf(x - prev) <= k + 1e-7f, "gate stepped %g in one sample", fabsf(x - prev));
        prev = x;
    }
}

static void closing_reaches_true_zero_within_a_quarter_second(void) {
    EngageGate g;
    engage_gate_init(&g, SR, true);
    bool found = false;
    size_t at = 0;
    for (size_t i = 0; i < (size_t)(SR * 0.25f); i++) {
        if (engage_gate_next(&g, false) == 0.0f) {
            found = true;
            at = i;
            break;
        }
    }
    CHECK(found, "gate never reached zero");
    if (found) {
        CHECK((float)at / SR < 0.25f, "took %zu samples", at);
    }
}

void test_gate(void) {
    born_closed_is_exact_zero_forever();
    born_open_is_exact_unity();
    travel_never_steps();
    closing_reaches_true_zero_within_a_quarter_second();
}
