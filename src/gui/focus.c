#include "focus.h"

Focus focus_owner(const App *a) {
    if (a->console_open && a->console_focused) return FOCUS_CONSOLE;
    if (a->preset_searching) return FOCUS_SEARCH;
    if (a->have_selected) return FOCUS_LIST;
    return FOCUS_CANVAS;
}

bool focus_console_open(const App *a) { return a->console_open; }

bool focus_console_active(const App *a) {
    return a->console_open && a->console_focused;
}

void focus_console_take(App *a) {
    a->console_open = true;
    a->console_focus = true;
}

void focus_console_drop(App *a) {
    a->console_open = false;
    a->console_focus = false;
    a->console_focused = false;
    a->console_input.len = 0;
    a->console_input.text[0] = '\0';
}

bool focus_browsing(const App *a) { return a->presets_open; }

bool focus_highlighted(const App *a, PresetRef *out) {
    if (!a->have_selected) return false;
    if (out) *out = a->preset_selected;
    return true;
}
