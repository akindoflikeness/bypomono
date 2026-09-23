#include "../src/dsp/dsp.h"
#include "test.h"
#include "walk.h"

#define SR 48000.0f

static void settled(VoicePair *p, RatioMode mode) {
    voice_pair_init(p, SR, patch_init(ALGORITHMS[0], mode));
    pair_skip(p, (size_t)SR);
}

static void held(VoicePair *p, RatioMode mode) {
    settled(p, mode);
    voice_pair_note_on(p, 110.0f, 1.0f);
    pair_skip(p, (size_t)(SR * 0.05f));
}

static void only_the_changes_that_cannot_be_moved_continuously_dip(void) {
    VoicePair p;
    held(&p, RATIO_GOLDEN);
    Patch next = *voice_pair_patch(&p);
    next.index = 0.31f;
    next.feedback = 0.62f;
    voice_pair_set_patch(&p, next);
    CHECK(!voice_pair_crossing(&p), "an index move must not dip");
    CHECK(p.voice.patch.index == 0.31f, "index is %g", p.voice.patch.index);
    CHECK(p.voice.patch.feedback == 0.62f, "feedback is %g", p.voice.patch.feedback);

    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_PLASTIC));
    CHECK(voice_pair_crossing(&p), "a mode change must dip");
    voice_pair_free(&p);
}

static void the_sounding_shape_holds_until_silence(void) {
    VoicePair p;
    held(&p, RATIO_GOLDEN);
    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_HARMONIC));

    CHECK(voice_pair_patch(&p)->ratio_mode == RATIO_HARMONIC,
          "the requested mode is not what the pair reports");
    CHECK(p.voice.patch.ratio_mode == RATIO_GOLDEN,
          "the sounding voice took the new mode before silence");

    pair_skip(&p, (size_t)(SR * STRUCT_DIP_SECONDS * 0.25f));
    CHECK(voice_pair_crossing(&p), "still dipping");
    CHECK(p.dip > 0.5f, "a quarter of the way down, dip is %g", p.dip);
    CHECK(p.voice.patch.ratio_mode == RATIO_GOLDEN, "the old mode left early");

    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_PLASTIC));
    CHECK(p.voice.patch.ratio_mode == RATIO_GOLDEN,
          "a second press landed on the voice that is still up");
    CHECK(voice_pair_patch(&p)->ratio_mode == RATIO_PLASTIC,
          "the second press did not replace the request");
    voice_pair_free(&p);
}

static void new_envelope_settings_reach_the_voice_without_a_dip(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    State tweak = voice_pair_state(&p);
    tweak.adsr.decay_s = 0.5f;
    voice_pair_set_state(&p, tweak);
    CHECK(!voice_pair_crossing(&p), "an envelope's own decay started a dip");
    CHECK(p.voice.adsr.decay_s == 0.5f, "the voice kept the old decay");
    voice_pair_free(&p);
}

static void the_voice_takes_the_note(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    voice_pair_note_on(&p, 220.0f, 0.5f);
    pair_skip(&p, 64);
    CHECK(fabsf(voice_target_hz(&p.voice) - 220.0f) < 1e-3f,
          "target hz is %g", voice_target_hz(&p.voice));
    CHECK(voice_note_sounding(&p.voice), "the voice is not sounding");
    voice_pair_note_off(&p);
    CHECK(voice_pair_note_sounding(&p), "a release is still sounding");
    voice_pair_free(&p);
}

static void a_dip_lands_on_the_new_shape(void) {
    VoicePair p;
    held(&p, RATIO_GOLDEN);
    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_HARMONIC));
    pair_skip(&p, (size_t)(SR * STRUCT_DIP_SECONDS * 2.0f) + 8);

    CHECK(!voice_pair_crossing(&p), "the dip never ended");
    CHECK(p.dip == 1.0f, "parked at %g rather than full level", p.dip);
    CHECK(p.voice.patch.ratio_mode == RATIO_HARMONIC, "the new mode never landed");
    voice_pair_free(&p);
}

static void a_ratio_glides_on_the_sounding_voice(void) {
    VoicePair p;
    held(&p, RATIO_GOLDEN);

    Patch next = *voice_pair_patch(&p);
    next.ops[1].ratio = 2.7182818f;
    next.ops[1].level = 0.37f;
    next.ops[3].ratio = 0.73f;
    next.ops[3].level = 0.61f;
    float was = p.voice.ratio_s[1];
    voice_pair_set_patch(&p, next);
    CHECK(!voice_pair_crossing(&p), "a direct ratio change dipped");
    CHECK(p.voice.patch.ops[1].ratio == next.ops[1].ratio, "the ratio was not stored");
    CHECK(p.voice.ratio_s[1] == was, "the sounding ratio jumped");

    pair_skip(&p, (size_t)(SR * 0.2f));
    CHECK(p.voice.ratio_s[1] != was, "the ratio never moved");
    CHECK(fabsf(p.voice.ratio_s[1] - next.ops[1].ratio) < 1e-3f,
          "the ratio settled at %g", p.voice.ratio_s[1]);
    CHECK(p.voice.patch.ops[1].level == next.ops[1].level, "the level was not stored");
    voice_pair_free(&p);
}

static void the_voice_keeps_its_note(void) {
    VoicePair p;
    settled(&p, RATIO_GOLDEN);
    voice_pair_glide_to_hz(&p, 220.0f);
    pair_skip(&p, (size_t)SR);
    CHECK(fabsf(voice_target_hz(&p.voice) - 220.0f) < 1e-3f,
          "the voice was left on %g hz", voice_target_hz(&p.voice));
    voice_pair_free(&p);
}

typedef struct {
    float prev;
    float *acc;
} StepCtx;

static void step_emit(void *userdata, const Frame *f) {
    StepCtx *c = userdata;
    *c->acc = fmaxf(*c->acc, fabsf(f->mix - c->prev));
    c->prev = f->mix;
}

static void a_mode_change_introduces_no_step(void) {
    VoicePair p;
    held(&p, RATIO_GOLDEN);
    float before = 0.0f;
    StepCtx ctx = { 0.0f, &before };
    pair_each(&p, (size_t)SR / 2, step_emit, &ctx);

    voice_pair_set_patch(&p, patch_init(ALGORITHMS[0], RATIO_GOLDEN_MIRROR));
    float during = 0.0f;
    ctx.acc = &during;
    pair_each(&p, (size_t)(SR * STRUCT_DIP_SECONDS * 2.0f) + 8, step_emit, &ctx);

    CHECK(during <= before * 1.5f,
          "the dip stepped by %g against a steady %g", during, before);
    voice_pair_free(&p);
}

void test_pair(void) {
    only_the_changes_that_cannot_be_moved_continuously_dip();
    the_sounding_shape_holds_until_silence();
    new_envelope_settings_reach_the_voice_without_a_dip();
    the_voice_takes_the_note();
    a_dip_lands_on_the_new_shape();
    a_ratio_glides_on_the_sounding_voice();
    the_voice_keeps_its_note();
    a_mode_change_introduces_no_step();
}
