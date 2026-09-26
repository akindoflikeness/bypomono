#include "ui.h"

#include <math.h>
#include <string.h>

UiId ui_id(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

UiId ui_id_n(const char *s, int n) {
    uint64_t h = ui_id(s);
    h ^= (uint64_t)(n + 1) * 0x9E3779B97F4A7C15ull;
    return h ? h : 1;
}

static Resp interact(Ui *ui, UiId id, Rct r, float slop, bool drag) {
    Resp out = {0};
    out.rect = r;
    out.pointer = ui->in.mouse;
    Rct hover_zone = rct_expand(r, slop);
    bool over = ui->in.mouse_in_window && rct_contains(hover_zone, ui->in.mouse)
                && rct_contains(ui->canvas->clip, ui->in.mouse);
    if (over && ui->active == 0) ui->hot = id;
    out.hovered = over || ui->active == id;

    if (ui->in.pressed && over && ui->active == 0) {
        ui->active = id;
        out.drag_started = drag;
        out.pressed_on = true;
        if (ui->in.double_clicked) out.double_clicked = true;
    }
    if (ui->active == id) {
        out.pressed_on = ui->in.down;
        if (drag && ui->in.down) {
            out.dragged = true;
            out.drag_delta = (P2){ui->in.mouse.x - ui->last_press_pos.x, 0};
        }
        if (ui->in.released) {
            ui->active = 0;
            if (over && !drag) out.clicked = true;
            if (over && drag) out.clicked = true;
        }
    }
    return out;
}

Resp ui_interact(Ui *ui, UiId id, Rct r, float slop) {
    return interact(ui, id, r, slop, false);
}

Resp ui_interact_drag(Ui *ui, UiId id, Rct r, float slop) {
    Resp out = interact(ui, id, r, slop, true);
    /* per-frame delta, both axes. On the press frame only the movement since
       the press counts, not what the pointer did before it. */
    if (out.drag_started) {
        out.drag_delta = (P2){ui->in.mouse.x - ui->last_press_pos.x,
                              ui->in.mouse.y - ui->last_press_pos.y};
    } else if (ui->active == id && ui->in.down) {
        out.drag_delta = (P2){ui->in.mouse.x - ui->drag_prev.x,
                              ui->in.mouse.y - ui->drag_prev.y};
    }
    return out;
}

float ui_animate_bool(Ui *ui, UiId id, bool target, float seconds) {
    for (int i = 0; i < ui->anim_count; i++) {
        if (ui->anim[i].id == id) {
            float step = seconds > 0.0f ? ui->dt / seconds : 1.0f;
            float v = ui->anim[i].v + (target ? step : -step);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            ui->anim[i].v = v;
            if (v > 0.0f && v < 1.0f) ui->repaint_soon = true;
            return v;
        }
    }
    if (ui->anim_count < (int)(sizeof ui->anim / sizeof ui->anim[0])) {
        ui->anim[ui->anim_count].id = id;
        ui->anim[ui->anim_count].v = target ? 1.0f : 0.0f;
        ui->anim_count++;
    }
    return target ? 1.0f : 0.0f;
}

float ui_scroll(Ui *ui, UiScroll *s, Rct viewport, float content_h) {
    float max_off = fmaxf(content_h - rct_h(viewport), 0.0f);
    if (ui->in.mouse_in_window && rct_contains(viewport, ui->in.mouse)
        && ui->in.wheel != 0.0f)
        s->offset += ui->in.wheel * 24.0f;
    s->offset = fmaxf(0.0f, fminf(s->offset, max_off));
    return s->offset;
}

bool ui_text_edit(Ui *ui, UiId id, UiText *t) {
    if (ui->focus != id) return false;
    bool changed = false;
    for (const char *p = ui->in.text; *p;) {
        int len = 1;
        uint8_t b = (uint8_t)*p;
        if (b >= 0xF0) len = 4;
        else if (b >= 0xE0) len = 3;
        else if (b >= 0xC0) len = 2;
        if (t->len + len < (int)sizeof t->text) {
            memcpy(t->text + t->len, p, (size_t)len);
            t->len += len;
            t->text[t->len] = '\0';
            changed = true;
        }
        p += len;
    }
    if (ui->in.backspace_repeat && t->len > 0) {
        do {
            t->len--;
        } while (t->len > 0 && ((uint8_t)t->text[t->len] & 0xC0) == 0x80);
        t->text[t->len] = '\0';
        changed = true;
    }
    return changed;
}
