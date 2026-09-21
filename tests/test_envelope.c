#include "../src/dsp/dsp.h"
#include "test.h"
#include <float.h>

#define SR 48000.0f

typedef struct { float gain; } GainCapture;

static void capture_gain(void *userdata, size_t n, const Frame *frame) {
    ((GainCapture *)userdata)->gain = frame->master;
}

static Envelope held(float secs, const EnvParams *p) {
    Envelope e;
    envelope_init(&e, SR);
    envelope_note_on(&e);
    for (size_t i = 0; i < (size_t)(SR * secs); i++) {
        envelope_tick(&e, p);
    }
    return e;
}

static void the_envelope_is_normalized(void) {
    EnvParams p = env_params_default();
    Envelope e;
    envelope_init(&e, SR);
    envelope_note_on(&e);
    for (size_t i = 0; i < (size_t)(SR * p.attack_s) + 2; i++) {
        envelope_tick(&e, &p);
    }
    CHECK(fabsf(envelope_level(&e) - 1.0f) < 1e-3f, "attack reached %g", envelope_level(&e));
}

static void sustain_is_a_literal_envelope_level(void) {
    EnvParams p = env_params_default();
    p.sustain = 0.37f;
    Envelope e = held(p.attack_s + p.decay_s + 0.1f, &p);
    CHECK(fabsf(envelope_level(&e) - p.sustain) < 1e-4f,
          "sustain %.3f settled at %g", (double)p.sustain, (double)envelope_level(&e));
}

/* the gap left to close, as a fraction, frac of the way through the decay */
static float decay_gap_at(float frac) {
    EnvParams p = env_params_default();
    p.sustain = 0.25f;
    Envelope e = held(p.attack_s + p.decay_s * frac, &p);
    return (envelope_level(&e) - 0.25f) / (1.0f - 0.25f);
}

static void the_decay_falls_evenly_in_db(void) {
    float half = decay_gap_at(0.5f), quarter = decay_gap_at(0.25f);
    CHECK_NEAR(20.0f * log10f(half), -40.0f, 0.3f, "half the decay left %g", half);
    CHECK_NEAR(20.0f * log10f(quarter), -20.0f, 0.3f, "a quarter in left %g",
               quarter);
}

static void a_retrigger_arrives_over_the_attack(void) {
    EnvParams p = env_params_default();
    p.sustain = 0.35f;
    Envelope e = held(p.attack_s + p.decay_s + 0.1f, &p);
    float before = envelope_level(&e);
    envelope_note_on(&e);
    float first = envelope_tick(&e, &p);
    float step = fabsf(envelope_level(&e) - before);
    float allowed = 1.0f / (SR * p.attack_s) + 1e-4f;
    CHECK(step <= allowed, "a harder retrigger stepped %g in one sample (level %g)", step, first);
}

static void a_retrigger_does_not_punch_a_hole(void) {
    EnvParams p = env_params_default();
    Envelope e = held(0.5f, &p);
    float before = envelope_level(&e);
    CHECK(before > 0.5f, "held level %g", before);
    envelope_note_on(&e);
    float lowest = FLT_MAX;
    for (size_t i = 0; i < (size_t)(SR * p.attack_s) + 2; i++) {
        lowest = fminf(lowest, envelope_tick(&e, &p));
    }
    CHECK(lowest >= before - 1e-3f, "retrigger dropped to %g from %g", lowest, before);
}

static void a_released_note_ends(void) {
    EnvParams p = env_params_default();
    Envelope e = held(0.1f, &p);
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
    envelope_note_on(&e);
    for (size_t i = 0; i < (size_t)(SR * p.attack_s * 0.5f); i++) {
        envelope_tick(&e, &p);
    }
    float at_release = envelope_level(&e);
    CHECK(at_release > 0.1f && at_release < 0.9f, "was %g", at_release);
    envelope_note_off(&e);
    float first = envelope_tick(&e, &p);
    CHECK(first <= at_release + 1e-3f, "the release stepped up to %g from %g", first, at_release);
}

static void rendered_release_follows_the_envelope_not_the_master_curve(void) {
    Patch patch = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    patch.master_level = 0.65f;
    Voice voice;
    voice_init(&voice, SR, patch);
    Chain chain = chain_default();
    chain.amp.kind = AMP_ENVELOPE;
    chain.amp.env = env_params_default();
    voice_set_chain(&voice, chain);
    voice_note_on(&voice, 110.0f, 0.5f);

    GainCapture capture = {0};
    voice_render_frames(&voice, (size_t)(SR * 0.1f), capture_gain, &capture);
    float held = capture.gain;
    CHECK_NEAR(held, master_gain(patch.master_level) * 0.5f, 1e-4f,
               "held gain %g compounds envelope and master", (double)held);

    voice_note_off(&voice);
    voice_render_frames(&voice, (size_t)(SR * chain.amp.env.release_s * 0.5f),
                        capture_gain, &capture);
    /* 80 dB over the release, so halfway is 40 dB down */
    CHECK_NEAR(capture.gain / held, 0.01f, 1e-3f,
               "halfway through the release the audible gain is %g",
               (double)(capture.gain / held));
    voice_free(&voice);
}

static void zero_sustain_stays_held(void) {
    EnvParams p = env_params_default();
    p.attack_s = 0.001f;
    p.decay_s = 0.01f;
    p.sustain = 0.4f;
    Envelope e = held(0.1f, &p);
    CHECK(fabsf(envelope_level(&e) - 0.4f) < 1e-4f, "sustain 0.4 settled at %g",
          envelope_level(&e));
    p.sustain = 0.0f;
    e = held(0.1f, &p);
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
        envelope_note_on(&e);
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
    zero_sustain_stays_held();
    the_envelope_is_normalized();
    sustain_is_a_literal_envelope_level();
    the_decay_falls_evenly_in_db();
    a_retrigger_arrives_over_the_attack();
    a_retrigger_does_not_punch_a_hole();
    a_released_note_ends();
    letting_go_early_falls_from_where_it_was();
    rendered_release_follows_the_envelope_not_the_master_curve();
}
