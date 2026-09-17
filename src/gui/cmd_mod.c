/* The modulation verbs: lfo and mods. */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "command.h"

/* ---------- what the audio thread's lfos are doing ---------- */

void lfo_meter_store(LfoMeter *m, const Mod *mod) {
    for (int i = 0; i < MOD_LFOS; i++) {
        float ph = fract_pos(mod->st[i].phase);
        if (mod->st[i].done) ph = 1.0f - 1e-6f;
        atomic_store_explicit(&m->phase_q16[i], (uint32_t)(ph * 65536.0f),
                              memory_order_relaxed);
        uint32_t bits;
        memcpy(&bits, &mod->st[i].value, sizeof bits);
        atomic_store_explicit(&m->value_bits[i], bits, memory_order_relaxed);
    }
}

float lfo_meter_phase(const LfoMeter *m, int slot) {
    if (slot < 0 || slot >= MOD_LFOS) return 0.0f;
    return (float)atomic_load_explicit(&((LfoMeter *)m)->phase_q16[slot],
                                       memory_order_relaxed)
           / 65536.0f;
}

float lfo_meter_value(const LfoMeter *m, int slot) {
    if (slot < 0 || slot >= MOD_LFOS) return 0.0f;
    uint32_t bits = atomic_load_explicit(&((LfoMeter *)m)->value_bits[slot],
                                         memory_order_relaxed);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

/* ---------- words ---------- */

static bool reason(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

/* a number, optionally followed by one of the given unit suffixes */
static bool number_word(const char *w, const char *const units[], float *out) {
    char *end = NULL;
    float v = strtof(w, &end);
    if (end == w || isnan(v) || isinf(v)) return false;
    if (*end) {
        bool unit = false;
        for (int i = 0; units && units[i]; i++)
            if (strcasecmp(end, units[i]) == 0) unit = true;
        if (!unit) return false;
    }
    *out = v;
    return true;
}

static int shape_of(const char *w) {
    for (int i = 0; i < LFO_SHAPE_COUNT; i++)
        if (strcasecmp(w, lfo_shape_name((LfoShape)i)) == 0) return i;
    if (strcasecmp(w, "triangle") == 0) return LFO_TRIANGLE;
    if (strcasecmp(w, "sin") == 0) return LFO_SINE;
    if (strcasecmp(w, "random") == 0) return LFO_SH;
    return -1;
}

static int mode_of(const char *w) {
    for (int i = 0; i < LFO_MODE_COUNT; i++)
        if (strcasecmp(w, lfo_mode_name((LfoMode)i)) == 0) return i;
    if (strcasecmp(w, "oneshot") == 0 || strcasecmp(w, "one-shot") == 0)
        return LFO_ONCE;
    return -1;
}

static int division_of(const char *w) {
    for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
        if (strcasecmp(w, CHANDAS_DIVISIONS[i].name) == 0) return i;
    return -1;
}

/* the target starting at words[i]; *used says how many words it took */
static int target_of(const Command *c, int i, int *used) {
    if (i + 1 < c->nwords) {
        char two[130];
        snprintf(two, sizeof two, "%s %s", c->words[i], c->words[i + 1]);
        if (strcasecmp(two, "chandas dimension") == 0) snprintf(two, sizeof two, "chandas dim");
        for (int t = MT_INDEX; t < MT_COUNT; t++)
            if (strcasecmp(two, MOD_TARGETS[t].name) == 0) {
                *used = 2;
                return t;
            }
    }
    *used = 1;
    const char *w = c->words[i];
    if (strcasecmp(w, "feedback") == 0) return MT_FB;
    for (int t = MT_INDEX; t < MT_COUNT; t++)
        if (strcasecmp(w, MOD_TARGETS[t].name) == 0) return t;
    return -1;
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 31) la = 31;
    if (lb > 31) lb = 31;
    int prev[32], cur[32];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 0; i < la; i++) {
        cur[0] = i + 1;
        for (int j = 0; j < lb; j++) {
            int best = prev[j + 1] + 1;
            if (cur[j] + 1 < best) best = cur[j] + 1;
            int sub = prev[j] + (tolower((unsigned char)a[i])
                                 != tolower((unsigned char)b[j]));
            if (sub < best) best = sub;
            cur[j + 1] = best;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

/* the closest known word within a couple of edits, or NULL */
static const char *nearest(const char *w, const char *const *extra, int nextra) {
    static const char *const KEYS[] = {"shape", "rate", "phase", "mode", "to",
                                       "rm",    "bi",   "uni"};
    const char *best = NULL;
    int best_d = strlen(w) > 4 ? 3 : 2;
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        int d = edit_distance(w, KEYS[i]);
        if (d < best_d) best_d = d, best = KEYS[i];
    }
    for (int i = 0; i < LFO_SHAPE_COUNT; i++) {
        int d = edit_distance(w, lfo_shape_name((LfoShape)i));
        if (d < best_d) best_d = d, best = lfo_shape_name((LfoShape)i);
    }
    for (int i = 0; i < LFO_MODE_COUNT; i++) {
        int d = edit_distance(w, lfo_mode_name((LfoMode)i));
        if (d < best_d) best_d = d, best = lfo_mode_name((LfoMode)i);
    }
    for (int i = 0; i < nextra; i++) {
        int d = edit_distance(w, extra[i]);
        if (d < best_d) best_d = d, best = extra[i];
    }
    return best;
}

static const char *nearest_target(const char *w) {
    const char *names[MT_COUNT];
    int n = 0;
    for (int t = MT_INDEX; t < MT_COUNT; t++) names[n++] = MOD_TARGETS[t].name;
    const char *best = NULL;
    int best_d = strlen(w) > 4 ? 3 : 2;
    for (int i = 0; i < n; i++) {
        const char *name = names[i];
        const char *sp = strchr(name, ' ');
        int d = edit_distance(w, sp ? sp + 1 : name);
        if (d < best_d) best_d = d, best = name;
    }
    return best;
}

/* ---------- lfo ---------- */

static const char *const HZ[] = {"hz", NULL};
static const char *const DEG[] = {"deg", "°", NULL};

bool mod_parse_lfo(Command *c, char *err, size_t n) {
    ModCmd *m = &c->mod;
    memset(m, 0, sizeof *m);
    m->slot = -1;
    m->division = -1;
    if (c->nwords == 0) return true;

    float slot;
    if (!number_word(c->words[0], NULL, &slot) || slot != floorf(slot)
        || slot < 1.0f || slot > (float)MOD_LFOS)
        return reason(err, n, "lfo wants its number first, 1 to %d, not '%s'",
                      MOD_LFOS, c->words[0]);
    m->slot = (int)slot - 1;

    for (int i = 1; i < c->nwords; i++) {
        const char *w = c->words[i];
        const char *next = i + 1 < c->nwords ? c->words[i + 1] : NULL;
        int k;
        if (strcasecmp(w, "rm") == 0) {
            if (c->nwords != 2)
                return reason(err, n, "rm stands alone: lfo %d rm", m->slot + 1);
            m->rm = true;
        } else if (strcasecmp(w, "shape") == 0) {
            if (!next || shape_of(next) < 0)
                return reason(err, n,
                              "shape wants sine tri saw ramp square exp sh "
                              "drift, not '%s'",
                              next ? next : "nothing");
            m->set_shape = true;
            m->shape = (uint8_t)shape_of(next);
            i++;
        } else if ((k = shape_of(w)) >= 0) {
            m->set_shape = true;
            m->shape = (uint8_t)k;
        } else if (strcasecmp(w, "rate") == 0) {
            float hz;
            if (next && (k = division_of(next)) >= 0) {
                m->division = (int8_t)k;
            } else if (next && number_word(next, HZ, &hz)
                       && hz >= LFO_RATE_MIN_HZ && hz <= LFO_RATE_MAX_HZ) {
                m->division = -1;
                m->rate_hz = hz;
            } else {
                return reason(err, n,
                              "rate wants 0.01 to 40 hz, or a division like "
                              "1/4 or 1/8T, not '%s'",
                              next ? next : "nothing");
            }
            m->set_rate = true;
            i++;
        } else if ((k = division_of(w)) >= 0) {
            m->set_rate = true;
            m->division = (int8_t)k;
        } else if (strcasecmp(w, "phase") == 0) {
            float deg;
            if (!next || !number_word(next, DEG, &deg) || deg < 0.0f
                || deg > 360.0f)
                return reason(err, n, "phase wants 0 to 360 degrees, not '%s'",
                              next ? next : "nothing");
            m->set_phase = true;
            m->phase = deg >= 360.0f ? 0.0f : deg / 360.0f;
            i++;
        } else if (strcasecmp(w, "mode") == 0) {
            if (!next || mode_of(next) < 0)
                return reason(err, n, "mode wants free retrig or once, not '%s'",
                              next ? next : "nothing");
            m->set_mode = true;
            m->mode = (uint8_t)mode_of(next);
            i++;
        } else if ((k = mode_of(w)) >= 0) {
            m->set_mode = true;
            m->mode = (uint8_t)k;
        } else if (strcasecmp(w, "bi") == 0 || strcasecmp(w, "uni") == 0) {
            m->set_pol = true;
            m->unipolar = strcasecmp(w, "uni") == 0;
        } else if (strcasecmp(w, "to") == 0) {
            if (!next)
                return reason(err, n, "to wants a target and a depth: "
                                      "to index 0.4");
            int used;
            int t = target_of(c, i + 1, &used);
            if (t < 0) {
                const char *near = nearest_target(next);
                if (near)
                    return reason(err, n,
                                  "no target called '%s'; did you mean %s?",
                                  next, near);
                return reason(err, n,
                              "no target called '%s'; help lfo lists them",
                              next);
            }
            int at = i + 1 + used;
            if (at >= c->nwords)
                return reason(err, n, "to %s wants a depth, -1 to 1, or off",
                              MOD_TARGETS[t].name);
            if (m->nroutes >= MOD_CMD_ROUTES)
                return reason(err, n, "one line points at %d targets at most",
                              MOD_CMD_ROUTES);
            float depth = 0.0f;
            bool off = strcasecmp(c->words[at], "off") == 0;
            if (!off
                && (!number_word(c->words[at], NULL, &depth) || depth < -1.0f
                    || depth > 1.0f))
                return reason(err, n, "depth wants -1 to 1, or off, not '%s'",
                              c->words[at]);
            m->route[m->nroutes].target = (uint8_t)t;
            m->route[m->nroutes].depth = depth;
            m->route[m->nroutes].off = off;
            m->nroutes++;
            i = at;
        } else {
            const char *near = nearest(w, NULL, 0);
            if (near)
                return reason(err, n, "lfo has no word '%s'; did you mean %s?",
                              w, near);
            return reason(err, n, "lfo has no word '%s'", w);
        }
    }
    return true;
}

static void rate_text(const LfoParams *p, char *out, size_t cap) {
    if (p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN)
        snprintf(out, cap, "%s", CHANDAS_DIVISIONS[p->division].name);
    else if (p->rate_hz < 1.0f)
        snprintf(out, cap, "%.3ghz", (double)p->rate_hz);
    else
        snprintf(out, cap, "%.2fhz", (double)p->rate_hz);
}

static void lfo_text(const App *a, int slot, bool with_routes, char *out,
                     size_t cap) {
    const LfoParams *p = &a->mods.lfo[slot];
    char rate[32];
    rate_text(p, rate, sizeof rate);
    snprintf(out, cap, "lfo %-2d %-6s %-6s ph %3d  %-6s %s", slot + 1,
             lfo_shape_name((LfoShape)p->shape), rate,
             (int)lroundf(p->phase * 360.0f), lfo_mode_name((LfoMode)p->mode),
             p->unipolar ? "uni" : "bi");
    if (!with_routes) return;
    bool first = true;
    for (int r = 0; r < MOD_ROUTES; r++) {
        const ModRoute *rt = &a->mods.route[r];
        if (rt->target == MT_NONE || rt->lfo != slot) continue;
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s %+.2f", first ? "  -> " : ", ",
                 MOD_TARGETS[rt->target].name, (double)rt->depth);
        first = false;
    }
}

typedef struct {
    const LfoParams *p;
    uint32_t seed;
    float cycles;
    float depth;
} ShapeDraw;

static float shape_draw(float x, void *ud) {
    const ShapeDraw *d = ud;
    return lfo_shape_at(d->p, x * d->cycles, d->seed) * d->depth;
}

/* random shapes draw four cycles so their character shows; the rest one */
static void lfo_graph(const App *a, int slot, float depth, int cols, Graph *g) {
    const LfoParams *p = &a->mods.lfo[slot];
    bool random = p->shape == LFO_SH || p->shape == LFO_DRIFT;
    ShapeDraw d = {p, (uint32_t)slot + 1u, random ? 4.0f : 1.0f, depth};
    float mark = lfo_meter_phase(&a->lfo_meter, slot) / d.cycles;
    graph_plot(g, cols, shape_draw, &d, mark);
}

static void send_lfo(App *a, int slot) {
    app_send(a, (Event){.kind = EV_SET_LFO, .u.lfo = {slot, a->mods.lfo[slot]}});
}

static void send_route(App *a, int slot) {
    app_send(a,
             (Event){.kind = EV_SET_ROUTE, .u.route = {slot, a->mods.route[slot]}});
}

bool mod_view_lfo(App *a, const Command *c, View *out) {
    view_clear(out);
    const ModCmd *m = &c->mod;
    char line[VIEW_TEXT];
    for (int s = 0; s < MOD_LFOS; s++) {
        if (m->slot >= 0 && s != m->slot) continue;
        if (!a->mods.lfo[s].used) {
            if (m->slot >= 0) view_add(out, "lfo %d is gone", s + 1);
            continue;
        }
        lfo_text(a, s, true, line, sizeof line);
        ViewLine *l = view_add(out, "%s", line);
        if (!l) break;
        l->place = GRAPH_BELOW;
        lfo_graph(a, s, 1.0f, 96, &l->graph);
    }
    return out->n > 0;
}

bool mod_run_lfo(App *a, const Command *c, char *err, size_t n) {
    const ModCmd *m = &c->mod;
    View v;
    if (m->slot < 0) {
        if (!mod_view_lfo(a, c, &v)) {
            push_log(a, "no lfos yet. lfo 1 tri rate 2 to index 0.3 makes one");
            return true;
        }
        if (!c->view) push_log_view(a, &v);
        return true;
    }

    int s = m->slot;
    bool used = a->mods.lfo[s].used;
    bool sets = m->set_shape || m->set_rate || m->set_phase || m->set_mode
                || m->set_pol || m->nroutes > 0;

    if (m->rm) {
        if (!used) return reason(err, n, "there is no lfo %d", s + 1);
        a->mods.lfo[s].used = false;
        int gone = 0;
        for (int r = 0; r < MOD_ROUTES; r++) {
            if (a->mods.route[r].target == MT_NONE || a->mods.route[r].lfo != s)
                continue;
            memset(&a->mods.route[r], 0, sizeof a->mods.route[r]);
            send_route(a, r);
            gone++;
        }
        send_lfo(a, s);
        if (gone)
            push_log(a, "lfo %d is gone, and %d route%s with it", s + 1, gone,
                     gone == 1 ? "" : "s");
        else
            push_log(a, "lfo %d is gone", s + 1);
        return true;
    }

    if (!sets) {
        if (!used)
            return reason(err, n, "there is no lfo %d yet; lfo %d sine makes it",
                          s + 1, s + 1);
        if (!c->view && mod_view_lfo(a, c, &v)) push_log_view(a, &v);
        return true;
    }

    /* check every route change before touching anything */
    ModBank next = a->mods;
    for (int i = 0; i < m->nroutes; i++) {
        int t = m->route[i].target;
        int at = mod_bank_find_route(&next, s, (ModTarget)t);
        if (m->route[i].off) {
            if (at < 0)
                return reason(err, n, "lfo %d does not point at %s", s + 1,
                              MOD_TARGETS[t].name);
            memset(&next.route[at], 0, sizeof next.route[at]);
            continue;
        }
        if (at < 0)
            for (int r = 0; r < MOD_ROUTES && at < 0; r++)
                if (next.route[r].target == MT_NONE) at = r;
        if (at < 0)
            return reason(err, n,
                          "all %d routes are in use; mods shows them, "
                          "to <target> off frees one",
                          MOD_ROUTES);
        next.route[at] = (ModRoute){(uint8_t)s, (uint8_t)t, m->route[i].depth};
    }

    LfoParams p = used ? next.lfo[s] : lfo_params_default();
    p.used = true;
    if (m->set_shape) p.shape = m->shape;
    if (m->set_rate) {
        p.division = m->division;
        if (m->division < 0) p.rate_hz = m->rate_hz;
    }
    if (m->set_phase) p.phase = m->phase;
    if (m->set_mode) p.mode = m->mode;
    if (m->set_pol) p.unipolar = m->unipolar;
    next.lfo[s] = p;

    ModBank was = a->mods;
    a->mods = next;
    send_lfo(a, s);
    for (int r = 0; r < MOD_ROUTES; r++)
        if (memcmp(&was.route[r], &next.route[r], sizeof next.route[r]) != 0)
            send_route(a, r);
    if (!c->view && mod_view_lfo(a, c, &v)) push_log_view(a, &v);
    return true;
}

/* ---------- mods ---------- */

bool mod_parse_mods(Command *c, char *err, size_t n) {
    if (c->nwords > 0)
        return reason(err, n, "mods takes nothing but -v, not '%s'", c->words[0]);
    return true;
}

bool mod_view_mods(App *a, const Command *c, View *out) {
    (void)c;
    view_clear(out);
    int lfos = 0, routes = 0;
    for (int s = 0; s < MOD_LFOS; s++) lfos += a->mods.lfo[s].used;
    for (int r = 0; r < MOD_ROUTES; r++)
        routes += a->mods.route[r].target != MT_NONE;
    if (lfos == 0) {
        view_add(out, "mods  no lfos yet. lfo 1 tri rate 2 to index 0.3 makes one");
        return true;
    }
    view_add(out, "mods  %d lfo%s, %d route%s", lfos, lfos == 1 ? "" : "s",
             routes, routes == 1 ? "" : "s");
    char head[VIEW_TEXT];
    for (int s = 0; s < MOD_LFOS; s++) {
        if (!a->mods.lfo[s].used) continue;
        lfo_text(a, s, false, head, sizeof head);
        bool any = false;
        for (int r = 0; r < MOD_ROUTES; r++) {
            const ModRoute *rt = &a->mods.route[r];
            if (rt->target == MT_NONE || rt->lfo != s) continue;
            ViewLine *l = view_add(out, "  %-40s -> %-14s %+.2f",
                                   any ? "" : head, MOD_TARGETS[rt->target].name,
                                   (double)rt->depth);
            if (!l) return true;
            /* full height so the shape reads; the number carries the depth
               and the sign flips the drawing */
            l->place = GRAPH_RIGHT;
            lfo_graph(a, s, rt->depth < 0.0f ? -1.0f : 1.0f, 48, &l->graph);
            any = true;
        }
        if (!any) {
            ViewLine *l = view_add(out, "  %-40s -> nowhere yet", head);
            if (!l) return true;
            l->place = GRAPH_RIGHT;
            lfo_graph(a, s, 1.0f, 48, &l->graph);
        }
    }
    return true;
}

bool mod_run_mods(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    View v;
    if (!c->view && mod_view_mods(a, c, &v)) push_log_view(a, &v);
    return true;
}
