/* **Every control this instrument has, measured across the moment it moves.**
   
   This file exists because the click tests never came over from the Rust
   build: `tests/no_clicks.rs` and the `seam` detector it leans on stayed
   behind, and the faults their ledger recorded in September came over instead.
   Tests that assert a destination cannot see a click, because a click lives in
   the path — so this one enumerates the controls and measures the join.
   
   `move_reach` is a switch with no default arm: a control added to the
   instrument stops this file compiling cleanly until someone has said how it
   reaches the output. The compiler asks the question. */
#include "../src/dsp/dsp.h"
#include "click.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>

#define SR 48000.0f
#define HZ 110.0f

/* BYPO_CLICKS=1 prints every measurement, not only the ones that fail */
static bool loud(void) {
    const char *v = getenv("BYPO_CLICKS");
    return v && *v && *v != '0';
}

/* ---------- how a control reaches the speaker ---------- */

typedef enum { REACH_GAIN, REACH_PHASE, REACH_STRUCTURE, REACH_TIMING } Reach;

typedef enum {
    MV_INDEX, MV_RIP, MV_FB, MV_LEVEL, MV_GLIDE, MV_DETUNE,
    MV_VOICES, MV_UNISON, MV_ALGORITHM, MV_RATIO_MODE, MV_OP_OFF, MV_OP_ON,
    MV_OP_LEVEL, MV_DRONE_HZ, MV_BEND, MV_NOTE_ON, MV_NOTE_OFF, MV_STEAL,
    MV_DRONE_OFF, MV_DRONE_ON, MV_ATTACK, MV_DECAY, MV_SUSTAIN, MV_RELEASE,
    MV_VERB_MIX, MV_VERB_GHOST, MV_VERB_DECAY, MV_VERB_DAMP, MV_VERB_HAUNT,
    MV_CHANDAS_ON, MV_CHANDAS_MIX, MV_CHANDAS_DIV, MV_PRESET,
    MV_REPEAT_IN_FADE, MV_COUNT
} Move;

/* No default arm, on purpose: a new control has to be classified here. */
static Reach move_reach(Move m) {
    switch (m) {
    case MV_LEVEL: case MV_OP_LEVEL: case MV_VERB_MIX: case MV_VERB_GHOST:
    case MV_VERB_DECAY: case MV_VERB_DAMP: case MV_VERB_HAUNT:
    case MV_CHANDAS_MIX: case MV_ATTACK: case MV_DECAY: case MV_SUSTAIN:
    case MV_RELEASE: case MV_OP_OFF: case MV_OP_ON: case MV_NOTE_ON:
    case MV_NOTE_OFF: case MV_STEAL: case MV_CHANDAS_ON: case MV_DRONE_OFF:
    case MV_DRONE_ON:
        return REACH_GAIN;
    case MV_INDEX: case MV_RIP: case MV_FB:
    case MV_GLIDE: case MV_DETUNE: case MV_DRONE_HZ: case MV_BEND:
        return REACH_PHASE;
    case MV_VOICES: case MV_UNISON: case MV_ALGORITHM: case MV_RATIO_MODE:
    case MV_PRESET: case MV_REPEAT_IN_FADE:
        return REACH_STRUCTURE;
    case MV_CHANDAS_DIV:
        return REACH_TIMING;
    case MV_COUNT:
        break;
    }
    return REACH_GAIN;
}

static const char *move_name(Move m) {
    switch (m) {
    case MV_INDEX: return "index";
    case MV_RIP: return "rip";
    case MV_FB: return "fb";
    case MV_LEVEL: return "level";
    case MV_GLIDE: return "glide";
    case MV_DETUNE: return "detune";
    case MV_VOICES: return "voices";
    case MV_UNISON: return "unison";
    case MV_ALGORITHM: return "algorithm";
    case MV_RATIO_MODE: return "ratio mode";
    case MV_OP_OFF: return "op off";
    case MV_OP_ON: return "op on";
    case MV_OP_LEVEL: return "op level";
    case MV_DRONE_HZ: return "drone hz";
    case MV_BEND: return "pitch bend";
    case MV_NOTE_ON: return "note on";
    case MV_NOTE_OFF: return "note off";
    case MV_STEAL: return "a fifth note steals";
    case MV_DRONE_OFF: return "drone let go";
    case MV_DRONE_ON: return "drone held";
    case MV_ATTACK: return "attack";
    case MV_DECAY: return "decay";
    case MV_SUSTAIN: return "sustain";
    case MV_RELEASE: return "release";
    case MV_VERB_MIX: return "room mix";
    case MV_VERB_GHOST: return "room ghost";
    case MV_VERB_DECAY: return "room decay";
    case MV_VERB_DAMP: return "room damp";
    case MV_VERB_HAUNT: return "room haunt";
    case MV_CHANDAS_ON: return "chandas on";
    case MV_CHANDAS_MIX: return "chandas mix";
    case MV_CHANDAS_DIV: return "chandas division";
    case MV_PRESET: return "a preset lands";
    case MV_REPEAT_IN_FADE: return "the same patch again inside the crossfade";
    case MV_COUNT: break;
    }
    return "?";
}

/* ---------- the instrument, wrapped so seam can render it ---------- */

typedef struct {
    VoiceBank bank;
    StereoVerb verb;
    Chandas chandas;
    Melody melody;
    bool melody_on, built;
    size_t repeat_in; /* samples until a second, identical patch message */
    Patch repeat;
    int poly, unison;
    bool notes, shun;
    Move move;
} Rig;

static EnvParams notes_adsr(void) {
    EnvParams e = env_params_default();
    e.attack_s = ENV_ATTACK_MIN; /* the worst case for a gate */
    return e;
}

static Patch plain_patch(int poly, int unison) {
    Patch p = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    p.voices = (uint8_t)poly;
    p.unison = (uint8_t)unison;
    p.index = 0.6f;
    return p;
}

/* the shape of a patch a player actually uses: everything on at once */
static Patch shun_patch(void) {
    AlgorithmId id = ALGORITHMS[0];
    algorithm_from_glyphs("PPSP", &id);
    Patch p = patch_init(id, RATIO_FIBONACCI);
    p.feedback = 0.3f;
    p.index = 0.102f;
    p.rip = 0.182f;
    p.master_level = 0.6f;
    p.glide_seconds = 6.7e-6f;
    p.voices = POLY_MAX;
    p.unison = UNISON_MAX;
    p.unison_detune = 6.8f;
    return p;
}

static void rig_make(void *ctx) {
    Rig *r = ctx;
    Patch p = r->shun ? shun_patch() : plain_patch(r->poly, r->unison);
    /* a move needs something to move: switching an operator on means starting
       with it off */
    if (r->move == MV_OP_ON) p.ops[0].enabled = false;
    voice_bank_init(&r->bank, SR, p);
    verb_init(&r->verb, SR);
    verb_configure(&r->verb, voice_bank_patch(&r->bank), voice_bank_compiled(&r->bank));
    verb_set_params(&r->verb, verb_params_default());
    chandas_init(&r->chandas, SR);
    MelodyParams m = melody_params_default();
    m.enabled = r->shun;
    m.rate_hz = 8.0f; /* gates often enough to land inside the window */
    m.root_midi = 40;
    melody_init(&r->melody, SR, m);
    r->melody_on = m.enabled;
    voice_bank_drone_to_hz(&r->bank, HZ);
    voice_bank_set_drone_hz(&r->bank, HZ);
    verb_set_drone_hz(&r->verb, HZ);
    if (r->shun) {
        VerbParams v = {0.2f, 0.202f, 1.24f, 0.228f, 0.612f};
        verb_set_params(&r->verb, v);
        ChandasParams c = chandas_params_default();
        c.enabled = true;
        c.mix = 0.396f;
        c.division = 10;
        c.spread = 0.38f;
        c.size = 0.79f;
        c.warp = 0.796f;
        c.tail = 0.252f;
        chandas_set_params(&r->chandas, c);
    }
    if (r->notes) {
        EnvParams e = notes_adsr();
        if (r->shun) e = (EnvParams){0.0436f, 0.296f, 0.368f, 0.427f};
        voice_bank_set_adsr_now(&r->bank, e);
        if (!r->melody_on) voice_bank_note_on(&r->bank, 45, HZ, 0.9f);
    } else {
        voice_bank_set_drone(&r->bank, true);
    }
    r->built = true;
}

static void rig_teardown(void *ctx) {
    Rig *r = ctx;
    if (!r->built) return;
    voice_bank_free(&r->bank);
    verb_free(&r->verb);
    chandas_free(&r->chandas);
    r->built = false;
}

static void patch_to(Rig *r, Patch p) {
    voice_bank_set_patch(&r->bank, p);
    verb_configure(&r->verb, voice_bank_patch(&r->bank), voice_bank_compiled(&r->bank));
}

static void rig_render(void *ctx, float *out, size_t n) {
    Rig *r = ctx;
    size_t at = 0;
    size_t done = 0;
    while (done < n) {
        size_t run = n - done;
        if (r->melody_on) {
            size_t until;
            if (melody_samples_until_fire(&r->melody, &until) && until == 0) {
                float hz = melody_fire(&r->melody);
                voice_bank_note_off_all(&r->bank);
                voice_bank_note_on(&r->bank, -1, hz, 1.0f);
                chandas_note_pulse(&r->chandas);
            }
            if (melody_samples_until_fire(&r->melody, &until) && until < run)
                run = until < 1 ? 1 : until;
        }
        /* a scheduled second message, counted in rendered samples so both
           runs of the counterfactual keep the same clock */
        if (r->repeat_in > 0 && r->repeat_in <= run) {
            run = r->repeat_in;
        }
        Frame chunk[128];
        size_t left = run;
        while (left) {
            size_t k = left < 128 ? left : 128;
            voice_bank_render_block(&r->bank, chunk, k);
            for (size_t i = 0; i < k; i++) {
                Stereo s = verb_process(&r->verb, &chunk[i]);
                s = chandas_process(&r->chandas, s);
                out[at++] = s.l;
            }
            left -= k;
        }
        if (r->repeat_in > 0) {
            r->repeat_in -= run;
            if (r->repeat_in == 0) patch_to(r, r->repeat);
        }
        if (r->melody_on) melody_advance(&r->melody, run);
        done += run;
    }
}

static void rig_apply(void *ctx) {
    Rig *r = ctx;
    Patch p = *voice_bank_patch(&r->bank);
    VerbParams v = verb_params(&r->verb);
    ChandasParams c = chandas_params(&r->chandas);
    switch (r->move) {
    case MV_INDEX: p.index = 0.9f; patch_to(r, p); break;
    case MV_RIP: p.rip = 0.7f; patch_to(r, p); break;
    case MV_FB: p.feedback = 0.9f; patch_to(r, p); break;
    case MV_LEVEL: p.master_level = 0.3f; patch_to(r, p); break;
    case MV_GLIDE: p.glide_seconds = 1.5f; patch_to(r, p); break;
    case MV_DETUNE: p.unison_detune = 40.0f; patch_to(r, p); break;
    case MV_VOICES: p.voices = p.voices > 1 ? 1 : POLY_MAX; patch_to(r, p); break;
    case MV_UNISON: p.unison = p.unison > 1 ? 1 : UNISON_MAX; patch_to(r, p); break;
    case MV_ALGORITHM: {
        Patch n = patch_init(ALGORITHMS[4], p.ratio_mode);
        n.index = p.index; n.voices = p.voices; n.unison = p.unison;
        patch_to(r, n);
        break;
    }
    case MV_RATIO_MODE: {
        Patch n = patch_init(p.algorithm, RATIO_PLASTIC);
        n.index = p.index; n.voices = p.voices; n.unison = p.unison;
        patch_to(r, n);
        break;
    }
    case MV_OP_OFF: p.ops[0].enabled = false; patch_to(r, p); break;
    case MV_OP_ON: p.ops[0].enabled = true; patch_to(r, p); break;
    case MV_OP_LEVEL: p.ops[0].level *= 0.4f; patch_to(r, p); break;
    case MV_DRONE_HZ:
        voice_bank_drone_to_hz(&r->bank, 220.0f);
        voice_bank_set_drone_hz(&r->bank, 220.0f);
        verb_set_drone_hz(&r->verb, 220.0f);
        break;
    case MV_BEND: voice_bank_set_bend_semitones(&r->bank, 2.0f); break;
    case MV_NOTE_ON: voice_bank_note_on(&r->bank, 52, midi_to_hz(52), 0.9f); break;
    case MV_NOTE_OFF: voice_bank_note_off(&r->bank, -1); break;
    case MV_STEAL:
        for (int i = 0; i < POLY_MAX + 1; i++)
            voice_bank_note_on(&r->bank, 48 + 3 * i, midi_to_hz(48 + 3 * i), 0.9f);
        break;
    case MV_DRONE_OFF: voice_bank_set_drone(&r->bank, false); break;
    case MV_DRONE_ON: voice_bank_set_drone(&r->bank, true); break;
    case MV_ATTACK:
    case MV_DECAY:
    case MV_SUSTAIN:
    case MV_RELEASE: {
        EnvParams e = *voice_bank_adsr(&r->bank);
        if (r->move == MV_ATTACK) e.attack_s = 2.0f;
        if (r->move == MV_DECAY) e.decay_s = 0.05f;
        if (r->move == MV_SUSTAIN) e.sustain = 0.1f;
        if (r->move == MV_RELEASE) e.release_s = 4.0f;
        voice_bank_set_adsr_now(&r->bank, e);
        break;
    }
    case MV_VERB_MIX: v.mix = 0.9f; verb_set_params(&r->verb, v); break;
    case MV_VERB_GHOST: v.ghost = 0.9f; verb_set_params(&r->verb, v); break;
    case MV_VERB_DECAY: v.decay = 0.3f; verb_set_params(&r->verb, v); break;
    case MV_VERB_DAMP: v.damp = 0.95f; verb_set_params(&r->verb, v); break;
    case MV_VERB_HAUNT: v.haunt = 0.9f; verb_set_params(&r->verb, v); break;
    case MV_CHANDAS_ON:
        c.enabled = !c.enabled;
        c.mix = 0.6f;
        chandas_set_params(&r->chandas, c);
        break;
    case MV_CHANDAS_MIX: c.enabled = true; c.mix = 0.9f; chandas_set_params(&r->chandas, c); break;
    case MV_CHANDAS_DIV:
        c.enabled = true;
        c.mix = 0.5f;
        c.division = 6;
        chandas_set_params(&r->chandas, c);
        break;
    case MV_PRESET: {
        Patch n = patch_init(ALGORITHMS[6], RATIO_FIBONACCI);
        n.index = 0.4f; n.rip = 0.35f; n.feedback = 0.2f;
        n.voices = p.voices; n.unison = p.unison;
        patch_to(r, n);
        break;
    }
    case MV_REPEAT_IN_FADE: {
        /* a host echoing its parameters, or a second click: the second one
           lands while the silence dip from the first is still running */
        Patch n = patch_init(ALGORITHMS[4], p.ratio_mode);
        n.index = p.index; n.voices = p.voices; n.unison = p.unison;
        patch_to(r, n);
        r->repeat = n;
        r->repeat_in = 256;
        break;
    }
    case MV_COUNT: break;
    }
}

/* ---------- the voicings the moves are measured in ---------- */

typedef struct {
    const char *name;
    int poly, unison;
    bool notes, shun;
} Voicing;

static const Voicing VOICINGS[] = {
    {"drone", 1, 1, false, false},
    {"shun", POLY_MAX, UNISON_MAX, true, true},
};
#define NVOICINGS ((int)(sizeof VOICINGS / sizeof VOICINGS[0]))

static Seam measure(Move m, const Voicing *vo) {
    static Rig rig;
    memset(&rig, 0, sizeof rig);
    rig.poly = vo->poly;
    rig.unison = vo->unison;
    rig.notes = vo->notes;
    rig.shun = vo->shun;
    rig.move = m;
    SeamProbe probe = {&rig, rig_make, rig_render, rig_apply, rig_teardown};
    return seam_check(seam_defaults(SR), probe);
}

/* **What is known to click today**, with the number it measured at and why.
   A ledger, not an excuse: anything not on it is held to SEAM_AUDIBLE_DB, and
   anything on it is held to its number in both directions — a listed move that
   gets better fails too, and the fix is to delete its row. */
typedef struct {
    Move move;
    float db;
    const char *why;
} Known;

static const Known KNOWN[] = {
    {MV_COUNT, 0.0f, NULL}, /* nothing is accepted: the list is empty */
};

static const Known *known(Move m) {
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (KNOWN[i].move == m && KNOWN[i].why) return &KNOWN[i];
    return NULL;
}

static void say(Move m, const Voicing *vo, const Seam *s) {
    if (!loud()) return;
    if (s->kind != SEAM_MEASURED) {
        printf("   %-8s %-42s  %s\n", vo->name, move_name(m), seam_why(s->kind));
    } else if (!s->moved) {
        printf("   %-8s %-42s  no effect within the window\n", vo->name, move_name(m));
    } else {
        static const char *const REACH[] = {"gain", "phase", "structure", "timing"};
        printf("   %-8s %-42s %-9s seam %6.1f dB (motion %6.1f, excess %+6.1f%s)\n", vo->name,
               move_name(m), REACH[move_reach(m)], (double)s->seam_db, (double)s->motion_db,
               (double)s->excess_db, s->deferred_by ? ", late" : "");
    }
}

/* filled in by the sweep, read by the completeness check */
static bool reached_the_sound[MV_COUNT];
static bool swept;

/* **No control clicks**, except the ones written down. The point of the file. */
static void no_control_clicks(void) {
    swept = true;
    if (loud()) printf("   %-8s %-42s  %s\n", "voicing", "move", "seam");
    for (int v = 0; v < NVOICINGS; v++) {
        for (Move m = 0; m < MV_COUNT; m++) {
            if (known(m)) continue;
            Seam s = measure(m, &VOICINGS[v]);
            if (s.kind == SEAM_MEASURED && s.moved) reached_the_sound[m] = true;
            say(m, &VOICINGS[v], &s);
            /* a move that cannot be measured is a hole in the coverage, and
               holes are what this file is for */
            CHECK(s.kind == SEAM_MEASURED, "%s: %s was not measured - %s", VOICINGS[v].name,
                  move_name(m), seam_why(s.kind));
            if (s.kind != SEAM_MEASURED || !s.moved) continue;
            CHECK(s.seam_db <= SEAM_AUDIBLE_DB, "%s: %s clicks at %.1f dB (motion %.1f)",
                  VOICINGS[v].name, move_name(m), (double)s.seam_db, (double)s.motion_db);
        }
    }
}

/* Moves whose effect is deferred by design: they change what the next event
   will do rather than the sound under them, so "no effect within the window"
   is the right answer and not a hole. */
static const char *deferred(Move m) {
    switch (m) {
    case MV_GLIDE: return "takes effect on the next pitch move";
    default: return NULL;
    }
}

/* **Every move is measured.** A move that never reaches the sound in any
   voicing is a probe that is not doing its job, which is how a control gets
   silently skipped. */
static void every_move_is_measured(void) {
    CHECK(swept, "the sweep did not run, so nothing was measured");
    for (Move m = 0; m < MV_COUNT; m++) {
        if (deferred(m)) continue;
        CHECK(reached_the_sound[m], "%s never reaches the sound in any voicing", move_name(m));
    }
}

/* **The ledger is current.** Every known click still reads what it read. */
static void the_known_clicks_have_not_moved(void) {
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        const Known *k = &KNOWN[i];
        if (!k->why) continue;
        Seam s = measure(k->move, &VOICINGS[0]);
        CHECK(s.kind == SEAM_MEASURED, "%s is on the ledger but was not measured",
              move_name(k->move));
        if (s.kind != SEAM_MEASURED) continue;
        float drift = s.seam_db - k->db;
        CHECK(drift <= 3.0f, "%s got worse: %.1f -> %.1f (%s)", move_name(k->move), (double)k->db,
              (double)s.seam_db, k->why);
        CHECK(drift >= -3.0f, "%s got better: %.1f -> %.1f. Delete its row from KNOWN.",
              move_name(k->move), (double)k->db, (double)s.seam_db);
    }
}

/* **The detector still knows a fault from a change.** */
static void the_detector_is_awake(void) {
    Rig rig;
    memset(&rig, 0, sizeof rig);
    rig.poly = 1;
    rig.unison = 1;
    rig.move = MV_COUNT; /* applies nothing */
    SeamProbe probe = {&rig, rig_make, rig_render, rig_apply, rig_teardown};
    Seam still = seam_check(seam_defaults(SR), probe);
    CHECK(still.kind == SEAM_MEASURED, "the still drone was not measured");
    CHECK(!still.moved, "nothing was changed and the sound moved anyway");
}

void test_clicks(void) {
    the_detector_is_awake();
    no_control_clicks();
    every_move_is_measured();
    the_known_clicks_have_not_moved();
}
