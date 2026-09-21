/* The space column: room or chandas, then the output stage. */
#include <math.h>
#include <stdio.h>

#include "app.h"

static void draw_room_page(App *a, Ui *ui, Rct r) {
    Stack s = stack_in(r, GROUP, "room");
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_MIX);
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_GHOST);
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_VERB_DECAY);
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_DAMP);
    {
        /* haunt trembles sideways under dread */
        Rct row = stack_row(&s, FADER_H);
        row.x0 += 2.0f + roundf(shell_tremble(a));
        param_fader(a, ui, row, PARAM_HAUNT);
    }
    stack_close(&s);
}

static void draw_chandas_page(App *a, Ui *ui, Rct r) {
    FontId body = ui_font(12.0f);
    ChandasParams *h = &a->shadow_chandas;
    Stack s = stack_in(r, GROUP, "chandas");
    {
        Rct row = stack_row(&s, ROW_H);
        row.x1 = row.x0 + fmaxf(text_width(body, "SYNC", 0.0f) + 2.0f * GAP, 60.0f);
        if (chip_button(ui, ui_id("chandas sync"), row, "SYNC", h->sync)) {
            h->sync = !h->sync;
            params_send(a, PG_CHANDAS);
        }
    }
    if (h->sync) {
        /* tempo divisions are named steps, not a range */
        int last = CHANDAS_DIVISIONS_LEN - 1;
        int d = (int)h->division > last ? last : (int)h->division;
        FaderAct act = fader_track(ui, ui_id("chandas time"),
                                   stack_row(&s, FADER_H), "time",
                                   CHANDAS_DIVISIONS[d].name,
                                   (float)d / (float)last);
        int next = d;
        if (act.kind == FADER_SET) next = (int)roundf(act.t * (float)last);
        else if (act.kind == FADER_RESET)
            next = (int)chandas_params_default().division;
        if (next != d) {
            h->division = (size_t)next;
            params_send(a, PG_CHANDAS);
        }
    } else {
        param_fader(a, ui, stack_row(&s, FADER_H), PARAM_CH_RATE);
    }
    static const ParamId REST[] = {PARAM_CH_MIX, PARAM_CH_SPREAD, PARAM_CH_SIZE,
                                   PARAM_CH_WARP, PARAM_CH_DIM, PARAM_CH_TAIL};
    for (size_t i = 0; i < sizeof REST / sizeof REST[0]; i++)
        param_fader(a, ui, stack_row(&s, FADER_H), REST[i]);
    stack_close(&s);
}

static const Tab SPACE[SPACE_TABS] = {
    [TAB_ROOM] = {"THE ROOM", draw_room_page},
    [TAB_CHANDAS] = {"CHANDAS", draw_chandas_page},
};

/* warmth and the limiter sit at the bottom, the same under either page;
   they are cut from the bottom up so the pages get whatever is left */
void draw_space_column(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    FontId f = ui_font(12.0f);
    r = rct_shrink(r, 6.0f);

    param_fader(a, ui, cut_bottom(&r, FADER_H), PARAM_CEILING);
    cut_bottom(&r, GROUP);
    Rct gr = cut_bottom(&r, text_row_height(f));
    char meter[32];
    snprintf(meter, sizeof meter, "GR %.1f dB", (double)a->limiter_reduction_db);
    text_draw(c, f, (P2){gr.x0, gr.y0}, ALIGN_LEFT_TOP, meter, PAPER, 0.0f);
    cut_bottom(&r, GROUP);
    if (chip_button(ui, ui_id("space limiter"), cut_bottom(&r, ROW_H),
                    "SAFE OUTPUT", a->shadow_limiter_enabled)) {
        a->shadow_limiter_enabled = !a->shadow_limiter_enabled;
        params_send(a, PG_LIMITER);
    }
    cut_bottom(&r, SECTION);
    param_fader(a, ui, cut_bottom(&r, FADER_H), PARAM_WARMTH);
    cut_bottom(&r, SECTION);

    tab_view(a, ui, "space tab", r, SPACE, SPACE_TABS, &a->space_tab);
}
