#include "../src/dsp/dsp.h"
#include "test.h"

#include <stdlib.h>

#include <string.h>

#define SR 48000.0f

static void noop_emit(void *userdata, size_t n, const Frame *frame) {}

static Patch voiced(int voices, int unison, float detune) {
    Patch p = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    p.voices = (uint8_t)voices;
    p.unison = (uint8_t)unison;
    p.unison_detune = detune;
    return p;
}

static float midi_hz(int key) {
    return midi_to_hz((uint8_t)key);
}

static bool holds_hz(const VoiceBank *b, float hz) {
    float held[POLY_MAX];
    int n = voice_bank_held_hz(b, held);
    for (int i = 0; i < n; i++)
        if (fabsf(held[i] - hz) < 1e-3f) return true;
    return false;
}

typedef struct {
    Frame *frames;
    size_t at;
} Tape_;

static void tape_emit(void *userdata, size_t n, const Frame *frame) {
    Tape_ *t = userdata;
    t->frames[t->at++] = *frame;
}

static void mono_is_the_pair_it_replaced(void) {
    static VoiceBank b;
    static VoicePair p;
    size_t len = (size_t)(SR / 2);
    Frame *want = calloc(len, sizeof *want);
    Frame *got = calloc(len, sizeof *got);
    Patch patch = patch_init(ALGORITHMS[2], RATIO_GOLDEN);
    voice_pair_init(&p, SR, patch);
    voice_bank_init(&b, SR, patch);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            voice_pair_note_on(&p, 220.0f, 0.7f);
            voice_bank_note_on(&b, 57, 220.0f, 0.7f);
        } else {
            voice_pair_glide_to_hz(&p, 164.0f);
            voice_bank_glide_to_hz(&b, 164.0f);
        }
        Tape_ tw = { want, 0 }, tg = { got, 0 };
        voice_pair_render_frames(&p, len, tape_emit, &tw);
        voice_bank_render_frames(&b, len, tape_emit, &tg);
        size_t differ = 0;
        for (size_t i = 0; i < len; i++)
            if (memcmp(&want[i], &got[i], sizeof want[i]) != 0) differ++;
        CHECK(differ == 0, "pass %d: %zu of %zu mono frames changed", pass, differ, len);
    }
    free(want);
    free(got);
    voice_pair_free(&p);
    voice_bank_free(&b);
}

static void four_notes_sound_together(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(4, 1, 0.0f));
    CHECK(voice_bank_poly(&b) == 4, "poly is %d", voice_bank_poly(&b));
    int keys[4] = { 60, 64, 67, 71 };
    for (int i = 0; i < 4; i++) voice_bank_note_on(&b, keys[i], midi_hz(keys[i]), 0.8f);
    voice_bank_render_frames(&b, 2048, noop_emit, NULL);
    float held[POLY_MAX];
    CHECK(voice_bank_held_hz(&b, held) == 4, "held %d", voice_bank_held_hz(&b, held));
    for (int i = 0; i < 4; i++) {
        CHECK(holds_hz(&b, midi_hz(keys[i])), "key %d is not held", keys[i]);
        CHECK(!voice_pair_silent(&b.pairs[i * UNISON_MAX]), "slot %d is silent", i);
        CHECK(fabsf(voice_target_hz(&b.pairs[i * UNISON_MAX].voice) - midi_hz(keys[i])) < 1e-3f,
              "slot %d glided instead of starting on its note", i);
    }
    voice_bank_free(&b);
}

static void a_fifth_note_takes_the_oldest(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(4, 1, 0.0f));
    int keys[4] = { 60, 64, 67, 71 };
    for (int i = 0; i < 4; i++) {
        voice_bank_note_on(&b, keys[i], midi_hz(keys[i]), 0.8f);
        voice_bank_render_frames(&b, 256, noop_emit, NULL);
    }
    voice_bank_note_on(&b, 74, midi_hz(74), 0.8f);
    CHECK(!holds_hz(&b, midi_hz(60)), "the oldest note survived the steal");
    CHECK(holds_hz(&b, midi_hz(74)), "the new note is not held");
    CHECK(holds_hz(&b, midi_hz(64)) && holds_hz(&b, midi_hz(67)) && holds_hz(&b, midi_hz(71)),
          "a newer note was stolen");
    voice_bank_free(&b);
}

static void only_a_held_poly_reassignment_uses_the_safety_glide(void) {
    static VoiceBank b;
    Patch p = voiced(4, 1, 0.0f);
    p.glide_seconds = 0.0f;
    voice_bank_init(&b, SR, p);
    int keys[4] = {60, 64, 67, 71};
    for (int i = 0; i < 4; i++)
        voice_bank_note_on(&b, keys[i], midi_hz(keys[i]), 0.8f);

    Voice *stolen = &b.pairs[0 * UNISON_MAX].voice;
    float from = stolen->freq;
    float target = midi_hz(74);
    voice_bank_note_on(&b, 74, target, 0.8f);
    CHECK(stolen->steal_glide_seconds > 0.0f,
          "a held note reassignment did not start its safety glide");
    voice_bank_render_frames(&b, 1, noop_emit, NULL);
    CHECK(stolen->freq > from && stolen->freq < target,
          "a held reassignment stepped from %g to %g", (double)from,
          (double)stolen->freq);

    /* A repeated key is a retrigger, not a replacement of another held key. */
    Voice *same_key = &b.pairs[3 * UNISON_MAX].voice;
    float same_target = midi_hz(72);
    voice_bank_note_on(&b, 71, same_target, 0.8f);
    CHECK(same_key->steal_glide_seconds == 0.0f,
          "a same-key retrigger acquired a steal glide");
    voice_bank_render_frames(&b, 1, noop_emit, NULL);
    CHECK_NEAR(same_key->freq, same_target, 1e-3f,
               "a same-key retrigger ignored zero player glide");

    /* A released tail is reusable, but no longer a held voice to steal. */
    Voice *released = &b.pairs[1 * UNISON_MAX].voice;
    voice_bank_note_off(&b, 64);
    float released_target = midi_hz(75);
    voice_bank_note_on(&b, 75, released_target, 0.8f);
    CHECK(released->steal_glide_seconds == 0.0f,
          "a released tail acquired a held-voice steal glide");
    voice_bank_render_frames(&b, 1, noop_emit, NULL);
    CHECK_NEAR(released->freq, released_target, 1e-3f,
               "a released tail ignored zero player glide");

    /* An anonymous event cannot be a same-key retrigger, so it does steal. */
    Voice *anonymous = &b.pairs[2 * UNISON_MAX].voice;
    voice_bank_note_on(&b, -1, midi_hz(76), 0.8f);
    CHECK(anonymous->steal_glide_seconds > 0.0f,
          "an anonymous held reassignment missed its safety glide");
    voice_bank_free(&b);
}

static void a_key_lets_go_of_its_own_note(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(4, 1, 0.0f));
    int keys[3] = { 48, 55, 63 };
    for (int i = 0; i < 3; i++) voice_bank_note_on(&b, keys[i], midi_hz(keys[i]), 0.8f);
    voice_bank_render_frames(&b, 1024, noop_emit, NULL);
    voice_bank_note_off(&b, 55);
    CHECK(holds_hz(&b, midi_hz(48)) && holds_hz(&b, midi_hz(63)), "another key was released");
    CHECK(!holds_hz(&b, midi_hz(55)), "key 55 is still held");
    CHECK(!voice_pair_silent(&b.pairs[1 * UNISON_MAX]), "the release was cut instead of ringing");

    /* the same key again comes back on its own ringing slot */
    voice_bank_note_on(&b, 55, midi_hz(55), 0.8f);
    CHECK(b.key[1] == 55 && b.held[1], "key 55 moved to another slot");

    voice_bank_note_off(&b, -1);
    float held[POLY_MAX];
    CHECK(voice_bank_held_hz(&b, held) == 0, "a wildcard release left notes held");
    voice_bank_render_frames(&b, (size_t)(SR * 3.0f), noop_emit, NULL);
    for (int s = 1; s < BANK_PAIRS; s++)
        CHECK(voice_pair_silent(&b.pairs[s]), "slot %d still sounds after its release", s);
    voice_bank_free(&b);
}

static void mono_lets_go_of_the_sounding_key_only(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(1, 1, 0.0f));
    voice_bank_note_on(&b, 60, midi_hz(60), 0.8f);
    voice_bank_note_on(&b, 62, midi_hz(62), 0.8f);
    voice_bank_note_off(&b, 60);
    float held[POLY_MAX];
    CHECK(voice_bank_held_hz(&b, held) == 1, "mono let go of the key still down");
    CHECK_NEAR(held[0], midi_hz(62), 1e-3f, "mono is holding %g", (double)held[0]);
    voice_bank_note_off(&b, 62);
    CHECK(voice_bank_held_hz(&b, held) == 0, "the sounding key did not release");
    voice_bank_free(&b);
}

/* in poly the drone keeps note 0; keys take the other three and let go
   without touching it */
static void a_held_drone_keeps_its_own_note(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(4, 1, 0.0f));
    voice_bank_drone_to_hz(&b, 55.0f);
    voice_bank_set_drone(&b, true);
    int keys[3] = {60, 64, 67};
    for (int i = 0; i < 3; i++) voice_bank_note_on(&b, keys[i], midi_hz(keys[i]), 0.8f);
    voice_bank_render_frames(&b, 2048, noop_emit, NULL);
    CHECK(b.key[0] == DRONE_KEY && b.held[0], "a key took the drone's note");
    CHECK(holds_hz(&b, 55.0f), "the drone's pitch is not held");
    voice_bank_note_on(&b, 71, midi_hz(71), 0.8f); /* a fourth key steals, not note 0 */
    CHECK(b.key[0] == DRONE_KEY, "a stolen note was the drone's");
    voice_bank_note_off(&b, -1);
    for (int n = 1; n < POLY_MAX; n++) CHECK(!b.held[n], "note %d still held", n);
    CHECK(b.held[0], "letting go of every key let go of the drone");
    voice_bank_set_drone(&b, false);
    CHECK(!b.held[0], "the drone still holds after it was let go");
    voice_bank_free(&b);
}

static void going_mono_lets_the_chord_go(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(4, 1, 0.0f));
    for (int k = 0; k < 3; k++) voice_bank_note_on(&b, 60 + 4 * k, midi_hz(60 + 4 * k), 0.8f);
    voice_bank_render_frames(&b, 1024, noop_emit, NULL);
    voice_bank_set_patch(&b, voiced(1, 1, 0.0f));
    CHECK(voice_bank_poly(&b) == 1, "poly is %d", voice_bank_poly(&b));
    for (int n = 1; n < POLY_MAX; n++) CHECK(!b.held[n], "note %d still held", n);
    /* the gate is a one-pole, so silence is reached rather than stepped to */
    voice_bank_render_frames(&b, (size_t)(SR * 0.2f), noop_emit, NULL);
    for (int s = 2; s < BANK_PAIRS; s++) CHECK(b.gain[s] == 0.0f, "slot %d at gain %g", s, (double)b.gain[s]);
    voice_bank_free(&b);
}

typedef struct {
    float peak_side;
    float prev_l, prev_r;
    float step;
    bool started;
} Stereo_;

static void stereo_emit(void *userdata, size_t n, const Frame *f) {
    Stereo_ *c = userdata;
    float l = f->mix + f->side, r = f->mix - f->side;
    c->peak_side = fmaxf(c->peak_side, fabsf(f->side));
    if (c->started) c->step = fmaxf(c->step, fmaxf(fabsf(l - c->prev_l), fabsf(r - c->prev_r)));
    c->prev_l = l;
    c->prev_r = r;
    c->started = true;
}

static void unison_detunes_and_spreads_the_pair(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(1, 2, 20.0f));
    voice_bank_set_drone(&b, true);
    CHECK_NEAR(b.pairs[0].voice.detune_to, exp2f(-10.0f / 1200.0f), 1e-6f, "copy 0 detune %g",
               (double)b.pairs[0].voice.detune_to);
    CHECK_NEAR(b.pairs[1].voice.detune_to, exp2f(10.0f / 1200.0f), 1e-6f, "copy 1 detune %g",
               (double)b.pairs[1].voice.detune_to);
    Stereo_ ctx = { 0 };
    voice_bank_render_frames(&b, (size_t)(SR / 4), stereo_emit, &ctx);
    CHECK(ctx.peak_side > 1e-4f, "unison stayed in the middle (side %g)", (double)ctx.peak_side);

    voice_bank_set_patch(&b, voiced(1, 1, 20.0f));
    CHECK(b.pairs[0].voice.detune_to == 1.0f, "a lone voice kept its detune");
    voice_bank_render_frames(&b, (size_t)(SR / 4), noop_emit, NULL);
    CHECK(b.spread == 0.0f && b.gain[1] == 0.0f, "unison never faded out");
    voice_bank_free(&b);
}

static void switching_unison_introduces_no_step(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(1, 1, 12.0f));
    voice_bank_set_drone(&b, true);
    voice_bank_render_frames(&b, (size_t)SR, noop_emit, NULL);
    Stereo_ steady = { 0 };
    voice_bank_render_frames(&b, (size_t)(SR / 2), stereo_emit, &steady);

    voice_bank_set_patch(&b, voiced(1, 2, 12.0f));
    Stereo_ on = steady;
    on.step = 0.0f;
    voice_bank_render_frames(&b, (size_t)(SR * 0.05f), stereo_emit, &on);
    CHECK(on.step <= steady.step * 1.5f, "unison on stepped by %g against a steady %g",
          (double)on.step, (double)steady.step);

    voice_bank_set_patch(&b, voiced(1, 1, 12.0f));
    Stereo_ off = on;
    off.step = 0.0f;
    voice_bank_render_frames(&b, (size_t)(SR * 0.05f), stereo_emit, &off);
    CHECK(off.step <= steady.step * 1.5f, "unison off stepped by %g against a steady %g",
          (double)off.step, (double)steady.step);
    voice_bank_free(&b);
}

static void a_returning_copy_is_on_the_note(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, voiced(1, 1, 8.0f));
    voice_bank_set_drone(&b, true);
    voice_bank_glide_to_hz(&b, 330.0f);
    voice_bank_render_frames(&b, (size_t)SR, noop_emit, NULL);
    voice_bank_set_patch(&b, voiced(1, 2, 8.0f));
    voice_bank_render_frames(&b, 64, noop_emit, NULL);
    CHECK(fabsf(b.pairs[1].voice.freq - 330.0f) < 1.0f, "the second copy came back at %g hz",
          (double)b.pairs[1].voice.freq);
    voice_bank_free(&b);
}

typedef struct {
    float h1, h2;
    int seen;
    float worst;
} Curve;

/* a step in a control shows up as curvature, which a step in the waveform
   itself does not reach */
static void curve_emit(void *userdata, size_t n, const Frame *f) {
    Curve *c = userdata;
    float x = f->mix + f->side;
    if (c->seen >= 2) c->worst = fmaxf(c->worst, fabsf(x - 2.0f * c->h1 + c->h2));
    c->h2 = c->h1;
    c->h1 = x;
    c->seen++;
}

static void moving_rip_does_not_step_the_carriers(void) {
    static VoiceBank b;
    Patch p = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    p.index = 0.0f; /* plain carriers, so a step has nothing to hide behind */
    p.rip = 0.0f;
    voice_bank_init(&b, SR, p);
    voice_bank_drone_to_hz(&b, 110.0f);
    voice_bank_set_drone(&b, true);
    voice_bank_render_frames(&b, (size_t)SR, noop_emit, NULL);
    Curve steady = {0};
    voice_bank_render_frames(&b, (size_t)(SR / 4), curve_emit, &steady);

    p.rip = 0.5f;
    voice_bank_set_patch(&b, p);
    Curve moved = {0};
    voice_bank_render_frames(&b, (size_t)(SR / 4), curve_emit, &moved);
    CHECK(moved.worst <= steady.worst * 20.0f,
          "a rip step bent the carriers by %g against a steady %g", moved.worst,
          steady.worst);
    voice_bank_free(&b);
}

void test_bank(void) {
    moving_rip_does_not_step_the_carriers();
    mono_is_the_pair_it_replaced();
    four_notes_sound_together();
    a_fifth_note_takes_the_oldest();
    only_a_held_poly_reassignment_uses_the_safety_glide();
    a_key_lets_go_of_its_own_note();
    mono_lets_go_of_the_sounding_key_only();
    a_held_drone_keeps_its_own_note();
    going_mono_lets_the_chord_go();
    unison_detunes_and_spreads_the_pair();
    switching_unison_introduces_no_step();
    a_returning_copy_is_on_the_note();
}
