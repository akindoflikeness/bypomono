#include "../src/dsp/dsp.h"
#include "test.h"

#define SR 48000.0f

static void noop_emit(void *userdata, size_t n, const Frame *frame) {}

static void settled(VoicePair *p, RatioMode mode) {
    voice_pair_init(p, SR, patch_init(ALGORITHMS[0], mode));
    voice_pair_render_frames(p, (size_t)SR, noop_emit, NULL);
}

static void only_the_changes_that_cannot_be_moved_continuously_cross(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    Patch next = *voice_pair_patch(&p);
    next.index = 0.31f;
    next.feedback = 0.62f;
    voice_pair_set_patch(&p, next);
    CHECK(!voice_pair_crossing(&p), "an index move must not start a crossfade");
    for (int i = 0; i < 2; i++) {
        CHECK(p.voices[i].patch.index == 0.31f, "index is %g", p.voices[i].patch.index);
        CHECK(p.voices[i].patch.feedback == 0.62f, "feedback is %g", p.voices[i].patch.feedback);
    }

    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_PLASTIC));
    CHECK(voice_pair_crossing(&p), "a mode change must cross");
    voice_pair_free(&p);
}

static void the_audible_voice_is_never_the_one_rebuilt(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    int was_live = p.target;
    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_HARMONIC));

    CHECK(p.target != was_live, "the crossing must go to the free slot");
    CHECK(p.voices[was_live].patch.ratio_mode == RATIO_GOLDEN,
          "the voice still sounding was rebuilt");
    CHECK(p.voices[p.target].patch.ratio_mode == RATIO_HARMONIC,
          "the incoming voice is not on the new mode");

    voice_pair_render_frames(&p, (size_t)(SR * CROSSFADE_SECONDS * 0.25f), noop_emit, NULL);
    CHECK(voice_pair_crossing(&p), "still crossing");
    CHECK(p.blend < 0.5f, "blend is %g", p.blend);
    int louder = 1 - p.target;
    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_PLASTIC));
    CHECK(p.voices[louder].patch.ratio_mode == RATIO_GOLDEN,
          "a fast second press rebuilt the louder voice");
    voice_pair_free(&p);

    VoicePair q;
    settled(&q, RATIO_GOLDEN);
    voice_pair_set_patch(&q, patch_init(ALGORITHMS[0], RATIO_HARMONIC));
    voice_pair_render_frames(&q, (size_t)(SR * CROSSFADE_SECONDS * 0.8f), noop_emit, NULL);
    CHECK(q.blend > 0.5f, "blend is %g", q.blend);
    int quieter = 1 - q.target;
    voice_pair_set_patch(&q, patch_init(ALGORITHMS[0], RATIO_PLASTIC));
    CHECK(q.target == quieter, "the louder voice was rebuilt");
    voice_pair_free(&q);
}

static void new_envelope_settings_reach_both_voices_without_a_crossfade(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    State tweak = voice_pair_state(&p);
    tweak.adsr.decay_s = 0.5f;
    voice_pair_set_state(&p, tweak);
    CHECK(!voice_pair_crossing(&p), "an envelope's own decay started a crossfade");
    for (int i = 0; i < 2; i++)
        CHECK(p.voices[i].adsr.decay_s == 0.5f, "voice %d kept the old decay", i);
    voice_pair_free(&p);
}

static void both_voices_take_the_note(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    voice_pair_note_on(&p, 220.0f, 0.5f);
    voice_pair_render_frames(&p, 64, noop_emit, NULL);
    for (int i = 0; i < 2; i++) {
        CHECK(fabsf(voice_target_hz(&p.voices[i]) - 220.0f) < 1e-3f,
              "target hz is %g", voice_target_hz(&p.voices[i]));
        CHECK(voice_note_sounding(&p.voices[i]), "voice %d is not sounding", i);
    }
    voice_pair_note_off(&p);
    CHECK(voice_pair_note_sounding(&p), "a release is still sounding");
    voice_pair_free(&p);
}

static void a_crossing_lands_and_leaves_a_clean_understudy(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_HARMONIC));
    voice_pair_render_frames(&p, (size_t)(SR * CROSSFADE_SECONDS * 2.0f), noop_emit, NULL);

    CHECK(!voice_pair_crossing(&p), "the crossing never ended");
    CHECK(p.blend == 0.0f || p.blend == 1.0f,
          "parked at %g rather than on an endpoint", p.blend);
    for (int i = 0; i < 2; i++) {
        CHECK(p.voices[i].patch.ratio_mode == RATIO_HARMONIC,
              "the understudy is stale, so the next change would cross to it");
    }
    voice_pair_free(&p);
}

static void custom_operator_tuning_lands_on_both_voices(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);

    /* A palette name is no longer the tuning itself.  Keep the same name so
       this catches an idle voice that is only checked by ratio_mode. */
    Patch next = *voice_pair_patch(&p);
    next.ops[1].ratio = 2.7182818f;
    next.ops[1].level = 0.37f;
    next.ops[3].ratio = 0.73f;
    next.ops[3].level = 0.61f;
    voice_pair_set_patch(&p, next);
    CHECK(voice_pair_crossing(&p), "a direct ratio change must cross");
    voice_pair_render_frames(&p, (size_t)(SR * CROSSFADE_SECONDS * 2.0f), noop_emit, NULL);

    CHECK(!voice_pair_crossing(&p), "the custom-tuning crossing never ended");
    for (int voice = 0; voice < 2; voice++) {
        for (int op = 0; op < NUM_OPS; op++) {
            CHECK(p.voices[voice].patch.ops[op].ratio == next.ops[op].ratio,
                  "voice %d op %d ratio is stale", voice, op);
            CHECK(p.voices[voice].patch.ops[op].level == next.ops[op].level,
                  "voice %d op %d level is stale", voice, op);
        }
    }
    voice_pair_free(&p);
}

static void the_understudy_is_on_the_same_note(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    voice_pair_glide_to_hz(&p, 220.0f);
    voice_pair_render_frames(&p, (size_t)SR, noop_emit, NULL);
    for (int i = 0; i < 2; i++) {
        CHECK(fabsf(voice_target_hz(&p.voices[i]) - 220.0f) < 1e-3f,
              "a voice was left on %g hz", voice_target_hz(&p.voices[i]));
    }
    voice_pair_free(&p);
}

typedef struct {
    float prev;
    float *acc;
} StepCtx;

static void step_emit(void *userdata, size_t n, const Frame *f) {
    StepCtx *c = userdata;
    *c->acc = fmaxf(*c->acc, fabsf(f->mix - c->prev));
    c->prev = f->mix;
}

static void a_mode_change_introduces_no_step(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    float before = 0.0f;
    StepCtx ctx = { 0.0f, &before };
    voice_pair_render_frames(&p, (size_t)SR / 2, step_emit, &ctx);

    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_GOLDEN_MIRROR));
    float during = 0.0f;
    ctx.acc = &during;
    voice_pair_render_frames(&p, (size_t)(SR * CROSSFADE_SECONDS * 2.0f), step_emit, &ctx);

    CHECK(during <= before * 1.5f,
          "the crossing stepped by %g against a steady %g", during, before);
    voice_pair_free(&p);
}

void test_pair(void) {
    only_the_changes_that_cannot_be_moved_continuously_cross();
    the_audible_voice_is_never_the_one_rebuilt();
    new_envelope_settings_reach_both_voices_without_a_crossfade();
    both_voices_take_the_note();
    a_crossing_lands_and_leaves_a_clean_understudy();
    custom_operator_tuning_lands_on_both_voices();
    the_understudy_is_on_the_same_note();
    a_mode_change_introduces_no_step();
}
