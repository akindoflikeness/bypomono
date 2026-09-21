/* The sound column: the core faders and the voice setup. */
#include <math.h>
#include <stdio.h>

#include "app.h"

static void voices(App *a, Ui *ui, Stack *s) {
    static const char *const NAMES[3] = {"mono", "poly 4", "unison"};
    FontId f = ui_font(12.0f);
    Flow flow = flow_in(stack_rest(s), ROW_H, GROUP);
    int hit = -1;
    for (int i = 0; i < 3; i++) {
        bool active = i == 0   ? a->shadow.voices <= 1
                      : i == 1 ? a->shadow.voices > 1
                               : a->shadow.unison > 1;
        if (i == 2) flow.x += GROUP; /* unison sits on top of either */
        Rct chip = flow_next(&flow, text_width(f, NAMES[i], 0.0f) + 2.0f * GAP);
        if (chip_button(ui, ui_id_n("sound voices", i), chip, NAMES[i], active)
            && (i == 2 || !active))
            hit = i;
    }
    stack_row(s, flow_bottom(&flow) - flow.area.y0);

    if (hit == 0) {
        a->shadow.voices = 1;
        push_log(a, "mono. one voice; a new note glides out of the last.");
    } else if (hit == 1) {
        a->shadow.voices = POLY_MAX;
        push_log(a, "poly 4. up to four notes at once; a fifth takes the "
                    "oldest. a held drone keeps one of the four.");
    } else if (hit == 2) {
        a->shadow.unison = a->shadow.unison > 1 ? 1 : UNISON_MAX;
        if (a->shadow.unison > 1)
            push_log(a, "unison. two voices a note, %.1f cents apart, "
                        "spread left and right.",
                     (double)a->shadow.unison_detune);
        else
            push_log(a, "unison off. one voice a note.");
    }
    if (hit >= 0) params_send(a, PG_PATCH);

    Rct row = stack_row(s, FADER_H);
    if (a->shadow.unison > 1) {
        param_fader(a, ui, row, PARAM_DETUNE);
    } else if (param_fader_veiled(a, ui, row, PARAM_DETUNE) && press_on(ui, row)) {
        push_log(a, "detune spreads the unison pair. switch unison on to "
                    "hear it.");
    }
}

void draw_sound_column(App *a, Ui *ui, Rct r) {
    Canvas *c = ui->canvas;
    float strip_h = text_row_height(ui_font(12.0f)) + 2.0f * SNUG;
    Stack s = stack_in(rct_shrink(r, 6.0f), GROUP, "sound");

    inverted_strip(c, stack_row(&s, strip_h), "SOUND");
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_INDEX);
    {
        /* RIP trembles sideways under dread */
        Rct row = stack_row(&s, FADER_H);
        row.x0 += 2.0f + roundf(shell_tremble(a));
        param_fader(a, ui, row, PARAM_RIP);
    }
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_FB);
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_GLIDE);
    {
        Rct row = stack_row(&s, FADER_H);
        if (midi_driving(a)) {
            if (param_fader_veiled(a, ui, row, PARAM_DRONE_HZ) && press_on(ui, row))
                push_log(a, "midi is connected, so the instrument is "
                            "configured for midi — this control is idle until "
                            "it's unplugged.");
        } else if (a->shadow_melody.enabled || a->shadow_pitch.enabled) {
            if (param_fader_veiled(a, ui, row, PARAM_DRONE_HZ) && press_on(ui, row))
                push_log(a, "the sequencer owns the pitch while it runs — "
                            "switch it off to hand this slider back.");
        } else {
            param_fader(a, ui, row, PARAM_DRONE_HZ);
        }
    }
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_FIELD);
    param_fader(a, ui, stack_row(&s, FADER_H), PARAM_CURVE); /* field's shape */

    stack_space(&s, GROUP);
    inverted_strip(c, stack_row(&s, strip_h), "VOICES");
    voices(a, ui, &s);
    stack_close(&s);
}
