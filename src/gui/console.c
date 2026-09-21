#include <SDL2/SDL_scancode.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "command.h"
#include "focus.h"

#define DIM_INK 120
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
     "mv <preset> <folder> files a preset. mkdir <folder> makes the folder."},
    {W_COMMANDING,
     "put -h after any verb — mv -h — and it will tell you how it is spelled."},
    {W_HIGHLIGHTED,
     "selected stands for the highlighted preset: mv selected <folder>."},
    {W_HIGHLIGHTED,
     "mv <preset> <new_name> renames it where it sits, folder and all."},
    {W_BROWSING, "click a preset to highlight it, click it again to load it."},
    {W_ALWAYS, "two clicks on the bar, under a second, opens it to type in."},
    {W_NOTHING_SAVED, "save never clobbers — use ow when a name is taken."},
    {W_COMMANDING,
     "rm <preset> and rm -r <folder> go to the trash. undo puts the last "
     "one back."},
    {W_HIGHLIGHTED,
     "ow selected replaces it with the sound you have now."},
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


static void set_input(App *a, const char *text) {
    snprintf(a->console_input.text, sizeof a->console_input.text, "%s", text);
    a->console_input.len = (int)strlen(a->console_input.text);
}

/* ---------- running ---------- */

static History history;

void console_run_line(App *a, const char *line) {
    /* Failed input is the most useful input to recover with Up: one typo
       should cost one edit, not retyping the whole command. */
    history_push(&history, line);
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
        break;
    case CMD_RUN: {
        char echo[512];
        command_echo(&c, echo, sizeof echo);
        push_log(a, "%s", echo);
        if (!command_run(a, &c, err, sizeof err)) {
            log_lines(a, err);
            break;
        }
        break;
    }
    }
}

/* ---------- completion walk ---------- */

/* what the typed line offers and would do; rebuilt whenever it is read */
static LineState g_state;

static const LineState *line_now(App *a) {
    line_state(a, a->console_input.text, &g_state);
    return &g_state;
}

/* any edit to the line drops the highlight */
static void line_sync(App *a) {
    if (strcmp(a->line_seen, a->console_input.text) == 0) return;
    snprintf(a->line_seen, sizeof a->line_seen, "%s", a->console_input.text);
    a->line_lit = 0;
}

/* the candidate the ghost text shows: the highlighted one, else the first */
static int line_pick(const App *a, const LineState *s) {
    if (s->ncand == 0) return -1;
    return a->line_lit > 0 ? (a->line_lit - 1) % s->ncand : 0;
}

/* only Tab highlights, so nothing is inverted until it is pressed */
static int line_sel(const App *a, const LineState *s) {
    return a->line_lit > 0 ? line_pick(a, s) : -1;
}

static void take_candidate(App *a, const LineState *s, int pick) {
    char text[LOG_LINE_LEN];
    if (!line_take(a->console_input.text, s, pick, text, sizeof text)) return;
    set_input(a, text);
    line_sync(a);
}

void console_tab(App *a) {
    if (!focus_console_active(a)) return;
    line_sync(a);
    const LineState *s = line_now(a);
    if (s->ncand == 1)
        take_candidate(a, s, 0);
    else if (s->ncand > 0)
        a->line_lit = a->line_lit % s->ncand + 1;
}

void console_enter(App *a) {
    line_sync(a);
    if (a->line_lit > 0) {
        const LineState *s = line_now(a);
        int pick = line_pick(a, s);
        if (pick >= 0) {
            take_candidate(a, s, pick);
            return;
        }
    }
    char line[LOG_LINE_LEN];
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
    float right_limit = r.x1 - GROUP;
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
        if (a->console_input.len == 0) {
            text_draw(c, small, (P2){field.x0, cy}, ALIGN_LEFT_CENTER,
                      "command", PAPER, 0.0f);
        } else {
            text_draw(c, small, (P2){tx, ty}, ALIGN_LEFT_TOP,
                      a->console_input.text, PAPER, 0.0f);
            /* the rest of the candidate, dim, where typing it would land */
            const LineState *s = line_now(a);
            int pick = line_pick(a, s);
            if (pick >= 0 && (int)strlen(s->cand[pick]) > s->prefix_len)
                text_draw(c, small, (P2){roundf(tx + tw), ty}, ALIGN_LEFT_TOP,
                          s->cand[pick] + s->prefix_len, DIM_INK, 0.0f);
        }
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

/* ---------- strips ---------- */

#define STRIP_H 25.0f
#define STRIP_PITCH 2.0f
#define ZERO_INK 80
#define MARK_INK 140

/* one column every STRIP_PITCH pixels, lit from its low to its high; the
   playhead is a dim full-height column with the value picked out on it */
static void draw_strip(Canvas *c, const Graph *g, float x0, float y0, float h) {
    float mid = roundf(y0 + 0.5f * h);
    float half = 0.5f * h - 1.0f;
    float w = (float)g->cols * STRIP_PITCH;
    for (float x = 0.0f; x < w; x += 2.0f * STRIP_PITCH)
        draw_rect_filled(c, rct_xywh(x0 + x, mid, 1.0f, 1.0f), ZERO_INK);
    for (int i = 0; i < g->cols; i++) {
        float x = x0 + (float)i * STRIP_PITCH;
        float yh = roundf(mid - (float)g->hi[i] / 127.0f * half);
        float yl = roundf(mid - (float)g->lo[i] / 127.0f * half);
        draw_rect_filled(c, rct(x, yh, x + 1.0f, yl + 1.0f), PAPER);
    }
    if (g->mark >= 0 && g->mark < g->cols) {
        float x = x0 + (float)g->mark * STRIP_PITCH;
        draw_rect_filled(c, rct(x, y0, x + 1.0f, y0 + h), MARK_INK);
        float yv = roundf(mid - 0.5f * (float)(g->hi[g->mark] + g->lo[g->mark])
                                    / 127.0f * half);
        draw_rect_filled(c, rct_xywh(x - 1.0f, yv - 1.0f, 3.0f, 3.0f), PAPER);
    }
}

static float line_height(GraphPlace place, float row) {
    return place == GRAPH_BELOW ? row + TIGHT + STRIP_H : row;
}

/* a text line with its strip; returns the height it took */
static float draw_view_line(Canvas *c, FontId f, float x, float y,
                            const char *text, GraphPlace place,
                            const Graph *g) {
    float row = text_row_height(f);
    text_draw(c, f, (P2){x, roundf(y)}, ALIGN_LEFT_TOP, text, PAPER, 0.0f);
    if (place == GRAPH_BELOW)
        draw_strip(c, g, x, roundf(y + row + TIGHT), STRIP_H);
    else if (place == GRAPH_RIGHT)
        draw_strip(c, g, roundf(x + text_width(f, text, 0.0f) + GROUP),
                   roundf(y), row);
    return line_height(place, row);
}

/* ---------- what the line offers and would do ---------- */

/* the candidate words, the highlighted one inverted */
static void draw_cands(Canvas *c, FontId f, Rct row, const LineState *s,
                       int pick) {
    float x = row.x0;
    int from = 0;
    /* keep the highlighted word on screen by starting the row at it */
    if (pick > 0) {
        float need = 0.0f;
        for (int i = 0; i <= pick; i++)
            need += text_width(f, s->cand[i], 0.0f) + 2.0f * GAP;
        while (need > rct_w(row) && from < pick) {
            need -= text_width(f, s->cand[from], 0.0f) + 2.0f * GAP;
            from++;
        }
    }
    for (int i = from; i < s->ncand; i++) {
        float w = text_width(f, s->cand[i], 0.0f);
        if (x + w > row.x1) break;
        if (i == pick) {
            draw_rect_filled(c, rct(x - 1.0f, row.y0 - 1.0f, x + w + 1.0f,
                                    row.y0 + text_row_height(f) + 1.0f),
                             PAPER);
            text_draw(c, f, (P2){x, row.y0}, ALIGN_LEFT_TOP, s->cand[i],
                      INK_BLACK, 0.0f);
        } else {
            text_draw(c, f, (P2){x, row.y0}, ALIGN_LEFT_TOP, s->cand[i],
                      DIM_INK, 0.0f);
        }
        x += w + 2.0f * GAP;
    }
}

/* a word still being typed is not an error yet, so the reason waits until
   nothing completes it */
static bool hint_shows_why(const LineState *s) {
    return s->bad && s->ncand == 0;
}

static float hint_height(const LineState *s, float row) {
    float h = s->ncand > 1 ? row + TIGHT : 0.0f;
    if (hint_shows_why(s)) return h + row;
    for (int i = 0; i < s->preview.n; i++)
        h += line_height(s->preview.line[i].place, row) + (i ? TIGHT : 0.0f);
    return fmaxf(h, row);
}

static void draw_hint(App *a, Canvas *c, FontId f, Rct zone,
                      const LineState *s) {
    float row = text_row_height(f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(saved, zone));
    float y = zone.y0;
    if (s->ncand > 1) {
        draw_cands(c, f, rct(zone.x0, y, zone.x1, y + row), s, line_sel(a, s));
        y += row + TIGHT;
    }
    if (hint_shows_why(s)) {
        text_draw(c, f, (P2){zone.x0, y}, ALIGN_LEFT_TOP, s->why, PAPER, 0.0f);
        canvas_set_clip(c, saved);
        return;
    }
    for (int i = 0; i < s->preview.n; i++) {
        const ViewLine *l = &s->preview.line[i];
        y += draw_view_line(c, f, zone.x0, y, l->text, l->place, &l->graph)
             + TIGHT;
    }
    canvas_set_clip(c, saved);
}

/* ---------- pins ---------- */

static View pin_views[PIN_MAX];
static bool pin_live[PIN_MAX];

static float pin_height(App *a, int i, float row) {
    if (!pin_live[i]) return row + GROUP;
    float h = 0.0f;
    for (int k = 0; k < pin_views[i].n; k++) {
        GraphPlace place = a->pin_folded[i] ? GRAPH_NONE
                                            : pin_views[i].line[k].place;
        h += line_height(place, row) + GROUP;
        if (a->pin_folded[i]) break; /* folded shows its first line only */
    }
    return h;
}

static float pins_height(App *a, float row) {
    float h = 0.0f;
    for (int i = 0; i < a->pin_count; i++) {
        pin_live[i] = command_view(a, a->pins[i], &pin_views[i]);
        h += pin_height(a, i, row);
    }
    return h;
}

static void draw_pins(App *a, Ui *ui, FontId f, Rct zone) {
    Canvas *c = ui->canvas;
    float row = text_row_height(f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(saved, zone));
    float y = zone.y0;
    float x = zone.x0 + GROUP;
    int let_go = -1;
    for (int i = 0; i < a->pin_count; i++) {
        float top = y;
        if (!pin_live[i]) {
            char gone[LOG_LINE_LEN + 16];
            snprintf(gone, sizeof gone, "%s  (gone)", a->pins[i]);
            y += draw_view_line(c, f, x, y, gone, GRAPH_NONE, NULL) + GROUP;
        } else {
            for (int k = 0; k < pin_views[i].n; k++) {
                const ViewLine *l = &pin_views[i].line[k];
                GraphPlace place = a->pin_folded[i] ? GRAPH_NONE : l->place;
                y += draw_view_line(c, f, x, y, l->text, place, &l->graph)
                     + GROUP;
                if (a->pin_folded[i]) {
                    text_draw(c, f, (P2){x + rct_w(zone) - 6.0f * GROUP, y - row
                                                                         - GROUP},
                              ALIGN_LEFT_TOP, "folded", DIM_INK, 0.0f);
                    break;
                }
            }
        }
        /* a rule down the left marks what is pinned; a click lets it go */
        Rct block = rct(zone.x0, top, zone.x1, y - GROUP);
        Resp r = ui_interact(ui, ui_id_n("pin", i), block, 0.0f);
        if (r.hovered) ui->cursor = CURSOR_POINTER;
        if (r.clicked) let_go = i;
        draw_rect_filled(c, rct(zone.x0, top, zone.x0 + 2.0f, y - GROUP),
                         r.hovered ? PAPER : DIM_INK);
    }
    canvas_set_clip(c, saved);
    if (let_go >= 0) {
        push_log(a, "%s let go", a->pins[let_go]);
        console_unpin_at(a, let_go);
    }
}

void draw_console_drawer(App *a, Ui *ui, Rct footer) {
    Canvas *c = ui->canvas;
    FontId small = ui_font(11.0f);
    float row = text_row_height(small);
    float pinned_h = a->pin_count ? pins_height(a, row) : 0.0f;
    const LineState *state = line_now(a);
    float hint_h = hint_height(state, row);
    float grown = pinned_h + (pinned_h > 0.0f ? GAP : 0.0f)
                  + fmaxf(hint_h - HINT_ROW_H, 0.0f);
    float h = fminf(FOOTER_OPEN_H + grown,
                    floorf((float)c->h * (grown > 0.0f ? 0.7f : 0.5f)));
    Rct rect = rct(footer.x0, footer.y0 - h, footer.x1, footer.y0);
    draw_rect_filled(c, rect, INK_BLACK);
    draw_rect_filled(c, rct(rect.x0, rect.y0, rect.x1, rect.y0 + 1.0f), PAPER);
    draw_rect_filled(c, rct(rect.x0, rect.y0, rect.x0 + 1.0f, rect.y1), PAPER);
    draw_rect_filled(c, rct(rect.x1 - 1.0f, rect.y0, rect.x1, rect.y1), PAPER);

    char visible[LOG_LINE_LEN];
    console_visible(a, visible, sizeof visible);
    Rct inner = rct_shrink(rect, GAP);
    float body_h = fmaxf(rct_h(rect) - 2.0f * GAP - hint_h - GAP, 0.0f);
    if (pinned_h > 0.0f) {
        float zone_h = fminf(pinned_h, fmaxf(body_h - 3.0f * row, row));
        Rct zone = rct(inner.x0, inner.y0, inner.x1, inner.y0 + zone_h);
        draw_pins(a, ui, small, zone);
        for (float x = inner.x0; x < inner.x1; x += 3.0f)
            draw_rect_filled(c, rct_xywh(x, zone.y1 + 1.0f, 1.0f, 1.0f),
                             MARK_INK);
        inner.y0 = zone.y1 + GAP;
        body_h = fmaxf(body_h - zone_h - GAP, 0.0f);
        ui->repaint_soon = true;
    }
    Rct view = rct(inner.x0, inner.y0, inner.x1, inner.y0 + body_h);

    int full = a->log_len > 0 ? a->log_len - 1 : 0;
    int items = full + (visible[0] ? 1 : 0);
    float content_h = 0.0f;
    for (int j = 0; j < items; j++) {
        int idx = (a->log_head - (a->log_len - 1) + j + 2 * LOG_LINES)
                  % LOG_LINES;
        content_h += line_height(a->log_place[idx], row) + (j ? GROUP : 0.0f);
    }

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
        y += draw_view_line(c, small, view.x0, y, a->log[idx],
                            a->log_place[idx], &a->log_graph[idx])
             + GROUP;
    }
    if (visible[0])
        draw_view_line(c, small, view.x0, y, visible, a->log_place[a->log_head],
                       &a->log_graph[a->log_head]);
    canvas_set_clip(c, saved);

    float hy = inner.y0 + body_h + GAP;
    draw_hint(a, c, small, rct(inner.x0, hy, inner.x1, hy + hint_h), state);
}
