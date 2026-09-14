#include "../src/dsp/dsp.h"
#include "test.h"
#include <float.h>

#define SR 48000.0f

static const RatioMode ALL_MODES[5] = {RATIO_HARMONIC, RATIO_FIBONACCI, RATIO_GOLDEN,
                                       RATIO_GOLDEN_MIRROR, RATIO_PLASTIC};

static void the_gain_never_falls_below_master_or_rises_above_unity(void) {
    const float masters[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const float fields[3] = {0.0f, 0.5f, 1.0f};
    for (int m = 0; m < 5; m++) {
        RatioMode mode = ALL_MODES[m];
        for (int j = 0; j < 5; j++) {
            float master = masters[j];
            for (int k = 0; k < 3; k++) {
                float field = fields[k];
                Breath b;
                breath_init(&b, SR, mode);
                for (size_t i = 0; i < (size_t)SR * 30; i++) {
                    float hz = 110.0f + (float)(i % 300);
                    Field out = breath_tick(&b, hz, field, master, 0.5f);
                    CHECK(out.gain >= master - 1e-6f && out.gain <= 1.0f + 1e-6f,
                          "mode %d master %g field %g: gain %g left [%g, 1]", (int)mode, master,
                          field, out.gain, master);
                }
            }
        }
    }
}

static void a_closed_field_is_exactly_master_and_no_pitch_movement(void) {
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    for (int i = 0; i < 10000; i++) {
        Field out = breath_tick(&b, 110.0f, 0.0f, 0.7f, 0.5f);
        CHECK(out.gain == 0.7f, "gain %g", out.gain);
        CHECK(out.pitch == 1.0f, "pitch %g", out.pitch);
    }
}

static void the_sequencer_cannot_drive_the_rate_to_audio(void) {
    const float hzs[3] = {4186.0f, 20000.0f, 1e6f};
    for (int i = 0; i < 3; i++) {
        float hz = hzs[i];
        float r = breath_rate_hz(hz);
        CHECK(r <= BREATH_RATE_MAX_HZ + 1e-6f, "%g hz produced a %g hz rate", hz, r);
        CHECK(r < 1.0f, "%g hz produced %g hz - audible as a rate, not a drift", hz, r);
    }
    CHECK(fabsf(breath_rate_hz(440.0f) - BREATH_RATE_MAX_HZ) < 1e-4f, "rate at 440 is %g",
          breath_rate_hz(440.0f));
    CHECK(fabsf(breath_rate_hz(27.5f) - BREATH_RATE_MIN_HZ) < 1e-4f, "rate at 27.5 is %g",
          breath_rate_hz(27.5f));
}

static float drift_of(RatioMode mode) {
    Breath b;
    breath_init(&b, SR, mode);
    float f = breath_rate_hz(110.0f);
    size_t period = (size_t)(SR / f * 5.0f);
    float first[64];
    float second[64];
    for (int i = 0; i < 64; i++) {
        first[i] = breath_tick(&b, 110.0f, 1.0f, 0.0f, 0.5f).amount;
    }
    for (size_t i = 64; i < period; i++) {
        breath_tick(&b, 110.0f, 1.0f, 0.0f, 0.5f);
    }
    for (int i = 0; i < 64; i++) {
        second[i] = breath_tick(&b, 110.0f, 1.0f, 0.0f, 0.5f).amount;
    }
    float sum = 0.0f;
    for (int i = 0; i < 64; i++) {
        sum += fabsf(first[i] - second[i]);
    }
    return sum / 64.0f;
}

static void only_harmonic_mode_loops(void) {
    float harmonic = drift_of(RATIO_HARMONIC);
    const RatioMode others[4] = {RATIO_FIBONACCI, RATIO_GOLDEN, RATIO_GOLDEN_MIRROR,
                                 RATIO_PLASTIC};
    for (int i = 0; i < 4; i++) {
        float other = drift_of(others[i]);
        CHECK(other > harmonic * 10.0f,
              "mode %d drifted %g against harmonic's %g - it should never land in the same "
              "place twice, and harmonic always should",
              (int)others[i], other, harmonic);
    }
}

static void trace_of(RatioMode mode, float out[40]) {
    Breath b;
    breath_init(&b, SR, mode);
    int n = 0;
    for (size_t i = 0; i < (size_t)SR * 4; i++) {
        float amount = breath_tick(&b, 110.0f, 1.0f, 0.0f, 0.5f).amount;
        if (i % 4800 == 0) {
            out[n++] = amount;
        }
    }
}

static void every_mode_has_its_own_movement(void) {
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            float ta[40];
            float tb[40];
            trace_of(ALL_MODES[i], ta);
            trace_of(ALL_MODES[j], tb);
            float diff = 0.0f;
            for (int k = 0; k < 40; k++) {
                diff += fabsf(ta[k] - tb[k]);
            }
            diff /= 40.0f;
            CHECK(diff > 1e-3f, "modes %d and %d move identically", (int)ALL_MODES[i],
                  (int)ALL_MODES[j]);
        }
    }
}

static void the_pitch_movement_has_no_dc(void) {
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    size_t n = (size_t)SR * 60;
    double mean = 0.0;
    for (size_t i = 0; i < n; i++) {
        mean += (double)breath_tick(&b, 110.0f, 1.0f, 0.5f, 0.5f).pitch;
    }
    mean /= (double)n;
    CHECK(fabs(mean - 1.0) < 2e-3, "pitch drifted: mean multiplier %g", mean);
}

static void the_field_is_deterministic(void) {
    Breath a;
    Breath b;
    breath_init(&a, SR, RATIO_PLASTIC);
    breath_init(&b, SR, RATIO_PLASTIC);
    for (int i = 0; i < 20000; i++) {
        float hz = 110.0f + (float)(i % 100);
        Field x = breath_tick(&a, hz, 0.5f, 0.5f, 0.5f);
        Field y = breath_tick(&b, hz, 0.5f, 0.5f, 0.5f);
        CHECK(x.gain == y.gain, "gain %g vs %g", x.gain, y.gain);
        CHECK(x.pitch == y.pitch, "pitch %g vs %g", x.pitch, y.pitch);
    }
}

static float swing_at(float rate_hz, float field) {
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    size_t step = (size_t)(SR / rate_hz);
    for (int k = 0; k < 4; k++) {
        breath_trigger(&b);
        for (size_t i = 0; i < step; i++) {
            breath_tick(&b, 110.0f, field, 0.0f, 0.5f);
        }
    }
    breath_trigger(&b);
    float lo = FLT_MAX;
    float hi = -FLT_MAX;
    for (size_t i = 0; i < step; i++) {
        float g = breath_tick(&b, 110.0f, field, 0.0f, 0.5f).amount;
        lo = fminf(lo, g);
        hi = fmaxf(hi, g);
    }
    return hi - lo;
}

static void there_are_dynamics_at_the_sequencers_fastest_rate(void) {
    float fast = swing_at(8.0f, 1.0f);
    CHECK(fast > 0.05f, "at 8 hz the gain moved only %g within a note - no audible dynamics",
          fast);
    float slow = swing_at(0.5f, 1.0f);
    CHECK(slow > 0.05f, "at 0.5 hz the gain moved only %g", slow);
}

static void the_gain_also_keeps_growing_with_the_control(void) {
    float last = 0.0f;
    for (int step = 1; step <= 10; step++) {
        float field = (float)step / 10.0f;
        Breath b;
        breath_init(&b, SR, RATIO_GOLDEN);
        size_t stride = (size_t)(SR / 8.0f);
        for (int k = 0; k < 4; k++) {
            breath_trigger(&b);
            for (size_t i = 0; i < stride; i++) {
                breath_tick(&b, 110.0f, field, 0.5f, 0.5f);
            }
        }
        breath_trigger(&b);
        float lo = FLT_MAX;
        float hi = -FLT_MAX;
        for (size_t i = 0; i < stride; i++) {
            float g = breath_tick(&b, 110.0f, field, 0.5f, 0.5f).gain;
            lo = fminf(lo, g);
            hi = fmaxf(hi, g);
        }
        float swing = hi - lo;
        CHECK(swing > last,
              "at floor 0.5, field %g swung %g, no more than %g below it - the gain has "
              "flattened even though the movement has not",
              field, swing, last);
        last = swing;
    }
}

static void the_field_control_keeps_working_past_a_third(void) {
    float last = 0.0f;
    for (int step = 1; step <= 10; step++) {
        float field = (float)step / 10.0f;
        float swing = swing_at(8.0f, field);
        CHECK(swing > last,
              "field %g moved %g, no more than %g at the step below - the control has saturated",
              field, swing, last);
        last = swing;
    }
}

static void across_a_note(float field, float floor_, float *at_note, float *elsewhere_out) {
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    size_t step = (size_t)(SR / 4.0f);
    for (int k = 0; k < 4; k++) {
        breath_trigger(&b);
        for (size_t i = 0; i < step; i++) {
            breath_tick(&b, 110.0f, field, floor_, 0.5f);
        }
    }

    float before = breath_tick(&b, 110.0f, field, floor_, 0.5f).gain;
    breath_trigger(&b);
    float after = breath_tick(&b, 110.0f, field, floor_, 0.5f).gain;

    float prev = after;
    float elsewhere = 0.0f;
    for (size_t i = 0; i < step; i++) {
        float g = breath_tick(&b, 110.0f, field, floor_, 0.5f).gain;
        elsewhere = fmaxf(elsewhere, fabsf(g - prev));
        prev = g;
    }
    *at_note = fabsf(after - before);
    *elsewhere_out = elsewhere;
}

static void a_note_on_is_not_a_discontinuity_in_the_gain(void) {
    const float cases[5][2] = {
        {1.0f, 0.2f}, {1.0f, 0.5f}, {1.0f, 0.8f}, {0.5f, 0.5f}, {0.25f, 0.5f}};
    for (int i = 0; i < 5; i++) {
        float field = cases[i][0];
        float floor_ = cases[i][1];
        float at_note;
        float elsewhere;
        across_a_note(field, floor_, &at_note, &elsewhere);
        CHECK(at_note <= elsewhere,
              "field %g floor %g: the note stepped %.6f in one sample, more than the %.6f the "
              "movement manages anywhere else in the note - that step is the click",
              field, floor_, at_note, elsewhere);
    }
}

static void a_note_off_is_not_a_discontinuity_either(void) {
    const float fields[2] = {0.5f, 1.0f};
    for (int k = 0; k < 2; k++) {
        float field = fields[k];
        Breath b;
        breath_init(&b, SR, RATIO_GOLDEN);
        size_t step = (size_t)(SR / 4.0f);
        for (int j = 0; j < 4; j++) {
            breath_trigger(&b);
            for (size_t i = 0; i < step; i++) {
                breath_tick(&b, 110.0f, field, 0.5f, 0.5f);
            }
        }
        breath_trigger(&b);
        for (size_t i = 0; i < step; i++) {
            breath_tick(&b, 110.0f, field, 0.5f, 0.5f);
        }

        float before = breath_tick(&b, 110.0f, field, 0.5f, 0.5f).gain;
        breath_release(&b);
        float after = breath_tick(&b, 110.0f, field, 0.5f, 0.5f).gain;

        float prev = after;
        float elsewhere = 0.0f;
        for (size_t i = 0; i < step; i++) {
            float g = breath_tick(&b, 110.0f, field, 0.5f, 0.5f).gain;
            elsewhere = fmaxf(elsewhere, fabsf(g - prev));
            prev = g;
        }
        CHECK(fabsf(after - before) <= elsewhere,
              "field %g: the release stepped %.6f, more than the %.6f the movement manages "
              "elsewhere",
              field, fabsf(after - before), elsewhere);
    }
}

static void the_arrival_does_not_shrink_the_gesture(void) {
    size_t settle = (size_t)(SR * BREATH_DECLICK_S) + 1;
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    for (int i = 0; i < 1000; i++) {
        breath_tick(&b, 110.0f, 1.0f, 0.5f, 0.5f);
    }
    breath_trigger(&b);
    float peak = 0.0f;
    for (size_t i = 0; i < settle; i++) {
        peak = fmaxf(peak, breath_tick(&b, 110.0f, 1.0f, 0.5f, 0.5f).amount);
    }
    CHECK(peak > 0.9f, "the note only reached %.4f of its depth after the arrival", peak);
}

static void a_closed_field_is_untouched(void) {
    Breath b;
    breath_init(&b, SR, RATIO_GOLDEN);
    for (int i = 0; i < 10000; i++) {
        if (i % 1000 == 0) {
            breath_trigger(&b);
        }
        Field f = breath_tick(&b, 110.0f, 0.0f, 0.5f, 0.5f);
        CHECK(f.amount == 0.0f, "amount %g", f.amount);
        CHECK(f.gain == 0.5f, "gain %g", f.gain);
        CHECK(f.pitch == 1.0f, "pitch %g", f.pitch);
    }
}

void test_breath(void) {
    the_gain_never_falls_below_master_or_rises_above_unity();
    a_closed_field_is_exactly_master_and_no_pitch_movement();
    the_sequencer_cannot_drive_the_rate_to_audio();
    only_harmonic_mode_loops();
    every_mode_has_its_own_movement();
    the_pitch_movement_has_no_dc();
    the_field_is_deterministic();
    there_are_dynamics_at_the_sequencers_fastest_rate();
    the_gain_also_keeps_growing_with_the_control();
    the_field_control_keeps_working_past_a_third();
    a_note_on_is_not_a_discontinuity_in_the_gain();
    a_note_off_is_not_a_discontinuity_either();
    the_arrival_does_not_shrink_the_gesture();
    a_closed_field_is_untouched();
}
