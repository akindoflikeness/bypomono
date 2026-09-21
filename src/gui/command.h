#ifndef BYPO_COMMAND_H
#define BYPO_COMMAND_H

#include "app.h"

#define SELECTED_WORD "selected"
#define TRASH_DIR "trash"

typedef enum { CMD_NOP, CMD_HELP, CMD_RUN } CommandKind;

#define MOD_CMD_ROUTES 8

/* lfo <n> [shape] [rate] [phase] [mode] [bi|uni] [to <target> <depth|off>]... */
typedef struct {
    int slot; /* 0-based; -1 = every lfo */
    bool rm;
    bool set_shape, set_rate, set_phase, set_mode, set_pol;
    uint8_t shape, mode;
    bool unipolar;
    int8_t division; /* -1 = rate_hz */
    float rate_hz, phase;
    int nroutes;
    struct {
        uint8_t target;
        float depth;
        bool off;
    } route[MOD_CMD_ROUTES];
} ModCmd;

#define CMD_WORDS 32

typedef struct Command {
    CommandKind kind;
    const struct Verb *verb; /* CMD_RUN only */
    char arg[2][192];
    int argc;
    bool has_end;
    float end;
    int cc;
    bool cc_all;
    bool recursive;
    CcTarget target;
    float value;                /* direct control value in its real unit */
    int choice;                 /* parsed enum or integer direct control */
    bool view;                  /* -v: pin the line's view above the log */
    char words[CMD_WORDS][64];  /* raw verbs: the words after the verb */
    int nwords;
    ModCmd mod;
    char text[1024]; /* CMD_HELP: lines joined with '\n' */
} Command;

typedef struct {
    const char *name;
    const char *value; /* NULL = bare flag */
    const char *about;
} Flag;

typedef enum {
    G_PRESETS, G_SOUND, G_MODULATION, G_MIDI, G_RECORDING, G_CONSOLE, G_COUNT
} VerbGroup;

/* false leaves a one-line reason in err; the caller adds the usage line */
typedef bool (*VerbRun)(App *a, const Command *c, char *err, size_t err_len);
/* raw verbs: reads c->words into the command at parse time, so a bad line
   is caught before Enter */
typedef bool (*VerbParse)(Command *c, char *err, size_t err_len);
/* what the line shows: logged once when it runs, redrawn live when pinned.
   false when there is nothing to show */
typedef bool (*VerbView)(App *a, const Command *c, View *out);
/* what the line would do if it ran, drawn under the input before Enter */
typedef bool (*VerbPreview)(App *a, const Command *c, View *out);
/* the words that could follow what is typed so far; prefix is the word being
   typed, and may be empty */
#define CAND_MAX 40
#define CAND_LEN 192
typedef int (*VerbComplete)(char *const words[], int nwords, const char *prefix,
                            char out[][CAND_LEN], int max);

typedef struct Verb {
    const char *name;
    const char *aliases[3]; /* NULL-terminated */
    VerbGroup group;
    const char *args[2];
    int nargs, required;
    bool words; /* single-word arguments, so plain spaces split them too */
    const Flag *flags;
    int nflags;
    const char *about;
    VerbRun run;
    VerbParse parse; /* non-NULL makes it a raw verb */
    VerbView view;
    VerbPreview preview;
    VerbComplete complete;
    const char *form;  /* raw verbs: the usage after the name */
    const char *extra; /* more help lines, '\n' separated */
} Verb;

/* cmd_mod.c */
bool mod_parse_lfo(Command *c, char *err, size_t err_len);
bool mod_run_lfo(App *a, const Command *c, char *err, size_t err_len);
bool mod_view_lfo(App *a, const Command *c, View *out);
bool mod_preview_lfo(App *a, const Command *c, View *out);
int mod_complete_lfo(char *const words[], int nwords, const char *prefix,
                     char out[][CAND_LEN], int max);
bool mod_parse_mods(Command *c, char *err, size_t err_len);
bool mod_run_mods(App *a, const Command *c, char *err, size_t err_len);
bool mod_view_mods(App *a, const Command *c, View *out);

/* cmd_controls.c */
bool control_parse(Command *c, char *err, size_t err_len);
bool control_run(App *a, const Command *c, char *err, size_t err_len);
int control_complete(char *const words[], int nwords, const char *prefix,
                     char out[][CAND_LEN], int max);
bool control_parse_mel(Command *c, char *err, size_t err_len);
bool control_run_mel(App *a, const Command *c, char *err, size_t err_len);
int control_complete_mel(char *const words[], int nwords, const char *prefix,
                         char out[][CAND_LEN], int max);
bool control_parse_chandas(Command *c, char *err, size_t err_len);
bool control_run_chandas(App *a, const Command *c, char *err, size_t err_len);
int control_complete_chandas(char *const words[], int nwords,
                             const char *prefix, char out[][CAND_LEN], int max);
bool control_get(App *a, char *const words[], int nwords, char *err, size_t n);
bool control_status(App *a, const char *section, char *err, size_t n);

/* ---------- the line being typed ---------- */

typedef struct {
    bool bad;
    char why[768];              /* the first line of the parse error */
    char cand[CAND_MAX][CAND_LEN];
    int ncand;
    int prefix_len;             /* how much of cand[] the typed word covers */
    View preview;               /* what Enter would do */
    bool has_preview;
} LineState;

/* candidates, ghost text and preview for a half-typed line */
void line_state(App *a, const char *line, LineState *out);
/* the typed line with candidate `pick` taken; false when there is nothing to
   take */
bool line_take(const char *line, const LineState *s, int pick, char *out,
               size_t cap);

/* the live view of a pinned line; false when it no longer parses */
bool command_view(App *a, const char *line, View *out);
/* -v toggles a pin: true when the line is now pinned */
bool console_toggle_pin(App *a, const char *pin, char *err, size_t err_len);
void console_unpin_at(App *a, int i);

int verb_count(void);
const Verb *verb_at(int i);
const Verb *verb_lookup(const char *word);
const Verb *verb_nearest(const char *word);
int verb_complete(const char *prefix, const Verb **out, int max);
const char *verb_group_title(VerbGroup g);
/* "usage: mv <source> <destination>" */
void verb_usage(const Verb *v, char *out, size_t cap);
/* usage, the description, the flags and the aliases, one per line */
void verb_help(const Verb *v, char *out, size_t cap);
/* one line per group */
void help_text(char *out, size_t cap);
bool help_group_text(const char *name, char *out, size_t cap);

/* Tokenises one line. false leaves a two-line error in err: what was
   wrong, then the usage. An empty line parses as CMD_NOP. */
bool parse_line(const char *line, Command *out, char *err, size_t err_len);
void command_echo(const Command *c, char *out, size_t cap);
/* Runs a CMD_RUN command; false leaves the same two-line error shape. */
bool command_run(App *a, const Command *c, char *err, size_t err_len);
/* what undo would put back, or false when the journal is empty */
bool undo_pending(char *what, size_t cap);

/* lines that ran, walked with Up and Down */
#define HISTORY_MAX 64
typedef struct {
    char line[HISTORY_MAX][LOG_LINE_LEN];
    int len;
    int cursor; /* -1 = not walking */
} History;
void history_push(History *h, const char *line);
const char *history_up(History *h);
const char *history_down(History *h); /* NULL = walked off the newest end */
void history_reset(History *h);

#endif
