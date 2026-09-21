#ifndef BYPO_TEXT_H
#define BYPO_TEXT_H

#include "canvas.h"

/* the three faces the program draws with: UI text, readouts, and a
   fallback with wide glyph coverage for symbols the other two lack */
enum {
    FACE_BYPOSERIF = 0,
    FACE_UNIFONTEXMONO,
    FACE_EUROPEAN_TELETEXT,
    FACE_COUNT
};

#define UI_FACE FACE_BYPOSERIF
#define READOUT_FACE FACE_EUROPEAN_TELETEXT
#define FALLBACK_FACE FACE_UNIFONTEXMONO

typedef struct {
    int face;
    float px;
} FontId;

typedef enum {
    ALIGN_LEFT_TOP, ALIGN_CENTER_TOP, ALIGN_RIGHT_TOP,
    ALIGN_LEFT_CENTER, ALIGN_CENTER_CENTER, ALIGN_RIGHT_CENTER,
    ALIGN_LEFT_BOTTOM, ALIGN_CENTER_BOTTOM, ALIGN_RIGHT_BOTTOM
} Align;

/* loads faces from <assets>/fonts/<slug>/<file>, falling back to the copy
   compiled into the binary; reports the source it used on stderr */
int text_init(const char *assets_dir);
/* the face compiled into this build, or NULL when it embeds none */
const unsigned char *embedded_face(int face, size_t *len);
void text_shutdown(void);

/* quantise a wanted size to the face's native pixel grid (ppp = 1 here) */
float grid_size(float want, float native);
FontId ui_font(float want);      /* UI_FACE, nearest drawn size */
FontId readout_font(float want); /* READOUT_FACE, native 16 */
float font_native(int face);     /* 0 if the face is not grid-snapped */

float text_width(FontId f, const char *s, float letter_spacing);
float text_row_height(FontId f);
float glyph_width(FontId f, uint32_t codepoint);

/* draws one line; \n is not special. returns the rect the text covered */
Rct text_draw(Canvas *c, FontId f, P2 anchor_pos, Align anchor, const char *s,
              uint8_t ink, float letter_spacing);

/* per-glyph x offsets (pre-advance) for caret/wave effects.
   xs gets n+1 entries (last = total width). returns glyph count. */
int text_glyph_xs(FontId f, const char *s, float letter_spacing, float *xs,
                  int max);
/* draw a single glyph at pos (baseline-left); advance returned */
float text_draw_glyph(Canvas *c, FontId f, P2 pos_topleft, uint32_t cp,
                      uint8_t ink);

/* greedy word wrap; draws if c != NULL, always returns total height */
float text_draw_wrapped(Canvas *c, FontId f, Rct area, const char *s,
                        uint8_t ink);

#endif
