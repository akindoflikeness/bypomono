#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static App g_app;
static Ui g_ui;

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

    if (ui->in.key_pressed[SDL_SCANCODE_F2]) report_arm(a);

    UiId bar_id = ui_id("preset bar");
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
        console_tab_complete(a);

    if (ui->in.key_pressed[SDL_SCANCODE_ESCAPE]) {
        if (a->report_open) {
            if (a->report_notes_len == 0) {
                report_discard(a);
            } else {
                push_log(a, "the report has words in it. save or discard it "
                            "by its own buttons.");
            }
        } else if (ui->focus == bar_id && a->preset_name.len > 0) {
            a->preset_name.len = 0;
            a->preset_name.text[0] = '\0';
            a->preset_searching = false;
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
        && ui->in.key_pressed[SDL_SCANCODE_RETURN]) {
        char line[256];
        snprintf(line, sizeof line, "%s", a->console_input.text);
        a->console_input.len = 0;
        a->console_input.text[0] = '\0';
        if (line[0]) console_run_line(a, line);
    }
}

static void frame(App *a, Ui *ui) {
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
    if (a->report_open) draw_report_panel(a, ui);
    if (a->show_fps) draw_fps_counter(a, ui, footer_h);
    if (a->console_open) draw_console_drawer(a, ui, footer);

    /* splash on top of everything */
    if (!a->splash_over) {
        float elapsed = (float)ui->time;
        if (!splash_draw(a, c, full, elapsed)) a->splash_over = true;
    }

    /* headless screenshot: BYPO_SHOT="secs:path" */
    if (a->have_shot && ui->time >= a->shot_at) {
        write_ppm(a->shot_path, c);
        a->quit = true;
        a->have_shot = false;
    }
    if (a->have_report_at && !a->report_fired && ui->time >= a->report_at) {
        a->report_fired = true;
        report_arm(a);
    }
}

static void app_defaults(App *a) {
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

    const char *shot = getenv("BYPO_SHOT");
    if (shot) {
        const char *colon = strchr(shot, ':');
        if (colon) {
            a->shot_at = atof(shot);
            snprintf(a->shot_path, sizeof a->shot_path, "%s", colon + 1);
            a->have_shot = true;
        }
    }
    const char *rep = getenv("BYPO_REPORT_AT");
    if (rep) {
        a->report_at = atof(rep);
        a->have_report_at = true;
    }
}

/* The window is DESIGN_W x DESIGN_H blown up by a whole number. WHOLE
   numbers only: the UI is a fixed 1180x780 pixel canvas that the renderer
   stretches, and a fractional factor doubles some rows and not others,
   which on a 1px hairline UI reads as a fault. So the window size is
   locked to the design and only its magnification moves.

   Largest scale that still fits the desktop, 1..4, in quarter steps so
   the screens between the integers are not stranded at 1x. The design
   grid never changes; only the final nearest-neighbour blit stretches,
   so fractional scales read as alternating fat/thin pixels rather than
   blur. On the three screens that matter: 1920x1080 -> 1.25,
   2560x1440 -> 1.5, 3840x2160 -> 2. BYPO_SCALE overrides (fractions
   allowed), and is the way to force a size the display cannot argue
   with. */
#define WINDOW_CHROME_H 64 /* title bar the usable bounds do not describe */

static float pick_scale(void) {
    const char *env = getenv("BYPO_SCALE");
    if (env) {
        float s = strtof(env, NULL);
        if (s < 1.0f) s = 1.0f;
        if (s > 4.0f) s = 4.0f;
        return s;
    }
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(0, &r) != 0) {
        /* no usable bounds (no window manager, or an old SDL): the whole
           desktop is the budget, and the chrome allowance covers the rest */
        SDL_DisplayMode m;
        if (SDL_GetDesktopDisplayMode(0, &m) != 0) return 1.0f;
        r.w = m.w;
        r.h = m.h;
    }
    int avail_h = r.h - WINDOW_CHROME_H;
    float s = 1.0f;
    for (float k = 1.25f; k <= 4.0f; k += 0.25f) {
        if (DESIGN_W * k <= (float)r.w && DESIGN_H * k <= (float)avail_h)
            s = k;
    }
    return s;
}

int main(void) {
    App *a = &g_app;
    app_defaults(a);
    prepare_preset_dir();

    if (text_init(asset_dir()) != 0)
        fprintf(stderr, "fonts not found under %s/fonts\n", asset_dir());

    a->restored = app_restore_state(a);
    preset_rescan(a);

    if (gui_audio_start(a) != 0) {
        fprintf(stderr, "no default audio output device\n");
        return 1;
    }
    Event ev = {.kind = EV_GLIDE_TO, .u.f = a->drone_hz};
    app_send(a, ev);

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    float scale = pick_scale();
    int win_w = (int)lroundf(DESIGN_W * scale);
    int win_h = (int)lroundf(DESIGN_H * scale);
    fprintf(stderr, "window: %dx%d (design %dx%d at %.2fx)\n", win_w, win_h,
            (int)DESIGN_W, (int)DESIGN_H, (double)scale);

    SDL_Window *win = SDL_CreateWindow(
        "BLOW YOUR PHASE OFF", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren =
        SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, (int)DESIGN_W,
                          (int)DESIGN_H);

    Canvas canvas;
    canvas_init(&canvas, (int)DESIGN_W, (int)DESIGN_H);
    Ui *ui = &g_ui;
    memset(ui, 0, sizeof *ui);
    ui->canvas = &canvas;

    SDL_StartTextInput();
    uint64_t t0 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    double last = 0.0;

    while (!a->quit) {
        UiInput *in = &ui->in;
        in->pressed = in->released = in->double_clicked = false;
        in->wheel = 0.0f;
        in->text[0] = '\0';
        in->backspace_repeat = false;
        memset(in->key_pressed, 0, sizeof in->key_pressed);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: a->quit = true; break;
            case SDL_MOUSEMOTION:
                in->mouse.x = (float)e.motion.x / scale;
                in->mouse.y = (float)e.motion.y / scale;
                in->mouse_in_window = true;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    in->down = true;
                    in->pressed = true;
                    if (e.button.clicks >= 2) in->double_clicked = true;
                    ui->last_press_pos = in->mouse;
                    ui->last_press_time = last;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    in->down = false;
                    in->released = true;
                }
                break;
            case SDL_MOUSEWHEEL:
                in->wheel = -(float)e.wheel.y;
                break;
            case SDL_TEXTINPUT: {
                size_t cur = strlen(in->text);
                snprintf(in->text + cur, sizeof in->text - cur, "%s",
                         e.text.text);
                break;
            }
            case SDL_KEYDOWN: {
                int sc = e.key.keysym.scancode;
                if (sc >= 0 && sc < 512) {
                    if (!e.key.repeat) in->key_pressed[sc] = true;
                    in->key_down[sc] = true;
                }
                if (sc == SDL_SCANCODE_BACKSPACE) in->backspace_repeat = true;
                break;
            }
            case SDL_KEYUP: {
                int sc = e.key.keysym.scancode;
                if (sc >= 0 && sc < 512) in->key_down[sc] = false;
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE)
                    in->mouse_in_window = false;
                break;
            }
        }

        double now = (double)(SDL_GetPerformanceCounter() - t0) / freq;
        ui->dt = (float)(now - last);
        last = now;
        ui->time = now;
        ui->cursor = CURSOR_DEFAULT;
        ui->hot = 0;
        ui->repaint_soon = false;

        frame(a, ui);
        ui->drag_prev = in->mouse;

        static SDL_Cursor *cursors[5];
        static bool cursors_made = false;
        if (!cursors_made) {
            cursors[CURSOR_DEFAULT] =
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            cursors[CURSOR_RESIZE_H] =
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
            cursors[CURSOR_RESIZE_V] =
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
            cursors[CURSOR_POINTER] =
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
            cursors[CURSOR_TEXT] =
                SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
            cursors_made = true;
        }
        SDL_SetCursor(cursors[ui->cursor]);

        SDL_UpdateTexture(tex, NULL, canvas.px, canvas.w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(REPAINT_FLOOR_MS);
    }

    app_save_state(a);
    if (a->recording) {
        gui_stop_record(a);
        gui_drain_recording(a);
        recorder_finalize(&a->recorder);
    }
    gui_audio_stop(a);
    text_shutdown();
    canvas_free(&canvas);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
