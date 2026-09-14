/* Presets are attacker-controlled input: these are the documents and file
   names a hostile one would carry. Nothing here may crash, and nothing may
   leave the parser holding a non-finite or out-of-range value. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/gui/app.h"
#include "test.h"

/* presets.c logs, posts engine events and names algorithms for the log line;
   the test binary links none of the GUI, so those are stubbed */
void push_log(App *a, const char *fmt, ...) { (void)a; (void)fmt; }
void app_send(App *a, Event ev) { (void)a; (void)ev; }
const char *const ROMAN[8] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII"};
const char *mode_name_of(RatioMode m) { (void)m; return "stub"; }
int algorithm_index_of(const Patch *p) { (void)p; return 0; }

/* ---------- helpers ---------- */

/* a Session whose every byte is recognisable, to prove a failed parse wrote
   nothing into the caller's copy */
static Session sentinel(void) {
    Session s;
    memset(&s, 0xA5, sizeof s);
    return s;
}

static bool untouched(const Session *s) {
    Session mark = sentinel();
    return memcmp(s, &mark, sizeof mark) == 0;
}

static void rejects(const char *json, const char *what) {
    Session s = sentinel();
    bool ok = session_from_json(json, &s);
    CHECK(!ok, "accepted %s", what);
    CHECK(untouched(&s), "output written despite refusing %s", what);
}

static void accepts(const char *json, Session *out, const char *what) {
    Session s = sentinel();
    bool ok = session_from_json(json, &s);
    CHECK(ok, "refused %s", what);
    /* the caller reads *out either way, so a refusal leaves it a real Session */
    *out = ok ? s : session_default();
}

static bool in_range(float v, float lo, float hi) {
    return isfinite(v) && v >= lo && v <= hi;
}

/* every documented clamp in session_sanitize, checked from the outside */
static void check_clamped(const Session *s, const char *what) {
    bool known = false;
    for (int i = 0; i < 8; i++)
        if (ALGORITHMS[i] == s->patch.algorithm) known = true;
    CHECK(known, "%s: algorithm %u not in the table", what,
          (unsigned)s->patch.algorithm);
    CHECK((unsigned)s->patch.ratio_mode < (unsigned)RATIO_MODE_COUNT,
          "%s: ratio_mode %d", what, (int)s->patch.ratio_mode);
    CHECK((unsigned)s->melody.tuning < (unsigned)NUM_TUNINGS, "%s: tuning %d",
          what, (int)s->melody.tuning);
    CHECK((unsigned)s->melody.scale < (unsigned)NUM_SCALES, "%s: scale %d",
          what, (int)s->melody.scale);
    CHECK(in_range(s->patch.feedback, 0.0f, 1.0f), "%s: feedback %g", what,
          (double)s->patch.feedback);
    CHECK(in_range(s->patch.index, 0.0f, 1.0f), "%s: index %g", what,
          (double)s->patch.index);
    CHECK(in_range(s->patch.rip, 0.0f, 1.0f), "%s: rip %g", what,
          (double)s->patch.rip);
    CHECK(in_range(s->patch.master_level, 0.0f, 1.0f), "%s: master_level %g",
          what, (double)s->patch.master_level);
    CHECK(in_range(s->patch.glide_seconds, 0.0f, 30.0f), "%s: glide %g", what,
          (double)s->patch.glide_seconds);
    CHECK(in_range(s->patch.field, 0.0f, 1.0f), "%s: field %g", what,
          (double)s->patch.field);
    CHECK(in_range(s->patch.curve, 0.0f, 1.0f), "%s: curve %g", what,
          (double)s->patch.curve);
    for (int i = 0; i < NUM_OPS; i++) {
        CHECK(in_range(s->patch.ops[i].level, 0.0f, 1.0f), "%s: op%d level %g",
              what, i, (double)s->patch.ops[i].level);
        CHECK(in_range(s->patch.ops[i].ratio, 0.01f, 64.0f), "%s: op%d ratio %g",
              what, i, (double)s->patch.ops[i].ratio);
        CHECK(in_range(s->patch.ops[i].detune_cents, -1200.0f, 1200.0f),
              "%s: op%d detune %g", what, i,
              (double)s->patch.ops[i].detune_cents);
    }
    CHECK(in_range(s->verb.mix, 0.0f, 1.0f), "%s: verb mix %g", what,
          (double)s->verb.mix);
    CHECK(in_range(s->verb.ghost, 0.0f, 1.0f), "%s: ghost %g", what,
          (double)s->verb.ghost);
    CHECK(in_range(s->verb.decay, 0.05f, 8.0f), "%s: decay %g", what,
          (double)s->verb.decay);
    CHECK(in_range(s->verb.damp, 0.0f, 0.99f), "%s: damp %g", what,
          (double)s->verb.damp);
    CHECK(in_range(s->verb.haunt, 0.0f, 1.0f), "%s: haunt %g", what,
          (double)s->verb.haunt);
    CHECK(in_range(s->melody.rate_hz, 0.1f, 8.0f), "%s: melody rate %g", what,
          (double)s->melody.rate_hz);
    CHECK(s->melody.root_midi >= 24 && s->melody.root_midi <= 57,
          "%s: root_midi %u", what, s->melody.root_midi);
    CHECK(s->melody.range_degrees >= 1 && s->melody.range_degrees <= 13,
          "%s: range %u", what, s->melody.range_degrees);
    CHECK(in_range(s->drone_hz, 27.5f, 440.0f), "%s: drone_hz %g", what,
          (double)s->drone_hz);
    CHECK(in_range(s->chandas.mix, 0.0f, 1.0f), "%s: chandas mix %g", what,
          (double)s->chandas.mix);
    CHECK(s->chandas.division < (size_t)CHANDAS_DIVISIONS_LEN,
          "%s: division %zu", what, s->chandas.division);
    CHECK(in_range(s->chandas.rate_hz, 0.1f, 8.0f), "%s: chandas rate %g", what,
          (double)s->chandas.rate_hz);
    CHECK(in_range(s->chandas.spread, 0.0f, 1.0f), "%s: spread %g", what,
          (double)s->chandas.spread);
    CHECK(in_range(s->chandas.size, CHANDAS_MIN_SIZE, CHANDAS_MAX_SIZE),
          "%s: size %g", what, (double)s->chandas.size);
    CHECK(in_range(s->chandas.warp, 0.0f, 1.0f), "%s: warp %g", what,
          (double)s->chandas.warp);
    CHECK(in_range(s->chandas.dimension, 0.0f, 1.0f), "%s: dimension %g", what,
          (double)s->chandas.dimension);
    CHECK(in_range(s->chandas.tail, 0.0f, 1.0f), "%s: tail %g", what,
          (double)s->chandas.tail);
    CHECK(in_range(s->tempo_bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM),
          "%s: tempo %g", what, (double)s->tempo_bpm);
    CHECK(in_range(s->warmth, MIN_WARMTH, MAX_WARMTH), "%s: warmth %g", what,
          (double)s->warmth);
}

/* every numeric field, as the enclosing object and the key inside it */
typedef struct {
    const char *open, *close, *key;
} NumField;

static const NumField NUM_FIELDS[] = {
    {"", "", "drone_hz"},
    {"", "", "tempo_bpm"},
    {"", "", "warmth"},
    {"\"patch\": {", "}", "algorithm"},
    {"\"patch\": {", "}", "feedback"},
    {"\"patch\": {", "}", "index"},
    {"\"patch\": {", "}", "rip"},
    {"\"patch\": {", "}", "master_level"},
    {"\"patch\": {", "}", "glide_seconds"},
    {"\"patch\": {", "}", "field"},
    {"\"patch\": {", "}", "curve"},
    {"\"patch\": {\"ops\": [{", "}]}", "ratio"},
    {"\"patch\": {\"ops\": [{", "}]}", "detune_cents"},
    {"\"patch\": {\"ops\": [{", "}]}", "level"},
    {"\"verb\": {", "}", "mix"},
    {"\"verb\": {", "}", "ghost"},
    {"\"verb\": {", "}", "decay"},
    {"\"verb\": {", "}", "damp"},
    {"\"verb\": {", "}", "haunt"},
    {"\"melody\": {", "}", "root_midi"},
    {"\"melody\": {", "}", "range_degrees"},
    {"\"melody\": {", "}", "rate_hz"},
    {"\"chandas\": {", "}", "mix"},
    {"\"chandas\": {", "}", "division"},
    {"\"chandas\": {", "}", "rate_hz"},
    {"\"chandas\": {", "}", "spread"},
    {"\"chandas\": {", "}", "size"},
    {"\"chandas\": {", "}", "warp"},
    {"\"chandas\": {", "}", "dimension"},
    {"\"chandas\": {", "}", "tail"},
};
#define NUM_FIELD_COUNT (sizeof NUM_FIELDS / sizeof NUM_FIELDS[0])

static void one_number_doc(char *out, size_t cap, const NumField *f,
                           const char *literal) {
    snprintf(out, cap, "{%s\"%s\": %s%s}", f->open, f->key, literal, f->close);
}

/* ---------- cases ---------- */

static void a_hundred_thousand_brackets_are_refused_not_followed(void) {
    const size_t n = 100000;
    char *doc = malloc(8 * n + 64);
    CHECK(doc != NULL, "allocation for the nesting case failed");
    if (!doc) return;

    /* arrays, closed */
    size_t at = 0;
    at += (size_t)sprintf(doc + at, "{\"junk\": ");
    memset(doc + at, '[', n);
    at += n;
    memset(doc + at, ']', n);
    at += n;
    doc[at++] = '}';
    doc[at] = 0;
    rejects(doc, "100k nested arrays");

    /* arrays, never closed */
    at = 0;
    at += (size_t)sprintf(doc + at, "{\"junk\": ");
    memset(doc + at, '[', n);
    at += n;
    doc[at] = 0;
    rejects(doc, "100k unterminated arrays");

    /* objects, through a key the parser recognises */
    at = (size_t)sprintf(doc, "{\"patch\": ");
    for (size_t i = 0; i < n; i++) {
        memcpy(doc + at, "{\"a\":", 5);
        at += 5;
    }
    doc[at++] = '1';
    memset(doc + at, '}', n);
    at += n;
    doc[at++] = '}';
    doc[at] = 0;
    rejects(doc, "100k nested objects under patch");

    free(doc);
}

static void non_finite_numbers_are_not_json(void) {
    static const char *const BAD[] = {"nan",  "NaN",       "-nan",  "nan(0)",
                                      "inf",  "Infinity",  "-inf",  "-Infinity",
                                      "0x1p3", "0X1.8p1",  "1e400", "-1e400",
                                      "1e999999", "+1",    ".5",    "01",
                                      "1.",   "1e",        "--1"};
    char doc[256];
    for (size_t f = 0; f < NUM_FIELD_COUNT; f++) {
        for (size_t b = 0; b < sizeof BAD / sizeof BAD[0]; b++) {
            one_number_doc(doc, sizeof doc, &NUM_FIELDS[f], BAD[b]);
            rejects(doc, doc);
        }
    }
}

static void huge_but_legal_numbers_land_inside_their_clamp(void) {
    static const char *const HUGE_NUMS[] = {"1e300", "-1e300", "1e-300", "-1e-300",
                                       "0", "-0", "340282350000000000000000000000000000000.0"};
    char doc[256];
    for (size_t f = 0; f < NUM_FIELD_COUNT; f++) {
        for (size_t h = 0; h < sizeof HUGE_NUMS / sizeof HUGE_NUMS[0]; h++) {
            one_number_doc(doc, sizeof doc, &NUM_FIELDS[f], HUGE_NUMS[h]);
            Session s;
            accepts(doc, &s, doc);
            check_clamped(&s, doc);
        }
    }
}

static void a_hostile_division_stays_a_real_index(void) {
    rejects("{\"chandas\": {\"division\": nan}}", "division nan");
    Session s;
    accepts("{\"chandas\": {\"division\": 1e300}}", &s, "division 1e300");
    CHECK(s.chandas.division == (size_t)(CHANDAS_DIVISIONS_LEN - 1),
          "division 1e300 landed on %zu", s.chandas.division);
    accepts("{\"chandas\": {\"division\": 0}}", &s, "division 0");
    CHECK(s.chandas.division == 0, "division 0 landed on %zu",
          s.chandas.division);
    /* the value is used as a table index, so prove it reads back */
    CHECK(CHANDAS_DIVISIONS[s.chandas.division].name != NULL,
          "division 0 has no name");
}

static void an_unknown_algorithm_only_costs_the_algorithm(void) {
    Session s;
    accepts("{\"patch\": {\"algorithm\": 4294967295, \"index\": 0.25,"
            " \"rip\": 0.75, \"feedback\": 0.125, \"curve\": 0.375},"
            " \"warmth\": 0.625, \"drone_hz\": 220.0}",
            &s, "an out-of-table algorithm number");
    check_clamped(&s, "unknown algorithm");
    CHECK(s.patch.index == 0.25f, "index %g did not survive",
          (double)s.patch.index);
    CHECK(s.patch.rip == 0.75f, "rip %g did not survive", (double)s.patch.rip);
    CHECK(s.patch.feedback == 0.125f, "feedback %g did not survive",
          (double)s.patch.feedback);
    CHECK(s.patch.curve == 0.375f, "curve %g did not survive",
          (double)s.patch.curve);
    CHECK(s.warmth == 0.625f, "warmth %g did not survive", (double)s.warmth);
    CHECK(s.drone_hz == 220.0f, "drone_hz %g did not survive",
          (double)s.drone_hz);

    /* the same rule one level down, where the reset actually happens */
    Session raw = session_default();
    raw.patch.algorithm = (AlgorithmId)200;
    raw.patch.index = 0.3f;
    raw.patch.ops[2].ratio = 3.0f;
    raw.patch.ratio_mode = RATIO_PLASTIC;
    Session out = session_sanitize(raw);
    CHECK(out.patch.algorithm == ALGORITHMS[0], "algorithm %u survived",
          (unsigned)out.patch.algorithm);
    CHECK(out.patch.index == 0.3f, "index %g lost to the algorithm reset",
          (double)out.patch.index);
    CHECK(out.patch.ops[2].ratio == 3.0f, "op ratio %g lost to the reset",
          (double)out.patch.ops[2].ratio);
    CHECK(out.patch.ratio_mode == RATIO_PLASTIC, "ratio_mode lost to the reset");
}

static void the_old_key_names_still_land(void) {
    Session s;
    accepts("{\"verb\": {\"rt60\": 2.5}}", &s, "rt60");
    CHECK(s.verb.decay == 2.5f, "rt60 landed on decay %g",
          (double)s.verb.decay);
    accepts("{\"harmony\": {\"mix\": 0.5, \"tail\": 0.25, \"division\": 3}}", &s,
            "harmony");
    CHECK(s.chandas.mix == 0.5f, "harmony mix %g", (double)s.chandas.mix);
    CHECK(s.chandas.tail == 0.25f, "harmony tail %g", (double)s.chandas.tail);
    CHECK(s.chandas.division == 3, "harmony division %zu", s.chandas.division);
}

static void a_field_of_the_wrong_type_fails_the_whole_document(void) {
    static const char *const BAD[] = {
        "{\"drone_hz\": \"440\"}",
        "{\"drone_hz\": true}",
        "{\"drone_hz\": null}",
        "{\"drone_hz\": [440]}",
        "{\"patch\": 5}",
        "{\"patch\": \"II\"}",
        "{\"patch\": []}",
        "{\"verb\": []}",
        "{\"verb\": {\"mix\": {}}}",
        "{\"melody\": {\"enabled\": 1}}",
        "{\"melody\": {\"tuning\": 2}}",
        "{\"melody\": {\"scale\": null}}",
        "{\"patch\": {\"ops\": {}}}",
        "{\"patch\": {\"ops\": 3}}",
        "{\"patch\": {\"ops\": [3]}}",
        "{\"patch\": {\"ratio_mode\": 0}}",
        "{\"chandas\": {\"sync\": \"yes\"}}",
        "{\"chandas\": {\"enabled\": 0}}",
        "{\"tempo_bpm\": }",
        "{\"tempo_bpm\"}",
        "{tempo_bpm: 120}",
        "{'tempo_bpm': 120}",
        "{\"tempo_bpm\": 120,}",
        "[]",
        "\"\"",
        "120",
        "",
        "{",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) rejects(BAD[i], BAD[i]);
}

static void anything_after_the_root_object_fails(void) {
    static const char *const BAD[] = {
        "{} x", "{}{}", "{}]", "{},", "{} null", "{\"warmth\": 0.5} 7",
        "{\"warmth\": 0.5}{\"warmth\": 0.5}",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
        rejects(BAD[i], "trailing garbage");
    /* whitespace after the root is still fine */
    Session s;
    accepts("{\"warmth\": 0.5}  \n\t\r ", &s, "trailing whitespace");
}

static void a_repeated_key_fails_the_document(void) {
    static const char *const BAD[] = {
        "{\"warmth\": 0.1, \"warmth\": 0.2}",
        "{\"chandas\": {\"mix\": 0.1, \"mix\": 0.2}}",
        "{\"verb\": {\"decay\": 1.0, \"rt60\": 2.0}}",
        "{\"verb\": {\"rt60\": 2.0, \"decay\": 1.0}}",
        "{\"chandas\": {}, \"harmony\": {}}",
        "{\"patch\": {\"index\": 0.1, \"index\": 0.1}}",
        "{\"patch\": {\"ops\": [{\"level\": 0.1, \"level\": 0.2}]}}",
        "{\"melody\": {\"rate_hz\": 1.0, \"rate_hz\": 2.0}}",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) rejects(BAD[i], BAD[i]);
}

static void raw_control_characters_are_not_json_strings(void) {
    char doc[128];
    for (int c = 1; c < 0x20; c++) {
        snprintf(doc, sizeof doc,
                 "{\"patch\": {\"algorithm\": \"ab%cd\"}}", c);
        rejects(doc, "a control character inside a string");
        snprintf(doc, sizeof doc, "{\"war%cmth\": 0.5}", c);
        rejects(doc, "a control character inside a key");
    }
    rejects("{\"patch\": {\"algorithm\": \"ab\\qcd\"}}", "an unknown escape");
    rejects("{\"patch\": {\"algorithm\": \"ab\\u00\"}}", "a short \\u escape");
    rejects("{\"patch\": {\"algorithm\": \"unterminated}", "an unterminated string");
}

static void an_overlong_string_fails_rather_than_truncating(void) {
    size_t n = 8192;
    char *doc = malloc(n + 128);
    CHECK(doc != NULL, "allocation for the long-string case failed");
    if (!doc) return;

    /* a key longer than the parser's key buffer */
    size_t at = (size_t)sprintf(doc, "{\"");
    memset(doc + at, 'k', n);
    at += n;
    at += (size_t)sprintf(doc + at, "\": 1}");
    (void)at;
    rejects(doc, "an 8k key");

    /* a value longer than the skip buffer, under a key nobody reads */
    at = (size_t)sprintf(doc, "{\"unknown\": \"");
    memset(doc + at, 'v', n);
    at += n;
    sprintf(doc + at, "\"}");
    rejects(doc, "an 8k skipped string");

    /* a glyph string longer than its destination */
    at = (size_t)sprintf(doc, "{\"patch\": {\"algorithm\": \"");
    memset(doc + at, 'x', n);
    at += n;
    sprintf(doc + at, "\"}}");
    rejects(doc, "an 8k algorithm glyph string");

    free(doc);
}

static void an_oversized_array_fails(void) {
    size_t n = 5000;
    char *doc = malloc(16 * n + 64);
    CHECK(doc != NULL, "allocation for the wide-array case failed");
    if (!doc) return;

    size_t at = (size_t)sprintf(doc, "{\"patch\": {\"ops\": [");
    for (size_t i = 0; i < n; i++) at += (size_t)sprintf(doc + at, "%s{}", i ? "," : "");
    sprintf(doc + at, "]}}");
    rejects(doc, "5000 ops");

    at = (size_t)sprintf(doc, "{\"unknown\": [");
    for (size_t i = 0; i < n; i++) at += (size_t)sprintf(doc + at, "%s0", i ? "," : "");
    sprintf(doc + at, "]}");
    rejects(doc, "a 5000-element skipped array");

    at = (size_t)sprintf(doc, "{\"unknown\": {");
    for (size_t i = 0; i < n; i++)
        at += (size_t)sprintf(doc + at, "%s\"k%zu\": 0", i ? "," : "", i);
    sprintf(doc + at, "}}");
    rejects(doc, "a 5000-key skipped object");

    free(doc);
}

/* ---------- files and names ---------- */

/* every path this suite touches lives under one temp dir, which is also the
   data home preset_dir() resolves through */
static char g_tmp[256];

static bool present(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int count_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char full[512];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            rm_rf(full);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* must run before anything calls preset_dir(), which caches its answer */
static bool tmp_home_open(void) {
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    snprintf(g_tmp, sizeof g_tmp, "%s/bypo-hostile-%ld", base, (long)getpid());
    rm_rf(g_tmp);
    if (mkdir(g_tmp, 0700) != 0) return false;
    setenv("XDG_DATA_HOME", g_tmp, 1);
    return true;
}

static void a_sixty_four_megabyte_preset_is_refused_unread(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/huge.json", g_tmp);

    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "could not open the oversized file");
    if (!f) return;
    static char pad[1 << 16];
    memset(pad, ' ', sizeof pad);
    fputs("{\"warmth\": 0.5", f);
    for (int i = 0; i < 1024; i++) fwrite(pad, 1, sizeof pad, f); /* 64 MiB */
    fputs("}", f);
    fclose(f);

    Session s = sentinel();
    CHECK(!session_load_file(path, &s), "a 64 MiB preset was read");
    CHECK(untouched(&s), "output written for the 64 MiB preset");
    unlink(path);

    /* the same loader still takes a document of a sane size */
    snprintf(path, sizeof path, "%s/small.json", g_tmp);
    f = fopen(path, "wb");
    CHECK(f != NULL, "could not open the small file");
    if (!f) return;
    fputs("{\"warmth\": 0.25}", f);
    fclose(f);
    Session ok;
    CHECK(session_load_file(path, &ok), "a small preset was refused");
    CHECK(ok.warmth == 0.25f, "small preset warmth %g", (double)ok.warmth);
    unlink(path);
}

static void a_dotted_file_name_never_becomes_a_path(void) {
    /* "..json", "...json" and ".json" reduce to the stems "..", "." and ""; a
       stem of "..." is harmless but is listed here to pin the behaviour */
    static const char *const BAD[] = {".",     "..",   "",      "a/b",
                                      "a\\b", "../x", "./x",   "/etc/passwd"};
    char path[1024];
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        PresetRef r;
        snprintf(r.bank, sizeof r.bank, "%s", "");
        snprintf(r.name, sizeof r.name, "%s", BAD[i]);
        CHECK(!preset_path(&r, path, sizeof path), "name '%s' made a path",
              BAD[i]);
        if (BAD[i][0]) {
            snprintf(r.bank, sizeof r.bank, "%s", BAD[i]);
            snprintf(r.name, sizeof r.name, "%s", "ok");
            CHECK(!preset_path(&r, path, sizeof path), "bank '%s' made a path",
                  BAD[i]);
        }
    }
    PresetRef good;
    snprintf(good.bank, sizeof good.bank, "%s", "BYPO");
    snprintf(good.name, sizeof good.name, "%s", "init");
    CHECK(preset_path(&good, path, sizeof path), "a plain preset made no path");
    CHECK(strstr(path, "..") == NULL, "a plain path carries '..': %s", path);
}

static void a_traversing_name_is_scrubbed_before_it_is_a_segment(void) {
    static const char *const BAD[] = {"../../../../tmp/pwned",
                                      "..",
                                      ".",
                                      "/etc/passwd",
                                      "..\\..\\windows",
                                      "   ",
                                      ""};
    char out[128];
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        sanitise_segment(BAD[i], out, sizeof out);
        CHECK(out[0] == 0, "'%s' sanitised to '%s'", BAD[i], out);
    }
}

static App hostile_app; /* zeroed: nothing here reaches the engine */

static void write_file(const char *dir, const char *name, const char *body) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
}

/* the scanner turns a file name into a preset name; these are the names that
   would turn back into a path segment the loader must never follow */
static void an_odd_file_name_never_becomes_a_preset(void) {
    static const char *const NAMES[] = {".json", "..json", "....json",
                                        "...json", ".json.json"};
    const char *dir = preset_dir();
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        write_file(dir, NAMES[i], "{\"warmth\": 0.5}");

    preset_rescan(&hostile_app);
    for (int i = 0; i < hostile_app.preset_count; i++) {
        const PresetRef *r = &hostile_app.preset_names[i];
        CHECK(strcmp(r->name, ".") != 0 && strcmp(r->name, "..") != 0,
              "the scanner listed a preset called '%s'", r->name);
        char path[1024];
        CHECK(preset_path(r, path, sizeof path), "listed '%s' makes no path",
              r->name);
        CHECK(strstr(path, "/../") == NULL && strstr(path, "/./") == NULL,
              "listed preset path traverses: %s", path);
    }

    /* a stem of "..." is a legal name, and it still loads from inside the dir */
    PresetRef dots;
    snprintf(dots.bank, sizeof dots.bank, "%s", "");
    snprintf(dots.name, sizeof dots.name, "%s", "...");
    char path[1024];
    CHECK(preset_path(&dots, path, sizeof path), "'...' made no path");
    CHECK(strncmp(path, dir, strlen(dir)) == 0, "'...' escaped to %s", path);
    Session s;
    CHECK(session_load_file(path, &s), "'...json' did not load");

    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, NAMES[i]);
        unlink(full);
    }
    preset_rescan(&hostile_app);
}

static void preset_save_in_writes_nothing_outside_the_preset_dir(void) {
    const char *dir = preset_dir();
    CHECK(strncmp(dir, g_tmp, strlen(g_tmp)) == 0,
          "preset_dir '%s' is not under the temp home", dir);
    if (strncmp(dir, g_tmp, strlen(g_tmp)) != 0) return;
    int before = count_entries(dir);

    preset_save_in(&hostile_app, "../../../../tmp/pwned", "evil");
    preset_save_in(&hostile_app, NULL, "../../../../tmp/pwned");
    preset_save_in(&hostile_app, "..", "evil");
    preset_save_in(&hostile_app, "BYPO", "..");
    preset_save_in(&hostile_app, "BYPO", "/etc/cron.d/x");
    preset_save_in(&hostile_app, "/etc", "evil");

    CHECK(count_entries(dir) == before, "the preset dir gained entries (%d -> %d)",
          before, count_entries(dir));
    CHECK(!present("/tmp/pwned"), "/tmp/pwned was created");
    CHECK(!present("/tmp/pwned.json"), "/tmp/pwned.json was created");
    char escaped[512];
    snprintf(escaped, sizeof escaped, "%s/pwned", g_tmp);
    CHECK(!present(escaped), "a preset escaped into the temp home root");

    /* a legitimate save still lands, and lands inside */
    preset_save_in(&hostile_app, "BYPO", "plain name");
    char want[1024];
    snprintf(want, sizeof want, "%s/BYPO/plain name.json", dir);
    CHECK(present(want), "a plain save did not write %s", want);
}

void test_presets_hostile(void) {
    if (!tmp_home_open()) {
        CHECK(false, "could not make a temp data home under /tmp");
        return;
    }
    a_hundred_thousand_brackets_are_refused_not_followed();
    non_finite_numbers_are_not_json();
    huge_but_legal_numbers_land_inside_their_clamp();
    a_hostile_division_stays_a_real_index();
    an_unknown_algorithm_only_costs_the_algorithm();
    the_old_key_names_still_land();
    a_field_of_the_wrong_type_fails_the_whole_document();
    anything_after_the_root_object_fails();
    a_repeated_key_fails_the_document();
    raw_control_characters_are_not_json_strings();
    an_overlong_string_fails_rather_than_truncating();
    an_oversized_array_fails();
    a_sixty_four_megabyte_preset_is_refused_unread();
    a_dotted_file_name_never_becomes_a_path();
    a_traversing_name_is_scrubbed_before_it_is_a_segment();
    an_odd_file_name_never_becomes_a_preset();
    preset_save_in_writes_nothing_outside_the_preset_dir();
    rm_rf(g_tmp);
}
