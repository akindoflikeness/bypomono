/* The note sources: the pitch sequencer, the melody on the clock, and a note
   starting under rip without a click. */
#include "../src/gui/command.h"
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000.0f

/* ---------- the pitch sequencer ---------- */

static PitchSeqParams four_steps(void) {
    PitchSeqParams p = pitch_seq_params_default();
    p.enabled = true;
    p.length = 4;
    p.root_midi = 57; /* A3, 220 hz */
    p.gate_len = 0.5f;
    p.pitch[1] = 12.0f;
    p.pitch[2] = 3.4f;
    p.gate[3] = false;
    p.velocity[1] = 0.25f;
    return p;
}

/* runs s for samples, recording each event and when it came */
typedef struct {
    PitchEvent ev[64];
    size_t at[64];
    int n;
} Heard;

static void run(PitchSeq *s, size_t samples, Heard *h) {
    size_t t = 0;
    while (t < samples) {
        size_t until;
        while (pitch_seq_samples_until(s, &until) && until == 0) {
            PitchEvent e = pitch_seq_fire(s);
            if (e.kind == PITCH_EV_NONE) break;
            if (h->n < 64) {
                h->ev[h->n] = e;
                h->at[h->n] = t;
                h->n++;
            }
        }
        size_t step = samples - t;
        if (pitch_seq_samples_until(s, &until) && until < step) step = until;
        if (step < 1) step = 1;
        pitch_seq_advance(s, step);
        t += step;
    }
}

static void gated_steps_play_on_the_clock(void) {
    PitchSeq s;
    pitch_seq_init(&s, SR);
    pitch_seq_set_tempo(&s, 120.0f);
    pitch_seq_set_params(&s, four_steps());
    size_t step = (size_t)(0.125f * SR); /* 1/16 at 120 */
    Heard h = {0};
    run(&s, step * 4, &h);
    /* on, off, on, off, on, off, then step 4 has no gate and only moves */
    CHECK(h.n == 7, "%d events in a bar of four steps", h.n);
    if (h.n < 7) return;
    CHECK(h.ev[0].kind == PITCH_EV_ON && h.at[0] == 0, "first note late");
    CHECK_NEAR(h.ev[0].hz, 220.0f, 0.01f, "root %g", h.ev[0].hz);
    CHECK(h.ev[1].kind == PITCH_EV_OFF && h.at[1] == step / 2,
          "gate 0.5 let go at %zu", h.at[1]);
    CHECK_NEAR(h.ev[2].hz, 440.0f, 0.02f, "an octave up %g", h.ev[2].hz);
    CHECK(h.ev[2].velocity == 0.25f, "velocity %g", h.ev[2].velocity);
    CHECK(h.at[2] == step, "second step at %zu", h.at[2]);
    CHECK_NEAR(h.ev[4].hz, 220.0f * exp2f(3.4f / 12.0f), 0.02f,
               "fine pitch %g", h.ev[4].hz);
    CHECK(h.ev[6].kind == PITCH_EV_MOVE && h.at[6] == step * 3,
          "a gate-off step should move the pitch on its step");
    CHECK_NEAR(h.ev[6].hz, 220.0f, 0.01f, "the move went to %g", h.ev[6].hz);
}

static void drop(void *u, size_t i, const Frame *f) {
    (void)u;
    (void)i;
    (void)f;
}

static void a_gate_off_step_moves_the_note_without_restarting_it(void) {
    static VoiceBank b;
    voice_bank_init(&b, SR, patch_init(ALGORITHMS[0], RATIO_GOLDEN));
    static Chandas h;
    chandas_init(&h, SR);
    static Mod m;
    mod_init(&m, SR);
    pitch_event_play((PitchEvent){PITCH_EV_ON, 220.0f, 1.0f}, &b, &h, &m);
    voice_bank_render_frames(&b, (size_t)(SR * 0.05f), drop, NULL);
    const Envelope *e = voice_bank_newest_env(&b);
    float t_before = e->t;
    pitch_event_play((PitchEvent){PITCH_EV_MOVE, 330.0f, 0.0f}, &b, &h, &m);
    CHECK(e->t >= t_before, "the move restarted the envelope");
    CHECK_NEAR(voice_bank_target_hz(&b), 330.0f, 1e-3f, "the note did not move");
    chandas_free(&h);
    voice_bank_free(&b);
}

static void snap_rounds_to_12_tet(void) {
    PitchSeqParams p = four_steps();
    p.snap = true;
    CHECK(pitch_seq_semitones(&p, 2) == 3.0f, "3.4 snapped to %g",
          pitch_seq_semitones(&p, 2));
    p.snap = false;
    CHECK(pitch_seq_semitones(&p, 2) == 3.4f, "unsnapped %g",
          pitch_seq_semitones(&p, 2));
    CHECK(!pitch_seq_params_default().snap, "12-tet should be off by default");
}

static void the_length_wraps_and_off_lets_go(void) {
    PitchSeq s;
    pitch_seq_init(&s, SR);
    PitchSeqParams p = four_steps();
    p.length = 2;
    p.gate_len = 1.0f;
    pitch_seq_set_params(&s, p);
    size_t step = (size_t)(0.125f * SR);
    Heard h = {0};
    run(&s, step * 4 + 10, &h);
    int ons = 0;
    for (int i = 0; i < h.n; i++) ons += h.ev[i].kind == PITCH_EV_ON;
    CHECK(ons == 5, "%d notes over four steps of a two-step loop", ons);
    CHECK_NEAR(h.ev[h.n - 1].hz, 220.0f, 0.01f, "wrapped to step 1");
    CHECK(s.held, "a full gate should still be holding");
    p.enabled = false;
    pitch_seq_set_params(&s, p);
    Heard off = {0};
    run(&s, 1, &off);
    CHECK(off.n == 1 && off.ev[0].kind == PITCH_EV_OFF,
          "turning it off left the note hanging");
}

/* ---------- the melody on the clock ---------- */

static void melody_sync_follows_the_tempo(void) {
    Melody m;
    MelodyParams p = melody_params_default();
    p.enabled = true;
    p.sync = true;
    p.division = MELODY_DEFAULT_DIVISION; /* 1/8 */
    melody_init(&m, SR, p);
    melody_set_tempo(&m, 120.0f);
    melody_fire(&m);
    size_t until;
    CHECK(melody_samples_until_fire(&m, &until), "melody not running");
    CHECK(until == (size_t)(0.25f * SR), "1/8 at 120 is %zu samples", until);
    p.sync = false;
    p.rate_hz = 4.0f;
    melody_set_params(&m, p);
    melody_fire(&m);
    melody_samples_until_fire(&m, &until);
    CHECK(until == (size_t)(SR / 4.0f), "free rate %zu", until);
}

/* ---------- presets and the console ---------- */

static void presets_carry_pitch_and_the_melody_clock(void) {
    Session s = session_default();
    s.pitch = four_steps();
    s.pitch.snap = true;
    s.pitch.division = 18;
    s.melody.sync = true;
    s.melody.division = 15;
    char *json = session_to_json(&s);
    Session back;
    CHECK(json && session_from_json(json, &back), "json did not parse");
    free(json);
    CHECK(back.pitch.enabled && back.pitch.snap && back.pitch.length == 4,
          "pitch settings lost");
    CHECK(back.pitch.division == 18 && back.pitch.root_midi == 57,
          "division %d root %d", back.pitch.division, back.pitch.root_midi);
    CHECK(back.pitch.pitch[2] == 3.4f && !back.pitch.gate[3]
              && back.pitch.velocity[1] == 0.25f, "steps lost");
    CHECK(!back.melody.enabled, "pitch on should leave the melody off");
    CHECK(back.melody.sync && back.melody.division == 15, "melody clock lost");

    Session old;
    CHECK(session_from_json("{\"mods\": {\"seqs\": [{\"slot\": 1, \"values\": "
                            "[0.5], \"gates\": [1, 2]}], \"routes\": []}}",
                            &old),
          "an older preset with sequence gates did not load");
    CHECK(old.mods.seq[0].used && !old.pitch.enabled, "old preset read wrong");
}

static App notes_app;

static bool line(const char *text) {
    Command c;
    char err[512];
    if (!parse_line(text, &c, err, sizeof err)) return false;
    return c.kind == CMD_RUN && command_run(&notes_app, &c, err, sizeof err);
}

static void pitch_and_melody_take_turns(void) {
    App *a = &notes_app;
    memset(a, 0, sizeof *a);
    a->shadow_melody = melody_params_default();
    a->shadow_pitch = pitch_seq_params_default();
    CHECK(line("mel on"), "mel on");
    CHECK(line("pitch on step 3 7.5 0.8 off len 12 root C#3 snap on gatelen 0.3"),
          "pitch line refused");
    const PitchSeqParams *p = &a->shadow_pitch;
    CHECK(p->enabled && !a->shadow_melody.enabled, "pitch on left the melody on");
    CHECK(p->pitch[2] == 7.5f && p->velocity[2] == 0.8f && !p->gate[2],
          "step 3 read wrong");
    CHECK(p->length == 12 && p->root_midi == 49 && p->snap && p->gate_len == 0.3f,
          "settings read wrong");
    CHECK(!line("pitch step 17 0"), "step 17 accepted");
    CHECK(!line("pitch step 1 30"), "30 semitones accepted");
    CHECK(line("mel on"), "mel on again");
    CHECK(a->shadow_melody.enabled && !a->shadow_pitch.enabled,
          "mel on left pitch on");
    CHECK(line("mel rate 1/16"), "mel rate 1/16");
    CHECK(a->shadow_melody.sync && a->shadow_melody.division == 21,
          "a division did not put the melody on the clock");
    CHECK(line("mel sync off") && !a->shadow_melody.sync, "mel sync off");
}

/* ---------- a new note under rip ---------- */

/* Rip feeds each voice's carriers its own past through a delay. A voice
   taking a new note used to fill a cleared line in one sample, and the
   carriers' phase jumped a delay later. Poly notes a fifth of a second apart
   the way the melody plays them, judged in 1 ms blocks against the 10 ms
   before: nothing may stand out of its own material. */
static float mono_buf[48000 * 4];
static size_t mono_n;
static void keep(void *u, size_t i, const Frame *f) {
    (void)u;
    (void)i;
    if (mono_n < sizeof mono_buf / sizeof mono_buf[0]) mono_buf[mono_n++] = f->mix;
}

static void a_note_under_rip_starts_without_a_click(void) {
    /* the patch the clicks were first heard on */
    Session s;
    CHECK(session_from_json(
              "{\"patch\": {\"algorithm\": \"PPSP\", \"ratio_mode\": "
              "\"Fibonacci\", \"ops\": [{\"level\": 0.618}, {}, {}, {}, {}], "
              "\"feedback\": 0.3, \"index\": 0.102, \"rip\": 0.18, "
              "\"master_level\": 0.6, \"glide_seconds\": 0, \"field\": 0.55, "
              "\"curve\": 0.74, \"voices\": 4, \"unison\": 2, "
              "\"unison_detune\": 6.8}}",
              &s),
          "patch did not parse");
    VoiceBank b;
    voice_bank_init(&b, SR, s.patch);
    voice_bank_set_adsr_now(&b, (EnvParams){0.044f, 0.3f, 0.37f, 0.43f});
    static const float NOTES[] = {82.4f, 110.0f, 98.0f, 130.8f, 87.3f, 146.8f};
    mono_n = 0;
    for (int k = 0; k < 18; k++) {
        voice_bank_note_off_all(&b);
        voice_bank_note_on(&b, -1, NOTES[k % 6], 1.0f);
        voice_bank_render_frames(&b, 8727, keep, NULL);
    }
    voice_bank_free(&b);

    enum { BLOCK = 48, CONTEXT = 10 };
    size_t nb = mono_n / BLOCK;
    float *peak = calloc(nb, sizeof *peak);
    for (size_t i = 2; i < mono_n; i++) {
        float d2 = fabsf(mono_buf[i] - 2.0f * mono_buf[i - 1] + mono_buf[i - 2]);
        if (i / BLOCK < nb && d2 > peak[i / BLOCK]) peak[i / BLOCK] = d2;
    }
    float worst = 0.0f;
    size_t worst_at = 0;
    for (size_t k = CONTEXT; k < nb; k++) {
        float before = 0.0f;
        for (size_t j = k - CONTEXT; j < k; j++) before = fmaxf(before, peak[j]);
        if (peak[k] < 0.01f) continue;
        float ratio = peak[k] / fmaxf(before, 1e-4f);
        if (ratio > worst) worst = ratio, worst_at = k;
    }
    free(peak);
    CHECK(worst < 4.0f, "a jump %.1fx its surroundings at %.3f s", worst,
          (double)(worst_at * BLOCK) / SR);
}

void test_notes(void) {
    gated_steps_play_on_the_clock();
    a_gate_off_step_moves_the_note_without_restarting_it();
    snap_rounds_to_12_tet();
    the_length_wraps_and_off_lets_go();
    melody_sync_follows_the_tempo();
    presets_carry_pitch_and_the_melody_clock();
    pitch_and_melody_take_turns();
    a_note_under_rip_starts_without_a_click();
}
