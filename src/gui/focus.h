#ifndef BYPO_FOCUS_H
#define BYPO_FOCUS_H

#include "app.h"

/* Who has the keyboard, derived from the App flags the panes maintain. */
typedef enum { FOCUS_CANVAS, FOCUS_LIST, FOCUS_SEARCH, FOCUS_CONSOLE } Focus;

Focus focus_owner(const App *a);
bool focus_console_open(const App *a);
/* the console line is open and holds the keyboard */
bool focus_console_active(const App *a);
/* open the console and hand it the keyboard on the next frame */
void focus_console_take(App *a);
/* close the console and clear its line */
void focus_console_drop(App *a);
bool focus_browsing(const App *a);
/* the highlighted row of the preset list, if there is one */
bool focus_highlighted(const App *a, PresetRef *out);

#endif
