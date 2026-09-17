#include <SDL2/SDL_scancode.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

static float min_share(float axis) {
    float s = SEAM_GRAB / axis;
    return s < 1.0f / 3.0f ? s : 1.0f / 3.0f;
}

typedef struct {
    bool moved;
    float delta;
    bool reset;
} SeamAct;

static SeamAct seam(Ui *ui, const char *id, Rct grab, bool upright) {
    SeamAct act = {0};
    Resp r = ui_interact_drag(ui, ui_id(id), grab, 0.0f);
    if (r.hovered)
        ui->cursor = upright ? CURSOR_RESIZE_H : CURSOR_RESIZE_V;
    if (r.hovered || r.dragged) {
        /* 1px PAPER line down the middle */
        if (upright) {
            float x = 0.5f * (grab.x0 + grab.x1);
            draw_rect_filled(ui->canvas, rct(x, grab.y0, x + 1.0f, grab.y1),
                             PAPER);
        } else {
            float y = 0.5f * (grab.y0 + grab.y1);
            draw_rect_filled(ui->canvas, rct(grab.x0, y, grab.x1, y + 1.0f),
                             PAPER);
        }
    }
    if (r.dragged) {
        act.moved = true;
        act.delta = upright ? r.drag_delta.x : r.drag_delta.y;
    }
    act.reset = r.double_clicked;
    return act;
}

static void handle_keys(App *a, Ui *ui) {
    /* any key dismisses the splash */
    bool any_key = ui->in.text[0] != '\0';
    for (int i = 0; i < 512 && !any_key; i++)
        if (ui->in.key_pressed[i]) any_key = true;
    if (!a->splash_over && (any_key || ui->in.pressed)) a->splash_over = true;

    UiId bar_id = ui_id("preset bar");
    UiId list_id = ui_id("preset list");
    UiId buttons_id = ui_id("preset buttons");
    UiId console_id = ui_id("console input");

    /* "/" opens the console when no field is focused */
    if (strcmp(ui->in.text, "/") == 0 && ui->focus != bar_id
        && ui->focus != console_id) {
        a->console_open = true;
        ui->focus = console_id;
        ui->in.text[0] = '\0'; /* swallow it */
    }

    if (ui->in.key_pressed[SDL_SCANCODE_TAB] && a->console_open
        && ui->focus == console_id)
        console_tab(a);
    presets_walk_keys(a, ui);

    if (ui->in.key_pressed[SDL_SCANCODE_ESCAPE]) {
        if (ui->focus == bar_id || ui->focus == list_id
            || ui->focus == buttons_id) {
            /* out of the preset controls: the query and the highlight go */
            ui->focus = 0;
            a->preset_name.len = 0;
            a->preset_name.text[0] = '\0';
            a->preset_searching = false;
            a->have_selected = false;
            a->preset_armed = 0;
        } else if (a->console_open && console_escape(a)) {
        } else if (a->console_open) {
            a->console_open = false;
            a->console_input.len = 0;
            a->console_input.text[0] = '\0';
            a->preset_armed = 0;
            if (ui->focus == console_id) ui->focus = 0;
        } else if (a->presets_open) {
            a->presets_open = false;
        }
    }

    if (a->console_open && ui->focus == console_id
        && ui->in.key_pressed[SDL_SCANCODE_RETURN])
        console_enter(a);
}

void app_frame(App *a, Ui *ui) {
    Canvas *c = ui->canvas;
    canvas_set_clip(c, rct(0, 0, DESIGN_W, DESIGN_H));
    canvas_fill(c, INK_BLACK);

    a->frame_count++;
    double now = ui->time;
    float dt = (float)(now - a->last_frame_time);
    a->last_frame_time = now;
    if (dt > 0.0f) {
        float inst = 1.0f / dt;
        float k = expf(-dt / 0.5f);
        a->fps = inst + (a->fps - inst) * k;
    }

    gui_sync_chain(a);
    tell_new_tips(a);
    gui_apply_cc(a, ui);
    gui_drain_viz(a);
    gui_drain_recording(a);
    a->pointer = ui->in.mouse;
    centre_prelayout(a, ui);
    handle_keys(a, ui);

    /* ---- carve the frame ---- */
    Rct full = rct(0, 0, DESIGN_W, DESIGN_H);

    Rct top = rct(full.x0, full.y0, full.x1, full.y0 + TITLE_BAR_H);
    draw_title_bar(a, ui, top);
    a->header_rect = top;

    float footer_h = fminf(FOOTER_LINE_H, DESIGN_H - TITLE_BAR_H);
    Rct footer = rct(full.x0, full.y1 - footer_h, full.x1, full.y1);
    draw_footer(a, ui, footer);

    Rct band = rct(full.x0, top.y1, full.x1, footer.y0);

    Rct lbar = rct(band.x0, band.y0, band.x0 + MARGIN_BAR_W, band.y1);
    Rct rbar = rct(band.x1 - MARGIN_BAR_W, band.y0, band.x1, band.y1);
    draw_margin_bar(c, lbar, false);
    draw_margin_bar(c, rbar, true);

    Rct row = rct(lbar.x1, band.y0, rbar.x0, band.y1);
    float row_w = rct_w(row);
    float stats_share = a->splits.tree >= 0.0f ? a->splits.tree : STATS_COL_SHARE;
    float ops_share = a->splits.ops >= 0.0f ? a->splits.ops : OPS_COL_SHARE;
    float lo = min_share(row_w);
    stats_share = clampf(stats_share, lo, 1.0f / 3.0f);
    ops_share = clampf(ops_share, lo, 1.0f / 3.0f);
    float stats_w = roundf(stats_share * row_w);
    float ops_w = roundf(ops_share * row_w);

    Rct stats = rct(row.x0, row.y0, row.x0 + stats_w, row.y1);
    Rct ops = rct(row.x1 - ops_w, row.y0, row.x1, row.y1);
    Rct centre = rct(stats.x1, row.y0, ops.x0, row.y1);

    draw_left_rail(a, ui, stats);
    draw_right_rail(a, ui, ops);

    /* central cells */
    float centre_w = rct_w(centre);
    float controls_share =
        a->splits.controls >= 0.0f ? a->splits.controls : CONTROLS_SHARE;
    controls_share = clampf(controls_share, min_share(centre_w), 0.8f);
    float top_share = a->splits.top >= 0.0f ? a->splits.top : GOLDEN_MAJOR;
    top_share = clampf(top_share, min_share(rct_h(centre)), 0.9f);

    float cx_w = roundf(controls_share * centre_w);
    Rct cell_x = rct(centre.x0, centre.y0, centre.x0 + cx_w, centre.y1);
    Rct right = rct(cell_x.x1 + CELL_GUTTER, centre.y0, centre.x1, centre.y1);
    float cy_h = roundf(top_share * rct_h(right));
    Rct cell_y = rct(right.x0, right.y0, right.x1, right.y0 + cy_h);
    Rct cell_w = rct(right.x0, cell_y.y1 + CELL_GUTTER, right.x1, right.y1);

    draw_rect_stroke(c, cell_x, 2.0f, PAPER);
    draw_rect_stroke(c, cell_y, 2.0f, PAPER);
    draw_rect_stroke(c, cell_w, 2.0f, PAPER);

    Rct stage_clip = rct_shrink(cell_y, 3.0f);
    Rct saved = canvas_clip(c);
    canvas_set_clip(c, stage_clip);
    draw_stage(a, ui, cell_y);
    canvas_set_clip(c, saved);

    draw_keyboard_cell(a, ui, rct_shrink(cell_w, 2.0f));
    draw_controls_house(a, ui, rct_shrink(cell_x, 2.0f));

    /* seams */
    SeamAct s;
    s = seam(ui, "seam tree",
             rct(stats.x1 - SEAM_GRAB * 0.5f, band.y0,
                 stats.x1 + SEAM_GRAB * 0.5f, band.y1),
             true);
    if (s.moved) a->splits.tree = clampf(stats_share + s.delta / row_w, lo, 1.0f / 3.0f);
    if (s.reset) a->splits.tree = -1.0f;

    s = seam(ui, "seam ops",
             rct(ops.x0 - SEAM_GRAB * 0.5f, band.y0, ops.x0 + SEAM_GRAB * 0.5f,
                 band.y1),
             true);
    if (s.moved) a->splits.ops = clampf(ops_share - s.delta / row_w, lo, 1.0f / 3.0f);
    if (s.reset) a->splits.ops = -1.0f;

    s = seam(ui, "seam controls",
             rct(cell_x.x1 + CELL_GUTTER * 0.5f - SEAM_GRAB * 0.5f, centre.y0,
                 cell_x.x1 + CELL_GUTTER * 0.5f + SEAM_GRAB * 0.5f, centre.y1),
             true);
    if (s.moved)
        a->splits.controls =
            clampf(controls_share + s.delta / centre_w, min_share(centre_w), 0.8f);
    if (s.reset) a->splits.controls = -1.0f;

    s = seam(ui, "seam rows",
             rct(right.x0, cell_y.y1 + CELL_GUTTER * 0.5f - SEAM_GRAB * 0.5f,
                 right.x1, cell_y.y1 + CELL_GUTTER * 0.5f + SEAM_GRAB * 0.5f),
             false);
    if (s.moved)
        a->splits.top = clampf(top_share + s.delta / rct_h(right),
                               min_share(rct_h(centre)), 0.9f);
    if (s.reset) a->splits.top = -1.0f;

    /* overlays */
    if (a->presets_open) draw_presets_pane(a, ui);
    if (a->info_open) draw_info_pane(a, ui);
    if (a->show_fps) draw_fps_counter(a, ui, footer_h);
    if (a->console_open) draw_console_drawer(a, ui, footer);

    /* splash on top of everything */
    if (!a->splash_over) {
        float elapsed = (float)ui->time;
        if (!splash_draw(a, c, full, elapsed)) a->splash_over = true;
    }
}

void app_init_defaults(App *a) {
    memset(a, 0, sizeof *a);
    Session s = session_default();
    a->shadow = s.patch;
    a->shadow_verb = s.verb;
    a->shadow_melody = s.melody;
    a->shadow_chandas = s.chandas;
    a->shadow_warmth = s.warmth;
    a->shadow_release_s = env_params_default().release_s;
    a->tempo_bpm = s.tempo_bpm;
    a->drone_hz = s.drone_hz;
    a->chain = chain_default();
    a->engaged = true;
    a->splits = (Splits){-1.0f, -1.0f, -1.0f, -1.0f};
    a->cc_bind[1] = CC_GLIDE; /* modwheel */
    a->preset_filter.kind = FILTER_ALL;
    a->ops_tab = 0;
}
