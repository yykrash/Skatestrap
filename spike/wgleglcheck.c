// Drives loader/wgl_egl.c the way the game does: config, ES 3 context, window + pbuffer, shared context on a 2nd thread.
#include <windows.h>
#include <stdio.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
void *wgl_egl_proc(const char *), *wgl_gl_proc(const char *); int wgl_begin(void); void wgl_end(void);
#define F(t, n, ...) t (*n)(__VA_ARGS__) = wgl_egl_proc(#n)
static EGLContext share; static EGLConfig cfg;
static DWORD WINAPI worker(void *p) {
    F(EGLContext, eglCreateContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    F(EGLSurface, eglCreatePbufferSurface, EGLDisplay, EGLConfig, const EGLint *);
    F(EGLBoolean, eglMakeCurrent, EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE }, pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLContext c = eglCreateContext((EGLDisplay)1, cfg, share, ca);
    EGLSurface s = eglCreatePbufferSurface((EGLDisplay)1, cfg, pa);
    printf("worker: shared ctx %p, current %d, GL %s\n", c, eglMakeCurrent((EGLDisplay)1, s, s, c), glGetString(GL_VERSION));
    return 0;
}
int main(void) {
    if (!wgl_begin()) return 1;
    void (*clearColor)(float, float, float, float) = wgl_gl_proc("glClearColor");
    void *(*mapRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = wgl_gl_proc("glMapBufferRange");
    const GLubyte *(*gs)(GLenum) = wgl_gl_proc("glGetString"); GLenum (*ge)(void) = wgl_gl_proc("glGetError");
    wgl_end();
    printf("glClearColor %p glMapBufferRange %p glDiscardFramebufferEXT %p\n", clearColor, mapRange, 0);
    F(EGLDisplay, eglGetDisplay, EGLNativeDisplayType); F(EGLBoolean, eglInitialize, EGLDisplay, EGLint *, EGLint *);
    F(EGLBoolean, eglChooseConfig, EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    F(EGLContext, eglCreateContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    F(EGLSurface, eglCreateWindowSurface, EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    F(EGLBoolean, eglMakeCurrent, EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    F(EGLBoolean, eglSwapBuffers, EGLDisplay, EGLSurface); F(EGLBoolean, eglQuerySurface, EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLDisplay d = eglGetDisplay(0); EGLint ma, mi, n; eglInitialize(d, &ma, &mi);
    EGLint want[] = { EGL_RENDERABLE_TYPE, 0x40, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    eglChooseConfig(d, want, 0, 0, &n); printf("EGL %d.%d, %d configs match\n", ma, mi, n);
    eglChooseConfig(d, want, &cfg, 1, &n);
    WNDCLASSA wc = { .style = CS_OWNDC, .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(0), .lpszClassName = "t" };
    RegisterClassA(&wc);
    HWND w = CreateWindowA("t", "t", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, 0, 0, wc.hInstance, 0);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    share = eglCreateContext(d, cfg, 0, ca);
    EGLSurface s = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)w, 0);
    EGLint W, H; eglQuerySurface(d, s, EGL_WIDTH, &W); eglQuerySurface(d, s, EGL_HEIGHT, &H);
    printf("main: current %d, %dx%d, %s\n", eglMakeCurrent(d, s, s, share), W, H, glGetString(GL_VERSION));
    printf("table glGetString: %s, wglGetCurrentContext %p, GetLastError-ish glGetError 0x%x\n", gs(GL_VERSION), wglGetCurrentContext(), ge());
    clearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); printf("swap %d glError 0x%x\n", eglSwapBuffers(d, s), glGetError());
    HANDLE t = CreateThread(0, 0, worker, 0, 0, 0); WaitForSingleObject(t, INFINITE);
    return 0;
}
