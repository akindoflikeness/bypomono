/* The space column: room or chandas, then the output stage. */
#include <math.h>
#include <stdio.h>

#include "app.h"

#define PENTAGRAM_H 110.0f

static void dotted_circle(Canvas *c, P2 center, float radius, uint8_t ink) {
    int n = (int)(TAU_F * radius / 4.0f);
    for (int i = 0; i < n; i++) {
        float ang = TAU_F * (float)i / (float)n;
        draw_rect_filled(c, rct_xywh(roundf(center.x + cosf(ang) * radius),
                                     roundf(center.y + sinf(ang) * radius),
                                     1.0f, 1.0f), ink);
    }
}

static void paint_chandas_walk(App *a, Ui *ui, const P2 pts[5]) {
    const ChandasParams *h = &a->shadow_chandas;
    if (!h->enabled || h->mix <= 0.0f) return;
    Canvas *c = ui->canvas;
    float period = fmaxf(chandas_base_seconds_of(h, a->tempo_bpm), 0.02f);
    float beats = (float)ui->time / period;
    long beat = (long)floorf(beats);
    float t = beats - (float)beat;
    ui->repaint_soon = true;

    /* the star is drawn point to point+2, so a walk along its lines steps 2 */
    int at = (int)(((beat * 2) % 5 + 5) % 5);
    P2 from = pts[at], to = pts[(at + 2) % 5];
    P2 grain = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
    draw_circle_filled(c, grain, 2.5f + 2.0f * h->mix, PAPER);

    float grow = 6.0f + 14.0f * h->size / CHANDAS_MAX_SIZE;
    int trail = 1 + (int)roundf(h->spread * 3.0f);
    for (int k = 0; k < trail; k++) {
        int p = (int)((((beat - k) * 2) % 5 + 5) % 5);
        float age = ((float)k + t) / (float)trail;
        float radius = 9.0f + grow * age;
        if (k == 0) draw_circle_stroke(c, pts[p], radius, 1.0f, PAPER);
        else dotted_circle(c, pts[p], radius, PAPER);
    }
}

/* The room draws haunt's chords and lights each point with its operator.
   Chandas walks the star: one grain travels along a line per chandas period,
   leaving a ring at the point it reaches that grows with size, and spread
   keeps that many earlier rings fading behind it. */
static void paint_pentagram(App *a, Ui *ui, Rct rect) {
    Canvas *c = ui->canvas;
    P2 center = rct_center(rect);
    float radius = fminf(rct_w(rect), rct_h(rect)) * 0.40f;
    float haunt = a->shadow_verb.haunt;
    P2 pts[5];
    for (int i = 0; i < 5; i++) {
        float ang = -PI_F / 2.0f + TAU_F * (float)i / 5.0f;
        pts[i] = (P2){center.x + cosf(ang) * radius,
                      center.y + sinf(ang) * radius};
    }
    int chords = (int)(haunt * 21.0f);
    for (int k = 1; k <= chords; k++) {
        float ta = (float)k / 21.0f;
        float tb = (float)(k * 2 % 21) / 21.0f;
        float aa = -PI_F / 2.0f + TAU_F * ta;
        float ab = -PI_F / 2.0f + TAU_F * tb;
        P2 pa = {center.x + cosf(aa) * radius, center.y + sinf(aa) * radius};
        P2 pb = {center.x + cosf(ab) * radius, center.y + sinf(ab) * radius};
        draw_line(c, pa, pb, 1.0f, PAPER);
    }
    for (int i = 0; i < 5; i++) {
        P2 from = pts[i], to = pts[(i + 3) % 5];
        float dx = to.x - from.x, dy = to.y - from.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        dx /= len;
        dy /= len;
        bubble_chain(c, ui, (P2){from.x + dx * 9.0f, from.y + dy * 9.0f},
                     (P2){to.x - dx * 9.0f, to.y - dy * 9.0f}, haunt > 0.0f,
                     a->shadow.index, ui->time);
    }
    for (int i = 0; i < 5; i++) {
        draw_circle_filled(c, pts[i], 7.0f, INK_BLACK);
        draw_circle_stroke(c, pts[i], 7.0f, 1.0f, PAPER);
        dither_circle(c, pts[i], 5.5f, fminf(a->env[i] * haunt * 2.0f, 1.0f),
                      2.0f);
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        text_draw(c, ui_font(10.0f), pts[i], ALIGN_CENTER_CENTER, num, PAPER,
                  0.0f);
    }
    paint_chandas_walk(a, ui, pts);
}

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

/* the limiter sits at the bottom, the same under either page;
   it is cut from the bottom up so the pages get whatever is left */
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

    paint_pentagram(a, ui, cut_top(&r, PENTAGRAM_H));
    cut_top(&r, GROUP);
    tab_view(a, ui, "space tab", r, SPACE, SPACE_TABS, &a->space_tab);
}
