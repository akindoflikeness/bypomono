/* Direct sound controls. One specification owns parsing, ranges, units and
   engine dispatch; get/set/status can consume this same table later. */
#include "command.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
    CT_ALG, CT_MODE, CT_PATCH, CT_VERB, CT_ENV, CT_HZ, CT_NOTE, CT_WARMTH
} ControlKind;

typedef enum {
    P_INDEX, P_RIP, P_FB, P_GLIDE, P_FIELD, P_CURVE, P_LEVEL, P_DETUNE,
    V_MIX, V_GHOST, V_DECAY, V_DAMP, V_HAUNT,
    E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE
} ControlSlot;

typedef struct {
    const char *name;
    ControlKind kind;
    ControlSlot slot;
    float lo, hi;
    const char *unit;
    const char *alt_unit;
    bool integer;
} ControlSpec;

static const ControlSpec SPECS[] = {
    {"alg", CT_ALG, 0, 1, 8, NULL, NULL, true},
    {"mode", CT_MODE, 0, 0, 0, NULL, NULL, false},
    {"index", CT_PATCH, P_INDEX, 0, 1, NULL, NULL, false},
    {"rip", CT_PATCH, P_RIP, 0, 1, NULL, NULL, false},
    {"fb", CT_PATCH, P_FB, 0, 1, NULL, NULL, false},
    {"glide", CT_PATCH, P_GLIDE, 0, 2, "s", "seconds", false},
    {"field", CT_PATCH, P_FIELD, 0, 1, NULL, NULL, false},
    {"curve", CT_PATCH, P_CURVE, 0, 1, NULL, NULL, false},
    {"level", CT_PATCH, P_LEVEL, 0, 1, NULL, NULL, false},
    {"detune", CT_PATCH, P_DETUNE, 0, UNISON_DETUNE_MAX, "cents", "cent", false},
    {"mix", CT_VERB, V_MIX, 0, 1, NULL, NULL, false},
    {"ghost", CT_VERB, V_GHOST, 0, 1, NULL, NULL, false},
    {"reverb_decay", CT_VERB, V_DECAY, 0.05f, 8, "s", "seconds", false},
    {"damp", CT_VERB, V_DAMP, 0, 0.99f, NULL, NULL, false},
    {"haunt", CT_VERB, V_HAUNT, 0, 1, NULL, NULL, false},
    {"attack", CT_ENV, E_ATTACK, ENV_ATTACK_MIN, ENV_TIME_MAX, "s", "seconds", false},
    {"env_decay", CT_ENV, E_DECAY, 0, ENV_TIME_MAX, "s", "seconds", false},
    {"sustain", CT_ENV, E_SUSTAIN, 0, 1, NULL, NULL, false},
    {"release", CT_ENV, E_RELEASE, ENV_RELEASE_MIN, ENV_TIME_MAX, "s", "seconds", false},
    {"hz", CT_HZ, 0, 27.5f, 440, "hz", NULL, false},
    {"note", CT_NOTE, 0, 0, 127, NULL, NULL, true},
    {"warmth", CT_WARMTH, 0, 0, 1, NULL, NULL, false},
};

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
    if (*end && (!s->unit || (strcasecmp(end, s->unit) != 0
                              && (!s->alt_unit
                                  || strcasecmp(end, s->alt_unit) != 0))))
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
    if (!number_of(c->words[0], s, &value) || value < s->lo || value > s->hi) {
        const char *unit = s->unit ? s->unit : "";
        return reason(err, n, "%s wants %g to %g%s, not '%s'", s->name,
                      (double)s->lo, (double)s->hi, unit, c->words[0]);
    }
    c->value = value;
    c->choice = (int)value;
    return true;
}

static void send_patch(App *a) {
    app_send(a, (Event){.kind = EV_SET_PATCH, .u.patch = a->shadow});
}

static void send_envelope(App *a) {
    if (a->chain.amp.kind != AMP_ENVELOPE) return;
    a->chain.amp.env.attack_s = a->shadow_attack_s;
    a->chain.amp.env.decay_s = a->shadow_decay_s;
    a->chain.amp.env.sustain = a->shadow_sustain;
    a->chain.amp.env.release_s = a->shadow_release_s;
    a->chain.amp.env.curve = a->shadow.curve;
    app_send(a, (Event){.kind = EV_SET_CHAIN, .u.chain = a->chain});
}

bool control_run(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    const ControlSpec *s = spec_of(c->verb->name);
    float v = c->value;
    switch (s->kind) {
    case CT_ALG:
        a->shadow.algorithm = ALGORITHMS[c->choice - 1];
        send_patch(a);
        break;
    case CT_MODE:
        patch_apply_ratio_mode(&a->shadow, (RatioMode)c->choice);
        send_patch(a);
        break;
    case CT_PATCH:
        switch (s->slot) {
        case P_INDEX: a->shadow.index = v; break;
        case P_RIP: a->shadow.rip = v; break;
        case P_FB: a->shadow.feedback = v; break;
        case P_GLIDE: a->shadow.glide_seconds = v; break;
        case P_FIELD: a->shadow.field = v; break;
        case P_CURVE: a->shadow.curve = v; break;
        case P_LEVEL: a->shadow.master_level = v; break;
        case P_DETUNE: a->shadow.unison_detune = v; break;
        default: break;
        }
        send_patch(a);
        if (s->slot == P_CURVE) send_envelope(a);
        break;
    case CT_VERB:
        switch (s->slot) {
        case V_MIX: a->shadow_verb.mix = v; break;
        case V_GHOST: a->shadow_verb.ghost = v; break;
        case V_DECAY: a->shadow_verb.decay = v; break;
        case V_DAMP: a->shadow_verb.damp = v; break;
        case V_HAUNT: a->shadow_verb.haunt = v; break;
        default: break;
        }
        app_send(a, (Event){.kind = EV_SET_VERB, .u.verb = a->shadow_verb});
        break;
    case CT_ENV:
        switch (s->slot) {
        case E_ATTACK: a->shadow_attack_s = v; break;
        case E_DECAY: a->shadow_decay_s = v; break;
        case E_SUSTAIN: a->shadow_sustain = v; break;
        case E_RELEASE: a->shadow_release_s = v; break;
        default: break;
        }
        send_envelope(a);
        break;
    case CT_HZ: a->drone_hz = v; goto pitch;
    case CT_NOTE:
        a->drone_hz = midi_to_hz((uint8_t)c->choice);
    pitch:
        app_send(a, (Event){.kind = EV_GLIDE_TO, .u.f = a->drone_hz});
        break;
    case CT_WARMTH:
        a->shadow_warmth = v;
        app_send(a, (Event){.kind = EV_SET_WARMTH, .u.f = v});
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
    float value;
    float lo, hi;
    bool integer = false;
    const char *unit = NULL;
    if (strcasecmp(key, "root") == 0) lo = 24, hi = 57, integer = true;
    else if (strcasecmp(key, "range") == 0) lo = 1, hi = 13, integer = true;
    else if (strcasecmp(key, "rate") == 0) lo = 0.1f, hi = 8, unit = "hz";
    else return reason(err, n, "mel has no control called %s", key);
    if (!plain_number(c->words[1], unit, &value) || value < lo || value > hi
        || (integer && value != floorf(value)))
        return reason(err, n, "mel %s wants %g to %g%s, not '%s'", key,
                      (double)lo, (double)hi, unit ? unit : "", c->words[1]);
    return true;
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
        float value;
        plain_number(c->words[1], strcasecmp(c->words[0], "rate") == 0 ? "hz" : NULL,
                     &value);
        if (strcasecmp(c->words[0], "root") == 0) m->root_midi = (uint8_t)value;
        else if (strcasecmp(c->words[0], "range") == 0)
            m->range_degrees = (uint8_t)value;
        else
            m->rate_hz = value;
    }
    app_send(a, (Event){.kind = EV_SET_MELODY, .u.melody = *m});
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
    float lo, hi;
    const char *unit = NULL;
    if (strcasecmp(key, "rate") == 0) lo = 0.1f, hi = 8, unit = "hz";
    else if (strcasecmp(key, "mix") == 0 || strcasecmp(key, "spread") == 0
             || strcasecmp(key, "warp") == 0 || strcasecmp(key, "dim") == 0
             || strcasecmp(key, "tail") == 0) lo = 0, hi = 1;
    else if (strcasecmp(key, "size") == 0)
        lo = CHANDAS_MIN_SIZE, hi = CHANDAS_MAX_SIZE;
    else return reason(err, n, "chandas has no control called %s", key);
    float value;
    if (!plain_number(c->words[1], unit, &value) || value < lo || value > hi)
        return reason(err, n, "chandas %s wants %g to %g%s, not '%s'", key,
                      (double)lo, (double)hi, unit ? unit : "", c->words[1]);
    return true;
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
        float value;
        plain_number(c->words[1], strcasecmp(key, "rate") == 0 ? "hz" : NULL,
                     &value);
        if (strcasecmp(key, "rate") == 0) h->rate_hz = value;
        else if (strcasecmp(key, "mix") == 0) {
            h->mix = value;
            if (value == 0.0f) h->enabled = false;
        } else if (strcasecmp(key, "spread") == 0) h->spread = value;
        else if (strcasecmp(key, "size") == 0) h->size = value;
        else if (strcasecmp(key, "warp") == 0) h->warp = value;
        else if (strcasecmp(key, "dim") == 0) h->dimension = value;
        else h->tail = value;
    }
    app_send(a, (Event){.kind = EV_SET_CHANDAS, .u.chandas = *h});
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
    if (s->kind == CT_HZ) return a->drone_hz;
    if (s->kind == CT_WARMTH) return a->shadow_warmth;
    if (s->kind == CT_PATCH) switch (s->slot) {
    case P_INDEX: return a->shadow.index;
    case P_RIP: return a->shadow.rip;
    case P_FB: return a->shadow.feedback;
    case P_GLIDE: return a->shadow.glide_seconds;
    case P_FIELD: return a->shadow.field;
    case P_CURVE: return a->shadow.curve;
    case P_LEVEL: return a->shadow.master_level;
    case P_DETUNE: return a->shadow.unison_detune;
    default: break;
    }
    if (s->kind == CT_VERB) switch (s->slot) {
    case V_MIX: return a->shadow_verb.mix;
    case V_GHOST: return a->shadow_verb.ghost;
    case V_DECAY: return a->shadow_verb.decay;
    case V_DAMP: return a->shadow_verb.damp;
    case V_HAUNT: return a->shadow_verb.haunt;
    default: break;
    }
    if (s->kind == CT_ENV) switch (s->slot) {
    case E_ATTACK: return a->shadow_attack_s;
    case E_DECAY: return a->shadow_decay_s;
    case E_SUSTAIN: return a->shadow_sustain;
    case E_RELEASE: return a->shadow_release_s;
    default: break;
    }
    return 0.0f;
}

static float scalar_default(const ControlSpec *s) {
    Patch p = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    VerbParams v = verb_params_default();
    EnvParams e = env_params_default();
    if (s->kind == CT_HZ) return START_HZ;
    if (s->kind == CT_WARMTH) return 0.5f;
    if (s->kind == CT_PATCH) switch (s->slot) {
    case P_INDEX: return p.index;
    case P_RIP: return p.rip;
    case P_FB: return p.feedback;
    case P_GLIDE: return p.glide_seconds;
    case P_FIELD: return p.field;
    case P_CURVE: return p.curve;
    case P_LEVEL: return p.master_level;
    case P_DETUNE: return p.unison_detune;
    default: break;
    }
    if (s->kind == CT_VERB) switch (s->slot) {
    case V_MIX: return v.mix;
    case V_GHOST: return v.ghost;
    case V_DECAY: return v.decay;
    case V_DAMP: return v.damp;
    case V_HAUNT: return v.haunt;
    default: break;
    }
    if (s->kind == CT_ENV) switch (s->slot) {
    case E_ATTACK: return e.attack_s;
    case E_DECAY: return e.decay_s;
    case E_SUSTAIN: return e.sustain;
    case E_RELEASE: return e.release_s;
    default: break;
    }
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
            push_log(a, "%s %g%s  range %g to %g%s  default %g%s", s->name,
                     (double)scalar_value(a, s), s->unit ? s->unit : "",
                     (double)s->lo, (double)s->hi, s->unit ? s->unit : "",
                     (double)scalar_default(s), s->unit ? s->unit : "");
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
