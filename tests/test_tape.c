#include "../src/dsp/dsp.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>

#define SR 48000.0f
#define TAU_D 6.283185307179586476925286766559

static double bin(const float *x, size_t len, float hz) {
    double w = TAU_D * (double)hz / (double)SR;
    double c = cos(w), s = sin(w);
    double zr = 0.0, zi = 0.0;
    for (size_t i = 0; i < len; i++) {
        double t = zr;
        zr = c * zr - s * zi + (double)x[i];
        zi = s * t + c * zi;
    }
    return sqrt(zr * zr + zi * zi) / (double)len;
}

static double rms(const float *x, size_t len) {
    double sum = 0.0;
    for (size_t i = 0; i < len; i++) {
        sum += (double)x[i] * (double)x[i];
    }
    return sqrt(sum / (double)len);
}

typedef float (*GenFn)(size_t i, const void *ctx);

static float *run(float w, GenFn gen, const void *ctx, size_t n) {
    Tape t;
    tape_init(&t, SR);
    tape_set(&t, w);
    for (size_t i = 0; i < (size_t)SR / 2; i++) {
        float x = gen(i, ctx);
        tape_process(&t, (Stereo){x, x});
    }
    float *out = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        float x = gen(i, ctx);
        out[i] = tape_process(&t, (Stereo){x, x}).l;
    }
    return out;
}

static float nominal(void) {
    return powf(10.0f, COMP_NOMINAL_DBFS / 20.0f);
}

typedef struct { float hz, amp; } ToneCtx;

static float tone_gen(size_t i, const void *ctx) {
    const ToneCtx *t = ctx;
    return sinf(TAU_F * t->hz * (float)i / SR) * t->amp;
}

static float at_lift(float dbfs, float lift) {
    return tether_lift_db(powf(10.0f, dbfs / 20.0f), lift);
}

static void the_tether_lifts_quiet_top_and_leaves_loud_top_alone(void) {
    float lift = TETHER_LIFT_DB;
    CHECK(at_lift(TETHER_THRESHOLD_DB + 6.0f, lift) == 0.0f, "lift above threshold: %g",
          at_lift(TETHER_THRESHOLD_DB + 6.0f, lift));
    CHECK(at_lift(-10.0f, lift) == 0.0f, "lift at -10: %g", at_lift(-10.0f, lift));
    CHECK(fabsf(at_lift(-90.0f, lift) - lift) < 1e-4f, "%g vs %g", at_lift(-90.0f, lift), lift);
    CHECK(at_lift(-40.0f, lift) > 0.0f && at_lift(-40.0f, lift) <= lift, "lift at -40: %g",
          at_lift(-40.0f, lift));
    float prev = 0.0f;
    float last_step = 0.0f;
    for (int i = 0; i <= 120; i++) {
        float db = TETHER_THRESHOLD_DB + 4.0f - (float)i * 0.5f;
        float v = at_lift(db, lift);
        float step = v - prev;
        CHECK(step >= -1e-6f, "the curve must not fall as the band gets quieter");
        CHECK(step - last_step < 0.06f, "corner at %g dBFS: %g -> %g", db, last_step, step);
        last_step = step;
        prev = v;
    }
}

static void the_moving_phase_costs_no_level(void) {
    float freqs[3] = {TETHER_AP_LO_HZ, 2000.0f, TETHER_AP_HI_HZ};
    for (int f = 0; f < 3; f++) {
        float hz = freqs[f];
        Biquad ap = {0};
        biquad_allpass(&ap, hz, SR);
        uint32_t n = 12345;
        float *noise = malloc(40000 * sizeof(float));
        float *out = malloc(40000 * sizeof(float));
        for (int i = 0; i < 40000; i++) {
            n = n * 1664525u + 1013904223u;
            noise[i] = (float)(n >> 9) / (float)(1u << 23) - 0.5f;
        }
        for (int i = 0; i < 40000; i++) {
            out[i] = biquad_process(&ap, noise[i]);
        }
        double a = rms(noise + 2000, 40000 - 2000);
        double b = rms(out + 2000, 40000 - 2000);
        double d = 20.0 * log10(b / a);
        CHECK(fabs(d) < 0.05, "allpass at %g hz moved the level %.3f db", hz, d);
        free(noise);
        free(out);
    }
}

static float phase_corner(float amp) {
    Tape t;
    tape_init(&t, SR);
    tape_set(&t, 1.0f);
    uint32_t n = 99;
    for (size_t i = 0; i < (size_t)SR * 2; i++) {
        n = n * 1664525u + 1013904223u;
        float x = amp * ((float)(n >> 9) / (float)(1u << 23) - 0.5f) * 2.0f;
        tape_process(&t, (Stereo){x, x});
    }
    return t.ap_hz;
}

static void the_phase_follows_the_material(void) {
    float quiet = phase_corner(powf(10.0f, -70.0f / 20.0f));
    float loud = phase_corner(powf(10.0f, -6.0f / 20.0f));
    CHECK(quiet > loud + 200.0f,
          "the corner should sit higher on quiet material: quiet %.0f hz, loud %.0f hz", quiet,
          loud);
    CHECK(loud >= TETHER_AP_LO_HZ - 1.0f && quiet <= TETHER_AP_HI_HZ + 1.0f,
          "the corner must stay inside its range: %.0f..%.0f hz", loud, quiet);
}

static void zero_is_a_straight_wire(void) {
    Tape t;
    tape_init(&t, SR);
    tape_set(&t, 0.0f);
    for (size_t i = 0; i < (size_t)SR; i++) {
        Stereo x = {sinf((float)i * 0.01f) * 0.6f, cosf((float)i * 0.013f) * 0.6f};
        Stereo y = tape_process(&t, x);
        CHECK(y.l == x.l && y.r == x.r, "sample %zu", i);
    }
}

static void warmth_pushes_the_level_up(void) {
    size_t n = (size_t)SR;
    float quiet = powf(10.0f, -40.0f / 20.0f);
    ToneCtx ctx = {1000.0f, quiet};
    float *y = run(0.0f, tone_gen, &ctx, n);
    double base = bin(y, n, 1000.0f);
    free(y);
    double last = base;
    float ws[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (int k = 0; k < 4; k++) {
        y = run(ws[k], tone_gen, &ctx, n);
        double g = bin(y, n, 1000.0f);
        free(y);
        CHECK(g > last, "warmth %g did not push: %.6f against %.6f", ws[k], g, last);
        last = g;
    }
    double db = 20.0 * log10(last / base);
    CHECK(db > 12.0, "the top of the slider only added %.1f dB", db);
}

static double loud_rms_db(float w, size_t n) {
    ToneCtx ctx = {1000.0f, 0.5f};
    float *y = run(w, tone_gen, &ctx, n);
    double v = 20.0 * log10(rms(y, n));
    free(y);
    return v;
}

static void loud_material_lands_rather_than_climbing(void) {
    size_t n = (size_t)SR;
    double half = loud_rms_db(0.5f, n);
    double full = loud_rms_db(1.0f, n);
    CHECK(full - half < 6.0, "the top of the slider climbed %.1f dB instead of landing",
          full - half);
    float ws[2] = {0.5f, 1.0f};
    for (int k = 0; k < 2; k++) {
        ToneCtx ctx = {1000.0f, 0.5f};
        float *y = run(ws[k], tone_gen, &ctx, n);
        float p = 0.0f;
        for (size_t i = 0; i < n; i++) {
            p = fmaxf(p, fabsf(y[i]));
        }
        free(y);
        CHECK(p <= 1.0f, "warmth %g peaked at %g", ws[k], p);
    }
}

static void the_curve_makes_even_harmonics_not_odd(void) {
    size_t n = (size_t)SR * 2;
    float ws[3] = {0.4f, 0.7f, 1.0f};
    for (int k = 0; k < 3; k++) {
        float w = ws[k];
        ToneCtx ctx = {220.0f, nominal()};
        float *y = run(w, tone_gen, &ctx, n);
        double f = bin(y, n, 220.0f);
        double second = 20.0 * log10(bin(y, n, 440.0f) / f);
        double third = 20.0 * log10(bin(y, n, 660.0f) / f);
        free(y);
        CHECK(second > -45.0, "warmth %g: no second harmonic, %.1f dB", w, second);
        CHECK(second > third + 24.0, "warmth %g: second %.1f dB is not clear of third %.1f dB", w,
              second, third);
    }
}

static void warmth_does_not_alias(void) {
    size_t n = (size_t)SR * 2;
    float ws[2] = {0.5f, 1.0f};
    for (int k = 0; k < 2; k++) {
        float w = ws[k];
        ToneCtx ctx = {15000.0f, nominal()};
        float *y = run(w, tone_gen, &ctx, n);
        double f = bin(y, n, 15000.0f);
        float bins[4] = {3000.0f, 6000.0f, 12000.0f, 18000.0f};
        for (int j = 0; j < 4; j++) {
            double a = 20.0 * log10(bin(y, n, bins[j]) / f);
            CHECK(a < -60.0, "warmth %g: %.1f dB folded back at %g Hz", w, a, bins[j]);
        }
        free(y);
    }
}

static float burst_gen(size_t i, const void *ctx) {
    (void)ctx;
    size_t t = i % ((size_t)SR / 4);
    if (t < (size_t)SR / 25) {
        return sinf(TAU_F * 440.0f * (float)i / SR) * 0.7f;
    }
    return 0.0f;
}

static void transients_keep_their_peak(void) {
    size_t n = (size_t)SR * 2;
    float *y = run(0.0f, burst_gen, NULL, n);
    float base = 0.0f;
    for (size_t i = 0; i < n; i++) {
        base = fmaxf(base, fabsf(y[i]));
    }
    free(y);
    float ws[3] = {0.3f, 0.6f, 1.0f};
    for (int k = 0; k < 3; k++) {
        y = run(ws[k], burst_gen, NULL, n);
        float p = 0.0f;
        for (size_t i = 0; i < n; i++) {
            p = fmaxf(p, fabsf(y[i]));
        }
        free(y);
        double db = 20.0 * log10((double)p / (double)base);
        CHECK(db > -3.0, "warmth %g: a transient lost %.2f dB of its peak", ws[k], db);
    }
}

static float left_after(float sustain_s) {
    Tape t;
    tape_init(&t, SR);
    tape_set(&t, 1.0f);
    size_t loud = (size_t)(SR * sustain_s);
    for (size_t i = 0; i < loud; i++) {
        float x = sinf(TAU_F * 300.0f * (float)i / SR) * 0.8f;
        tape_process(&t, (Stereo){x, x});
    }
    for (size_t i = 0; i < (size_t)(SR * 0.15f); i++) {
        tape_process(&t, (Stereo){0.0f, 0.0f});
    }
    return fmaxf(t.env_fast, t.env_slow);
}

static void the_release_is_faster_after_a_hit_than_after_a_drone(void) {
    float hit = left_after(0.01f);
    float drone = left_after(3.0f);
    CHECK(drone > hit * 3.0f, "the release did not adapt: %.5f left after a hit, %.5f after a drone",
          hit, drone);
}

static float zero_gen(size_t i, const void *ctx) {
    (void)i;
    (void)ctx;
    return 0.0f;
}

static void silence_stays_silent_at_every_warmth(void) {
    size_t n = (size_t)SR;
    float ws[4] = {0.0f, 0.3f, 0.6f, 1.0f};
    for (int k = 0; k < 4; k++) {
        float *y = run(ws[k], zero_gen, NULL, n);
        CHECK(rms(y, n) == 0.0, "warmth %g added something out of nothing", ws[k]);
        free(y);
    }
}

static float slider_tone(size_t i) {
    return sinf((float)i * 0.02f) * 0.5f;
}

static float worst_over(Tape *t, size_t from) {
    float prev = tape_process(t, (Stereo){slider_tone(from), slider_tone(from)}).l;
    float worst = 0.0f;
    for (size_t i = from + 1; i < from + 24000; i++) {
        float l = tape_process(t, (Stereo){slider_tone(i), slider_tone(i)}).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    return worst;
}

static void the_slider_never_steps(void) {
    size_t warm = (size_t)(SR * 0.5f);
    Tape settled;
    tape_init(&settled, SR);
    tape_set(&settled, 1.0f);
    for (size_t i = 0; i < warm; i++) {
        tape_process(&settled, (Stereo){slider_tone(i), slider_tone(i)});
    }
    float steady = worst_over(&settled, warm);
    Tape dragged;
    tape_init(&dragged, SR);
    tape_set(&dragged, 0.0f);
    for (size_t i = 0; i < warm; i++) {
        tape_process(&dragged, (Stereo){slider_tone(i), slider_tone(i)});
    }
    tape_set(&dragged, 1.0f);
    float moving = worst_over(&dragged, warm);
    CHECK(moving <= steady * 1.25f + 0.005f,
          "the drag stepped: %.4f while moving against %.4f settled", moving, steady);
}

static void it_stays_bounded_at_every_setting(void) {
    float ws[3] = {0.0f, 0.5f, 1.0f};
    for (int k = 0; k < 3; k++) {
        float w = ws[k];
        Tape t;
        tape_init(&t, SR);
        tape_set(&t, w);
        float peak = 0.0f;
        for (size_t i = 0; i < (size_t)SR * 2; i++) {
            float x = sinf((float)i * 0.05f) * 0.95f;
            Stereo y = tape_process(&t, (Stereo){x, -x});
            CHECK(isfinite(y.l) && isfinite(y.r), "warmth %g: not finite at %zu", w, i);
            peak = fmaxf(peak, fmaxf(fabsf(y.l), fabsf(y.r)));
        }
        CHECK(peak <= 1.0f, "warmth %g: peak %g left the ceiling", w, peak);
    }
}

void test_tape(void) {
    the_tether_lifts_quiet_top_and_leaves_loud_top_alone();
    the_moving_phase_costs_no_level();
    the_phase_follows_the_material();
    zero_is_a_straight_wire();
    warmth_pushes_the_level_up();
    loud_material_lands_rather_than_climbing();
    the_curve_makes_even_harmonics_not_odd();
    warmth_does_not_alias();
    transients_keep_their_peak();
    the_release_is_faster_after_a_hit_than_after_a_drone();
    silence_stays_silent_at_every_warmth();
    the_slider_never_steps();
    it_stays_bounded_at_every_setting();
}
