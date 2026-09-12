#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static App g_app;
static Ui g_ui;

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
    app_init_defaults(a);
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

        app_frame(a, ui);
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
