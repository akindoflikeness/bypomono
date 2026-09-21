/* The console's verb table, parser, errors, history and the trash+undo
   round trip, all against a temp preset dir. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/gui/app.h"
#include "../src/gui/command.h"
#include "test.h"

/* the verbs that reach the audio rig are stubbed; the test binary links no
   engine (push_log and app_send are stubbed by the presets suite) */
static int rec_calls, bind_calls, unbind_calls, last_cc, last_unbind;
void gui_run_record(App *a, const char *args) { (void)a; (void)args; rec_calls++; }
void gui_run_bind(App *a, int cc, CcTarget t) { (void)a; (void)t; bind_calls++; last_cc = cc; }
void gui_run_unbind(App *a, int cc) { (void)a; unbind_calls++; last_unbind = cc; }
void midi_note_clear(MidiNoteAtom *m) { memset(m, 0, sizeof *m); }
int midi_port_names(char names[][128], int max) {
    if (max < 1) return 0;
    snprintf(names[0], 128, "Test MIDI");
    return 1;
}
bool gui_set_midi_port(App *a, const char *name) {
    a->midi_open = name != NULL;
    snprintf(a->midi_port, sizeof a->midi_port, "%s", name ? name : "");
    return !name || strcmp(name, "Test MIDI") == 0;
}
const char *cc_target_name(CcTarget t) {
    switch (t) {
    case CC_INDEX: return "index";
    case CC_RIP: return "rip";
    case CC_FB: return "fb";
    case CC_FIELD: return "field";
    case CC_CURVE: return "curve";
    case CC_RELEASE: return "release";
    case CC_GLIDE: return "glide";
    case CC_DRONEHZ: return "dronehz";
    case CC_MIX: return "mix";
    case CC_GHOST: return "ghost";
    case CC_DECAY: return "decay";
    case CC_DAMP: return "damp";
    case CC_HAUNT: return "haunt";
    case CC_WARMTH: return "warmth";
    case CC_ATTACK: return "attack";
    case CC_ENVDECAY: return "envdecay";
    case CC_SUSTAIN: return "sustain";
    default: return "?";
    }
}
CcTarget cc_target_from_name(const char *s) {
    for (int t = CC_INDEX; t <= CC_LAST; t++)
        if (strcmp(cc_target_name((CcTarget)t), s) == 0) return (CcTarget)t;
    return CC_NONE;
}

static App app;
static bool run_ok(const char *line, char *err, size_t cap);

/* ---------- files ---------- */

static bool present(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            rm_rf(full);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static bool read_file(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    return true;
}

static void in_dir(const char *leaf, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", preset_dir(), leaf);
}

/* ---------- parsing ---------- */

static bool parses(const char *line, CommandKind want) {
    Command c;
    char err[768];
    bool ok = parse_line(line, &c, err, sizeof err);
    CHECK(ok, "'%s' failed to parse: %s", line, err);
    CHECK(c.kind == want, "'%s' parsed as kind %d, wanted %d", line, c.kind,
          want);
    return ok && c.kind == want;
}

static void error_lines(const char *line, const char *first,
                        const char *second_prefix) {
    Command c;
    char err[768];
    bool ok = parse_line(line, &c, err, sizeof err);
    CHECK(!ok, "'%s' parsed", line);
    if (ok) return;
    char *nl = strchr(err, '\n');
    CHECK(nl != NULL, "'%s': error has one line: %s", line, err);
    if (!nl) return;
    CHECK(strchr(nl + 1, '\n') == NULL, "'%s': error has more than two lines",
          line);
    *nl = 0;
    CHECK(strcmp(err, first) == 0, "'%s': first line '%s', wanted '%s'", line,
          err, first);
    CHECK(strncmp(nl + 1, second_prefix, strlen(second_prefix)) == 0,
          "'%s': second line '%s', wanted '%s...'", line, nl + 1,
          second_prefix);
}

static void every_verb_parses_its_forms(void) {
    CHECK(verb_lookup("lfo") == NULL && verb_lookup("mods") == NULL
              && verb_lookup("bind") == NULL && verb_lookup("unbind") == NULL,
          "old modulation verbs are still exposed");
    for (int i = 0; i < verb_count(); i++) {
        const Verb *v = verb_at(i);
        char line[256];
        if (strcmp(v->name, "mod") == 0) {
            parses("mod cc 7 index", CMD_RUN);
            parses("mod cc cc7 off", CMD_RUN);
            parses("mod cc all off", CMD_RUN);
            parses("mod lfo 1 tri", CMD_RUN);
            continue;
        }
        if (strcmp(v->name, "tempo") == 0) {
            parses("tempo 120", CMD_RUN);
            continue;
        }
        if (strcmp(v->name, "bend") == 0) {
            parses("bend -1", CMD_RUN);
            continue;
        }
        if (strcmp(v->name, "help") == 0) {
            parses("help", CMD_HELP);
            parses("?", CMD_HELP);
            parses("help rm", CMD_HELP);
            parses("help presets", CMD_HELP);
            continue;
        }
        if (strcmp(v->name, "op") == 0) {
            parses("op 1", CMD_RUN);
            parses("op 5 off", CMD_RUN);
            continue;
        }
        if (strcmp(v->name, "get") == 0) {
            parses("get index", CMD_RUN);
            parses("get op 2", CMD_RUN);
            continue;
        }
        if (strcmp(v->name, "set") == 0) {
            parses("set index 0.5", CMD_RUN);
            parses("set chandas warp 0.2", CMD_RUN);
            continue;
        }
        if (v->run == control_run) {
            char line[128];
            const char *value = strcmp(v->name, "mode") == 0 ? "golden"
                                : strcmp(v->name, "hz") == 0 ? "110hz"
                                : strcmp(v->name, "note") == 0 ? "45"
                                : strcmp(v->name, "alg") == 0 ? "3"
                                : "0.5";
            snprintf(line, sizeof line, "%s %s", v->name, value);
            parses(line, CMD_RUN);
            continue;
        }
        if (v->required == 0) parses(v->name, CMD_RUN);
        if (v->nargs >= 1) {
            snprintf(line, sizeof line, "%s some_name", v->name);
            if (v->nargs == 1) parses(line, CMD_RUN);
        }
        if (v->nargs >= 2) {
            snprintf(line, sizeof line, "%s some_name other_one", v->name);
            parses(line, CMD_RUN);
        }
        for (int k = 0; v->aliases[k]; k++) {
            const char *args = v->required == 0 ? ""
                               : v->nargs == 1 ? " some_name"
                                              : " some_name other_one";
            snprintf(line, sizeof line, "%s%s", v->aliases[k], args);
            parses(line, CMD_RUN);
        }
        for (int f = 0; f < v->nflags; f++) {
            if (v->flags[f].value)
                snprintf(line, sizeof line, "%s a_take --%s 30", v->name,
                         v->flags[f].name);
            else
                snprintf(line, sizeof line, "%s --%s", v->name,
                         v->flags[f].name);
            parses(line, CMD_RUN);
        }
    }
    Command c;
    char err[768];
    parse_line("mv my_long_name my_folder", &c, err, sizeof err);
    CHECK(c.argc == 2 && strcmp(c.arg[0], "my_long_name") == 0
              && strcmp(c.arg[1], "my_folder") == 0,
          "two-argument split: '%s' / '%s'", c.arg[0], c.arg[1]);
    parse_line("rec a_take --end 30", &c, err, sizeof err);
    CHECK(c.has_end && c.end == 30.0f && strcmp(c.arg[0], "a_take") == 0,
          "rec flag: end=%g name='%s'", (double)c.end, c.arg[0]);
    parse_line("mod cc cc7 index", &c, err, sizeof err);
    CHECK(c.cc == 7 && c.target == CC_INDEX, "mod cc parsed cc%d target %d", c.cc,
          c.target);
    parse_line("mod cc all off", &c, err, sizeof err);
    CHECK(c.cc_all, "mod cc all off");
    parse_line("   ", &c, err, sizeof err);
    CHECK(c.kind == CMD_NOP, "blank line is a no-op");
    char echo[256];
    parse_line("mv drift lab", &c, err, sizeof err);
    command_echo(&c, echo, sizeof echo);
    CHECK(strcmp(echo, "mv drift lab") == 0, "echo '%s'", echo);
}

static void help_on_every_verb(void) {
    static const char *const FORMS[] = {"-h", "--help", "?"};
    for (int i = 0; i < verb_count(); i++) {
        const Verb *v = verb_at(i);
        for (size_t f = 0; f < 3; f++) {
            char line[128], want[128];
            snprintf(line, sizeof line, "%s %s", v->name, FORMS[f]);
            snprintf(want, sizeof want, "usage: %s", v->name);
            Command c;
            char err[768];
            bool ok = parse_line(line, &c, err, sizeof err);
            CHECK(ok && c.kind == CMD_HELP, "'%s' is not help", line);
            CHECK(strncmp(c.text, want, strlen(want)) == 0,
                  "'%s' help starts '%s', wanted '%s'", line, c.text, want);
            CHECK(strstr(c.text, v->about) != NULL, "'%s' help lacks its about",
                  line);
        }
    }
    Command c;
    char err[768];
    parse_line("rec -h", &c, err, sizeof err);
    CHECK(strstr(c.text, "--end <seconds>") != NULL, "rec help lists --end");
    CHECK(strstr(c.text, "also: record") != NULL, "rec help lists its alias");
    parse_line("help", &c, err, sizeof err);
    int lines = 1;
    for (const char *p = c.text; *p; p++)
        if (*p == '\n') lines++;
    CHECK(lines <= 20, "help is %d lines", lines);
    for (int i = 0; i < verb_count(); i++)
        CHECK(strstr(c.text, verb_at(i)->name) != NULL, "help omits %s",
              verb_at(i)->name);
    parse_line("help presets", &c, err, sizeof err);
    CHECK(strstr(c.text, "undo") != NULL && strstr(c.text, "trashed") != NULL,
          "help presets has full rows");
    CHECK(!parse_line("help nonsense", &c, err, sizeof err),
          "help of an unknown topic parsed");
}

static void two_line_errors(void) {
    error_lines("save", "save wants a name", "usage: save <name>");
    error_lines("mv drift", "mv wants <path> <destination>",
                "usage: mv <path> <destination>");
    error_lines("undo now", "undo takes nothing", "usage: undo");
    error_lines("rec --end", "--end wants a seconds", "usage: rec");
    error_lines("rec --end soon", "--end wants a number of seconds, not 'soon'",
                "usage: rec");
    error_lines("save --loud x", "save has no flag --loud", "usage: save");
    error_lines("mv drift - lab", "mv takes 2 arguments", "usage: mv");
    error_lines("mod cc 300 index", "'300' is not a controller: cc wants 0 to 127",
                "usage: mod");
    error_lines("mod cc x off", "'x' is not a controller: cc wants 0 to 127",
                "usage: mod");
    error_lines("save ../x", "'../x' is not a name or one-level preset path",
                "usage: save");
    error_lines("delet drift", "no verb called delet; did you mean rm?",
                "help lists every verb");
    error_lines("recrd", "no verb called recrd; did you mean rec?",
                "help lists every verb");
    error_lines("xqzvwp", "no verb called xqzvwp", "help lists every verb");
    Command c;
    char err[768];
    CHECK(!parse_line("mod cc 7 loudness", &c, err, sizeof err),
          "unknown control parsed");
    CHECK(strstr(err, "index") != NULL, "unknown control lists the controls");
}

/* ---------- completion ---------- */

static bool has_candidate(const LineState *s, const char *want) {
    for (int i = 0; i < s->ncand; i++)
        if (strcmp(s->cand[i], want) == 0) return true;
    return false;
}

static void completion_uses_live_preset_state(void) {
    app.preset_count = 2;
    snprintf(app.preset_names[0].bank, sizeof app.preset_names[0].bank,
             "%s", STOCK_BANK);
    snprintf(app.preset_names[0].name, sizeof app.preset_names[0].name,
             "tokyo_ghost");
    app.preset_names[1].bank[0] = 0;
    snprintf(app.preset_names[1].name, sizeof app.preset_names[1].name,
             "drift");
    app.folder_count = 2;
    snprintf(app.preset_folders[0], sizeof app.preset_folders[0], "%s",
             STOCK_BANK);
    snprintf(app.preset_folders[1], sizeof app.preset_folders[1], "lab");

    LineState s;
    line_state(&app, "load BYPO/to", &s);
    CHECK(has_candidate(&s, "BYPO/tokyo_ghost"),
          "load did not complete a qualified preset");
    line_state(&app, "cd l", &s);
    CHECK(has_candidate(&s, "lab"), "cd did not complete a live folder");
    line_state(&app, "ls ", &s);
    CHECK(has_candidate(&s, "ALL") && has_candidate(&s, "USER")
              && has_candidate(&s, "mod"),
          "ls did not complete its views");
    line_state(&app, "mv USER/drift ", &s);
    CHECK(has_candidate(&s, "USER") && has_candidate(&s, "lab"),
          "mv did not complete writable destinations");
    CHECK(!has_candidate(&s, "BYPO"), "mv offered its read-only destination");
    line_state(&app, "rm -r l", &s);
    CHECK(has_candidate(&s, "lab"), "rm -r did not complete a folder");
    line_state(&app, "mod cc 7 in", &s);
    CHECK(has_candidate(&s, "index"), "mod cc did not complete controls");
    line_state(&app, "mod cc ", &s);
    CHECK(has_candidate(&s, "all"), "mod cc did not complete all");
    line_state(&app, "clock ", &s);
    CHECK(has_candidate(&s, "host") && has_candidate(&s, "set"),
          "clock did not complete its sources");
    line_state(&app, "where ", &s);
    CHECK(has_candidate(&s, "presets") && has_candidate(&s, "recordings")
              && has_candidate(&s, "assets"),
          "where did not complete its folders");

    char completed[256];
    line_state(&app, "load BYPO/to", &s);
    CHECK(line_take("load BYPO/to", &s, 0, completed, sizeof completed)
              && strcmp(completed, "load BYPO/tokyo_ghost ") == 0,
          "completion produced '%s'", completed);

    line_state(&app, "drone o", &s);
    CHECK(has_candidate(&s, "on") && has_candidate(&s, "off"),
          "drone did not complete on/off");
    line_state(&app, "op 3 ", &s);
    CHECK(has_candidate(&s, "on") && has_candidate(&s, "off"),
          "op did not complete on/off");
    line_state(&app, "mel tuning p", &s);
    CHECK(has_candidate(&s, "phi powers") && has_candidate(&s, "phi walk"),
          "mel did not complete tuning names");
    line_state(&app, "chandas time 1/4", &s);
    CHECK(has_candidate(&s, "1/4") && has_candidate(&s, "1/4T"),
          "chandas did not complete divisions");
}

static void switches_query_and_take_explicit_state(void) {
    char err[768];
    app.engaged = true;
    app.shadow_melody.enabled = false;
    app.shadow_melody.rate_hz = PHI;
    app.shadow.ops[1].enabled = true;
    app.shadow.ops[1].ratio = 2.0f;
    app.shadow.ops[1].level = 0.75f;

    CHECK(run_ok("drone", err, sizeof err), "drone query: %s", err);
    CHECK(app.engaged, "drone query changed its state");
    CHECK(run_ok("mel", err, sizeof err), "mel query: %s", err);
    CHECK(!app.shadow_melody.enabled, "mel query changed its state");
    CHECK(run_ok("op 2", err, sizeof err), "op query: %s", err);
    CHECK(app.shadow.ops[1].enabled, "op query changed its state");

    CHECK(run_ok("drone off", err, sizeof err), "drone off: %s", err);
    CHECK(!app.engaged, "drone off left it on");
    CHECK(run_ok("mel on", err, sizeof err), "mel on: %s", err);
    CHECK(app.shadow_melody.enabled, "mel on left it off");
    CHECK(run_ok("op 2 off", err, sizeof err), "op off: %s", err);
    CHECK(!app.shadow.ops[1].enabled, "op 2 off left it on");
    CHECK(run_ok("op 2 ratio 3.5", err, sizeof err), "op ratio: %s", err);
    CHECK(run_ok("op 2 detune -7", err, sizeof err), "op detune: %s", err);
    CHECK(run_ok("op 2 level 0.6", err, sizeof err), "op level: %s", err);
    CHECK(app.shadow.ops[1].ratio == 3.5f
              && app.shadow.ops[1].detune_cents == -7.0f
              && app.shadow.ops[1].level == 0.6f,
          "operator fields did not land");
    CHECK(run_ok("op 2 ratio", err, sizeof err), "op ratio query: %s", err);
    CHECK(!run_ok("op 2 ratio 100", err, sizeof err),
          "op accepted an out-of-range ratio");

    Command c;
    CHECK(!parse_line("drone toggle", &c, err, sizeof err)
              && strstr(err, "wants on or off"),
          "drone accepted an implicit toggle: %s", err);
    CHECK(!parse_line("op 0 on", &c, err, sizeof err)
              && strstr(err, "1 to 5"),
          "op accepted zero: %s", err);
}

static void direct_controls_use_real_units(void) {
    char err[768];
    app.shadow = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    app.shadow_verb = verb_params_default();
    app.chain = chain_default();

    CHECK(run_ok("alg 4", err, sizeof err), "alg: %s", err);
    CHECK(app.shadow.algorithm == ALGORITHMS[3], "alg did not set IV");
    CHECK(run_ok("mode harmonic", err, sizeof err), "mode: %s", err);
    CHECK(app.shadow.ratio_mode == RATIO_HARMONIC
              && app.shadow.ops[4].ratio == ratio_mode_ratio(RATIO_HARMONIC, 4),
          "mode did not load the harmonic ratios");
    CHECK(run_ok("glide 250ms", err, sizeof err) == false,
          "glide guessed milliseconds");
    CHECK(run_ok("glide 0.25s", err, sizeof err), "glide seconds: %s", err);
    CHECK(app.shadow.glide_seconds == 0.25f, "glide is %g",
          (double)app.shadow.glide_seconds);
    CHECK(run_ok("detune 12cents", err, sizeof err), "detune cents: %s", err);
    CHECK(app.shadow.unison_detune == 12.0f, "detune is %g",
          (double)app.shadow.unison_detune);
    CHECK(run_ok("hz 220hz", err, sizeof err), "hz: %s", err);
    CHECK(app.drone_hz == 220.0f, "drone hz is %g", (double)app.drone_hz);
    CHECK(run_ok("note 45", err, sizeof err), "note: %s", err);
    CHECK(app.drone_hz == midi_to_hz(45), "note did not become real hz");
    CHECK(run_ok("attack 0.008s", err, sizeof err), "attack: %s", err);
    CHECK(app.shadow_attack_s == 0.008f, "attack is %g",
          (double)app.shadow_attack_s);
    CHECK(run_ok("reverb_decay 4seconds", err, sizeof err),
          "reverb decay: %s", err);
    CHECK(app.shadow_verb.decay == 4.0f, "room decay is %g",
          (double)app.shadow_verb.decay);
    CHECK(run_ok("level 0.8", err, sizeof err), "level: %s", err);
    CHECK(app.shadow.master_level == 0.8f, "level is %g",
          (double)app.shadow.master_level);
    CHECK(!run_ok("damp 1", err, sizeof err) && strstr(err, "0.99"),
          "damp accepted an out-of-range value: %s", err);

    app.shadow.voices = 1;
    app.shadow.unison = 1;
    CHECK(run_ok("poly", err, sizeof err), "poly query: %s", err);
    CHECK(app.shadow.voices == 1, "poly query changed voices");
    CHECK(run_ok("poly on", err, sizeof err), "poly on: %s", err);
    CHECK(app.shadow.voices == POLY_MAX, "poly did not use POLY_MAX");
    CHECK(run_ok("unison on", err, sizeof err), "unison on: %s", err);
    CHECK(app.shadow.unison == UNISON_MAX, "unison did not use UNISON_MAX");
}

static void grouped_controls_query_and_set(void) {
    char err[768];
    app.shadow_melody = melody_params_default();
    app.shadow_chandas = chandas_params_default();

    MelodyParams before_mel = app.shadow_melody;
    CHECK(run_ok("mel", err, sizeof err), "mel query: %s", err);
    CHECK(memcmp(&before_mel, &app.shadow_melody, sizeof before_mel) == 0,
          "mel query changed state");
    CHECK(run_ok("mel src xorshift", err, sizeof err), "mel src: %s", err);
    CHECK(app.shadow_melody.source == HOLD_XORSHIFT, "mel src did not land");
    CHECK(run_ok("mel tuning phi powers", err, sizeof err), "mel tuning: %s", err);
    CHECK(app.shadow_melody.tuning == TUNING_GOLDEN_POWERS,
          "mel tuning did not land");
    CHECK(run_ok("mel scale nat minor", err, sizeof err), "mel scale: %s", err);
    CHECK(app.shadow_melody.scale == SCALE_NATURAL_MINOR,
          "mel scale did not land");
    CHECK(run_ok("mel root 48", err, sizeof err), "mel root: %s", err);
    CHECK(run_ok("mel range 11", err, sizeof err), "mel range: %s", err);
    CHECK(run_ok("mel rate 2.5hz", err, sizeof err), "mel rate: %s", err);
    CHECK(app.shadow_melody.root_midi == 48
              && app.shadow_melody.range_degrees == 11
              && app.shadow_melody.rate_hz == 2.5f,
          "mel numeric controls did not land");
    CHECK(!run_ok("mel root 48.5", err, sizeof err),
          "mel accepted a fractional MIDI note");

    ChandasParams before_chandas = app.shadow_chandas;
    CHECK(run_ok("chandas", err, sizeof err), "chandas query: %s", err);
    CHECK(memcmp(&before_chandas, &app.shadow_chandas, sizeof before_chandas) == 0,
          "chandas query changed state");
    CHECK(run_ok("chandas sync", err, sizeof err), "chandas sync query: %s", err);
    CHECK(app.shadow_chandas.sync, "sync query changed state");
    CHECK(run_ok("chandas sync off", err, sizeof err), "chandas sync off: %s", err);
    CHECK(!app.shadow_chandas.sync, "sync stayed on");
    CHECK(run_ok("chandas bypass on", err, sizeof err), "chandas bypass: %s", err);
    CHECK(!app.shadow_chandas.enabled, "bypass left Chandas enabled");
    CHECK(run_ok("chandas time 1/8T", err, sizeof err), "chandas time: %s", err);
    CHECK(strcmp(CHANDAS_DIVISIONS[app.shadow_chandas.division].name, "1/8T") == 0,
          "division did not land");
    CHECK(run_ok("chandas rate 3hz", err, sizeof err), "chandas rate: %s", err);
    CHECK(run_ok("chandas size 1.5", err, sizeof err), "chandas size: %s", err);
    CHECK(run_ok("chandas dim 0.4", err, sizeof err), "chandas dim: %s", err);
    CHECK(app.shadow_chandas.rate_hz == 3.0f
              && app.shadow_chandas.size == 1.5f
              && app.shadow_chandas.dimension == 0.4f,
          "Chandas numeric controls did not land");
    CHECK(!run_ok("chandas size 3", err, sizeof err),
          "Chandas accepted an oversized grain");

    CHECK(run_ok("get index", err, sizeof err), "get index: %s", err);
    CHECK(run_ok("get op 2", err, sizeof err), "get op: %s", err);
    CHECK(run_ok("status envelope", err, sizeof err), "status: %s", err);
    CHECK(run_ok("show room", err, sizeof err), "show alias: %s", err);
    CHECK(run_ok("set index 0.37", err, sizeof err), "generic set: %s", err);
    CHECK(app.shadow.index == 0.37f, "generic set index did not land");
    CHECK(run_ok("set chandas warp 0.6", err, sizeof err),
          "generic grouped set: %s", err);
    CHECK(app.shadow_chandas.warp == 0.6f,
          "generic set chandas warp did not land");
    CHECK(!run_ok("set damp 2", err, sizeof err) && strstr(err, "0.99"),
          "generic set bypassed the canonical range: %s", err);
}

/* ---------- history ---------- */

/* what the console does with a line: remember every submitted non-empty line,
   then try to parse and run it */
static bool run_line(App *a, History *h, const char *line) {
    history_push(h, line);
    Command c;
    char err[768];
    if (!parse_line(line, &c, err, sizeof err)) return false;
    if (c.kind == CMD_RUN && !command_run(a, &c, err, sizeof err)) return false;
    return true;
}

static void history_records_every_submitted_line(void) {
    History h;
    memset(&h, 0, sizeof h);
    h.cursor = -1;
    CHECK(history_up(&h) == NULL, "empty history walks");
    CHECK(run_line(&app, &h, "mod cc 7 index"), "mod cc ran");
    CHECK(bind_calls == 1 && last_cc == 7, "mod cc reached the rig");
    CHECK(!run_line(&app, &h, "mod cc 900 index"), "bad mod cc ran");
    CHECK(!run_line(&app, &h, "rm nothere"), "delete of nothing ran");
    CHECK(run_line(&app, &h, "mod cc all off"), "mod cc all off ran");
    CHECK(unbind_calls == 1 && last_unbind == -1, "mod cc all off reached the rig");
    CHECK(run_line(&app, &h, "mod cc all off"), "repeat ran");
    CHECK(run_line(&app, &h, "   "), "blank ran");
    CHECK(h.len == 4, "history holds %d lines, wanted 4", h.len);
    CHECK(strcmp(history_up(&h), "mod cc all off") == 0, "up 1");
    CHECK(strcmp(history_up(&h), "rm nothere") == 0, "up 2");
    CHECK(strcmp(history_up(&h), "mod cc 900 index") == 0, "up 3");
    CHECK(strcmp(history_up(&h), "mod cc 7 index") == 0, "up 4");
    CHECK(strcmp(history_up(&h), "mod cc 7 index") == 0,
          "up stays on oldest");
    CHECK(strcmp(history_down(&h), "mod cc 900 index") == 0, "down 1");
    CHECK(strcmp(history_down(&h), "rm nothere") == 0, "down 2");
    CHECK(strcmp(history_down(&h), "mod cc all off") == 0, "down 3");
    CHECK(history_down(&h) == NULL, "down off the newest end");
    CHECK(history_up(&h) != NULL && strcmp(h.line[h.cursor], "mod cc all off") == 0,
          "up after leaving starts at the newest");
    for (int i = 0; i < HISTORY_MAX + 5; i++) {
        char line[64];
        snprintf(line, sizeof line, "mod cc cc%d off", i % 100);
        history_push(&h, line);
    }
    CHECK(h.len == HISTORY_MAX, "history capped at %d, is %d", HISTORY_MAX,
          h.len);
}

static void utility_clock_transport_and_midi_commands(void) {
    char err[768];

    app.shadow.index = 0.2f;
    CHECK(run_ok("init", err, sizeof err), "init: %s", err);
    CHECK(app.shadow.index == session_default().patch.index && !app.have_loaded
              && app.tempo_source == TEMPO_INTERNAL,
          "init did not restore the default session");

    CHECK(run_ok("tempo 96", err, sizeof err), "tempo: %s", err);
    CHECK(app.tempo_bpm == 96.0f && app.tempo_source == TEMPO_INTERNAL,
          "tempo did not select the internal clock");
    CHECK(run_ok("clock host", err, sizeof err), "clock host: %s", err);
    CHECK(app.tempo_source == TEMPO_HOST, "clock host did not land");
    CHECK(run_ok("clock set 123", err, sizeof err), "clock set: %s", err);
    CHECK(app.tempo_source == TEMPO_INTERNAL && app.tempo_bpm == 123.0f,
          "clock set did not land");
    CHECK(!run_ok("tempo 301", err, sizeof err), "tempo accepted 301");

    CHECK(run_ok("stop", err, sizeof err) && !app.transport_running,
          "stop did not halt transport");
    CHECK(run_ok("start", err, sizeof err) && app.transport_running,
          "start did not run transport");
    CHECK(run_ok("panic", err, sizeof err) && !app.transport_running,
          "panic did not halt transport");
    CHECK(run_ok("reset", err, sizeof err), "reset: %s", err);
    CHECK(run_ok("bend -1.5", err, sizeof err), "bend: %s", err);
    CHECK(!run_ok("bend 2.1", err, sizeof err), "bend accepted 2.1");

    CHECK(run_ok("midi", err, sizeof err), "midi list: %s", err);
    CHECK(run_ok("midi Test MIDI", err, sizeof err) && app.midi_open,
          "midi port did not open");
    CHECK(run_ok("midi none", err, sizeof err) && !app.midi_open,
          "midi port did not close");
    CHECK(!run_ok("midi Missing", err, sizeof err), "missing MIDI port opened");

    CHECK(run_ok("where presets", err, sizeof err), "where presets: %s", err);
    CHECK(!run_ok("where nowhere", err, sizeof err), "where accepted nowhere");
    app.log_len = 3;
    app.log_head = 3;
    CHECK(run_ok("cls", err, sizeof err) && app.log_len == 0 && app.log_head == 0,
          "cls did not clear the log");

    app.hosted = true;
    CHECK(!run_ok("quit", err, sizeof err) && !app.quit,
          "quit ran in the plugin");
    app.hosted = false;
    CHECK(run_ok("quit", err, sizeof err) && app.quit,
          "quit did not close the standalone");
    app.quit = false;
}

/* ---------- trash and undo ---------- */

static bool run_ok(const char *line, char *err, size_t cap) {
    Command c;
    if (!parse_line(line, &c, err, cap)) return false;
    return command_run(&app, &c, err, cap);
}

static void trash_and_undo_round_trip(void) {
    char err[768];
    char drift[1024], trashed[1024], lab[1024], lab_drift[1024], drifted[1024];
    in_dir("drift.json", drift, sizeof drift);
    in_dir("trash/drift.json", trashed, sizeof trashed);
    in_dir("lab", lab, sizeof lab);
    in_dir("lab/drift.json", lab_drift, sizeof lab_drift);
    in_dir("drifted.json", drifted, sizeof drifted);

    Session s = session_default();
    char *json = session_to_json(&s);
    CHECK(json != NULL, "session serialises");
    if (!json) return;
    CHECK(write_file(drift, json), "wrote %s", drift);
    preset_rescan(&app);
    CHECK(app.preset_count == 1, "one preset scanned, got %d", app.preset_count);
    CHECK(!run_ok("save drift", err, sizeof err), "save replaced drift");
    CHECK(strstr(err, "use ow drift") != NULL, "duplicate save said: %s", err);

    /* delete: missing name, no highlight, then the real thing */
    CHECK(!run_ok("rm nothere", err, sizeof err), "rm nothere ran");
    CHECK(strncmp(err, "no preset called nothere\n", 25) == 0,
          "rm nothere said: %s", err);
    app.have_selected = false;
    CHECK(!run_ok("rm", err, sizeof err), "bare rm ran");
    CHECK(strncmp(err, "rm wants a preset\n", 18) == 0, "bare rm said: %s",
          err);
    CHECK(run_ok("rm USER/drift", err, sizeof err), "rm path: %s", err);
    CHECK(!present(drift) && present(trashed), "drift moved to the trash");
    char what[256];
    CHECK(undo_pending(what, sizeof what) && strcmp(what, "USER/drift") == 0,
          "journal holds '%s'", what);
    CHECK(run_ok("undo", err, sizeof err), "undo: %s", err);
    CHECK(present(drift) && !present(trashed), "undo put drift back");
    CHECK(!undo_pending(NULL, 0), "journal cleared");
    CHECK(!run_ok("undo", err, sizeof err) && strncmp(err, "nothing to undo", 15) == 0,
          "second undo: %s", err);

    /* delete through the highlight, and a second copy in the trash */
    app.preset_selected = app.preset_names[0];
    app.have_selected = true;
    CHECK(run_ok("rm", err, sizeof err), "rm highlighted: %s", err);
    CHECK(!app.have_selected, "highlight dropped with the file");
    CHECK(write_file(drift, json), "rewrote drift");
    preset_rescan(&app);
    CHECK(run_ok("rm selected", err, sizeof err) == false,
          "delete selected with no highlight ran");
    CHECK(run_ok("rm drift", err, sizeof err), "rm drift again: %s", err);
    char trashed2[1024];
    in_dir("trash/drift~1.json", trashed2, sizeof trashed2);
    CHECK(present(trashed) && present(trashed2), "second copy kept as ~1");
    CHECK(run_ok("undo", err, sizeof err), "undo the second: %s", err);
    CHECK(present(drift) && !present(trashed2), "undo restored the ~1 copy");

    /* add, move, undo */
    CHECK(run_ok("mkdir lab", err, sizeof err), "mkdir lab: %s", err);
    CHECK(is_dir(lab), "lab made");
    CHECK(!run_ok("mkdir lab", err, sizeof err), "mkdir lab twice ran");
    CHECK(run_ok("rmdir lab", err, sizeof err), "rmdir lab: %s", err);
    CHECK(!is_dir(lab), "rmdir left lab behind");
    CHECK(run_ok("mkdir lab", err, sizeof err), "remade lab: %s", err);
    CHECK(!run_ok("mkdir trash", err, sizeof err), "mkdir trash ran");
    CHECK(!run_ok("mv drift nowhere/drift", err, sizeof err), "move to nowhere ran");
    CHECK(strncmp(err, "no folder called nowhere", 24) == 0, "move said: %s", err);
    CHECK(run_ok("mv drift lab", err, sizeof err), "move: %s", err);
    CHECK(!present(drift) && present(lab_drift), "drift filed under lab");
    CHECK(run_ok("cd lab", err, sizeof err), "cd lab: %s", err);
    CHECK(app.preset_filter.kind == FILTER_BANK
              && strcmp(app.preset_filter.bank, "lab") == 0,
          "cd did not select lab");
    CHECK(run_ok("ls", err, sizeof err), "ls lab: %s", err);
    CHECK(run_ok("load lab/drift", err, sizeof err), "load path: %s", err);
    CHECK(app.have_loaded && strcmp(app.preset_loaded.bank, "lab") == 0,
          "load path did not load lab/drift");
    CHECK(!run_ok("rmdir lab", err, sizeof err), "rmdir removed nonempty lab");
    CHECK(run_ok("undo", err, sizeof err), "undo move: %s", err);
    CHECK(present(drift) && !present(lab_drift), "undo unfiled it");

    /* rename, undo */
    CHECK(run_ok("mv drift drifted", err, sizeof err), "rename: %s", err);
    CHECK(!present(drift) && present(drifted), "renamed on disk");
    CHECK(run_ok("undo", err, sizeof err), "undo rename: %s", err);
    CHECK(present(drift) && !present(drifted), "undo renamed it back");

    /* overwrite keeps the old file in the trash; undo brings it back */
    CHECK(write_file(drift, "{\"marker\": true}"), "marked drift");
    CHECK(run_ok("ow drift", err, sizeof err), "overwrite: %s", err);
    char text[4096];
    CHECK(read_file(drift, text, sizeof text) && strstr(text, "marker") == NULL,
          "overwrite wrote the current sound");
    /* trash/drift.json is still held by the highlighted delete above */
    CHECK(read_file(trashed2, text, sizeof text) && strstr(text, "marker") != NULL,
          "the old file sits in the trash");
    CHECK(run_ok("undo", err, sizeof err), "undo overwrite: %s", err);
    CHECK(read_file(drift, text, sizeof text) && strstr(text, "marker") != NULL,
          "undo restored the old contents");

    /* remove a folder with something in it, undo */
    CHECK(run_ok("mv drift lab", err, sizeof err), "move for remove: %s", err);
    CHECK(run_ok("rm -r lab", err, sizeof err), "remove lab: %s", err);
    char trashed_lab[1024];
    in_dir("trash/lab/drift.json", trashed_lab, sizeof trashed_lab);
    CHECK(!is_dir(lab) && present(trashed_lab), "lab and its preset in the trash");
    CHECK(run_ok("undo", err, sizeof err), "undo remove: %s", err);
    CHECK(is_dir(lab) && present(lab_drift), "undo restored the folder");
    CHECK(!run_ok("rm -r trash", err, sizeof err), "remove trash ran");
    CHECK(!run_ok("rm -r USER", err, sizeof err), "remove USER ran");
    free(json);
}

void test_console(void) {
    /* the presets suite ran first and pinned preset_dir() to a temp home it
       has since removed, so bring that directory back before using it */
    mkdir_p(preset_dir());
    CHECK(is_dir(preset_dir()), "preset dir %s", preset_dir());
    memset(&app, 0, sizeof app);
    every_verb_parses_its_forms();
    help_on_every_verb();
    two_line_errors();
    completion_uses_live_preset_state();
    switches_query_and_take_explicit_state();
    direct_controls_use_real_units();
    grouped_controls_query_and_set();
    history_records_every_submitted_line();
    utility_clock_transport_and_midi_commands();
    trash_and_undo_round_trip();
    /* the temp home is two levels up: <home>/bypo/presets */
    char root[1024];
    snprintf(root, sizeof root, "%s", preset_dir());
    for (int up = 0; up < 2; up++) {
        char *slash = strrchr(root, '/');
        if (slash) *slash = 0;
    }
    rm_rf(strstr(root, "bypo-hostile-") ? root : preset_dir());
}
