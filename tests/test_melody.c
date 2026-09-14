#include "../src/dsp/dsp.h"
#include "test.h"
#include <string.h>

extern const float melody_fib_hz[10];

#define SR 48000.0f

static const Tuning TUNING_ALL[NUM_TUNINGS] = {
    TUNING_SCALE, TUNING_FIBONACCI_HZ, TUNING_GOLDEN_POWERS,
    TUNING_PLASTIC_POWERS, TUNING_GOLDEN_WALK,
};

static const Scale SCALE_ALL[NUM_SCALES] = {
    SCALE_PHRYGIAN,         SCALE_PHRYGIAN_DOMINANT, SCALE_NATURAL_MINOR,
    SCALE_HARMONIC_MINOR,   SCALE_NEAPOLITAN_MINOR,  SCALE_BYZANTINE,
    SCALE_FIBONACCI,        SCALE_PHYLLOTAXIS,
};

static const HoldSource SOURCE_ALL[2] = {HOLD_GOLDEN_WEYL, HOLD_XORSHIFT};

static int hz_to_midi(float hz) {
    return (int)roundf(69.0f + 12.0f * log2f(hz / 440.0f));
}

static bool contains_u8(const uint8_t *arr, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) {
        if (arr[i] == v) {
            return true;
        }
    }
    return false;
}

static void sort_u8(uint8_t *arr, size_t n) {
    for (size_t i = 1; i < n; i++) {
        uint8_t v = arr[i];
        size_t j = i;
        while (j > 0 && arr[j - 1] > v) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = v;
    }
}

static void phyllotaxis_is_golden_angle_placement(void) {
    uint8_t notes[7];
    for (int k = 0; k < 7; k++) {
        notes[k] = (uint8_t)roundf(fmodf((float)k * 12.0f / PHI, 12.0f)) % 12;
    }
    sort_u8(notes, 7);
    size_t n = 0;
    for (size_t i = 0; i < 7; i++) {
        if (n == 0 || notes[n - 1] != notes[i]) {
            notes[n++] = notes[i];
        }
    }
    size_t count;
    const uint8_t *iv = scale_intervals(SCALE_PHYLLOTAXIS, &count);
    CHECK(n == count, "phyllotaxis dedup count %zu vs %zu", n, count);
    for (size_t i = 0; i < n && i < count; i++) {
        CHECK(notes[i] == iv[i], "phyllotaxis note %zu: %d vs %d", i, notes[i], iv[i]);
    }
    size_t pcount;
    const uint8_t *phrygian = scale_intervals(SCALE_PHRYGIAN, &pcount);
    size_t shared = 0;
    for (size_t i = 0; i < count; i++) {
        if (contains_u8(phrygian, pcount, iv[i])) {
            shared++;
        }
    }
    CHECK(shared == 6, "shared with phrygian: %zu", shared);
}

static void fibonacci_scale_derives_and_self_terminates(void) {
    uint8_t degrees[16];
    size_t dn = 1;
    degrees[0] = 0;
    uint32_t a = 1, b = 1;
    for (;;) {
        uint8_t n = (uint8_t)(a % 12);
        if (contains_u8(degrees, dn, n) && a > 1) {
            CHECK(a == 13, "termination should occur at F = 13, got %u", a);
            break;
        }
        if (!contains_u8(degrees, dn, n)) {
            degrees[dn++] = n;
        }
        uint32_t next = a + b;
        b = a;
        a = next;
    }
    sort_u8(degrees, dn);
    size_t count;
    const uint8_t *iv = scale_intervals(SCALE_FIBONACCI, &count);
    CHECK(dn == count, "fibonacci degree count %zu vs %zu", dn, count);
    for (size_t i = 0; i < dn && i < count; i++) {
        CHECK(degrees[i] == iv[i], "fibonacci degree %zu: %d vs %d", i, degrees[i], iv[i]);
    }
}

static void fibonacci_hz_ladder_converges_on_phi(void) {
    CHECK(fabsf(melody_fib_hz[7] / melody_fib_hz[6] - PHI) < 1e-4f, "987/610 vs phi");
    for (int i = 0; i < 9; i++) {
        float ratio = melody_fib_hz[i + 1] / melody_fib_hz[i];
        CHECK(fabsf(ratio - PHI) < 0.02f, "ratio %g strays from phi", ratio);
    }
}

static void melodies_are_deterministic(void) {
    for (int ti = 0; ti < NUM_TUNINGS; ti++) {
        for (int si = 0; si < 2; si++) {
            MelodyParams params = melody_params_default();
            params.enabled = true;
            params.tuning = TUNING_ALL[ti];
            params.source = SOURCE_ALL[si];
            Melody a, b;
            melody_init(&a, SR, params);
            melody_init(&b, SR, params);
            for (int i = 0; i < 500; i++) {
                float fa = melody_fire(&a);
                float fb = melody_fire(&b);
                CHECK(fa == fb, "%s/%d: %g vs %g", tuning_name(params.tuning), si, fa, fb);
            }
        }
    }
}

static void fired_notes_stay_in_scale(void) {
    for (int sc = 0; sc < NUM_SCALES; sc++) {
        for (int si = 0; si < 2; si++) {
            MelodyParams params = melody_params_default();
            params.enabled = true;
            params.scale = SCALE_ALL[sc];
            params.source = SOURCE_ALL[si];
            params.range_degrees = 11;
            uint8_t root = params.root_midi;
            size_t count;
            const uint8_t *iv = scale_intervals(params.scale, &count);
            Melody m;
            melody_init(&m, SR, params);
            for (int i = 0; i < 300; i++) {
                int offset = hz_to_midi(melody_fire(&m)) - (int)root;
                CHECK(offset >= 0, "%s: note below root", scale_name(params.scale));
                CHECK(contains_u8(iv, count, (uint8_t)(offset % 12)),
                      "%s: %d not in scale", scale_name(params.scale), offset);
            }
        }
    }
}

static void free_tunings_respect_their_bounds(void) {
    float root_hz = midi_to_hz(45);
    const Tuning tunings[3] = {TUNING_GOLDEN_POWERS, TUNING_PLASTIC_POWERS,
                               TUNING_GOLDEN_WALK};
    for (int ti = 0; ti < 3; ti++) {
        Tuning tuning = tunings[ti];
        MelodyParams params = melody_params_default();
        params.enabled = true;
        params.tuning = tuning;
        params.range_degrees = 24;
        Melody m;
        melody_init(&m, SR, params);
        bool has_prev = false;
        float prev = 0.0f;
        for (int i = 0; i < 500; i++) {
            float hz = melody_fire(&m);
            CHECK(hz >= root_hz * 0.999f && hz <= MELODY_PITCH_CAP_HZ,
                  "%s: %g", tuning_name(tuning), hz);
            if (tuning == TUNING_GOLDEN_WALK) {
                float hi = root_hz * exp2f(24.0f / 12.0f);
                CHECK(hz <= hi * 1.001f, "walk escaped its bounds: %g", hz);
                if (has_prev) {
                    float r = hz > prev ? hz / prev : prev / hz;
                    CHECK(fabsf(r - PHI) < 1e-3f || r < PHI,
                          "walk step %g exceeds phi", r);
                }
                prev = hz;
                has_prev = true;
            }
        }
    }
}

static void golden_weyl_visits_every_degree_fairly(void) {
    MelodyParams params = melody_params_default();
    params.enabled = true;
    params.range_degrees = 7;
    Melody m;
    melody_init(&m, SR, params);
    size_t counts[7] = {0};
    size_t count;
    const uint8_t *intervals = scale_intervals(params.scale, &count);
    for (int i = 0; i < 700; i++) {
        uint8_t offset = (uint8_t)(hz_to_midi(melody_fire(&m)) - (int)params.root_midi) % 12;
        int pos = -1;
        for (size_t d = 0; d < count; d++) {
            if (intervals[d] == offset) {
                pos = (int)d;
                break;
            }
        }
        CHECK(pos >= 0, "offset %d not a scale degree", offset);
        if (pos >= 0 && pos < 7) {
            counts[pos]++;
        }
    }
    for (int d = 0; d < 7; d++) {
        CHECK(counts[d] >= 50, "degree %d visited only %zu/700 times", d, counts[d]);
    }
}

static void a_tuning_that_claims_a_grid_lands_on_one(void) {
    const uint8_t RANGE = 8;
    for (int ti = 0; ti < NUM_TUNINGS; ti++) {
        Tuning tuning = TUNING_ALL[ti];
        MelodyParams params = melody_params_default();
        params.enabled = true;
        params.tuning = tuning;
        params.range_degrees = RANGE;
        Melody m;
        melody_init(&m, SR, params);
        float seen[600];
        size_t seen_n = 0;
        for (int i = 0; i < 600; i++) {
            float hz = melody_fire(&m);
            bool found = false;
            for (size_t s = 0; s < seen_n; s++) {
                if (fabsf(seen[s] - hz) < 1e-3f) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                seen[seen_n++] = hz;
            }
        }
        if (quantization_is_quantized(tuning_quantization(tuning))) {
            CHECK(seen_n <= (size_t)RANGE,
                  "%s claims a grid but produced %zu distinct pitches for %d degrees",
                  tuning_name(tuning), seen_n, RANGE);
        } else {
            CHECK(seen_n > (size_t)RANGE,
                  "%s claims no grid yet produced only %zu distinct pitches - "
                  "it is snapping to something",
                  tuning_name(tuning), seen_n);
        }
    }
}

static void one_tuning_is_twelve_tet_and_the_rest_are_symbols(void) {
    Tuning tet[NUM_TUNINGS];
    size_t tet_n = 0;
    for (int ti = 0; ti < NUM_TUNINGS; ti++) {
        if (tuning_quantization(TUNING_ALL[ti]).kind == QUANT_TWELVE_TET) {
            tet[tet_n++] = TUNING_ALL[ti];
        }
    }
    CHECK(tet_n == 1 && tet[0] == TUNING_SCALE,
          "twelve-tet tunings: %zu", tet_n);
    for (int ti = 0; ti < NUM_TUNINGS; ti++) {
        Quantization q = tuning_quantization(TUNING_ALL[ti]);
        if (q.kind == QUANT_TWELVE_TET) {
            CHECK(strcmp(q.symbol, "") == 0, "twelve-tet symbol not empty");
        } else {
            CHECK(strlen(q.symbol) > 0, "%s has no symbol to show",
                  tuning_name(TUNING_ALL[ti]));
        }
    }
}

static void fires_at_the_configured_rate(void) {
    MelodyParams params = melody_params_default();
    params.enabled = true;
    params.rate_hz = 2.0f;
    Melody m;
    melody_init(&m, SR, params);
    int fires = 0;
    size_t remaining = (size_t)(SR * 10.0f);
    while (remaining > 0) {
        size_t c;
        if (!melody_samples_until_fire(&m, &c)) {
            break;
        }
        if (c == 0) {
            melody_fire(&m);
            fires++;
        } else {
            size_t step = c < remaining ? c : remaining;
            melody_advance(&m, step);
            remaining -= step;
        }
    }
    CHECK(fires >= 19 && fires <= 21, "expected ~20 fires, got %d", fires);
}

void test_melody(void) {
    phyllotaxis_is_golden_angle_placement();
    fibonacci_scale_derives_and_self_terminates();
    fibonacci_hz_ladder_converges_on_phi();
    melodies_are_deterministic();
    fired_notes_stay_in_scale();
    free_tunings_respect_their_bounds();
    golden_weyl_visits_every_degree_fairly();
    a_tuning_that_claims_a_grid_lands_on_one();
    one_tuning_is_twelve_tet_and_the_rest_are_symbols();
    fires_at_the_configured_rate();
}
