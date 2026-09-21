#include "command.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "focus.h"

#define PATHBUF 1024

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

static bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* the one-line reason; the caller appends the usage line */
static bool reason(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

static bool refs_equal(const PresetRef *a, const PresetRef *b) {
    return strcmp(a->bank, b->bank) == 0 && strcmp(a->name, b->name) == 0;
}

static PresetRef ref_make(const char *bank, const char *name) {
    PresetRef r;
    snprintf(r.bank, sizeof r.bank, "%.63s", bank ? bank : "");
    snprintf(r.name, sizeof r.name, "%.127s", name);
    return r;
}

static bool is_view_name(const char *folder) {
    return strcasecmp(folder, "ALL") == 0 || strcasecmp(folder, MINE_BANK) == 0;
}

/* ---------- the verb table ---------- */

static bool run_rec(App *a, const Command *c, char *err, size_t n);
static bool run_ls(App *a, const Command *c, char *err, size_t n);
static bool run_cd(App *a, const Command *c, char *err, size_t n);
static bool run_load(App *a, const Command *c, char *err, size_t n);
static bool run_save(App *a, const Command *c, char *err, size_t n);
static bool run_overwrite(App *a, const Command *c, char *err, size_t n);
static bool run_delete(App *a, const Command *c, char *err, size_t n);
static bool run_rename(App *a, const Command *c, char *err, size_t n);
static bool run_add(App *a, const Command *c, char *err, size_t n);
static bool run_remove(App *a, const Command *c, char *err, size_t n);
static bool run_rmdir(App *a, const Command *c, char *err, size_t n);
static bool folder_ok(const char *folder, char *err, size_t n);
static bool run_undo(App *a, const Command *c, char *err, size_t n);
static bool run_bind(App *a, const Command *c, char *err, size_t n);
static bool run_unbind(App *a, const Command *c, char *err, size_t n);
static bool run_help(App *a, const Command *c, char *err, size_t n);
static bool parse_switch(Command *c, char *err, size_t n);
static bool run_switch(App *a, const Command *c, char *err, size_t n);
static bool parse_op(Command *c, char *err, size_t n);
static bool run_op(App *a, const Command *c, char *err, size_t n);
static int switch_complete(char *const words[], int nwords, const char *prefix,
                           char out[][CAND_LEN], int max);
static int op_complete(char *const words[], int nwords, const char *prefix,
                       char out[][CAND_LEN], int max);
static bool run_pins(App *a, const Command *c, char *err, size_t n);
static bool parse_pins(Command *c, char *err, size_t n);
static int pins_complete(char *const words[], int nwords, const char *prefix,
                         char out[][CAND_LEN], int max);
static int add_cand(char out[][CAND_LEN], int n, int max, const char *prefix,
                    const char *word);

static const Flag REC_FLAGS[] = {
    {"end", "seconds", "finish the take on its own after this long"},
};

static const Flag RM_FLAGS[] = {
    {"r", NULL, "move a folder and everything in it to the trash"},
};

static const Verb VERBS[] = {
    {"ls", {NULL}, G_PRESETS, {"path or mod", NULL}, 1, 0, true, NULL, 0,
     "list presets in a folder, the browser view, or modulation routes", run_ls,
     NULL, NULL, NULL, NULL, NULL, NULL},
    {"cd", {NULL}, G_PRESETS, {"folder", NULL}, 1, 0, true, NULL, 0,
     "set the preset browser view; no argument prints it", run_cd, NULL, NULL,
     NULL, NULL, NULL, NULL},
    {"load", {NULL}, G_PRESETS, {"path", NULL}, 1, 1, true, NULL, 0,
     "load a preset by path or unambiguous name", run_load, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"save", {NULL}, G_PRESETS, {"name", NULL}, 1, 1, false, NULL, 0,
     "write the current sound under a name", run_save, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"ow", {"overwrite", NULL}, G_PRESETS, {"path", NULL}, 1, 1, true, NULL, 0,
     "replace a preset with the current sound", run_overwrite, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"rm", {"delete", NULL}, G_PRESETS, {"path", NULL}, 1, 0, true,
     RM_FLAGS, 1, "move a preset to the trash; -r moves a folder", run_delete, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"mv", {"rename", "move", NULL}, G_PRESETS, {"path", "destination"}, 2, 2, true, NULL, 0,
     "give a preset a different name, where it sits", run_rename, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"mkdir", {"add", NULL}, G_PRESETS, {"folder", NULL}, 1, 1, true, NULL, 0,
     "make an empty folder", run_add, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"rmdir", {NULL}, G_PRESETS, {"folder", NULL}, 1, 1, true, NULL, 0,
     "remove an empty folder", run_rmdir, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"undo", {NULL}, G_PRESETS, {NULL, NULL}, 0, 0, false, NULL, 0,
     "put back the last trashed, renamed, moved or overwritten item", run_undo, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {.name = "drone",
     .group = G_SOUND,
     .about = "show or set whether the drone is sounding",
     .run = run_switch,
     .parse = parse_switch,
     .complete = switch_complete,
     .form = "[on|off]"},
    {.name = "mel",
     .group = G_SOUND,
     .about = "show or set whether the melody sequencer is running",
     .run = run_switch,
     .parse = parse_switch,
     .complete = switch_complete,
     .form = "[on|off]"},
    {.name = "op",
     .group = G_SOUND,
     .about = "show an operator, or set its power",
     .run = run_op,
     .parse = parse_op,
     .complete = op_complete,
     .form = "<1-5> [on|off]",
     .extra = "with no state word, prints power, ratio, detune in cents and level"},
    {"bind", {NULL}, G_MIDI, {"cc", "control"}, 2, 2, true, NULL, 0,
     "point a controller at a control", run_bind, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"unbind", {NULL}, G_MIDI, {"cc", NULL}, 1, 1, true, NULL, 0,
     "let a controller go, or 'all' of them", run_unbind, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"rec", {"record", NULL}, G_RECORDING, {"name", NULL}, 1, 0, false,
     REC_FLAGS, 1, "start a take, or finish the one running", run_rec, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {"help", {NULL}, G_CONSOLE, {"verb or group", NULL}, 1, 0, false, NULL, 0,
     "list the verbs, or explain one", run_help, NULL, NULL, NULL,
     NULL, NULL, NULL},
    {.name = "lfo",
     .group = G_MODULATION,
     .about = "make, shape and point an lfo; lfo alone lists them",
     .run = mod_run_lfo,
     .parse = mod_parse_lfo,
     .view = mod_view_lfo,
     .preview = mod_preview_lfo,
     .complete = mod_complete_lfo,
     .form = "[1-16] [shape] [rate <hz|1/4>] [phase <deg>] [free|retrig|once] "
             "[bi|uni] [to <target> <depth|off>]... [rm] [-v]",
     .extra = "shapes  sine tri saw ramp square exp sh drift\n"
              "targets index rip fb field curve level pitch mix ghost decay "
              "damp haunt warmth mel rate\n"
              "        chandas mix|rate|spread|size|warp|dim|tail\n"
              "depth   -1 to 1 of the target's range; pitch is +-12 semitones\n"
              "-v      pin it above the log, live; -v again lets it go"},
    {.name = "pins",
     .group = G_CONSOLE,
     .about = "the views pinned above the log",
     .run = run_pins,
     .parse = parse_pins,
     .complete = pins_complete,
     .form = "[rm <n>|fold <n>|open <n>|clear]",
     .extra = "a click on a pinned view lets it go, as does -v on its line"},
    {.name = "mods",
     .group = G_MODULATION,
     .about = "the routing table: every lfo and where it points",
     .run = mod_run_mods,
     .parse = mod_parse_mods,
     .view = mod_view_mods,
     .form = "[-v]",
     .extra = "-v      pin the table above the log, live; -v again lets it go"},
};
#define NVERBS ((int)(sizeof VERBS / sizeof VERBS[0]))

static const char *const GROUP_TITLES[G_COUNT] = {
    "presets", "sound", "modulation", "midi", "recording", "console"};

int verb_count(void) { return NVERBS; }
const Verb *verb_at(int i) { return i >= 0 && i < NVERBS ? &VERBS[i] : NULL; }
const char *verb_group_title(VerbGroup g) {
    return g >= 0 && g < G_COUNT ? GROUP_TITLES[g] : "";
}

const Verb *verb_lookup(const char *word) {
    for (int i = 0; i < NVERBS; i++) {
        const Verb *v = &VERBS[i];
        if (strcasecmp(v->name, word) == 0) return v;
        for (int k = 0; v->aliases[k]; k++)
            if (strcasecmp(v->aliases[k], word) == 0) return v;
    }
    return NULL;
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    int prev[64], cur[64];
    if (lb >= 64) lb = 63;
    if (la >= 64) la = 63;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 0; i < la; i++) {
        cur[0] = i + 1;
        for (int j = 0; j < lb; j++) {
            int cost = a[i] != b[j];
            int best = prev[j + 1] + 1;
            if (cur[j] + 1 < best) best = cur[j] + 1;
            if (prev[j] + cost < best) best = prev[j] + cost;
            cur[j + 1] = best;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

const Verb *verb_nearest(const char *word) {
    char low[64];
    lower_into(word, low, sizeof low);
    int limit = (int)strlen(low) / 2;
    if (limit < 1) limit = 1;
    if (limit > 3) limit = 3;
    const Verb *best = NULL;
    int best_d = limit + 1;
    for (int i = 0; i < NVERBS; i++) {
        const Verb *v = &VERBS[i];
        int d = edit_distance(low, v->name);
        for (int k = 0; v->aliases[k]; k++) {
            int e = edit_distance(low, v->aliases[k]);
            if (e < d) d = e;
        }
        if (d < best_d) {
            best_d = d;
            best = v;
        }
    }
    return best;
}

int verb_complete(const char *prefix, const Verb **out, int max) {
    char low[64];
    lower_into(prefix, low, sizeof low);
    size_t ln = strlen(low);
    int n = 0;
    for (int i = 0; i < NVERBS && n < max; i++) {
        const Verb *v = &VERBS[i];
        bool hit = strncmp(v->name, low, ln) == 0;
        for (int k = 0; !hit && v->aliases[k]; k++)
            hit = strncmp(v->aliases[k], low, ln) == 0;
        if (hit) out[n++] = v;
    }
    return n;
}

static void verb_form(const Verb *v, char *out, size_t cap) {
    snprintf(out, cap, "%s", v->name);
    if (v->form) {
        scat(out, cap, " %s", v->form);
        return;
    }
    for (int i = 0; i < v->nargs; i++) {
        const char *sep = " ";
        if (i < v->required)
            scat(out, cap, "%s<%s>", sep, v->args[i]);
        else
            scat(out, cap, "%s[%s]", sep, v->args[i]);
    }
    for (int i = 0; i < v->nflags; i++) {
        if (v->flags[i].value)
            scat(out, cap, " [--%s <%s>]", v->flags[i].name, v->flags[i].value);
        else
            scat(out, cap, " [--%s]", v->flags[i].name);
    }
}

void verb_usage(const Verb *v, char *out, size_t cap) {
    char form[192];
    verb_form(v, form, sizeof form);
    snprintf(out, cap, "usage: %s", form);
}

void verb_help(const Verb *v, char *out, size_t cap) {
    verb_usage(v, out, cap);
    scat(out, cap, "\n  %s", v->about);
    for (int i = 0; i < v->nflags; i++) {
        char head[64];
        if (v->flags[i].value)
            snprintf(head, sizeof head, "--%s <%s>", v->flags[i].name,
                     v->flags[i].value);
        else
            snprintf(head, sizeof head, "--%s", v->flags[i].name);
        scat(out, cap, "\n  %-20s %s", head, v->flags[i].about);
    }
    if (v->extra) {
        const char *p = v->extra;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            scat(out, cap, "\n  %.*s", (int)n, p);
            if (!nl) break;
            p = nl + 1;
        }
    }
    if (v->aliases[0]) {
        scat(out, cap, "\n  also:");
        for (int k = 0; v->aliases[k]; k++) scat(out, cap, " %s", v->aliases[k]);
    }
}

void help_text(char *out, size_t cap) {
    out[0] = '\0';
    for (int g = 0; g < G_COUNT; g++) {
        scat(out, cap, "%-11s", GROUP_TITLES[g]);
        for (int i = 0; i < NVERBS; i++)
            if (VERBS[i].group == (VerbGroup)g) scat(out, cap, " %s", VERBS[i].name);
        scat(out, cap, "\n");
    }
    scat(out, cap,
         "help <verb> for its usage, help <group> for the group in full; -h "
         "after any verb does the same");
}

bool help_group_text(const char *name, char *out, size_t cap) {
    int g = -1;
    for (int i = 0; i < G_COUNT; i++)
        if (strcasecmp(GROUP_TITLES[i], name) == 0) g = i;
    if (g < 0) return false;
    char heads[NVERBS][256];
    size_t width = 0;
    for (int i = 0; i < NVERBS; i++) {
        heads[i][0] = '\0';
        if (VERBS[i].group != (VerbGroup)g) continue;
        verb_form(&VERBS[i], heads[i], sizeof heads[i]);
        if (strlen(heads[i]) > width) width = strlen(heads[i]);
    }
    snprintf(out, cap, "%s", GROUP_TITLES[g]);
    for (int i = 0; i < NVERBS; i++)
        if (heads[i][0])
            scat(out, cap, "\n  %-*s  %s", (int)width, heads[i], VERBS[i].about);
    return true;
}

/* ---------- parsing ---------- */

static int split_words(const char *line, char *buf, size_t cap, char *words[],
                       int max) {
    snprintf(buf, cap, "%s", line);
    int n = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, " \t\r\n", &save); w && n < max;
         w = strtok_r(NULL, " \t\r\n", &save))
        words[n++] = w;
    return n;
}

/* a word is a flag when it starts with a dash and is not a negative number
   or the bare " - " separator */
static bool is_flag(const char *w) {
    return w[0] == '-' && w[1] && !isdigit((unsigned char)w[1]) && w[1] != '.';
}

static bool is_help_word(const char *w) {
    return strcmp(w, "-h") == 0 || strcmp(w, "--help") == 0 || strcmp(w, "?") == 0;
}

static int parse_cc_word(const char *w) {
    const char *d = w;
    if (strncasecmp(w, "cc", 2) == 0) d = w + 2;
    if (!*d || strlen(d) > 3) return -1;
    for (const char *p = d; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    long n = strtol(d, NULL, 10);
    return n < 128 ? (int)n : -1;
}

static void bindable_list(char *out, size_t cap) {
    out[0] = '\0';
    for (int t = CC_INDEX; t <= CC_LAST; t++)
        scat(out, cap, "%s%s", t == CC_INDEX ? "" : " ",
             cc_target_name((CcTarget)t));
}

static bool clean_name(const char *raw, char *out, size_t cap, char *err,
                       size_t err_len) {
    if (strcasecmp(raw, SELECTED_WORD) == 0) {
        snprintf(out, cap, "%s", SELECTED_WORD);
        return true;
    }
    const char *slash = strchr(raw, '/');
    if (!slash) {
        sanitise_segment(raw, out, cap);
    } else {
        if (slash == raw || !slash[1] || strchr(slash + 1, '/')) out[0] = '\0';
        else {
            char left[128], clean_left[128], right[192];
            snprintf(left, sizeof left, "%.*s", (int)(slash - raw), raw);
            sanitise_segment(left, clean_left, sizeof clean_left);
            sanitise_segment(slash + 1, right, sizeof right);
            size_t nl = strlen(clean_left), nr = strlen(right);
            if (clean_left[0] && right[0] && nl + nr + 2 <= cap) {
                memcpy(out, clean_left, nl);
                out[nl] = '/';
                memcpy(out + nl + 1, right, nr + 1);
            } else {
                out[0] = '\0';
            }
        }
    }
    if (!out[0])
        return reason(err, err_len,
                      "'%s' is not a name or one-level preset path", raw);
    return true;
}

static bool parse_help(char *const words[], int n, Command *out, char *err,
                       size_t err_len) {
    out->kind = CMD_HELP;
    if (n == 1) {
        help_text(out->text, sizeof out->text);
        return true;
    }
    if (n > 2) return reason(err, err_len, "help wants one word");
    const Verb *v = verb_lookup(words[1]);
    if (v) {
        verb_help(v, out->text, sizeof out->text);
        return true;
    }
    if (help_group_text(words[1], out->text, sizeof out->text)) return true;
    return reason(err, err_len, "no verb or group called %s", words[1]);
}

static bool parse_words(char *const words[], int n, Command *out, char *err,
                        size_t err_len) {
    const Verb *v = verb_lookup(words[0]);
    if (!v) {
        const Verb *near = verb_nearest(words[0]);
        if (near)
            return reason(err, err_len, "no verb called %s; did you mean %s?",
                          words[0], near->name);
        return reason(err, err_len, "no verb called %s", words[0]);
    }
    out->verb = v;
    for (int i = 1; i < n; i++)
        if (is_help_word(words[i])) {
            out->kind = CMD_HELP;
            verb_help(v, out->text, sizeof out->text);
            return true;
        }
    if (v->run == run_help) return parse_help(words, n, out, err, err_len);

    if (v->parse) {
        for (int i = 1; i < n; i++) {
            const char *w = words[i];
            if (strcmp(w, "-v") == 0 || strcmp(w, "--view") == 0) {
                out->view = true;
                continue;
            }
            if (is_flag(w))
                return reason(err, err_len, "%s has no flag %s", v->name, w);
            if (out->nwords >= CMD_WORDS)
                return reason(err, err_len, "%s: too many words", v->name);
            snprintf(out->words[out->nwords++], sizeof out->words[0], "%s", w);
        }
        if (!v->parse(out, err, err_len)) return false;
        out->kind = CMD_RUN;
        return true;
    }

    char *pos[64];
    int npos = 0;
    for (int i = 1; i < n; i++) {
        const char *w = words[i];
        if (!is_flag(w)) {
            if (npos < 64) pos[npos++] = words[i];
            continue;
        }
        const char *key = w + (w[1] == '-' ? 2 : 1);
        const Flag *f = NULL;
        for (int k = 0; k < v->nflags; k++)
            if (strcasecmp(v->flags[k].name, key) == 0) f = &v->flags[k];
        if (!f) return reason(err, err_len, "%s has no flag %s", v->name, w);
        const char *val = NULL;
        if (f->value) {
            if (i + 1 >= n)
                return reason(err, err_len, "--%s wants a %s", f->name, f->value);
            val = words[++i];
        }
        if (strcmp(f->name, "end") == 0) {
            char *endp = NULL;
            float s = strtof(val, &endp);
            if (!endp || *endp || !(s > 0.0f))
                return reason(err, err_len,
                              "--end wants a number of seconds, not '%s'", val);
            out->has_end = true;
            out->end = s;
        } else if (strcmp(f->name, "r") == 0) {
            out->recursive = true;
        }
    }

    char given[2][192];
    int ngiven = npos;
    if (npos > 0 && v->nargs == 0)
        return reason(err, err_len, "%s takes nothing", v->name);
    if (npos > v->nargs)
        return reason(err, err_len, "%s takes %d argument%s", v->name,
                      v->nargs, v->nargs == 1 ? "" : "s");
    for (int i = 0; i < ngiven; i++)
        snprintf(given[i], sizeof given[i], "%s", pos[i]);
    if (ngiven < v->required) {
        if (v->required >= 2)
            return reason(err, err_len, "%s wants <%s>%s<%s>", v->name,
                          v->args[0], " ", v->args[1]);
        return reason(err, err_len, "%s wants a %s", v->name, v->args[0]);
    }
    for (int i = 0; i < ngiven; i++)
        if (!clean_name(given[i], out->arg[i], sizeof out->arg[i], err, err_len))
            return false;
    out->argc = ngiven;

    if (v->run == run_bind) {
        int cc = parse_cc_word(out->arg[0]);
        if (cc < 0)
            return reason(err, err_len,
                          "'%s' is not a controller: cc wants 0 to 127",
                          out->arg[0]);
        char low[192];
        lower_into(out->arg[1], low, sizeof low);
        CcTarget t = cc_target_from_name(low);
        if (t == CC_NONE) {
            char list[256];
            bindable_list(list, sizeof list);
            return reason(err, err_len, "'%s' cannot be bound; the controls: %s",
                          out->arg[1], list);
        }
        out->cc = cc;
        out->target = t;
    } else if (v->run == run_unbind) {
        if (strcasecmp(out->arg[0], "all") == 0) {
            out->cc_all = true;
        } else {
            int cc = parse_cc_word(out->arg[0]);
            if (cc < 0)
                return reason(err, err_len,
                              "'%s' is not a controller: cc wants 0 to 127, "
                              "or all",
                              out->arg[0]);
            out->cc = cc;
        }
    }
    out->kind = CMD_RUN;
    return true;
}

/* second line of every error: the verb's usage, or where to find it */
static void add_usage(const Verb *v, char *err, size_t err_len) {
    if (v) {
        char usage[256];
        verb_usage(v, usage, sizeof usage);
        scat(err, err_len, "\n%s", usage);
    } else {
        scat(err, err_len, "\nhelp lists every verb");
    }
}

bool parse_line(const char *line, Command *out, char *err, size_t err_len) {
    memset(out, 0, sizeof *out);
    if (err_len) err[0] = '\0';
    char buf[LOG_LINE_LEN * 2];
    char *words[128];
    int n = split_words(line ? line : "", buf, sizeof buf, words, 128);
    if (n == 0) {
        out->kind = CMD_NOP;
        return true;
    }
    if (strcmp(words[0], "?") == 0) words[0] = (char *)"help";
    if (parse_words(words, n, out, err, err_len)) return true;
    add_usage(out->verb, err, err_len);
    out->kind = CMD_NOP;
    return false;
}

void command_echo(const Command *c, char *out, size_t cap) {
    out[0] = '\0';
    if (c->kind != CMD_RUN || !c->verb) return;
    const Verb *v = c->verb;
    snprintf(out, cap, "%s", v->name);
    if (v->parse) {
        for (int i = 0; i < c->nwords; i++) scat(out, cap, " %s", c->words[i]);
        if (c->view) scat(out, cap, " -v");
        return;
    }
    if (v->run == run_bind) {
        scat(out, cap, " cc%d %s", c->cc, cc_target_name(c->target));
        return;
    }
    if (v->run == run_unbind) {
        if (c->cc_all)
            scat(out, cap, " all");
        else
            scat(out, cap, " cc%d", c->cc);
        return;
    }
    if (c->argc > 0) scat(out, cap, " %s", c->arg[0]);
    if (c->argc > 1) scat(out, cap, " %s", c->arg[1]);
    if (c->has_end) scat(out, cap, " --end %g", (double)c->end);
}

bool command_run(App *a, const Command *c, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    if (c->kind != CMD_RUN || !c->verb) return true;
    if (!c->verb->run(a, c, err, err_len)) {
        add_usage(c->verb, err, err_len);
        return false;
    }
    if (c->view && c->verb->view) {
        /* a pin keeps the verb and the number it names, so "lfo 3 rate 2 -v"
           pins "lfo 3" and later edits to lfo 3 show in it */
        char pin[LOG_LINE_LEN];
        snprintf(pin, sizeof pin, "%s", c->verb->name);
        if (c->nwords > 0 && isdigit((unsigned char)c->words[0][0]))
            scat(pin, sizeof pin, " %s", c->words[0]);
        if (console_toggle_pin(a, pin, err, err_len))
            push_log(a, "pinned. -v again lets it go");
        else if (err[0])
            return false;
        else
            push_log(a, "let go");
    }
    return true;
}

void console_unpin_at(App *a, int i) {
    if (i < 0 || i >= a->pin_count) return;
    int rest = a->pin_count - i - 1;
    memmove(a->pins[i], a->pins[i + 1], (size_t)rest * sizeof a->pins[0]);
    memmove(&a->pin_folded[i], &a->pin_folded[i + 1],
            (size_t)rest * sizeof a->pin_folded[0]);
    a->pin_count--;
}

bool console_toggle_pin(App *a, const char *pin, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    for (int i = 0; i < a->pin_count; i++) {
        if (strcmp(a->pins[i], pin) != 0) continue;
        console_unpin_at(a, i);
        return false;
    }
    if (a->pin_count >= PIN_MAX) {
        reason(err, err_len,
               "%d views are pinned already; run one again with -v to let it go",
               PIN_MAX);
        return false;
    }
    a->pin_folded[a->pin_count] = false;
    snprintf(a->pins[a->pin_count++], sizeof a->pins[0], "%s", pin);
    return true;
}

/* ---------- pins ---------- */

static bool parse_pins(Command *c, char *err, size_t n) {
    if (c->nwords == 0) return true;
    const char *w = c->words[0];
    bool one_of = strcasecmp(w, "rm") == 0 || strcasecmp(w, "fold") == 0
                  || strcasecmp(w, "open") == 0;
    if (strcasecmp(w, "clear") == 0) {
        if (c->nwords > 1)
            return reason(err, n, "pins clear takes nothing, not '%s'",
                          c->words[1]);
        return true;
    }
    if (!one_of)
        return reason(err, n, "pins takes rm, fold, open or clear, not '%s'", w);
    if (c->nwords < 2)
        return reason(err, n, "pins %s wants which pin, 1 to %d", w, PIN_MAX);
    char *end = NULL;
    long at = strtol(c->words[1], &end, 10);
    if (!end || *end || at < 1 || at > PIN_MAX)
        return reason(err, n, "pins %s wants a pin, 1 to %d, not '%s'", w,
                      PIN_MAX, c->words[1]);
    return true;
}

static int pins_complete(char *const words[], int nwords, const char *prefix,
                         char out[][CAND_LEN], int max) {
    int n = 0;
    if (nwords == 0) {
        static const char *const KEYS[] = {"rm", "fold", "open", "clear"};
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
            n = add_cand(out, n, max, prefix, KEYS[i]);
        return n;
    }
    if (strcasecmp(words[nwords - 1], "clear") != 0)
        for (int i = 1; i <= PIN_MAX; i++) {
            char num[8];
            snprintf(num, sizeof num, "%d", i);
            n = add_cand(out, n, max, prefix, num);
        }
    return n;
}

static bool run_pins(App *a, const Command *c, char *err, size_t n) {
    if (c->nwords == 0) {
        if (a->pin_count == 0) {
            push_log(a, "nothing is pinned. any line with -v pins its view");
            return true;
        }
        for (int i = 0; i < a->pin_count; i++)
            push_log(a, "%d  %s%s", i + 1, a->pins[i],
                     a->pin_folded[i] ? "  (folded)" : "");
        return true;
    }
    if (strcasecmp(c->words[0], "clear") == 0) {
        int was = a->pin_count;
        a->pin_count = 0;
        push_log(a, "%d pin%s let go", was, was == 1 ? "" : "s");
        return true;
    }
    int at = (int)strtol(c->words[1], NULL, 10) - 1;
    if (at >= a->pin_count)
        return reason(err, n, "there is no pin %d; pins lists them", at + 1);
    if (strcasecmp(c->words[0], "rm") == 0) {
        char was[LOG_LINE_LEN];
        snprintf(was, sizeof was, "%s", a->pins[at]);
        console_unpin_at(a, at);
        push_log(a, "%s let go", was);
        return true;
    }
    a->pin_folded[at] = strcasecmp(c->words[0], "fold") == 0;
    push_log(a, "%s %s", a->pins[at],
             a->pin_folded[at] ? "folded" : "open");
    return true;
}

/* ---------- the line being typed ---------- */

static bool starts_with_ci(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0;
}

static int add_cand(char out[][CAND_LEN], int n, int max, const char *prefix,
                    const char *word) {
    if (n >= max || !starts_with_ci(word, prefix)) return n;
    for (int i = 0; i < n; i++)
        if (strcasecmp(out[i], word) == 0) return n;
    snprintf(out[n], CAND_LEN, "%s", word);
    return n + 1;
}

static bool switch_word(const char *word, bool *out) {
    if (strcasecmp(word, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(word, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_switch(Command *c, char *err, size_t n) {
    if (c->nwords == 0) return true;
    bool on;
    if (c->nwords > 1)
        return reason(err, n, "%s takes one state word", c->verb->name);
    if (!switch_word(c->words[0], &on))
        return reason(err, n, "%s wants on or off, not '%s'", c->verb->name,
                      c->words[0]);
    return true;
}

static bool run_switch(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    bool *state;
    Event ev;
    if (strcmp(c->verb->name, "drone") == 0) {
        state = &a->engaged;
        ev.kind = EV_ENGAGE;
    } else {
        state = &a->shadow_melody.enabled;
        ev.kind = EV_SET_MELODY;
    }
    if (c->nwords == 0) {
        if (strcmp(c->verb->name, "drone") == 0)
            push_log(a, "drone %s at %.1f hz", *state ? "on" : "off",
                     (double)a->drone_hz);
        else
            push_log(a, "mel %s at %.3g hz", *state ? "on" : "off",
                     (double)a->shadow_melody.rate_hz);
        return true;
    }
    bool on;
    switch_word(c->words[0], &on);
    *state = on;
    if (strcmp(c->verb->name, "drone") == 0) {
        ev.u.flag = on;
    } else {
        ev.u.melody = a->shadow_melody;
    }
    app_send(a, ev);
    push_log(a, "%s %s", c->verb->name, on ? "on" : "off");
    return true;
}

static bool parse_op(Command *c, char *err, size_t n) {
    if (c->nwords == 0)
        return reason(err, n, "op wants its number first, 1 to %d", NUM_OPS);
    if (c->nwords > 2) return reason(err, n, "op takes a number and a state");
    char *end = NULL;
    long op = strtol(c->words[0], &end, 10);
    if (!end || *end || op < 1 || op > NUM_OPS)
        return reason(err, n, "op wants its number first, 1 to %d, not '%s'",
                      NUM_OPS, c->words[0]);
    if (c->nwords == 2) {
        bool on;
        if (!switch_word(c->words[1], &on))
            return reason(err, n, "op wants on or off, not '%s'", c->words[1]);
    }
    return true;
}

static bool run_op(App *a, const Command *c, char *err, size_t n) {
    (void)err;
    (void)n;
    int at = (int)strtol(c->words[0], NULL, 10) - 1;
    OpParams *op = &a->shadow.ops[at];
    if (c->nwords == 1) {
        push_log(a, "op %d %s  ratio %.3g  detune %.3g cents  level %.3g",
                 at + 1, op->enabled ? "on" : "off", (double)op->ratio,
                 (double)op->detune_cents, (double)op->level);
        return true;
    }
    bool on;
    switch_word(c->words[1], &on);
    op->enabled = on;
    app_send(a, (Event){.kind = EV_SET_PATCH, .u.patch = a->shadow});
    push_log(a, "op %d %s", at + 1, on ? "on" : "off");
    return true;
}

static int switch_complete(char *const words[], int nwords, const char *prefix,
                           char out[][CAND_LEN], int max) {
    (void)words;
    if (nwords > 0) return 0;
    int n = add_cand(out, 0, max, prefix, "on");
    return add_cand(out, n, max, prefix, "off");
}

static int op_complete(char *const words[], int nwords, const char *prefix,
                       char out[][CAND_LEN], int max) {
    int n = 0;
    if (nwords == 0) {
        for (int i = 1; i <= NUM_OPS; i++) {
            char number[4];
            snprintf(number, sizeof number, "%d", i);
            n = add_cand(out, n, max, prefix, number);
        }
        return n;
    }
    if (nwords == 1) return switch_complete(words, 0, prefix, out, max);
    return 0;
}

static int complete_paths(const App *a, const char *prefix,
                          char out[][CAND_LEN], int n, int max) {
    n = add_cand(out, n, max, prefix, SELECTED_WORD);
    for (int i = 0; i < a->preset_count; i++) {
        const PresetRef *p = &a->preset_names[i];
        if (strcmp(p->bank, TRASH_DIR) == 0) continue;
        char path[256];
        preset_qualified(p, path, sizeof path);
        n = add_cand(out, n, max, prefix, path);
    }
    return n;
}

static int complete_folders(const App *a, const char *prefix, bool views,
                            bool writable, char out[][CAND_LEN], int n,
                            int max) {
    if (views) {
        n = add_cand(out, n, max, prefix, "ALL");
        n = add_cand(out, n, max, prefix, MINE_BANK);
    } else if (!writable) {
        n = add_cand(out, n, max, prefix, MINE_BANK);
    }
    for (int i = 0; i < a->folder_count; i++) {
        const char *folder = a->preset_folders[i];
        if (strcasecmp(folder, TRASH_DIR) == 0) continue;
        if (writable && strcasecmp(folder, STOCK_BANK) == 0) continue;
        n = add_cand(out, n, max, prefix, folder);
    }
    return n;
}

static int complete_generic(const App *a, const Verb *v,
                            char *const words[], int nwords,
                            const char *prefix, char out[][CAND_LEN], int max) {
    int positional = 0;
    bool recursive = false;
    for (int i = 0; i < nwords; i++) {
        if (strcmp(words[i], "-r") == 0 || strcmp(words[i], "--r") == 0)
            recursive = true;
        else if (!is_flag(words[i]))
            positional++;
    }
    int n = 0;
    if (strcmp(v->name, "load") == 0 || strcmp(v->name, "ow") == 0
        || (strcmp(v->name, "rm") == 0 && !recursive)
        || (strcmp(v->name, "mv") == 0 && positional == 0))
        return complete_paths(a, prefix, out, n, max);
    if (strcmp(v->name, "cd") == 0)
        return complete_folders(a, prefix, true, false, out, n, max);
    if (strcmp(v->name, "ls") == 0) {
        n = add_cand(out, n, max, prefix, "mod");
        return complete_folders(a, prefix, true, false, out, n, max);
    }
    if (strcmp(v->name, "mv") == 0 && positional == 1) {
        n = add_cand(out, n, max, prefix, MINE_BANK);
        return complete_folders(a, prefix, false, true, out, n, max);
    }
    if (strcmp(v->name, "rmdir") == 0
        || (strcmp(v->name, "rm") == 0 && recursive))
        return complete_folders(a, prefix, false, true, out, n, max);
    if (strcmp(v->name, "bind") == 0 && positional == 1) {
        for (int t = CC_INDEX; t <= CC_LAST; t++)
            n = add_cand(out, n, max, prefix,
                         cc_target_name((CcTarget)t));
        return n;
    }
    if (strcmp(v->name, "unbind") == 0 && positional == 0)
        return add_cand(out, n, max, prefix, "all");
    return 0;
}

void line_state(App *a, const char *line, LineState *out) {
    memset(out, 0, sizeof *out);
    char buf[LOG_LINE_LEN * 2];
    char *words[CMD_WORDS + 2];
    int n = split_words(line ? line : "", buf, sizeof buf, words, CMD_WORDS + 2);
    /* a trailing space means the word being typed is empty */
    size_t len = line ? strlen(line) : 0;
    bool fresh_word = len == 0 || isspace((unsigned char)line[len - 1]);
    const char *prefix = fresh_word || n == 0 ? "" : words[n - 1];
    int given = fresh_word ? n : n - 1; /* words already finished */
    out->prefix_len = (int)strlen(prefix);

    if (given <= 0) {
        for (int i = 0; i < verb_count(); i++)
            out->ncand = add_cand(out->cand, out->ncand, CAND_MAX, prefix,
                                  verb_at(i)->name);
    } else {
        const Verb *v = verb_lookup(words[0]);
        if (v) {
            if (v->complete)
                out->ncand = v->complete(words + 1, given - 1, prefix,
                                         out->cand, CAND_MAX);
            else
                out->ncand = complete_generic(a, v, words + 1, given - 1,
                                              prefix, out->cand, CAND_MAX);
        }
    }

    if (n == 0) return;
    Command c;
    char err[768];
    if (!parse_line(line, &c, err, sizeof err)) {
        out->bad = true;
        const char *nl = strchr(err, '\n');
        size_t m = nl ? (size_t)(nl - err) : strlen(err);
        if (m >= sizeof out->why) m = sizeof out->why - 1;
        memcpy(out->why, err, m);
        out->why[m] = '\0';
        return;
    }
    if (c.kind == CMD_HELP) {
        view_add(&out->preview, "%s", c.verb ? "the verb's help" : "the verbs");
        out->has_preview = true;
        return;
    }
    if (c.kind != CMD_RUN || !c.verb) return;
    if (c.verb->preview && c.verb->preview(a, &c, &out->preview)) {
        out->has_preview = true;
        return;
    }
    char echo[512];
    command_echo(&c, echo, sizeof echo);
    view_add(&out->preview, "%s - %s", echo, c.verb->about);
    out->has_preview = true;
}

bool line_take(const char *line, const LineState *s, int pick, char *out,
               size_t cap) {
    if (pick < 0 || pick >= s->ncand) return false;
    size_t len = strlen(line);
    size_t keep = len - (size_t)s->prefix_len;
    snprintf(out, cap, "%.*s%s", (int)keep, line, s->cand[pick]);
    /* one trailing space, so the next word starts where the cursor is */
    size_t n = strlen(out);
    if (n + 2 < cap) {
        out[n] = ' ';
        out[n + 1] = '\0';
    }
    return true;
}

bool command_view(App *a, const char *line, View *out) {
    Command c;
    char err[512];
    view_clear(out);
    if (!parse_line(line, &c, err, sizeof err)) return false;
    if (c.kind != CMD_RUN || !c.verb || !c.verb->view) return false;
    return c.verb->view(a, &c, out);
}

/* ---------- presets on disk ---------- */

static bool resolve(App *a, const char *name, PresetRef *out, char *err,
                    size_t err_len) {
    char want[192];
    lower_into(name, want, sizeof want);
    if (strcmp(want, SELECTED_WORD) == 0) {
        if (focus_highlighted(a, out)) return true;
        return reason(err, err_len, "%s needs a preset highlighted first",
                      SELECTED_WORD);
    }
    char *slash = strchr(want, '/');
    const char *want_name = want;
    const char *want_bank = NULL;
    if (slash) {
        *slash = '\0';
        want_name = slash + 1;
        want_bank = strcasecmp(want, MINE_BANK) == 0 ? "" : want;
    }
    int hits = 0;
    const PresetRef *hit = NULL;
    int passes = want_bank ? 1 : 2;
    for (int pass = 0; pass < passes && hits == 0; pass++) {
        for (int i = 0; i < a->preset_count; i++) {
            const PresetRef *p = &a->preset_names[i];
            if (strcmp(p->bank, TRASH_DIR) == 0) continue;
            if (want_bank && strcasecmp(p->bank, want_bank) != 0) continue;
            bool in_view = true;
            switch (a->preset_filter.kind) {
            case FILTER_ALL: break;
            case FILTER_MINE: in_view = strcmp(p->bank, STOCK_BANK) != 0; break;
            case FILTER_BANK:
                in_view = strcmp(p->bank, a->preset_filter.bank) == 0;
                break;
            }
            if (!want_bank && pass == 0 && !in_view) continue;
            char low[192];
            lower_into(p->name, low, sizeof low);
            if (strcmp(low, want_name) != 0) continue;
            hits++;
            hit = p;
        }
    }
    if (hits == 0) return reason(err, err_len, "no preset called %s", name);
    if (hits > 1)
        return reason(err, err_len,
                      "%s is in %d folders; use a folder/name path", name,
                      hits);
    *out = *hit;
    return true;
}

/* drops a highlighted or loaded preset whose file has gone */
static void prune_refs(App *a) {
    char path[PATHBUF];
    if (a->have_selected
        && (!preset_path(&a->preset_selected, path, sizeof path)
            || !path_exists(path)))
        a->have_selected = false;
    if (a->have_loaded
        && (!preset_path(&a->preset_loaded, path, sizeof path)
            || !path_exists(path)))
        a->have_loaded = false;
}

static void clear_name_bar(App *a) {
    a->preset_name.len = 0;
    a->preset_name.text[0] = '\0';
}

static void refile(App *a, const PresetRef *was, const PresetRef *now) {
    preset_rescan(a);
    clear_name_bar(a);
    if (a->have_selected && refs_equal(&a->preset_selected, was))
        a->preset_selected = *now;
    if (a->have_loaded && refs_equal(&a->preset_loaded, was))
        a->preset_loaded = *now;
}

/* ---------- trash and the undo journal ---------- */

typedef enum { J_NONE, J_TRASHED, J_RENAMED, J_MOVED, J_OVERWRITTEN } JournalKind;

static struct {
    JournalKind kind;
    char from[PATHBUF], to[PATHBUF];
    char what[256];
} journal;

static void journal_set(JournalKind kind, const char *from, const char *to,
                        const char *what) {
    journal.kind = kind;
    snprintf(journal.from, sizeof journal.from, "%s", from);
    snprintf(journal.to, sizeof journal.to, "%s", to);
    snprintf(journal.what, sizeof journal.what, "%s", what);
}

bool undo_pending(char *what, size_t cap) {
    if (journal.kind == J_NONE) return false;
    if (what) snprintf(what, cap, "%s", journal.what);
    return true;
}

/* a free slot in trash/ for leaf+ext, made on demand; ~N when taken */
static bool trash_slot(const char *leaf, const char *ext, char *out,
                       size_t cap) {
    char dir[PATHBUF];
    snprintf(dir, sizeof dir, "%s/%s", preset_dir(), TRASH_DIR);
    if (!is_dir_path(dir) && mkdir(dir, 0755) != 0) return false;
    int n = snprintf(out, cap, "%s/%s%s", dir, leaf, ext);
    if (n < 0 || (size_t)n >= cap) return false;
    for (int x = 1; path_exists(out); x++) {
        n = snprintf(out, cap, "%s/%s~%d%s", dir, leaf, x, ext);
        if (n < 0 || (size_t)n >= cap) return false;
    }
    return true;
}

static bool move_to_trash(const char *src, const char *leaf, const char *ext,
                          char *dst, size_t cap) {
    if (!trash_slot(leaf, ext, dst, cap)) return false;
    return rename(src, dst) == 0;
}

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ---------- handlers ---------- */

static const char *view_name(const App *a) {
    switch (a->preset_filter.kind) {
    case FILTER_ALL: return "ALL";
    case FILTER_MINE: return MINE_BANK;
    case FILTER_BANK: return a->preset_filter.bank;
    }
    return "ALL";
}

static bool run_cd(App *a, const Command *c, char *err, size_t n) {
    if (c->argc == 0) {
        push_log(a, "%s", view_name(a));
        return true;
    }
    const char *folder = c->arg[0];
    if (strcasecmp(folder, "ALL") == 0) {
        a->preset_filter.kind = FILTER_ALL;
        a->preset_filter.bank[0] = '\0';
    } else if (strcasecmp(folder, MINE_BANK) == 0) {
        a->preset_filter.kind = FILTER_MINE;
        a->preset_filter.bank[0] = '\0';
    } else {
        int found = -1;
        for (int i = 0; i < a->folder_count; i++)
            if (strcasecmp(a->preset_folders[i], folder) == 0) found = i;
        if (found < 0 || strcasecmp(folder, TRASH_DIR) == 0)
            return reason(err, n, "no preset folder called %s", folder);
        a->preset_filter.kind = FILTER_BANK;
        snprintf(a->preset_filter.bank, sizeof a->preset_filter.bank, "%s",
                 a->preset_folders[found]);
    }
    push_log(a, "preset view: %s", view_name(a));
    return true;
}

static bool run_ls(App *a, const Command *c, char *err, size_t n) {
    if (c->argc > 0 && strcasecmp(c->arg[0], "mod") == 0) {
        Command mods = {0};
        return mod_run_mods(a, &mods, err, n);
    }
    PresetFilter filter = a->preset_filter;
    if (c->argc > 0) {
        const char *path = c->arg[0];
        if (strchr(path, '/')) {
            PresetRef p;
            if (!resolve(a, path, &p, err, n)) return false;
            char q[256];
            preset_qualified(&p, q, sizeof q);
            push_log(a, "%s", q);
            return true;
        }
        if (strcasecmp(path, "ALL") == 0)
            filter.kind = FILTER_ALL;
        else if (strcasecmp(path, MINE_BANK) == 0)
            filter.kind = FILTER_MINE;
        else {
            bool found = false;
            for (int i = 0; i < a->folder_count; i++)
                if (strcasecmp(a->preset_folders[i], path) == 0) {
                    filter.kind = FILTER_BANK;
                    snprintf(filter.bank, sizeof filter.bank, "%s",
                             a->preset_folders[i]);
                    found = true;
                }
            if (!found) return reason(err, n, "no preset folder called %s", path);
        }
    }
    int listed = 0;
    for (int i = 0; i < a->preset_count; i++) {
        const PresetRef *p = &a->preset_names[i];
        bool show = filter.kind == FILTER_ALL
                    || (filter.kind == FILTER_MINE
                        && strcmp(p->bank, STOCK_BANK) != 0)
                    || (filter.kind == FILTER_BANK
                        && strcmp(p->bank, filter.bank) == 0);
        if (!show || strcmp(p->bank, TRASH_DIR) == 0) continue;
        char q[256];
        preset_qualified(p, q, sizeof q);
        push_log(a, "%s", q);
        listed++;
    }
    if (listed == 0) push_log(a, "%s is empty", c->argc ? c->arg[0] : view_name(a));
    return true;
}

static bool run_load(App *a, const Command *c, char *err, size_t n) {
    PresetRef p;
    if (!resolve(a, c->arg[0], &p, err, n)) return false;
    preset_load(a, &p);
    return true;
}

static bool run_rec(App *a, const Command *c, char *err, size_t n) {
    char args[512] = "";
    if (c->argc > 0) snprintf(args, sizeof args, "%s", c->arg[0]);
    if (c->has_end)
        scat(args, sizeof args, "%s--end %g", args[0] ? " " : "", (double)c->end);
    gui_run_record(a, args);
    return true;
}

static bool run_save(App *a, const Command *c, char *err, size_t n) {
    PresetRef p = ref_make(NULL, c->arg[0]);
    char path[PATHBUF];
    if (!preset_path(&p, path, sizeof path))
        return reason(err, n, "'%s' cannot be a preset name", c->arg[0]);
    if (path_exists(path))
        return reason(err, n, "%s already exists; use ow %s to replace it",
                      c->arg[0], c->arg[0]);
    if (!preset_run_save(a, c->arg[0]))
        return reason(err, n, "%s could not be saved", c->arg[0]);
    return true;
}

static bool run_overwrite(App *a, const Command *c, char *err, size_t n) {
    PresetRef p;
    if (!resolve(a, c->arg[0], &p, err, n)) return false;
    char path[PATHBUF];
    if (!preset_path(&p, path, sizeof path))
        return reason(err, n, "'%s' cannot be a name", p.name);
    Session s = app_session(a);
    char *json = session_to_json(&s);
    if (!json) return reason(err, n, "the current sound could not be written");
    char q[256];
    preset_qualified(&p, q, sizeof q);
    if (path_exists(path)) {
        char backup[PATHBUF];
        if (!move_to_trash(path, p.name, ".json", backup, sizeof backup)) {
            free(json);
            return reason(err, n, "overwrite failed: %s", strerror(errno));
        }
        journal_set(J_OVERWRITTEN, path, backup, q);
    }
    bool ok = write_text(path, json);
    free(json);
    if (!ok) return reason(err, n, "overwrite failed: %s", strerror(errno));
    preset_rescan(a);
    a->preset_selected = p;
    a->have_selected = true;
    a->preset_loaded = p;
    a->have_loaded = true;
    clear_name_bar(a);
    push_log(a, "'%s' overwritten with the current sound. undo brings the old one back.", q);
    return true;
}

static bool run_delete(App *a, const Command *c, char *err, size_t n) {
    if (c->recursive) {
        if (c->argc == 0) return reason(err, n, "rm -r wants a folder");
        return run_remove(a, c, err, n);
    }
    PresetRef p;
    if (c->argc > 0) {
        if (!resolve(a, c->arg[0], &p, err, n)) return false;
    } else if (!focus_highlighted(a, &p)) {
        return reason(err, n, "rm wants a preset");
    }
    char path[PATHBUF], dst[PATHBUF];
    if (!preset_path(&p, path, sizeof path) || !path_exists(path))
        return reason(err, n, "no preset called %s", p.name);
    if (!move_to_trash(path, p.name, ".json", dst, sizeof dst))
        return reason(err, n, "delete failed: %s", strerror(errno));
    char q[256];
    preset_qualified(&p, q, sizeof q);
    journal_set(J_TRASHED, path, dst, q);
    preset_rescan(a);
    prune_refs(a);
    push_log(a, "preset '%s' moved to the trash. undo puts it back.", q);
    return true;
}

static bool run_rename(App *a, const Command *c, char *err, size_t n) {
    PresetRef p;
    if (!resolve(a, c->arg[0], &p, err, n)) return false;
    if (strcmp(p.bank, STOCK_BANK) == 0)
        return reason(err, n,
                      "%s is the shipped bank and an update rewrites it; save "
                      "your own copy instead",
                      STOCK_BANK);
    if (strcasecmp(c->arg[1], SELECTED_WORD) == 0)
        return reason(err, n, "%s cannot be a destination", SELECTED_WORD);

    PresetRef dest;
    const char *slash = strchr(c->arg[1], '/');
    if (slash) {
        char bank[64];
        snprintf(bank, sizeof bank, "%.*s", (int)(slash - c->arg[1]),
                 c->arg[1]);
        dest = ref_make(strcasecmp(bank, MINE_BANK) == 0 ? "" : bank,
                        slash + 1);
    } else {
        char candidate[PATHBUF];
        snprintf(candidate, sizeof candidate, "%s/%s", preset_dir(),
                 c->arg[1]);
        dest = is_dir_path(candidate) ? ref_make(c->arg[1], p.name)
                                      : ref_make(p.bank, c->arg[1]);
    }
    if (refs_equal(&dest, &p))
        return reason(err, n, "%s is already there", c->arg[0]);
    if (dest.bank[0]) {
        if (!folder_ok(dest.bank, err, n)) return false;
        char folder_path[PATHBUF];
        snprintf(folder_path, sizeof folder_path, "%s/%s", preset_dir(),
                 dest.bank);
        if (!is_dir_path(folder_path))
            return reason(err, n, "no folder called %s; mkdir %s makes it",
                          dest.bank, dest.bank);
    }
    char src[PATHBUF], dst[PATHBUF];
    if (!preset_path(&p, src, sizeof src) || !preset_path(&dest, dst, sizeof dst))
        return reason(err, n, "'%s' cannot be a name", c->arg[1]);
    if (path_exists(dst))
        return reason(err, n, "%s already holds a %s", preset_bank_label(&p),
                      dest.name);
    if (rename(src, dst) != 0)
        return reason(err, n, "rename failed: %s", strerror(errno));
    char q[256];
    preset_qualified(&dest, q, sizeof q);
    journal_set(strcmp(p.bank, dest.bank) == 0 ? J_RENAMED : J_MOVED, src, dst,
                q);
    refile(a, &p, &dest);
    push_log(a, "'%s' moved to '%s'.", p.name, q);
    return true;
}

static bool folder_ok(const char *folder, char *err, size_t n) {
    if (strcasecmp(folder, STOCK_BANK) == 0)
        return reason(err, n,
                      "%s is the shipped bank and an update rewrites it; use "
                      "one of yours",
                      STOCK_BANK);
    if (is_view_name(folder))
        return reason(err, n, "%s is a view of the list, not a folder on disk",
                      folder);
    if (strcasecmp(folder, TRASH_DIR) == 0)
        return reason(err, n, "%s is where deleted things wait for undo",
                      TRASH_DIR);
    return true;
}

static bool run_add(App *a, const Command *c, char *err, size_t n) {
    const char *folder = c->arg[0];
    if (!folder_ok(folder, err, n)) return false;
    if (strcasecmp(folder, SELECTED_WORD) == 0)
        return reason(err, n, "%s cannot be a folder name", SELECTED_WORD);
    char path[PATHBUF];
    snprintf(path, sizeof path, "%s/%s", preset_dir(), folder);
    if (is_dir_path(path)) return reason(err, n, "%s is already there", folder);
    if (mkdir(path, 0755) != 0)
        return reason(err, n, "folder %s could not be made: %s", folder,
                      strerror(errno));
    preset_rescan(a);
    clear_name_bar(a);
    push_log(a, "folder '%s' made. it is empty until you move something in.",
             folder);
    return true;
}

static bool run_remove(App *a, const Command *c, char *err, size_t n) {
    const char *folder = c->arg[0];
    if (is_view_name(folder))
        return reason(err, n, "%s is a view of the list, not a folder on disk",
                      folder);
    if (strcasecmp(folder, TRASH_DIR) == 0)
        return reason(err, n, "%s is where deleted things wait for undo",
                      TRASH_DIR);
    char path[PATHBUF], dst[PATHBUF];
    snprintf(path, sizeof path, "%s/%s", preset_dir(), folder);
    if (!is_dir_path(path)) return reason(err, n, "no folder called %s", folder);
    if (!move_to_trash(path, folder, "", dst, sizeof dst))
        return reason(err, n, "remove failed: %s", strerror(errno));
    journal_set(J_TRASHED, path, dst, folder);
    preset_rescan(a);
    clear_name_bar(a);
    if (a->preset_filter.kind == FILTER_BANK
        && strcmp(a->preset_filter.bank, folder) == 0) {
        a->preset_filter.kind = FILTER_ALL;
        a->preset_filter.bank[0] = '\0';
    }
    prune_refs(a);
    push_log(a, "folder '%s' and everything in it moved to the trash. undo puts it back.",
             folder);
    return true;
}

static bool run_rmdir(App *a, const Command *c, char *err, size_t n) {
    const char *folder = c->arg[0];
    if (!folder_ok(folder, err, n)) return false;
    char path[PATHBUF];
    snprintf(path, sizeof path, "%s/%s", preset_dir(), folder);
    if (!is_dir_path(path)) return reason(err, n, "no folder called %s", folder);
    if (rmdir(path) != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST)
            return reason(err, n, "%s is not empty; use rm -r %s", folder,
                          folder);
        return reason(err, n, "rmdir failed: %s", strerror(errno));
    }
    preset_rescan(a);
    if (a->preset_filter.kind == FILTER_BANK
        && strcmp(a->preset_filter.bank, folder) == 0) {
        a->preset_filter.kind = FILTER_ALL;
        a->preset_filter.bank[0] = '\0';
    }
    push_log(a, "empty folder '%s' removed.", folder);
    return true;
}

static bool run_undo(App *a, const Command *c, char *err, size_t n) {
    if (journal.kind == J_NONE) return reason(err, n, "nothing to undo");
    if (!path_exists(journal.to))
        return reason(err, n, "%s has gone from where undo left it",
                      journal.what);
    if (journal.kind == J_OVERWRITTEN)
        remove(journal.from);
    else if (path_exists(journal.from))
        return reason(err, n, "%s is taken again, so undo cannot put it back",
                      journal.what);
    if (rename(journal.to, journal.from) != 0)
        return reason(err, n, "undo failed: %s", strerror(errno));
    preset_rescan(a);
    prune_refs(a);
    push_log(a, "'%s' put back.", journal.what);
    journal.kind = J_NONE;
    return true;
}

static bool run_bind(App *a, const Command *c, char *err, size_t n) {
    gui_run_bind(a, c->cc, c->target);
    return true;
}

static bool run_unbind(App *a, const Command *c, char *err, size_t n) {
    gui_run_unbind(a, c->cc_all ? -1 : c->cc);
    return true;
}

/* help is answered at parse time; nothing reaches here */
static bool run_help(App *a, const Command *c, char *err, size_t n) {
    return true;
}

/* ---------- history ---------- */

void history_push(History *h, const char *line) {
    h->cursor = -1;
    char t[LOG_LINE_LEN];
    trim_into(line, t, sizeof t);
    if (!t[0]) return;
    if (h->len > 0 && strcmp(h->line[h->len - 1], t) == 0) return;
    if (h->len == HISTORY_MAX) {
        memmove(h->line[0], h->line[1], sizeof h->line[0] * (HISTORY_MAX - 1));
        h->len--;
    }
    snprintf(h->line[h->len++], LOG_LINE_LEN, "%s", t);
}

const char *history_up(History *h) {
    if (h->len == 0) return NULL;
    int i = h->cursor < 0 ? h->len - 1 : h->cursor - 1;
    if (i < 0) i = 0;
    h->cursor = i;
    return h->line[i];
}

const char *history_down(History *h) {
    if (h->cursor < 0) return NULL;
    int i = h->cursor + 1;
    if (i >= h->len) {
        h->cursor = -1;
        return NULL;
    }
    h->cursor = i;
    return h->line[i];
}

void history_reset(History *h) { h->cursor = -1; }
