// Prints the GLES extensions ANGLE (loader/libEGL.dll) exposes, one per line.
#include <windows.h>
#include <stdio.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
int main(void) {
    HMODULE e = LoadLibraryA("../loader/libEGL.dll"); LoadLibraryA("../loader/libGLESv2.dll");
    HMODULE g = GetModuleHandleA("libGLESv2.dll");
#define P(m, t, n) t n = (t)GetProcAddress(m, #n)
    P(e, PFNEGLGETDISPLAYPROC, eglGetDisplay); P(e, PFNEGLINITIALIZEPROC, eglInitialize); P(e, PFNEGLCHOOSECONFIGPROC, eglChooseConfig);
    P(e, PFNEGLCREATECONTEXTPROC, eglCreateContext); P(e, PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface); P(e, PFNEGLMAKECURRENTPROC, eglMakeCurrent);
    const GLubyte *(*gsi)(GLenum, GLuint) = (void *)GetProcAddress(g, "glGetStringi"); void (*gi)(GLenum, GLint *) = (void *)GetProcAddress(g, "glGetIntegerv");
    const GLubyte *(*gs)(GLenum) = (void *)GetProcAddress(g, "glGetString");
    EGLDisplay d = eglGetDisplay(0); eglInitialize(d, 0, 0);
    EGLint ca[] = { EGL_RENDERABLE_TYPE, 0x40, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE }, n; EGLConfig c; eglChooseConfig(d, ca, &c, 1, &n);
    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE }, pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLContext x = eglCreateContext(d, c, 0, xa); EGLSurface s = eglCreatePbufferSurface(d, c, pa); eglMakeCurrent(d, s, s, x);
    fprintf(stderr, "%s\n", gs(GL_VERSION));
    GLint k = 0; gi(GL_NUM_EXTENSIONS, &k);
    for (GLint i = 0; i < k; i++) printf("%s\n", gsi(GL_EXTENSIONS, i));
}
