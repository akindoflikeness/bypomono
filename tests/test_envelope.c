#include "../src/dsp/dsp.h"
#include "test.h"
#include <float.h>

#define SR 48000.0f

static Envelope held(float vel, float secs, const EnvParams *p) {
    Envelope e;
    envelope_init(&e, SR);
    envelope_note_on(&e, vel);
    for (size_t i = 0; i < (size_t)(SR * secs); i++) {
        envelope_tick(&e, p);
    }
    return e;
}

static void velocity_is_the_fader_and_the_knob_is_the_last_fifth(void) {
    EnvParams p = env_params_default();
    Envelope e;
    envelope_init(&e, SR);
    envelope_note_on(&e, 1.0f);
    for (size_t i = 0; i < (size_t)(SR * p.attack_s) + 2; i++) {
        envelope_tick(&e, &p);
    }
    CHECK(fabsf(envelope_level(&e) - 1.0f) < 1e-3f, "attack reached %g", envelope_level(&e));

    CHECK(fabsf(envelope_amplitude(&e, 0.0f) - 0.8f) < 1e-4f,
          "max velocity should reach level 0.8, reached %g", envelope_amplitude(&e, 0.0f));
    CHECK(fabsf(envelope_amplitude(&e, 1.0f) - 1.0f) < 1e-4f,
          "the knob at full should add exactly 0.2, reached %g", envelope_amplitude(&e, 1.0f));
    CHECK(fabsf(envelope_amplitude(&e, 1.0f) - envelope_amplitude(&e, 0.0f) - 0.2f) < 1e-4f,
          "knob span is not 0.2");
    CHECK(fabsf(envelope_amplitude(&e, 0.5f) - 0.9f) < 1e-4f, "half knob is not 0.9");

    Envelope silent;
    envelope_init(&silent, SR);
    envelope_note_on(&silent, 0.0f);
    envelope_tick(&silent, &p);
    CHECK(envelope_amplitude(&silent, 1.0f) == 0.0f, "silent amplitude %g",
          envelope_amplitude(&silent, 1.0f));
}

static void velocity_for_level_peaks_on_the_drone_floor(void) {
    EnvParams p = env_params_default();
    const float ms[4] = {0.1f, 0.4995f, 0.8f, 1.0f};
    for (int k = 0; k < 4; k++) {
        float m = ms[k];
        Envelope e;
        envelope_init(&e, SR);
        envelope_note_on(&e, velocity_for_level(m));
        for (size_t i = 0; i < (size_t)(SR * p.attack_s) + 2; i++) {
            envelope_tick(&e, &p);
        }
        CHECK(fabsf(envelope_amplitude(&e, m) - m) < 1e-3f, "at level %g the step peaked at %g", m,
              envelope_amplitude(&e, m));
    }
    CHECK(velocity_for_level(0.0f) == 0.0f, "velocity_for_level(0) is %g",
          velocity_for_level(0.0f));
    CHECK(fabsf(velocity_for_level(1.0f) - 1.0f) < 1e-6f, "velocity_for_level(1) is %g",
          velocity_for_level(1.0f));
}

static void a_soft_note_settles_lower_than_a_hard_one(void) {
    EnvParams p = env_params_default();
    Envelope hard = held(1.0f, p.decay_s * 1.5f, &p);
    Envelope soft = held(0.4f, p.decay_s * 1.5f, &p);
    CHECK(fabsf(envelope_level(&hard) - 1.0f) < 0.02f, "hard settled at %g",
          envelope_level(&hard));
    CHECK(fabsf(envelope_level(&soft) - 0.16f) < 0.02f, "soft settled at %g",
          envelope_level(&soft));
    CHECK(envelope_amplitude(&soft, 0.0f) < envelope_amplitude(&hard, 0.0f) * 0.5f,
          "soft %g not below half of hard %g", envelope_amplitude(&soft, 0.0f),
          envelope_amplitude(&hard, 0.0f));
}

static float decay_shape_at(float curve) {
    EnvParams p = env_params_default();
    p.curve = curve;
    Envelope e = held(0.5f, p.attack_s + p.decay_s * 0.5f, &p);
    return (envelope_level(&e) - 0.25f) / (0.5f - 0.25f);
}

static void the_decay_follows_the_field_s_own_curve_control(void) {
    float log_ = decay_shape_at(0.0f);
    float lin = decay_shape_at(0.5f);
    float exp_ = decay_shape_at(1.0f);
    CHECK(fabsf(lin - 0.5f) < 0.02f, "linear should be halfway, was %g", lin);
    CHECK(log_ > lin, "log %g should hold above linear %g", log_, lin);
    CHECK(exp_ < lin, "exp %g should have plucked below linear %g", exp_, lin);
    CHECK(log_ > 0.7f && log_ < 0.82f, "log at half was %g", log_);
    CHECK(exp_ > 0.12f && exp_ < 0.2f, "exp at half was %g", exp_);
}

static void a_harder_retrigger_arrives_over_the_attack(void) {
    EnvParams p = env_params_default();
    Envelope e = held(0.35f, 0.5f, &p);
    float before = envelope_amplitude(&e, 0.0f);
    envelope_note_on(&e, 1.0f);
    float first = envelope_tick(&e, &p);
    float step = fabsf(envelope_amplitude(&e, 0.0f) - before);
    float allowed = 1.0f / (SR * p.attack_s) + 1e-4f;
    CHECK(step <= allowed, "a harder retrigger stepped %g in one sample (level %g)", step, first);
}

static void a_retrigger_does_not_punch_a_hole(void) {
    EnvParams p = env_params_default();
    Envelope e = held(1.0f, 0.5f, &p);
    float before = envelope_level(&e);
    CHECK(before > 0.5f, "held level %g", before);
    envelope_note_on(&e, 1.0f);
    float lowest = FLT_MAX;
    for (size_t i = 0; i < (size_t)(SR * p.attack_s) + 2; i++) {
        lowest = fminf(lowest, envelope_tick(&e, &p));
    }
    CHECK(lowest >= before - 1e-3f, "retrigger dropped to %g from %g", lowest, before);
}

static void a_released_note_ends(void) {
    EnvParams p = env_params_default();
    Envelope e = held(1.0f, 0.1f, &p);
    envelope_note_off(&e);
    CHECK(envelope_active(&e), "released note should still be active");
    for (size_t i = 0; i < (size_t)(SR * (p.release_s + 0.1f)); i++) {
        envelope_tick(&e, &p);
    }
    CHECK(envelope_level(&e) == 0.0f, "level %g after release", envelope_level(&e));
    CHECK(!envelope_active(&e), "a finished note still says it is sounding");
}

static void letting_go_early_falls_from_where_it_was(void) {
    EnvParams p = env_params_default();
    Envelope e;
    envelope_init(&e, SR);
    envelope_note_on(&e, 1.0f);
    for (size_t i = 0; i < (size_t)(SR * p.attack_s * 0.5f); i++) {
        envelope_tick(&e, &p);
    }
    float at_release = envelope_level(&e);
    CHECK(at_release > 0.1f && at_release < 0.9f, "was %g", at_release);
    envelope_note_off(&e);
    float first = envelope_tick(&e, &p);
    CHECK(first <= at_release + 1e-3f, "the release stepped up to %g from %g", first, at_release);
}

static void sustain_scales_the_velocity_law(void) {
    EnvParams p = env_params_default();
    p.attack_s = 0.001f;
    p.decay_s = 0.01f;
    Envelope e = held(0.5f, 0.1f, &p);
    CHECK(fabsf(envelope_level(&e) - 0.25f) < 1e-4f, "full sustain settled at %g",
          envelope_level(&e));
    p.sustain = 0.4f;
    e = held(0.5f, 0.1f, &p);
    CHECK(fabsf(envelope_level(&e) - 0.1f) < 1e-4f, "sustain 0.4 settled at %g",
          envelope_level(&e));
    p.sustain = 0.0f;
    e = held(0.5f, 0.1f, &p);
    CHECK(envelope_level(&e) < 1e-4f, "zero sustain settled at %g", envelope_level(&e));
    CHECK(envelope_active(&e), "a held note at zero sustain is still held");
}

/* a knob moved under a sounding note must bend the shape from where the
   level already is */
static void moving_a_setting_mid_note_does_not_step_the_level(void) {
    for (int k = 0; k < 4; k++) {
        EnvParams p = env_params_default();
        p.attack_s = 0.05f;
        p.decay_s = 2.0f;
        p.sustain = 0.8f;
        p.release_s = 2.0f;
        Envelope e;
        envelope_init(&e, SR);
        envelope_note_on(&e, 0.9f);
        /* k 3 measures the release, the others the held stages */
        for (size_t i = 0; i < (size_t)(SR * (k == 1 ? 0.02f : 0.4f)); i++)
            envelope_tick(&e, &p);
        if (k == 3) {
            envelope_note_off(&e);
            for (size_t i = 0; i < (size_t)(SR * 0.3f); i++) envelope_tick(&e, &p);
        }
        float before = envelope_level(&e);
        float steady = fabsf(before - envelope_tick(&e, &p));
        float worst = 0.0f;
        for (int step = 1; step <= 40; step++) {
            switch (k) {
            case 0: p.decay_s = 2.0f - 0.045f * (float)step; break;
            case 1: p.attack_s = 0.05f + 0.02f * (float)step; break;
            case 2: p.sustain = 0.8f - 0.02f * (float)step; break;
            default: p.release_s = 2.0f + 0.1f * (float)step; break;
            }
            float was = envelope_level(&e);
            float now = envelope_tick(&e, &p);
            worst = fmaxf(worst, fabsf(now - was));
        }
        CHECK(worst <= steady * 4.0f + 1e-6f,
              "setting %d stepped the level by %g against a steady %g", k, worst, steady);
    }
}

void test_envelope(void) {
    moving_a_setting_mid_note_does_not_step_the_level();
    sustain_scales_the_velocity_law();
    velocity_is_the_fader_and_the_knob_is_the_last_fifth();
    velocity_for_level_peaks_on_the_drone_floor();
    a_soft_note_settles_lower_than_a_hard_one();
    the_decay_follows_the_field_s_own_curve_control();
    a_harder_retrigger_arrives_over_the_attack();
    a_retrigger_does_not_punch_a_hole();
    a_released_note_ends();
    letting_go_early_falls_from_where_it_was();
}
