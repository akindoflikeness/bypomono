#include <stdlib.h>
#include <string.h>

#include "../src/gui/app.h"
#include "test.h"

#define SR 48000.0f

static LfoParams lfo(LfoShape shape, float hz) {
    LfoParams p = lfo_params_default();
    p.shape = shape;
    p.rate_hz = hz;
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

static void shapes_stay_in_their_polarity(void) {
    for (int sh = 0; sh < LFO_SHAPE_COUNT; sh++) {
        LfoParams p = lfo((LfoShape)sh, 1.0f);
        for (int uni = 0; uni < 2; uni++) {
            p.unipolar = uni;
            float lo = uni ? 0.0f : -1.0f;
            for (int i = 0; i <= 400; i++) {
                float v = lfo_shape_at(&p, (float)i / 100.0f, 7u);
                CHECK(v >= lo - 1e-5f && v <= 1.0f + 1e-5f,
                      "%s uni=%d gave %g", lfo_shape_name((LfoShape)sh), uni, v);
            }
        }
    }
}

static void sine_starts_at_its_phase(void) {
    LfoParams p = lfo(LFO_SINE, 1.0f);
    p.phase = 0.25f;
    Mod m;
    mod_init(&m, SR);
    mod_set_lfo(&m, 0, p);
    mod_advance(&m, 0, 120.0f);
    CHECK_NEAR(m.st[0].value, 1.0f, 1e-4f, "value %g at a quarter cycle",
               m.st[0].value);
}

static void one_hertz_completes_a_cycle_in_a_second(void) {
    Mod m;
    mod_init(&m, SR);
    mod_set_lfo(&m, 0, lfo(LFO_RAMP, 1.0f));
    for (int i = 0; i < 1500; i++) mod_advance(&m, MOD_BLOCK, 120.0f);
    CHECK_NEAR(m.st[0].phase, 0.0f, 1e-3f, "phase %g after 48000 samples",
               m.st[0].phase);
}

static void synced_rate_follows_tempo(void) {
    LfoParams p = lfo(LFO_SINE, 1.0f);
    for (int d = 0; d < CHANDAS_DIVISIONS_LEN; d++)
        if (strcmp(CHANDAS_DIVISIONS[d].name, "1/4") == 0) p.division = (int8_t)d;
    CHECK_NEAR(lfo_effective_hz(&p, 120.0f), 2.0f, 1e-4f, "1/4 at 120 bpm");
    CHECK_NEAR(lfo_effective_hz(&p, 60.0f), 1.0f, 1e-4f, "1/4 at 60 bpm");
}

static void retrig_resets_and_free_does_not(void) {
    Mod m;
    mod_init(&m, SR);
    LfoParams free_ = lfo(LFO_RAMP, 3.0f);
    LfoParams retrig = lfo(LFO_RAMP, 3.0f);
    retrig.mode = LFO_RETRIG;
    mod_set_lfo(&m, 0, free_);
    mod_set_lfo(&m, 1, retrig);
    mod_advance(&m, 10000, 120.0f);
    float free_before = m.st[0].phase;
    mod_note_on(&m);
    CHECK(m.st[0].phase == free_before, "free lfo moved on a note");
    CHECK(m.st[1].phase == 0.0f, "retrig lfo at %g after a note", m.st[1].phase);
}

static void once_runs_one_cycle_and_holds(void) {
    Mod m;
    mod_init(&m, SR);
    LfoParams p = lfo(LFO_RAMP, 10.0f);
    p.mode = LFO_ONCE;
    mod_set_lfo(&m, 0, p);
    mod_note_on(&m);
    mod_advance(&m, (size_t)SR, 120.0f);
    CHECK(m.st[0].done, "once never finished");
    float held = m.st[0].value;
    mod_advance(&m, (size_t)SR, 120.0f);
    CHECK(m.st[0].value == held, "once moved after finishing");
    CHECK_NEAR(held, 1.0f, 1e-3f, "ramp held at %g, wants its end", held);
}

static void routes_add_depth_times_span_and_clamp(void) {
    Session s = session_default();
    s.patch.index = 0.5f;
    s.verb.decay = 4.0f;
    Mod m;
    mod_init(&m, SR);
    LfoParams p = lfo(LFO_SQUARE, 1.0f);
    mod_set_lfo(&m, 0, p);
    mod_set_route(&m, 0, (ModRoute){0, MT_INDEX, 0.25f});
    mod_set_route(&m, 1, (ModRoute){0, MT_DECAY, 1.0f});
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

static void pitch_depth_one_is_an_octave(void) {
    Session s = session_default();
    Mod m;
    mod_init(&m, SR);
    mod_set_lfo(&m, 0, lfo(LFO_SQUARE, 1.0f));
    mod_set_route(&m, 0, (ModRoute){0, MT_PITCH, 1.0f});
    mod_advance(&m, 0, 120.0f);
    ModBase base = base_of(&s), out;
    mod_apply(&m, &base, &out);
    CHECK_NEAR(out.bend, 12.0f, 1e-4f, "bend %g", out.bend);
}

static void a_removed_route_writes_its_group_once_more(void) {
    Session s = session_default();
    Mod m;
    mod_init(&m, SR);
    mod_set_lfo(&m, 0, lfo(LFO_SINE, 1.0f));
    mod_set_route(&m, 0, (ModRoute){0, MT_GHOST, 0.5f});
    ModBase base = base_of(&s), out;
    CHECK(mod_apply(&m, &base, &out) & MOD_G_VERB, "ghost not written");
    mod_set_route(&m, 0, (ModRoute){0, MT_NONE, 0.0f});
    CHECK(mod_apply(&m, &base, &out) & MOD_G_VERB,
          "ghost not put back after the route went");
    CHECK(out.verb.ghost == base.verb.ghost, "ghost %g, base %g",
          out.verb.ghost, base.verb.ghost);
    CHECK(mod_apply(&m, &base, &out) == 0, "still writing with no routes");
}

static void sanitize_drops_routes_to_empty_lfos(void) {
    ModBank b = mod_bank_default();
    b.route[0] = (ModRoute){3, MT_INDEX, 0.5f};
    b.route[1] = (ModRoute){0, 200, 0.5f};
    b.lfo[0] = lfo(LFO_SINE, 999.0f);
    b.route[2] = (ModRoute){0, MT_MIX, 7.0f};
    b = mod_bank_sanitize(b);
    CHECK(b.route[0].target == MT_NONE, "route to an empty lfo survived");
    CHECK(b.route[1].target == MT_NONE, "route to a bad target survived");
    CHECK(b.lfo[0].rate_hz == LFO_RATE_MAX_HZ, "rate %g", b.lfo[0].rate_hz);
    CHECK(b.route[2].depth == 1.0f, "depth %g", b.route[2].depth);
}

static void presets_carry_lfos_and_routes(void) {
    Session s = session_default();
    LfoParams p = lfo(LFO_DRIFT, 0.25f);
    p.mode = LFO_ONCE;
    p.unipolar = true;
    p.phase = 0.5f;
    s.mods.lfo[2] = p;
    LfoParams q = lfo(LFO_SQUARE, 1.0f);
    q.division = 15;
    s.mods.lfo[0] = q;
    s.mods.route[0] = (ModRoute){2, MT_CH_WARP, -0.3f};
    s.mods.route[1] = (ModRoute){0, MT_PITCH, 0.05f};
    char *json = session_to_json(&s);
    CHECK(json != NULL, "no json");
    Session back;
    CHECK(session_from_json(json, &back), "json did not parse:\n%s", json);
    free(json);
    CHECK(back.mods.lfo[2].used && back.mods.lfo[2].shape == LFO_DRIFT,
          "lfo 3 shape lost");
    CHECK(back.mods.lfo[2].mode == LFO_ONCE && back.mods.lfo[2].unipolar,
          "lfo 3 mode or polarity lost");
    CHECK(back.mods.lfo[2].phase == 0.5f && back.mods.lfo[2].rate_hz == 0.25f,
          "lfo 3 phase %g rate %g", back.mods.lfo[2].phase,
          back.mods.lfo[2].rate_hz);
    CHECK(back.mods.lfo[0].division == 15, "lfo 1 division %d",
          back.mods.lfo[0].division);
    CHECK(!back.mods.lfo[1].used, "lfo 2 appeared from nowhere");
    CHECK(mod_bank_find_route(&back.mods, 2, MT_CH_WARP) >= 0,
          "chandas warp route lost");
    int pr = mod_bank_find_route(&back.mods, 0, MT_PITCH);
    CHECK(pr >= 0 && back.mods.route[pr].depth == 0.05f, "pitch route lost");
}

static void a_preset_without_mods_has_none(void) {
    Session s;
    CHECK(session_from_json("{\"warmth\": 0.2}", &s), "minimal preset");
    for (int i = 0; i < MOD_LFOS; i++)
        CHECK(!s.mods.lfo[i].used, "lfo %d used in a preset without mods", i);
    Session d = session_default();
    char *json = session_to_json(&d);
    CHECK(json && !strstr(json, "\"mods\""), "empty mods were written");
    free(json);
}

static void hostile_mods_never_escape_their_ranges(void) {
    const char *docs[] = {
        "{\"mods\": {\"lfos\": [{\"slot\": 99, \"shape\": \"sine\"}]}}",
        "{\"mods\": {\"lfos\": [{\"slot\": 0}]}}",
        "{\"mods\": {\"lfos\": [{\"slot\": 1, \"rate_hz\": 1e30, "
        "\"phase\": -5, \"shape\": \"nope\"}], \"routes\": "
        "[{\"lfo\": 1, \"target\": \"index\", \"depth\": 1e9}, "
        "{\"lfo\": 16, \"target\": \"mix\", \"depth\": 0.5}]}}",
        "{\"mods\": {\"routes\": [{\"lfo\": 1, \"target\": \"index\"}]}}",
        "{\"mods\": {\"lfos\": {}}}",
        "{\"mods\": [1,2,3]}",
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        Session s;
        if (!session_from_json(docs[i], &s)) continue;
        for (int k = 0; k < MOD_LFOS; k++) {
            const LfoParams *p = &s.mods.lfo[k];
            CHECK(p->rate_hz >= LFO_RATE_MIN_HZ && p->rate_hz <= LFO_RATE_MAX_HZ,
                  "doc %zu lfo %d rate %g", i, k, p->rate_hz);
            CHECK(p->phase >= 0.0f && p->phase <= 1.0f, "doc %zu phase %g", i,
                  p->phase);
            CHECK(p->shape < LFO_SHAPE_COUNT, "doc %zu shape %d", i, p->shape);
        }
        for (int k = 0; k < MOD_ROUTES; k++) {
            const ModRoute *r = &s.mods.route[k];
            if (r->target == MT_NONE) continue;
            CHECK(s.mods.lfo[r->lfo].used, "doc %zu route to empty lfo", i);
            CHECK(r->depth >= -1.0f && r->depth <= 1.0f, "doc %zu depth %g", i,
                  r->depth);
        }
    }
}

static void nowhere(void *ud, size_t n, const Frame *f) { (void)ud; (void)n; (void)f; }

/* an lfo writes fb every control block, so it glides to its target instead
   of arriving on the first sample (rip is smoothed on main) */
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

/* the random shapes are drawn from what they did, so the readout has to
   carry their spread */
static void random_shapes_fill_their_history(void) {
    static LfoMeter meter;
    memset(&meter, 0, sizeof meter);
    Mod m;
    mod_init(&m, SR);
    LfoParams p = lfo(LFO_SH, 6.0f);
    mod_set_lfo(&m, 0, p);
    for (int i = 0; i < 3000; i++) {
        mod_advance(&m, MOD_BLOCK, 120.0f);
        lfo_meter_store(&meter, &m);
    }
    float hist[LFO_HIST];
    int n = lfo_meter_history(&meter, 0, hist, LFO_HIST);
    CHECK(n == LFO_HIST, "history holds %d of %d", n, LFO_HIST);
    float lo = 1.0f, hi = -1.0f, steps = 0.0f;
    for (int i = 0; i < n; i++) {
        if (hist[i] < lo) lo = hist[i];
        if (hist[i] > hi) hi = hist[i];
        if (i && fabsf(hist[i] - hist[i - 1]) > 0.01f) steps += 1.0f;
    }
    CHECK(hi - lo > 0.5f, "sample-hold spread is only %g", hi - lo);
    CHECK(steps > 2.0f, "only %g steps in the window", steps);
}

/* ---------- the lfo and mods verbs ---------- */

#include "../src/gui/command.h"

static App mod_app;

static bool parses(const char *line, Command *c, char *err, size_t cap) {
    return parse_line(line, c, err, cap);
}

static void lfo_line_reads_every_word(void) {
    Command c;
    char err[512];
    CHECK(parses("mod lfo 2 tri rate 2hz phase 90 retrig uni to index 0.4 "
                 "to chandas warp -0.5 -v",
                 &c, err, sizeof err),
          "did not parse: %s", err);
    const ModCmd *m = &c.mod;
    CHECK(c.view, "-v lost");
    CHECK(m->slot == 1, "slot %d", m->slot);
    CHECK(m->set_shape && m->shape == LFO_TRIANGLE, "shape");
    CHECK(m->set_rate && m->division == -1 && m->rate_hz == 2.0f, "rate");
    CHECK(m->set_phase && fabsf(m->phase - 0.25f) < 1e-6f, "phase %g", m->phase);
    CHECK(m->set_mode && m->mode == LFO_RETRIG, "mode");
    CHECK(m->set_pol && m->unipolar, "polarity");
    CHECK(m->nroutes == 2, "routes %d", m->nroutes);
    CHECK(m->route[1].target == MT_CH_WARP && m->route[1].depth == -0.5f,
          "two-word target");
    CHECK(parses("mod lfo 1 1/8T", &c, err, sizeof err), "bare division: %s", err);
    CHECK(c.mod.set_rate && c.mod.division >= 0, "division not read");
}

static void lfo_errors_name_the_word_and_what_it_wants(void) {
    Command c;
    char err[512];
    CHECK(!parses("mod lfo 3 sqare", &c, err, sizeof err), "typo parsed");
    CHECK(strstr(err, "did you mean square") != NULL, "no suggestion: %s", err);
    CHECK(strchr(err, '\n') != NULL, "error lacks the usage line: %s", err);
    CHECK(!parses("mod lfo 0", &c, err, sizeof err), "lfo 0 parsed");
    CHECK(strstr(err, "1 to 16") != NULL, "range missing: %s", err);
    CHECK(!parses("mod lfo 1 to indx 0.3", &c, err, sizeof err), "bad target");
    CHECK(strstr(err, "did you mean index") != NULL, "target hint: %s", err);
    CHECK(!parses("mod lfo 1 to index 3", &c, err, sizeof err), "depth 3 parsed");
    CHECK(strstr(err, "-1 to 1") != NULL, "depth range missing: %s", err);
    CHECK(!parses("mod lfo 1 rate 99", &c, err, sizeof err), "rate 99 parsed");
    CHECK(!parses("mod lfo 1 rm shape tri", &c, err, sizeof err), "rm with words");
    CHECK(!parses("ls mod 3", &c, err, sizeof err), "ls mod took a word");
}

static bool run(const char *line, char *err, size_t cap) {
    Command c;
    if (!parse_line(line, &c, err, cap)) return false;
    return command_run(&mod_app, &c, err, cap);
}

static void lfo_runs_make_point_and_remove(void) {
    App *a = &mod_app;
    memset(a, 0, sizeof *a);
    a->mods = mod_bank_default();
    char err[512];
    CHECK(!run("mod lfo 5", err, sizeof err), "query of a missing lfo ran");
    CHECK(run("mod lfo 5 saw to mix 0.2 to pitch -0.1", err, sizeof err), "%s", err);
    CHECK(a->mods.lfo[4].used && a->mods.lfo[4].shape == LFO_SAW, "lfo 5 not made");
    CHECK(mod_bank_find_route(&a->mods, 4, MT_MIX) >= 0, "mix route missing");
    CHECK(run("mod lfo 5 to mix 0.6", err, sizeof err), "%s", err);
    int r = mod_bank_find_route(&a->mods, 4, MT_MIX);
    CHECK(r >= 0 && a->mods.route[r].depth == 0.6f, "depth not replaced");
    int count = 0;
    for (int i = 0; i < MOD_ROUTES; i++) count += a->mods.route[i].target != MT_NONE;
    CHECK(count == 2, "replacing a depth made a new route: %d", count);
    CHECK(!run("mod lfo 5 to ghost off", err, sizeof err), "removed a missing route");
    CHECK(run("mod lfo 5 to mix off", err, sizeof err), "%s", err);
    CHECK(mod_bank_find_route(&a->mods, 4, MT_MIX) < 0, "mix route stayed");
    CHECK(run("mod lfo 5 rm", err, sizeof err), "%s", err);
    CHECK(!a->mods.lfo[4].used, "lfo 5 stayed");
    CHECK(mod_bank_find_route(&a->mods, 4, MT_PITCH) < 0, "orphan route stayed");
}

static void a_failed_line_changes_nothing(void) {
    App *a = &mod_app;
    memset(a, 0, sizeof *a);
    a->mods = mod_bank_default();
    char err[512];
    for (int i = 0; i < MOD_ROUTES; i++) {
        char line[64];
        snprintf(line, sizeof line, "mod lfo %d to %s 0.1", i % MOD_LFOS + 1,
                 MOD_TARGETS[MT_INDEX + i % (MT_COUNT - 1)].name);
        run(line, err, sizeof err);
    }
    ModBank before = a->mods;
    CHECK(!run("mod lfo 16 tri to ghost 0.5 to mix 0.5", err, sizeof err),
          "ran with every route taken");
    CHECK(memcmp(&before, &a->mods, sizeof before) == 0,
          "a refused line still changed the bank");
}

static void minus_v_toggles_a_pin(void) {
    App *a = &mod_app;
    memset(a, 0, sizeof *a);
    a->mods = mod_bank_default();
    char err[512];
    CHECK(run("mod lfo 1 tri -v", err, sizeof err), "%s", err);
    CHECK(a->pin_count == 1 && strcmp(a->pins[0], "mod lfo 1") == 0,
          "pin '%s' count %d", a->pins[0], a->pin_count);
    View v;
    CHECK(command_view(a, a->pins[0], &v) && v.n == 1
              && v.line[0].place == GRAPH_BELOW,
          "pinned view");
    CHECK(run("ls mod -v", err, sizeof err), "%s", err);
    CHECK(run("mod lfo 1 rate 3 -v", err, sizeof err), "%s", err);
    CHECK(a->pin_count == 1 && strcmp(a->pins[0], "ls mod") == 0,
          "second -v did not let lfo 1 go");
    CHECK(a->mods.lfo[0].rate_hz == 3.0f, "the unpinning line did not apply");
}

void test_mod(void) {
    fb_glides();
    random_shapes_fill_their_history();
    lfo_line_reads_every_word();
    lfo_errors_name_the_word_and_what_it_wants();
    lfo_runs_make_point_and_remove();
    a_failed_line_changes_nothing();
    minus_v_toggles_a_pin();
    presets_carry_lfos_and_routes();
    a_preset_without_mods_has_none();
    hostile_mods_never_escape_their_ranges();
    shapes_stay_in_their_polarity();
    sine_starts_at_its_phase();
    one_hertz_completes_a_cycle_in_a_second();
    synced_rate_follows_tempo();
    retrig_resets_and_free_does_not();
    once_runs_one_cycle_and_holds();
    routes_add_depth_times_span_and_clamp();
    pitch_depth_one_is_an_octave();
    a_removed_route_writes_its_group_once_more();
    sanitize_drops_routes_to_empty_lfos();
}
