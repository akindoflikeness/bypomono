/* The three faces compiled straight into the binary. A .clap is a single file
   on Linux and Windows with nothing installed beside it, so the plugin build
   defines BYPO_EMBED_FONTS and carries its own copies; the standalone ships
   assets/ next to the executable and gets the empty version of this file. */

#include "text.h"

#ifdef BYPO_EMBED_FONTS

/* .incbin resolves against the assembler's working directory, which is the
   repository root because that is where make runs */
#ifndef BYPO_FONT_DIR
#define BYPO_FONT_DIR "assets/fonts"
#endif

#if defined(__APPLE__)
#define EMB_SECTION ".section __TEXT,__const\n"
#define EMB_SYM(name) "_" name
#elif defined(_WIN32)
#define EMB_SECTION ".section .rdata,\"dr\"\n"
#if defined(__i386__)
#define EMB_SYM(name) "_" name
#else
#define EMB_SYM(name) name
#endif
#else
#define EMB_SECTION ".section .rodata\n"
#define EMB_SYM(name) name
#endif

/* Start and end labels around the file; the size is the difference. The
   trailing .text puts the assembler back where the compiler expects it, and
   is spelt out rather than .popsection because the PE assembler has no such
   directive. */
#define EMBED(sym, file)                                                      \
    __asm__(EMB_SECTION ".p2align 4\n"                                        \
            ".globl " EMB_SYM(#sym) "\n" EMB_SYM(#sym) ":\n"                  \
            ".incbin \"" BYPO_FONT_DIR "/" file "\"\n"                        \
            ".globl " EMB_SYM(#sym "_end") "\n" EMB_SYM(#sym "_end") ":\n"    \
            ".text\n")

EMBED(bypo_font_ui, "byposerif/BYPOSerif.otb");
EMBED(bypo_font_fallback, "unifontexmono/UnifontExMono.ttf");
EMBED(bypo_font_readout, "european_teletext/EuropeanTeletext.ttf");

extern const unsigned char bypo_font_ui[], bypo_font_ui_end[];
extern const unsigned char bypo_font_fallback[], bypo_font_fallback_end[];
extern const unsigned char bypo_font_readout[], bypo_font_readout_end[];

const unsigned char *embedded_face(int face, size_t *len) {
    const unsigned char *start, *end;
    switch (face) {
    case FACE_BYPOSERIF:
        start = bypo_font_ui; end = bypo_font_ui_end; break;
    case FACE_UNIFONTEXMONO:
        start = bypo_font_fallback; end = bypo_font_fallback_end; break;
    case FACE_EUROPEAN_TELETEXT:
        start = bypo_font_readout; end = bypo_font_readout_end; break;
    default:
        return NULL;
    }
    *len = (size_t)(end - start);
    return start;
}

#else

const unsigned char *embedded_face(int face, size_t *len) {
    (void)face;
    (void)len;
    return NULL;
}

#endif
