#include <windows.h>
#include <stdio.h>
void *wgl_gl_proc(const char *); int wgl_begin(void); void wgl_end(void);
int main(void) {
    wgl_begin();
    const char *n[] = { "glClear", "glDrawElements", "glDrawArrays", "glTexImage2D", "glBindTexture", "glViewport", "glGetIntegerv", "glFinish", "glFlush", "glEnable", "glBlendFunc", "glReadPixels", "glGetError", "glScissor", "glDeleteTextures", "glGenTextures", "glTexParameteri", "glPixelStorei", "glDepthMask", "glColorMask", "glCullFace", "glPolygonOffset" };
    int miss = 0;
    for (int i = 0; i < sizeof n / sizeof *n; i++) { PROC p = wglGetProcAddress(n[i]); if ((UINT_PTR)p <= 3 || p == (PROC)-1) { printf("only opengl32: %s\n", n[i]); miss++; } }
    printf("%d of %d need opengl32\n", miss, (int)(sizeof n / sizeof *n));
    wgl_end();
}
