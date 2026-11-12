// Can the GPU driver give OpenGL ES (or desktop GL with ES3 compatibility) contexts straight through WGL, no ANGLE?
// Build: clang wglcheck.c -lopengl32 -lgdi32 -luser32 -o wglcheck.exe
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x1
#define WGL_CONTEXT_ES2_PROFILE_BIT_EXT 0x4
#define GL_NUM_EXTENSIONS 0x821D
typedef HGLRC(WINAPI *CreateAttribs)(HDC, HGLRC, const int *);
typedef const char *(WINAPI *ExtString)(HDC);
typedef const GLubyte *(WINAPI *GetStringi)(GLenum, GLuint);

static void try_ctx(HDC dc, CreateAttribs ca, int major, int minor, int profile, const char *what) {
    int at[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, major, WGL_CONTEXT_MINOR_VERSION_ARB, minor, WGL_CONTEXT_PROFILE_MASK_ARB, profile, 0 };
    HGLRC rc = ca(dc, 0, at);
    if (!rc) { printf("%-22s %d.%d: refused (error 0x%lx)\n", what, major, minor, GetLastError() & 0xffff); return; }
    wglMakeCurrent(dc, rc);
    printf("%-22s %d.%d: OK -> %s\n", what, major, minor, (const char *)glGetString(GL_VERSION));
    if (profile == WGL_CONTEXT_CORE_PROFILE_BIT_ARB) {
        GetStringi gs = (GetStringi)wglGetProcAddress("glGetStringi"); GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        int es3 = 0; for (GLint i = 0; gs && i < n; i++) if (!strcmp((const char *)gs(GL_EXTENSIONS, i), "GL_ARB_ES3_compatibility")) es3 = 1;
        int es32 = 0; for (GLint i = 0; gs && i < n; i++) if (!strcmp((const char *)gs(GL_EXTENSIONS, i), "GL_ARB_ES3_2_compatibility")) es32 = 1;
        printf("    GL_ARB_ES3_compatibility: %s, GL_ARB_ES3_2_compatibility: %s\n", es3 ? "yes" : "no", es32 ? "yes" : "no");
    }
    wglMakeCurrent(0, 0); wglDeleteContext(rc);
}

int main(void) {
    WNDCLASSA wc = { .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(0), .lpszClassName = "wglcheck" };
    RegisterClassA(&wc);
    HWND w = CreateWindowA("wglcheck", "wglcheck", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC old = wglCreateContext(dc); wglMakeCurrent(dc, old);
    printf("vendor:   %s\nrenderer: %s\nlegacy:   %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    ExtString es = (ExtString)wglGetProcAddress("wglGetExtensionsStringARB");
    const char *ext = es ? es(dc) : "";
    printf("WGL_EXT_create_context_es2_profile: %s\n", strstr(ext, "WGL_EXT_create_context_es2_profile") ? "yes" : "no");
    printf("WGL_EXT_create_context_es_profile:  %s\n", strstr(ext, "WGL_EXT_create_context_es_profile") ? "yes" : "no");
    CreateAttribs ca = (CreateAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    wglMakeCurrent(0, 0); wglDeleteContext(old);
    if (!ca) { printf("no wglCreateContextAttribsARB\n"); return 1; }
    try_ctx(dc, ca, 3, 2, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, "OpenGL ES");
    try_ctx(dc, ca, 3, 1, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, "OpenGL ES");
    try_ctx(dc, ca, 3, 0, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, "OpenGL ES");
    try_ctx(dc, ca, 2, 0, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, "OpenGL ES");
    try_ctx(dc, ca, 4, 6, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, "desktop GL core");
    return 0;
}
