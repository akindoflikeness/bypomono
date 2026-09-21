#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gui/app.h"
#include "test.h"

#define SR 48000.0f

static SeqParams seq_of(SeqFill fill, bool smooth) {
    SeqParams p = seq_params_default();
    seq_fill(&p, fill, 1u);
    p.smooth = smooth;
    return p;
}

static ModBase base_of(const Session *s) {
    ModBase b;
    b.patch = s->patch;
    b.verb = s->verb;
    b.chandas = s->chandas;
    b.melody = s->melody;
    b.warmth = s->warmth;
    b.bend = 0.0f;
    return b;
}

/* ---------- the curve ---------- */

static void held_steps_hit_their_values(void) {
    SeqParams p = seq_of(SEQ_FILL_RANDOM, false);
    for (int k = 0; k < SEQ_STEPS; k++) {
        CHECK(seq_value_at(&p, (float)k + 0.01f) == p.value[k], "step %d start", k);
        CHECK(seq_value_at(&p, (float)k + 0.99f) == p.value[k], "step %d end", k);
    }
}

static void smooth_passes_through_every_value_without_overshoot(void) {
    for (int mode = 0; mode < SEQ_MODE_COUNT; mode++) {
        SeqParams p = seq_of(SEQ_FILL_RANDOM, true);
        p.mode = (uint8_t)mode;
        for (int k = 0; k < SEQ_STEPS; k++)
            CHECK_NEAR(seq_value_at(&p, (float)k + 0.5f), p.value[k], 1e-5f,
                       "mode %d step %d centre", mode, k);
        for (int k = 0; k + 1 < SEQ_STEPS; k++) {
            float lo = fminf(p.value[k], p.value[k + 1]);
            float hi = fmaxf(p.value[k], p.value[k + 1]);
            for (int i = 1; i < 20; i++) {
                float v = seq_value_at(&p, (float)k + 0.5f + (float)i / 20.0f);
                CHECK(v >= lo - 1e-5f && v <= hi + 1e-5f,
                      "mode %d between %d and %d: %g outside %g..%g", mode, k,
                      k + 1, v, lo, hi);
            }
        }
    }
}

static void a_loop_joins_its_ends(void) {
    SeqParams p = seq_of(SEQ_FILL_SINE, true);
    float before = seq_value_at(&p, 15.999f), after = seq_value_at(&p, 0.0f);
    CHECK_NEAR(before, after, 1e-3f, "loop jumps at the join: %g then %g", before,
               after);
}

static void once_holds_its_last_value(void) {
    Mod m;
    mod_init(&m, SR);
    SeqParams p = seq_of(SEQ_FILL_RAMP, true);
    p.mode = SEQ_ONCE;
    p.division = -1;
    p.length_s = 1.0f;
    mod_set_seq(&m, 0, p);
    mod_advance(&m, (size_t)(SR * 1.5f), 120.0f);
    CHECK(m.st[0].done, "once did not finish");
    float held = m.st[0].value;
    CHECK_NEAR(held, 2.0f * p.value[SEQ_STEPS - 1] - 1.0f, 1e-4f, "held %g", held);
    mod_advance(&m, (size_t)SR, 120.0f);
    CHECK(m.st[0].value == held, "once moved after finishing");
    mod_note_on(&m);
    CHECK(!m.st[0].done && m.st[0].pos == 0.0f, "a note did not restart it");
}

static void a_synced_step_follows_tempo(void) {
    SeqParams p = seq_params_default(); /* 1/16 */
    CHECK_NEAR(seq_step_seconds(&p, 120.0f), 0.125f, 1e-6f, "1/16 at 120");
    Mod m;
    mod_init(&m, SR);
    mod_set_seq(&m, 0, p);
    mod_advance(&m, (size_t)(SR * 0.5f), 120.0f);
    CHECK_NEAR(m.st[0].pos, 4.0f, 1e-2f, "half a second is 4 steps, got %g",
               m.st[0].pos);
}

/* ---------- routes ---------- */

static void routes_add_depth_times_span_and_clamp(void) {
    Session s = session_default();
    s.patch.index = 0.5f;
    s.verb.decay = 4.0f;
    Mod m;
    mod_init(&m, SR);
    SeqParams p = seq_params_default();
    for (int k = 0; k < SEQ_STEPS; k++) p.value[k] = 1.0f;
    mod_set_seq(&m, 0, p);
    mod_set_route(&m, 0, (ModRoute){0, MT_INDEX, 0.25f, false});
    mod_set_route(&m, 1, (ModRoute){0, MT_DECAY, 1.0f, false});
    mod_advance(&m, 0, 120.0f);
    ModBase base = base_of(&s), out;
    int g = mod_apply(&m, &base, &out);
    CHECK(g & MOD_G_PATCH, "patch group not written");
    CHECK(g & MOD_G_VERB, "verb group not written");
    CHECK_NEAR(out.patch.index, 0.75f, 1e-4f, "index %g", out.patch.index);
    CHECK_NEAR(out.verb.decay, 8.0f, 1e-4f, "decay %g should clamp to 8",
               out.verb.decay);
    CHECK(base.patch.index == 0.5f, "base was changed");
}

static void a_middle_value_changes_nothing(void) {
    Session s = session_default();
    Mod m;
    mod_init(&m, SR);
    mod_set_seq(&m, 0, seq_params_default());
    mod_set_route(&m, 0, (ModRoute){0, MT_INDEX, 1.0f, false});
    mod_advance(&m, 64, 120.0f);
    ModBase base = base_of(&s), out;
    mod_apply(&m, &base, &out);
    CHECK_NEAR(out.patch.index, base.patch.index, 1e-6f, "flat moved index");
}

static void snap_rounds_pitch_to_semitones(void) {
    Session s = session_default();
    Mod m;
    mod_init(&m, SR);
    SeqParams p = seq_params_default();
    for (int k = 0; k < SEQ_STEPS; k++) p.value[k] = 0.5f + 0.37f * 0.5f;
    mod_set_seq(&m, 0, p);
    mod_set_route(&m, 0, (ModRoute){0, MT_PITCH, 1.0f, false});
    mod_advance(&m, 0, 120.0f);
    ModBase base = base_of(&s), out;
    mod_apply(&m, &base, &out);
    CHECK_NEAR(out.bend, 0.37f * 12.0f, 1e-3f, "free bend %g", out.bend);
    mod_set_route(&m, 0, (ModRoute){0, MT_PITCH, 1.0f, true});
    mod_apply(&m, &base, &out);
    CHECK(out.bend == roundf(0.37f * 12.0f), "snapped bend %g", out.bend);
}

static void a_removed_route_writes_its_group_once_more(void) {
    Session s = session_default();
    Mod m;
    mod_init(&m, SR);
    mod_set_seq(&m, 0, seq_of(SEQ_FILL_SINE, true));
    mod_set_route(&m, 0, (ModRoute){0, MT_GHOST, 0.5f, false});
    ModBase base = base_of(&s), out;
    CHECK(mod_apply(&m, &base, &out) & MOD_G_VERB, "ghost not written");
    mod_set_route(&m, 0, (ModRoute){0, MT_NONE, 0.0f, false});
    CHECK(mod_apply(&m, &base, &out) & MOD_G_VERB,
          "ghost not put back after the route went");
    CHECK(out.verb.ghost == base.verb.ghost, "ghost %g, base %g",
          out.verb.ghost, base.verb.ghost);
    CHECK(mod_apply(&m, &base, &out) == 0, "still writing with no routes");
}

static void sanitize_drops_routes_to_empty_sequences(void) {
    ModBank b = mod_bank_default();
    b.route[0] = (ModRoute){0, MT_INDEX, 0.5f, false};
    b.seq[1] = seq_params_default();
    b.seq[1].length_s = 1e9f;
    b.seq[1].value[3] = 7.0f;
    b.route[1] = (ModRoute){1, MT_COUNT + 3, 0.5f, false};
    b.route[2] = (ModRoute){1, MT_MIX, 9.0f, true};
    b = mod_bank_sanitize(b);
    CHECK(b.route[0].target == MT_NONE, "route to an empty sequence survived");
    CHECK(b.route[1].target == MT_NONE, "route to a bad target survived");
    CHECK(b.route[2].depth == 1.0f && !b.route[2].snap, "depth %g snap %d",
          b.route[2].depth, b.route[2].snap);
    CHECK(b.seq[1].length_s == SEQ_LENGTH_MAX_S, "length %g", b.seq[1].length_s);
    CHECK(b.seq[1].value[3] == 1.0f, "value %g", b.seq[1].value[3]);
}

/* ---------- presets ---------- */

static void presets_carry_sequences_and_routes(void) {
    Session s = session_default();
    SeqParams p = seq_of(SEQ_FILL_TRI, false);
    p.mode = SEQ_ONCE;
    p.division = -1;
    p.length_s = 3.5f;
    s.mods.seq[2] = p;
    s.mods.seq[0] = seq_of(SEQ_FILL_SINE, true);
    s.mods.route[0] = (ModRoute){2, MT_CH_WARP, -0.3f, false};
    s.mods.route[1] = (ModRoute){0, MT_PITCH, 0.5f, true};
    char *json = session_to_json(&s);
    CHECK(json != NULL, "no json");
    Session back;
    CHECK(session_from_json(json, &back), "json did not parse:\n%s", json);
    CHECK(json && strstr(json, "\"lfo") == NULL, "old format written");
    free(json);
    const SeqParams *q = &back.mods.seq[2];
    CHECK(q->used && q->mode == SEQ_ONCE && !q->smooth, "seq 3 mode lost");
    CHECK(q->division == -1 && q->length_s == 3.5f, "seq 3 length %g",
          q->length_s);
    for (int k = 0; k < SEQ_STEPS; k++)
        CHECK_NEAR(q->value[k], p.value[k], 1e-6f, "value %d", k);
    CHECK(back.mods.seq[0].division == SEQ_DEFAULT_DIVISION, "seq 1 division");
    CHECK(!back.mods.seq[1].used, "seq 2 appeared from nowhere");
    CHECK(mod_bank_find_route(&back.mods, 2, MT_CH_WARP) >= 0, "warp route lost");
    int pr = mod_bank_find_route(&back.mods, 0, MT_PITCH);
    CHECK(pr >= 0 && back.mods.route[pr].snap, "pitch snap lost");
}

static void an_old_lfo_preset_becomes_sequences(void) {
    const char *doc =
        "{\"tempo_bpm\": 120, \"mods\": {\"lfos\": ["
        "{\"slot\": 3, \"shape\": \"sine\", \"mode\": \"free\", "
        "\"unipolar\": false, \"rate_hz\": 0.5, \"phase\": 0},"
        "{\"slot\": 9, \"shape\": \"square\", \"mode\": \"once\", "
        "\"unipolar\": false, \"rate_hz\": 1, \"division\": \"1/1\", "
        "\"phase\": 0}],"
        "\"routes\": [{\"lfo\": 3, \"target\": \"index\", \"depth\": 0.4},"
        "{\"lfo\": 9, \"target\": \"pitch\", \"depth\": 0.1}]}}";
    Session s;
    CHECK(session_from_json(doc, &s), "old preset did not load");
    const SeqParams *sine = &s.mods.seq[0];
    CHECK(sine->used && sine->smooth && sine->mode == SEQ_LOOP, "sine seq");
    for (int k = 0; k < SEQ_STEPS; k++) {
        float want = 0.5f + 0.5f * sinf(TAU_F * ((float)k + 0.5f) / 16.0f);
        CHECK(fabsf(sine->value[k] - want) < 0.05f, "sine step %d: %g, want %g",
              k, sine->value[k], want);
    }
    CHECK(sine->division == -1 && sine->length_s == 2.0f, "0.5 hz is 2 s, got %g",
          sine->length_s);
    const SeqParams *sq = &s.mods.seq[1];
    CHECK(sq->used && !sq->smooth && sq->mode == SEQ_ONCE, "square seq");
    CHECK(sq->division == SEQ_DEFAULT_DIVISION, "a bar cycle is 1/16 steps: %d",
          sq->division);
    int r = mod_bank_find_route(&s.mods, 0, MT_INDEX);
    CHECK(r >= 0 && s.mods.route[r].depth == 0.4f, "index route lost");
    CHECK(mod_bank_find_route(&s.mods, 1, MT_PITCH) >= 0, "pitch route lost");

    char *json = session_to_json(&s);
    Session back;
    CHECK(json && session_from_json(json, &back), "converted preset round trip");
    CHECK(json && strstr(json, "\"seqs\"") && !strstr(json, "\"lfos\""),
          "converted preset not written as sequences");
    free(json);
    CHECK(memcmp(&back.mods.seq[0], &s.mods.seq[0], sizeof back.mods.seq[0]) == 0,
          "sequence changed on the way round");
}

static void a_preset_without_mods_has_none(void) {
    Session s;
    CHECK(session_from_json("{\"tempo_bpm\": 100}", &s), "minimal preset");
    for (int i = 0; i < SEQS; i++)
        CHECK(!s.mods.seq[i].used, "seq %d used in a preset without mods", i);
    Session d = session_default();
    char *json = session_to_json(&d);
    CHECK(json && !strstr(json, "\"mods\""), "empty mods were written");
    free(json);
}

static void hostile_mods_never_escape_their_ranges(void) {
    const char *docs[] = {
        "{\"mods\": {\"seqs\": [{\"slot\": 99}]}}",
        "{\"mods\": {\"seqs\": [{\"slot\": 0}]}}",
        "{\"mods\": {\"seqs\": [{\"slot\": 1, \"length_s\": 1e30, "
        "\"values\": [9, -9, 1e30], \"gates\": [0, 17, 3], \"mode\": \"x\"}], "
        "\"routes\": [{\"seq\": 1, \"target\": \"index\", \"depth\": 1e9, "
        "\"snap\": true}, {\"seq\": 8, \"target\": \"mix\", \"depth\": 0.5}]}}",
        "{\"mods\": {\"lfos\": [{\"slot\": 1, \"rate_hz\": 1e30, "
        "\"shape\": \"nope\"}], \"routes\": [{\"lfo\": 1, \"target\": "
        "\"index\", \"depth\": 1e9}]}}",
        "{\"mods\": {\"seqs\": {}}}",
        "{\"mods\": [1,2,3]}",
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        Session s;
        if (!session_from_json(docs[i], &s)) continue;
        for (int k = 0; k < SEQS; k++) {
            const SeqParams *p = &s.mods.seq[k];
            CHECK(p->length_s >= SEQ_LENGTH_MIN_S && p->length_s <= SEQ_LENGTH_MAX_S,
                  "doc %zu seq %d length %g", i, k, p->length_s);
            CHECK(p->mode < SEQ_MODE_COUNT, "doc %zu mode %d", i, p->mode);
            for (int v = 0; v < SEQ_STEPS; v++)
                CHECK(p->value[v] >= 0.0f && p->value[v] <= 1.0f,
                      "doc %zu value %g", i, p->value[v]);
        }
        for (int k = 0; k < MOD_ROUTES; k++) {
            const ModRoute *r = &s.mods.route[k];
            if (r->target == MT_NONE) continue;
            CHECK(s.mods.seq[r->seq].used, "doc %zu route to empty seq", i);
            CHECK(r->depth >= -1.0f && r->depth <= 1.0f, "doc %zu depth %g", i,
                  r->depth);
            CHECK(!r->snap || r->target == MT_PITCH, "doc %zu snap off pitch", i);
        }
    }
}

static void nowhere(void *ud, size_t n, const Frame *f) { (void)ud; (void)n; (void)f; }

/* a sequence writes fb every control block, so it glides to its target
   instead of arriving on the first sample */
static void fb_glides(void) {
    Patch patch = patch_init(ALGORITHMS[0], RATIO_HARMONIC);
    patch.feedback = 0.0f;
    Voice v;
    voice_init(&v, SR, patch);
    CHECK(v.fb_smooth == 0.0f, "born off target");
    Patch loud = patch;
    loud.feedback = 1.0f;
    voice_set_patch(&v, loud);
    voice_render_frames(&v, 1, nowhere, NULL);
    CHECK(v.fb_smooth > 0.0f && v.fb_smooth < 0.05f,
          "fb jumped to %g in one sample", v.fb_smooth);
    voice_render_frames(&v, (size_t)(SR * 0.1f), nowhere, NULL);
    CHECK(v.fb_smooth > 0.99f, "fb only reached %g in 100 ms", v.fb_smooth);
    voice_free(&v);
}

/* ---------- the seq verb ---------- */

#include "../src/gui/command.h"

static App mod_app;

static bool run(const char *line, char *err, size_t cap) {
    Command c;
    if (!parse_line(line, &c, err, cap)) return false;
    return command_run(&mod_app, &c, err, cap);
}

static void fresh_app(void) {
    memset(&mod_app, 0, sizeof mod_app);
    mod_app.mods = mod_bank_default();
}

static void seq_line_reads_every_word(void) {
    Command c;
    char err[512];
    CHECK(parse_line("seq 2 fill tri steps once rate 2.5s step 3 0.9 "
                     "to pitch 0.5 snap to chandas warp -0.5 -v",
                     &c, err, sizeof err),
          "did not parse: %s", err);
    const SeqCmd *m = &c.seq;
    CHECK(c.view, "-v lost");
    CHECK(m->slot == 1, "slot %d", m->slot);
    CHECK(m->set_fill && m->fill == SEQ_FILL_TRI, "fill");
    CHECK(m->set_smooth && !m->smooth, "steps");
    CHECK(m->set_mode && m->mode == SEQ_ONCE, "once");
    CHECK(m->set_rate && m->division == -1 && m->length_s == 2.5f, "rate");
    CHECK(m->nsteps == 1 && m->steps[0].step == 2 && m->steps[0].v == 0.9f,
          "step");
    CHECK(m->nroutes == 2 && m->route[0].snap, "pitch snap route");
    CHECK(m->route[1].target == MT_CH_WARP && m->route[1].depth == -0.5f,
          "two-word target");
    CHECK(parse_line("seq 1 rate 1/8T", &c, err, sizeof err), "division: %s", err);
    CHECK(c.seq.set_rate && c.seq.division >= 0, "division not read");
}

static void seq_errors_say_what_they_want(void) {
    Command c;
    char err[512];
    CHECK(!parse_line("seq 0", &c, err, sizeof err), "seq 0 parsed");
    CHECK(strstr(err, "1 to 8") != NULL, "range missing: %s", err);
    CHECK(!parse_line("seq 1 fill sqare", &c, err, sizeof err), "bad fill");
    CHECK(!parse_line("seq 1 to index 3", &c, err, sizeof err), "depth 3 parsed");
    CHECK(strstr(err, "-1 to 1") != NULL, "depth range missing: %s", err);
    CHECK(!parse_line("seq 1 to index 0.5 snap", &c, err, sizeof err),
          "snap off pitch parsed");
    CHECK(!parse_line("seq 1 rate 99", &c, err, sizeof err), "rate 99 parsed");
    CHECK(!parse_line("seq 1 set 0.1 0.2", &c, err, sizeof err), "short set");
    CHECK(!parse_line("seq 1 rm fill sine", &c, err, sizeof err), "rm with words");
    CHECK(!parse_line("mod lfo 1 tri", &c, err, sizeof err), "mod lfo still parses");
}

static void seq_runs_make_point_and_remove(void) {
    fresh_app();
    ModBank *b = &mod_app.mods;
    char err[512];
    CHECK(!run("seq 5", err, sizeof err), "query of a missing sequence ran");
    CHECK(run("seq 5 fill saw to mix 0.2 to pitch -0.1", err, sizeof err), "%s", err);
    CHECK(b->seq[4].used && b->seq[4].value[0] > b->seq[4].value[15],
          "seq 5 not made as a saw");
    CHECK(run("seq 5 set 0 .1 .2 .3 .4 .5 .6 .7 .8 .9 1 1 1 1 1 1", err, sizeof err),
          "%s", err);
    CHECK(b->seq[4].value[3] == 0.3f, "set did not land");
    CHECK(run("seq 5 to mix 0.6", err, sizeof err), "%s", err);
    int r = mod_bank_find_route(b, 4, MT_MIX);
    CHECK(r >= 0 && b->route[r].depth == 0.6f, "depth not replaced");
    CHECK(!run("seq 5 to ghost off", err, sizeof err), "removed a missing route");
    CHECK(run("seq 5 to mix off", err, sizeof err), "%s", err);
    CHECK(mod_bank_find_route(b, 4, MT_MIX) < 0, "mix route stayed");
    CHECK(run("seq 5 rm", err, sizeof err), "%s", err);
    CHECK(!b->seq[4].used, "seq 5 stayed");
    CHECK(mod_bank_find_route(b, 4, MT_PITCH) < 0, "orphan route stayed");
}

static void a_failed_line_changes_nothing(void) {
    fresh_app();
    char err[512];
    for (int i = 0; i < MOD_ROUTES; i++) {
        char line[96];
        snprintf(line, sizeof line, "seq %d to %s 0.1", i % SEQS + 1,
                 MOD_TARGETS[MT_INDEX + i % (MT_COUNT - 1)].name);
        run(line, err, sizeof err);
    }
    ModBank before = mod_app.mods;
    CHECK(!run("seq 8 fill tri to ghost 0.5 to damp 0.5", err, sizeof err),
          "ran with every route taken");
    CHECK(memcmp(&before, &mod_app.mods, sizeof before) == 0,
          "a refused line still changed the bank");
}

static void pins_hold_a_sequence_and_the_list(void) {
    fresh_app();
    App *a = &mod_app;
    char err[512];
    CHECK(run("seq 1 fill sine -v", err, sizeof err), "%s", err);
    CHECK(a->pin_count == 1 && strcmp(a->pins[0], "seq 1") == 0,
          "pin '%s' count %d", a->pins[0], a->pin_count);
    View v;
    CHECK(command_view(a, a->pins[0], &v) && v.n == 1
              && v.line[0].place == GRAPH_BELOW,
          "pinned view");
    CHECK(run("seq -v", err, sizeof err), "%s", err);
    CHECK(a->pin_count == 2 && strcmp(a->pins[1], "seq") == 0, "seq list pin");
    CHECK(command_view(a, a->pins[1], &v), "the list pin is gone");
    CHECK(run("seq 1 rm", err, sizeof err), "%s", err);
    CHECK(command_view(a, a->pins[0], &v), "a pin to a removed sequence is gone");
}

void test_mod(void) {
    held_steps_hit_their_values();
    smooth_passes_through_every_value_without_overshoot();
    a_loop_joins_its_ends();
    once_holds_its_last_value();
    a_synced_step_follows_tempo();
    routes_add_depth_times_span_and_clamp();
    a_middle_value_changes_nothing();
    snap_rounds_pitch_to_semitones();
    a_removed_route_writes_its_group_once_more();
    sanitize_drops_routes_to_empty_sequences();
    presets_carry_sequences_and_routes();
    an_old_lfo_preset_becomes_sequences();
    a_preset_without_mods_has_none();
    hostile_mods_never_escape_their_ranges();
    fb_glides();
    seq_line_reads_every_word();
    seq_errors_say_what_they_want();
    seq_runs_make_point_and_remove();
    a_failed_line_changes_nothing();
    pins_hold_a_sequence_and_the_list();
}
