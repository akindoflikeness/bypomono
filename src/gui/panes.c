#include <SDL2/SDL_scancode.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#define PRESET_DOUBLE_CLICK_S 1.0
#define PRESET_ROW_GAP GROUP

static UiScroll s_bank_scroll, s_midi_scroll;

/* ---------- shared helpers ---------- */

static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]
               && tolower((unsigned char)hay[i])
                      == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return true;
    }
    return false;
}

static void trimmed(const char *raw, char *out, size_t cap) {
    while (*raw && isspace((unsigned char)*raw)) raw++;
    size_t n = strlen(raw);
    while (n > 0 && isspace((unsigned char)raw[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
}

static bool is_command_text(const char *raw) {
    static const char *const NAMES[] = {
        "rec",    "record", "save",   "ow",     "overwrite", "rm",
        "delete", "mv",     "rename", "move",   "mkdir",     "add",
        "rmdir",  "bind",   "unbind", "remove", "undo",      "help",
    };
    char t[256];
    trimmed(raw, t, sizeof t);
    if (!t[0]) return false;
    if (strcmp(t, "?") == 0) return true;
    char head[64];
    size_t i = 0;
    while (t[i] && !isspace((unsigned char)t[i]) && i < sizeof head - 1) {
        head[i] = (char)tolower((unsigned char)t[i]);
        i++;
    }
    head[i] = '\0';
    for (size_t k = 0; k < sizeof NAMES / sizeof NAMES[0]; k++)
        if (strcmp(head, NAMES[k]) == 0) return true;
    return false;
}

static bool refs_same(const PresetRef *a, const PresetRef *b) {
    return strcmp(a->bank, b->bank) == 0 && strcmp(a->name, b->name) == 0;
}

static bool filter_matches(const PresetFilter *f, const PresetRef *p) {
    switch (f->kind) {
    case FILTER_ALL: return true;
    case FILTER_MINE: return strcmp(p->bank, STOCK_BANK) != 0;
    case FILTER_BANK: return strcmp(p->bank, f->bank) == 0;
    }
    return true;
}

static bool query_matches_any(App *a) {
    char q[256];
    trimmed(a->preset_name.text, q, sizeof q);
    for (char *c = q; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!q[0] || is_command_text(a->preset_name.text)) return false;
    for (int i = 0; i < a->preset_count; i++) {
        const PresetRef *p = &a->preset_names[i];
        if (filter_matches(&a->preset_filter, p) && ci_contains(p->name, q))
            return true;
    }
    return false;
}

static void save_from_bar(App *a, Ui *ui) {
    char typed[256], name[256];
    trimmed(a->preset_name.text, typed, sizeof typed);
    sanitise_segment(typed, name, sizeof name);
    if (!name[0]) {
        push_log(a, "a preset needs a name. type one in the bar.");
        a->preset_searching = true;
        a->preset_focus = true;
        (void)ui;
        return;
    }
    if (is_command_text(typed)) {
        push_log(a, "that is a command — press enter to run it.");
        return;
    }
    char line[300];
    snprintf(line, sizeof line, "save %s", name);
    console_run_line(a, line);
}

static void std_button_draw(Canvas *c, Rct r, const char *text, FontId f,
                            bool lit, bool hovered) {
    draw_rect_filled(c, r, lit ? PAPER : INK_BLACK);
    draw_rect_stroke(c, r, hovered ? 2.0f : 1.0f, PAPER);
    text_draw(c, f, rct_center(r), ALIGN_CENTER_CENTER, text,
              lit ? INK_BLACK : PAPER, 0.0f);
}

/* ---------- keyboard walk ---------- */

/* compact-bar buttons in left-to-right screen order */
enum { PB_PREV, PB_NEXT };

static void focus_ring(Canvas *c, Rct r) {
    draw_rect_stroke(c, rct_expand(r, 2.0f), 1.0f, PAPER);
}

/* true when the walk sits on button k and Return was pressed this frame */
static bool key_hit(const App *a, const Ui *ui, int k) {
    return ui->focus == ui_id("preset buttons") && a->preset_button_at == k
           && ui->in.key_pressed[SDL_SCANCODE_RETURN];
}

static void focus_search(App *a, Ui *ui) {
    a->preset_searching = true;
    a->preset_focus = true;
    ui->focus = ui_id("preset bar");
}

void presets_walk_keys(App *a, Ui *ui) {
    UiId bar_id = ui_id("preset bar");
    UiId list_id = ui_id("preset list");
    UiId buttons_id = ui_id("preset buttons");
    bool walking = ui->focus == bar_id || ui->focus == list_id
                   || ui->focus == buttons_id;
    if (ui->in.key_pressed[SDL_SCANCODE_TAB]
        && (walking || (a->presets_open && ui->focus == 0))) {
        if (ui->focus == bar_id) {
            ui->focus = list_id;
            a->presets_open = true;
        } else if (ui->focus == list_id) {
            ui->focus = buttons_id;
        } else {
            focus_search(a, ui);
        }
    }
    if (ui->focus == buttons_id) {
        if (ui->in.key_pressed[SDL_SCANCODE_LEFT] && a->preset_button_at > 0)
            a->preset_button_at--;
        if (ui->in.key_pressed[SDL_SCANCODE_RIGHT]
            && a->preset_button_at < PRESET_BUTTONS - 1)
            a->preset_button_at++;
    }
    if (ui->focus == list_id && !a->presets_open) ui->focus = 0;
}

/* ---------- preset bar ---------- */

static float preset_nav_width(FontId f) {
    float arrows = fmaxf(text_width(f, "◁", 0.0f), text_width(f, "▷", 0.0f));
    return roundf(arrows + 14.0f);
}

typedef struct {
    Rct info, previous, name, next;
} PresetBarLayout;

/* Keep the compact controls centred as one named group.  Drawing code below
   uses these rectangles directly, so adding or reordering controls does not
   depend on a right-edge cursor and reverse placement. */
static PresetBarLayout preset_bar_layout(Rct area, FontId f, float cy) {
    float nav_w = preset_nav_width(f);
    float nav_h = roundf(text_row_height(f) + 8.0f);
    float group_w = INFO_BUTTON_W + 3.0f * GROUP + 2.0f * nav_w
                    + PRESET_NAME_W;
    float x = roundf(rct_center(area).x - 0.5f * group_w);
    PresetBarLayout layout = {
        .info = rct_xywh(x, roundf(cy - 0.5f * INFO_BUTTON_W), INFO_BUTTON_W,
                          INFO_BUTTON_W),
    };
    x = layout.info.x1 + GROUP;
    layout.previous = rct_xywh(x, roundf(cy - 0.5f * nav_h), nav_w, nav_h);
    x = layout.previous.x1 + GROUP;
    layout.name = rct_xywh(x, roundf(cy - 10.5f), PRESET_NAME_W, 21.0f);
    x = layout.name.x1 + GROUP;
    layout.next = rct_xywh(x, roundf(cy - 0.5f * nav_h), nav_w, nav_h);
    return layout;
}

void draw_preset_bar(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    FontId f12 = ui_font(12.0f);
    float row12 = text_row_height(f12);
    float cy = 0.7f * (r.y0 + r.y1);
    PresetBarLayout layout = preset_bar_layout(r, f12, cy);
    UiId bar_id = ui_id("preset bar");
    UiId buttons_id = ui_id("preset buttons");
    bool on_buttons = ui->focus == buttons_id;

    if (icon_button(ui, ui_id("info tab"), layout.info, ICON_COG, NULL, a->info_open,
                    1.0f))
        a->info_open = !a->info_open;
    {
        if (pane_button(ui, ui_id("preset next"), layout.next, "▷", false)
            || key_hit(a, ui, PB_NEXT))
            preset_cycle(a, true);
        if (on_buttons && a->preset_button_at == PB_NEXT) focus_ring(c, layout.next);
    }

    Rct bar = layout.name;
    a->preset_bar_rect = bar;
    a->have_preset_bar_rect = true;

    if (a->preset_searching) {
        if (a->preset_focus) {
            ui->focus = bar_id;
            a->preset_focus = false;
        }
        Resp fr = ui_interact(ui, bar_id, bar, 0.0f);
        if (fr.hovered) ui->cursor = CURSOR_TEXT;
        if (fr.clicked) ui->focus = bar_id;
        /* a press in the pane keeps the query, so the row clicked is the row
           that was showing */
        bool in_pane = a->presets_open && a->have_presets_pane_rect
                       && rct_contains(a->presets_pane_rect, ui->in.mouse);
        if (ui->in.pressed && !rct_contains(bar, ui->in.mouse) && !in_pane) {
            if (ui->focus == bar_id) ui->focus = 0;
            a->preset_searching = false;
        }
        if (ui->focus == bar_id) {
            /* "/" is the console's key and never part of a query */
            char *w = ui->in.text;
            for (const char *p = ui->in.text; *p; p++)
                if (*p != '/') *w++ = *p;
            *w = '\0';
        }
        ui_text_edit(ui, bar_id, &a->preset_name);
        draw_rect_filled(c, bar, INK_BLACK);
        draw_rect_stroke(c, bar, fr.hovered ? 2.0f : 1.0f, PAPER);
        Rct saved = canvas_clip(c);
        canvas_set_clip(c, rct_intersect(saved, bar));
        P2 tp = {bar.x0 + 4.0f, roundf(cy - row12 * 0.5f)};
        if (a->preset_name.len > 0)
            text_draw(c, f12, tp, ALIGN_LEFT_TOP, a->preset_name.text, PAPER,
                      0.0f);
        else
            text_draw(c, f12, tp, ALIGN_LEFT_TOP, "search", PAPER, 0.0f);
        if (ui->focus == bar_id)
            draw_block_caret(c, ui, f12, tp,
                             a->preset_name.len > 0 ? a->preset_name.text : "",
                             INK_BLACK);
        canvas_set_clip(c, saved);

        if (ui->focus == bar_id && ui->in.key_pressed[SDL_SCANCODE_RETURN]) {
            if (is_command_text(a->preset_name.text)) {
                /* a verb typed here runs as if it were in the console */
                char line[256];
                trimmed(a->preset_name.text, line, sizeof line);
                a->preset_name.len = 0;
                a->preset_name.text[0] = '\0';
                console_run_line(a, line);
            } else if (!query_matches_any(a)) {
                save_from_bar(a, ui);
            }
        }
        if (!a->preset_searching && ui->focus == bar_id) ui->focus = 0;
    } else {
        if (ui->focus == bar_id) ui->focus = 0;
        char label[256];
        if (a->have_loaded)
            preset_qualified(&a->preset_loaded, label, sizeof label);
        else
            snprintf(label, sizeof label, "presets");
        bool open = a->presets_open;
        Resp br = ui_interact(ui, bar_id, bar, 0.0f);
        draw_rect_filled(c, bar, open ? PAPER : INK_BLACK);
        draw_rect_stroke(c, bar, br.hovered ? 2.0f : 1.0f, PAPER);
        Rct saved = canvas_clip(c);
        canvas_set_clip(c, rct_intersect(saved, bar));
        text_draw(c, f12, rct_center(bar), ALIGN_CENTER_CENTER, label,
                  open ? INK_BLACK : PAPER, 0.0f);
        canvas_set_clip(c, saved);
        if (br.clicked) {
            double now = ui->time;
            bool paired = a->have_click_at
                          && now - a->preset_click_at <= PRESET_DOUBLE_CLICK_S;
            if (paired) {
                a->preset_name.len = 0;
                a->preset_name.text[0] = '\0';
                a->preset_searching = true;
                a->preset_focus = true;
                a->presets_open = true;
                a->have_click_at = false;
            } else {
                a->presets_open = !a->presets_open;
                a->preset_click_at = now;
                a->have_click_at = true;
            }
        }
    }
    {
        if (pane_button(ui, ui_id("preset prev"), layout.previous, "◁", false)
            || key_hit(a, ui, PB_PREV))
            preset_cycle(a, false);
        if (on_buttons && a->preset_button_at == PB_PREV)
            focus_ring(c, layout.previous);
    }
}

/* ---------- presets pane ---------- */

void draw_presets_pane(App *a, Ui *ui) {
    const float PANE_W = 340.0f;
    Canvas *c = ui->canvas;
    FontId f12 = ui_font(12.0f);
    FontId f11 = ui_font(11.0f);

    float pane_w = fmaxf(fminf(PANE_W, DESIGN_W - 2.0f * GROUP), GROUP);
    float pane_h = fmaxf(fminf(300.0f, DESIGN_H - GROUP), GROUP);
    Rct anchor = a->have_preset_bar_rect ? a->preset_bar_rect
                                         : rct(0, 0, DESIGN_W, DESIGN_H);
    float right_limit = fmaxf(DESIGN_W - pane_w - GROUP, GROUP);
    float px = clampf(rct_center(anchor).x - pane_w * 0.5f, GROUP, right_limit);
    float pane_top_limit = fmaxf(DESIGN_H - FOOTER_LINE_H - pane_h, 0.0f);
    float py = clampf(anchor.y1 + GROUP, 0.0f, pane_top_limit);
    Rct pane = rct_xywh(roundf(px), roundf(py), pane_w, pane_h);

    draw_rect_filled(c, pane, INK_BLACK);
    draw_rect_stroke(c, pane, 2.0f, PAPER);
    Rct inner = rct_shrink(pane, GAP);
    a->presets_pane_rect = pane;
    a->have_presets_pane_rect = true;

    char query[256];
    if (is_command_text(a->preset_name.text)) {
        query[0] = '\0';
    } else {
        trimmed(a->preset_name.text, query, sizeof query);
        for (char *q = query; *q; q++) {
            if (isspace((unsigned char)*q))
                *q = '_';
            else
                *q = (char)tolower((unsigned char)*q);
        }
    }

    int shown[MAX_PRESETS];
    int n = 0;
    for (int i = 0; i < a->preset_count; i++) {
        const PresetRef *p = &a->preset_names[i];
        if (!filter_matches(&a->preset_filter, p)) continue;
        if (query[0] && !ci_contains(p->name, query)) continue;
        shown[n++] = i;
    }

    float row11 = text_row_height(f11);
    float row12 = text_row_height(f12);
    Rct saved = canvas_clip(c);

    {
        Rct bcol = rct(inner.x0, inner.y0, inner.x0 + 108.0f, inner.y1);
        float bh = fmaxf(row11 + 2.0f * GAP, 21.0f);
        int nb = 2 + a->folder_count;
        float content_h = (float)nb * bh + (float)(nb - 1) * GROUP;
        float off = ui_scroll(ui, &s_bank_scroll, bcol, content_h);
        canvas_set_clip(c, rct_intersect(saved, bcol));
        float y = bcol.y0 - off;
        for (int i = 0; i < nb; i++) {
            const char *label;
            PresetFilter want = {FILTER_ALL, ""};
            if (i == 0) {
                label = "ALL";
            } else if (i == 1) {
                label = MINE_BANK;
                want.kind = FILTER_MINE;
            } else {
                label = a->preset_folders[i - 2];
                want.kind = FILTER_BANK;
                snprintf(want.bank, sizeof want.bank, "%s", label);
            }
            bool sel = a->preset_filter.kind == want.kind
                       && (want.kind != FILTER_BANK
                           || strcmp(a->preset_filter.bank, want.bank) == 0);
            float w = roundf(text_width(f11, label, 0.0f) + 2.0f * GROUP
                             + SNUG);
            Rct br = rct(bcol.x0, roundf(y), bcol.x0 + w, roundf(y) + bh);
            if (bookmark(ui, ui_id_n("preset bank", i), br, label, sel))
                a->preset_filter = want;
            y += bh + GROUP;
        }
        canvas_set_clip(c, saved);
    }

    {
        UiId list_id = ui_id("preset list");
        float lx0 = inner.x0 + 108.0f + SECTION;
        float lw = fmaxf(rct_w(inner) - 108.0f - SECTION, 120.0f);
        Rct lcol = rct(lx0, inner.y0, lx0 + lw, inner.y1);
        float rh = row12 + 8.0f;
        float pitch = rh + PRESET_ROW_GAP;
        float content_h = n > 0 ? (float)n * pitch - PRESET_ROW_GAP : 0.0f;
        float view_h = rct_h(lcol);

        int at = -1;
        for (int k = 0; k < n && at < 0; k++)
            if (a->have_selected
                && refs_same(&a->preset_selected, &a->preset_names[shown[k]]))
                at = k;

        /* keyboard: Up/Down walk the highlight, Return loads it, Page and
           Home/End move the view; the highlight is kept in sight */
        bool focused = ui->focus == list_id;
        int want = at;
        if (focused && n > 0) {
            const bool *k = ui->in.key_pressed;
            if (k[SDL_SCANCODE_DOWN]) want = at < 0 ? 0 : (at + 1 < n ? at + 1 : at);
            if (k[SDL_SCANCODE_UP]) want = at < 0 ? n - 1 : (at > 0 ? at - 1 : at);
            if (k[SDL_SCANCODE_HOME]) want = 0;
            if (k[SDL_SCANCODE_END]) want = n - 1;
            if (k[SDL_SCANCODE_PAGEDOWN]) a->preset_scroll.offset += view_h;
            if (k[SDL_SCANCODE_PAGEUP]) a->preset_scroll.offset -= view_h;
            if (want != at) {
                a->preset_selected = a->preset_names[shown[want]];
                a->have_selected = true;
                at = want;
                float top = (float)at * pitch, bottom = top + rh;
                if (top < a->preset_scroll.offset)
                    a->preset_scroll.offset = top;
                else if (bottom > a->preset_scroll.offset + view_h)
                    a->preset_scroll.offset = bottom - view_h;
            } else if (k[SDL_SCANCODE_RETURN] && at >= 0) {
                PresetRef pick = a->preset_selected;
                preset_load(a, &pick);
            }
        }
        float off = ui_scroll(ui, &a->preset_scroll, lcol, content_h);
        canvas_set_clip(c, rct_intersect(saved, lcol));
        float y = lcol.y0 - off;
        float cxm = 0.5f * (lcol.x0 + lcol.x1);
        for (int k = 0; k < n; k++) {
            const PresetRef *p = &a->preset_names[shown[k]];
            /* rows outside the view are neither drawn nor clickable */
            if (y + rh < lcol.y0 || y > lcol.y1) {
                y += pitch;
                continue;
            }
            bool sel = k == at;
            float w = roundf(text_width(f12, p->name, 0.0f) + 14.0f);
            Rct rr = rct(roundf(cxm - w * 0.5f), roundf(y),
                         roundf(cxm - w * 0.5f) + w, roundf(y) + rh);
            if (pane_button(ui, ui_id_n("preset row", shown[k]), rr, p->name,
                            sel)) {
                ui->focus = list_id;
                if (sel) {
                    PresetRef pick = *p;
                    preset_load(a, &pick);
                } else {
                    a->preset_selected = *p;
                    a->have_selected = true;
                }
            }
            y += pitch;
        }
        if (focused) focus_ring(c, rct_shrink(lcol, 1.0f));
        canvas_set_clip(c, saved);
    }
}

/* ---------- info pane ---------- */

static void set_midi_port_ui(App *a, const char *name) {
    bool had = a->midi_open;
    char was[128];
    snprintf(was, sizeof was, "%s", a->midi_port);
    bool ok = gui_set_midi_port(a, name);
    if (had) {
        push_log(a, "midi in closed: %s.", was);
        push_log(a, "pitch is free again. the drone knob drives it.");
    }
    if (name) {
        if (ok) {
            push_log(a, "midi in open: %s.", name);
            push_log(a,
                     "pitch is quantised to the keyboard. the drone knob is "
                     "idle.");
        } else {
            push_log(a, "midi port would not open.");
        }
    }
}

void draw_info_pane(App *a, Ui *ui) {
    Canvas *c = ui->canvas;
    FontId f11 = ui_font(11.0f);
    FontId f12 = ui_font(12.0f);
    float row11 = text_row_height(f11);
    float row12 = text_row_height(f12);

    char names[64][128];
    int nports = midi_port_names(names, 64);

    uint32_t frames =
        atomic_load_explicit(&a->meter.frames, memory_order_relaxed);
    float load = (float)atomic_load_explicit(&a->meter.load_q16,
                                             memory_order_relaxed)
                 / 65536.0f;
    float peak = (float)atomic_load_explicit(&a->meter.peak_q16,
                                             memory_order_relaxed)
                 / 65536.0f;

    char buffer_v[64], rate_v[32], load_v[64], fps_v[16];
    if (frames == 0)
        snprintf(buffer_v, sizeof buffer_v, "waiting");
    else
        snprintf(buffer_v, sizeof buffer_v, "%u fr / %.1f ms", frames,
                 (double)((float)frames * 1000.0f / a->sample_rate));
    snprintf(rate_v, sizeof rate_v, "%.0f hz", (double)a->sample_rate);
    snprintf(load_v, sizeof load_v, "%.0f%%   peak %.0f%%",
             (double)(load * 100.0f), (double)(peak * 100.0f));
    snprintf(fps_v, sizeof fps_v, "%.0f", (double)a->fps);

    const char *labels[6] = {"audio out", "sample rate", "buffer",
                             "dsp load",  "fps",         "midi in"};
    const char *values[6] = {"default", rate_v, buffer_v, load_v, fps_v,
                             a->midi_open ? a->midi_port : "none"};

    float chrome_h = roundf(row12 + 2.0f * SNUG);
    float strip_h = chrome_h;
    float rows_h = 6.0f * 13.0f + 5.0f * GROUP;
    float ports_h;
    if (nports == 0)
        ports_h = row11;
    else {
        int rows = nports + 1;
        ports_h = fminf((float)rows * 21.0f + (float)(rows - 1) * GROUP,
                        89.0f);
    }
    float info_w = fmaxf(fminf(340.0f, DESIGN_W - 2.0f * GROUP), GROUP);
    float info_h = GAP + chrome_h + SECTION + rows_h + SECTION + 21.0f + 16.0f
                   + strip_h + SECTION + ports_h + GAP;
    Rct pane = rct(roundf(DESIGN_W - GROUP - info_w), GROUP,
                   roundf(DESIGN_W - GROUP), GROUP + roundf(info_h));

    draw_rect_filled(c, pane, INK_BLACK);
    draw_rect_stroke(c, pane, 2.0f, PAPER);
    Rct inner = rct_shrink(pane, GAP);
    float y = inner.y0;

    Rct strip = rct(inner.x0, y, inner.x1, y + chrome_h);
    window_chrome_tagged(c, strip, "INFO", NULL);
    y += chrome_h + SECTION;

    for (int i = 0; i < 6; i++) {
        float rcy = roundf(y + 6.5f);
        text_draw(c, f11, (P2){inner.x0 + 44.5f, rcy}, ALIGN_CENTER_CENTER,
                  labels[i], PAPER, 0.0f);
        text_draw(c, f11, (P2){inner.x0 + 89.0f + GROUP, rcy},
                  ALIGN_LEFT_CENTER, values[i], PAPER, 0.0f);
        y += 13.0f + GROUP;
    }
    y += SECTION - GROUP;

    {
        Rct br = rct(inner.x0, roundf(y), inner.x1, roundf(y) + 21.0f);
        Resp resp = ui_interact(ui, ui_id("fps toggle"), br, 0.0f);
        std_button_draw(c, br, "FPS COUNTER", f11, a->show_fps, resp.hovered);
        if (resp.clicked) a->show_fps = !a->show_fps;
        y += 21.0f + 16.0f;
    }

    inverted_strip(c, rct(inner.x0, roundf(y), inner.x1, roundf(y) + strip_h),
                   "MIDI IN");
    y += strip_h + SECTION;

    if (nports == 0) {
        text_draw(c, f11, (P2){inner.x0, roundf(y)}, ALIGN_LEFT_TOP,
                  "the system offers no midi inputs.", PAPER, 0.0f);
    } else {
        Rct view = rct(inner.x0, roundf(y), inner.x1, roundf(y) + ports_h);
        int rows = nports + 1;
        float content_h = (float)rows * 21.0f + (float)(rows - 1) * GROUP;
        float off = ui_scroll(ui, &s_midi_scroll, view, content_h);
        Rct saved = canvas_clip(c);
        canvas_set_clip(c, rct_intersect(saved, view));
        float ry = view.y0 - off;
        int choose = -2; /* -2 none picked, -1 close, >=0 open index */
        for (int i = 0; i < rows; i++) {
            const char *name = i == 0 ? "none" : names[i - 1];
            bool open = i == 0 ? !a->midi_open
                               : (a->midi_open
                                  && strcmp(a->midi_port, name) == 0);
            Rct br = rct(view.x0, roundf(ry), view.x1, roundf(ry) + 21.0f);
            Resp resp = ui_interact(ui, ui_id_n("midi port", i), br, 0.0f);
            std_button_draw(c, br, name, f11, open, resp.hovered);
            if (resp.clicked) choose = (i == 0 || open) ? -1 : i - 1;
            ry += 21.0f + GROUP;
        }
        canvas_set_clip(c, saved);
        if (choose == -1)
            set_midi_port_ui(a, NULL);
        else if (choose >= 0)
            set_midi_port_ui(a, names[choose]);
    }
}

/* ---------- fps counter ---------- */

void draw_fps_counter(App *a, Ui *ui, float footer_h) {
    Canvas *c = ui->canvas;
    FontId f11 = ui_font(11.0f);
    char text[24];
    snprintf(text, sizeof text, "%.0f fps", (double)a->fps);
    float w = roundf(text_width(f11, text, 0.0f) + 2.0f * GAP);
    float h = roundf(text_row_height(f11) + 2.0f * GAP);
    Rct box = rct(DESIGN_W - GROUP - w, DESIGN_H - GROUP - footer_h - h,
                  DESIGN_W - GROUP, DESIGN_H - GROUP - footer_h);
    draw_rect_filled(c, box, INK_BLACK);
    draw_rect_stroke(c, box, 1.0f, PAPER);
    text_draw(c, f11, rct_center(box), ALIGN_CENTER_CENTER, text, PAPER, 0.0f);
}
