/* Scale arithmetic shared by the standalone and the plugin; it links no
   SDL, because the plugin does not. */

#include <stdlib.h>

#include "app.h"

#define WINDOW_CHROME_H 64 /* title bar the reported bounds do not describe */

float pick_display_scale(int avail_w, int avail_h) {
    const char *env = getenv("BYPO_SCALE");
    if (env) return clampf(strtof(env, NULL), WINDOW_SCALE_MIN, WINDOW_SCALE_MAX);
    if (avail_w <= 0 || avail_h <= 0) return WINDOW_SCALE_MIN;

    float budget_h = (float)avail_h - WINDOW_CHROME_H;
    float s = WINDOW_SCALE_MIN;
    for (float k = WINDOW_SCALE_MIN + WINDOW_SCALE_STEP; k <= WINDOW_SCALE_MAX; k += WINDOW_SCALE_STEP) {
        if (DESIGN_W * k <= (float)avail_w && DESIGN_H * k <= budget_h) s = k;
    }
    return s;
}

float snap_scale(float s) {
    return clampf(roundf(s / WINDOW_SCALE_STEP) * WINDOW_SCALE_STEP, WINDOW_SCALE_MIN, WINDOW_SCALE_MAX);
}
