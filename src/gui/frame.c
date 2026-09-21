#include <SDL2/SDL_scancode.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

static void handle_keys(App *a, Ui *ui) {
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
        } else if (a->console_open && console_escape(a)) {
        } else if (a->console_open) {
            a->console_open = false;
            a->console_input.len = 0;
            a->console_input.text[0] = '\0';
            if (ui->focus == console_id) ui->focus = 0;
        } else if (presets_showing(a)) {
            presets_show(a, false);
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
    shell_prelayout(a, ui);
    handle_keys(a, ui);
    layout_frame_begin();

    Screen s = screen_layout(rct(0, 0, DESIGN_W, DESIGN_H),
                             fminf(FOOTER_LINE_H, DESIGN_H));
    draw_header(a, ui, &s);
    draw_footer(a, ui, s.footer);

    struct { Rct r; void (*draw)(App *, Ui *, Rct); } panels[] = {
        {s.sound, draw_sound_column},     {s.fm, draw_fm_column},
        {s.display, draw_display_column}, {s.space, draw_space_column},
        {s.melody, draw_melody_bar},
    };
    Rct saved = canvas_clip(c);
    for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
        Rct inner = rct_shrink(panels[i].r, 2.0f);
        canvas_set_clip(c, rct_intersect(saved, inner));
        panels[i].draw(a, ui, inner);
        canvas_set_clip(c, saved);
        draw_rect_stroke(c, panels[i].r, 2.0f, PAPER);
    }

    /* overlays */
    if (a->info_open) draw_info_pane(a, ui);
    if (a->show_fps) draw_fps_counter(a, ui, rct_h(s.footer));
    if (a->console_open) draw_console_drawer(a, ui, s.footer);
}

void app_init_defaults(App *a) {
    memset(a, 0, sizeof *a);
    Session s = session_default();
    a->shadow = s.patch;
    a->shadow_verb = s.verb;
    a->shadow_melody = s.melody;
    a->shadow_chandas = s.chandas;
    a->shadow_warmth = s.warmth;
    a->shadow_limiter_enabled = s.limiter_enabled;
    a->shadow_limiter_ceiling_db = s.limiter_ceiling_db;
    a->shadow_attack_s = s.attack_s;
    a->shadow_decay_s = s.decay_s;
    a->shadow_sustain = s.sustain;
    a->shadow_release_s = s.release_s;
    a->tempo_bpm = s.tempo_bpm;
    a->tempo_source = TEMPO_INTERNAL;
    a->transport_running = true;
    a->drone_hz = s.drone_hz;
    a->chain = chain_default();
    a->engaged = true;
    a->cc_bind[1] = CC_GLIDE; /* modwheel */
    a->preset_filter.kind = FILTER_ALL;
    a->space_tab = TAB_ROOM;
    a->display_tab = TAB_SHELL;
    push_log(a, "BLOW YOUR PHASE OFF v%s", APP_VERSION);
}
