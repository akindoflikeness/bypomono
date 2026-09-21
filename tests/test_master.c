#include "../src/dsp/dsp.h"
#include "test.h"

static void quiet_passes_untouched_and_loud_cannot_escape(void) {
    static const float quiet[] = {0.0f, 0.1f, 0.35f, 0.6999f};
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) {
        float x = quiet[i];
        CHECK(soft_clip(x) == x, "%g should be untouched below the knee", x);
        CHECK(soft_clip(-x) == -x, "%g should be untouched below the knee", x);
    }
    static const float loud[] = {0.71f, 1.0f, 2.5f, 40.0f, 1e6f};
    for (size_t i = 0; i < sizeof loud / sizeof loud[0]; i++) {
        float x = loud[i];
        CHECK(soft_clip(x) <= 1.0f, "%g escaped the ceiling", x);
        CHECK(soft_clip(-x) >= -1.0f, "-%g escaped the ceiling", x);
    }
    CHECK(soft_clip(0.9f) < 0.9f, "above the knee it must actually shape");
}

static void the_knee_has_no_corner(void) {
    float d = 1e-4f;
    float slope_below = (soft_clip(SOFT_CLIP_KNEE - d) - soft_clip(SOFT_CLIP_KNEE - 2.0f * d)) / d;
    float slope_above = (soft_clip(SOFT_CLIP_KNEE + 2.0f * d) - soft_clip(SOFT_CLIP_KNEE + d)) / d;
    CHECK(fabsf(slope_below - slope_above) < 1e-2f,
          "slope jumps at the knee: %g below, %g above", slope_below, slope_above);
}

static void a_raised_ceiling_does_not_move_quiet_signal(void) {
    static const float xs[] = {0.05f, 0.4f, 1.0f, 1.8f};
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        float x = xs[i];
        float y = soft_clip_to(x, 2.618f);
        CHECK(fabsf(y - x) < 1e-6f, "%g moved to %g under a ceiling it never reached", x, y);
    }
    CHECK(soft_clip_to(100.0f, 2.618f) <= 2.618f, "loud escaped the raised ceiling");
}

static void output_limiter_is_linked_bounded_and_recovers(void) {
    Limiter l;
    limiter_init(&l, 48000.0f);
    limiter_set(&l, true, -1.0f);
    float ceiling = powf(10.0f, -1.0f / 20.0f);
    float peak = 0.0f;
    float ratio = 0.0f;
    for (int i = 0; i < 48000; i++) {
        float x = i < 240 ? 4.0f : 0.25f;
        Stereo y = limiter_process(&l, (Stereo){x, 0.5f * x});
        peak = fmaxf(peak, fmaxf(fabsf(y.l), fabsf(y.r)));
        if (i > 200 && fabsf(y.l) > 1e-6f) ratio = y.r / y.l;
    }
    CHECK(peak <= ceiling + 1e-6f, "limiter peak %g exceeded %g", peak, ceiling);
    CHECK(fabsf(ratio - 0.5f) < 1e-5f, "stereo link changed image to %g", ratio);
    CHECK(limiter_reduction_db(&l) < 0.1f, "limiter never recovered: %g dB",
          limiter_reduction_db(&l));
    limiter_free(&l);
}

void test_master(void) {
    quiet_passes_untouched_and_loud_cannot_escape();
    the_knee_has_no_corner();
    a_raised_ceiling_does_not_move_quiet_signal();
    output_limiter_is_linked_bounded_and_recovers();
}
