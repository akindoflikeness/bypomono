/* Cocoa backend: a layer-backed NSView subview of the host's view. The core
   hands over the 1:1 design canvas and Core Animation magnifies it on the GPU
   with nearest-neighbour filtering, so nothing scales on the host's main
   thread. The view frame is points, the surface size device pixels. No ARC. */

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <stdatomic.h>
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
    KC_HOME = 115,
    KC_PAGEUP = 116,
    KC_END = 119,
    KC_F2 = 120,
    KC_PAGEDOWN = 121,
    KC_LEFT = 123,
    KC_RIGHT = 124,
    KC_DOWN = 125,
    KC_UP = 126
};

@interface BypoView : NSView {
    Gui *gui;
    NSTrackingArea *area;
}
- (void)setGui:(Gui *)g;
@end

typedef struct FrameLink FrameLink; /* the display link, at end of file */

typedef struct {
    BypoView *view;
    FrameLink *fl; /* NULL unless a display link is running */
    /* Two present buffers used in turn. The layer keeps the image from the
       last present, and that image reads its bytes straight out of one of
       these, so the next frame is written into the other one. */
    unsigned char *buf[2];
    size_t buf_bytes;
    int next;
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
    case KC_HOME: return KEY_HOME;
    case KC_END: return KEY_END;
    case KC_PAGEUP: return KEY_PAGEUP;
    case KC_PAGEDOWN: return KEY_PAGEDOWN;
    case KC_LEFT: return KEY_LEFT;
    case KC_RIGHT: return KEY_RIGHT;
    case KC_UP: return KEY_UP;
    case KC_DOWN: return KEY_DOWN;
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

bool backend_scales_itself(void) { return true; }

bool backend_open(Gui *g) {
    CocoaBack *b = calloc(1, sizeof *b);
    if (!b) return false;
    gui_surface(g)->back = b;
    return true;
}

void backend_close(Gui *g) {
    CocoaBack *b = back_of(g);
    if (!b) return;
    backend_stop_frame_timer(g);
    if (b->view) {
        /* drop the image before the buffer it reads from goes away */
        [[b->view layer] setContents:nil];
        [b->view removeFromSuperview];
        [b->view release];
    }
    free(b->buf[0]);
    free(b->buf[1]);
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

bool backend_host_size(Gui *g, int *w, int *h) {
    CocoaBack *b = back_of(g);
    NSView *parent = (b && b->view) ? [b->view superview] : nil;
    if (!parent) return false;
    NSRect r = [parent bounds];
    CGFloat s = backing_of(b->view);
    *w = (int)(NSWidth(r) * s);
    *h = (int)(NSHeight(r) * s);
    return true;
}

bool backend_attach(Gui *g, const clap_window_t *window) {
    CocoaBack *b = back_of(g);
    if (!b || b->view) return false;
    NSView *parent = (NSView *)window->cocoa;
    if (!parent) return false;
    b->view = [[BypoView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    if (!b->view) return false;
    [b->view setGui:g];
    [b->view setWantsLayer:YES];
    CALayer *l = [b->view layer];
    [l setMagnificationFilter:kCAFilterNearest];
    [l setMinificationFilter:kCAFilterNearest];
    [l setContentsGravity:kCAGravityResize];
    [l setOpaque:YES];
    /* a leaf layer: nothing is positioned in its coordinates, so keep the
       contents upright whatever the flipped view asks for */
    [l setGeometryFlipped:NO];
    [parent addSubview:b->view];
    return true;
}

bool backend_resize(Gui *g) {
    CocoaBack *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->view || !s->src_px) return false;
    CGFloat bs = backing_of(b->view);
    [b->view setFrame:NSMakeRect(0, 0, (CGFloat)s->win_w / bs,
                                 (CGFloat)s->win_h / bs)];
    [[b->view layer] setContentsScale:bs];
    return true;
}

void backend_present(Gui *g) {
    CocoaBack *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->view || !s->src_px) return;
    size_t w = (size_t)s->src_w, h = (size_t)s->src_h;
    size_t bytes = w * h * 4;
    if (bytes != b->buf_bytes) {
        free(b->buf[0]);
        free(b->buf[1]);
        b->buf[0] = malloc(bytes);
        b->buf[1] = malloc(bytes);
        b->next = 0;
        b->buf_bytes = bytes;
        if (!b->buf[0] || !b->buf[1]) {
            free(b->buf[0]);
            free(b->buf[1]);
            b->buf[0] = b->buf[1] = NULL;
            b->buf_bytes = 0;
            return;
        }
    }
    /* the layer reads its contents whenever it likes, so hand it a private
       copy rather than the canvas the core keeps drawing into */
    unsigned char *px = b->buf[b->next];
    b->next ^= 1;
    memcpy(px, s->src_px, bytes);
    /* the provider is thrown away with the image it backs, so the buffer is
       free again one present later, which is what the pair is for */
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, px, bytes, NULL);
    if (!prov) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* the buffer is XRGB words, so BGRX bytes on a little-endian machine */
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                   kCGImageAlphaNoneSkipFirst
                                       | kCGBitmapByteOrder32Little,
                                   prov, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);
    if (!img) return;
    [[b->view layer] setContents:(id)img];
    CGImageRelease(img);
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

/* ---------- display link ---------- */
/* CVDisplayLink is deprecated from macOS 15 and the NSView call that replaces
   it is 14 and later, while this builds back to 12. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct FrameLink {
    CVDisplayLinkRef link;
    Gui *gui;
    void (*cb)(Gui *g);
    atomic_bool queued; /* a frame is already waiting on the main queue */
    atomic_bool alive;  /* cleared by stop; a queued block then frees this */
};

/* Runs on the link's own thread, so all it does is hand the frame to the main
   queue. Rendering never happens here. */
static CVReturn frame_tick(CVDisplayLinkRef link, const CVTimeStamp *now,
                           const CVTimeStamp *out, CVOptionFlags flags,
                           CVOptionFlags *out_flags, void *ctx) {
    FrameLink *fl = (FrameLink *)ctx;
    /* a frame slower than the refresh must not build a backlog */
    if (atomic_exchange(&fl->queued, true)) return kCVReturnSuccess;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!atomic_load(&fl->alive)) {
            free(fl); /* stop ran while this was in flight */
            return;
        }
        atomic_store(&fl->queued, false);
        void (*cb)(Gui *) = fl->cb;
        Gui *g = fl->gui;
        cb(g); /* read both out first: the frame may stop the link */
    });
    return kCVReturnSuccess;
}

bool backend_start_frame_timer(Gui *g, void (*cb)(Gui *g)) {
    CocoaBack *b = back_of(g);
    if (!b || !b->view) return false;
    if (b->fl) return true;
    FrameLink *fl = calloc(1, sizeof *fl);
    if (!fl) return false;
    fl->gui = g;
    fl->cb = cb;
    atomic_init(&fl->queued, false);
    atomic_init(&fl->alive, true);
    if (CVDisplayLinkCreateWithActiveCGDisplays(&fl->link) != kCVReturnSuccess
        || !fl->link) {
        free(fl);
        return false;
    }
    if (CVDisplayLinkSetOutputCallback(fl->link, frame_tick, fl)
            != kCVReturnSuccess
        || CVDisplayLinkStart(fl->link) != kCVReturnSuccess) {
        CVDisplayLinkRelease(fl->link);
        free(fl);
        return false;
    }
    b->fl = fl;
    return true;
}

void backend_stop_frame_timer(Gui *g) {
    CocoaBack *b = back_of(g);
    if (!b || !b->fl) return;
    FrameLink *fl = b->fl;
    b->fl = NULL;
    CVDisplayLinkStop(fl->link); /* no further callback after this returns */
    CVDisplayLinkRelease(fl->link);
    fl->link = NULL;
    /* this is the main thread, so a queued block is waiting, not running:
       leave the free to it and do it here only when there is none */
    atomic_store(&fl->alive, false);
    if (!atomic_load(&fl->queued)) free(fl);
}

#pragma clang diagnostic pop
