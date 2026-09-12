#include "text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *slug;
    const char *file;
    float native; /* 0 = not grid-snapped */
} FaceSpec;

static const FaceSpec FACES[FACE_COUNT] = {
    {"pixeloid_mono", "PixeloidMono.ttf", 9},
    {"pixeloid_sans", "PixeloidSans.ttf", 9},
    {"unifontexmono", "UnifontExMono.ttf", 16},
    {"modern_dos", "ModernDOS8x16.ttf", 16},
    {"european_teletext", "EuropeanTeletext.ttf", 16},
    {"pixel_operator", "PixelOperator.ttf", 16},
    {"better_vcr", "BetterVCR 25.09.ttf", 16},
    {"enter_command", "EnterCommand.ttf", 16},
    {"scriptorium", "Scriptorium.ttf", 16},
    {"cyborgsister", "CyborgSister.ttf", 16},
    {"alkhemikal", "Alkhemikal.ttf", 16},
    {"pixantiqua", "PixAntiqua.ttf", 0},
};

static FT_Library g_ft;
static FT_Face g_face[FACE_COUNT];

typedef struct {
    int face;
    int px;
    uint32_t cp;
    int w, h, left, top;
    float advance;
    uint8_t *bitmap;
    bool used;
} GlyphEntry;

#define GLYPH_CACHE 4096
static GlyphEntry g_cache[GLYPH_CACHE];

int text_init(const char *assets_dir) {
    if (FT_Init_FreeType(&g_ft)) return -1;
    int loaded = 0;
    for (int i = 0; i < FACE_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/fonts/%s/%s", assets_dir,
                 FACES[i].slug, FACES[i].file);
        if (FT_New_Face(g_ft, path, 0, &g_face[i]) == 0)
            loaded++;
        else {
            g_face[i] = NULL;
            fprintf(stderr, "font missing: %s\n", path);
        }
    }
    return loaded > 0 ? 0 : -1;
}

void text_shutdown(void) {
    for (int i = 0; i < GLYPH_CACHE; i++) {
        free(g_cache[i].bitmap);
        g_cache[i] = (GlyphEntry){0};
    }
    for (int i = 0; i < FACE_COUNT; i++)
        if (g_face[i]) FT_Done_Face(g_face[i]);
    if (g_ft) FT_Done_FreeType(g_ft);
    g_ft = NULL;
}

float font_native(int face) {
    return (face >= 0 && face < FACE_COUNT) ? FACES[face].native : 0.0f;
}

float grid_size(float want, float native) {
    if (native <= 0.0f) return want;
    float mult = roundf(want / native);
    if (mult < 1.0f) mult = 1.0f;
    return mult * native;
}

FontId ui_font(float want) {
    return (FontId){UI_FACE, grid_size(want, 9.0f)};
}

FontId readout_font(float want) {
    return (FontId){READOUT_FACE, grid_size(want, 16.0f)};
}

static GlyphEntry *lookup(int face, int px, uint32_t cp) {
    /* try the face, then the fallback face */
    for (int attempt = 0; attempt < 2; attempt++) {
        int f = attempt == 0 ? face : FALLBACK_FACE;
        FT_Face ft = (f >= 0 && f < FACE_COUNT) ? g_face[f] : NULL;
        if (!ft) continue;
        if (attempt == 1 && FT_Get_Char_Index(ft, cp) == 0) continue;
        if (attempt == 0 && FT_Get_Char_Index(ft, cp) == 0 && g_face[FALLBACK_FACE]
            && FT_Get_Char_Index(g_face[FALLBACK_FACE], cp) != 0)
            continue;
        uint32_t h = ((uint32_t)f * 31u + (uint32_t)px) * 2654435761u ^ cp;
        for (uint32_t probe = 0; probe < GLYPH_CACHE; probe++) {
            GlyphEntry *e = &g_cache[(h + probe) % GLYPH_CACHE];
            if (e->used && e->face == f && e->px == px && e->cp == cp) return e;
            if (!e->used) {
                if (FT_Set_Pixel_Sizes(ft, 0, (FT_UInt)px)) return NULL;
                if (FT_Load_Char(ft, cp, FT_LOAD_RENDER)) return NULL;
                FT_GlyphSlot s = ft->glyph;
                e->face = f;
                e->px = px;
                e->cp = cp;
                e->w = (int)s->bitmap.width;
                e->h = (int)s->bitmap.rows;
                e->left = s->bitmap_left;
                e->top = s->bitmap_top;
                e->advance = (float)(s->advance.x >> 6);
                e->bitmap = malloc((size_t)e->w * e->h);
                for (int r = 0; r < e->h; r++)
                    memcpy(e->bitmap + (size_t)r * e->w,
                           s->bitmap.buffer + (size_t)r * s->bitmap.pitch,
                           (size_t)e->w);
                e->used = true;
                return e;
            }
        }
        return NULL;
    }
    return NULL;
}

static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t cp = *s;
    int len = 1;
    if (cp >= 0xF0) {
        cp = (cp & 0x07) << 18 | (s[1] & 0x3F) << 12 | (s[2] & 0x3F) << 6
             | (s[3] & 0x3F);
        len = 4;
    } else if (cp >= 0xE0) {
        cp = (cp & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        len = 3;
    } else if (cp >= 0xC0) {
        cp = (cp & 0x1F) << 6 | (s[1] & 0x3F);
        len = 2;
    }
    *p += len;
    return cp;
}

static void face_metrics(int face, int px, float *ascent, float *descent) {
    FT_Face ft = (face >= 0 && face < FACE_COUNT && g_face[face])
                     ? g_face[face]
                     : g_face[FALLBACK_FACE];
    if (!ft) {
        *ascent = (float)px;
        *descent = 0;
        return;
    }
    FT_Set_Pixel_Sizes(ft, 0, (FT_UInt)px);
    *ascent = (float)(ft->size->metrics.ascender >> 6);
    *descent = (float)(-(ft->size->metrics.descender >> 6));
}

float text_row_height(FontId f) {
    float a, d;
    face_metrics(f.face, (int)lroundf(f.px), &a, &d);
    return a + d;
}

float glyph_width(FontId f, uint32_t cp) {
    GlyphEntry *e = lookup(f.face, (int)lroundf(f.px), cp);
    return e ? e->advance : f.px * 0.5f;
}

float text_width(FontId f, const char *s, float letter_spacing) {
    float w = 0.0f;
    int n = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        GlyphEntry *e = lookup(f.face, (int)lroundf(f.px), cp);
        w += e ? e->advance : f.px * 0.5f;
        n++;
    }
    if (n > 1) w += letter_spacing * (float)(n - 1);
    return w;
}

int text_glyph_xs(FontId f, const char *s, float letter_spacing, float *xs,
                  int max) {
    float x = 0.0f;
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        xs[n] = x;
        uint32_t cp = utf8_next(&p);
        GlyphEntry *e = lookup(f.face, (int)lroundf(f.px), cp);
        x += (e ? e->advance : f.px * 0.5f) + letter_spacing;
        n++;
    }
    if (n < max + 1) xs[n] = x > 0.0f ? x - letter_spacing : 0.0f;
    return n;
}

float text_draw_glyph(Canvas *c, FontId f, P2 pos_topleft, uint32_t cp,
                      uint8_t ink) {
    int px = (int)lroundf(f.px);
    GlyphEntry *e = lookup(f.face, px, cp);
    if (!e) return f.px * 0.5f;
    float ascent, descent;
    face_metrics(f.face, px, &ascent, &descent);
    int gx = (int)lroundf(pos_topleft.x) + e->left;
    int gy = (int)lroundf(pos_topleft.y + ascent) - e->top;
    canvas_blit_a8(c, gx, gy, e->bitmap, e->w, e->h, e->w, ink);
    return e->advance;
}

Rct text_draw(Canvas *c, FontId f, P2 pos, Align anchor, const char *s,
              uint8_t ink, float letter_spacing) {
    float w = text_width(f, s, letter_spacing);
    float h = text_row_height(f);
    float x = pos.x, y = pos.y;
    switch (anchor) {
    case ALIGN_CENTER_TOP: case ALIGN_CENTER_CENTER: case ALIGN_CENTER_BOTTOM:
        x -= w * 0.5f; break;
    case ALIGN_RIGHT_TOP: case ALIGN_RIGHT_CENTER: case ALIGN_RIGHT_BOTTOM:
        x -= w; break;
    default: break;
    }
    switch (anchor) {
    case ALIGN_LEFT_CENTER: case ALIGN_CENTER_CENTER: case ALIGN_RIGHT_CENTER:
        y -= h * 0.5f; break;
    case ALIGN_LEFT_BOTTOM: case ALIGN_CENTER_BOTTOM: case ALIGN_RIGHT_BOTTOM:
        y -= h; break;
    default: break;
    }
    x = roundf(x);
    y = roundf(y);
    float cx = x;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        cx += text_draw_glyph(c, f, (P2){cx, y}, cp, ink) + letter_spacing;
    }
    return rct(x, y, x + w, y + h);
}

float text_draw_wrapped(Canvas *c, FontId f, Rct area, const char *s,
                        uint8_t ink) {
    float row_h = text_row_height(f);
    float y = area.y0;
    const char *line = s;
    char buf[1024];
    while (*line) {
        /* take words until the line would overflow or \n */
        const char *p = line;
        const char *last_fit = line;
        float w = 0.0f;
        while (*p && *p != '\n') {
            const char *word_end = p;
            while (*word_end && *word_end != ' ' && *word_end != '\n') word_end++;
            size_t len = (size_t)(word_end - line);
            if (len >= sizeof buf) len = sizeof buf - 1;
            memcpy(buf, line, len);
            buf[len] = '\0';
            float ww = text_width(f, buf, 0.0f);
            if (ww > rct_w(area) && last_fit != line) break;
            w = ww;
            last_fit = word_end;
            p = *word_end == ' ' ? word_end + 1 : word_end;
        }
        (void)w;
        size_t len = (size_t)(last_fit - line);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (c && len > 0)
            text_draw(c, f, (P2){area.x0, y}, ALIGN_LEFT_TOP, buf, ink, 0.0f);
        y += row_h;
        line = last_fit;
        while (*line == ' ') line++;
        if (*line == '\n') line++;
        if (len == 0 && *line == '\0') break;
        if (len == 0 && last_fit == line - 0 && *last_fit != '\n' && *last_fit != '\0')
            line++; /* single over-long word: force progress */
    }
    return y - area.y0;
}
