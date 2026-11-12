// The EGL calls whose meaning differs between Android and Windows (gen_gl.c holds the plain ones).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include "shim.h"

void *shim_lookup_gl(const char *name), *shim_gl_host(const char *name);

// Function pointers handed to the game must take the SysV convention, so they come from our wrapper table.
static SYSV void *s_eglGetProcAddress(const char *name) {
    void *shim_lookup_egl_hand(const char *);
    void *w = shim_lookup_egl_hand(name);
    if (!w) w = shim_lookup_gl(name);
    if (!w) printf("eglGetProcAddress(%s): no wrapper\n", name);
    return w;
}
static SYSV EGLDisplay s_eglGetDisplay(EGLNativeDisplayType d) {
    EGLDisplay (__stdcall *f)(EGLNativeDisplayType) = shim_gl_host("eglGetDisplay");
    return f ? f(d) : EGL_NO_DISPLAY;  // Android passes EGL_DEFAULT_DISPLAY (0), same value on Windows
}
// SKATESTRAP_FPS_CAP set: vsync off, so the display rate does not cap the frame rate (Mocktail kUnthrottled)
static SYSV EGLBoolean s_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    EGLBoolean (__stdcall *f)(EGLDisplay, EGLint) = shim_gl_host("eglSwapInterval");
    return f ? f(dpy, getenv("SKATESTRAP_FPS_CAP") ? 0 : interval) : EGL_FALSE;
}
// The game passes an ANativeWindow; ours carries the Win32 window it stands for.
static SYSV EGLSurface s_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, SkateWindow *win, const EGLint *attrs) {
    EGLSurface (__stdcall *f)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *) = shim_gl_host("eglCreateWindowSurface");
    if (!f || !win || win->magic != SKATE_WINDOW_MAGIC) { printf("eglCreateWindowSurface: not one of our windows\n"); return EGL_NO_SURFACE; }
    return f(dpy, cfg, (EGLNativeWindowType)win->hwnd, attrs);
}

// The one game window. ANativeWindow_fromSurface hands it out for any Surface (there is only one).
static SkateWindow g_win = { SKATE_WINDOW_MAGIC, 1 };
// SKATESTRAP_RENDER_SCALE=0.25..0.99 with Vulkan: the game sees a window this much smaller and renders at that size;
// vk_hand.c stretches each frame onto the real swapchain, start-up scales mouse positions. 1 = off.
float skate_render_scale(void) {
    static float s = -1;
    if (s < 0) { const char *e = getenv("SKATESTRAP_RENDER_SCALE"); s = e && getenv("SKATESTRAP_VULKAN") ? (float)atof(e) : 1; if (!(s >= 0.25f && s < 1)) s = 1; }
    return s;
}
static int scaled(int v) { int r = (int)(v * skate_render_scale() + 0.5f); return r > 0 ? r : 1; }
int (*skate_input)(UINT m, WPARAM wp, LPARAM lp);  // set by start-up once Roblox can take input; nonzero = handled
// Resize: WM_SIZE only updates the window size here. skate_resize_pending() lets start-up pick up a settled new size
// and re-send it to the engine (Mocktail's nativeAppBridgeV2UpdateSurface*WithPlatformParams), which recreates the
// render view at that size. Without this the window grows but the game keeps rendering at the start-up size.
static volatile LONG g_sizing;            // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (a live drag)
static int g_applied_w, g_applied_h;      // the size the engine was last told
// Fullscreen: borderless window over the whole monitor (covers the taskbar); F11 or Alt+Enter switches,
// SKATESTRAP_FULLSCREEN=1 starts in it. The WM_SIZE it causes goes through the normal resize path.
static WINDOWPLACEMENT g_place = { sizeof g_place };
static int g_full;
static void set_fullscreen(HWND h, int on) {
    if (on == g_full) return;
    g_full = on;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(h, &g_place);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(h, GWL_STYLE, (GetWindowLongPtrW(h, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        RECT r = mi.rcMonitor;
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, (GetWindowLongPtrW(h, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_place);
        SetWindowPos(h, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}
static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if ((m == WM_KEYDOWN && wp == VK_F11) || (m == WM_SYSKEYDOWN && wp == VK_RETURN)) {
        if (!(lp & (1 << 30))) set_fullscreen(h, !g_full);  // (not on key repeat)
        return 0;
    }
    if ((m == WM_KEYUP && wp == VK_F11) || (m == WM_SYSKEYUP && wp == VK_RETURN)) return 0;
    if (m == WM_SYSCHAR && wp == VK_RETURN) return 0;  // no beep on Alt+Enter
    // Roblox draws its own cursor in the game picture; hide the Windows one there so there are not two (borders keep the resize arrows)
    if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(0); return TRUE; }
    if (skate_input && skate_input(m, wp, lp)) return 0;
    if (m == WM_ENTERSIZEMOVE) g_sizing = 1;
    if (m == WM_EXITSIZEMOVE) g_sizing = 0;
    if (m == WM_SIZE && wp != SIZE_MINIMIZED) { g_win.width = scaled(LOWORD(lp)); g_win.height = scaled(HIWORD(lp)); }
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, wp, lp);
}
void *skate_window_hwnd(void) { return g_win.hwnd; }
// 1 (with w,h filled) when the window has settled at a size the engine was not yet told about; 0 otherwise.
int skate_resize_pending(int *w, int *h) {
    if (g_sizing) return 0;                                    // wait until the drag ends
    int cw = g_win.width, ch = g_win.height;
    if (cw < 2 || ch < 2) return 0;                            // minimized / not ready
    if (cw == g_applied_w && ch == g_applied_h) return 0;
    g_applied_w = cw; g_applied_h = ch; *w = cw; *h = ch; return 1;
}
SkateWindow *skate_window_create(int w, int h) {
    if (g_win.hwnd) return &g_win;
    WNDCLASSW wc = { .style = CS_OWNDC, .lpfnWndProc = win_proc, .hInstance = GetModuleHandleW(0), .lpszClassName = L"Skatestrap",
                     .hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW) };
    RegisterClassW(&wc);
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);
    g_win.hwnd = CreateWindowW(L"Skatestrap", L"Skatestrap", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    g_win.width = scaled(w); g_win.height = scaled(h);
    g_applied_w = g_win.width; g_applied_h = g_win.height;  // start-up's own UpdateSurface covers this first size
    { const char *f = getenv("SKATESTRAP_FULLSCREEN"); if (f && *f == '1') set_fullscreen(g_win.hwnd, 1); }  // (resize path re-sends the size)
    return &g_win;
}
static SYSV SkateWindow *s_ANativeWindow_fromSurface(void *env, void *surface) {
    if (!g_win.hwnd) { printf("ANativeWindow_fromSurface: no window yet\n"); return 0; }
    static LONG said; if (!InterlockedExchange(&said, 1)) printf("ANativeWindow_fromSurface -> window %dx%d\n", g_win.width, g_win.height);
    InterlockedIncrement((volatile LONG *)&g_win.refs);
    return &g_win;
}
static SYSV void s_ANativeWindow_acquire(SkateWindow *w) { if (w) InterlockedIncrement((volatile LONG *)&w->refs); }
static SYSV void s_ANativeWindow_release(SkateWindow *w) { if (w) InterlockedDecrement((volatile LONG *)&w->refs); }  // the window outlives its refs
static SYSV int32_t s_ANativeWindow_getWidth(SkateWindow *w) { return w ? w->width : -1; }
static SYSV int32_t s_ANativeWindow_getHeight(SkateWindow *w) { return w ? w->height : -1; }

// SKATESTRAP_FPS: glMapBufferRange calls by access flags (count, ms) -- ANGLE stalls here when it must sync with the GPU
static volatile LONG map_n[64]; static volatile long long map_ns[64];
// ANGLE (D3D11) takes ~0.1 ms per map even with UNSYNCHRONIZED. A write-only map of an invalidated range needs
// none of the old bytes, so hand out CPU scratch and upload it with glBufferSubData at unmap (SKATESTRAP_MAP_SHADOW=1).
// One pending map per target, as GL allows; render thread only (single GL context).
static struct { GLenum t; GLintptr o; GLsizeiptr len, cap; void *p; } shadow[16];
static void *shadow_map(GLenum t, GLintptr o, GLsizeiptr len, GLbitfield a) {
    static int on = -1; if (on < 0) on = getenv("SKATESTRAP_MAP_SHADOW") != 0;
    // whole-buffer invalidate stays on ANGLE: it orphans cheaply. Explicit flush would hit an unmapped buffer.
    if (!on || (a & (GL_MAP_READ_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) || !(a & GL_MAP_INVALIDATE_RANGE_BIT) || len <= 0) return 0;
    int i = 0; while (i < 16 && shadow[i].t && shadow[i].t != t) i++;
    if (i == 16) return 0;
    if (shadow[i].cap < len) { free(shadow[i].p); shadow[i].p = malloc(len); shadow[i].cap = shadow[i].p ? len : 0; if (!shadow[i].p) return 0; }
    shadow[i].t = t; shadow[i].o = o; shadow[i].len = len;
    return shadow[i].p;
}
static int shadow_unmap(GLenum t) {
    for (int i = 0; i < 16; i++) if (shadow[i].t == t && shadow[i].len) {
        static void (__stdcall *sub)(GLenum, GLintptr, GLsizeiptr, const void *); if (!sub) sub = shim_gl_host("glBufferSubData");
        sub(t, shadow[i].o, shadow[i].len, shadow[i].p); shadow[i].len = 0;
        return 1;
    }
    return 0;
}
static SYSV void *s_glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr len, GLbitfield a) {
    static void *(__stdcall *f)(GLenum, GLintptr, GLsizeiptr, GLbitfield); if (!f) f = shim_gl_host("glMapBufferRange");
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0); void *r = shadow_map(t, o, len, a); if (!r) r = f(t, o, len, a); QueryPerformanceCounter(&q1);
    InterlockedIncrement(&map_n[a & 63]); map_ns[a & 63] += q1.QuadPart - q0.QuadPart;
    return r;
}
static SYSV GLboolean s_glUnmapBuffer(GLenum t) {
    static GLboolean (__stdcall *f)(GLenum); if (!f) f = shim_gl_host("glUnmapBuffer");
    return shadow_unmap(t) ? GL_TRUE : f(t);
}
// frame stats every 5 s: frames/s, worst gap between frames, time inside eglSwapBuffers (SKATESTRAP_FPS=1)
volatile DWORD skate_render_tid; volatile long long skate_rw_ns, skate_rw_n, skate_rw_to;  // thread that calls eglSwapBuffers (long-wait tracing, SKATESTRAP_FPS)
volatile DWORD skate_poster_tid; volatile long long skate_last_frame_ns;
volatile DWORD skate_wk_tid[8]; volatile LONG skate_wk_n; volatile long long skate_wk_ns[8];  // last wakes of the render thread
void skate_frame(int after) {
    if (!after && getenv("SKATESTRAP_FPS")) {
        static int shown; static LONG last_n; int64_t now_ns(int); long long t = now_ns(1); static long long lastt;
        if (shown++ < 400 && shown > 200) {
            LONG n = skate_wk_n; printf("frame +%lld ms, %ld wakes since; last wakes:", (t - lastt) / 1000000, n - last_n);
            for (int i = 0; i < 6; i++) { int k = (n - i) & 7; printf(" %lu@-%lldms", skate_wk_tid[k], (t - skate_wk_ns[k]) / 1000000); }
            printf("\n"); last_n = n;
        }
        { extern volatile long long skate_gp_n, skate_gp_ns, skate_gp_max; extern char skate_gp_chain[];
          static int gaps; static long long first; if (!first) first = t;  // skip the first 40 s of frames: loading
          if (lastt && t - lastt > 50000000 && t - first > 40000000000ll && gaps++ < 300)
              printf("gap %.0f ms: render parked %lld times, %.1f ms in all; longest: %s\n", (t - lastt) / 1e6, skate_gp_n, skate_gp_ns / 1e6, skate_gp_max ? skate_gp_chain : "-");
          skate_gp_n = skate_gp_ns = skate_gp_max = 0; }
        lastt = t; skate_last_frame_ns = t;
        { const char *pf = getenv("SKATESTRAP_PROF"); if (pf && pf[0] == 'p' && shown > 10) skate_poster_tid = skate_wk_tid[skate_wk_n & 7]; }
    }
    static int on = -1; static LARGE_INTEGER f, t0, last, in; static long long swap, worst; static int n;
    if (on < 0) { on = getenv("SKATESTRAP_FPS") != 0; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); last = t0; }
    if (!on) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    skate_render_tid = GetCurrentThreadId();
    if (!after) { static int shown; if (shown++ % 8 == 7 && shown < 60) { void lib_callers(const char *, uint64_t *); lib_callers("swap from", __builtin_frame_address(0)); } }
    if (!after) { if (now.QuadPart - last.QuadPart > worst) worst = now.QuadPart - last.QuadPart; last = now; in = now; return; }
    swap += now.QuadPart - in.QuadPart; n++;
    if (now.QuadPart - t0.QuadPart >= 5 * f.QuadPart) {
        double s = (double)(now.QuadPart - t0.QuadPart) / f.QuadPart;
        printf("fps %.1f, worst gap %.0f ms, swap %.1f ms/frame\n", n / s, worst * 1000.0 / f.QuadPart, swap * 1000.0 / f.QuadPart / n);
        printf("  render thread futex: %lld waits, %lld timed out, %.0f ms total\n", skate_rw_n, skate_rw_to, skate_rw_ns / 1e6);
        { extern volatile LONG skate_clock_n[12]; printf("  clock_gettime calls by id:"); for (int i = 0; i < 12; i++) if (skate_clock_n[i]) printf(" %d:%ld", i, skate_clock_n[i]); printf("\n"); }
        printf("  glMapBufferRange by access:"); for (int i = 0; i < 64; i++) if (map_n[i]) { printf(" %x:%ld/%.0fms", i, map_n[i], map_ns[i] * 1000.0 / f.QuadPart); map_n[i] = 0; map_ns[i] = 0; } printf("\n");
        { extern volatile LONG64 skate_madv_n, skate_madv_kb, skate_madv_us, skate_madv_worst;
          printf("  madvise DONTNEED: %lld calls, %lld MB, %.1f ms in all, worst %.1f ms\n", skate_madv_n, skate_madv_kb >> 10, skate_madv_us / 1000.0, skate_madv_worst / 1000.0);
          skate_madv_n = skate_madv_kb = skate_madv_us = skate_madv_worst = 0; }
        { void vk_stats(void); vk_stats(); }
        skate_rw_n = skate_rw_to = skate_rw_ns = 0;
        t0 = now; n = 0; swap = 0; worst = 0;
    }
}

void *shim_lookup_egl_hand(const char *name) {
    static int native = -1;
    if (native < 0) { const char *g = getenv("SKATESTRAP_GL"); native = g && !strcmp(g, "native"); }
    void *wgl_hook(const char *);
    if (native && !strncmp(name, "glGet", 5)) { void *h = wgl_hook(name); if (h) return h; }
    if (!strcmp(name, "ANativeWindow_fromSurface")) return s_ANativeWindow_fromSurface;
    if (!strcmp(name, "ANativeWindow_acquire")) return s_ANativeWindow_acquire;
    if (!strcmp(name, "ANativeWindow_release")) return s_ANativeWindow_release;
    if (!strcmp(name, "ANativeWindow_getWidth")) return s_ANativeWindow_getWidth;
    if (!strcmp(name, "ANativeWindow_getHeight")) return s_ANativeWindow_getHeight;
    if (!strcmp(name, "eglGetProcAddress")) return s_eglGetProcAddress;
    if (!strcmp(name, "eglGetDisplay")) return s_eglGetDisplay;
    if (!strcmp(name, "eglCreateWindowSurface")) return s_eglCreateWindowSurface;
    if (!strcmp(name, "eglSwapInterval")) return s_eglSwapInterval;
    if ((getenv("SKATESTRAP_FPS") || getenv("SKATESTRAP_MAP_SHADOW")) && !strcmp(name, "glMapBufferRange")) return s_glMapBufferRange;
    if (getenv("SKATESTRAP_MAP_SHADOW") && !strcmp(name, "glUnmapBuffer")) return s_glUnmapBuffer;
    return 0;
}
