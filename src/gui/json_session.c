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
    "rip",       "master_level", "glide_seconds", "field", "curve"};
/* rt60 is the old name for decay */
static const char *const VERB_KEYS[] = {"mix",   "ghost", "decay",
                                        "damp",  "haunt", "rt60"};
static const char *const MELODY_KEYS[] = {"enabled",       "tuning",
                                          "scale",         "root_midi",
                                          "range_degrees", "rate_hz",
                                          "source"};
static const char *const CHANDAS_KEYS[] = {
    "enabled", "mix",    "sync", "division",  "rate_hz",
    "spread",  "size",   "warp", "dimension", "tail"};
/* harmony is the old name for chandas */
static const char *const SESSION_KEYS[] = {"patch",     "verb",   "melody",
                                           "drone_hz",  "chandas", "tempo_bpm",
                                           "warmth",    "harmony"};

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
        int k = js_key(key, PATCH_KEYS, 10);
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

static uint8_t js_u8(Js *j, uint8_t dflt) {
    double d;
    if (!js_uint(j, 255.0, &d)) return dflt;
    return (uint8_t)d;
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
        int k = js_key(key, MELODY_KEYS, 7);
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

bool session_from_json(const char *json, Session *out) {
    if (!json || !out) return false;
    Js j = {json, json + strlen(json), false, 0};
    Session s = session_default();
    s.warmth = 0.0f;
    s.tempo_bpm = CHANDAS_DEFAULT_BPM;
    s.chandas = chandas_params_default();
    if (!js_ch(&j, '{')) return false;
    bool first = true;
    uint32_t seen = 0;
    char key[64];
    int r;
    while ((r = js_obj_next(&j, &first, key, sizeof key)) == 1) {
        int k = js_key(key, SESSION_KEYS, 8);
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
        default: js_skip(&j); break;
        }
        if (j.err) return false;
    }
    if (r < 0 || j.err) return false;
    js_ws(&j);
    if (j.p != j.end) return false; /* trailing garbage after the root object */
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
    sb_key_f(&b, "    ", "curve", s->patch.curve, false);
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
             false);
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

    sb_key_f(&b, "  ", "tempo_bpm", s->tempo_bpm, true);
    sb_key_f(&b, "  ", "warmth", s->warmth, false);
    sb_put(&b, "}");
    if (b.err) {
        free(b.buf);
        return NULL;
    }
    return b.buf;
}
