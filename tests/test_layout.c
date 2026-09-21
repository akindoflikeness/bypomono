/* The screen carve and the layout helpers, and the parameter table's
   curves and defaults. None of it needs fonts or a window. */
#include <string.h>

#include "../src/gui/app.h"
#include "test.h"

static bool overlaps(Rct a, Rct b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static float area(Rct r) { return rct_w(r) * rct_h(r); }

static void screen_tiles(void) {
    Rct full = rct(0, 0, DESIGN_W, DESIGN_H);
    Screen s = screen_layout(full, FOOTER_LINE_H);
    Rct parts[] = {s.header, s.sound, s.fm, s.display, s.space, s.strip,
                   s.footer};
    const char *names[] = {"header", "sound", "fm", "display", "space",
                           "strip", "footer"};
    const int n = (int)(sizeof parts / sizeof parts[0]);
    float covered = 0.0f;
    for (int i = 0; i < n; i++) {
        CHECK(rct_w(parts[i]) > 0.0f && rct_h(parts[i]) > 0.0f, "%s is empty",
              names[i]);
        CHECK(parts[i].x0 >= full.x0 && parts[i].y0 >= full.y0
                  && parts[i].x1 <= full.x1 && parts[i].y1 <= full.y1,
              "%s leaves the window", names[i]);
        for (int j = i + 1; j < n; j++)
            CHECK(!overlaps(parts[i], parts[j]), "%s overlaps %s", names[i],
                  names[j]);
        covered += area(parts[i]);
    }
    /* everything else is the gaps between panels */
    float gaps = 2.0f * PANEL_GAP * (DESIGN_W)
                 + 3.0f * PANEL_GAP * rct_h(s.sound);
    CHECK_NEAR(covered + gaps, area(full), 1.0f, "panels and gaps cover %g of %g",
               (double)(covered + gaps), (double)area(full));
    CHECK(rct_h(s.sound) >= 480.0f, "the main band shrank to %g",
          (double)rct_h(s.sound));
}

static void helpers(void) {
    Rct r = rct(0, 0, 100, 50);
    Rct top = cut_top(&r, 10);
    CHECK(top.y1 == 10 && r.y0 == 10, "cut_top");
    Rct too_much = cut_left(&r, 500);
    CHECK(too_much.x1 == 100 && rct_w(r) == 0, "a cut takes no more than is left");

    layout_frame_begin();
    Stack s = stack_in(rct(0, 0, 100, 50), 5, "test");
    stack_row(&s, 20);
    stack_row(&s, 25);
    stack_close(&s);
    CHECK(layout_overflow_count() == 0, "20 + 5 + 25 fits 50");
    stack_row(&s, 10);
    stack_close(&s);
    float px = 0.0f;
    CHECK(layout_overflow_count() == 1 && strcmp(layout_overflow_first(&px), "test") == 0
              && px == 15.0f,
          "overflow by 15 not reported (got %g)", (double)px);

    Flow f = flow_in(rct(0, 0, 100, 100), 20, 5);
    flow_next(&f, 60);
    Rct second = flow_next(&f, 60);
    CHECK(second.x0 == 0 && second.y0 == 25, "a chip that will not fit wraps");
    CHECK(flow_bottom(&f) == 45, "flow bottom");
}

static void params_round_trip(void) {
    static App a;
    Session s = session_default();
    a.shadow = s.patch;
    a.shadow_verb = s.verb;
    a.shadow_melody = s.melody;
    a.shadow_chandas = s.chandas;
    for (int i = 0; i < PARAM_COUNT; i++) {
        const Control *p = &PARAMS[i];
        float d = param_default(&a, (ParamId)i);
        CHECK(d >= p->lo && d <= p->hi, "%s default %g outside %g..%g", p->name,
              (double)d, (double)p->lo, (double)p->hi);
        CHECK(param_find(p->name) == i, "%s is not unique", p->name);
        for (int k = 0; k <= 4; k++) {
            float pos = (float)k / 4.0f;
            float v = param_at((ParamId)i, pos);
            CHECK(v >= p->lo - 1e-4f && v <= p->hi + 1e-4f,
                  "%s at %g gives %g", p->name, (double)pos, (double)v);
            if (!p->integer)
                CHECK_NEAR(param_pos((ParamId)i, v), pos, 1e-3f,
                           "%s does not round trip at %g", p->name, (double)pos);
        }
        param_set(&a, (ParamId)i, p->hi + 1000.0f);
        CHECK(param_get(&a, (ParamId)i) == p->hi, "%s set did not clamp",
              p->name);
    }
}

/* the sequence cells get what is left of the strip under its tab row; below
   about 120 px the sixteen sliders stop being paintable */
static void strip_leaves_room_for_the_cells(void) {
    Screen s = screen_layout(rct(0, 0, DESIGN_W, DESIGN_H), FOOTER_LINE_H);
    float cells = rct_h(s.strip) - 2.0f * 6.0f - ROW_H - GAP;
    CHECK(cells >= 120.0f, "sequence cells only get %g px", cells);
}

void test_layout(void) {
    screen_tiles();
    strip_leaves_room_for_the_cells();
    helpers();
    params_round_trip();
}
