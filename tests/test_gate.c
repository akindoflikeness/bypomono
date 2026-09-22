/* The drone is a held gate on the ADSR: engaging it plays note 0 through
   attack and decay and holds it at sustain until it is let go. */
#include "../src/dsp/dsp.h"
#include "test.h"

#define SR 48000.0f

static void drop(void *u, size_t i, const Frame *f) {
    (void)u;
    (void)i;
    (void)f;
}

static const EnvParams ADSR = {0.01f, 0.1f, 0.2f, 0.5f};

static void held_drone(VoiceBank *b, int voices) {
    Patch p = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    p.voices = (uint8_t)voices;
    voice_bank_init(b, SR, p);
    voice_bank_set_adsr_now(b, ADSR);
    voice_bank_drone_to_hz(b, 110.0f);
    voice_bank_set_drone(b, true);
}

static const Envelope *note0(const VoiceBank *b) {
    const VoicePair *p = &b->pairs[0];
    return &p->voice.env;
}

static void the_drone_settles_at_sustain_and_stays(void) {
    static VoiceBank b;
    held_drone(&b, 1);
    voice_bank_render_frames(&b, (size_t)SR, drop, NULL);
    CHECK_NEAR(envelope_level(note0(&b)), ADSR.sustain, 1e-3f, "level %g after a second",
               envelope_level(note0(&b)));
    voice_bank_render_frames(&b, (size_t)(SR * 3.0f), drop, NULL);
    CHECK(note0(&b)->stage == ENV_HELD, "the drone let go by itself");
    CHECK_NEAR(envelope_level(note0(&b)), ADSR.sustain, 1e-3f, "level %g held",
               envelope_level(note0(&b)));
    voice_bank_free(&b);
}

static void a_key_under_the_drone_comes_back_to_sustain(void) {
    static VoiceBank b;
    held_drone(&b, 1);
    voice_bank_render_frames(&b, (size_t)SR, drop, NULL);
    voice_bank_note_on(&b, 64, midi_to_hz(64), 1.0f);
    voice_bank_render_frames(&b, (size_t)(SR * 0.01f), drop, NULL);
    CHECK(envelope_level(note0(&b)) > ADSR.sustain + 0.1f,
          "the key did not fire the attack: %g", envelope_level(note0(&b)));
    voice_bank_note_off(&b, 64);
    CHECK(note0(&b)->stage == ENV_HELD, "letting go of the key released the drone");
    CHECK_NEAR(voice_bank_target_hz(&b), 110.0f, 1e-3f, "the key did not hand the pitch back");
    voice_bank_render_frames(&b, (size_t)SR, drop, NULL);
    CHECK_NEAR(envelope_level(note0(&b)), ADSR.sustain, 1e-3f, "level %g after the key",
               envelope_level(note0(&b)));
    voice_bank_free(&b);
}

static void a_sequencer_note_under_the_drone_keeps_its_pitch(void) {
    static VoiceBank b;
    held_drone(&b, 1);
    voice_bank_note_off_all(&b);
    voice_bank_note_on(&b, -1, 330.0f, 1.0f);
    voice_bank_note_off_all(&b);
    CHECK(note0(&b)->stage == ENV_HELD, "a sequencer's note off released the drone");
    CHECK_NEAR(voice_bank_target_hz(&b), 330.0f, 1e-3f,
               "the pitch went home under a running sequence");
    voice_bank_free(&b);
}

static void letting_the_drone_go_releases_to_silence(void) {
    static VoiceBank b;
    held_drone(&b, 1);
    voice_bank_render_frames(&b, (size_t)SR, drop, NULL);
    voice_bank_set_drone(&b, false);
    CHECK(note0(&b)->stage == ENV_RELEASED, "the drone did not start its release");
    voice_bank_render_frames(&b, (size_t)(SR * (ADSR.release_s + 0.05f)), drop, NULL);
    CHECK(!voice_bank_note_sounding(&b), "still sounding after the release");
    voice_bank_free(&b);
}

static void a_held_key_outlasts_the_drone(void) {
    static VoiceBank b;
    held_drone(&b, 1);
    voice_bank_note_on(&b, 64, midi_to_hz(64), 1.0f);
    voice_bank_set_drone(&b, false);
    CHECK(note0(&b)->stage == ENV_HELD, "dropping the drone cut a held key");
    voice_bank_note_off(&b, 64);
    CHECK(note0(&b)->stage == ENV_RELEASED, "the key's release was held back");
    voice_bank_free(&b);
}

void test_gate(void) {
    the_drone_settles_at_sustain_and_stays();
    a_key_under_the_drone_comes_back_to_sustain();
    a_sequencer_note_under_the_drone_keeps_its_pitch();
    letting_the_drone_go_releases_to_silence();
    a_held_key_outlasts_the_drone();
}
