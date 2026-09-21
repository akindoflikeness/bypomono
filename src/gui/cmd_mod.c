/* The seq namespace: sequences, their routes, and their views. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "command.h"

/* ---------- where the audio thread's sequences are ---------- */

void seq_meter_store(SeqMeter *m, const Mod *mod) {
    for (int i = 0; i < SEQS; i++)
        atomic_store_explicit(&m->pos_q16[i],
                              (uint32_t)(mod->st[i].pos * 65536.0f),
                              memory_order_relaxed);
}

float seq_meter_pos(const SeqMeter *m, int slot) {
    if (slot < 0 || slot >= SEQS) return 0.0f;
    return (float)atomic_load_explicit(&((SeqMeter *)m)->pos_q16[slot],
                                       memory_order_relaxed)
           / 65536.0f;
}

/* ---------- sending ---------- */

/* sends whatever differs between the bank the app has and next, then keeps
   next; the GUI and the console both land their edits through here */
void mods_commit(App *a, const ModBank *next) {
    for (int s = 0; s < SEQS; s++)
        if (memcmp(&a->mods.seq[s], &next->seq[s], sizeof next->seq[s]) != 0)
            app_send(a, (Event){.kind = EV_SET_SEQ, .u.seq = {s, next->seq[s]}});
    for (int r = 0; r < MOD_ROUTES; r++)
        if (memcmp(&a->mods.route[r], &next->route[r], sizeof next->route[r]))
            app_send(a, (Event){.kind = EV_SET_ROUTE,
                                .u.route = {r, next->route[r]}});
    a->mods = *next;
}

/* adds or replaces seq -> target; false when every route is taken */
bool mods_route_set(ModBank *b, int seq, ModTarget t, float depth, bool snap) {
    int at = mod_bank_find_route(b, seq, t);
    for (int r = 0; r < MOD_ROUTES && at < 0; r++)
        if (b->route[r].target == MT_NONE) at = r;
    if (at < 0) return false;
    b->route[at] = (ModRoute){(uint8_t)seq, (uint8_t)t, depth,
                              snap && t == MT_PITCH};
    return true;
}

void mods_seq_remove(ModBank *b, int seq) {
    b->seq[seq].used = false;
    for (int r = 0; r < MOD_ROUTES; r++)
        if (b->route[r].target != MT_NONE && b->route[r].seq == seq)
            memset(&b->route[r], 0, sizeof b->route[r]);
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

/* "2.5s": seconds for the whole sequence */
static bool seconds_word(const char *w, float *out) {
    char *end = NULL;
    float v = strtof(w, &end);
    if (end == w || isnan(v) || isinf(v)) return false;
    if (strcasecmp(end, "s") != 0) return false;
    *out = v;
    return true;
}

static int fill_of(const char *w) {
    for (int i = 0; i < SEQ_FILL_COUNT; i++)
        if (strcasecmp(w, seq_fill_name((SeqFill)i)) == 0) return i;
    if (strcasecmp(w, "triangle") == 0) return SEQ_FILL_TRI;
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
        if (strcasecmp(two, "chandas dimension") == 0)
            snprintf(two, sizeof two, "chandas dim");
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

static bool parse_route_words(Command *c, int *i, char *err, size_t n) {
    SeqCmd *m = &c->seq;
    const char *next = *i + 1 < c->nwords ? c->words[*i + 1] : NULL;
    if (!next)
        return reason(err, n, "to wants a target and a depth: to index 0.4");
    int used;
    int t = target_of(c, *i + 1, &used);
    if (t < 0)
        return reason(err, n, "no target called '%s'; help seq lists them", next);
    int at = *i + 1 + used;
    if (at >= c->nwords)
        return reason(err, n, "to %s wants a depth, -1 to 1, or off",
                      MOD_TARGETS[t].name);
    if (m->nroutes >= SEQ_CMD_ROUTES)
        return reason(err, n, "one line points at %d targets at most",
                      SEQ_CMD_ROUTES);
    float depth = 0.0f;
    bool off = strcasecmp(c->words[at], "off") == 0;
    if (!off && (!number(c->words[at], &depth) || depth < -1.0f || depth > 1.0f))
        return reason(err, n, "depth wants -1 to 1, or off, not '%s'",
                      c->words[at]);
    bool snap = at + 1 < c->nwords && strcasecmp(c->words[at + 1], "snap") == 0;
    if (snap && t != MT_PITCH)
        return reason(err, n, "snap only goes with pitch");
    m->route[m->nroutes].target = (uint8_t)t;
    m->route[m->nroutes].depth = depth;
    m->route[m->nroutes].off = off;
    m->route[m->nroutes].snap = snap;
    m->nroutes++;
    *i = snap ? at + 1 : at;
    return true;
}

bool seq_parse(Command *c, char *err, size_t n) {
    SeqCmd *m = &c->seq;
    memset(m, 0, sizeof *m);
    m->slot = -1;
    if (c->nwords == 0) return true;
    if (!whole(c->words[0], 1, SEQS, &m->slot))
        return reason(err, n, "seq wants its number first, 1 to %d, not '%s'",
                      SEQS, c->words[0]);
    m->slot--;

    for (int i = 1; i < c->nwords; i++) {
        const char *w = c->words[i];
        const char *next = i + 1 < c->nwords ? c->words[i + 1] : NULL;
        int k;
        float v;
        if (strcasecmp(w, "rm") == 0) {
            if (c->nwords != 2)
                return reason(err, n, "rm stands alone: seq %d rm", m->slot + 1);
            m->rm = true;
        } else if (strcasecmp(w, "loop") == 0 || strcasecmp(w, "once") == 0) {
            m->set_mode = true;
            m->mode = strcasecmp(w, "once") == 0 ? SEQ_ONCE : SEQ_LOOP;
        } else if (strcasecmp(w, "smooth") == 0 || strcasecmp(w, "steps") == 0) {
            m->set_smooth = true;
            m->smooth = strcasecmp(w, "smooth") == 0;
        } else if (strcasecmp(w, "fill") == 0) {
            if (!next || fill_of(next) < 0)
                return reason(err, n, "fill wants flat sine saw ramp tri square "
                                      "or random, not '%s'",
                              next ? next : "nothing");
            m->set_fill = true;
            m->fill = (uint8_t)fill_of(next);
            i++;
        } else if (strcasecmp(w, "rate") == 0) {
            if (next && (k = division_of(next)) >= 0) {
                m->division = (int8_t)k;
            } else if (next && seconds_word(next, &v) && v >= SEQ_LENGTH_MIN_S
                       && v <= SEQ_LENGTH_MAX_S) {
                m->division = -1;
                m->length_s = v;
            } else {
                return reason(err, n,
                              "rate wants a step length like 1/16, or seconds "
                              "for all 16 steps like 2.5s, not '%s'",
                              next ? next : "nothing");
            }
            m->set_rate = true;
            i++;
        } else if (strcasecmp(w, "set") == 0) {
            if (c->nwords - i - 1 < SEQ_STEPS)
                return reason(err, n, "set wants all %d values, 0 to 1",
                              SEQ_STEPS);
            for (int s = 0; s < SEQ_STEPS; s++) {
                const char *vw = c->words[i + 1 + s];
                if (!number(vw, &v) || v < 0.0f || v > 1.0f)
                    return reason(err, n, "values go 0 to 1, not '%s'", vw);
                m->values[s] = v;
            }
            m->set_values = true;
            i += SEQ_STEPS;
        } else if (strcasecmp(w, "step") == 0) {
            int step;
            if (!next || !whole(next, 1, SEQ_STEPS, &step) || i + 2 >= c->nwords)
                return reason(err, n, "step wants a step, 1 to 16, then a value");
            const char *val = c->words[i + 2];
            if (!number(val, &v) || v < 0.0f || v > 1.0f)
                return reason(err, n, "a step value goes 0 to 1, not '%s'", val);
            m->steps[m->nsteps].step = (uint8_t)(step - 1);
            m->steps[m->nsteps].v = v;
            m->nsteps++;
            i += 2;
        } else if (strcasecmp(w, "to") == 0) {
            if (!parse_route_words(c, &i, err, n)) return false;
        } else {
            return reason(err, n, "seq has no word '%s'; help seq lists them", w);
        }
    }
    return true;
}

/* ---------- applying ---------- */

/* the bank a line would leave behind; false leaves the reason in err */
static bool apply_seq_cmd(const SeqCmd *m, ModBank *bank, char *err, size_t n) {
    int s = m->slot;
    bool used = bank->seq[s].used;
    if (m->rm) {
        if (!used) return reason(err, n, "there is no seq %d", s + 1);
        mods_seq_remove(bank, s);
        return true;
    }
    SeqParams p = used ? bank->seq[s] : seq_params_default();
    p.used = true;
    if (m->set_fill) seq_fill(&p, (SeqFill)m->fill, (uint32_t)s + 1u);
    if (m->set_values) memcpy(p.value, m->values, sizeof p.value);
    for (int i = 0; i < m->nsteps; i++) p.value[m->steps[i].step] = m->steps[i].v;
    if (m->set_mode) p.mode = m->mode;
    if (m->set_smooth) p.smooth = m->smooth;
    if (m->set_rate) {
        p.division = m->division;
        if (m->division < 0) p.length_s = m->length_s;
    }
    bank->seq[s] = p;
    for (int i = 0; i < m->nroutes; i++) {
        ModTarget t = (ModTarget)m->route[i].target;
        if (m->route[i].off) {
            int at = mod_bank_find_route(bank, s, t);
            if (at < 0)
                return reason(err, n, "seq %d does not point at %s", s + 1,
                              MOD_TARGETS[t].name);
            memset(&bank->route[at], 0, sizeof bank->route[at]);
        } else if (!mods_route_set(bank, s, t, m->route[i].depth,
                                   m->route[i].snap)) {
            return reason(err, n, "all %d routes are in use; to <target> off "
                                  "frees one", MOD_ROUTES);
        }
    }
    return true;
}

/* ---------- views ---------- */

void seq_rate_text(const SeqParams *p, char *out, size_t cap) {
    if (p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN)
        snprintf(out, cap, "%s", CHANDAS_DIVISIONS[p->division].name);
    else
        snprintf(out, cap, "%.2fs", (double)p->length_s);
}

static void seq_text(const ModBank *bank, int slot, char *out, size_t cap) {
    const SeqParams *p = &bank->seq[slot];
    char rate[32];
    seq_rate_text(p, rate, sizeof rate);
    snprintf(out, cap, "seq %d  %s %s %s", slot + 1,
             seq_mode_name((SeqMode)p->mode), p->smooth ? "smooth" : "steps",
             rate);
    bool first = true;
    for (int r = 0; r < MOD_ROUTES; r++) {
        const ModRoute *rt = &bank->route[r];
        if (rt->target == MT_NONE || rt->seq != slot) continue;
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s %+.2f%s", first ? "  -> " : ", ",
                 MOD_TARGETS[rt->target].name, (double)rt->depth,
                 rt->snap ? " snap" : "");
        first = false;
    }
}

static float seq_draw(float x, void *ud) {
    return 2.0f * seq_value_at(ud, x * (float)SEQ_STEPS) - 1.0f;
}

static void seq_graph(const App *a, const ModBank *bank, int slot, int cols,
                      Graph *g) {
    float mark = seq_meter_pos(&a->seq_meter, slot) / (float)SEQ_STEPS;
    graph_plot(g, cols, seq_draw, (void *)&bank->seq[slot], mark);
}

/* one line and one strip per sequence; slot < 0 is every one in use */
static bool seq_lines(const App *a, const ModBank *bank, int slot, View *out) {
    char line[VIEW_TEXT];
    for (int s = 0; s < SEQS; s++) {
        if (slot >= 0 && s != slot) continue;
        if (!bank->seq[s].used) {
            if (slot >= 0) view_add(out, "seq %d is empty", s + 1);
            continue;
        }
        seq_text(bank, s, line, sizeof line);
        ViewLine *l = view_add(out, "%s", line);
        if (!l) break;
        l->place = GRAPH_BELOW;
        seq_graph(a, bank, s, 96, &l->graph);
    }
    if (out->n == 0)
        view_add(out, "no sequences yet. seq 1 fill sine to index 0.3 makes one");
    return true;
}

bool seq_view(App *a, const Command *c, View *out) {
    view_clear(out);
    return seq_lines(a, &a->mods, c->seq.slot, out);
}

bool seq_preview(App *a, const Command *c, View *out) {
    view_clear(out);
    const SeqCmd *m = &c->seq;
    if (m->slot < 0) {
        view_add(out, "lists every sequence");
        return true;
    }
    ModBank next = a->mods;
    char err[256];
    if (!apply_seq_cmd(m, &next, err, sizeof err)) {
        view_add(out, "%s", err);
        return true;
    }
    if (m->rm) {
        view_add(out, "seq %d goes, and every route from it", m->slot + 1);
        return true;
    }
    return seq_lines(a, &next, m->slot, out);
}

bool seq_run(App *a, const Command *c, char *err, size_t n) {
    const SeqCmd *m = &c->seq;
    View v;
    bool sets = m->set_mode || m->set_smooth || m->set_rate || m->set_fill
                || m->set_values || m->nsteps || m->nroutes;
    if (m->slot >= 0 && !sets && !m->rm && !a->mods.seq[m->slot].used)
        return reason(err, n, "there is no seq %d yet; seq %d fill flat makes it",
                      m->slot + 1, m->slot + 1);
    if (m->slot >= 0 && (sets || m->rm)) {
        /* the whole line lands, or none of it does */
        ModBank next = a->mods;
        if (!apply_seq_cmd(m, &next, err, n)) return false;
        mods_commit(a, &next);
        if (m->rm) {
            push_log(a, "seq %d is gone", m->slot + 1);
            return true;
        }
    }
    if (!c->view && seq_view(a, c, &v)) push_log_view(a, &v);
    return true;
}

/* ---------- completion ---------- */

static int add(char out[][CAND_LEN], int n, int max, const char *prefix,
               const char *word) {
    if (n >= max || strncasecmp(word, prefix, strlen(prefix)) != 0) return n;
    snprintf(out[n], CAND_LEN, "%s", word);
    return n + 1;
}

int seq_complete(char *const words[], int nwords, const char *prefix,
                 char out[][CAND_LEN], int max) {
    int n = 0;
    if (nwords == 0) {
        for (int i = 1; i <= SEQS; i++) {
            char num[8];
            snprintf(num, sizeof num, "%d", i);
            n = add(out, n, max, prefix, num);
        }
        return n;
    }
    const char *prev = words[nwords - 1];
    if (strcasecmp(prev, "fill") == 0) {
        for (int i = 0; i < SEQ_FILL_COUNT; i++)
            n = add(out, n, max, prefix, seq_fill_name((SeqFill)i));
        return n;
    }
    if (strcasecmp(prev, "rate") == 0) {
        for (int i = 0; i < CHANDAS_DIVISIONS_LEN; i++)
            n = add(out, n, max, prefix, CHANDAS_DIVISIONS[i].name);
        return add(out, n, max, prefix, "2s");
    }
    if (strcasecmp(prev, "to") == 0) {
        for (int t = MT_INDEX; t < MT_COUNT; t++)
            n = add(out, n, max, prefix, MOD_TARGETS[t].name);
        return n;
    }
    static const char *const KEYS[] = {"fill", "set",  "step",  "loop",
                                       "once", "smooth", "steps", "rate",
                                       "to",   "rm"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        n = add(out, n, max, prefix, KEYS[i]);
    return n;
}
