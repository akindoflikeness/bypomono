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
    {"byposerif", "BYPOSerif.otb", 0},
    {"unifontexmono", "UnifontExMono.ttf", 16},
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
    int from_file = 0, from_mem = 0;
    for (int i = 0; i < FACE_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/fonts/%s/%s", assets_dir,
                 FACES[i].slug, FACES[i].file);
        g_face[i] = NULL;
        if (FT_New_Face(g_ft, path, 0, &g_face[i]) == 0) {
            from_file++;
            continue;
        }
        g_face[i] = NULL;
        size_t n = 0;
        const unsigned char *data = embedded_face(i, &n);
        if (data && n
            && FT_New_Memory_Face(g_ft, data, (FT_Long)n, 0, &g_face[i]) == 0)
            from_mem++;
        else
            g_face[i] = NULL;
    }
    if (from_file && from_mem)
        fprintf(stderr, "fonts: %d from %s/fonts, %d embedded\n", from_file,
                assets_dir, from_mem);
    else if (from_mem)
        fprintf(stderr, "fonts: embedded\n");
    else if (from_file)
        fprintf(stderr, "fonts: %s/fonts\n", assets_dir);
    else
        fprintf(stderr, "fonts: none, looked under %s/fonts\n", assets_dir);
    return from_file + from_mem > 0 ? 0 : -1;
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

/* BYPOSerif is hand-drawn at these sizes only and has no outlines */
static const float UI_SIZES[] = {10, 11, 12, 13, 16, 18};

/* the first drawn size above the one asked for, so text runs a step large */
FontId ui_font(float want) {
    size_t n = sizeof UI_SIZES / sizeof UI_SIZES[0];
    float asked = roundf(want);
    for (size_t i = 0; i < n; i++)
        if (UI_SIZES[i] > asked) return (FontId){UI_FACE, UI_SIZES[i]};
    return (FontId){UI_FACE, UI_SIZES[n - 1]};
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
                for (int r = 0; r < e->h; r++) {
                    const uint8_t *row =
                        s->bitmap.buffer + (size_t)r * s->bitmap.pitch;
                    uint8_t *dst = e->bitmap + (size_t)r * e->w;
                    /* embedded bitmap strikes come back one bit per pixel */
                    if (s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                        for (int x = 0; x < e->w; x++)
                            dst[x] = (row[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0;
                    else
                        memcpy(dst, row, (size_t)e->w);
                }
                e->used = true;
                return e;
            }
        }
        return NULL;
    }
    return NULL;
}

/* stops at any byte that is not a continuation, so a truncated sequence
   never reads past the terminator */
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = *p;
    int extra = 0;
    if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    p++;
    while (extra-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
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
