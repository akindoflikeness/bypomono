/* Direct sound controls. Continuous ones take their range, unit and default
   from PARAMS (params.c); the rest are listed here. */
#include "command.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum { CT_ALG, CT_MODE, CT_PARAM, CT_NOTE, CT_LIMITER } ControlKind;

typedef struct {
    const char *name;
    ControlKind kind;
    ParamId param; /* CT_PARAM */
    float lo, hi;  /* the others */
    bool integer;
} ControlSpec;

static const ControlSpec SPECS[] = {
    {"alg", CT_ALG, 0, 1, 8, true},
    {"mode", CT_MODE, 0, 0, 0, false},
    {"index", CT_PARAM, PARAM_INDEX, 0, 0, false},
    {"rip", CT_PARAM, PARAM_RIP, 0, 0, false},
    {"fb", CT_PARAM, PARAM_FB, 0, 0, false},
    {"glide", CT_PARAM, PARAM_GLIDE, 0, 0, false},
    {"field", CT_PARAM, PARAM_FIELD, 0, 0, false},
    {"curve", CT_PARAM, PARAM_CURVE, 0, 0, false},
    {"level", CT_PARAM, PARAM_LEVEL, 0, 0, false},
    {"detune", CT_PARAM, PARAM_DETUNE, 0, 0, false},
    {"mix", CT_PARAM, PARAM_MIX, 0, 0, false},
    {"ghost", CT_PARAM, PARAM_GHOST, 0, 0, false},
    {"reverb_decay", CT_PARAM, PARAM_VERB_DECAY, 0, 0, false},
    {"damp", CT_PARAM, PARAM_DAMP, 0, 0, false},
    {"haunt", CT_PARAM, PARAM_HAUNT, 0, 0, false},
    {"attack", CT_PARAM, PARAM_ATTACK, 0, 0, false},
    {"env_decay", CT_PARAM, PARAM_ENV_DECAY, 0, 0, false},
    {"sustain", CT_PARAM, PARAM_SUSTAIN, 0, 0, false},
    {"release", CT_PARAM, PARAM_RELEASE, 0, 0, false},
    {"hz", CT_PARAM, PARAM_DRONE_HZ, 0, 0, false},
    {"note", CT_NOTE, 0, 0, 127, true},
    {"warmth", CT_PARAM, PARAM_WARMTH, 0, 0, false},
    {"limiter", CT_LIMITER, 0, 0, 1, true},
    {"ceiling", CT_PARAM, PARAM_CEILING, 0, 0, false},
};

static float spec_lo(const ControlSpec *s) {
    return s->kind == CT_PARAM ? PARAMS[s->param].lo : s->lo;
}
static float spec_hi(const ControlSpec *s) {
    return s->kind == CT_PARAM ? PARAMS[s->param].hi : s->hi;
}
static const char *spec_unit(const ControlSpec *s) {
    return s->kind == CT_PARAM ? PARAMS[s->param].unit : NULL;
}
static const char *spec_alt_unit(const ControlSpec *s) {
    return s->kind == CT_PARAM ? PARAMS[s->param].alt_unit : NULL;
}

static bool reason(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

static const ControlSpec *spec_of(const char *name) {
    for (size_t i = 0; i < sizeof SPECS / sizeof SPECS[0]; i++)
        if (strcmp(SPECS[i].name, name) == 0) return &SPECS[i];
    return NULL;
}

static int mode_of(const char *word) {
    static const char *const NAMES[] = {
        "harmonic", "fibonacci", "golden", "mirror", "plastic"};
    for (int i = 0; i < RATIO_MODE_COUNT; i++)
        if (strcasecmp(word, NAMES[i]) == 0) return i;
    return -1;
}

static bool number_of(const char *word, const ControlSpec *s, float *out) {
    char *end = NULL;
    float v = strtof(word, &end);
    if (end == word || !isfinite(v)) return false;
    const char *unit = spec_unit(s), *alt = spec_alt_unit(s);
    if (*end && (!unit || (strcasecmp(end, unit) != 0
                           && (!alt || strcasecmp(end, alt) != 0))))
        return false;
    if (s->integer && v != floorf(v)) return false;
    *out = v;
    return true;
}

bool control_parse(Command *c, char *err, size_t n) {
    const ControlSpec *s = spec_of(c->verb->name);
    if (!s) return reason(err, n, "internal: no control called %s", c->verb->name);
    if (c->nwords != 1)
        return reason(err, n, "%s wants one value", c->verb->name);
    if (s->kind == CT_MODE) {
        int mode = mode_of(c->words[0]);
        if (mode < 0)
            return reason(err, n,
                          "mode wants harmonic, fibonacci, golden, mirror or "
                          "plastic, not '%s'", c->words[0]);
        c->choice = mode;
        return true;
    }
    float value;
    if (!number_of(c->words[0], s, &value) || value < spec_lo(s)
        || value > spec_hi(s)) {
        const char *unit = spec_unit(s) ? spec_unit(s) : "";
        return reason(err, n, "%s wants %g to %g%s, not '%s'", s->name,
                      (double)spec_lo(s), (double)spec_hi(s), unit, c->words[0]);
    }
    c->value = value;
    c->choice = (int)value;
    return true;
}

bool control_run(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    const ControlSpec *s = spec_of(c->verb->name);
    float v = c->value;
    switch (s->kind) {
    case CT_ALG:
        a->shadow.algorithm = ALGORITHMS[c->choice - 1];
        params_send(a, PG_PATCH);
        break;
    case CT_MODE:
        patch_apply_ratio_mode(&a->shadow, (RatioMode)c->choice);
        params_send(a, PG_PATCH);
        break;
    case CT_PARAM:
        param_set(a, s->param, v);
        params_send(a, PARAMS[s->param].group);
        break;
    case CT_NOTE:
        a->drone_hz = midi_to_hz((uint8_t)c->choice);
        params_send(a, PG_DRONE);
        break;
    case CT_LIMITER:
        a->shadow_limiter_enabled = v > 0.5f;
        params_send(a, PG_LIMITER);
        break;
    }
    if (s->kind == CT_NOTE)
        push_log(a, "note %d  %.1f hz", c->choice, (double)a->drone_hz);
    return true;
}

static int add(char out[][CAND_LEN], int n, int max, const char *prefix,
               const char *word) {
    if (n >= max || strncasecmp(word, prefix, strlen(prefix)) != 0) return n;
    snprintf(out[n++], CAND_LEN, "%s", word);
    return n;
}

int control_complete(char *const words[], int nwords, const char *prefix,
                     char out[][CAND_LEN], int max) {
    (void)words;
    if (nwords > 0) return 0;
    static const char *const MODES[] = {
        "harmonic", "fibonacci", "golden", "mirror", "plastic"};
    int n = 0;
    for (int i = 0; i < RATIO_MODE_COUNT; i++)
        n = add(out, n, max, prefix, MODES[i]);
    return n;
}

/* ---------- grouped melody and Chandas controls ---------- */

static bool on_off(const char *word, bool *out) {
    if (strcasecmp(word, "on") == 0) return *out = true, true;
    if (strcasecmp(word, "off") == 0) return *out = false, true;
    return false;
}

static bool plain_number(const char *word, const char *unit, float *out) {
    char *end = NULL;
    float v = strtof(word, &end);
    if (end == word || !isfinite(v)) return false;
    if (*end && (!unit || strcasecmp(end, unit) != 0)) return false;
    *out = v;
    return true;
}

static void joined(const Command *c, int from, char *out, size_t cap) {
    out[0] = 0;
    for (int i = from; i < c->nwords; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", used ? " " : "", c->words[i]);
    }
}

/* the grouped numeric controls, as "<group> <key>" rows of PARAMS */
static int grouped_param(const char *group, const char *key) {
    char name[160];
    snprintf(name, sizeof name, "%s %s", group, key);
    return param_find(name);
}

/* a grouped numeric value in the row's range and unit */
static bool grouped_number(const char *group, const char *key, int id,
                           const char *word, char *err, size_t n) {
    const Control *p = &PARAMS[id];
    float value;
    if (!plain_number(word, p->unit, &value) || value < p->lo || value > p->hi
        || (p->integer && value != floorf(value)))
        return reason(err, n, "%s %s wants %g to %g%s, not '%s'", group, key,
                      (double)p->lo, (double)p->hi, p->unit ? p->unit : "",
                      word);
    return true;
}

static void grouped_set(App *a, int id, const char *word) {
    float value;
    plain_number(word, PARAMS[id].unit, &value);
    param_set(a, (ParamId)id, value);
}

static int tuning_of(const char *word) {
    for (int i = 0; i < NUM_TUNINGS; i++)
        if (strcasecmp(word, tuning_name((Tuning)i)) == 0) return i;
    return -1;
}

static int scale_of(const char *word) {
    for (int i = 0; i < NUM_SCALES; i++)
        if (strcasecmp(word, scale_name((Scale)i)) == 0) return i;
    return -1;
}

bool control_parse_mel(Command *c, char *err, size_t n) {
    if (c->nwords == 0) return true;
    bool state;
    if (on_off(c->words[0], &state)) {
        if (c->nwords == 1) return true;
        return reason(err, n, "mel %s stands alone", c->words[0]);
    }
    const char *key = c->words[0];
    if (strcasecmp(key, "src") == 0) {
        if (c->nwords == 2
            && (strcasecmp(c->words[1], "weyl") == 0
                || strcasecmp(c->words[1], "xorshift") == 0)) return true;
        return reason(err, n, "mel src wants weyl or xorshift");
    }
    if (strcasecmp(key, "tuning") == 0 || strcasecmp(key, "scale") == 0) {
        char name[128];
        joined(c, 1, name, sizeof name);
        int choice = strcasecmp(key, "tuning") == 0 ? tuning_of(name)
                                                     : scale_of(name);
        if (choice >= 0) return true;
        return reason(err, n, "mel %s does not know '%s'", key,
                      name[0] ? name : "nothing");
    }
    if (c->nwords != 2)
        return reason(err, n, "mel %s wants one value", key);
    int id = grouped_param("mel", key);
    if (id < 0) return reason(err, n, "mel has no control called %s", key);
    return grouped_number("mel", key, id, c->words[1], err, n);
}

bool control_run_mel(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    MelodyParams *m = &a->shadow_melody;
    if (c->nwords == 0) {
        push_log(a, "mel %s  src %s  tuning %s  scale %s", m->enabled ? "on" : "off",
                 m->source == HOLD_GOLDEN_WEYL ? "weyl" : "xorshift",
                 tuning_name(m->tuning), scale_name(m->scale));
        push_log(a, "mel root %d  range %d  rate %.3g hz", m->root_midi,
                 m->range_degrees, (double)m->rate_hz);
        return true;
    }
    bool state;
    if (on_off(c->words[0], &state)) {
        m->enabled = state;
    } else if (strcasecmp(c->words[0], "src") == 0) {
        m->source = strcasecmp(c->words[1], "weyl") == 0 ? HOLD_GOLDEN_WEYL
                                                          : HOLD_XORSHIFT;
    } else if (strcasecmp(c->words[0], "tuning") == 0) {
        char name[128];
        joined(c, 1, name, sizeof name);
        m->tuning = (Tuning)tuning_of(name);
    } else if (strcasecmp(c->words[0], "scale") == 0) {
        char name[128];
        joined(c, 1, name, sizeof name);
        m->scale = (Scale)scale_of(name);
    } else {
        grouped_set(a, grouped_param("mel", c->words[0]), c->words[1]);
    }
    params_send(a, PG_MELODY);
    return true;
}

int control_complete_mel(char *const words[], int nwords, const char *prefix,
                         char out[][CAND_LEN], int max) {
    static const char *const KEYS[] = {
        "on", "off", "src", "tuning", "scale", "root", "range", "rate"};
    int n = 0;
    if (nwords == 0) {
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
            n = add(out, n, max, prefix, KEYS[i]);
        return n;
    }
    if (nwords != 1) return 0;
    if (strcasecmp(words[0], "src") == 0) {
        n = add(out, n, max, prefix, "weyl");
        return add(out, n, max, prefix, "xorshift");
    }
    if (strcasecmp(words[0], "tuning") == 0) {
        for (int i = 0; i < NUM_TUNINGS; i++)
            n = add(out, n, max, prefix, tuning_name((Tuning)i));
    } else if (strcasecmp(words[0], "scale") == 0) {
        for (int i = 0; i < NUM_SCALES; i++)
            n = add(out, n, max, prefix, scale_name((Scale)i));
    }
    return n;
}

static int division_of(const char *word) {
    for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
        if (strcasecmp(word, CHANDAS_DIVISIONS[i].name) == 0) return i;
    return -1;
}

bool control_parse_chandas(Command *c, char *err, size_t n) {
    if (c->nwords == 0) return true;
    const char *key = c->words[0];
    bool boolean = strcasecmp(key, "bypass") == 0
                   || strcasecmp(key, "sync") == 0;
    if (boolean) {
        if (c->nwords == 1) return true;
        bool state;
        if (c->nwords == 2 && on_off(c->words[1], &state)) return true;
        return reason(err, n, "chandas %s wants on or off", key);
    }
    if (c->nwords != 2)
        return reason(err, n, "chandas %s wants one value", key);
    if (strcasecmp(key, "time") == 0) {
        if (division_of(c->words[1]) >= 0) return true;
        return reason(err, n, "chandas time does not know '%s'", c->words[1]);
    }
    int id = grouped_param("chandas", key);
    if (id < 0) return reason(err, n, "chandas has no control called %s", key);
    return grouped_number("chandas", key, id, c->words[1], err, n);
}

bool control_run_chandas(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    ChandasParams *h = &a->shadow_chandas;
    if (c->nwords == 0) {
        push_log(a, "chandas bypass %s  sync %s  time %s  rate %.3g hz",
                 h->enabled ? "off" : "on", h->sync ? "on" : "off",
                 CHANDAS_DIVISIONS[h->division].name, (double)h->rate_hz);
        push_log(a, "chandas mix %.3g  spread %.3g  size %.3g  warp %.3g",
                 (double)h->mix, (double)h->spread, (double)h->size,
                 (double)h->warp);
        push_log(a, "chandas dim %.3g  tail %.3g", (double)h->dimension,
                 (double)h->tail);
        return true;
    }
    const char *key = c->words[0];
    if (strcasecmp(key, "bypass") == 0 || strcasecmp(key, "sync") == 0) {
        if (c->nwords == 1) {
            bool state = strcasecmp(key, "bypass") == 0 ? !h->enabled : h->sync;
            push_log(a, "chandas %s %s", key, state ? "on" : "off");
            return true;
        }
        bool state;
        on_off(c->words[1], &state);
        if (strcasecmp(key, "bypass") == 0) h->enabled = !state;
        else h->sync = state;
    } else if (strcasecmp(key, "time") == 0) {
        h->division = (size_t)division_of(c->words[1]);
    } else {
        grouped_set(a, grouped_param("chandas", key), c->words[1]);
    }
    params_send(a, PG_CHANDAS);
    return true;
}

int control_complete_chandas(char *const words[], int nwords,
                             const char *prefix, char out[][CAND_LEN], int max) {
    static const char *const KEYS[] = {"bypass", "sync", "time", "rate", "mix",
                                       "spread", "size", "warp", "dim", "tail"};
    int n = 0;
    if (nwords == 0) {
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
            n = add(out, n, max, prefix, KEYS[i]);
        return n;
    }
    if (nwords != 1) return 0;
    if (strcasecmp(words[0], "bypass") == 0
        || strcasecmp(words[0], "sync") == 0) {
        n = add(out, n, max, prefix, "on");
        return add(out, n, max, prefix, "off");
    }
    if (strcasecmp(words[0], "time") == 0)
        for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
            n = add(out, n, max, prefix, CHANDAS_DIVISIONS[i].name);
    return n;
}

static float scalar_value(const App *a, const ControlSpec *s) {
    if (s->kind == CT_PARAM) return param_get(a, s->param);
    if (s->kind == CT_LIMITER) return a->shadow_limiter_enabled ? 1.0f : 0.0f;
    return 0.0f;
}

static float scalar_default(const App *a, const ControlSpec *s) {
    if (s->kind == CT_PARAM) return param_default(a, s->param);
    if (s->kind == CT_LIMITER) return session_default().limiter_enabled ? 1.0f : 0.0f;
    return 0.0f;
}

bool control_get(App *a, char *const words[], int nwords, char *err, size_t n) {
    if (nwords == 1) {
        const ControlSpec *s = spec_of(words[0]);
        if (s && s->kind == CT_MODE) {
            push_log(a, "mode %s  default golden", mode_name_of(a->shadow.ratio_mode));
            return true;
        }
        if (s && s->kind == CT_ALG) {
            push_log(a, "alg %d  range 1 to 8  default 1",
                     algorithm_index_of(&a->shadow) + 1);
            return true;
        }
        if (s && s->kind == CT_NOTE)
            return reason(err, n, "note is an action; get hz shows its result");
        if (s) {
            const char *unit = spec_unit(s) ? spec_unit(s) : "";
            push_log(a, "%s %g%s  range %g to %g%s  default %g%s", s->name,
                     (double)scalar_value(a, s), unit, (double)spec_lo(s),
                     (double)spec_hi(s), unit, (double)scalar_default(a, s),
                     unit);
            return true;
        }
        if (strcasecmp(words[0], "drone") == 0
            || strcasecmp(words[0], "poly") == 0
            || strcasecmp(words[0], "unison") == 0) {
            bool on = strcasecmp(words[0], "drone") == 0 ? a->engaged
                      : strcasecmp(words[0], "poly") == 0 ? a->shadow.voices > 1
                                                           : a->shadow.unison > 1;
            push_log(a, "%s %s  default off", words[0], on ? "on" : "off");
            return true;
        }
        if (strcasecmp(words[0], "mel") == 0) {
            Command c = {0};
            return control_run_mel(a, &c, err, n);
        }
        if (strcasecmp(words[0], "chandas") == 0) {
            Command c = {0};
            return control_run_chandas(a, &c, err, n);
        }
    }
    if (nwords == 2 && strcasecmp(words[0], "op") == 0) {
        char *end = NULL;
        long k = strtol(words[1], &end, 10);
        if (end && !*end && k >= 1 && k <= NUM_OPS) {
            const OpParams *op = &a->shadow.ops[k - 1];
            push_log(a, "op %ld %s  ratio %.3g  detune %.3g cents  level %.3g",
                     k, op->enabled ? "on" : "off", (double)op->ratio,
                     (double)op->detune_cents, (double)op->level);
            return true;
        }
    }
    return reason(err, n, "no parameter called %s%s%s", words[0],
                  nwords > 1 ? " " : "", nwords > 1 ? words[1] : "");
}

bool control_status(App *a, const char *section, char *err, size_t n) {
    bool all = !section;
    if (all || strcasecmp(section, "sound") == 0)
        push_log(a, "sound alg %d  mode %s  index %.3g  rip %.3g  fb %.3g  level %.3g",
                 algorithm_index_of(&a->shadow) + 1, mode_name_of(a->shadow.ratio_mode),
                 (double)a->shadow.index, (double)a->shadow.rip,
                 (double)a->shadow.feedback, (double)a->shadow.master_level);
    if (all || strcasecmp(section, "operators") == 0)
        for (int i = 0; i < NUM_OPS; i++)
            push_log(a, "op %d %s  ratio %.3g  level %.3g", i + 1,
                     a->shadow.ops[i].enabled ? "on" : "off",
                     (double)a->shadow.ops[i].ratio, (double)a->shadow.ops[i].level);
    if (all || strcasecmp(section, "envelope") == 0)
        push_log(a, "envelope attack %.3gs  decay %.3gs  sustain %.3g  release %.3gs",
                 (double)a->shadow_attack_s, (double)a->shadow_decay_s,
                 (double)a->shadow_sustain, (double)a->shadow_release_s);
    if (all || strcasecmp(section, "room") == 0)
        push_log(a, "room mix %.3g  ghost %.3g  decay %.3gs  damp %.3g  haunt %.3g",
                 (double)a->shadow_verb.mix, (double)a->shadow_verb.ghost,
                 (double)a->shadow_verb.decay, (double)a->shadow_verb.damp,
                 (double)a->shadow_verb.haunt);
    if (all || strcasecmp(section, "melody") == 0) {
        Command c = {0};
        control_run_mel(a, &c, err, n);
    }
    if (all || strcasecmp(section, "chandas") == 0) {
        Command c = {0};
        control_run_chandas(a, &c, err, n);
    }
    if (all || !strcasecmp(section, "sound") || !strcasecmp(section, "operators")
        || !strcasecmp(section, "envelope") || !strcasecmp(section, "room")
        || !strcasecmp(section, "melody") || !strcasecmp(section, "chandas"))
        return true;
    return reason(err, n, "no status section called %s", section);
}
