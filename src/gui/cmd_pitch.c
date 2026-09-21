/* The pitch namespace: the note sequencer from the console, and the switch
   that lets it and the melody take turns playing the notes. */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "command.h"

void midi_note_name(int midi, char *out, size_t cap) {
    static const char *const NAMES[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                          "F#", "G",  "G#", "A",  "A#", "B"};
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    snprintf(out, cap, "%s%d", NAMES[midi % 12], midi / 12 - 1);
}

void pitch_send(App *a) {
    a->shadow_pitch = pitch_seq_sanitize(a->shadow_pitch);
    app_send(a, (Event){.kind = EV_SET_PITCH, .u.pitch = a->shadow_pitch});
}

void pitch_set_enabled(App *a, bool on) {
    a->shadow_pitch.enabled = on;
    if (on && a->shadow_melody.enabled) {
        a->shadow_melody.enabled = false;
        params_send(a, PG_MELODY);
        push_log(a, "the melody stops; the pitch sequencer plays the notes.");
    }
    pitch_send(a);
}

void melody_set_enabled(App *a, bool on) {
    a->shadow_melody.enabled = on;
    if (on && a->shadow_pitch.enabled) {
        a->shadow_pitch.enabled = false;
        pitch_send(a);
        push_log(a, "the pitch sequencer stops; the melody plays the notes.");
    }
    params_send(a, PG_MELODY);
}

/* ---------- words ---------- */

static bool reason(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

static bool number(const char *w, float *out) {
    char *end = NULL;
    float v = strtof(w, &end);
    if (end == w || *end || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

static bool whole(const char *w, int lo, int hi, int *out) {
    float v;
    if (!number(w, &v) || v != floorf(v) || v < (float)lo || v > (float)hi)
        return false;
    *out = (int)v;
    return true;
}

static bool on_off(const char *w, bool *out) {
    if (strcasecmp(w, "on") == 0) *out = true;
    else if (strcasecmp(w, "off") == 0) *out = false;
    else return false;
    return true;
}

static int division_of(const char *w) {
    for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
        if (strcasecmp(w, CHANDAS_DIVISIONS[i].name) == 0) return i;
    return -1;
}

/* a midi number, or a note name like A2, C#3 or Eb1 */
static bool note_of(const char *w, int *out) {
    if (whole(w, 0, 127, out)) return true;
    static const int LETTER[7] = {9, 11, 0, 2, 4, 5, 7}; /* a..g */
    char l = (char)tolower((unsigned char)w[0]);
    if (l < 'a' || l > 'g') return false;
    int pc = LETTER[l - 'a'];
    const char *p = w + 1;
    if (*p == '#') pc++, p++;
    else if (*p == 'b') pc--, p++;
    int octave;
    if (!whole(p, -1, 9, &octave)) return false;
    int midi = (octave + 1) * 12 + pc;
    if (midi < 0 || midi > 127) return false;
    *out = midi;
    return true;
}

/* ---------- a line applied to a copy ---------- */

/* the settings the words leave behind; false leaves the reason in err */
static bool apply_words(const Command *c, PitchSeqParams *p, char *err, size_t n) {
    for (int i = 0; i < c->nwords; i++) {
        const char *w = c->words[i];
        const char *next = i + 1 < c->nwords ? c->words[i + 1] : NULL;
        const char *third = i + 2 < c->nwords ? c->words[i + 2] : NULL;
        int k, v;
        float f;
        bool b;
        if (on_off(w, &b)) {
            p->enabled = b;
        } else if (strcasecmp(w, "step") == 0) {
            if (!next || !whole(next, 1, PITCH_STEPS, &k) || !third
                || !number(third, &f) || fabsf(f) > PITCH_RANGE_ST)
                return reason(err, n, "step wants a step, 1 to 16, then "
                                      "semitones, -24 to 24");
            p->pitch[k - 1] = f;
            i += 2;
            /* then optionally a velocity and on or off */
            if (i + 1 < c->nwords && number(c->words[i + 1], &f)) {
                if (f < 0.0f || f > 1.0f)
                    return reason(err, n, "velocity goes 0 to 1, not '%s'",
                                  c->words[i + 1]);
                p->velocity[k - 1] = f;
                i++;
            }
            if (i + 1 < c->nwords && on_off(c->words[i + 1], &b)) {
                p->gate[k - 1] = b;
                i++;
            }
        } else if (strcasecmp(w, "set") == 0) {
            if (c->nwords - i - 1 < PITCH_STEPS)
                return reason(err, n, "set wants all %d steps in semitones",
                              PITCH_STEPS);
            for (int s = 0; s < PITCH_STEPS; s++) {
                const char *sw = c->words[i + 1 + s];
                if (!number(sw, &f) || fabsf(f) > PITCH_RANGE_ST)
                    return reason(err, n, "steps go -24 to 24, not '%s'", sw);
                p->pitch[s] = f;
            }
            i += PITCH_STEPS;
        } else if (strcasecmp(w, "gate") == 0) {
            if (!next || !whole(next, 1, PITCH_STEPS, &k) || !third
                || !on_off(third, &b))
                return reason(err, n, "gate wants a step, 1 to 16, then on or off");
            p->gate[k - 1] = b;
            i += 2;
        } else if (strcasecmp(w, "vel") == 0) {
            if (!next || !whole(next, 1, PITCH_STEPS, &k) || !third
                || !number(third, &f) || f < 0.0f || f > 1.0f)
                return reason(err, n, "vel wants a step, 1 to 16, then 0 to 1");
            p->velocity[k - 1] = f;
            i += 2;
        } else if (strcasecmp(w, "len") == 0) {
            if (!next || !whole(next, 1, PITCH_STEPS, &v))
                return reason(err, n, "len wants 1 to 16 steps");
            p->length = (uint8_t)v;
            i++;
        } else if (strcasecmp(w, "rate") == 0) {
            if (!next || (v = division_of(next)) < 0)
                return reason(err, n, "rate wants a step length like 1/16, not '%s'",
                              next ? next : "nothing");
            p->division = (int8_t)v;
            i++;
        } else if (strcasecmp(w, "root") == 0) {
            if (!next || !note_of(next, &v))
                return reason(err, n, "root wants a note like A2 or a midi "
                                      "number, not '%s'",
                              next ? next : "nothing");
            p->root_midi = (uint8_t)v;
            i++;
        } else if (strcasecmp(w, "snap") == 0) {
            if (!next || !on_off(next, &b))
                return reason(err, n, "snap wants on or off");
            p->snap = b;
            i++;
        } else if (strcasecmp(w, "gatelen") == 0) {
            if (!next || !number(next, &f) || f < PITCH_GATE_LEN_MIN || f > 1.0f)
                return reason(err, n, "gatelen wants %.2f to 1 of a step",
                              (double)PITCH_GATE_LEN_MIN);
            p->gate_len = f;
            i++;
        } else {
            return reason(err, n, "pitch has no word '%s'; help pitch lists them", w);
        }
    }
    return true;
}

bool pitch_parse(Command *c, char *err, size_t n) {
    PitchSeqParams p = pitch_seq_params_default();
    return apply_words(c, &p, err, n);
}

/* ---------- the view ---------- */

typedef struct {
    const PitchSeqParams *p;
} Plot;

static float pitch_draw(float x, void *ud) {
    const PitchSeqParams *p = ((Plot *)ud)->p;
    int k = (int)(x * (float)p->length);
    if (k >= p->length) k = p->length - 1;
    return p->gate[k] ? pitch_seq_semitones(p, k) / PITCH_RANGE_ST : 0.0f;
}

static void pitch_lines(const App *a, const PitchSeqParams *p, View *out) {
    char root[8];
    midi_note_name(p->root_midi, root, sizeof root);
    view_add(out, "pitch %s  step %s  len %d  root %s  12-tet %s  gate %d%%",
             p->enabled ? "on" : "off", CHANDAS_DIVISIONS[p->division].name,
             p->length, root, p->snap ? "on" : "off",
             (int)roundf(p->gate_len * 100.0f));
    char line[VIEW_TEXT] = "";
    for (int k = 0; k < p->length; k++) {
        size_t len = strlen(line);
        if (!p->gate[k])
            snprintf(line + len, sizeof line - len, "%s-", k ? " " : "");
        else if (p->snap)
            snprintf(line + len, sizeof line - len, "%s%+d", k ? " " : "",
                     (int)pitch_seq_semitones(p, k));
        else
            snprintf(line + len, sizeof line - len, "%s%+.2f", k ? " " : "",
                     (double)p->pitch[k]);
    }
    ViewLine *l = view_add(out, "%s", line);
    if (!l) return;
    l->place = GRAPH_BELOW;
    int step = atomic_load_explicit(&((App *)a)->seq_meter.pitch_step,
                                    memory_order_relaxed);
    float mark = p->enabled && step >= 0 ? ((float)step + 0.5f) / (float)p->length
                                         : -1.0f;
    Plot plot = {p};
    graph_plot(&l->graph, 96, pitch_draw, &plot, mark);
}

bool pitch_view(App *a, const Command *c, View *out) {
    (void)c;
    view_clear(out);
    pitch_lines(a, &a->shadow_pitch, out);
    return true;
}

bool pitch_preview(App *a, const Command *c, View *out) {
    view_clear(out);
    PitchSeqParams p = a->shadow_pitch;
    char err[256];
    if (!apply_words(c, &p, err, sizeof err)) {
        view_add(out, "%s", err);
        return true;
    }
    pitch_lines(a, &p, out);
    return true;
}

bool pitch_run(App *a, const Command *c, char *err, size_t n) {
    PitchSeqParams p = a->shadow_pitch;
    if (!apply_words(c, &p, err, n)) return false;
    bool turned = p.enabled != a->shadow_pitch.enabled;
    a->shadow_pitch = p;
    if (turned) pitch_set_enabled(a, p.enabled);
    else pitch_send(a);
    View v;
    if (!c->view && pitch_view(a, c, &v)) push_log_view(a, &v);
    return true;
}

static int add(char out[][CAND_LEN], int n, int max, const char *prefix,
               const char *word) {
    if (n >= max || strncasecmp(word, prefix, strlen(prefix)) != 0) return n;
    snprintf(out[n], CAND_LEN, "%s", word);
    return n + 1;
}

int pitch_complete(char *const words[], int nwords, const char *prefix,
                   char out[][CAND_LEN], int max) {
    int n = 0;
    const char *prev = nwords ? words[nwords - 1] : "";
    if (strcasecmp(prev, "rate") == 0) {
        for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
            n = add(out, n, max, prefix, CHANDAS_DIVISIONS[i].name);
        return n;
    }
    if (strcasecmp(prev, "snap") == 0) {
        n = add(out, n, max, prefix, "on");
        return add(out, n, max, prefix, "off");
    }
    static const char *const KEYS[] = {"on",   "off",  "step", "set",
                                       "gate", "vel",  "len",  "rate",
                                       "root", "snap", "gatelen"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        n = add(out, n, max, prefix, KEYS[i]);
    return n;
}
