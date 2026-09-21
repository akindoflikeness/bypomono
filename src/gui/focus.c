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

/* the presets list is a display page; closing it goes back to the page
   that was showing before */
bool presets_showing(const App *a) { return a->display_tab == TAB_PRE; }

void presets_show(App *a, bool on) {
    if (on == presets_showing(a)) return;
    if (on) {
        a->display_back = a->display_tab;
        a->display_tab = TAB_PRE;
    } else {
        a->display_tab = a->display_back == TAB_PRE ? TAB_SHELL : a->display_back;
    }
}

bool focus_browsing(const App *a) { return presets_showing(a); }

bool focus_highlighted(const App *a, PresetRef *out) {
    if (!a->have_selected) return false;
    if (out) *out = a->preset_selected;
    return true;
}
