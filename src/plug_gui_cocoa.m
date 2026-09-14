/* Cocoa backend: an NSView subview of the host's view. The editor's output
   buffer is device pixels; the view frame is points, so it is divided by the
   backing scale factor before it becomes a frame. No ARC. */

#import <Cocoa/Cocoa.h>

#include <stdlib.h>
#include <string.h>

#include "plug_gui_backend.h"

/* hardware key codes, kHIDUsage / Carbon kVK_* values */
enum {
    KC_RETURN = 36,
    KC_TAB = 48,
    KC_DELETE = 51,
    KC_ESCAPE = 53,
    KC_KP_ENTER = 76,
    KC_F2 = 120
};

@interface BypoView : NSView {
    Gui *gui;
    NSTrackingArea *area;
}
- (void)setGui:(Gui *)g;
@end

typedef struct {
    BypoView *view;
} CocoaBack;

static CocoaBack *back_of(Gui *g) { return (CocoaBack *)gui_surface(g)->back; }

static CGFloat backing_of(BypoView *v) {
    NSWindow *w = v ? [v window] : nil;
    CGFloat s = w ? [w backingScaleFactor] : [[NSScreen mainScreen] backingScaleFactor];
    return s > 0.0 ? s : 1.0;
}

@implementation BypoView

- (void)setGui:(Gui *)g { gui = g; }
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)ev { return YES; }

- (void)updateTrackingAreas {
    if (area) {
        [self removeTrackingArea:area];
        [area release];
        area = nil;
    }
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited
                                 | NSTrackingMouseMoved | NSTrackingActiveAlways
                                 | NSTrackingInVisibleRect;
    area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                        options:opts
                                          owner:self
                                       userInfo:nil];
    [self addTrackingArea:area];
    [super updateTrackingAreas];
}

- (void)dealloc {
    if (area) {
        [self removeTrackingArea:area];
        [area release];
    }
    [super dealloc];
}

- (void)drawRect:(NSRect)dirty {
    GuiSurface *s = gui ? gui_surface(gui) : NULL;
    if (!s || !s->out_px) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) return;
    size_t w = (size_t)s->win_w, h = (size_t)s->win_h;
    CGDataProviderRef prov =
        CGDataProviderCreateWithData(NULL, s->out_px, w * h * 4, NULL);
    if (!prov) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* the buffer is XRGB words, so BGRX bytes on a little-endian machine */
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                   kCGImageAlphaNoneSkipFirst
                                       | kCGBitmapByteOrder32Little,
                                   prov, NULL, false, kCGRenderingIntentDefault);
    NSRect b = [self bounds];
    if (img) {
        CGContextSaveGState(ctx);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        /* the view is flipped; undo that for CGImage's bottom-left origin */
        CGContextTranslateCTM(ctx, 0.0, NSHeight(b));
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, CGRectMake(0, 0, NSWidth(b), NSHeight(b)), img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
    }
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);
}

/* ---------- input ---------- */

- (void)feedMotion:(NSEvent *)ev {
    NSPoint p = [self convertPoint:[ev locationInWindow] fromView:nil];
    CGFloat s = backing_of(self);
    gui_in_motion(gui, (int)(p.x * s), (int)(p.y * s));
}

- (void)mouseMoved:(NSEvent *)ev { [self feedMotion:ev]; }
- (void)mouseDragged:(NSEvent *)ev { [self feedMotion:ev]; }
- (void)rightMouseDragged:(NSEvent *)ev { [self feedMotion:ev]; }
- (void)mouseEntered:(NSEvent *)ev { gui_in_inside(gui, true); }
- (void)mouseExited:(NSEvent *)ev { gui_in_inside(gui, false); }

- (void)mouseDown:(NSEvent *)ev {
    [[self window] makeFirstResponder:self];
    [self feedMotion:ev];
    gui_in_button(gui, 1, true);
}

- (void)mouseUp:(NSEvent *)ev {
    [self feedMotion:ev];
    gui_in_button(gui, 1, false);
}

- (void)rightMouseDown:(NSEvent *)ev {
    [self feedMotion:ev];
    gui_in_button(gui, 3, true);
}

- (void)rightMouseUp:(NSEvent *)ev {
    [self feedMotion:ev];
    gui_in_button(gui, 3, false);
}

- (void)scrollWheel:(NSEvent *)ev {
    /* AppKit counts up as positive; the editor counts down as positive */
    CGFloat d = [ev scrollingDeltaY];
    if ([ev hasPreciseScrollingDeltas]) d /= 10.0;
    gui_in_wheel(gui, (float)-d);
}

- (int)scancodeOf:(NSEvent *)ev {
    switch ([ev keyCode]) {
    case KC_RETURN:
    case KC_KP_ENTER: return KEY_RETURN;
    case KC_ESCAPE: return KEY_ESCAPE;
    case KC_TAB: return KEY_TAB;
    case KC_DELETE: return KEY_BACKSPACE;
    case KC_F2: return KEY_F2;
    default: return -1;
    }
}

- (void)keyDown:(NSEvent *)ev {
    int sc = [self scancodeOf:ev];
    if (sc >= 0) gui_in_key(gui, sc, true);
    NSString *s = [ev characters];
    const char *u8 = s ? [s UTF8String] : NULL;
    if (u8 && (unsigned char)u8[0] >= 32 && u8[0] != 127)
        gui_in_text(gui, u8, (int)strlen(u8));
}

- (void)keyUp:(NSEvent *)ev {
    int sc = [self scancodeOf:ev];
    if (sc >= 0) gui_in_key(gui, sc, false);
}

@end

/* ---------- backend interface ---------- */

bool backend_open(Gui *g) {
    CocoaBack *b = calloc(1, sizeof *b);
    if (!b) return false;
    gui_surface(g)->back = b;
    return true;
}

void backend_close(Gui *g) {
    CocoaBack *b = back_of(g);
    if (!b) return;
    if (b->view) {
        [b->view removeFromSuperview];
        [b->view release];
    }
    free(b);
    gui_surface(g)->back = NULL;
}

void backend_usable_screen(Gui *g, int *w, int *h) {
    NSScreen *s = [NSScreen mainScreen];
    NSRect vf = s ? [s visibleFrame] : NSMakeRect(0, 0, 0, 0);
    CGFloat f = s ? [s backingScaleFactor] : 1.0;
    if (f <= 0.0) f = 1.0;
    *w = (int)(NSWidth(vf) * f);
    *h = (int)(NSHeight(vf) * f);
}

float backend_px_per_point(Gui *g) {
    CocoaBack *b = back_of(g);
    return (float)backing_of(b ? b->view : nil);
}

bool backend_attach(Gui *g, const clap_window_t *window) {
    CocoaBack *b = back_of(g);
    if (!b || b->view) return false;
    NSView *parent = (NSView *)window->cocoa;
    if (!parent) return false;
    b->view = [[BypoView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    if (!b->view) return false;
    [b->view setGui:g];
    [parent addSubview:b->view];
    return true;
}

bool backend_resize(Gui *g) {
    CocoaBack *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->view || !s->out_px) return false;
    CGFloat bs = backing_of(b->view);
    [b->view setFrame:NSMakeRect(0, 0, (CGFloat)s->win_w / bs,
                                 (CGFloat)s->win_h / bs)];
    [b->view setNeedsDisplay:YES];
    return true;
}

void backend_present(Gui *g) {
    CocoaBack *b = back_of(g);
    if (!b || !b->view) return;
    [b->view setNeedsDisplay:YES];
    [b->view displayIfNeeded];
}

void backend_show(Gui *g) {
    CocoaBack *b = back_of(g);
    if (b && b->view) [b->view setHidden:NO];
}

void backend_hide(Gui *g) {
    CocoaBack *b = back_of(g);
    if (b && b->view) [b->view setHidden:YES];
}

/* input arrives through the NSView on the host's run loop */
int backend_event_fd(Gui *g) { return -1; }

void backend_pump(Gui *g) {}
