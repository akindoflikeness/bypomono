#include "../src/dsp/dsp.h"
#include "test.h"

#define SR 48000.0f
#define NOTE_HZ 220.0f
#define SETTLE 24000 /* half a second at SR, so every glide has settled */
#define WINDOW 8192

static void mix_emit(void *userdata, size_t n, const Frame *frame) {
    ((float *)userdata)[n] = frame->mix;
}

/* the fundamental by normalised autocorrelation: the shortest lag whose
   peak is within a few percent of the strongest one, refined parabolically */
static float fundamental_hz(const float *x, int n) {
    int min_lag = (int)(SR / 2000.0f), max_lag = (int)(SR / 40.0f);
    float energy = 0.0f;
    for (int i = 0; i < n; i++) energy += x[i] * x[i];
    if (energy <= 0.0f) return 0.0f;
    static float r[WINDOW];
    float best = 0.0f;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        float acc = 0.0f;
        for (int i = 0; i + lag < n; i++) acc += x[i] * x[i + lag];
        r[lag] = acc / energy;
        if (r[lag] > best) best = r[lag];
    }
    for (int lag = min_lag + 1; lag < max_lag; lag++) {
        if (r[lag] < 0.9f * best) continue;
        if (r[lag] < r[lag - 1] || r[lag] < r[lag + 1]) continue;
        float a = r[lag - 1], b = r[lag], c = r[lag + 1];
        float den = a - 2.0f * b + c;
        float shift = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
        return SR / ((float)lag + shift);
    }
    return 0.0f;
}

static float rendered_pitch(Patch patch) {
    static float buf[SETTLE + WINDOW];
    Voice v;
    voice_init(&v, SR, patch);
    voice_set_freq_hz(&v, NOTE_HZ);
    voice_render_frames(&v, SETTLE + WINDOW, mix_emit, buf);
    voice_free(&v);
    return fundamental_hz(buf + SETTLE, WINDOW);
}

/* an integer-ratio spectrum repeats at the base whatever op carries, so the
   carriers are also checked on their own at index 0 */
static void every_algorithm_sounds_at_its_pitch_in_harmonic_mode(void) {
    for (int i = 0; i < 8; i++) {
        Patch patch = patch_init(ALGORITHMS[i], RATIO_HARMONIC);
        float hz = rendered_pitch(patch);
        CHECK(fabsf(hz - NOTE_HZ) <= 0.02f * NOTE_HZ,
              "algorithm %d in harmonic mode sounds at %g hz, not %g", i + 1,
              (double)hz, (double)NOTE_HZ);
        patch.index = 0.0f;
        hz = rendered_pitch(patch);
        CHECK(fabsf(hz - NOTE_HZ) <= 0.02f * NOTE_HZ,
              "algorithm %d carriers in harmonic mode sound at %g hz, not %g",
              i + 1, (double)hz, (double)NOTE_HZ);
    }
}

static void every_carrier_sits_on_the_fundamental_in_golden_mode(void) {
    for (int i = 0; i < 8; i++) {
        Patch patch = patch_init(ALGORITHMS[i], RATIO_GOLDEN);
        patch.index = 0.0f;
        float hz = rendered_pitch(patch);
        CHECK(fabsf(hz - NOTE_HZ) <= 0.02f * NOTE_HZ,
              "algorithm %d carriers in golden mode sound at %g hz, not %g",
              i + 1, (double)hz, (double)NOTE_HZ);
    }
}

static void carriers_take_the_first_table_entry(void) {
    for (int mode = 0; mode < RATIO_MODE_COUNT; mode++) {
        for (int i = 0; i < 8; i++) {
            Patch patch = patch_init(ALGORITHMS[i], (RatioMode)mode);
            Compiled compiled = compile(ALGORITHMS[i]);
            bool used[NUM_OPS] = {false};
            for (int op = 0; op < NUM_OPS; op++) {
                if ((compiled.carriers >> op & 1) == 1) {
                    CHECK(patch.ops[op].ratio == 1.0f,
                          "mode %d algorithm %d carrier op%d ratio %g", mode,
                          i + 1, op + 1, (double)patch.ops[op].ratio);
                    continue;
                }
                int slot = -1;
                for (int k = 0; k < NUM_OPS; k++)
                    if (!used[k]
                        && ratio_mode_ratio((RatioMode)mode, k) == patch.ops[op].ratio) {
                        slot = k;
                        break;
                    }
                CHECK(slot >= 0, "mode %d algorithm %d op%d ratio %g is off the table",
                      mode, i + 1, op + 1, (double)patch.ops[op].ratio);
                if (slot >= 0) used[slot] = true;
            }
        }
    }
}

void test_pitch(void) {
    carriers_take_the_first_table_entry();
    every_algorithm_sounds_at_its_pitch_in_harmonic_mode();
    every_carrier_sits_on_the_fundamental_in_golden_mode();
}
