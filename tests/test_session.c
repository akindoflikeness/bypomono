#include <stdlib.h>
#include <string.h>

#include "../src/gui/app.h"
#include "test.h"

static void sanitize_rejects_an_unknown_algorithm(void) {
    Session s = session_default();
    s.patch.algorithm = (AlgorithmId)14;
    Session out = session_sanitize(s);
    CHECK(out.patch.algorithm == ALGORITHMS[0], "algorithm %d survived sanitize",
          (int)out.patch.algorithm);
}

static void sanitize_clamps_the_documented_ranges(void) {
    Session s = session_default();
    s.patch.index = 4.0f;
    s.verb.decay = 99.0f;
    s.drone_hz = 1.0f;
    s.release_s = 99.0f;
    s.attack_s = -1.0f;
    s.decay_s = 99.0f;
    s.sustain = 2.0f;
    Session out = session_sanitize(s);
    CHECK(out.attack_s == 0.0f, "attack_s %g", out.attack_s);
    CHECK(out.decay_s == ENV_TIME_MAX, "decay_s %g", out.decay_s);
    CHECK(out.sustain == 1.0f, "sustain %g", out.sustain);
    CHECK(out.patch.index == 1.0f, "index %g", out.patch.index);
    CHECK(out.verb.decay == 8.0f, "decay %g", out.verb.decay);
    CHECK(out.drone_hz == 27.5f, "drone_hz %g", out.drone_hz);
    CHECK(out.release_s == 8.0f, "release_s %g", out.release_s);
}

static void sanitize_pulls_chandas_back_into_range(void) {
    Session s = session_default();
    s.chandas.division = 9999;
    s.chandas.size = 0.0f;
    s.tempo_bpm = 0.0f;
    Session out = session_sanitize(s);
    CHECK(out.chandas.division == (size_t)(CHANDAS_DIVISIONS_LEN - 1), "division %zu",
          out.chandas.division);
    CHECK(out.chandas.size == CHANDAS_MIN_SIZE, "size %g", out.chandas.size);
    CHECK(out.tempo_bpm == CHANDAS_MIN_BPM, "tempo %g", out.tempo_bpm);
    CHECK(out.tempo_bpm > 0.0f, "tempo %g not positive", out.tempo_bpm);
}

/* Every value the plugin exposes as a CLAP parameter has to live in Session,
   or the host's state round trip silently drops it. One distinct value per
   field, out through the writer and back through the parser. */
static Session marked_session(void) {
    Session s = session_default();
    s.patch.algorithm = ALGORITHMS[3];
    s.patch.ratio_mode = RATIO_PLASTIC;
    s.patch.index = 0.37f;
    s.patch.rip = 0.61f;
    s.patch.feedback = 0.23f;
    s.patch.glide_seconds = 0.77f;
    s.patch.field = 0.41f;
    s.patch.curve = 0.19f;
    s.patch.master_level = 0.53f;
    for (int i = 0; i < NUM_OPS; i++) s.patch.ops[i].enabled = (i & 1) == 0;
    s.verb.mix = 0.29f;
    s.verb.ghost = 0.83f;
    s.verb.decay = 5.5f;
    s.verb.damp = 0.71f;
    s.verb.haunt = 0.13f;
    s.melody.enabled = true;
    s.melody.source = HOLD_XORSHIFT;
    s.melody.tuning = TUNING_GOLDEN_POWERS;
    s.melody.scale = SCALE_BYZANTINE;
    s.melody.root_midi = 39;
    s.melody.range_degrees = 11;
    s.melody.rate_hz = 3.25f;
    s.drone_hz = 233.5f;
    s.chandas.enabled = true;
    s.chandas.mix = 0.67f;
    s.chandas.sync = false;
    s.chandas.division = 7;
    s.chandas.rate_hz = 2.75f;
    s.chandas.spread = 0.44f;
    s.chandas.size = 1.5f;
    s.chandas.warp = 0.22f;
    s.chandas.dimension = 0.88f;
    s.chandas.tail = 0.34f;
    s.tempo_bpm = 97.0f;
    s.warmth = 0.66f;
    s.release_s = 4.4636f;
    s.attack_s = 0.125f;
    s.decay_s = 3.5f;
    s.sustain = 0.375f;
    s.drone = true;
    s.patch.voices = POLY_MAX;
    s.patch.unison = UNISON_MAX;
    s.patch.unison_detune = 17.5f;
    return s;
}

static void check_same_session(const Session *a, const Session *b) {
    CHECK(a->patch.algorithm == b->patch.algorithm, "algorithm");
    CHECK(a->patch.ratio_mode == b->patch.ratio_mode, "ratio_mode");
    CHECK(a->patch.index == b->patch.index, "index %g", b->patch.index);
    CHECK(a->patch.rip == b->patch.rip, "rip %g", b->patch.rip);
    CHECK(a->patch.feedback == b->patch.feedback, "feedback %g", b->patch.feedback);
    CHECK(a->patch.glide_seconds == b->patch.glide_seconds, "glide %g",
          b->patch.glide_seconds);
    CHECK(a->patch.field == b->patch.field, "field %g", b->patch.field);
    CHECK(a->patch.curve == b->patch.curve, "curve %g", b->patch.curve);
    CHECK(a->patch.master_level == b->patch.master_level, "level %g",
          b->patch.master_level);
    CHECK(a->patch.voices == b->patch.voices, "voices %u", b->patch.voices);
    CHECK(a->patch.unison == b->patch.unison, "unison %u", b->patch.unison);
    CHECK(a->patch.unison_detune == b->patch.unison_detune, "unison_detune %g",
          b->patch.unison_detune);
    for (int i = 0; i < NUM_OPS; i++) {
        CHECK(a->patch.ops[i].enabled == b->patch.ops[i].enabled, "op %d enabled", i);
        CHECK(a->patch.ops[i].ratio == b->patch.ops[i].ratio, "op %d ratio", i);
        CHECK(a->patch.ops[i].level == b->patch.ops[i].level, "op %d level", i);
        CHECK(a->patch.ops[i].detune_cents == b->patch.ops[i].detune_cents,
              "op %d detune", i);
    }
    CHECK(a->verb.mix == b->verb.mix, "verb mix %g", b->verb.mix);
    CHECK(a->verb.ghost == b->verb.ghost, "verb ghost %g", b->verb.ghost);
    CHECK(a->verb.decay == b->verb.decay, "verb decay %g", b->verb.decay);
    CHECK(a->verb.damp == b->verb.damp, "verb damp %g", b->verb.damp);
    CHECK(a->verb.haunt == b->verb.haunt, "verb haunt %g", b->verb.haunt);
    CHECK(a->melody.enabled == b->melody.enabled, "melody enabled");
    CHECK(a->melody.source == b->melody.source, "melody source");
    CHECK(a->melody.tuning == b->melody.tuning, "melody tuning");
    CHECK(a->melody.scale == b->melody.scale, "melody scale");
    CHECK(a->melody.root_midi == b->melody.root_midi, "melody root %u",
          (unsigned)b->melody.root_midi);
    CHECK(a->melody.range_degrees == b->melody.range_degrees, "melody range %u",
          (unsigned)b->melody.range_degrees);
    CHECK(a->melody.rate_hz == b->melody.rate_hz, "melody rate %g", b->melody.rate_hz);
    CHECK(a->drone_hz == b->drone_hz, "drone_hz %g", b->drone_hz);
    CHECK(a->chandas.enabled == b->chandas.enabled, "chandas enabled");
    CHECK(a->chandas.mix == b->chandas.mix, "chandas mix %g", b->chandas.mix);
    CHECK(a->chandas.sync == b->chandas.sync, "chandas sync");
    CHECK(a->chandas.division == b->chandas.division, "chandas division %zu",
          b->chandas.division);
    CHECK(a->chandas.rate_hz == b->chandas.rate_hz, "chandas rate %g",
          b->chandas.rate_hz);
    CHECK(a->chandas.spread == b->chandas.spread, "chandas spread %g",
          b->chandas.spread);
    CHECK(a->chandas.size == b->chandas.size, "chandas size %g", b->chandas.size);
    CHECK(a->chandas.warp == b->chandas.warp, "chandas warp %g", b->chandas.warp);
    CHECK(a->chandas.dimension == b->chandas.dimension, "chandas dimension %g",
          b->chandas.dimension);
    CHECK(a->chandas.tail == b->chandas.tail, "chandas tail %g", b->chandas.tail);
    CHECK(a->tempo_bpm == b->tempo_bpm, "tempo %g", b->tempo_bpm);
    CHECK(a->warmth == b->warmth, "warmth %g", b->warmth);
    CHECK(a->release_s == b->release_s, "release_s %g", b->release_s);
    CHECK(a->attack_s == b->attack_s, "attack_s %g", b->attack_s);
    CHECK(a->decay_s == b->decay_s, "decay_s %g", b->decay_s);
    CHECK(a->sustain == b->sustain, "sustain %g", b->sustain);
    CHECK(a->drone == b->drone, "drone %d", (int)b->drone);
}

static void json_round_trip_keeps_every_parameter(void) {
    Session want = session_sanitize(marked_session());
    char *json = session_to_json(&want);
    CHECK(json != NULL, "session_to_json returned nothing");
    if (!json) return;
    Session got;
    bool ok = session_from_json(json, &got);
    CHECK(ok, "session_from_json rejected our own output");
    if (ok) check_same_session(&want, &got);
    /* writing the parsed session again must produce the same document */
    if (ok) {
        char *again = session_to_json(&got);
        CHECK(again && strcmp(json, again) == 0, "second write differs");
        free(again);
    }
    free(json);
}

static void a_missing_key_falls_back_to_the_default(void) {
    static const char OLD[] =
        "{\n  \"drone_hz\": 220.0,\n  \"warmth\": 0.25\n}";
    Session s;
    CHECK(session_from_json(OLD, &s), "a session without the new keys failed");
    CHECK(s.release_s == session_default().release_s, "release_s %g", s.release_s);
    CHECK(s.drone == false, "drone %d", (int)s.drone);
    CHECK(s.drone_hz == 220.0f, "drone_hz %g", s.drone_hz);
}

static void a_repeated_new_key_fails_the_parse(void) {
    static const char DUP_RELEASE[] =
        "{\n  \"release_s\": 1.0,\n  \"release_s\": 2.0\n}";
    static const char DUP_DRONE[] =
        "{\n  \"drone\": true,\n  \"drone\": false\n}";
    Session s;
    CHECK(!session_from_json(DUP_RELEASE, &s), "duplicate release_s accepted");
    CHECK(!session_from_json(DUP_DRONE, &s), "duplicate drone accepted");
}

void test_session(void) {
    sanitize_rejects_an_unknown_algorithm();
    sanitize_clamps_the_documented_ranges();
    sanitize_pulls_chandas_back_into_range();
    json_round_trip_keeps_every_parameter();
    a_missing_key_falls_back_to_the_default();
    a_repeated_new_key_fails_the_parse();
}
