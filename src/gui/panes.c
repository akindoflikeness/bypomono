#include <SDL2/SDL_scancode.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

/* console Command ids follow the Rust COMMANDS roster order, 1-based:
   rec=1 save=2 overwrite=3 delete=4 rename=5 move=6 add=7 remove=8
   bind=9 unbind=10 report=11 — console.c must agree */
#define CMD_SAVE 2
#define CMD_DELETE 4

#define PRESET_DOUBLE_CLICK_S 1.0
#define ICON_SCALE 2.0f
#define REPORT_PANEL_W 420.0f
#define REPORT_DIR "design/reports"

static bool s_captured;
static bool s_have_shot;
static char s_shot_path[512];
static struct { char k[20]; char v[256]; } s_state[16];
static int s_state_n;
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
        "rec", "record", "save", "overwrite", "delete", "rename",
        "move", "add",    "bind", "unbind",    "remove", "report",
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
    char name[256];
    trimmed(a->preset_name.text, name, sizeof name);
    if (!name[0]) {
        push_log(a, "a preset needs a name. type one in the bar.");
        a->preset_armed = 0;
        a->preset_searching = true;
        a->preset_focus = true;
        (void)ui;
        return;
    }
    if (is_command_text(name)) {
        push_log(a, "that is a command — press enter to run it.");
        return;
    }
    char line[300];
    snprintf(line, sizeof line, "save %s", name);
    console_run_line(a, line);
}

static void preset_label_of(bool have, const PresetRef *p, char *out,
                            size_t cap) {
    if (!have) {
        snprintf(out, cap, "—");
    } else if (p->bank[0]) {
        snprintf(out, cap, "%s/%s", p->bank, p->name);
    } else {
        snprintf(out, cap, "%s", p->name);
    }
}

static void std_button_draw(Canvas *c, Rct r, const char *text, FontId f,
                            bool lit, bool hovered) {
    draw_rect_filled(c, r, lit ? PAPER : INK_BLACK);
    draw_rect_stroke(c, r, hovered ? 2.0f : 1.0f, PAPER);
    text_draw(c, f, rct_center(r), ALIGN_CENTER_CENTER, text,
              lit ? INK_BLACK : PAPER, 0.0f);
}

/* ---------- title bar + preset bar ---------- */

void draw_preset_bar(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    FontId f12 = ui_font(12.0f);
    float row12 = text_row_height(f12);
    float cy = 0.5f * (r.y0 + r.y1);
    float x = r.x1;
    UiId bar_id = ui_id("preset bar");

    {
        float w = roundf(text_width(f12, "▷", 0.0f) + 14.0f);
        float h = roundf(row12 + 8.0f);
        Rct br = rct(x - w, roundf(cy - h * 0.5f), x, roundf(cy - h * 0.5f) + h);
        if (pane_button(ui, ui_id("preset next"), br, "▷", false))
            preset_cycle(a, true);
        x = br.x0 - GROUP;
    }

    Rct bar = rct(roundf(x - 178.0f), roundf(cy - 10.5f), roundf(x),
                  roundf(cy - 10.5f) + 21.0f);
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
        if (ui->in.pressed && !rct_contains(bar, ui->in.mouse)) {
            if (ui->focus == bar_id) ui->focus = 0;
            a->preset_searching = false;
        }
        if (ui_text_edit(ui, bar_id, &a->preset_name)) a->preset_armed = 0;

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
            if (query_matches_any(a))
                a->preset_armed = 0;
            else
                save_from_bar(a, ui);
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
    x = bar.x0 - GROUP;

    {
        float w = roundf(text_width(f12, "◁", 0.0f) + 14.0f);
        float h = roundf(row12 + 8.0f);
        Rct br = rct(x - w, roundf(cy - h * 0.5f), x, roundf(cy - h * 0.5f) + h);
        if (pane_button(ui, ui_id("preset prev"), br, "◁", false))
            preset_cycle(a, false);
        x = br.x0 - GROUP;
    }

    float side = 9.0f * ICON_SCALE;
    float iw = side + 2.0f * GAP;
    float ih = side + 2.0f * GAP;
    {
        Rct ir = rct(roundf(x - iw), roundf(cy - ih * 0.5f), roundf(x),
                     roundf(cy - ih * 0.5f) + ih);
        if (icon_button(ui, ui_id("preset delete"), ir, ICON_DELETE, NULL,
                        a->preset_armed == CMD_DELETE, ICON_SCALE)) {
            if (!a->have_selected) {
                push_log(a, "no preset highlighted to delete.");
                a->preset_armed = 0;
            } else {
                console_run_line(a, "delete");
            }
        }
        x = ir.x0 - GROUP;
    }
    {
        Rct ir = rct(roundf(x - iw), roundf(cy - ih * 0.5f), roundf(x),
                     roundf(cy - ih * 0.5f) + ih);
        if (icon_button(ui, ui_id("preset save"), ir, ICON_SAVE, NULL,
                        a->preset_armed == CMD_SAVE, ICON_SCALE))
            save_from_bar(a, ui);
        x = ir.x0 - GROUP;
    }
    {
        Rct ir = rct(roundf(x - iw), roundf(cy - ih * 0.5f), roundf(x),
                     roundf(cy - ih * 0.5f) + ih);
        if (icon_button(ui, ui_id("preset folder"), ir, ICON_FOLDER, NULL,
                        false, ICON_SCALE)) {
            snprintf(a->console_input.text, sizeof a->console_input.text,
                     "add ");
            a->console_input.len = (int)strlen(a->console_input.text);
            a->console_open = true;
            a->console_focus = true;
            ui->focus = ui_id("console input");
            push_log(a, "name the bank, then enter.");
        }
    }
}

void draw_title_bar(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    FontId f12 = ui_font(12.0f);
    FontId f11 = ui_font(11.0f);
    FontId f26 = ui_font(26.0f);
    Rct content = rct(r.x0 + GROUP, r.y0 + GAP, r.x1 - GROUP, r.y1 - GAP);
    float cy = 0.5f * (content.y0 + content.y1);
    float x = content.x1;

    {
        const char *v = "v" APP_VERSION;
        float row = text_row_height(f12);
        float w = roundf(text_width(f12, v, 0.0f) + 2.0f * GAP);
        float h = roundf(row + 2.0f * TIGHT);
        Rct cr = rct(x - w, roundf(cy - h * 0.5f), x, roundf(cy - h * 0.5f) + h);
        draw_rect_stroke(c, cr, 1.0f, PAPER);
        text_draw(c, f12, rct_center(cr), ALIGN_CENTER_CENTER, v, PAPER, 0.0f);
        x = cr.x0 - GROUP;
    }

    {
        float w = roundf(text_width(f12, "info", 0.0f) + 2.0f * GAP);
        float h = 21.0f;
        Rct br = rct(x - w, roundf(cy - h * 0.5f), x, roundf(cy - h * 0.5f) + h);
        Resp resp = ui_interact(ui, ui_id("info tab"), br, 0.0f);
        std_button_draw(c, br, "info", f12, a->info_open, resp.hovered);
        if (resp.clicked) a->info_open = !a->info_open;
        x = br.x0 - GROUP;
    }

    {
        char elapsed[16];
        const char *label = NULL;
        if (a->recording && a->recorder.sample_rate > 0.0f) {
            uint32_t s =
                (uint32_t)((float)a->recorder.frames / a->recorder.sample_rate);
            snprintf(elapsed, sizeof elapsed, "%02u:%02u", s / 60, s % 60);
            label = elapsed;
        }
        float side = 9.0f;
        float lw = label ? GAP + text_width(f11, label, 0.0f) : 0.0f;
        float lh = label ? text_row_height(f11) : 0.0f;
        float w = roundf(side + lw + 2.0f * GAP);
        float h = roundf(fmaxf(side, lh) + 2.0f * GAP);
        Rct ir = rct(x - w, roundf(cy - h * 0.5f), x, roundf(cy - h * 0.5f) + h);
        if (!a->hosted) {
            if (icon_button(ui, ui_id("record button"), ir, ICON_RECORD, label,
                            a->recording, 1.0f))
                console_run_line(a, "rec");
            x = ir.x0 - GROUP;
        }
    }

    draw_preset_bar(a, ui, rct(content.x0, content.y0, x, content.y1));

    text_draw(c, f26, (P2){r.x0 + GAP, roundf(cy)}, ALIGN_LEFT_CENTER,
              "BLOW YOUR PHASE OFF", PAPER, 1.2f);
}

/* ---------- presets pane ---------- */

void draw_presets_pane(App *a, Ui *ui) {
    const float LIST_H = 230.0f;
    const float PANE_W = 340.0f;
    Canvas *c = ui->canvas;
    FontId f12 = ui_font(12.0f);
    FontId f11 = ui_font(11.0f);

    float pane_w = fmaxf(fminf(PANE_W, DESIGN_W - 2.0f * GROUP), GROUP);
    float pane_h = fmaxf(fminf(300.0f, DESIGN_H - GROUP), GROUP);
    Rct anchor = a->have_preset_bar_rect ? a->preset_bar_rect
                                         : rct(0, 0, DESIGN_W, DESIGN_H);
    float right_limit = fmaxf(DESIGN_W - pane_w - GROUP, GROUP);
    float top = a->header_rect.y1;
    float px = clampf(rct_center(anchor).x - pane_w * 0.5f, GROUP, right_limit);
    float py = fminf(top, fmaxf(DESIGN_H - pane_h, 0.0f));
    Rct pane = rct_xywh(roundf(px), roundf(py), pane_w, pane_h);

    draw_rect_filled(c, pane, INK_BLACK);
    draw_rect_stroke(c, pane, 2.0f, PAPER);
    Rct inner = rct_shrink(pane, GAP);

    char query[256];
    if (is_command_text(a->preset_name.text)) {
        query[0] = '\0';
    } else {
        trimmed(a->preset_name.text, query, sizeof query);
        for (char *q = query; *q; q++) *q = (char)tolower((unsigned char)*q);
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
        Rct bcol = rct(inner.x0, inner.y0, inner.x0 + 108.0f,
                       inner.y0 + LIST_H);
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
        float lx0 = inner.x0 + 108.0f + SECTION;
        float lw = fmaxf(rct_w(inner) - 108.0f - SECTION, 120.0f);
        Rct lcol = rct(lx0, inner.y0, lx0 + lw, inner.y0 + LIST_H);
        float rh = row12 + 8.0f;
        float content_h = (float)n * rh + (float)(n > 0 ? n - 1 : 0) * GROUP;
        float off = ui_scroll(ui, &a->preset_scroll, lcol, content_h);
        canvas_set_clip(c, rct_intersect(saved, lcol));
        float y = lcol.y0 - off;
        float cxm = 0.5f * (lcol.x0 + lcol.x1);
        for (int k = 0; k < n; k++) {
            const PresetRef *p = &a->preset_names[shown[k]];
            bool sel = a->have_selected
                       && strcmp(a->preset_selected.bank, p->bank) == 0
                       && strcmp(a->preset_selected.name, p->name) == 0;
            float w = roundf(text_width(f12, p->name, 0.0f) + 14.0f);
            Rct rr = rct(roundf(cxm - w * 0.5f), roundf(y),
                         roundf(cxm - w * 0.5f) + w, roundf(y) + rh);
            if (pane_button(ui, ui_id_n("preset row", shown[k]), rr, p->name,
                            sel)) {
                if (sel) {
                    PresetRef pick = *p;
                    preset_load(a, &pick);
                } else {
                    a->preset_selected = *p;
                    a->have_selected = true;
                    a->preset_armed = 0;
                }
            }
            y += rh + GROUP;
        }
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

    const char *labels[7] = {"audio out", "sample rate", "buffer", "dsp load",
                             "fps",       "version",     "midi in"};
    const char *values[7] = {
        "default",       rate_v, buffer_v,
        load_v,          fps_v,  "v" APP_VERSION,
        a->midi_open ? a->midi_port : "none"};

    float chrome_h = roundf(row12 + 2.0f * SNUG);
    float strip_h = chrome_h;
    float rows_h = 7.0f * 13.0f + 6.0f * GROUP;
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
    Rct pane = rct(roundf(DESIGN_W - GROUP - info_w), 34.0f,
                   roundf(DESIGN_W - GROUP), 34.0f + roundf(info_h));

    draw_rect_filled(c, pane, INK_BLACK);
    draw_rect_stroke(c, pane, 2.0f, PAPER);
    Rct inner = rct_shrink(pane, GAP);
    float y = inner.y0;

    Rct strip = rct(inner.x0, y, inner.x1, y + chrome_h);
    window_chrome_tagged(c, strip, "INFO", NULL);
    y += chrome_h + SECTION;

    for (int i = 0; i < 7; i++) {
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

/* ---------- report ---------- */

void write_ppm(const char *path, const Canvas *c) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "capture: write failed: %s\n", strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    bool ok = true;
    for (int i = 0; i < c->w * c->h && ok; i++) {
        uint32_t p = c->px[i];
        uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
        ok = fwrite(rgb, 1, 3, f) == 3;
    }
    if (fclose(f) != 0) ok = false;
    if (ok)
        fprintf(stderr, "capture: wrote %s\n", path);
    else
        fprintf(stderr, "capture: write failed: %s\n", strerror(errno));
}

static void state_row(const char *k, const char *v) {
    if (s_state_n >= (int)(sizeof s_state / sizeof s_state[0])) return;
    snprintf(s_state[s_state_n].k, sizeof s_state[s_state_n].k, "%s", k);
    snprintf(s_state[s_state_n].v, sizeof s_state[s_state_n].v, "%s", v);
    s_state_n++;
}

static void share_of(float s, char *out, size_t cap) {
    if (s < 0.0f)
        snprintf(out, cap, "house");
    else
        snprintf(out, cap, "%.4f", (double)s);
}

void report_arm(App *a) {
    if (a->report_open) return;
    a->report_open = true;
    s_captured = false;
}

void report_capture(App *a, Ui *ui) {
    snprintf(s_shot_path, sizeof s_shot_path, "/tmp/bypo-report-pending-%d.ppm",
             (int)getpid());
    write_ppm(s_shot_path, ui->canvas);
    struct stat st;
    s_have_shot = stat(s_shot_path, &st) == 0 && S_ISREG(st.st_mode);

    s_state_n = 0;
    char v[256];
    snprintf(v, sizeof v, "%d x %d px", ui->canvas->w, ui->canvas->h);
    state_row("WINDOW", v);
    share_of(a->splits.tree, v, sizeof v);
    state_row("SPLIT tree", v);
    share_of(a->splits.ops, v, sizeof v);
    state_row("SPLIT ops", v);
    share_of(a->splits.controls, v, sizeof v);
    state_row("SPLIT controls", v);
    share_of(a->splits.top, v, sizeof v);
    state_row("SPLIT top", v);
    state_row("SEARCH", a->preset_name.len > 0 ? a->preset_name.text : "—");
    preset_label_of(a->have_selected, &a->preset_selected, v, sizeof v);
    state_row("SELECTED", v);
    preset_label_of(a->have_loaded, &a->preset_loaded, v, sizeof v);
    state_row("LOADED", v);
    snprintf(v, sizeof v, "%.2f hz", (double)a->drone_hz);
    state_row("DRONE", v);
    state_row("ENGAGED", a->engaged ? "true" : "false");
    state_row("CONSOLE", a->console_open ? "true" : "false");
    state_row("BROWSER", a->presets_open ? "true" : "false");
    state_row("INFO", a->info_open ? "true" : "false");
    snprintf(v, sizeof v, "%.0f", (double)a->fps);
    state_row("FPS", v);
    state_row("VERSION", "v" APP_VERSION);

    a->report_notes[0] = '\0';
    a->report_notes_len = 0;
    a->report_err[0] = '\0';
    s_captured = true;
}

static void slug_of(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        out[n++] = isalnum(ch) ? (char)tolower(ch) : '-';
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == '-') out[--n] = '\0';
    size_t lead = 0;
    while (out[lead] == '-') lead++;
    if (lead) memmove(out, out + lead, n - lead + 1);
}

static void err_up(App *a, const char *path) {
    snprintf(a->report_err, sizeof a->report_err, "%s: %s", path,
             strerror(errno));
    for (char *p = a->report_err; *p; p++)
        *p = (char)toupper((unsigned char)*p);
}

static bool notes_empty(const App *a) {
    for (int i = 0; i < a->report_notes_len; i++)
        if (!isspace((unsigned char)a->report_notes[i])) return false;
    return true;
}

void report_save(App *a) {
    if (notes_empty(a)) {
        snprintf(a->report_err, sizeof a->report_err, "NOTHING WRITTEN");
        return;
    }
    struct stat st;
    const char *root = NULL;
    if (stat("crates/fibonacci-gui", &st) == 0 && S_ISDIR(st.st_mode))
        root = ".";
    else
        root = user_data_root();
    if (!root || !root[0]) {
        snprintf(a->report_err, sizeof a->report_err, "NOWHERE TO WRITE");
        return;
    }

    char place[64];
    if (s_have_shot) {
        snprintf(place, sizeof place, "bypo");
        for (int i = 0; i < s_state_n; i++) {
            if (strcmp(s_state[i].k, "WINDOW") != 0) continue;
            size_t n = 0;
            for (const char *p = s_state[i].v;
                 *p && n + 1 < sizeof place; p++) {
                if (*p == ' ') continue;
                if (strncmp(p, "px", 2) == 0 && p[2] == '\0') break;
                place[n++] = *p;
            }
            place[n] = '\0';
            if (!n) snprintf(place, sizeof place, "bypo");
        }
    } else {
        snprintf(place, sizeof place, "bypo");
    }

    char dir[600];
    snprintf(dir, sizeof dir, "%s/design", root);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { err_up(a, dir); return; }
    snprintf(dir, sizeof dir, "%s/" REPORT_DIR, root);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { err_up(a, dir); return; }

    char slug[64];
    slug_of(place, slug, sizeof slug);
    unsigned long long stamp = (unsigned long long)time(NULL);
    char base[128], md[800];
    snprintf(base, sizeof base, "%llu-%s", stamp, slug);
    snprintf(md, sizeof md, "%s/%s.md", dir, base);

    FILE *f = fopen(md, "w");
    if (!f) { err_up(a, md); return; }
    char place_up[64];
    snprintf(place_up, sizeof place_up, "%s", place);
    for (char *p = place_up; *p; p++) *p = (char)toupper((unsigned char)*p);
    fprintf(f, "# Report %llu · %s\n\n", stamp, place_up);
    fputs("**Scope: one observation, written while looking at it.** A record, "
          "not a\nruling and not a bug report — what it is is decided when it "
          "is read. The\nstate block below was captured with the shot, not "
          "typed. Owner: AKOL.\n\n",
          f);
    if (s_have_shot) {
        fprintf(f,
                "Shot: `%s.ppm` — the frame as it stood when the report was "
                "asked for.\n\n",
                base);
        fprintf(f, "Picture: `%s/%s.ppm`\n\n", dir, base);
    } else {
        fputs("Shot: none — the capture did not arrive. The note stands on "
              "its own.\n\n",
              f);
    }
    fputs("## The note\n\n", f);
    {
        int end = a->report_notes_len;
        while (end > 0 && isspace((unsigned char)a->report_notes[end - 1]))
            end--;
        fwrite(a->report_notes, 1, (size_t)end, f);
        fputc('\n', f);
    }
    fputs("\n## What was true at the shot\n\n| | |\n|---|---|\n", f);
    for (int i = 0; i < s_state_n; i++)
        fprintf(f, "| %s | %s |\n", s_state[i].k, s_state[i].v);
    if (ferror(f) || fclose(f) != 0) {
        err_up(a, md);
        return;
    }

    if (s_have_shot) {
        char to[800];
        snprintf(to, sizeof to, "%s/%s.ppm", dir, base);
        bool moved = rename(s_shot_path, to) == 0;
        if (!moved) {
            FILE *in = fopen(s_shot_path, "rb");
            FILE *out = in ? fopen(to, "wb") : NULL;
            if (in && out) {
                char buf[8192];
                size_t n;
                moved = true;
                while ((n = fread(buf, 1, sizeof buf, in)) > 0)
                    if (fwrite(buf, 1, n, out) != n) { moved = false; break; }
            }
            if (in) fclose(in);
            if (out && fclose(out) != 0) moved = false;
        }
        if (!moved) {
            FILE *rf = fopen(md, "r");
            if (rf) {
                char *body = malloc(1 << 20);
                size_t got = body ? fread(body, 1, (1 << 20) - 1, rf) : 0;
                fclose(rf);
                if (body) {
                    body[got] = '\0';
                    char from_tag[160];
                    snprintf(from_tag, sizeof from_tag, "Shot: `%s.ppm`",
                             base);
                    char *at = strstr(body, from_tag);
                    FILE *wf = at ? fopen(md, "w") : NULL;
                    if (wf) {
                        fwrite(body, 1, (size_t)(at - body), wf);
                        fputs("Shot: none — the capture could not be moved "
                              "beside it",
                              wf);
                        fputs(at + strlen(from_tag), wf);
                        fclose(wf);
                    }
                    free(body);
                }
            }
        }
    }

    push_log(a, "report: %s", md);
    a->report_open = false;
    a->report_err[0] = '\0';
    a->report_notes[0] = '\0';
    a->report_notes_len = 0;
    s_have_shot = false;
    s_captured = false;
}

void report_discard(App *a) {
    if (s_have_shot) remove(s_shot_path);
    s_have_shot = false;
    s_captured = false;
    a->report_open = false;
    a->report_err[0] = '\0';
    a->report_notes[0] = '\0';
    a->report_notes_len = 0;
}

void draw_report_panel(App *a, Ui *ui) {
    Canvas *c = ui->canvas;
    if (!s_captured) report_capture(a, ui);

    FontId f12 = ui_font(12.0f);
    FontId f11 = ui_font(11.0f);
    float row11 = text_row_height(f11);
    float row12 = text_row_height(f12);
    UiId notes_id = ui_id("report notes");

    float w = fmaxf(fminf(REPORT_PANEL_W, DESIGN_W - 2.0f * GROUP), GROUP);
    float chrome_h = roundf(row12 + 2.0f * SNUG);
    float note_h = roundf(6.0f * row11 + 4.0f);
    float err_h = a->report_err[0] ? row11 + SECTION : 0.0f;
    float content_h =
        chrome_h + SECTION + row11 + SECTION + note_h + SECTION + err_h + 21.0f;
    float h = content_h + 2.0f * GAP;
    Rct pane = rct_xywh(roundf((DESIGN_W - w) * 0.5f),
                        roundf((DESIGN_H - h) * 0.5f), roundf(w), roundf(h));

    draw_rect_filled(c, pane, INK_BLACK);
    draw_rect_stroke(c, pane, 2.0f, PAPER);
    Rct inner = rct_shrink(pane, GAP);
    float y = inner.y0;

    window_chrome_tagged(c, rct(inner.x0, y, inner.x1, y + chrome_h), "REPORT",
                         NULL);
    y += chrome_h + SECTION;

    text_draw(c, f11, (P2){inner.x0, roundf(y)}, ALIGN_LEFT_TOP,
              "what you saw, and what you expected instead", PAPER, 0.0f);
    y += row11 + SECTION;

    Rct area = rct(inner.x0, roundf(y), inner.x1, roundf(y) + note_h);
    Resp ar = ui_interact(ui, notes_id, area, 0.0f);
    if (ar.hovered) ui->cursor = CURSOR_TEXT;
    if (ar.clicked) ui->focus = notes_id;
    if (ui->in.pressed && !rct_contains(area, ui->in.mouse)
        && ui->focus == notes_id)
        ui->focus = 0;

    if (ui->focus == notes_id) {
        for (const char *p = ui->in.text; *p; p++) {
            if (a->report_notes_len + 1 < (int)sizeof a->report_notes)
                a->report_notes[a->report_notes_len++] = *p;
        }
        if (ui->in.key_pressed[SDL_SCANCODE_RETURN]
            && a->report_notes_len + 1 < (int)sizeof a->report_notes)
            a->report_notes[a->report_notes_len++] = '\n';
        if (ui->in.backspace_repeat && a->report_notes_len > 0) {
            do {
                a->report_notes_len--;
            } while (a->report_notes_len > 0
                     && ((unsigned char)a->report_notes[a->report_notes_len]
                         & 0xC0)
                            == 0x80);
        }
        a->report_notes[a->report_notes_len] = '\0';
    }
    a->report_notes[a->report_notes_len] = '\0';

    draw_rect_stroke(c, area, ar.hovered || ui->focus == notes_id ? 2.0f : 1.0f,
                     PAPER);
    Rct text_area = rct(area.x0 + 4.0f, area.y0 + 2.0f, area.x1 - 4.0f,
                        area.y1 - 2.0f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, rct_intersect(saved, area));
    float used_h =
        text_draw_wrapped(c, f11, text_area, a->report_notes, PAPER);
    if (ui->focus == notes_id) {
        const char *tail = a->report_notes;
        const char *nl = strrchr(a->report_notes, '\n');
        if (nl) tail = nl + 1;
        while (*tail
               && text_width(f11, tail, 0.0f) > rct_w(text_area))
            tail++;
        float ly = text_area.y0 + fmaxf(used_h - row11, 0.0f);
        draw_block_caret(c, ui, f11, (P2){text_area.x0, roundf(ly)}, tail,
                         INK_BLACK);
    }
    canvas_set_clip(c, saved);
    y += note_h + SECTION;

    if (a->report_err[0]) {
        text_draw(c, f11, (P2){inner.x0, roundf(y)}, ALIGN_LEFT_TOP,
                  a->report_err, PAPER, 0.0f);
        y += row11 + SECTION;
    }

    bool save = false, discard = false;
    {
        float bw = roundf(text_width(f11, "SAVE", 0.0f) + 2.0f * GAP);
        Rct br = rct(inner.x0, roundf(y), inner.x0 + bw, roundf(y) + 21.0f);
        Resp resp = ui_interact(ui, ui_id("report save"), br, 0.0f);
        std_button_draw(c, br, "SAVE", f11, false, resp.hovered);
        save = resp.clicked;
        float dw = roundf(text_width(f11, "DISCARD", 0.0f) + 2.0f * GAP);
        Rct dr = rct(br.x1 + GROUP, br.y0, br.x1 + GROUP + dw, br.y1);
        resp = ui_interact(ui, ui_id("report discard"), dr, 0.0f);
        std_button_draw(c, dr, "DISCARD", f11, false, resp.hovered);
        discard = resp.clicked;
    }

    if (save)
        report_save(a);
    else if (discard)
        report_discard(a);
}
