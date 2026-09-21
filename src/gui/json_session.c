#include "app.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Presets are untrusted input. Recursion, string length, number length and
   element counts are all bounded, and anything outside strict JSON fails the
   whole parse rather than being coerced. */
enum {
    JS_MAX_DEPTH = 32,
    JS_MAX_STRING = 4096,
    JS_MAX_ELEMS = 4096,
    JS_MAX_OPS_SEEN = 64,
    JS_MAX_NUMBER = 512
};

typedef struct {
    const char *p, *end;
    bool err;
    int depth;
} Js;

/* only the skip path recurses, so that is where the depth is counted */
static bool js_enter(Js *j) {
    if (j->depth >= JS_MAX_DEPTH) {
        j->err = true;
        return false;
    }
    j->depth++;
    return true;
}

static void js_leave(Js *j) { j->depth--; }

static void js_ws(Js *j) {
    while (j->p < j->end &&
           (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static bool js_ch(Js *j, char c) {
    js_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

static bool js_lit(Js *j, const char *s) {
    size_t n = strlen(s);
    if ((size_t)(j->end - j->p) >= n && memcmp(j->p, s, n) == 0) {
        j->p += n;
        return true;
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool js_str_put(Js *j, char *out, size_t limit, size_t *n, char c) {
    if (*n + 1 >= limit) {
        j->err = true;
        return false;
    }
    if (out) out[*n] = c;
    (*n)++;
    return true;
}

/* out == NULL skips the string. A value longer than the destination, or than
   JS_MAX_STRING when skipping, is an error rather than a silent truncation. */
static bool js_string(Js *j, char *out, size_t cap) {
    js_ws(j);
    if ((out && cap == 0) || j->p >= j->end || *j->p != '"') {
        j->err = true;
        return false;
    }
    j->p++;
    size_t limit = out ? cap : (size_t)JS_MAX_STRING + 1;
    size_t n = 0;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') {
            if (out) out[n] = 0;
            return true;
        }
        if (c < 0x20) break; /* raw control characters are not legal in a string */
        if (c == '\\') {
            if (j->p >= j->end) break;
            char e = *j->p++;
            char put = 0;
            switch (e) {
            case '"': put = '"'; break;
            case '\\': put = '\\'; break;
            case '/': put = '/'; break;
            case 'b': put = '\b'; break;
            case 'f': put = '\f'; break;
            case 'n': put = '\n'; break;
            case 'r': put = '\r'; break;
            case 't': put = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    int h = j->p < j->end ? hex_val(*j->p) : -1;
                    if (h < 0) {
                        j->err = true;
                        return false;
                    }
                    cp = cp * 16 + (unsigned)h;
                    j->p++;
                }
                char enc[4];
                int en;
                if (cp < 0x80) {
                    enc[0] = (char)cp;
                    en = 1;
                } else if (cp < 0x800) {
                    enc[0] = (char)(0xC0 | (cp >> 6));
                    enc[1] = (char)(0x80 | (cp & 0x3F));
                    en = 2;
                } else {
                    enc[0] = (char)(0xE0 | (cp >> 12));
                    enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    enc[2] = (char)(0x80 | (cp & 0x3F));
                    en = 3;
                }
                for (int k = 0; k < en; k++)
                    if (!js_str_put(j, out, limit, &n, enc[k])) return false;
                continue;
            }
            default: j->err = true; return false;
            }
            if (!js_str_put(j, out, limit, &n, put)) return false;
            continue;
        }
        if (!js_str_put(j, out, limit, &n, (char)c)) return false;
    }
    j->err = true;
    return false;
}

/* strict JSON number grammar: strtod on its own also takes nan, inf,
   infinity, nan(chars) and hex floats */
static bool js_number(Js *j, double *out) {
    js_ws(j);
    const char *s = j->p, *q = j->p;
    if (q < j->end && *q == '-') q++;
    if (q >= j->end || *q < '0' || *q > '9') {
        j->err = true;
        return false;
    }
    if (*q == '0')
        q++;
    else
        while (q < j->end && *q >= '0' && *q <= '9') q++;
    if (q < j->end && *q == '.') {
        q++;
        if (q >= j->end || *q < '0' || *q > '9') {
            j->err = true;
            return false;
        }
        while (q < j->end && *q >= '0' && *q <= '9') q++;
    }
    if (q < j->end && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < j->end && (*q == '+' || *q == '-')) q++;
        if (q >= j->end || *q < '0' || *q > '9') {
            j->err = true;
            return false;
        }
        while (q < j->end && *q >= '0' && *q <= '9') q++;
    }
    char buf[JS_MAX_NUMBER];
    size_t n = (size_t)(q - s);
    if (n >= sizeof buf) {
        j->err = true;
        return false;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    double v = strtod(buf, NULL);
    if (!isfinite(v)) {
        j->err = true;
        return false;
    }
    j->p = q;
    *out = v;
    return true;
}

static void js_skip(Js *j);

static void js_skip_object(Js *j) {
    if (!js_enter(j)) return;
    if (js_ch(j, '}')) {
        js_leave(j);
        return;
    }
    for (long count = 0;;) {
        if (!js_string(j, NULL, 0)) break;
        if (!js_ch(j, ':')) {
            j->err = true;
            break;
        }
        js_skip(j);
        if (j->err) break;
        if (++count > JS_MAX_ELEMS) {
            j->err = true;
            break;
        }
        if (js_ch(j, ',')) continue;
        if (!js_ch(j, '}')) j->err = true;
        break;
    }
    js_leave(j);
}

static void js_skip_array(Js *j) {
    if (!js_enter(j)) return;
    if (js_ch(j, ']')) {
        js_leave(j);
        return;
    }
    for (long count = 0;;) {
        js_skip(j);
        if (j->err) break;
        if (++count > JS_MAX_ELEMS) {
            j->err = true;
            break;
        }
        if (js_ch(j, ',')) continue;
        if (!js_ch(j, ']')) j->err = true;
        break;
    }
    js_leave(j);
}

static void js_skip(Js *j) {
    js_ws(j);
    if (j->p >= j->end) {
        j->err = true;
        return;
    }
    char c = *j->p;
    if (c == '"') {
        js_string(j, NULL, 0);
    } else if (c == '{') {
        j->p++;
        js_skip_object(j);
    } else if (c == '[') {
        j->p++;
        js_skip_array(j);
    } else if (c == 't') {
        if (!js_lit(j, "true")) j->err = true;
    } else if (c == 'f') {
        if (!js_lit(j, "false")) j->err = true;
    } else if (c == 'n') {
        if (!js_lit(j, "null")) j->err = true;
    } else {
        double d;
        js_number(j, &d);
    }
}

static int js_obj_next(Js *j, bool *first, char *key, size_t cap) {
    js_ws(j);
    if (js_ch(j, '}')) return 0;
    if (!*first && !js_ch(j, ',')) {
        j->err = true;
        return -1;
    }
    *first = false;
    if (!js_string(j, key, cap)) return -1;
    if (!js_ch(j, ':')) {
        j->err = true;
        return -1;
    }
    return 1;
}

static int js_key(const char *key, const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(key, names[i]) == 0) return i;
    return -1;
}

/* one bit per known key of the enclosing object; a repeat fails the parse */
static bool js_dup(Js *j, uint32_t *seen, int bit) {
    if (*seen & (1u << bit)) {
        j->err = true;
        return true;
    }
    *seen |= 1u << bit;
    return false;
}

static float js_f32(Js *j, float dflt) {
    double d;
    if (!js_number(j, &d)) return dflt;
    if (d > (double)FLT_MAX) return FLT_MAX;
    if (d < -(double)FLT_MAX) return -FLT_MAX;
    return (float)d;
}

/* js_number rejects non-finite values, and the clamp keeps the caller's cast
   inside the destination type */
static bool js_uint(Js *j, double hi, double *out) {
    double d;
    if (!js_number(j, &d) || !isfinite(d)) return false;
    *out = d < 0.0 ? 0.0 : (d > hi ? hi : d);
    return true;
}

static bool js_bool(Js *j, bool dflt) {
    js_ws(j);
    if (js_lit(j, "true")) return true;
    if (js_lit(j, "false")) return false;
    j->err = true;
    return dflt;
}

static int js_enum(Js *j, const char *const *names, int n, int dflt) {
    char s[64];
    if (!js_string(j, s, sizeof s)) return dflt;
    for (int i = 0; i < n; i++)
        if (strcmp(s, names[i]) == 0) return i;
    return dflt;
}

static const char *const RATIO_MODE_NAMES[] = {
    "Harmonic", "Fibonacci", "Golden", "GoldenMirror", "Plastic"};
static const char *const SCALE_NAMES[] = {
    "Phrygian",        "PhrygianDominant", "NaturalMinor", "HarmonicMinor",
    "NeapolitanMinor", "Byzantine",        "Fibonacci",    "Phyllotaxis"};
static const char *const TUNING_NAMES[] = {
    "Scale", "FibonacciHz", "GoldenPowers", "PlasticPowers", "GoldenWalk"};
static const char *const HOLD_NAMES[] = {"GoldenWeyl", "Xorshift"};

static const char *const OP_KEYS[] = {"enabled", "ratio", "detune_cents",
                                      "level"};
static const char *const PATCH_KEYS[] = {
    "algorithm", "ratio_mode", "ops",   "feedback", "index",
    "rip",       "master_level", "glide_seconds", "field", "curve",
    "voices",    "unison",       "unison_detune"};
/* rt60 is the old name for decay */
static const char *const VERB_KEYS[] = {"mix",   "ghost", "decay",
                                        "damp",  "haunt", "rt60"};
static const char *const MELODY_KEYS[] = {"enabled",       "tuning",
                                          "scale",         "root_midi",
                                          "range_degrees", "rate_hz",
                                          "source",        "sync",
                                          "division"};
static const char *const PITCH_KEYS[] = {"enabled",  "division", "length",
                                         "root_midi", "snap",    "gate_len",
                                         "pitches",  "gates",    "velocities"};
static const char *const CHANDAS_KEYS[] = {
    "enabled", "mix",    "sync", "division",  "rate_hz",
    "spread",  "size",   "warp", "dimension", "tail"};
/* harmony is the old name for chandas */
static const char *const SESSION_KEYS[] = {
    "patch",   "verb",    "melody",    "drone_hz",  "chandas",
    "tempo_bpm", "warmth", "harmony",  "release_s", "drone",
    "attack_s",  "decay_s", "sustain", "mods", "limiter_enabled",
    "limiter_ceiling_db", "pitch"};
static const char *const MODS_KEYS[] = {"seqs", "routes", "lfos"};
/* older presets also carry "gates", which are no longer read */
static const char *const SEQ_KEYS[] = {"slot",     "mode",   "smooth",
                                       "division", "length_s", "values"};
static const char *const ROUTE_KEYS[] = {"seq", "target", "depth", "snap",
                                         "lfo"};
/* presets saved before sequences: lfos, converted once everything is read */
static const char *const OLD_LFO_KEYS[] = {"slot",    "shape",    "mode",
                                           "unipolar", "rate_hz", "division",
                                           "phase"};

/* a CHANDAS_DIVISIONS name like "1/16" */
static int8_t js_division(Js *j, int8_t dflt) {
    const char *names[CHANDAS_DIVISIONS_LEN];
    for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
        names[i] = CHANDAS_DIVISIONS[i].name;
    return (int8_t)js_enum(j, names, CHANDAS_DIVISIONS_LEN, dflt);
}

static uint8_t js_u8(Js *j, uint8_t dflt) {
    double d;
    if (!js_uint(j, 255.0, &d)) return dflt;
    return (uint8_t)d;
}

static void parse_ops(Js *j, OpParams ops[NUM_OPS]) {
    if (!js_ch(j, '[')) {
        j->err = true;
        return;
    }
    if (js_ch(j, ']')) return;
    int i = 0;
    for (;;) {
        if (i >= JS_MAX_OPS_SEEN) {
            j->err = true;
            return;
        }
        if (i < NUM_OPS) {
            if (!js_ch(j, '{')) {
                j->err = true;
                return;
            }
            OpParams *op = &ops[i];
            bool first = true;
            uint32_t seen = 0;
            char key[64];
            int r;
            while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
                int k = js_key(key, OP_KEYS, 4);
                if (k >= 0 && js_dup(j, &seen, k)) return;
                switch (k) {
                case 0: op->enabled = js_bool(j, op->enabled); break;
                case 1: op->ratio = js_f32(j, op->ratio); break;
                case 2: op->detune_cents = js_f32(j, op->detune_cents); break;
                case 3: op->level = js_f32(j, op->level); break;
                default: js_skip(j); break;
                }
                if (j->err) return;
            }
            if (r < 0) return;
        } else {
            js_skip(j);
            if (j->err) return;
        }
        i++;
        if (js_ch(j, ',')) continue;
        if (js_ch(j, ']')) return;
        j->err = true;
        return;
    }
}

static void parse_patch(Js *j, Patch *p) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    p->field = 0.0f;
    p->curve = 0.5f;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, PATCH_KEYS, 13);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        switch (k) {
        case 0: {
            js_ws(j);
            if (j->p < j->end && *j->p == '"') {
                char g[16];
                if (js_string(j, g, sizeof g)) {
                    AlgorithmId id;
                    if (strlen(g) == NUM_NODES && algorithm_from_glyphs(g, &id))
                        p->algorithm = id;
                }
            } else {
                double d;
                if (js_uint(j, 4294967295.0, &d))
                    p->algorithm =
                        algorithm_from_legacy_bits((uint8_t)((uint32_t)d & 0x0Fu));
            }
            break;
        }
        case 1:
            p->ratio_mode = (RatioMode)js_enum(j, RATIO_MODE_NAMES,
                                               RATIO_MODE_COUNT, RATIO_GOLDEN);
            break;
        case 2: parse_ops(j, p->ops); break;
        case 3: p->feedback = js_f32(j, p->feedback); break;
        case 4: p->index = js_f32(j, p->index); break;
        case 5: p->rip = js_f32(j, p->rip); break;
        case 6: p->master_level = js_f32(j, p->master_level); break;
        case 7: p->glide_seconds = js_f32(j, p->glide_seconds); break;
        case 8: p->field = js_f32(j, p->field); break;
        case 9: p->curve = js_f32(j, p->curve); break;
        case 10: p->voices = js_u8(j, p->voices); break;
        case 11: p->unison = js_u8(j, p->unison); break;
        case 12: p->unison_detune = js_f32(j, p->unison_detune); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

static void parse_verb(Js *j, VerbParams *v) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, VERB_KEYS, 6);
        if (k == 5) k = 2;
        if (k >= 0 && js_dup(j, &seen, k)) return;
        switch (k) {
        case 0: v->mix = js_f32(j, v->mix); break;
        case 1: v->ghost = js_f32(j, v->ghost); break;
        case 2: v->decay = js_f32(j, v->decay); break;
        case 3: v->damp = js_f32(j, v->damp); break;
        case 4: v->haunt = js_f32(j, v->haunt); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

static void parse_melody(Js *j, MelodyParams *m) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, MELODY_KEYS, 9);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        switch (k) {
        case 0: m->enabled = js_bool(j, m->enabled); break;
        case 1:
            m->tuning =
                (Tuning)js_enum(j, TUNING_NAMES, NUM_TUNINGS, TUNING_SCALE);
            break;
        case 2:
            m->scale = (Scale)js_enum(j, SCALE_NAMES, NUM_SCALES,
                                      SCALE_PHRYGIAN);
            break;
        case 3: m->root_midi = js_u8(j, m->root_midi); break;
        case 4: m->range_degrees = js_u8(j, m->range_degrees); break;
        case 5: m->rate_hz = js_f32(j, m->rate_hz); break;
        case 6:
            m->source = (HoldSource)js_enum(j, HOLD_NAMES, 2, HOLD_GOLDEN_WEYL);
            break;
        case 7: m->sync = js_bool(j, m->sync); break;
        case 8: m->division = js_division(j, MELODY_DEFAULT_DIVISION); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

static void parse_chandas(Js *j, ChandasParams *c) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, CHANDAS_KEYS, 10);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        switch (k) {
        case 0: c->enabled = js_bool(j, c->enabled); break;
        case 1: c->mix = js_f32(j, c->mix); break;
        case 2: c->sync = js_bool(j, c->sync); break;
        case 3: {
            double d;
            if (js_uint(j, (double)(CHANDAS_DIVISIONS_LEN - 1), &d))
                c->division = (size_t)d;
            break;
        }
        case 4: c->rate_hz = js_f32(j, c->rate_hz); break;
        case 5: c->spread = js_f32(j, c->spread); break;
        case 6: c->size = js_f32(j, c->size); break;
        case 7: c->warp = js_f32(j, c->warp); break;
        case 8: c->dimension = js_f32(j, c->dimension); break;
        case 9: c->tail = js_f32(j, c->tail); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

/* calls each(j, ud) on every element of an array; each reads one value */
static void js_each(Js *j, void (*each)(Js *, void *), void *ud) {
    if (!js_ch(j, '[')) {
        j->err = true;
        return;
    }
    if (js_ch(j, ']')) return;
    for (int n = 0;; n++) {
        if (n >= JS_MAX_ELEMS) {
            j->err = true;
            return;
        }
        each(j, ud);
        if (j->err) return;
        if (js_ch(j, ',')) continue;
        if (js_ch(j, ']')) return;
        j->err = true;
        return;
    }
}

/* ---------- old lfo presets ---------- */

#define OLD_LFOS 16
enum { OLD_SINE, OLD_TRI, OLD_SAW, OLD_RAMP, OLD_SQUARE, OLD_EXP, OLD_SH,
       OLD_DRIFT, OLD_SHAPES };
static const char *const OLD_SHAPE_NAMES[OLD_SHAPES] = {
    "sine", "tri", "saw", "ramp", "square", "exp", "sh", "drift"};
static const char *const OLD_MODE_NAMES[] = {"free", "retrig", "once"};

typedef struct {
    bool used;
    int shape, mode;
    bool unipolar;
    int division; /* the whole cycle, in CHANDAS_DIVISIONS; -1 = rate_hz */
    float rate_hz, phase;
} OldLfo;

typedef struct {
    ModBank *bank;
    OldLfo lfo[OLD_LFOS];
    ModRoute route[MOD_ROUTES]; /* .seq holds the old lfo slot */
    int nroutes;
} ModsRead;

static float old_shape(int shape, float ph) {
    switch (shape) {
    case OLD_SINE: return sinf(TAU_F * ph);
    case OLD_TRI:
        return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph
                                                   : 4.0f * ph - 4.0f;
    case OLD_SAW: return 1.0f - 2.0f * ph;
    case OLD_RAMP: return 2.0f * ph - 1.0f;
    case OLD_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
    case OLD_EXP: return 2.0f * expf(-5.0f * ph) - 1.0f;
    default: return 0.0f;
    }
}

/* the step division closest to beats, if one is within 3% */
static int8_t step_division(float beats) {
    int best = -1;
    float best_err = 0.03f;
    for (int d = 0; d < CHANDAS_DIVISIONS_LEN; d++) {
        float err = fabsf(logf(CHANDAS_DIVISIONS[d].beats / beats));
        if (err < best_err) best_err = err, best = d;
    }
    return (int8_t)best;
}

/* the step values sit at step centres, so the shape is sampled there */
static SeqParams seq_from_old(const OldLfo *o, int slot, float bpm) {
    SeqParams p = seq_params_default();
    p.mode = o->mode == 2 ? SEQ_ONCE : SEQ_LOOP;
    p.smooth = o->shape != OLD_SQUARE && o->shape != OLD_SH;
    bool random = o->shape == OLD_SH || o->shape == OLD_DRIFT;
    if (random) seq_fill(&p, SEQ_FILL_RANDOM, (uint32_t)slot + 1u);
    for (int k = 0; k < SEQ_STEPS && !random; k++) {
        float ph = fract_pos(((float)k + 0.5f) / (float)SEQ_STEPS + o->phase);
        float b = old_shape(o->shape, ph);
        p.value[k] = o->unipolar ? 0.5f + 0.25f * (b + 1.0f) : 0.5f + 0.5f * b;
    }
    float cycle_s;
    if (o->division >= 0 && o->division < CHANDAS_DIVISIONS_LEN) {
        float beats = CHANDAS_DIVISIONS[o->division].beats;
        p.division = step_division(beats / (float)SEQ_STEPS);
        cycle_s = beats * 60.0f / clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
    } else {
        p.division = -1;
        cycle_s = 1.0f / fmaxf(o->rate_hz, 0.001f);
    }
    p.length_s = clampf(cycle_s, SEQ_LENGTH_MIN_S, SEQ_LENGTH_MAX_S);
    return p;
}

/* old lfos fill sequences in slot order; routes follow their lfo */
static void convert_old(const ModsRead *m, float bpm) {
    int seq_of[OLD_LFOS];
    int next = 0;
    for (int i = 0; i < OLD_LFOS; i++) {
        seq_of[i] = -1;
        if (!m->lfo[i].used || next >= SEQS) continue;
        while (next < SEQS && m->bank->seq[next].used) next++;
        if (next >= SEQS) break;
        m->bank->seq[next] = seq_from_old(&m->lfo[i], i, bpm);
        seq_of[i] = next++;
    }
    for (int r = 0; r < m->nroutes; r++) {
        ModRoute rt = m->route[r];
        if (rt.seq >= OLD_LFOS || seq_of[rt.seq] < 0) continue;
        rt.seq = (uint8_t)seq_of[rt.seq];
        for (int i = 0; i < MOD_ROUTES; i++)
            if (m->bank->route[i].target == MT_NONE) {
                m->bank->route[i] = rt;
                break;
            }
    }
}

static void parse_old_lfo(Js *j, void *ud) {
    ModsRead *m = ud;
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    OldLfo o = {true, OLD_SINE, 0, false, -1, 1.0f, 0.0f};
    int slot = -1;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, OLD_LFO_KEYS, 7);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        double d;
        switch (k) {
        case 0:
            if (js_uint(j, (double)OLD_LFOS, &d) && d >= 1.0)
                slot = (int)d - 1;
            else
                j->err = true;
            break;
        case 1: o.shape = js_enum(j, OLD_SHAPE_NAMES, OLD_SHAPES, OLD_SINE); break;
        case 2: o.mode = js_enum(j, OLD_MODE_NAMES, 3, 0); break;
        case 3: o.unipolar = js_bool(j, o.unipolar); break;
        case 4: o.rate_hz = js_f32(j, o.rate_hz); break;
        case 5: {
            const char *names[CHANDAS_DIVISIONS_LEN];
            for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
                names[i] = CHANDAS_DIVISIONS[i].name;
            o.division = js_enum(j, names, CHANDAS_DIVISIONS_LEN, -1);
            break;
        }
        case 6: o.phase = js_f32(j, o.phase); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
    if (r < 0) return;
    if (slot >= 0 && slot < OLD_LFOS) m->lfo[slot] = o;
}

/* ---------- sequences ---------- */

typedef struct {
    SeqParams *p;
    int n;
} StepsRead;

static void parse_value(Js *j, void *ud) {
    StepsRead *s = ud;
    float v = js_f32(j, 0.5f);
    if (s->n < SEQ_STEPS) s->p->value[s->n] = v;
    s->n++;
}

static void parse_seq(Js *j, void *ud) {
    ModsRead *m = ud;
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    SeqParams p = seq_params_default();
    int slot = -1;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, SEQ_KEYS, 6);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        double d;
        StepsRead steps = {&p, 0};
        switch (k) {
        case 0:
            if (js_uint(j, (double)SEQS, &d) && d >= 1.0)
                slot = (int)d - 1;
            else
                j->err = true;
            break;
        case 1: {
            const char *names[SEQ_MODE_COUNT];
            for (int i = 0; i < SEQ_MODE_COUNT; i++)
                names[i] = seq_mode_name((SeqMode)i);
            p.mode = (uint8_t)js_enum(j, names, SEQ_MODE_COUNT, SEQ_LOOP);
            break;
        }
        case 2: p.smooth = js_bool(j, p.smooth); break;
        case 3: {
            const char *names[CHANDAS_DIVISIONS_LEN];
            for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
                names[i] = CHANDAS_DIVISIONS[i].name;
            p.division = (int8_t)js_enum(j, names, CHANDAS_DIVISIONS_LEN, -1);
            break;
        }
        case 4:
            p.length_s = js_f32(j, p.length_s);
            if (!(seen & (1u << 3))) p.division = -1;
            break;
        case 5: js_each(j, parse_value, &steps); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
    if (r < 0) return;
    if (slot >= 0 && slot < SEQS) m->bank->seq[slot] = p;
}

static void parse_route(Js *j, void *ud) {
    ModsRead *m = ud;
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    ModRoute rt = {0, MT_NONE, 0.0f, false};
    bool old = false;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, ROUTE_KEYS, 5);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        double d;
        switch (k) {
        case 0:
        case 4:
            old = k == 4;
            if (js_uint(j, (double)(old ? OLD_LFOS : SEQS), &d) && d >= 1.0)
                rt.seq = (uint8_t)(d - 1.0);
            else
                j->err = true;
            break;
        case 1: {
            const char *names[MT_COUNT];
            for (int i = 0; i < MT_COUNT; i++) names[i] = MOD_TARGETS[i].name;
            rt.target = (uint8_t)js_enum(j, names, MT_COUNT, MT_NONE);
            break;
        }
        case 2: rt.depth = js_f32(j, rt.depth); break;
        case 3: rt.snap = js_bool(j, rt.snap); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
    if (r < 0 || rt.target == MT_NONE) return;
    if (old) {
        if (m->nroutes < MOD_ROUTES) m->route[m->nroutes++] = rt;
        return;
    }
    for (int i = 0; i < MOD_ROUTES; i++)
        if (m->bank->route[i].target == MT_NONE) {
            m->bank->route[i] = rt;
            return;
        }
}

static void parse_mods(Js *j, ModsRead *m) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, MODS_KEYS, 3);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        switch (k) {
        case 0: js_each(j, parse_seq, m); break;
        case 1: js_each(j, parse_route, m); break;
        case 2: js_each(j, parse_old_lfo, m); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

/* ---------- pitch sequencer ---------- */

typedef struct {
    PitchSeqParams *p;
    int n;
} PitchRead;

static void parse_pitch_st(Js *j, void *ud) {
    PitchRead *r = ud;
    float v = js_f32(j, 0.0f);
    if (r->n < PITCH_STEPS) r->p->pitch[r->n] = v;
    r->n++;
}

static void parse_pitch_gate(Js *j, void *ud) {
    PitchRead *r = ud;
    bool v = js_bool(j, true);
    if (r->n < PITCH_STEPS) r->p->gate[r->n] = v;
    r->n++;
}

static void parse_pitch_vel(Js *j, void *ud) {
    PitchRead *r = ud;
    float v = js_f32(j, 1.0f);
    if (r->n < PITCH_STEPS) r->p->velocity[r->n] = v;
    r->n++;
}

static void parse_pitch(Js *j, PitchSeqParams *p) {
    if (!js_ch(j, '{')) {
        j->err = true;
        return;
    }
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, PITCH_KEYS, 9);
        if (k >= 0 && js_dup(j, &seen, k)) return;
        PitchRead steps = {p, 0};
        switch (k) {
        case 0: p->enabled = js_bool(j, p->enabled); break;
        case 1: p->division = js_division(j, PITCH_DEFAULT_DIVISION); break;
        case 2: p->length = js_u8(j, p->length); break;
        case 3: p->root_midi = js_u8(j, p->root_midi); break;
        case 4: p->snap = js_bool(j, p->snap); break;
        case 5: p->gate_len = js_f32(j, p->gate_len); break;
        case 6: js_each(j, parse_pitch_st, &steps); break;
        case 7: js_each(j, parse_pitch_gate, &steps); break;
        case 8: js_each(j, parse_pitch_vel, &steps); break;
        default: js_skip(j); break;
        }
        if (j->err) return;
    }
}

bool session_from_json(const char *json, Session *out) {
    if (!json || !out) return false;
    Js j = {json, json + strlen(json), false, 0};
    Session s = session_default();
    ModsRead mods = {&s.mods, {{0}}, {{0}}, 0};
    s.warmth = 0.0f;
    s.tempo_bpm = CHANDAS_DEFAULT_BPM;
    s.chandas = chandas_params_default();
    if (!js_ch(&j, '{')) return false;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(&j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, SESSION_KEYS, 17);
        if (k == 7) k = 4;
        if (k >= 0 && js_dup(&j, &seen, k)) return false;
        switch (k) {
        case 0: parse_patch(&j, &s.patch); break;
        case 1: parse_verb(&j, &s.verb); break;
        case 2: parse_melody(&j, &s.melody); break;
        case 3: s.drone_hz = js_f32(&j, s.drone_hz); break;
        case 4: parse_chandas(&j, &s.chandas); break;
        case 5: s.tempo_bpm = js_f32(&j, s.tempo_bpm); break;
        case 6: s.warmth = js_f32(&j, s.warmth); break;
        case 8: s.release_s = js_f32(&j, s.release_s); break;
        case 9: s.drone = js_bool(&j, s.drone); break;
        case 10: s.attack_s = js_f32(&j, s.attack_s); break;
        case 11: s.decay_s = js_f32(&j, s.decay_s); break;
        case 12: s.sustain = js_f32(&j, s.sustain); break;
        case 13: parse_mods(&j, &mods); break;
        case 14: s.limiter_enabled = js_bool(&j, s.limiter_enabled); break;
        case 15: s.limiter_ceiling_db = js_f32(&j, s.limiter_ceiling_db); break;
        case 16: parse_pitch(&j, &s.pitch); break;
        default: js_skip(&j); break;
        }
        if (j.err) return false;
    }
    if (r < 0 || j.err) return false;
    js_ws(&j);
    if (j.p != j.end) return false; /* trailing garbage after the root object */
    convert_old(&mods, s.tempo_bpm);
    *out = session_sanitize(s);
    return true;
}

typedef struct {
    char *buf;
    size_t len, cap;
    bool err;
} Sb;

static void sb_put(Sb *b, const char *s) {
    if (b->err) return;
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 512;
        while (c < b->len + n + 1) c *= 2;
        char *nb = realloc(b->buf, c);
        if (!nb) {
            b->err = true;
            return;
        }
        b->buf = nb;
        b->cap = c;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

/* shortest digits that round-trip through strtod */
static void fmt_f32(char *out, size_t cap, float v) {
    char tmp[48];
    if (isnan(v) || isinf(v)) v = 0.0f;
    int p = 9;
    for (int q = 1; q <= 9; q++) {
        snprintf(tmp, sizeof tmp, "%.*e", q - 1, (double)v);
        if (strtof(tmp, NULL) == v) {
            p = q;
            break;
        }
    }
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, (double)v);
    const char *sign = tmp[0] == '-' ? "-" : "";
    const char *t = tmp[0] == '-' ? tmp + 1 : tmp;
    char digits[16];
    int nd = 0;
    digits[nd++] = t[0];
    for (const char *q = t + 1; *q && *q != 'e'; q++)
        if (*q != '.') digits[nd++] = *q;
    digits[nd] = 0;
    int ex = atoi(strchr(t, 'e') + 1);
    char body[48];
    if (ex < -5 || ex > 15) {
        if (nd > 1)
            snprintf(body, sizeof body, "%c.%se%d", digits[0], digits + 1, ex);
        else
            snprintf(body, sizeof body, "%ce%d", digits[0], ex);
    } else if (ex >= nd - 1) {
        int zeros = ex - (nd - 1);
        snprintf(body, sizeof body, "%s%.*s.0", digits, zeros,
                 "000000000000000");
    } else if (ex >= 0) {
        snprintf(body, sizeof body, "%.*s.%s", ex + 1, digits,
                 digits + ex + 1);
    } else {
        snprintf(body, sizeof body, "0.%.*s%s", -ex - 1, "000000000000000",
                 digits);
    }
    snprintf(out, cap, "%s%s", sign, body);
}

static void sb_key_f(Sb *b, const char *ind, const char *key, float v,
                     bool comma) {
    char num[64], line[192];
    fmt_f32(num, sizeof num, v);
    snprintf(line, sizeof line, "%s\"%s\": %s%s\n", ind, key, num,
             comma ? "," : "");
    sb_put(b, line);
}

static void sb_key_s(Sb *b, const char *ind, const char *key, const char *v,
                     bool comma) {
    char line[192];
    snprintf(line, sizeof line, "%s\"%s\": \"%s\"%s\n", ind, key, v,
             comma ? "," : "");
    sb_put(b, line);
}

static void sb_key_b(Sb *b, const char *ind, const char *key, bool v,
                     bool comma) {
    char line[192];
    snprintf(line, sizeof line, "%s\"%s\": %s%s\n", ind, key,
             v ? "true" : "false", comma ? "," : "");
    sb_put(b, line);
}

static void sb_key_u(Sb *b, const char *ind, const char *key, unsigned long v,
                     bool comma) {
    char line[192];
    snprintf(line, sizeof line, "%s\"%s\": %lu%s\n", ind, key, v,
             comma ? "," : "");
    sb_put(b, line);
}

/* a corrupt index must not read past a name table */
static const char *name_at(const char *const *names, int n, int i) {
    return (unsigned)i < (unsigned)n ? names[i] : names[0];
}

static void write_seq(Sb *b, int slot, const SeqParams *p, bool first) {
    char line[160], num[48];
    snprintf(line, sizeof line,
             "%s\n      {\"slot\": %d, \"mode\": \"%s\", \"smooth\": %s, ",
             first ? "" : ",", slot + 1, seq_mode_name((SeqMode)p->mode),
             p->smooth ? "true" : "false");
    sb_put(b, line);
    if (p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN) {
        snprintf(line, sizeof line, "\"division\": \"%s\", ",
                 CHANDAS_DIVISIONS[p->division].name);
    } else {
        fmt_f32(num, sizeof num, p->length_s);
        snprintf(line, sizeof line, "\"length_s\": %s, ", num);
    }
    sb_put(b, line);
    sb_put(b, "\"values\": [");
    for (int k = 0; k < SEQ_STEPS; k++) {
        fmt_f32(num, sizeof num, p->value[k]);
        snprintf(line, sizeof line, "%s%s", k ? ", " : "", num);
        sb_put(b, line);
    }
    sb_put(b, "]}");
}

static void write_mods(Sb *b, const ModBank *m) {
    char line[256];
    sb_put(b, "  \"mods\": {\n");
    sb_put(b, "    \"seqs\": [");
    bool first = true;
    for (int i = 0; i < SEQS; i++) {
        if (!m->seq[i].used) continue;
        write_seq(b, i, &m->seq[i], first);
        first = false;
    }
    sb_put(b, first ? "],\n" : "\n    ],\n");
    sb_put(b, "    \"routes\": [");
    first = true;
    for (int i = 0; i < MOD_ROUTES; i++) {
        const ModRoute *r = &m->route[i];
        if (r->target == MT_NONE || r->target >= MT_COUNT) continue;
        char depth[48];
        fmt_f32(depth, sizeof depth, r->depth);
        snprintf(line, sizeof line,
                 "%s\n      {\"seq\": %d, \"target\": \"%s\", \"depth\": %s, "
                 "\"snap\": %s}",
                 first ? "" : ",", r->seq + 1, MOD_TARGETS[r->target].name,
                 depth, r->snap ? "true" : "false");
        sb_put(b, line);
        first = false;
    }
    sb_put(b, first ? "]\n" : "\n    ]\n");
    sb_put(b, "  },\n");
}

static const char *division_name(int d) {
    return CHANDAS_DIVISIONS[d >= 0 && d < CHANDAS_DIVISIONS_LEN ? d : 0].name;
}

static void write_pitch(Sb *b, const PitchSeqParams *p) {
    char line[64], num[48];
    sb_put(b, "  \"pitch\": {\n");
    sb_key_b(b, "    ", "enabled", p->enabled, true);
    sb_key_s(b, "    ", "division", division_name(p->division), true);
    sb_key_u(b, "    ", "length", p->length, true);
    sb_key_u(b, "    ", "root_midi", p->root_midi, true);
    sb_key_b(b, "    ", "snap", p->snap, true);
    sb_key_f(b, "    ", "gate_len", p->gate_len, true);
    sb_put(b, "    \"pitches\": [");
    for (int k = 0; k < PITCH_STEPS; k++) {
        fmt_f32(num, sizeof num, p->pitch[k]);
        snprintf(line, sizeof line, "%s%s", k ? ", " : "", num);
        sb_put(b, line);
    }
    sb_put(b, "],\n    \"gates\": [");
    for (int k = 0; k < PITCH_STEPS; k++) {
        snprintf(line, sizeof line, "%s%s", k ? ", " : "",
                 p->gate[k] ? "true" : "false");
        sb_put(b, line);
    }
    sb_put(b, "],\n    \"velocities\": [");
    for (int k = 0; k < PITCH_STEPS; k++) {
        fmt_f32(num, sizeof num, p->velocity[k]);
        snprintf(line, sizeof line, "%s%s", k ? ", " : "", num);
        sb_put(b, line);
    }
    sb_put(b, "]\n  },\n");
}

char *session_to_json(const Session *s) {
    Sb b = {NULL, 0, 0, false};
    char glyphs[NUM_NODES + 1];
    algorithm_to_glyphs(s->patch.algorithm, glyphs);

    sb_put(&b, "{\n");
    sb_put(&b, "  \"patch\": {\n");
    sb_key_s(&b, "    ", "algorithm", glyphs, true);
    sb_key_s(&b, "    ", "ratio_mode",
             name_at(RATIO_MODE_NAMES, RATIO_MODE_COUNT, s->patch.ratio_mode),
             true);
    sb_put(&b, "    \"ops\": [\n");
    for (int i = 0; i < NUM_OPS; i++) {
        const OpParams *op = &s->patch.ops[i];
        sb_put(&b, "      {\n");
        sb_key_b(&b, "        ", "enabled", op->enabled, true);
        sb_key_f(&b, "        ", "ratio", op->ratio, true);
        sb_key_f(&b, "        ", "detune_cents", op->detune_cents, true);
        sb_key_f(&b, "        ", "level", op->level, false);
        sb_put(&b, i + 1 < NUM_OPS ? "      },\n" : "      }\n");
    }
    sb_put(&b, "    ],\n");
    sb_key_f(&b, "    ", "feedback", s->patch.feedback, true);
    sb_key_f(&b, "    ", "index", s->patch.index, true);
    sb_key_f(&b, "    ", "rip", s->patch.rip, true);
    sb_key_f(&b, "    ", "master_level", s->patch.master_level, true);
    sb_key_f(&b, "    ", "glide_seconds", s->patch.glide_seconds, true);
    sb_key_f(&b, "    ", "field", s->patch.field, true);
    sb_key_f(&b, "    ", "curve", s->patch.curve, true);
    sb_key_u(&b, "    ", "voices", s->patch.voices, true);
    sb_key_u(&b, "    ", "unison", s->patch.unison, true);
    sb_key_f(&b, "    ", "unison_detune", s->patch.unison_detune, false);
    sb_put(&b, "  },\n");

    sb_put(&b, "  \"verb\": {\n");
    sb_key_f(&b, "    ", "mix", s->verb.mix, true);
    sb_key_f(&b, "    ", "ghost", s->verb.ghost, true);
    sb_key_f(&b, "    ", "decay", s->verb.decay, true);
    sb_key_f(&b, "    ", "damp", s->verb.damp, true);
    sb_key_f(&b, "    ", "haunt", s->verb.haunt, false);
    sb_put(&b, "  },\n");

    sb_put(&b, "  \"melody\": {\n");
    sb_key_b(&b, "    ", "enabled", s->melody.enabled, true);
    sb_key_s(&b, "    ", "tuning",
             name_at(TUNING_NAMES, NUM_TUNINGS, s->melody.tuning), true);
    sb_key_s(&b, "    ", "scale",
             name_at(SCALE_NAMES, NUM_SCALES, s->melody.scale), true);
    sb_key_u(&b, "    ", "root_midi", s->melody.root_midi, true);
    sb_key_u(&b, "    ", "range_degrees", s->melody.range_degrees, true);
    sb_key_f(&b, "    ", "rate_hz", s->melody.rate_hz, true);
    sb_key_s(&b, "    ", "source", name_at(HOLD_NAMES, 2, s->melody.source),
             true);
    sb_key_b(&b, "    ", "sync", s->melody.sync, true);
    sb_key_s(&b, "    ", "division", division_name(s->melody.division), false);
    sb_put(&b, "  },\n");

    sb_key_f(&b, "  ", "drone_hz", s->drone_hz, true);

    sb_put(&b, "  \"chandas\": {\n");
    sb_key_b(&b, "    ", "enabled", s->chandas.enabled, true);
    sb_key_f(&b, "    ", "mix", s->chandas.mix, true);
    sb_key_b(&b, "    ", "sync", s->chandas.sync, true);
    sb_key_u(&b, "    ", "division", (unsigned long)s->chandas.division, true);
    sb_key_f(&b, "    ", "rate_hz", s->chandas.rate_hz, true);
    sb_key_f(&b, "    ", "spread", s->chandas.spread, true);
    sb_key_f(&b, "    ", "size", s->chandas.size, true);
    sb_key_f(&b, "    ", "warp", s->chandas.warp, true);
    sb_key_f(&b, "    ", "dimension", s->chandas.dimension, true);
    sb_key_f(&b, "    ", "tail", s->chandas.tail, false);
    sb_put(&b, "  },\n");

    bool any_seq = false;
    for (int i = 0; i < SEQS; i++) any_seq = any_seq || s->mods.seq[i].used;
    if (any_seq) write_mods(&b, &s->mods);
    write_pitch(&b, &s->pitch);

    sb_key_f(&b, "  ", "tempo_bpm", s->tempo_bpm, true);
    sb_key_f(&b, "  ", "warmth", s->warmth, true);
    sb_key_b(&b, "  ", "limiter_enabled", s->limiter_enabled, true);
    sb_key_f(&b, "  ", "limiter_ceiling_db", s->limiter_ceiling_db, true);
    sb_key_f(&b, "  ", "attack_s", s->attack_s, true);
    sb_key_f(&b, "  ", "decay_s", s->decay_s, true);
    sb_key_f(&b, "  ", "sustain", s->sustain, true);
    sb_key_f(&b, "  ", "release_s", s->release_s, true);
    sb_key_b(&b, "  ", "drone", s->drone, false);
    sb_put(&b, "}");
    if (b.err) {
        free(b.buf);
        return NULL;
    }
    return b.buf;
}
