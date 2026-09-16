/* Win32 backend: a WS_CHILD window inside the host's HWND, the editor's
   output buffer blitted as a top-down 32-bit DIB. */

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>

#include <stdlib.h>
#include <string.h>

#include "plug_gui_backend.h"

#define BYPO_WNDCLASS L"BypoClapEditor"
/* The system tick is ~15.6 ms, so a 16 ms timer lands one frame per tick. */
#define FRAME_TIMER_ID 1
#define FRAME_TIMER_MS 16

typedef struct {
    HWND hwnd;
    HWND host; /* the window the host handed over */
    BITMAPINFO bmi;
    bool tracking; /* a WM_MOUSELEAVE is armed */
    void (*frame_cb)(Gui *g);
    bool frame_on;
} W32Back;

static W32Back *back_of(Gui *g) { return gui_surface(g)->back; }

static HINSTANCE self_instance(void) {
    HMODULE h = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&self_instance, &h);
    return (HINSTANCE)h;
}

/* GetDpiForWindow is Windows 10 1607 and later; 96 is the classic density and
   also what a DPI-unaware process always sees. */
static UINT win_dpi(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowFn fn =
        user32 ? (GetDpiForWindowFn)(void (*)(void))GetProcAddress(
                     user32, "GetDpiForWindow")
               : NULL;
    UINT dpi = fn ? fn(hwnd) : 0;
    return dpi ? dpi : 96;
}

/* ---------- painting ---------- */

static void blit_to(Gui *g, HDC dc) {
    W32Back *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !s->out_px) return;
    StretchDIBits(dc, 0, 0, s->win_w, s->win_h, 0, 0, s->win_w, s->win_h,
                  s->out_px, &b->bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* ---------- input ---------- */

static int vk_scancode(WPARAM vk) {
    switch (vk) {
    case VK_RETURN: return KEY_RETURN;
    case VK_ESCAPE: return KEY_ESCAPE;
    case VK_TAB: return KEY_TAB;
    case VK_BACK: return KEY_BACKSPACE;
    case VK_F2: return KEY_F2;
    case VK_HOME: return KEY_HOME;
    case VK_END: return KEY_END;
    case VK_PRIOR: return KEY_PAGEUP;
    case VK_NEXT: return KEY_PAGEDOWN;
    case VK_LEFT: return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_UP: return KEY_UP;
    case VK_DOWN: return KEY_DOWN;
    default: return -1;
    }
}

/* one UTF-16 code unit to UTF-8; surrogate halves are dropped */
static void feed_char(Gui *g, WPARAM ch) {
    unsigned c = (unsigned)ch;
    if (c < 32 || c == 127 || (c >= 0xD800 && c <= 0xDFFF)) return;
    char u8[4];
    int n;
    if (c < 0x80) {
        u8[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        u8[0] = (char)(0xC0 | (c >> 6));
        u8[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else {
        u8[0] = (char)(0xE0 | (c >> 12));
        u8[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        u8[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    }
    gui_in_text(g, u8, n);
}

static void arm_leave(Gui *g) {
    W32Back *b = back_of(g);
    if (b->tracking) return;
    TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, b->hwnd, 0};
    if (TrackMouseEvent(&tme)) b->tracking = true;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gui *g = (Gui *)(void *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!g) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        blit_to(g, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        W32Back *b = back_of(g);
        if (wp == FRAME_TIMER_ID && b && b->frame_cb) b->frame_cb(g);
        return 0;
    }
    case WM_ERASEBKGND: return 1; /* every pixel is painted anyway */
    case WM_SIZE: return 0;
    case WM_MOUSEMOVE:
        arm_leave(g);
        gui_in_motion(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        back_of(g)->tracking = false;
        gui_in_inside(g, false);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        gui_in_motion(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        gui_in_button(g, 1, true);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        gui_in_motion(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        gui_in_button(g, 1, false);
        return 0;
    case WM_RBUTTONDOWN:
        gui_in_motion(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        gui_in_button(g, 3, true);
        return 0;
    case WM_RBUTTONUP:
        gui_in_motion(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        gui_in_button(g, 3, false);
        return 0;
    case WM_MOUSEWHEEL:
        /* positive is away from the user; the editor counts down as positive */
        gui_in_wheel(g, -(float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int sc = vk_scancode(wp);
        if (sc >= 0) gui_in_key(g, sc, true);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int sc = vk_scancode(wp);
        if (sc >= 0) gui_in_key(g, sc, false);
        return 0;
    }
    case WM_CHAR:
        feed_char(g, wp);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool register_class(void) {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = self_instance();
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = BYPO_WNDCLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    done = true;
    return true;
}

/* ---------- backend interface ---------- */

bool backend_scales_itself(void) { return false; }

bool backend_open(Gui *g) {
    W32Back *b = calloc(1, sizeof *b);
    if (!b) return false;
    b->bmi.bmiHeader.biSize = sizeof b->bmi.bmiHeader;
    b->bmi.bmiHeader.biPlanes = 1;
    b->bmi.bmiHeader.biBitCount = 32;
    b->bmi.bmiHeader.biCompression = BI_RGB;
    gui_surface(g)->back = b;
    return true;
}

void backend_close(Gui *g) {
    W32Back *b = back_of(g);
    if (!b) return;
    if (b->hwnd) {
        SetWindowLongPtrW(b->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(b->hwnd);
    }
    free(b);
    gui_surface(g)->back = NULL;
}

void backend_usable_screen(Gui *g, int *w, int *h) {
    RECT r;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) {
        *w = (int)(r.right - r.left);
        *h = (int)(r.bottom - r.top);
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

float backend_px_per_point(Gui *g) { return 1.0f; }

bool backend_host_size(Gui *g, int *w, int *h) {
    W32Back *b = back_of(g);
    RECT r;
    if (!b || !b->host || !GetClientRect(b->host, &r)) return false;
    *w = (int)(r.right - r.left);
    *h = (int)(r.bottom - r.top);
    return true;
}

bool backend_attach(Gui *g, const clap_window_t *window) {
    W32Back *b = back_of(g);
    if (!b || b->hwnd || !register_class()) return false;
    HWND parent = (HWND)window->win32;
    b->host = parent;
    b->hwnd = CreateWindowExW(0, BYPO_WNDCLASS, L"", WS_CHILD, 0, 0, 1, 1,
                              parent, NULL, self_instance(), NULL);
    if (!b->hwnd) return false;
    SetWindowLongPtrW(b->hwnd, GWLP_USERDATA, (LONG_PTR)(void *)g);
    /* a host that never calls set_scale still tells us the monitor density */
    UINT dpi = win_dpi(b->hwnd);
    GuiSurface *s = gui_surface(g);
    if (dpi != 96 && s->host_scale == 1.0f) s->host_scale = (float)dpi / 96.0f;
    return true;
}

bool backend_resize(Gui *g) {
    W32Back *b = back_of(g);
    GuiSurface *s = gui_surface(g);
    if (!b || !b->hwnd || !s->out_px) return false;
    b->bmi.bmiHeader.biWidth = s->win_w;
    b->bmi.bmiHeader.biHeight = -s->win_h; /* negative: top-down rows */
    SetWindowPos(b->hwnd, NULL, 0, 0, s->win_w, s->win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    return true;
}

void backend_present(Gui *g) {
    W32Back *b = back_of(g);
    if (!b || !b->hwnd) return;
    HDC dc = GetDC(b->hwnd);
    if (!dc) return;
    blit_to(g, dc);
    ReleaseDC(b->hwnd, dc);
}

void backend_show(Gui *g) {
    W32Back *b = back_of(g);
    if (b && b->hwnd) ShowWindow(b->hwnd, SW_SHOWNA);
}

void backend_hide(Gui *g) {
    W32Back *b = back_of(g);
    if (b && b->hwnd) ShowWindow(b->hwnd, SW_HIDE);
}

/* input arrives through the window procedure on the host's message loop */
int backend_event_fd(Gui *g) { return -1; }

void backend_pump(Gui *g) {}

/* WM_TIMER on the child window: the host's message loop dispatches it to
   wndproc, so the frame runs on the thread that owns the window. */
bool backend_start_frame_timer(Gui *g, void (*cb)(Gui *g)) {
    W32Back *b = back_of(g);
    if (!b || !b->hwnd) return false;
    if (b->frame_on) return true;
    if (!SetTimer(b->hwnd, FRAME_TIMER_ID, FRAME_TIMER_MS, NULL)) return false;
    b->frame_cb = cb;
    b->frame_on = true;
    return true;
}

void backend_stop_frame_timer(Gui *g) {
    W32Back *b = back_of(g);
    if (!b || !b->frame_on) return;
    KillTimer(b->hwnd, FRAME_TIMER_ID);
    b->frame_cb = NULL;
    b->frame_on = false;
}
