#include <SDL2/SDL_scancode.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "command.h"
#include "focus.h"

/* widgets.c */
extern float hint_chip(Ui *ui, P2 at, const char *text, bool highlighted);

#define TYPE_CPS 24.0f
#define TYPE_JITTER 0.7f
#define CONSOLE_CPS 80.0f

/* ---------- tips ---------- */

typedef enum {
    W_ALWAYS = 0, W_HIGHLIGHTED, W_BROWSING, W_COMMANDING, W_NOTHING_SAVED
} TipWhen;

typedef struct {
    TipWhen needs;
    const char *text;
} Tip;

static const Tip TIPS[] = {
    {W_ALWAYS, "press / to type a command. help on its own lists them."},
    {W_ALWAYS, "USER holds everything you made, wherever you filed it."},
    {W_COMMANDING,
     "move <preset> - <folder> files a preset. add <folder> makes the folder."},
    {W_COMMANDING,
     "put -h after any verb — move -h — and it will tell you how it is spelled."},
    {W_HIGHLIGHTED,
     "selected stands for the highlighted preset: move selected - <folder>."},
    {W_HIGHLIGHTED,
     "rename <preset> - <new name> renames it where it sits, folder and all."},
    {W_BROWSING, "click a preset to highlight it, click it again to load it."},
    {W_ALWAYS, "two clicks on the bar, under a second, opens it to type in."},
    {W_NOTHING_SAVED, "save never clobbers — a taken name earns a ~1."},
    {W_COMMANDING,
     "delete <preset> and remove <folder> go to the trash. undo puts the last "
     "one back."},
    {W_HIGHLIGHTED,
     "overwrite selected replaces it with the sound you have now."},
};

void tell_new_tips(App *a) {
    TipWhen hot[5];
    int n = 0;
    hot[n++] = W_ALWAYS;
    if (focus_highlighted(a, NULL)) hot[n++] = W_HIGHLIGHTED;
    if (focus_browsing(a)) hot[n++] = W_BROWSING;
    if (focus_console_open(a)) hot[n++] = W_COMMANDING;
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

static void trim_into(const char *s, char *out, size_t cap) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
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

/* one push_log per line of a multi-line text */
static void log_lines(App *a, const char *text) {
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        push_log(a, "%.*s", (int)n, p);
        if (!nl) break;
        p = nl + 1;
    }
}

static void first_line(const char *text, char *out, size_t cap) {
    const char *nl = strchr(text, '\n');
    size_t n = nl ? (size_t)(nl - text) : strlen(text);
    if (n >= cap) n = cap - 1;
    memcpy(out, text, n);
    out[n] = '\0';
}

static void set_input(App *a, const char *text) {
    snprintf(a->console_input.text, sizeof a->console_input.text, "%s", text);
    a->console_input.len = (int)strlen(a->console_input.text);
}

/* ---------- hints ---------- */

typedef enum { H_ROSTER, H_COMPLETING, H_PREVIEW, H_BAD } HintKind;

#define MAX_VERBS 32

typedef struct {
    HintKind kind;
    const Verb *m[MAX_VERBS];
    int m_len;
    char line[512], note[256];
    char why[768];
} Hint;

static void hint_for(const char *raw_in, Hint *out) {
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
    if (single && !verb_lookup(head) && strcmp(head, "?") != 0) {
        out->m_len = verb_complete(head, out->m, MAX_VERBS);
        if (out->m_len == 0) {
            out->kind = H_BAD;
            const Verb *near = verb_nearest(head);
            if (near)
                snprintf(out->why, sizeof out->why,
                         "no verb called %s; did you mean %s?", head, near->name);
            else
                snprintf(out->why, sizeof out->why, "no verb called %s", head);
        } else {
            out->kind = H_COMPLETING;
        }
        return;
    }
    Command c;
    char err[768];
    if (!parse_line(raw, &c, err, sizeof err)) {
        out->kind = H_BAD;
        first_line(err, out->why, sizeof out->why);
        return;
    }
    switch (c.kind) {
    case CMD_RUN:
        out->kind = H_PREVIEW;
        command_echo(&c, out->line, sizeof out->line);
        snprintf(out->note, sizeof out->note, "%s", c.verb->about);
        break;
    case CMD_HELP:
        out->kind = H_BAD;
        first_line(c.text, out->why, sizeof out->why);
        break;
    case CMD_NOP: out->kind = H_ROSTER; break;
    }
}

/* ---------- running ---------- */

static History history;

void console_run_line(App *a, const char *line) {
    /* nothing arms any more; the field stays for the icons panes.c draws */
    a->preset_armed = 0;
    Command c;
    char err[768];
    if (!parse_line(line, &c, err, sizeof err)) {
        log_lines(a, err);
        return;
    }
    switch (c.kind) {
    case CMD_NOP: break;
    case CMD_HELP:
        log_lines(a, c.text);
        history_push(&history, line);
        break;
    case CMD_RUN: {
        char echo[512];
        command_echo(&c, echo, sizeof echo);
        push_log(a, "%s", echo);
        if (!command_run(a, &c, err, sizeof err)) {
            log_lines(a, err);
            break;
        }
        history_push(&history, line);
        break;
    }
    }
}

/* ---------- completion walk ---------- */

/* the verbs the chip row offers for this line, in chip order */
static int line_candidates(const char *text, const Verb **out) {
    Hint h;
    hint_for(text, &h);
    if (h.kind == H_COMPLETING) {
        for (int i = 0; i < h.m_len; i++) out[i] = h.m[i];
        return h.m_len;
    }
    if (h.kind != H_ROSTER) return 0;
    int n = verb_count();
    if (n > MAX_VERBS) n = MAX_VERBS;
    for (int i = 0; i < n; i++) out[i] = verb_at(i);
    return n;
}

/* any edit to the line drops the highlight */
static void line_sync(App *a) {
    if (strcmp(a->line_seen, a->console_input.text) == 0) return;
    snprintf(a->line_seen, sizeof a->line_seen, "%s", a->console_input.text);
    a->line_lit = 0;
}

static void line_take(App *a, const Verb *v) {
    char text[256];
    snprintf(text, sizeof text, v->nargs > 0 ? "%s " : "%s", v->name);
    set_input(a, text);
    line_sync(a);
}

void console_tab(App *a) {
    if (!focus_console_active(a)) return;
    line_sync(a);
    const Verb *m[MAX_VERBS];
    int n = line_candidates(a->console_input.text, m);
    if (n == 1 && a->console_input.len > 0) line_take(a, m[0]);
    else if (n > 0) a->line_lit = a->line_lit % n + 1;
}

void console_enter(App *a) {
    line_sync(a);
    const Verb *m[MAX_VERBS];
    int n = line_candidates(a->console_input.text, m);
    if (a->line_lit > 0 && n > 0) {
        line_take(a, m[(a->line_lit - 1) % n]);
        return;
    }
    char line[256];
    snprintf(line, sizeof line, "%s", a->console_input.text);
    set_input(a, "");
    line_sync(a);
    if (line[0]) console_run_line(a, line);
}

bool console_escape(App *a) {
    line_sync(a);
    if (a->line_lit == 0) return false;
    a->line_lit = 0;
    return true;
}

/* Up and Down walk the lines that ran; walking off the newest end clears */
static void walk_history(App *a, Ui *ui) {
    if (ui->in.key_pressed[SDL_SCANCODE_UP]) {
        const char *l = history_up(&history);
        if (l) set_input(a, l);
    } else if (ui->in.key_pressed[SDL_SCANCODE_DOWN]) {
        const char *l = history_down(&history);
        set_input(a, l ? l : "");
    } else if (ui->in.text[0] || ui->in.key_pressed[SDL_SCANCODE_RETURN]) {
        history_reset(&history);
    }
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
    bool open = focus_console_open(a);
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
        bool focused = ui->focus == fid;
        if (focused) walk_history(a, ui);
        ui_text_edit(ui, fid, &a->console_input);
        line_sync(a);
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
        if (open) {
            focus_console_drop(a);
            if (ui->focus == fid) ui->focus = 0;
        } else {
            focus_console_take(a);
        }
    }
}

/* ---------- drawer ---------- */

static void draw_hint(App *a, Ui *ui, FontId small, Rct row) {
    Canvas *c = ui->canvas;
    Hint h;
    hint_for(a->console_input.text, &h);
    if (h.kind == H_ROSTER || h.kind == H_COMPLETING) {
        const Verb *rows[MAX_VERBS];
        int n = line_candidates(a->console_input.text, rows);
        int lit = n > 0 && a->line_lit > 0 ? (a->line_lit - 1) % n : -1;
        float x = row.x0;
        for (int i = 0; i < n; i++) {
            float w = hint_chip(ui, (P2){x, row.y0}, rows[i]->name, i == lit);
            x += w + GAP;
        }
        return;
    }
    if (h.kind == H_PREVIEW) {
        char line[1024];
        snprintf(line, sizeof line, "%s — %s · enter", h.line, h.note);
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
