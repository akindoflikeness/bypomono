#ifndef BYPO_COMMAND_H
#define BYPO_COMMAND_H

#include "app.h"

#define ARG_SEP " - "
#define SELECTED_WORD "selected"
#define TRASH_DIR "trash"

typedef enum { CMD_NOP, CMD_HELP, CMD_RUN } CommandKind;

typedef struct Command {
    CommandKind kind;
    const struct Verb *verb; /* CMD_RUN only */
    char arg[2][192];
    int argc;
    bool has_end;
    float end;
    int cc;
    bool cc_all;
    CcTarget target;
    char text[1024]; /* CMD_HELP: lines joined with '\n' */
} Command;

typedef struct {
    const char *name;
    const char *value; /* NULL = bare flag */
    const char *about;
} Flag;

typedef enum { G_PRESETS, G_MIDI, G_RECORDING, G_CONSOLE, G_COUNT } VerbGroup;

/* false leaves a one-line reason in err; the caller adds the usage line */
typedef bool (*VerbRun)(App *a, const Command *c, char *err, size_t err_len);

typedef struct Verb {
    const char *name;
    const char *aliases[2]; /* NULL-terminated */
    VerbGroup group;
    const char *args[2]; /* placeholders; the second follows " - " */
    int nargs, required;
    bool words; /* single-word arguments, so plain spaces split them too */
    const Flag *flags;
    int nflags;
    const char *about;
    VerbRun run;
} Verb;

int verb_count(void);
const Verb *verb_at(int i);
const Verb *verb_lookup(const char *word);
const Verb *verb_nearest(const char *word);
int verb_complete(const char *prefix, const Verb **out, int max);
const char *verb_group_title(VerbGroup g);
/* "usage: move <preset> - <folder>" */
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
