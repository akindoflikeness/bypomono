#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"

/* widgets.c */
extern float hint_chip(Ui *ui, P2 at, const char *text, bool lit);

#define TYPE_CPS 24.0f
#define TYPE_JITTER 0.7f
#define CONSOLE_CPS 80.0f

#define ARG_SEP " - "
#define SELECTED_WORD "selected"

/* ---------- tips ---------- */

typedef enum {
    W_ALWAYS = 0, W_HIGHLIGHTED, W_BROWSING, W_COMMANDING, W_NOTHING_SAVED
} TipWhen;

typedef struct {
    TipWhen needs;
    const char *text;
} Tip;

static const Tip TIPS[] = {
    {W_ALWAYS, "press / to type a command. ? on its own lists them."},
    {W_ALWAYS, "USER holds everything you made, wherever you filed it."},
    {W_COMMANDING,
     "move <preset> - <folder> files a preset. add <folder> makes the folder."},
    {W_COMMANDING,
     "put ? after any verb — move ? — and it will tell you how it is spelled."},
    {W_HIGHLIGHTED,
     "selected stands for the highlighted preset: move selected - <folder>."},
    {W_HIGHLIGHTED,
     "rename <preset> - <new name> renames it where it sits, folder and all."},
    {W_BROWSING, "click a preset to highlight it, click it again to load it."},
    {W_ALWAYS, "two clicks on the bar, under a second, opens it to type in."},
    {W_NOTHING_SAVED, "save never clobbers — a taken name earns a ~1."},
    {W_COMMANDING,
     "delete <preset> takes a preset. remove <folder> takes the folder and all "
     "of it."},
    {W_HIGHLIGHTED,
     "overwrite selected replaces it with the sound you have now."},
};

void tell_new_tips(App *a) {
    TipWhen hot[5];
    int n = 0;
    hot[n++] = W_ALWAYS;
    if (a->have_selected) hot[n++] = W_HIGHLIGHTED;
    if (a->presets_open) hot[n++] = W_BROWSING;
    if (a->console_open) hot[n++] = W_COMMANDING;
    bool made = false;
    for (int i = 0; i < a->preset_count; i++)
        if (strcmp(a->preset_names[i].bank, STOCK_BANK) != 0) {
            made = true;
            break;
        }
    if (!made) hot[n++] = W_NOTHING_SAVED;
    for (int i = 0; i < n; i++) {
        uint32_t bit = 1u << hot[i];
        if (a->tips_told & bit) continue;
        a->tips_told |= bit;
        for (size_t t = 0; t < sizeof TIPS / sizeof TIPS[0]; t++)
            if (TIPS[t].needs == hot[i]) push_log(a, "%s", TIPS[t].text);
    }
}

/* ---------- small helpers ---------- */

static void scat(char *out, size_t cap, const char *fmt, ...) {
    size_t n = strlen(out);
    if (n >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
}

static void trim_into(const char *s, char *out, size_t cap) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static void lower_into(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = '\0';
}

static int utf8_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

static int utf8_prefix_bytes(const char *s, int nchars) {
    int i = 0;
    while (s[i] && nchars > 0) {
        i++;
        while (((unsigned char)s[i] & 0xC0) == 0x80) i++;
        nchars--;
    }
    return i;
}

/* ---------- the command language ---------- */

typedef enum {
    V_RECORD, V_SAVE, V_OVERWRITE, V_DELETE, V_RENAME, V_MOVE,
    V_ADD, V_REMOVE, V_BIND, V_UNBIND
} Verb;

typedef struct {
    const char *name;
    const char *value; /* NULL = bare flag */
    const char *effect;
} Flag;

typedef struct {
    const char *name;
    const char *alias; /* at most one in the roster */
    Verb verb;
    const char *effect;
    const char *args[2];
    int nargs, required;
    const Flag *flags;
    int nflags;
    bool arms;
} Spec;

static const Flag REC_FLAGS[] = {
    {"end", "seconds", "finish the take on its own after this long"},
};

static const Spec ROSTER[] = {
    {"rec", "record", V_RECORD, "start a take, or finish the one running",
     {"name", NULL}, 1, 0, REC_FLAGS, 1, false},
    {"save", NULL, V_SAVE, "write the current sound under a name",
     {"name", NULL}, 1, 1, NULL, 0, true},
    {"overwrite", NULL, V_OVERWRITE, "replace a preset with the current sound",
     {"preset", NULL}, 1, 1, NULL, 0, true},
    {"delete", NULL, V_DELETE, "remove a preset, or the highlighted one",
     {"name", NULL}, 1, 0, NULL, 0, true},
    {"rename", NULL, V_RENAME,
     "give a preset a different name, where it sits", {"preset", "new name"},
     2, 2, NULL, 0, true},
    {"move", NULL, V_MOVE, "put a preset in a folder that already exists",
     {"preset", "folder"}, 2, 2, NULL, 0, true},
    {"add", NULL, V_ADD, "make an empty folder", {"folder", NULL}, 1, 1, NULL,
     0, true},
    {"remove", NULL, V_REMOVE, "delete a folder and everything in it",
     {"folder", NULL}, 1, 1, NULL, 0, true},
    {"bind", NULL, V_BIND, "point a controller at a control",
     {"cc", "control"}, 2, 2, NULL, 0, false},
    {"unbind", NULL, V_UNBIND, "let a controller go, or 'all' of them",
     {"cc", NULL}, 1, 1, NULL, 0, false},
};
#define NROSTER ((int)(sizeof ROSTER / sizeof ROSTER[0]))

static void spec_usage(const Spec *s, char *out, size_t cap) {
    snprintf(out, cap, "%s", s->name);
    for (int i = 0; i < s->nargs; i++) {
        if (i < s->required)
            scat(out, cap, "%s<%s>", i == 0 ? " " : ARG_SEP, s->args[i]);
        else
            scat(out, cap, "%s[%s]", i == 0 ? " " : ARG_SEP, s->args[i]);
    }
    for (int i = 0; i < s->nflags; i++) {
        if (s->flags[i].value)
            scat(out, cap, " [--%s <%s>]", s->flags[i].name, s->flags[i].value);
        else
            scat(out, cap, " [--%s]", s->flags[i].name);
    }
}

static const Flag *spec_flag(const Spec *s, const char *name) {
    for (int i = 0; i < s->nflags; i++)
        if (strcasecmp(s->flags[i].name, name) == 0) return &s->flags[i];
    return NULL;
}

static bool spec_matches(const Spec *s, const char *word) {
    if (strcasecmp(s->name, word) == 0) return true;
    return s->alias && strcasecmp(s->alias, word) == 0;
}

static void roster_names(char *out, size_t cap) {
    out[0] = '\0';
    for (int i = 0; i < NROSTER; i++)
        scat(out, cap, "%s%s", i == 0 ? "" : ", ", ROSTER[i].name);
}

static void bindable_list(char *out, size_t cap) {
    snprintf(out, cap, "continuous controls only:");
    for (int t = CC_INDEX; t <= CC_WARMTH; t++)
        scat(out, cap, " %s", cc_target_name((CcTarget)t));
}

typedef struct {
    Verb verb;
    char arg0[192], arg1[192];
    int argc;
    bool has_end;
    float end;
    int cc;
    bool cc_all;
    CcTarget target;
} Cmd;

static const Spec *spec_of(Verb v) {
    for (int i = 0; i < NROSTER; i++)
        if (ROSTER[i].verb == v) return &ROSTER[i];
    return &ROSTER[0];
}

static void cmd_echo(const Cmd *c, char *out, size_t cap) {
    switch (c->verb) {
    case V_RECORD:
        snprintf(out, cap, "rec");
        if (c->argc > 0) scat(out, cap, " %s", c->arg0);
        if (c->has_end) scat(out, cap, " --end %g", (double)c->end);
        break;
    case V_SAVE: snprintf(out, cap, "save %s", c->arg0); break;
    case V_OVERWRITE: snprintf(out, cap, "overwrite %s", c->arg0); break;
    case V_DELETE:
        if (c->argc > 0)
            snprintf(out, cap, "delete %s", c->arg0);
        else
            snprintf(out, cap, "delete");
        break;
    case V_RENAME:
        snprintf(out, cap, "rename %s%s%s", c->arg0, ARG_SEP, c->arg1);
        break;
    case V_MOVE:
        snprintf(out, cap, "move %s%s%s", c->arg0, ARG_SEP, c->arg1);
        break;
    case V_ADD: snprintf(out, cap, "add %s", c->arg0); break;
    case V_BIND:
        snprintf(out, cap, "bind cc%d%s%s", c->cc, ARG_SEP,
                 cc_target_name(c->target));
        break;
    case V_UNBIND:
        if (c->cc_all)
            snprintf(out, cap, "unbind all");
        else
            snprintf(out, cap, "unbind cc%d", c->cc);
        break;
    case V_REMOVE: snprintf(out, cap, "remove %s", c->arg0); break;
    }
}

static int parse_cc_word(const char *w) {
    const char *d = w;
    if (strncmp(w, "cc", 2) == 0 || strncmp(w, "CC", 2) == 0) d = w + 2;
    if (!*d || strlen(d) > 3) return -1;
    for (const char *p = d; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    long n = strtol(d, NULL, 10);
    return n < 128 ? (int)n : -1;
}

typedef enum { L_SEARCH, L_HELP, L_ROSTER, L_READY, L_BAD } LineKind;

typedef struct {
    LineKind kind;
    const Spec *spec; /* help */
    Cmd cmd;          /* ready */
    char why[768];
} Parsed;

static Parsed parse_line(const char *raw_in) {
    Parsed out;
    memset(&out, 0, sizeof out);
    char raw[LOG_LINE_LEN];
    trim_into(raw_in, raw, sizeof raw);
    if (!raw[0]) {
        out.kind = L_SEARCH;
        return out;
    }
    if (strcmp(raw, "?") == 0) {
        out.kind = L_ROSTER;
        return out;
    }
    char head[256], tail[LOG_LINE_LEN];
    const char *sp = strchr(raw, ' ');
    if (sp) {
        size_t hn = (size_t)(sp - raw);
        if (hn >= sizeof head) hn = sizeof head - 1;
        memcpy(head, raw, hn);
        head[hn] = '\0';
        trim_into(sp + 1, tail, sizeof tail);
    } else {
        snprintf(head, sizeof head, "%s", raw);
        tail[0] = '\0';
    }
    const Spec *spec = NULL;
    for (int i = 0; i < NROSTER; i++)
        if (spec_matches(&ROSTER[i], head)) {
            spec = &ROSTER[i];
            break;
        }
    if (!spec) {
        out.kind = L_SEARCH;
        return out;
    }
    if (strcmp(tail, "?") == 0) {
        out.kind = L_HELP;
        out.spec = spec;
        return out;
    }

    char usage[LOG_LINE_LEN];
    spec_usage(spec, usage, sizeof usage);

    struct { const Flag *f; char val[64]; } got[4];
    int ngot = 0;
    char rest[LOG_LINE_LEN] = "";
    char toks[LOG_LINE_LEN];
    snprintf(toks, sizeof toks, "%s", tail);
    char *save = NULL;
    for (char *tok = strtok_r(toks, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (tok[0] != '-' || tok[1] != '-') {
            scat(rest, sizeof rest, "%s%s", rest[0] ? " " : "", tok);
            continue;
        }
        const Flag *f = spec_flag(spec, tok + 2);
        if (!f) {
            out.kind = L_BAD;
            snprintf(out.why, sizeof out.why, "'%s' is not a flag of %s. %s.",
                     tok, spec->name, usage);
            return out;
        }
        if (f->value) {
            char *val = strtok_r(NULL, " \t\r\n", &save);
            if (!val) {
                out.kind = L_BAD;
                snprintf(out.why, sizeof out.why, "--%s wants a %s. %s.",
                         f->name, f->value, usage);
                return out;
            }
            if (ngot < 4) {
                got[ngot].f = f;
                snprintf(got[ngot].val, sizeof got[ngot].val, "%s", val);
                ngot++;
            }
        } else if (ngot < 4) {
            got[ngot].f = f;
            got[ngot].val[0] = '\0';
            ngot++;
        }
    }

    char given[2][192];
    int ngiven = 0;
    if (rest[0] && spec->nargs > 0) {
        if (spec->nargs >= 2) {
            char *sep = NULL;
            for (char *p = strstr(rest, ARG_SEP); p; p = strstr(p + 1, ARG_SEP))
                sep = p;
            if (sep) {
                *sep = '\0';
                trim_into(rest, given[0], sizeof given[0]);
                trim_into(sep + 3, given[1], sizeof given[1]);
                ngiven = 2;
            } else {
                trim_into(rest, given[0], sizeof given[0]);
                ngiven = 1;
            }
        } else {
            trim_into(rest, given[0], sizeof given[0]);
            ngiven = 1;
        }
    }

    if (ngiven < spec->required || ngiven > spec->nargs) {
        out.kind = L_BAD;
        snprintf(out.why, sizeof out.why, "%s — %s.", usage, spec->effect);
        return out;
    }

    char clean[2][192];
    for (int i = 0; i < ngiven; i++) {
        if (strcasecmp(given[i], SELECTED_WORD) == 0) {
            snprintf(clean[i], sizeof clean[i], "%s", SELECTED_WORD);
            continue;
        }
        sanitise_segment(given[i], clean[i], sizeof clean[i]);
        if (!clean[i][0]) {
            out.kind = L_BAD;
            snprintf(out.why, sizeof out.why,
                     "'%s' cannot be a name — no slashes, and not empty.",
                     given[i]);
            return out;
        }
    }

    Cmd *c = &out.cmd;
    c->verb = spec->verb;
    c->argc = ngiven;
    if (ngiven > 0) snprintf(c->arg0, sizeof c->arg0, "%s", clean[0]);
    if (ngiven > 1) snprintf(c->arg1, sizeof c->arg1, "%s", clean[1]);

    switch (spec->verb) {
    case V_RECORD:
        for (int i = 0; i < ngot; i++) {
            if (strcmp(got[i].f->name, "end") != 0) continue;
            char *endp = NULL;
            float n = strtof(got[i].val, &endp);
            if (!got[i].val[0] || !endp || *endp || !(n > 0.0f)) {
                out.kind = L_BAD;
                snprintf(out.why, sizeof out.why,
                         "--end wants a number of seconds, not '%s'.",
                         got[i].val);
                return out;
            }
            c->has_end = true;
            c->end = n;
        }
        break;
    case V_BIND: {
        int cc = parse_cc_word(clean[0]);
        if (cc < 0) {
            out.kind = L_BAD;
            snprintf(out.why, sizeof out.why,
                     "'%s' is not a controller. cc0 to cc127, or just the "
                     "number.",
                     clean[0]);
            return out;
        }
        CcTarget t = cc_target_from_name(clean[1]);
        if (t == CC_NONE) {
            char list[LOG_LINE_LEN];
            bindable_list(list, sizeof list);
            out.kind = L_BAD;
            snprintf(out.why, sizeof out.why, "'%s' cannot be bound. %s.",
                     clean[1], list);
            return out;
        }
        c->cc = cc;
        c->target = t;
        break;
    }
    case V_UNBIND:
        if (strcasecmp(clean[0], "all") == 0) {
            c->cc_all = true;
        } else {
            int cc = parse_cc_word(clean[0]);
            if (cc < 0) {
                out.kind = L_BAD;
                snprintf(out.why, sizeof out.why,
                         "'%s' is not a controller. cc0 to cc127, or 'all'.",
                         clean[0]);
                return out;
            }
            c->cc = cc;
        }
        break;
    default: break;
    }

    out.kind = L_READY;
    return out;
}

/* ---------- preset resolution for arming notes ---------- */

static bool filter_matches(const App *a, const PresetRef *p) {
    switch (a->preset_filter.kind) {
    case FILTER_ALL: return true;
    case FILTER_MINE: return strcmp(p->bank, STOCK_BANK) != 0;
    case FILTER_BANK:
        return p->bank[0] && strcmp(p->bank, a->preset_filter.bank) == 0;
    }
    return true;
}

static bool console_resolve(const App *a, const char *name, PresetRef *out) {
    char trimmed[192], want[192];
    trim_into(name, trimmed, sizeof trimmed);
    lower_into(trimmed, want, sizeof want);
    if (strcmp(want, SELECTED_WORD) == 0) {
        if (!a->have_selected) return false;
        *out = a->preset_selected;
        return true;
    }
    int hits = 0;
    const PresetRef *hit = NULL;
    for (int pass = 0; pass < 2 && hits == 0; pass++) {
        for (int i = 0; i < a->preset_count; i++) {
            const PresetRef *p = &a->preset_names[i];
            if (pass == 0 && !filter_matches(a, p)) continue;
            char low[192];
            lower_into(p->name, low, sizeof low);
            if (strcmp(low, want) != 0) continue;
            hits++;
            hit = p;
        }
    }
    if (hits != 1) return false;
    *out = *hit;
    return true;
}

static bool arming_note(const App *a, const Cmd *c, char *out, size_t cap) {
    if (c->verb == V_REMOVE) {
        int n = 0;
        for (int i = 0; i < a->preset_count; i++)
            if (strcmp(a->preset_names[i].bank, c->arg0) == 0) n++;
        if (n == 0)
            snprintf(out, cap, "delete '%s', which is empty", c->arg0);
        else if (n == 1)
            snprintf(out, cap, "delete '%s' and the 1 preset in it", c->arg0);
        else
            snprintf(out, cap, "delete '%s' and the %d presets in it", c->arg0,
                     n);
        return true;
    }
    if (c->verb == V_OVERWRITE) {
        PresetRef p;
        if (!console_resolve(a, c->arg0, &p)) return false;
        char q[256];
        preset_qualified(&p, q, sizeof q);
        snprintf(out, cap, "replace '%s' with the current sound", q);
        return true;
    }
    return false;
}

/* ---------- hints ---------- */

typedef enum { H_ROSTER, H_COMPLETING, H_PREVIEW, H_BAD } HintKind;

typedef struct {
    HintKind kind;
    const Spec *m[NROSTER];
    int m_len;
    char line[512], note[768];
    bool arms;
    char why[768];
} Hint;

static void hint_for(const App *a, const char *raw_in, bool with_note,
                     Hint *out) {
    memset(out, 0, sizeof *out);
    char raw[LOG_LINE_LEN];
    trim_into(raw_in, raw, sizeof raw);
    if (!raw[0]) {
        out->kind = H_ROSTER;
        return;
    }
    char head[64];
    size_t hn = 0;
    while (raw[hn] && !isspace((unsigned char)raw[hn])) hn++;
    size_t copy = hn < sizeof head - 1 ? hn : sizeof head - 1;
    memcpy(head, raw, copy);
    head[copy] = '\0';
    bool single = raw[hn] == '\0';
    bool known = false;
    for (int i = 0; i < NROSTER; i++)
        if (spec_matches(&ROSTER[i], head)) known = true;
    if (single && !known) {
        char low[64];
        lower_into(head, low, sizeof low);
        size_t ln = strlen(low);
        for (int i = 0; i < NROSTER; i++) {
            const Spec *s = &ROSTER[i];
            if (strncmp(s->name, low, ln) == 0
                || (s->alias && strncmp(s->alias, low, ln) == 0))
                out->m[out->m_len++] = s;
        }
        if (out->m_len == 0) {
            out->kind = H_BAD;
            snprintf(out->why, sizeof out->why, "'%s' is not a command.",
                     head);
        } else {
            out->kind = H_COMPLETING;
        }
        return;
    }
    Parsed p = parse_line(raw);
    switch (p.kind) {
    case L_READY: {
        out->kind = H_PREVIEW;
        cmd_echo(&p.cmd, out->line, sizeof out->line);
        const Spec *spec = spec_of(p.cmd.verb);
        if (!with_note || !arming_note(a, &p.cmd, out->note, sizeof out->note))
            snprintf(out->note, sizeof out->note, "%s", spec->effect);
        out->arms = spec->arms;
        break;
    }
    case L_BAD:
        out->kind = H_BAD;
        snprintf(out->why, sizeof out->why, "%s", p.why);
        break;
    case L_HELP: {
        char usage[LOG_LINE_LEN];
        spec_usage(p.spec, usage, sizeof usage);
        out->kind = H_BAD;
        snprintf(out->why, sizeof out->why, "%s — %s.", usage,
                 p.spec->effect);
        break;
    }
    case L_ROSTER: out->kind = H_ROSTER; break;
    case L_SEARCH:
        out->kind = H_BAD;
        snprintf(out->why, sizeof out->why, "'%s' is not a command.", head);
        break;
    }
}

/* ---------- running ---------- */

/* the armed command, held as its canonical echo; valid while
   App.preset_armed != 0 (preset_armed carries verb+1 as the Command id) */
static char armed_echo[512];

static void run_cmd(App *a, const Cmd *c) {
    switch (c->verb) {
    case V_RECORD: {
        char args[512] = "";
        if (c->argc > 0) snprintf(args, sizeof args, "%s", c->arg0);
        if (c->has_end)
            scat(args, sizeof args, "%s--end %g", args[0] ? " " : "",
                 (double)c->end);
        gui_run_record(a, args);
        break;
    }
    case V_SAVE: preset_run_save(a, c->arg0); break;
    case V_OVERWRITE: preset_run_overwrite(a, c->arg0); break;
    case V_DELETE: preset_run_delete(a, c->argc > 0 ? c->arg0 : ""); break;
    case V_RENAME: {
        char args[512];
        snprintf(args, sizeof args, "%s%s%s", c->arg0, ARG_SEP, c->arg1);
        preset_run_rename(a, args);
        break;
    }
    case V_MOVE: {
        char args[512];
        snprintf(args, sizeof args, "%s%s%s", c->arg0, ARG_SEP, c->arg1);
        preset_run_move(a, args);
        break;
    }
    case V_ADD: preset_run_add(a, c->arg0); break;
    case V_REMOVE: preset_run_remove(a, c->arg0); break;
    case V_BIND: gui_run_bind(a, c->cc, c->target); break;
    case V_UNBIND: gui_run_unbind(a, c->cc_all ? -1 : c->cc); break;
    }
}

static void arm_or_run(App *a, const Cmd *c) {
    const Spec *spec = spec_of(c->verb);
    char echo[512];
    cmd_echo(c, echo, sizeof echo);
    if (!spec->arms) {
        a->preset_armed = 0;
        push_log(a, "%s", echo);
        run_cmd(a, c);
        return;
    }
    if (!(a->preset_armed && strcmp(armed_echo, echo) == 0)) {
        char what[768];
        if (!arming_note(a, c, what, sizeof what))
            snprintf(what, sizeof what, "%s", spec->effect);
        push_log(a, "%s — %s. again to confirm.", echo, what);
        snprintf(armed_echo, sizeof armed_echo, "%s", echo);
        a->preset_armed = (int)c->verb + 1;
        return;
    }
    a->preset_armed = 0;
    push_log(a, "%s", echo);
    run_cmd(a, c);
}

void console_run_line(App *a, const char *line) {
    Parsed p = parse_line(line);
    switch (p.kind) {
    case L_READY: arm_or_run(a, &p.cmd); break;
    case L_BAD:
        push_log(a, "%s", p.why);
        a->preset_armed = 0;
        break;
    case L_HELP: {
        char usage[LOG_LINE_LEN];
        spec_usage(p.spec, usage, sizeof usage);
        push_log(a, "%s — %s.", usage, p.spec->effect);
        a->preset_armed = 0;
        break;
    }
    case L_ROSTER: {
        char names[LOG_LINE_LEN];
        roster_names(names, sizeof names);
        push_log(a, "%s — put ? after any of them.", names);
        a->preset_armed = 0;
        break;
    }
    case L_SEARCH: {
        char trimmed[LOG_LINE_LEN], names[LOG_LINE_LEN];
        trim_into(line, trimmed, sizeof trimmed);
        roster_names(names, sizeof names);
        push_log(a, "'%s' is not a command. try: %s.", trimmed, names);
        a->preset_armed = 0;
        break;
    }
    }
}

void console_tab_complete(App *a) {
    if (!a->console_open || !a->console_focused) return;
    Hint h;
    hint_for(a, a->console_input.text, false, &h);
    if (h.kind != H_COMPLETING || h.m_len == 0) return;
    snprintf(a->console_input.text, sizeof a->console_input.text, "%s ",
             h.m[0]->name);
    a->console_input.len = (int)strlen(a->console_input.text);
    a->console_open = true;
    a->console_focus = true;
}

/* ---------- typewriter ---------- */

static float char_dwell(int i, uint32_t prev, bool console) {
    if (console) return 1.0f / CONSOLE_CPS;
    float base = 1.0f / TYPE_CPS;
    uint64_t h = ((uint64_t)(uint32_t)i * 2654435761ull) ^ 0x9E3779B9ull;
    float unit = (float)((h >> 13) & 0xFFFF) / 65535.0f;
    float jitter = 1.0f + TYPE_JITTER * (unit * 2.0f - 1.0f);
    float hold;
    switch (prev) {
    case '.': case '!': case '?': hold = 9.0f; break;
    case '\n': hold = 5.0f; break;
    case ',': case ';': case ':': case 0x2014: hold = 4.0f; break;
    default: hold = 0.0f; break;
    }
    return base * (jitter + hold);
}

static void advance_console(App *a, float dt) {
    int len = utf8_len(a->console_typing);
    if (a->console_revealed > len) a->console_revealed = len;
    a->console_credit += dt;
    while (a->console_revealed < len) {
        float cost = char_dwell(a->console_revealed, 0, true);
        if (a->console_credit < cost) break;
        a->console_credit -= cost;
        a->console_revealed++;
    }
}

static void console_visible(const App *a, char *out, size_t cap) {
    int nb = utf8_prefix_bytes(a->console_typing, a->console_revealed);
    if (nb >= (int)cap) nb = (int)cap - 1;
    memcpy(out, a->console_typing, (size_t)nb);
    out[nb] = '\0';
}

/* ---------- footer ---------- */

void draw_footer(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    advance_console(a, ui->dt);
    if (a->console_revealed < utf8_len(a->console_typing))
        ui->repaint_soon = true;
    draw_rect_filled(c, r, INK_BLACK);
    bool open = a->console_open;
    char visible[LOG_LINE_LEN];
    console_visible(a, visible, sizeof visible);
    FontId small = ui_font(11.0f), body = ui_font(12.0f);
    float cy = roundf(0.5f * (r.y0 + r.y1));

    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d", a->log_len);
    float chw = roundf(text_width(body, cnt, 0.0f) + 2.0f * GAP);
    float chh = roundf(text_row_height(body) + 2.0f * TIGHT);
    Rct chip = rct_xywh(roundf(r.x1 - GROUP - chw), roundf(cy - 0.5f * chh),
                        chw, chh);
    draw_rect_stroke(c, chip, 1.0f, PAPER);
    text_draw(c, body, rct_center(chip), ALIGN_CENTER_CENTER, cnt, PAPER,
              0.0f);

    const char *tab_label = "console";
    float bw = roundf(text_width(body, tab_label, 0.0f) + 2.0f * GAP);
    float bh = 21.0f;
    Rct tab = rct_xywh(r.x0 + GROUP, roundf(cy - 0.5f * bh), bw, bh);
    Resp tr = ui_interact(ui, ui_id("console tab"), tab, 0.0f);
    draw_rect_filled(c, tab, open ? PAPER : INK_BLACK);
    draw_rect_stroke(c, tab, tr.hovered || tr.pressed_on ? 2.0f : 1.0f, PAPER);
    text_draw(c, body, rct_center(tab), ALIGN_CENTER_CENTER, tab_label,
              open ? INK_BLACK : PAPER, 0.0f);
    if (tr.hovered) ui->cursor = CURSOR_POINTER;

    float x = tab.x1 + GROUP;
    float right_limit = chip.x0 - GROUP;
    UiId fid = ui_id("console input");

    if (!open) {
        if (visible[0] && right_limit > x) {
            Rct saved = canvas_clip(c);
            canvas_set_clip(c, rct_intersect(saved,
                                             rct(x, r.y0, right_limit, r.y1)));
            text_draw(c, small, (P2){x, cy}, ALIGN_LEFT_CENTER, visible, PAPER,
                      0.0f);
            canvas_set_clip(c, saved);
        }
        a->console_focused = false;
        if (ui->focus == fid) ui->focus = 0;
    } else {
        text_draw(c, small, (P2){x, cy}, ALIGN_LEFT_CENTER, ">", PAPER, 0.0f);
        x += roundf(text_width(small, ">", 0.0f)) + GROUP;
        Rct field = rct(x, roundf(cy - 10.5f), right_limit,
                        roundf(cy - 10.5f) + 21.0f);
        Resp fr = ui_interact(ui, fid, field, 0.0f);
        if (fr.hovered) ui->cursor = CURSOR_TEXT;
        if (fr.clicked) ui->focus = fid;
        if (a->console_focus) {
            ui->focus = fid;
            a->console_focus = false;
        }
        ui_text_edit(ui, fid, &a->console_input);
        bool focused = ui->focus == fid;
        float row = text_row_height(small);
        float ty = roundf(cy - 0.5f * row);
        Rct saved = canvas_clip(c);
        canvas_set_clip(c, rct_intersect(saved, field));
        float tw = text_width(small, a->console_input.text, 0.0f);
        float tx = tw <= rct_w(field) ? field.x0 : field.x1 - tw;
        if (a->console_input.len == 0)
            text_draw(c, small, (P2){field.x0, cy}, ALIGN_LEFT_CENTER,
                      "command", PAPER, 0.0f);
        else
            text_draw(c, small, (P2){tx, ty}, ALIGN_LEFT_TOP,
                      a->console_input.text, PAPER, 0.0f);
        if (focused)
            draw_block_caret(c, ui, small, (P2){tx, ty},
                             a->console_input.text, INK_BLACK);
        canvas_set_clip(c, saved);
        a->console_focused = focused;
    }

    if (tr.clicked) {
        a->console_open = !a->console_open;
        a->console_focus = a->console_open;
        if (!a->console_open) {
            a->console_input.len = 0;
            a->console_input.text[0] = '\0';
            a->preset_armed = 0;
            if (ui->focus == fid) ui->focus = 0;
        }
    }
}

/* ---------- drawer ---------- */

static void draw_hint(App *a, Ui *ui, FontId small, Rct row) {
    Canvas *c = ui->canvas;
    Hint h;
    hint_for(a, a->console_input.text, true, &h);
    if (h.kind == H_ROSTER || h.kind == H_COMPLETING) {
        const Spec *rows[NROSTER];
        int n;
        if (h.kind == H_COMPLETING) {
            n = h.m_len;
            for (int i = 0; i < n; i++) rows[i] = h.m[i];
        } else {
            n = NROSTER;
            for (int i = 0; i < n; i++) rows[i] = &ROSTER[i];
        }
        float x = row.x0;
        for (int i = 0; i < n; i++) {
            float w = hint_chip(ui, (P2){x, row.y0}, rows[i]->name,
                                i == 0 && n > 1);
            x += w + GAP;
        }
        return;
    }
    if (h.kind == H_PREVIEW) {
        char line[1536];
        snprintf(line, sizeof line, "%s — %s%s", h.line, h.note,
                 h.arms ? " · enter twice" : " · enter");
        text_draw(c, small, (P2){row.x0, row.y0}, ALIGN_LEFT_TOP, line, PAPER,
                  0.0f);
        return;
    }
    text_draw(c, small, (P2){row.x0, row.y0}, ALIGN_LEFT_TOP, h.why, PAPER,
              0.0f);
}

void draw_console_drawer(App *a, Ui *ui, Rct footer) {
    Canvas *c = ui->canvas;
    float h = fminf(FOOTER_OPEN_H, floorf((float)c->h * 0.5f));
    Rct rect = rct(footer.x0, footer.y0 - h, footer.x1, footer.y0);
    draw_rect_filled(c, rect, INK_BLACK);
    draw_rect_filled(c, rct(rect.x0, rect.y0, rect.x1, rect.y0 + 1.0f), PAPER);
    draw_rect_filled(c, rct(rect.x0, rect.y0, rect.x0 + 1.0f, rect.y1), PAPER);
    draw_rect_filled(c, rct(rect.x1 - 1.0f, rect.y0, rect.x1, rect.y1), PAPER);

    FontId small = ui_font(11.0f);
    char visible[LOG_LINE_LEN];
    console_visible(a, visible, sizeof visible);
    Rct inner = rct_shrink(rect, GAP);
    float body_h = fmaxf(rct_h(rect) - 2.0f * GAP - HINT_ROW_H - GAP, 0.0f);
    Rct view = rct(inner.x0, inner.y0, inner.x1, inner.y0 + body_h);

    float row = text_row_height(small);
    int full = a->log_len > 0 ? a->log_len - 1 : 0;
    int items = full + (visible[0] ? 1 : 0);
    float content_h =
        items > 0 ? (float)items * row + (float)(items - 1) * GROUP : 0.0f;

    /* stick_to_bottom: re-pin whenever the view was at the end last frame */
    static float prev_max = -1.0f;
    float max_off = fmaxf(content_h - rct_h(view), 0.0f);
    if (prev_max < 0.0f || a->log_scroll.offset >= prev_max - 0.5f)
        a->log_scroll.offset = max_off;
    prev_max = max_off;
    float off = ui_scroll(ui, &a->log_scroll, view, content_h);

    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(saved, view));
    float y = view.y0 - off;
    for (int j = 0; j < full; j++) {
        int idx = (a->log_head - (a->log_len - 1) + j + 2 * LOG_LINES)
                  % LOG_LINES;
        text_draw(c, small, (P2){view.x0, roundf(y)}, ALIGN_LEFT_TOP,
                  a->log[idx], PAPER, 0.0f);
        y += row + GROUP;
    }
    if (visible[0])
        text_draw(c, small, (P2){view.x0, roundf(y)}, ALIGN_LEFT_TOP, visible,
                  PAPER, 0.0f);
    canvas_set_clip(c, saved);

    float hy = inner.y0 + body_h + GAP;
    draw_hint(a, ui, small, rct(inner.x0, hy, inner.x1, hy + HINT_ROW_H));
}
