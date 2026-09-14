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

void test_master(void) {
    quiet_passes_untouched_and_loud_cannot_escape();
    the_knee_has_no_corner();
    a_raised_ceiling_does_not_move_quiet_signal();
}
