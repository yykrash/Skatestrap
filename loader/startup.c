// The app's start-up, as the Android Java side would drive it: settings natives first.
// Order and values follow Mocktail's legacy_runtime.cc (Apache-2.0, github.com/komaruworld/mocktail).
// Identity is honest: device "Skatestrap", manufacturer "Skatestrap". No login or cookies are passed here.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <dpapi.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

void *skatestrap_export(const char *name), *shim_lookup(const char *name);
void *jni_env(void), *jni_class(const char *), *jni_string(const char *), *jni_object(const char *), *jni_array(int);
void jni_set(void *o, const char *field, uint64_t v), jni_set_str(void *o, const char *field, const char *s);
const char *skatestrap_version(void);

typedef SYSV void (*Fn0)(void *, void *);
typedef SYSV void (*Fn1)(void *, void *, void *);
typedef SYSV void (*Fn2)(void *, void *, void *, void *);
#define ASSETS "/data/app/com.roblox.client/assets/content/"
#define GL "Java_com_roblox_engine_jni_NativeGLInterface_"
#define SETTINGS "Java_com_roblox_engine_jni_NativeSettingsInterface_"
#define UA "Roblox/Android (Android 13; Skatestrap on Windows)"  // honest: says what we are

static void *env, *settings;
static void *need(const char *name) {
    void *f = skatestrap_export(name);
    if (!f) printf("  start-up: no export %s (skipped)\n", name);
    return f;
}
static void call1(const char *name, const char *a) {
    Fn1 f = need(name);
    if (!f) return;
    printf("  start-up: %s(%s)\n", name + sizeof SETTINGS - 1, a);
    f(env, settings, jni_string(a));
}
static void call2(const char *name, const char *a, const char *b) {
    Fn2 f = need(name);
    if (!f) return;
    printf("  start-up: %s(%s, %s)\n", name + sizeof SETTINGS - 1, a, b);
    f(env, settings, jni_string(a), jni_string(b));
}

static void *device_params(void) {
    void *p = jni_object("com/roblox/engine/jni/model/DeviceParams");
    char res[32];
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    snprintf(res, sizeof res, "%dx%d", w, h);
    MEMORYSTATUSEX m = { sizeof m };
    GlobalMemoryStatusEx(&m);
    jni_set_str(p, "osVersion", "33"); jni_set_str(p, "deviceName", "Skatestrap");
    jni_set_str(p, "appVersion", skatestrap_version()); jni_set_str(p, "country", "US");
    jni_set_str(p, "manufacturer", "Skatestrap"); jni_set_str(p, "displayResolution", res);
    jni_set_str(p, "networkType", "wifi"); jni_set_str(p, "deviceSku", "skatestrap-x86_64");
    jni_set_str(p, "socModel", "x86_64"); jni_set_str(p, "appBuildVariant", "googleProdRelease");
    jni_set_str(p, "testDeviceName", "Skatestrap");
    jni_set(p, "deviceTotalMemoryMB", m.ullTotalPhys >> 20);
    jni_set(p, "displayPhysicalWidthPixels", w); jni_set(p, "displayPhysicalHeightPixels", h);
    jni_set(p, "memoryClass", 512); jni_set(p, "largeMemoryClass", 1024);  // Android's per-app heap limits (MB)
    jni_set(p, "lowMemoryKillerBackgroundAppThreshold", 256ull << 20);
    jni_set(p, "lowMemoryKillerForegroundAppThreshold", 128ull << 20);
    jni_set(p, "cpu64Bit", 1);
    // android.os.Build statics, same honest identity as above (Java code reads these; a null crashed GetStringUTFLength)
    void *b = jni_class("android/os/Build");
    const char *bs[][2] = { { "BOARD", "skatestrap" }, { "BRAND", "Skatestrap" }, { "DEVICE", "skatestrap" }, { "HARDWARE", "skatestrap" },
                            { "MANUFACTURER", "Skatestrap" }, { "MODEL", "Skatestrap" }, { "PRODUCT", "skatestrap" }, { "ID", "SKATESTRAP" },
                            { "DISPLAY", "Skatestrap" }, { "FINGERPRINT", "Skatestrap/skatestrap/skatestrap:13/SKATESTRAP/1:user/release-keys" },
                            { "HOST", "skatestrap" }, { "TAGS", "release-keys" }, { "TYPE", "user" }, { "USER", "skatestrap" },
                            { "BOOTLOADER", "unknown" }, { "RADIO", "unknown" }, { "SERIAL", "unknown" } };
    for (size_t i = 0; i < sizeof bs / sizeof *bs; i++) jni_set_str(b, bs[i][0], bs[i][1]);
    return p;
}

// --- input: Win32 messages -> NativeInputInterface / nativePassKeyEvent ---------------------------------
// Keys go as (Linux evdev scan code, Android key code), as Mocktail sends them. Windows set-1 scan codes equal
// evdev codes for plain keys; E0-prefixed (extended) keys get their evdev code from ext_scan.
static const uint8_t android_key[128] = {
    [1] = 111, [2] = 8, [3] = 9, [4] = 10, [5] = 11, [6] = 12, [7] = 13, [8] = 14, [9] = 15, [10] = 16, [11] = 7,
    [12] = 69, [13] = 70, [14] = 67, [15] = 61, [16] = 45, [17] = 51, [18] = 33, [19] = 46, [20] = 48, [21] = 53,
    [22] = 49, [23] = 37, [24] = 43, [25] = 44, [26] = 71, [27] = 72, [28] = 66, [29] = 113, [30] = 29, [31] = 47,
    [32] = 32, [33] = 34, [34] = 35, [35] = 36, [36] = 38, [37] = 39, [38] = 40, [39] = 74, [40] = 75, [41] = 68,
    [42] = 59, [43] = 73, [44] = 54, [45] = 52, [46] = 31, [47] = 50, [48] = 30, [49] = 42, [50] = 41, [51] = 55,
    [52] = 56, [53] = 76, [54] = 60, [55] = 155, [56] = 57, [57] = 62, [58] = 115, [59] = 131, [60] = 132,
    [61] = 133, [62] = 134, [63] = 135, [64] = 136, [65] = 137, [66] = 138, [67] = 139, [68] = 140, [69] = 143,
    [70] = 116, [71] = 151, [72] = 152, [73] = 153, [74] = 156, [75] = 148, [76] = 149, [77] = 150, [78] = 157,
    [79] = 145, [80] = 146, [81] = 147, [82] = 144, [83] = 158, [87] = 141, [88] = 142, [96] = 160, [97] = 114,
    [98] = 154, [99] = 120, [100] = 58, [102] = 122, [103] = 19, [104] = 92, [105] = 21, [106] = 22, [107] = 123,
    [108] = 20, [109] = 93, [110] = 124, [111] = 112, [119] = 121, [125] = 117, [126] = 118, [127] = 82,
};
static int ext_scan(int s) {  // E0 xx -> evdev
    switch (s) {
    case 0x1c: return 96; case 0x1d: return 97; case 0x35: return 98; case 0x37: return 99; case 0x38: return 100;
    case 0x47: return 102; case 0x48: return 103; case 0x49: return 104; case 0x4b: return 105; case 0x4d: return 106;
    case 0x4f: return 107; case 0x50: return 108; case 0x51: return 109; case 0x52: return 110; case 0x53: return 111;
    case 0x5b: return 125; case 0x5c: return 126; case 0x5d: return 127;
    }
    return 0;
}
static void *in_env, *in_cls, *in_gl;
static SYSV void (*in_move)(void *, void *, float, float, float, float);
static SYSV void (*in_button)(void *, void *, float, float, uint8_t, int32_t);
static SYSV void (*in_wheel)(void *, void *, float, float, float);
static SYSV void (*in_key)(void *, void *, uint8_t, int32_t, int32_t, uint8_t);
static int skate_input_on;
static void key_tap(int ev, int down) { if (android_key[ev]) in_key(in_env, in_gl, down, ev, android_key[ev], 0); }
// Ctrl+V: the Android app has no paste from a PC keyboard, so type the Windows clipboard as key presses.
// shortcut: only characters the keyboard layout can type (no emoji / other scripts), first line only, 500 max.
static void paste_clipboard(void) {
    if (!OpenClipboard(0)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT); const WCHAR *t = h ? GlobalLock(h) : 0;
    int lctrl = GetKeyState(VK_LCONTROL) < 0, rctrl = GetKeyState(VK_RCONTROL) < 0;
    if (lctrl) key_tap(29, 0);
    if (rctrl) key_tap(97, 0);  // let go of Ctrl so the letters are not shortcuts
    for (int i = 0; t && t[i] && t[i] != 13 && t[i] != 10 && i < 500; i++) {
        SHORT k = VkKeyScanW(t[i]);
        if (k == -1 || (k & 0x600)) continue;  // not on this layout, or needs Ctrl/Alt
        int ev = MapVirtualKeyW(k & 0xff, MAPVK_VK_TO_VSC);
        if (ev <= 0 || ev >= 128 || !android_key[ev]) continue;
        if (k & 0x100) key_tap(42, 1);
        key_tap(ev, 1); key_tap(ev, 0);
        if (k & 0x100) key_tap(42, 0);
    }
    if (lctrl) key_tap(29, 1);
    if (rctrl) key_tap(97, 1);
    if (t) GlobalUnlock(h);
    CloseClipboard();
}
// Roblox TextBoxes (chat): Android types into them through its on-screen keyboard (passText), not key events.
// Engine calls showKeyboard on focus and hideKeyboard on blur (jni.c); typed text goes back the way Mocktail's
// roblox_text_editor.cc does it. shortcut: cursor always at the end (no arrow keys / selection), add when asked.
static SYSV void (*tb_pass)(void *, void *, int64_t, void *, uint8_t, int32_t);
static SYSV void (*tb_sync)(void *, void *, void *, int32_t);
static SYSV void (*tb_return)(void *, void *, int64_t), (*tb_release)(void *, void *, int64_t);
static SRWLOCK tb_lock = SRWLOCK_INIT;
static int64_t tb_handle;  // 0 = no TextBox focused
static DWORD tb_at;        // when it took focus: keys typed before that are not for it
static WCHAR tb_text[1024]; static int tb_len;
static float tb_box[5]; static uint32_t tb_color;  // NativeTextBoxInfo: x, y, w, h, font size (game pixels), ARGB
#define WM_TB_DRAW (WM_APP + 7)
void *skate_window_hwnd(void); float skate_render_scale(void);
// The engine hides a focused TextBox's text on Android (the app's own text field draws it there), so draw it ourselves:
// a click-through layered popup over the box, text in the TextBox colour with per-pixel alpha. Window thread only.
// shortcut: one line, Segoe UI, left-aligned, end kept in view; no caret/selection (cursor is always at the end).
static void tb_draw(void) {
    static HWND pop;
    HWND game = skate_window_hwnd();
    if (!game) return;
    if (!pop) {
        WNDCLASSW wc = { .lpfnWndProc = DefWindowProcW, .hInstance = GetModuleHandleW(0), .lpszClassName = L"SkatestrapText" };
        RegisterClassW(&wc);
        pop = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"SkatestrapText", L"",
                              WS_POPUP, 0, 0, 1, 1, game, 0, wc.hInstance, 0);
        if (!pop) return;
    }
    AcquireSRWLockShared(&tb_lock);
    float k = skate_render_scale(), b[5]; memcpy(b, tb_box, sizeof b);
    int on = tb_handle && b[2] >= 2 && b[3] >= 2, n = tb_len; uint32_t col = tb_color; WCHAR t[1024]; memcpy(t, tb_text, n * sizeof *t);
    ReleaseSRWLockShared(&tb_lock);
    if (!on || IsIconic(game)) { ShowWindow(pop, SW_HIDE); return; }
    POINT at = { (LONG)(b[0] / k), (LONG)(b[1] / k) }; ClientToScreen(game, &at);
    int w = (int)(b[2] / k), h = (int)(b[3] / k), fh = (int)((b[4] > 1 ? b[4] : b[3] * 0.6f) / k);
    if (fh < 8) fh = 8;
    if (fh > h) fh = h;
    if (!col) col = 0xffffffff;
    BITMAPINFO bi = { { sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB } }; uint32_t *px;
    HDC dc = CreateCompatibleDC(0); HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, 0, 0);
    if (!bm) { DeleteDC(dc); return; }
    HGDIOBJ obm = SelectObject(dc, bm);
    HFONT f = CreateFontW(-fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"), of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));  // white on black = coverage, coloured below
    SIZE sz = { 0 }; GetTextExtentPoint32W(dc, t, n, &sz);
    int pad = h / 8, x = sz.cx + 2 * pad > w ? w - pad - sz.cx : pad;  // too long: keep the end (where typing happens) in view
    TextOutW(dc, x, (h - sz.cy) / 2, t, n);
    GdiFlush();
    uint32_t r = (col >> 16) & 255, g = (col >> 8) & 255, bl = col & 255;
    for (int i = 0; i < w * h; i++) { uint32_t a = px[i] & 255; px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | bl * a / 255; }
    POINT zero = { 0 }; SIZE s = { w, h }; BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(pop, 0, &at, &s, dc, &zero, 0, &bf, ULW_ALPHA);
    SetWindowPos(pop, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(dc, of); DeleteObject(f); SelectObject(dc, obm); DeleteObject(bm); DeleteDC(dc);
}
static void on_keyboard(int show, uint64_t handle, const char *utf, int len, void *info) {
    int jni_textbox_info(void *, float[5], uint32_t *);
    AcquireSRWLockExclusive(&tb_lock);
    tb_handle = show ? (int64_t)handle : 0; tb_at = GetTickCount();
    tb_len = show && len > 0 ? MultiByteToWideChar(CP_UTF8, 0, utf, len, tb_text, 1023) : 0;
    if (!show || !jni_textbox_info(info, tb_box, &tb_color)) memset(tb_box, 0, sizeof tb_box);
    ReleaseSRWLockExclusive(&tb_lock);
    if (show) printf("  textbox: focus %.0f,%.0f %.0fx%.0f size %.0f colour %08x\n", tb_box[0], tb_box[1], tb_box[2], tb_box[3], tb_box[4], tb_color);
    HWND g = skate_window_hwnd(); if (g) PostMessageW(g, WM_TB_DRAW, 0, 0);  // engine thread: the window thread draws
}
static void tb_send(int64_t h, int finished) {
    char u[4096]; int n = WideCharToMultiByte(CP_UTF8, 0, tb_text, tb_len, u, sizeof u - 1, 0, 0); u[n] = 0;
    tb_sync(in_env, in_gl, jni_string(u), tb_len);
    tb_pass(in_env, in_gl, h, jni_string(u), finished, tb_len);
    tb_draw();
}
// one character (or Enter / Backspace) typed while a TextBox has focus; 0 = no TextBox, send it as a key
static int tb_char(WCHAR c) {
    AcquireSRWLockShared(&tb_lock); int64_t h = tb_handle; ReleaseSRWLockShared(&tb_lock);
    if (!h || !tb_pass) return 0;
    if (c == 13) {
        char u[4096]; int n = WideCharToMultiByte(CP_UTF8, 0, tb_text, tb_len, u, sizeof u - 1, 0, 0); u[n] = 0;
        tb_sync(in_env, in_gl, jni_string(u), tb_len);  // Mocktail's order: sync, return, final text, clear
        tb_return(in_env, in_gl, h); tb_pass(in_env, in_gl, h, jni_string(u), 1, tb_len); tb_len = 0;
        tb_draw();
        return 1;
    }
    if (c == 8) { if (tb_len) tb_len--; if (tb_len && IS_LOW_SURROGATE(tb_text[tb_len])) tb_len--; }
    else if (c >= 32 && c != 127 && tb_len < 1023) tb_text[tb_len++] = c;
    else return 1;
    tb_send(h, 0);
    return 1;
}
static int on_input(UINT m, WPARAM wp, LPARAM lp) {
    static float lx, ly;
    static int rdrag, rd_cx, rd_cy; static POINT rd_screen;  // right-drag: client and screen spot it was pressed at
    float skate_render_scale(void), k = skate_render_scale();  // the game's pixels, not the window's
    float x = (float)(short)LOWORD(lp) * k, y = (float)(short)HIWORD(lp) * k;
    switch (m) {
    case WM_TB_DRAW: tb_draw(); return 1;
    case WM_MOVE: case WM_SIZE: if (tb_handle) tb_draw(); return 0;  // keep the typed text on its box
    case WM_MOUSEMOVE:
        if (rdrag && !(wp & MK_RBUTTON)) rdrag = 0;  // released outside our messages (alt-tab, lost capture): never keep pinning
        if (rdrag) {  // right-drag camera: like Windows Roblox, the cursor stays where it was pressed; only the movement goes in
            int dx = (short)LOWORD(lp) - rd_cx, dy = (short)HIWORD(lp) - rd_cy;
            if (!dx && !dy) return 1;  // our own SetCursorPos coming back
            // a virtual position moves with the deltas, so it is right whether Roblox reads positions or deltas
            if (in_move) in_move(in_env, in_cls, lx + dx * k, ly + dy * k, dx * k, dy * k);
            lx += dx * k; ly += dy * k;
            SetCursorPos(rd_screen.x, rd_screen.y);
            return 1;
        }
        if (in_move) in_move(in_env, in_cls, x, y, x - lx, y - ly); lx = x; ly = y; return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        int down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN;
        int b = m <= WM_LBUTTONUP ? 0 : m <= WM_RBUTTONUP ? 1 : 2;  // Roblox: left 0, right 1, middle 2
        if (down) SetCapture(GetActiveWindow()); else ReleaseCapture();
        if (b == 1 && down) { rdrag = 1; rd_cx = (short)LOWORD(lp); rd_cy = (short)HIWORD(lp); GetCursorPos(&rd_screen); lx = x; ly = y; }
        if (b == 1 && !down && rdrag) {  // release at the virtual spot, then bring Roblox's cursor back to where it was pressed
            rdrag = 0; float vx = lx, vy = ly; x = rd_cx * k; y = rd_cy * k;
            if (in_button) in_button(in_env, in_cls, vx, vy, 0, 1);
            if (in_move) in_move(in_env, in_cls, x, y, 0, 0);  // zero delta: the jump back must not turn the camera
            lx = x; ly = y; return 1;
        }
        if (in_button) in_button(in_env, in_cls, x, y, down, b);
        return 1;
    }
    case WM_MOUSEWHEEL: {  // wheel lParam is in screen coordinates
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; ScreenToClient(GetActiveWindow(), &pt);
        if (in_wheel) in_wheel(in_env, in_cls, pt.x * k, pt.y * k,GET_WHEEL_DELTA_WPARAM(wp) / 120.f);
        return 1;
    }
    case WM_CHAR:
        if ((DWORD)GetMessageTime() - tb_at > 0x80000000u) return 1;  // typed before the TextBox took focus (the chat key)
        return tb_char((WCHAR)wp);
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        int s = (lp >> 16) & 0xff, ev = lp & (1 << 24) ? ext_scan(s) : s;
        if (wp == VK_F4 && (m == WM_SYSKEYDOWN)) return 0;  // let Alt+F4 close the window
        if (tb_handle && tb_pass && m == WM_KEYDOWN) {  // a TextBox has focus: keys type text (WM_CHAR), not move the character
            if (wp == VK_ESCAPE) { int64_t h = tb_handle; tb_release(in_env, in_gl, h); on_keyboard(0, 0, 0, 0, 0); }
            else if (wp == 'V' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && OpenClipboard(0)) {
                HANDLE ch = GetClipboardData(CF_UNICODETEXT); const WCHAR *t = ch ? GlobalLock(ch) : 0;
                for (int i = 0; t && t[i] && t[i] != 13 && t[i] != 10 && tb_len < 1023; i++) tb_text[tb_len++] = t[i];
                if (t) { GlobalUnlock(ch); tb_send(tb_handle, 0); }
                CloseClipboard();
            }
            return 1;  // (key-ups still go through, so nothing held before the focus sticks)
        }
        if (ev <= 0 || ev >= 128 || !android_key[ev] || !in_key) return 0;
        int down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN, repeat = down && (lp & (1 << 30));
        if (wp == 'V' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) { if (down && !repeat) paste_clipboard(); return 1; }
        in_key(in_env, in_gl, down, ev, android_key[ev], repeat);
        return 1;
    }
    }
    return 0;
}

// --- gamepads: XInput -> NativeInputInterface gamepad exports (mapping from Mocktail roblox_gamepad_input.cc) ---
// PS4 / PS5 pads are read raw over HID (hid_reader) and turned into the same XInput state, slots 4-7.
typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad g; } XState;
static DWORD (WINAPI *x_get)(DWORD, XState *);
static SYSV void (*gp_key)(void *, void *, int32_t, int32_t, uint8_t, int32_t);           // nativeSetGamepadSupportedKeyWithGamepadType
static SYSV void (*gp_motion)(void *, void *, int32_t, int32_t, int32_t, uint8_t, int32_t); // nativeSetGamepadSupportedMotionWithGamepadType
static SYSV void (*gp_connect)(void *, void *, int32_t, int32_t);
static SYSV void (*gp_disconnect)(void *, void *, int32_t);
static SYSV void (*gp_button)(void *, void *, int32_t, int32_t, int32_t);
static SYSV void (*gp_axis)(void *, void *, int32_t, int32_t, float, float, float);
enum { GP_TYPE = 3 };  // Roblox Android gamepad type 3 = Xbox (1 = PS4, 2 = PS5: Mocktail RobloxGamepadType)

// --- DualShock 4 / DualSense over HID (USB or Bluetooth). Report layouts as SDL's hidapi drivers read them.
// One thread looks for Sony pads every 2 s; each open pad gets a thread doing blocking reads into hid_pad[].
static struct { volatile LONG on; int type; XPad g; WCHAR path[260]; } hid_pad[4];
static SRWLOCK hid_lock = SRWLOCK_INIT;
static SHORT hid_axis(BYTE v, int flip) { int a = v * 257 - 32768; if (flip) a = -1 - a; return (SHORT)a; }  // 0..255 -> -32768..32767
// One input report -> XInput state; 0 if it is not a stick/button report. ps5: the pad is a DualSense.
static int hid_parse(const BYTE *r, DWORD n, int ps5, XPad *g) {
    const BYTE *p; int full;  // p = left stick X; full = DualSense layout (triggers before buttons)
    if (n >= 10 && r[0] == 0x01) { p = r + 1; full = ps5 && n >= 64; }  // USB, or Bluetooth's short report
    else if (n >= 12 && r[0] == 0x11 && !ps5) { p = r + 3; full = 0; }  // DS4 Bluetooth full report
    else if (n >= 12 && r[0] == 0x31 && ps5) { p = r + 2; full = 1; }   // DualSense Bluetooth full report
    else return 0;
    BYTE b1 = full ? p[7] : p[4], b2 = full ? p[8] : p[5];
    g->lx = hid_axis(p[0], 0); g->ly = hid_axis(p[1], 1); g->rx = hid_axis(p[2], 0); g->ry = hid_axis(p[3], 1);
    g->lt = full ? p[4] : p[7]; g->rt = full ? p[5] : p[8];
    static const WORD hat[9] = { 0x0001, 0x0009, 0x0008, 0x000A, 0x0002, 0x0006, 0x0004, 0x0005, 0 };  // up, up-right, ... none
    WORD b = hat[(b1 & 15) < 8 ? b1 & 15 : 8];
    if (b1 & 0x10) b |= 0x4000;  // square -> X
    if (b1 & 0x20) b |= 0x1000;  // cross -> A
    if (b1 & 0x40) b |= 0x2000;  // circle -> B
    if (b1 & 0x80) b |= 0x8000;  // triangle -> Y
    if (b2 & 0x01) b |= 0x0100;  // L1
    if (b2 & 0x02) b |= 0x0200;  // R1
    if (b2 & 0x10) b |= 0x0020;  // share / create -> back
    if (b2 & 0x20) b |= 0x0010;  // options -> start
    if (b2 & 0x40) b |= 0x0040;  // L3
    if (b2 & 0x80) b |= 0x0080;  // R3
    g->b = b;
    return 1;
}
static DWORD WINAPI hid_reader(void *arg) {
    int i = (int)(INT_PTR)arg; HANDLE h = CreateFileW(hid_pad[i].path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    BYTE r[128]; DWORD n;
    while (h != INVALID_HANDLE_VALUE && ReadFile(h, r, sizeof r, &n, 0)) {
        XPad g;
        if (!hid_parse(r, n, hid_pad[i].type == 2, &g)) continue;
        AcquireSRWLockExclusive(&hid_lock); hid_pad[i].g = g; ReleaseSRWLockExclusive(&hid_lock);
        InterlockedExchange(&hid_pad[i].on, 2);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (hid_pad[i].on == 2) printf("  [gamepad] %s pad unplugged\n", hid_pad[i].type == 2 ? "PS5" : "PS4");
    // (a pad another program holds, e.g. DS4Windows' exclusive mode, fails here quietly and is tried again in 2 s)
    InterlockedExchange(&hid_pad[i].on, 0);
    return 0;
}
static DWORD WINAPI hid_scan(void *arg) {
    GUID hid; HidD_GetHidGuid(&hid);
    for (;; Sleep(2000)) {
        HDEVINFO set = SetupDiGetClassDevsW(&hid, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA d = { sizeof d };
        for (DWORD k = 0; SetupDiEnumDeviceInterfaces(set, 0, &hid, k, &d); k++) {
            struct { SP_DEVICE_INTERFACE_DETAIL_DATA_W h; WCHAR more[260]; } det = { { sizeof det.h } };
            if (!SetupDiGetDeviceInterfaceDetailW(set, &d, &det.h, sizeof det, 0, 0) || !wcsstr(_wcslwr(det.h.DevicePath), L"vid_054c")) continue;
            int slot = -1, open = 0;
            for (int i = 0; i < 4; i++) {
                if (hid_pad[i].on && !wcscmp(hid_pad[i].path, det.h.DevicePath)) open = 1;
                if (!hid_pad[i].on && slot < 0) slot = i;
            }
            if (open || slot < 0) continue;
            HANDLE h = CreateFileW(det.h.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);  // (0 access: attributes only)
            HIDD_ATTRIBUTES a = { sizeof a }; int ok = h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &a);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            int type = !ok ? 0 : (a.ProductID == 0x05C4 || a.ProductID == 0x09CC || a.ProductID == 0x0BA0) ? 1 : (a.ProductID == 0x0CE6 || a.ProductID == 0x0DF2) ? 2 : 0;
            if (!type) continue;
            wcscpy(hid_pad[slot].path, det.h.DevicePath); hid_pad[slot].type = type; hid_pad[slot].on = 1;
            CloseHandle(CreateThread(0, 0, hid_reader, (void *)(INT_PTR)slot, 0, 0));
        }
        SetupDiDestroyDeviceInfoList(set);
    }
}
static const struct { WORD mask; int32_t key; } gp_buttons[14] = {  // Android key codes
    { 0x1000, 96 }, { 0x2000, 97 }, { 0x4000, 99 }, { 0x8000, 100 },   // A B X Y
    { 0x0001, 19 }, { 0x0002, 20 }, { 0x0004, 21 }, { 0x0008, 22 },     // d-pad up down left right
    { 0x0100, 102 }, { 0x0200, 103 }, { 0x0040, 106 }, { 0x0080, 107 }, // LB RB L3 R3
    { 0x0010, 108 }, { 0x0020, 109 },                                   // start back
};
static const int32_t gp_axes[6] = { 0, 1, 11, 14, 17, 18 };  // LX LY RX RY LT RT (Android AXIS_*)
static float gp_stick1(SHORT v) { return v / (v < 0 ? 32768.f : 32767.f); }
static void gp_stick(SHORT rx, SHORT ry, float o[2]) {  // radial 0.20 dead zone; XInput y is already up-positive
    float x = gp_stick1(rx), y = gp_stick1(ry), l = sqrtf(x * x + y * y);
    if (l <= 0.2f) { o[0] = o[1] = 0; return; }
    float s = (fminf(l, 1.f) - 0.2f) / (0.8f * l); o[0] = x * s; o[1] = y * s;
}
static float gp_trigger(BYTE v) { float f = v / 255.f; return f <= 0.02f ? 0 : (f - 0.02f) / 0.98f; }

static void gamepad_poll(void) {
    static struct { int present, connected; WORD b; float ax[6]; DWORD next_probe; } pad[8];  // 0-3 XInput, 4-7 PS over HID
    if (!gp_button || !gp_axis || !gp_connect || !gp_key || !gp_motion || !gp_disconnect) return;
    static int hid_started;
    if (!hid_started) { hid_started = 1; CloseHandle(CreateThread(0, 0, hid_scan, 0, 0, 0)); }
    DWORD now = GetTickCount();
    int focused = GetActiveWindow() != 0, xinput = 0;
    for (int i = 0; i < 8; i++) {
        static int ps_icons = -1; if (ps_icons < 0) { const char *e = getenv("SKATESTRAP_PS_ICONS"); ps_icons = e && *e == '1'; }
        int dev = i + 1, type = ps_icons ? 1 : GP_TYPE;  // PS icons: every pad says it is a PS4 pad, so Roblox shows PS buttons
        XState st; int gone;
        if (i < 4) {
            if (!x_get || (!pad[i].present && (int)(now - pad[i].next_probe) < 0)) continue;  // empty slots are slow to query: probe 1/s
            gone = x_get(i, &st) != 0;
            if (!gone) xinput = 1;
        } else {
            // PS pads count only while no XInput pad is in (Steam Input / DS4Windows show the same pad as XInput too)
            int h = i - 4;
            gone = hid_pad[h].on != 2 || xinput;
            type = hid_pad[h].type;
            if (gone && !pad[i].present) continue;
            if (!gone) { AcquireSRWLockShared(&hid_lock); st.g = hid_pad[h].g; ReleaseSRWLockShared(&hid_lock); }
        }
        if (gone) {
            if (pad[i].connected) {
                for (int k = 0; k < 14; k++) if (pad[i].b & gp_buttons[k].mask) gp_button(in_env, in_cls, dev, gp_buttons[k].key, 0);
                gp_disconnect(in_env, in_cls, dev);
                printf("  [gamepad] device %d disconnected\n", dev);
            }
            memset(&pad[i], 0, sizeof pad[i]); pad[i].next_probe = now + 1000;
            continue;
        }
        if (!pad[i].present) { pad[i].present = 1; printf("  [gamepad] XInput pad %d detected\n", i); }
        XPad g = st.g;
        if (!focused) { g.b = 0; g.lt = g.rt = 0; g.lx = g.ly = g.rx = g.ry = 0; }  // unfocused: release everything
        float ax[6]; gp_stick(g.lx, g.ly, ax); gp_stick(g.rx, g.ry, ax + 2); ax[4] = gp_trigger(g.lt); ax[5] = gp_trigger(g.rt);
        if (g.b == pad[i].b && !memcmp(ax, pad[i].ax, sizeof ax)) continue;
        if (!pad[i].connected) {  // Android order: capabilities, connect, then the first action (lazy, as Mocktail)
            for (int k = 0; k < 14; k++) gp_key(in_env, in_cls, dev, gp_buttons[k].key, 1, type);
            for (int a = 0; a < 6; a++) gp_motion(in_env, in_cls, dev, gp_axes[a], -1, 1, type);
            static const int32_t off[4] = { 22, 23, 15, 16 };  // gas/brake and hat axes: not used (d-pad comes as keys)
            for (int a = 0; a < 4; a++) {
                gp_motion(in_env, in_cls, dev, off[a], -1, 0, type);
                if (off[a] >= 15 && off[a] <= 16) gp_motion(in_env, in_cls, dev, off[a], 1, 0, type);
            }
            gp_connect(in_env, in_cls, dev, type); pad[i].connected = 1;
            printf("  [gamepad] device %d connected to Roblox (type %d)\n", dev, type);
        }
        for (int k = 0; k < 14; k++) {
            WORD m = gp_buttons[k].mask;
            if ((g.b ^ pad[i].b) & m) gp_button(in_env, in_cls, dev, gp_buttons[k].key, (g.b & m) != 0);
        }
        for (int s = 0; s < 4; s += 2)  // each stick axis id gets the (x, y) pair, as Mocktail sends it
            if (ax[s] != pad[i].ax[s] || ax[s + 1] != pad[i].ax[s + 1]) {
                gp_axis(in_env, in_cls, dev, gp_axes[s], ax[s], ax[s + 1], 0);
                gp_axis(in_env, in_cls, dev, gp_axes[s + 1], ax[s], ax[s + 1], 0);
            }
        for (int t = 4; t < 6; t++) if (ax[t] != pad[i].ax[t]) gp_axis(in_env, in_cls, dev, gp_axes[t], 0, 0, ax[t]);
        pad[i].b = g.b; memcpy(pad[i].ax, ax, sizeof ax);
    }
}

// shortcut: ../clientsettings.json is fetched by hand once; refetch when flags go stale
static const char *flags_json(void) {
    FILE *f = fopen("../clientsettings.json", "rb");
    if (!f) return "{\"applicationSettings\":{}}";
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1); s[fread(s, 1, n, f)] = 0; fclose(f);
    printf("  start-up: %ld bytes of client settings\n", n);
    return s;
}

// Mocktail performance_policy.cc "throughput" defaults (constant rendering flags); SKATESTRAP_PERF=0 disables
static const char perf_const[] =
    ",\"FFlagMovePrerenderV2\":\"True\""
    ",\"FFlagGcInParallelWithRenderPrepare3\":\"True\""
    ",\"FFlagSimRuntimeContentTranscodeBlockingCall\":\"False\""
    ",\"FFlagRenderEnableLowEndLOD\":\"True\""
    ",\"FIntTerrainArraySliceSize\":\"4\""
    ",\"FFlagRenderFixParticlesCulling\":\"True\""
    ",\"FFlagLuauIncrementalGC\":\"True\""
    ",\"FIntLuauGcStepMultiplier\":\"100\""
    ",\"FIntLuauGcGoalRatio\":\"200\""
    ",\"FIntAssetProviderThreads\":\"2\""
    ",\"FFlagUITextureCompressionDesktop\":\"True\""
    ",\"FFlagTCTextureCompressionDesktop\":\"True\""
    ",\"FFlagUITextureUncompressed\":\"False\""
    ",\"FFlagMeshCompression\":\"True\""
    ",\"FFlagPhysicsMeshCompression\":\"True\""
    ",\"FFlagSupportMeshLOD\":\"True\""
    ",\"FFlagMeshLOD\":\"True\""
    ",\"FFlagMeshLodApplyGeomMeshOptimizer\":\"False\""
    ",\"FFlagAdaptiveMeshCacheSizeForAndroid\":\"True\""
    ",\"FIntDefaultMeshCacheSizeMB\":\"64\""
    ",\"FIntRenderMeshOrphanBudgetMB\":\"8\""
    ",\"FIntTerrainGpuMeshMemoryTargetMib\":\"32\""
    ",\"FIntAvatarMeshMemoryMax\":\"33554432\""
    ",\"FIntSmoothTerrainPhysicsCacheSize\":\"32\""
    ",\"FIntRenderTextureTotalBudgetMB\":\"128\""
    ",\"FIntRenderTextureOrphanBudgetMB\":\"8\""
    ",\"FFlagDebugTerrainVTCompressedTextures\":\"True\""
    ",\"FFlagStreamingObserverMemoryOptimization\":\"True\""
    ",\"FIntNetworkStreamingGCMaxMicroSecondLimit\":\"5000\""
    ",\"FFlagfeatureFastClusterCacheEnabled\":\"True\""
    ",\"FFlagEnableMultiVETextureManagerSharing\":\"True\""
    ",\"FFlagVulkanCompressedTextureAliasing\":\"True\""
    ",\"FFlagCSGPublishDedupSolidMesh\":\"True\""
    ",\"FFlagDeduplicateLodRemovalRegions\":\"True\""
    ",\"FFlagCurlUsePools\":\"True\""
    ",\"FIntRenderTextureCompositorBudget\":\"32\""
    ",\"FIntTextureCompositorActiveJobs\":\"1\""
    ",\"FIntTextureCompositorLowResFactor\":\"2\""
    ",\"FIntTextureCompositorMeshCache\":\"16\""
    ",\"FFlagContentProviderMmapAssets\":\"True\""
    ",\"FIntAssetProviderAssetCacheReadThreadCount\":\"1\""
    ",\"FIntAssetProviderAssetCacheWriteThreadCount\":\"1\""
    ",\"FIntInitialAudioAssetCacheSize\":\"32\""
    ",\"FIntAvatarTextureMemoryMax\":\"33554432\""
    ",\"FFlagEnableSLIMAvatars\":\"True\""
    ",\"FFlagEnableSlimAvatarsDefaultEnabled\":\"True\""
    ",\"FFlagLayeredClothingCacheOptimizations\":\"True\""
    ",\"FFlagLuauStartupGcSuppression\":\"False\""
    ",\"FIntLuauGcStepMul\":\"300\""
    ",\"FIntLuauGcGoalCore\":\"120\""
    ",\"FIntLuauGcStartupWorkingSetMb\":\"64\""
    ",\"FIntLuaGcMaxKb\":\"262144\""
    ",\"FIntTaskSchedulerMaxTempArenaSizeBytes\":\"16777216\""
    ",\"FFlagLocalStorageArenaOptimization\":\"True\""
    ",\"FIntProjectedMaxBytesUsedForSoundsMB\":\"32\""
    ",\"FIntAudioMetadataCacheSizeKB\":\"2048\""
    ",\"FIntDefaultAudioDecodeBufferSizeMs\":\"50\""
    ",\"FIntMaxAudibleSoundChannels\":\"32\""
    ;

static int physical_cores(void) {
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION buf[256]; DWORD len = sizeof buf; int n = 0;
    DWORD_PTR pm = ~(DWORD_PTR)0, sm;  // only cores this process may run on (start /affinity)
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~(DWORD_PTR)0;
    if (GetLogicalProcessorInformation(buf, &len))
        for (DWORD i = 0; i < len / sizeof *buf; i++) n += buf[i].Relationship == RelationProcessorCore && (buf[i].ProcessorMask & pm);
    return n > 0 ? n : 4;
}

// SKATESTRAP_FPS_CAP=N|unlimited: client-settings overrides that lift the 60 fps cap (Mocktail frame_rate_policy.cc)
static const char *fps_overrides(void) {
    static char s[8192];
    int o = 1; s[0] = '{'; s[1] = 0;  // every entry leads with a comma; the first one is dropped below
    const char *p = getenv("SKATESTRAP_PERF");
    if (!p || strcmp(p, "0")) {
        int w = physical_cores();
        o += snprintf(s + o, sizeof s - o,
            ",\"FIntTaskSchedulerThreadMin\":\"0\",\"FIntTaskSchedulerAsyncTasksMinimumThreadCount\":\"%d\""
            ",\"FIntTaskSchedulerAutoThreadLimit\":\"%d\",\"DFIntSimMidPhaseContactPipelineBatchSize\":\"128\""
            ",\"FIntSmoothClusterTaskQueueMaxParallelTasks\":\"%d\",\"FIntOcclusionWorkerThreadCount\":\"%d\""
            ",\"FIntRenderTextureMipBias\":\"1\"%s",
            w < 3 ? w : 3, w, w, w / 2 > 1 ? w / 2 : 1, perf_const);
        printf("  start-up: perf overrides on (%d physical cores)\n", w);
    }
    const char *v = getenv("SKATESTRAP_FPS_CAP");
    int n = !v ? 0 : strcmp(v, "unlimited") ? atoi(v) : 9999;  // unlimited: past Roblox's 240 limit (LimitTargetFpsTo2402 off below)
    if (v && n <= 0) printf("  start-up: SKATESTRAP_FPS_CAP=%s ignored (want a number or unlimited)\n", v);
    if (n > 0) {
        // above 240 the in-game "Maximum Frame Rate" menu (tops out at 240) must be off, or it wins over the target
        o += snprintf(s + o, sizeof s - o, ",\"FFlagGameBasicSettingsFramerateCap5\":\"%s\",\"DFIntTaskSchedulerTargetFps\":\"%d\"%s",
                      n > 240 ? "False" : "True", n, n > 240 ? ",\"FFlagTaskSchedulerLimitTargetFpsTo2402\":\"False\"" : "");
        printf("  start-up: fps overrides target %d\n", n);
    }
    // SKATESTRAP_FLAGS_FILE=path: the user's own flags (a flat JSON object, e.g. from Skatestrap), added last so they win
    const char *ff = getenv("SKATESTRAP_FLAGS_FILE"); FILE *f = ff ? fopen(ff, "rb") : 0;
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *u = malloc(n + 1); u[fread(u, 1, n, f)] = 0; fclose(f);
        char *a = strchr(u, '{'), *b = strrchr(u, '}');
        if (a && b > a) {
            *b = 0; a++; while (*a && isspace((unsigned char)*a)) a++;
            char *big = malloc(o + strlen(a) + 4); memcpy(big, s, o);
            int m = o + sprintf(big + o, "%s%s}", *a ? "," : "", a);
            if (big[1] == ',') memmove(big + 1, big + 2, m);
            printf("  start-up: %ld bytes of user flags from SKATESTRAP_FLAGS_FILE\n", n);
            free(u); return big;
        }
        printf("  start-up: SKATESTRAP_FLAGS_FILE has no { } object, ignored\n"); free(u);
    }
    snprintf(s + o, sizeof s - o, "}");
    if (s[1] == ',') memmove(s + 1, s + 2, strlen(s + 1));
    return s;
}

static void *start_game_params;
static DWORD WINAPI start_game(void *x) {
    typedef SYSV int (*FnStart)(void *, void *, void *);
    FnStart sg = (FnStart)need("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam");
    if (!sg) return 0;
    int r = sg(jni_env(), jni_class("com/roblox/engine/jni/NativeGLInterface"), start_game_params);
    printf("  start-up: nativeAppBridgeV2StartGameWithParam returned %d\n", r); fflush(stdout);
    return 0;
}

// Value of "key" in the app's flat launch JSON: a number as text, or a string with \" \\ \/ \n undone. 0 if absent/null.
// shortcut: \uXXXX escapes are kept as written; decode them if a launch field ever carries non-ASCII text
static int json_get(const char *j, const char *key, char *out, size_t n) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    for (const char *p = strstr(j, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat); while (*q == ' ') q++;
        if (*q != ':') continue;  // the name showed up as a value, not a key
        q++; while (*q == ' ') q++;
        size_t k = 0;
        if (*q == '"') {
            for (q++; *q && *q != '"' && k + 1 < n; q++) {
                if (*q == '\\' && q[1]) { q++; out[k++] = *q == 'n' ? '\n' : *q; } else out[k++] = *q;
            }
        } else if (!strncmp(q, "null", 4)) return 0;
        else while (*q && *q != ',' && *q != '}' && *q != ' ' && k + 1 < n) out[k++] = *q++;
        out[k] = 0; return 1;
    }
    return 0;
}

// One game join (StartGameParams fields from Mocktail's BuildFreshStartGameParams). json: the app's
// ExperienceProtocol launch request, or 0 for SKATESTRAP_PLACE_ID (a plain place join).
static void *g_surface, *g_pp; static HANDLE g_start_thread;
static int join_game(long long place, const char *json) {
    if (g_start_thread && WaitForSingleObject(g_start_thread, 0) == WAIT_TIMEOUT) { printf("  launch: still starting the last game; ignored\n"); return 0; }
    static char v[16384]; const char *J = json ? json : "{}";
    long long user = json_get(J, "userId", v, sizeof v) ? _atoi64(v) : 0;
    long long conv = json_get(J, "conversationId", v, sizeof v) ? _atoi64(v) : 0;
    long long ref = json_get(J, "referredByPlayerId", v, sizeof v) ? _atoi64(v) : 0;
    if (place <= 0 || user < 0 || conv < 0 || ref < 0) { printf("  launch: bad place/ids; ignored\n"); return 0; }
    void *gp = jni_object("com/roblox/engine/jni/autovalue/StartGameParams");
    jni_set(gp, "surface", (uint64_t)g_surface); jni_set(gp, "platformParams", (uint64_t)g_pp);
    jni_set(gp, "placeId", (uint64_t)place);
    jni_set(gp, "userId", (uint64_t)user);  // the user to follow into their game (Mocktail join_target_user_id), not the logged-in user
    jni_set(gp, "conversationId", (uint64_t)conv); jni_set(gp, "referredByPlayerId", (uint64_t)ref);
    jni_set(gp, "deviceParams", (uint64_t)device_params());
    static const char *const keys[][2] = { { "accessCode", "accessCode" }, { "callId", "callId" }, { "linkCode", "linkCode" },
        { "reservedServerAccessCode", "reservedServerAccessCode" }, { "gameId", "gameInstanceId" }, { "referralPage", "referralPage" },
        { "launchData", "launchData" }, { "gameJoinContext", "gameJoinContext" }, { "eventId", "eventId" }, { "joinAttemptId", "joinAttemptId" },
        { "joinAttemptOrigin", "joinAttemptOrigin" }, { "isoContext", "isoContext" } };
    int has[12];
    for (int i = 0; i < 12; i++) { has[i] = json_get(J, keys[i][1], v, sizeof v) && *v; jni_set_str(gp, keys[i][0], has[i] ? v : ""); }
    // join type: Mocktail JoinRequestTypeFor (APK zh.l0): 6 chat, 1 follow user, 2 private link, 3 server id, 8 reserved, 0 plain
    int type = conv > 0 ? 6 : user > 0 ? 1 : has[0] || has[2] ? 2 : has[4] ? 3 : has[3] ? 8 : 0;
    jni_set(gp, "joinRequestType", type);
    { const char *un = getenv("SKATESTRAP_USERNAME"); jni_set_str(gp, "username", un ? un : ""); }
    jni_set(gp, "vrContext", (uint64_t)jni_object("android/app/Activity"));
    start_game_params = gp;
    printf("  launch: nativeAppBridgeV2StartGameWithParam(place %lld, join type %d) on its own thread\n", place, type); fflush(stdout);
    if (g_start_thread) CloseHandle(g_start_thread);
    g_start_thread = CreateThread(0, 16 << 20, start_game, 0, 0, 0);  // Mocktail starts the game off the main thread, which keeps pumping
    return 1;
}

// A game picked in the app (ExperienceProtocol launch, any thread): kept here until the pump loop starts it.
static SRWLOCK launch_lock = SRWLOCK_INIT; static char *launch_json;
volatile DWORD skate_pump_tid; volatile LONG skate_pump_handoffs; static long skate_pump_hot;  // pump thread, its wakes of render (shim.c)
static void on_raw_message(void *callback, const char *utf) {
    size_t n = strlen(utf);
    if (!n || n > 64 * 1024) return;
    AcquireSRWLockExclusive(&launch_lock);
    free(launch_json); launch_json = _strdup(utf);
    ReleaseSRWLockExclusive(&launch_lock);
}

// The app signed in on its own login screen: keep its session for next time, DPAPI-encrypted for this Windows user
// (the same login.bin the launcher reads). Only when the launcher asks (SKATESTRAP_LOGIN_FILE); the cookie is never printed.
static void on_cookie_sync(void) {
    const char *path = getenv("SKATESTRAP_LOGIN_FILE");
    typedef SYSV void *(*FnGet)(void *, void *, void *);
    static FnGet get; if (!get && path) get = (FnGet)need("Java_com_roblox_engine_jni_NativeSettingsInterface_nativeGetCookiesForDomain");
    if (!path) return;
    extern const char *jni_utf(void *);
    const char *all = get ? jni_utf(get(jni_env(), jni_class("com/roblox/engine/jni/NativeSettingsInterface"), jni_string("https://www.roblox.com/"))) : 0;
    // the engine answers "name=value; ..." or cookie-jar lines ("domain\tTRUE\t/\tFALSE\texpiry\tname\tvalue")
    const char *p = all ? strstr(all, ".ROBLOSECURITY=") : 0, *jar = all ? strstr(all, "\t.ROBLOSECURITY\t") : 0;
    if (p) p += 15; else if (jar) p = jar + 16;
    {   // log when a signed-in session appears or goes (never the value)
        static int had = -1; int has = p != 0;
        if (has != had) { had = has; printf("  login: %s\n", has ? "Roblox has a signed-in session" : "no signed-in session yet"); fflush(stdout); }
    }
    if (!p) return;  // shortcut: signing out in the app keeps login.bin; the launcher's Log out button removes it
    size_t n = strcspn(p, ";\r\n\t ");
    static size_t saved_n; static char saved_hash[32];
    char h[32] = { 0 }; for (size_t i = 0; i < n; i++) h[i % 32] ^= p[i];  // skip rewriting the same session on every sync
    if (n == saved_n && !memcmp(h, saved_hash, 32)) return;
    DATA_BLOB in = { (DWORD)n, (BYTE *)p }, out = { 0 };
    if (!n || !CryptProtectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) { printf("  login: could not save the session\n"); return; }
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
    if (f) fclose(f);
    LocalFree(out.pbData);
    if (ok) { saved_n = n; memcpy(saved_hash, h, 32); }
    printf("  login: %s the session from Roblox's sign-in\n", ok ? "saved" : "could not save"); fflush(stdout);
}

void skatestrap_startup(void) {
    env = jni_env();
    settings = jni_class("com/roblox/engine/jni/NativeSettingsInterface");
    typedef SYSV int (*Mkdir)(const char *, int);
    Mkdir mk = (Mkdir)shim_lookup("mkdir");
    static const char *dirs[] = {
        "/data/user/0/com.roblox.client", "/data/user/0/com.roblox.client/files", "/data/user/0/com.roblox.client/cache",
        "/data/user/0/com.roblox.client/rbx-storage", "/data/user/0/com.roblox.client/shared_prefs", "/data/user/0/com.roblox.client/appData",
        "/data/user/0/com.roblox.client/appData/LocalStorage", "/data/user/0/com.roblox.client/appData/rbx-storage",
        "/data/user/0/com.roblox.client/files/rbx-storage", "/data/user/0/com.roblox.client/files/appData",
        "/data/user/0/com.roblox.client/files/appData/LocalStorage", "/data/user/0/com.roblox.client/files/appData/OTAPatchBackups",
        "/data/user/0/com.roblox.client/files/appData/rbx-storage", "/data/user/0/com.roblox.client/cache/ContentProvider_2",
        "/data/user/0/com.roblox.client/cache/rbx-storage", "/data/user/0/com.roblox.client/cache/sounds",
        "/sdcard/Android", "/sdcard/Android/data", "/sdcard/Android/data/com.roblox.client",
        "/sdcard/Android/data/com.roblox.client/files", "/data/user/0/com.roblox.client/files/exe",
    };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) mk(dirs[i], 0700);  // (EEXIST is fine)
    // the Java side unpacks the CA bundle from the APK on start; curl reads it as CAINFO (else CURLE_SSL_CACERT_BADFILE)
    typedef SYSV FILE *(*Fopen)(const char *, const char *);
    Fopen fo = (Fopen)shim_lookup("fopen");
    FILE *src = fo("/data/app/com.roblox.client/assets/ssl/cacert.pem", "rb"), *dst = src ? fo("/data/user/0/com.roblox.client/files/exe/cacert.pem", "wb") : 0;
    if (src && dst) { char buf[16384]; size_t n; while ((n = fread(buf, 1, sizeof buf, src))) fwrite(buf, 1, n, dst); }
    else printf("  start-up: cacert.pem not unpacked (no assets/ssl/cacert.pem?)\n");
    if (src) fclose(src);
    if (dst) fclose(dst);

    void *activity_cls = jni_class("com/roblox/client/startup/MainGameActivity");
    Fn1 sap = need("Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath");
    if (sap) { printf("  start-up: nativeSetAssetPath(%s)\n", ASSETS); sap(env, activity_cls, jni_string(ASSETS)); }

    call1(SETTINGS "nativeSetExceptionReasonFilename", "exception_reason.txt");
    call2(SETTINGS "nativeSetBaseUrl", "https://www.roblox.com/", "https://api.roblox.com/");
    {   // login: the user's own cookie, never printed (same call Mocktail makes; Apache-2.0, credit in FEASIBILITY.md)
        const char *path = getenv("SKATESTRAP_COOKIE_FILE"); if (!path) path = "cookie.txt";  // default login cookie file; SKATESTRAP_COOKIE_FILE overrides
        const char *ev = getenv("SKATESTRAP_COOKIE");  // Skatestrap login: the cookie itself, so it is never on disk in plain text
        FILE *cf = ev && *ev ? 0 : fopen(path, "rb");
        static char ck[4096]; size_t n = 0;
        if (cf) { n = fread(ck + 15, 1, sizeof ck - 16, cf); fclose(cf); }
        if (ev && strlen(ev) < sizeof ck - 16) { n = strlen(ev); memcpy(ck + 15, ev, n); _putenv("SKATESTRAP_COOKIE="); }  // gone from our environment once copied
        while (n && (ck[15 + n - 1] == 10 || ck[15 + n - 1] == 13 || ck[15 + n - 1] == 32)) n--;
        Fn2 setc = n ? need(SETTINGS "nativeSetMultipleCookies") : 0;
        if (setc) {
            memcpy(ck, ".ROBLOSECURITY=", 15); ck[15 + n] = 0;
            setc(env, settings, jni_string("https://www.roblox.com/"), jni_string(ck));
            printf("  start-up: nativeSetMultipleCookies(login cookie, %zu bytes)\n", n);
            SecureZeroMemory(ck, sizeof ck);  // shortcut: the jstring copy stays in the fake JVM heap
        } else printf("  start-up: no login cookie (%s)\n", path);
    }
    call1(SETTINGS "nativeSetRobloxChannel", "production");
    call1(SETTINGS "nativeOverrideChannelPlatformName", "GoogleAndroidApp");
    call1(SETTINGS "nativeSetRobloxVersion", skatestrap_version());
    Fn1 dev = need(SETTINGS "nativeSetDeviceInfo");
    if (dev) { printf("  start-up: nativeSetDeviceInfo\n"); dev(env, settings, device_params()); }
    call2(SETTINGS "nativeSetBaseDataDirectories", "/data/user/0/com.roblox.client", "/sdcard/Android/data/com.roblox.client");
    call1(SETTINGS "nativeSetCacheDirectory", "/data/user/0/com.roblox.client");
    call1(SETTINGS "nativeSetFilesDirectory", "/data/user/0/com.roblox.client/files");
    call1(SETTINGS "nativeSetExternalDirectory", "/sdcard/Android/data/com.roblox.client/files");
    call1(SETTINGS "nativeSetPreferencesFile", "prefs");
    Fn0 fl = need(SETTINGS "nativeInitFastLog");
    if (fl) { printf("  start-up: nativeInitFastLog\n"); fl(env, settings); }
    printf("  start-up: settings stage done\n");

    // asset manager and local storage (the Java AssetManager is an empty object: AAsset reads the extracted assets/)
    void *assets = jni_object("android/content/res/AssetManager");
    Fn1 am = need("Java_com_roblox_client_JNIAAssetManagerSetup_initNative");
    if (am) { printf("  start-up: JNIAAssetManagerSetup.initNative\n"); am(env, jni_class("com/roblox/client/JNIAAssetManagerSetup"), assets); }
    typedef SYSV void (*Fn3)(void *, void *, void *, void *, void *);
    Fn3 sm = need("Java_com_roblox_client_LocalStorageManager_initStorageManagerNativeV3");
    if (sm) {
        printf("  start-up: LocalStorageManager.initStorageManagerNativeV3\n");
        sm(env, jni_object("com/roblox/client/LocalStorageManager"), assets,
           jni_string("/data/user/0/com.roblox.client/files"), jni_string("/data/user/0/com.roblox.client/cache"));
    }
    Fn1 bu = need("Java_com_roblox_universalapp_linking_JNIBaseUrlProtocol_init");
    if (bu) { printf("  start-up: JNIBaseUrlProtocol.init\n"); bu(env, jni_class("com/roblox/universalapp/linking/JNIBaseUrlProtocol"), jni_object("com/roblox/client/startup/MainGameActivity")); }
    printf("  start-up: storage stage done\n");

    void *gl = jni_class("com/roblox/engine/jni/NativeGLInterface"), *input = jni_class("com/roblox/engine/jni/NativeInputInterface");
    Fn0 gi = need(GL "nativeGameGlobalInit");
    if (gi) { printf("  start-up: nativeGameGlobalInit\n"); gi(env, gl); }
    Fn0 ua = need(GL "nativeUpdateAdapterInit");
    if (ua) { printf("  start-up: nativeUpdateAdapterInit\n"); ua(env, gl); }
    typedef SYSV void (*FnI)(void *, void *, int);
    FnI so = need("Java_com_roblox_engine_jni_NativeInputInterface_nativeUpdateScreenOrientation");
    if (so) { printf("  start-up: nativeUpdateScreenOrientation(2)\n"); so(env, input, 2); }  // 2 = landscape
    // shortcut: no fetched settings yet (the real app gets them from clientsettingscdn.roblox.com); add with networking
    typedef SYSV int (*FnS)(void *, void *, void *, void *, void *);
    FnS cs = need(GL "nativeInitClientSettings");
    if (cs) printf("  start-up: nativeInitClientSettings -> %d\n", cs(env, gl, jni_string(flags_json()), jni_string(fps_overrides()), jni_string("GoogleAndroidApp")));
    printf("  start-up: global stage done\n");

    // no app ran before, so the list of past exits is empty (calls on unknown objects answer 0: size() == 0)
    Fn1 pc = need(GL "nativePostClientSettingsLoadedInitialization3");
    if (pc) { printf("  start-up: nativePostClientSettingsLoadedInitialization3\n"); pc(env, gl, jni_object("java/util/ArrayList")); }
    typedef SYSV void *(*FnO)(void *, void *, void *);
    FnO nf = need("Java_com_roblox_client_flags_FlagJniInterface_nativeInitializeNativeFlags");
    if (nf) printf("  start-up: nativeInitializeNativeFlags -> %p\n", nf(env, jni_class("com/roblox/client/flags/FlagJniInterface"), jni_array(0)));
    printf("  start-up: post-settings stage done\n");

    // AppBridge: init params (fill fields as the log asks). The game draws into a 1280x720 window (its Surface).
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), ww = 1280, wh = 720;
    SkateWindow *skate_window_create(int, int);
    SkateWindow *win = skate_window_create(ww, wh);
    ww = win->width; wh = win->height;  // smaller than 1280x720 with SKATESTRAP_RENDER_SCALE
    void *surface = jni_object("android/view/Surface");
    jni_set(surface, "width", ww); jni_set(surface, "height", wh); jni_set(surface, "valid", 1); jni_set(surface, "isValid", 1);
    void *pp = jni_object("com/roblox/engine/jni/model/PlatformParams");
    jni_set(pp, "surface", (uint64_t)surface);
    jni_set_str(pp, "platform", "Android"); jni_set_str(pp, "deviceName", "Skatestrap"); jni_set_str(pp, "locale", "en_us");
    jni_set_str(pp, "assetFolderPath", ASSETS);
    jni_set(pp, "width", ww); jni_set(pp, "height", wh); jni_set(pp, "screenWidth", sw); jni_set(pp, "screenHeight", sh);
    jni_set(pp, "densityDpi", 160); jni_set(pp, "viewportWidthMm", sw * 254 / 1600); jni_set(pp, "viewportHeightMm", sh * 254 / 1600);
    jni_set(pp, "dpiScale", 0x3f800000);  // 1.0f
    jni_set(pp, "isKeyboardDevice", 1); jni_set(pp, "isMouseDevice", 1);
    jni_set(pp, "isLuaHomePageEnabled", 1); jni_set(pp, "isLuaGamesPageEnabled", 1); jni_set(pp, "isLuaChatEnabled", 1);
    void *ip = jni_object("com/roblox/engine/jni/autovalue/InitParams");
    jni_set(ip, "platformParams", (uint64_t)pp);
    jni_set_str(ip, "baseURL", "https://www.roblox.com"); jni_set_str(ip, "userAgent", UA);
    jni_set_str(ip, "buildVariant", "googleProdRelease"); jni_set_str(ip, "packageName", "com.roblox.client");
    jni_set_str(ip, "platformName", "Android"); jni_set_str(ip, "appVersion", skatestrap_version());
    jni_set_str(ip, "deviceName", "Skatestrap"); jni_set_str(ip, "locale", "en_us"); jni_set_str(ip, "assetPath", ASSETS);
    jni_set(ip, "screenWidth", sw); jni_set(ip, "screenHeight", sh); jni_set(ip, "densityDpi", 160); jni_set(ip, "sdkVersion", 33);
    Fn1 sip = need("Java_com_roblox_client_startup_MainGameActivity_nativeAppBridgeSetInitParams");
    if (sip) { printf("  start-up: nativeAppBridgeSetInitParams\n"); sip(env, jni_class("com/roblox/client/startup/MainGameActivity"), ip); }
    void *bridge = jni_class("com/roblox/engine/jni/NativeAppBridgeInterface");
    Fn1 fi = need("Java_com_roblox_engine_jni_NativeAppBridgeInterface_setIsFirstInstall");
    if (fi) { printf("  start-up: setIsFirstInstall(false)\n"); fi(env, bridge, 0); }
    typedef SYSV void (*FnStart)(void *, void *, void *, void *, uint64_t, void *, void *, void *);
    FnStart as = need("Java_com_roblox_engine_jni_NativeAppBridgeInterface_nativeAppBridgeAppStart__Ljava_lang_String_2Ljava_lang_String_2ZLjava_lang_String_2Ljava_lang_String_2Ljava_lang_String_2");
    if (as) {
        printf("  start-up: nativeAppBridgeAppStart\n");
        as(env, bridge, jni_string("https://www.roblox.com/"), jni_string(UA), 0, jni_string("0000000000000000"), jni_string("AppAndroidV"), jni_string(""));
    }
    Fn1 v2 = need(GL "nativeAppBridgeV2InitWithParams");
    if (v2) { printf("  start-up: nativeAppBridgeV2InitWithParams\n"); v2(env, jni_class("com/roblox/engine/jni/NativeGLInterface"), ip); }
    printf("  start-up: AppBridge stage done\n");

    void *glc = jni_class("com/roblox/engine/jni/NativeGLInterface");
    {   // Android's activity lifecycle up to "resumed" (ported from Mocktail, Apache-2.0)
        static const char *st[] = { "PreCreated", "Created", "PostCreated", "PreStarted", "Started", "PostStarted", "PreResumed", "Resumed", "PostResumed" };
        void *lc = jni_object("com/roblox/universalapp/activitylifecyclecallbacks/JNIActivityLifecycleCallbacks");
        void *an = jni_string("MainGameActivity");
        for (int i = 0; i < 9; i++) {
            char nm[160]; snprintf(nm, sizeof nm, "Java_com_roblox_universalapp_activitylifecyclecallbacks_JNIActivityLifecycleCallbacks_nativeOn%s", st[i]);
            Fn1 f = need(nm);
            if (f) { printf("  start-up: lifecycle nativeOn%s\n", st[i]); f(env, lc, an); }
        }
    }
    Fn0 act = need("Java_com_roblox_universalapp_applifecyclenativeadapter_JNIAppLifecycleNativeAdapter_setActive");
    if (act) { printf("  start-up: JNIAppLifecycleNativeAdapter.setActive\n"); act(env, jni_class("com/roblox/universalapp/applifecyclenativeadapter/JNIAppLifecycleNativeAdapter")); }
    Fn0 frag = need(GL "nativeOnFragmentStart");
    if (frag) { printf("  start-up: nativeOnFragmentStart\n"); frag(env, glc); }
    typedef SYSV void (*FnF)(void *, void *, float);
    FnF hz = need(GL "nativePassCurrentDisplayRefreshRate");
    DEVMODEW dm = { .dmSize = sizeof dm };
    float rate = EnumDisplaySettingsW(0, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? (float)dm.dmDisplayFrequency : 60.f;
    if (hz) { printf("  start-up: nativePassCurrentDisplayRefreshRate(%.0f)\n", rate); hz(env, glc, rate); }
    Fn0 lua = need(GL "nativeAppBridgeStartLuaAppDM");
    if (lua) { printf("  start-up: nativeAppBridgeStartLuaAppDM\n"); lua(env, glc); }
    typedef SYSV void (*FnSurf)(void *, void *, void *, void *);
    FnSurf us = need(GL "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams");
    FnSurf usg = need(GL "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams");  // the in-game render view (used on resize)
    if (us) { printf("  start-up: nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams(%dx%d window)\n", ww, wh); us(env, glc, surface, pp); }
    typedef SYSV void (*FnBg)(void *, void *, int, void *);
    FnBg bg = need(GL "setTaskSchedulerBackgroundMode");
    if (bg) { printf("  start-up: setTaskSchedulerBackgroundMode(false)\n"); bg(env, glc, 0, jni_string("ASMA.start")); }
    Fn1 sa = need(GL "nativeAppBridgeV2StartAppWithParams");
    if (sa) {
        void *sp = jni_object("com/roblox/engine/jni/autovalue/StartAppParams");
        jni_set(sp, "surface", (uint64_t)surface); jni_set(sp, "platformParams", (uint64_t)pp);
        jni_set_str(sp, "appStarterPlace", "rbxasset://places/Mobile.rbxl"); jni_set_str(sp, "appStarterScript", "LuaAppStarterScript");
        jni_set_str(sp, "selectedTheme", "Dark"); jni_set_str(sp, "launchMode", "normal");
        const char *ap = "{\"launchMode\":\"normal\",\"placeId\":0}";
        jni_set_str(sp, "appParams", ap); jni_set_str(sp, "appParamsJson", ap); jni_set_str(sp, "launchParams", ap);
        jni_set(sp, "width", ww); jni_set(sp, "height", wh); jni_set(sp, "screenWidth", sw); jni_set(sp, "screenHeight", sh);
        jni_set(sp, "densityDpi", 160); jni_set(sp, "sdkVersion", 33);
        printf("  start-up: nativeAppBridgeV2StartAppWithParams\n");
        sa(env, glc, sp);
    }
    {   // what Android's Java side sends once the app runs (Mocktail order): app-ready for the Home tab
        typedef SYSV void (*FnReady)(void *, void *, void *, void *, void *, void *);
        FnReady ready = need(GL "nativeAppBridgeV2SendAppEventOnAppReady");
        if (ready) { void *e0 = jni_string(""); printf("  start-up: nativeAppBridgeV2SendAppEventOnAppReady(Home)\n"); ready(env, glc, e0, e0, e0, jni_string("Home")); }
    }
    {   // games picked in the app: subscribe to ExperienceProtocol's launch message (Mocktail RobloxExperienceLaunchBridge)
        extern void (*jni_on_raw_message)(void *, const char *), (*jni_on_cookie_sync)(void);
        jni_on_raw_message = on_raw_message; jni_on_cookie_sync = on_cookie_sync;
        g_surface = surface; g_pp = pp;
        typedef SYSV void *(*FnId)(void *, void *);
        typedef SYSV void *(*FnSub)(void *, void *, void *, void *, int);
        FnId lid = need("Java_com_roblox_universalapp_experience_JNIExperienceProtocol_getLaunchId");
        FnSub sub = need("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw");
        void *id = lid ? lid(env, jni_class("com/roblox/universalapp/experience/JNIExperienceProtocol")) : 0;
        void *con = id && sub ? sub(env, jni_object("com/roblox/universalapp/messagebus/MessageBus"), id,
                                    jni_object("com/roblox/universalapp/messagebus/RawCallback"), 0) : 0;
        printf("  start-up: ExperienceProtocol launch subscription %s\n", con ? "on" : "FAILED (games picked in the app will not start)");
    }
    {   // input on from here (the app is running)
        extern int (*skate_input)(UINT, WPARAM, LPARAM);
        const char *inp = "Java_com_roblox_engine_jni_NativeInputInterface_";
        char nm[160];
        in_env = env; in_gl = glc; in_cls = jni_class("com/roblox/engine/jni/NativeInputInterface");
        snprintf(nm, sizeof nm, "%snativePassMouseMove", inp); in_move = need(nm);
        snprintf(nm, sizeof nm, "%snativePassMouseButton", inp); in_button = need(nm);
        snprintf(nm, sizeof nm, "%snativePassMouseWheel", inp); in_wheel = need(nm);
        in_key = need(GL "nativePassKeyEvent");
        tb_pass = need(GL "nativePassText"); tb_sync = need(GL "syncTextboxTextAndCursorPosition2");
        tb_return = need(GL "nativeReturnPressedFromOnScreenKeyboard"); tb_release = need(GL "nativeReleaseFocus");
        if (!tb_sync || !tb_return || !tb_release) tb_pass = 0;
        { extern void (*jni_on_keyboard)(int, uint64_t, const char *, int, void *); jni_on_keyboard = on_keyboard; }
        snprintf(nm, sizeof nm, "%snativeSetGamepadSupportedKeyWithGamepadType", inp); gp_key = need(nm);
        snprintf(nm, sizeof nm, "%snativeSetGamepadSupportedMotionWithGamepadType", inp); gp_motion = need(nm);
        snprintf(nm, sizeof nm, "%snativeGamepadConnectEventWithGamepadType", inp); gp_connect = need(nm);
        snprintf(nm, sizeof nm, "%snativeGamepadDisconnectEvent", inp); gp_disconnect = need(nm);
        snprintf(nm, sizeof nm, "%snativeGamepadButtonEvent", inp); gp_button = need(nm);
        snprintf(nm, sizeof nm, "%snativeGamepadAxisEvent", inp); gp_axis = need(nm);
        HMODULE xi = LoadLibraryA("xinput1_4.dll"); if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
        if (xi) x_get = (void *)GetProcAddress(xi, "XInputGetState");
        printf("  start-up: gamepads %s\n", gp_button && gp_axis ? (x_get ? "on (XInput, PS4/PS5 over HID)" : "on (PS4/PS5 over HID)") : "off");
        skate_input = on_input; skate_input_on = 1;
        printf("  start-up: input on (mouse %s, keys %s)\n", in_move && in_button ? "yes" : "no", in_key ? "yes" : "no");
    }
    printf("  start-up: lifecycle stage done\n");

    // the Android main thread's Looper: Roblox queues work for it and this native runs it (Mocktail pumps it each frame)
    Fn0 pump = need(GL "nativeCallMessagesFromMainThread");
    const char *w = getenv("SKATESTRAP_WATCH_MS");
    DWORD until = GetTickCount() + (w ? atoi(w) : 10000), n = 0;
    void *gl_cls = jni_class("com/roblox/engine/jni/NativeGLInterface");
    int quit = 0;
    // SKATESTRAP_PLACE_ID=<id>: join that place 6 s after the app starts (StartGameParams fields from Mocktail's BuildStartGameParams)
    const char *place = getenv("SKATESTRAP_PLACE_ID"); DWORD join_at = GetTickCount() + 6000;
    int forever = w && atoi(w) == 0;  // SKATESTRAP_WATCH_MS=0: run until the window is closed
    int in_game = 0;  // set once the game-join thread starts, so resizes also refresh the in-game render view
    if (pump) while (!quit && (forever || (int)(until - GetTickCount()) > 0)) {
        MSG m;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) quit = 1; TranslateMessage(&m); DispatchMessageW(&m); }
        pump(env, gl_cls); n++;
        {   // window resized/fullscreened: tell the engine the new surface size so it rebuilds the render view (Mocktail)
            extern int skate_resize_pending(int *, int *); int rw, rh;
            if (skate_resize_pending(&rw, &rh)) {
                jni_set(pp, "width", rw); jni_set(pp, "height", rh);
                jni_set(surface, "width", rw); jni_set(surface, "height", rh);
                if (us) us(env, glc, surface, pp);
                if (in_game && usg) usg(env, glc, surface, pp);
                printf("  resize: surface now %dx%d\n", rw, rh);
            }
        }
        { static ULONGLONG gp_at; ULONGLONG t = GetTickCount64(); if (skate_input_on && t - gp_at >= 4) { gp_at = t; gamepad_poll(); } }
        // the app does not always call syncCookiesFromEngine after sign-in: look every 5 s (saves only when the session changes)
        { static DWORD ck_at; if (skate_input_on && GetTickCount() - ck_at >= 5000) { ck_at = GetTickCount(); on_cookie_sync(); } }
        if (place && (int)(GetTickCount() - join_at) > 0) { if (join_game(_atoi64(place), 0)) in_game = 1; place = 0; }
        if (launch_json) {  // a game picked in the app
            AcquireSRWLockExclusive(&launch_lock); char *j = launch_json; launch_json = 0; ReleaseSRWLockExclusive(&launch_lock);
            char id[32];
            if (j && !json_get(j, "placeId", id, sizeof id)) printf("  launch: request without a placeId (following a user is not wired yet); ignored\n");
            else if (j && join_game(_atoi64(id), j)) in_game = 1;
            free(j);
        }
        static int jiggle = -1; if (jiggle < 0) jiggle = getenv("SKATESTRAP_JIGGLE") != 0;
        if (jiggle && skate_input_on && n % 16 == 0) {  // test: fake mouse moves inside our window (does input raise fps?)
            int k = (int)(n / 16) % 200;
            on_input(WM_MOUSEMOVE, 0, MAKELPARAM(400 + k, 300 + k / 2));
        }
        {   // test: SKATESTRAP_TEST_ESC_MS=<ms after start> presses Esc once (opens the in-game menu, which drops fps)
            static DWORD esc_at = 1; if (esc_at == 1) { const char *e = getenv("SKATESTRAP_TEST_ESC_MS"); esc_at = e ? GetTickCount() + atoi(e) : 0; }
            if (esc_at && skate_input_on && (int)(GetTickCount() - esc_at) > 0) {
                esc_at = 0; puts("  test: Esc pressed");
                on_input(WM_KEYDOWN, VK_ESCAPE, 0x00010001); on_input(WM_KEYUP, VK_ESCAPE, 0xC0010001);
            }
        }
        {   // test: SKATESTRAP_TEST_RESIZE_MS=<ms after start> maximizes the window once (checks the resize path fills the game)
            static DWORD rz_at = 1; if (rz_at == 1) { const char *e = getenv("SKATESTRAP_TEST_RESIZE_MS"); rz_at = e ? GetTickCount() + atoi(e) : 0; }
            if (rz_at && (int)(GetTickCount() - rz_at) > 0) {
                rz_at = 0; puts("  test: window maximized");
                extern void *skate_window_hwnd(void); HWND hw = (HWND)skate_window_hwnd(); if (hw) ShowWindow(hw, SW_MAXIMIZE);
            }
        }
        // uncapped (SKATESTRAP_FPS_CAP set): never sleep, like Mocktail's PaceInputPump; else wake every 1 ms or on input
        static int spin = -1; if (spin < 0) spin = getenv("SKATESTRAP_FPS_CAP") != 0;
        // uncapped: nap up to SKATESTRAP_PUMP_US (250) us on a high-res timer or until input, so the pump does not hold a whole CPU (0 = spin)
        static int nap_us = -1; static HANDLE nap_t;
        if (nap_us < 0) { const char *e = getenv("SKATESTRAP_PUMP_US"); nap_us = e ? atoi(e) : 250;
            if (nap_us > 0) nap_t = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (nap_us > 0) printf("  start-up: pump naps %d us (%s timer)\n", nap_us, nap_t ? "high-res" : "no"); }
        // run L: render and Main hand work back and forth hundreds of times in slow frames, and each nap cost ~0.5 ms.
        // So no nap within 2 ms of this thread waking the render thread (run M, no nap: worst-gap median 21 -> 6 ms)
        static LONG seen_ho; static int64_t hot_until; int64_t now_ns(int); int64_t tnow = now_ns(1);
        skate_pump_tid = GetCurrentThreadId();
        if (skate_pump_handoffs != seen_ho) { seen_ho = skate_pump_handoffs; hot_until = tnow + 2000000; skate_pump_hot++; }
        if (spin && nap_t && tnow >= hot_until) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)nap_us * 10;
            if (SetWaitableTimerEx(nap_t, &due, 0, 0, 0, 0, 0)) MsgWaitForMultipleObjects(1, &nap_t, FALSE, INFINITE, QS_ALLINPUT); }
        if (!spin) MsgWaitForMultipleObjects(0, 0, FALSE, 1, QS_ALLINPUT);
    }
    printf("  start-up: main-thread pump ran %lu times, %ld handing work to render\n", n, skate_pump_hot);
    extern volatile long long skate_exec_bytes, skate_exec_calls, skate_exec_fails;
    printf("  native code: %lld KB made executable in %lld calls, %lld failed\n", skate_exec_bytes >> 10, skate_exec_calls, skate_exec_fails);
}

// Selftest: PS4 / PS5 reports (USB, Bluetooth short and full) -> XInput buttons, sticks, triggers
int shim_pad_selftest(void) {
    int bad = 0; XPad g; BYTE r[80] = { 0 };
    r[0] = 0x01; r[1] = 255; r[2] = 0; r[3] = 128; r[4] = 255; r[5] = 0x20 | 2; r[6] = 0x01 | 0x20; r[8] = 200; r[9] = 10;  // DS4 USB
    if (!hid_parse(r, 64, 0, &g) || g.b != (0x1000 | 0x0008 | 0x0100 | 0x0010) || g.lx != 32767 || g.ly != 32767 || g.ry != -32768 || g.lt != 200 || g.rt != 10) { bad++; printf("pad selftest: DS4 USB\n"); }
    if (!hid_parse(r, 10, 1, &g) || g.b != (0x1000 | 0x0008 | 0x0100 | 0x0010)) { bad++; printf("pad selftest: DS5 Bluetooth short\n"); }
    memset(r, 0, sizeof r); r[0] = 0x01; r[1] = 128; r[5] = 77; r[6] = 99; r[8] = 0x80 | 8; r[9] = 0x80;  // DS5 USB
    if (!hid_parse(r, 64, 1, &g) || g.b != (0x8000 | 0x0080) || g.lt != 77 || g.rt != 99 || g.lx != 128) { bad++; printf("pad selftest: DS5 USB\n"); }
    memmove(r + 1, r, 64); r[0] = 0x31;  // DS5 Bluetooth full
    if (!hid_parse(r, 78, 1, &g) || g.b != (0x8000 | 0x0080) || g.lt != 77) { bad++; printf("pad selftest: DS5 Bluetooth\n"); }
    memset(r, 0, sizeof r); r[0] = 0x11; r[7] = 0x10 | 8; r[8] = 0x02;  // DS4 Bluetooth full
    if (!hid_parse(r, 78, 0, &g) || g.b != (0x4000 | 0x0200)) { bad++; printf("pad selftest: DS4 Bluetooth\n"); }
    r[0] = 0x05; if (hid_parse(r, 78, 0, &g)) { bad++; printf("pad selftest: other report taken\n"); }
    {   // launch JSON from the app (ExperienceProtocol)
        const char *j = "{\"referralPage\":\"placeId\",\"placeId\": 606849621,\"launchData\":\"{\\\"a\\\":1}\",\"gameInstanceId\":null,\"userId\":0}";
        char v[64];
        if (!json_get(j, "placeId", v, sizeof v) || strcmp(v, "606849621")) { bad++; printf("launch selftest: placeId\n"); }
        if (!json_get(j, "launchData", v, sizeof v) || strcmp(v, "{\"a\":1}")) { bad++; printf("launch selftest: launchData\n"); }
        if (json_get(j, "gameInstanceId", v, sizeof v) || json_get(j, "accessCode", v, sizeof v)) { bad++; printf("launch selftest: null/absent\n"); }
    }
    return bad;
}
