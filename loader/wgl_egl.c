// SKATESTRAP_GL=native: the game's EGL calls answered with OpenGL ES contexts straight from the GPU driver
// (WGL_EXT_create_context_es2_profile), so no ANGLE translation sits between the game and the driver.
// Only what the game imports is real: displays, configs, contexts, window/pbuffer surfaces, swap.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_ES2_PROFILE_BIT_EXT 0x4
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x40
#endif
#define EGL_CONTEXT_MINOR_VERSION 0x30FB

static HGLRC (WINAPI *create_attribs)(HDC, HGLRC, const int *);
static BOOL (WINAPI *swap_interval)(int);
static HMODULE opengl32;
static int pixel_format;
static PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32,
                                     0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8 };

typedef struct { HGLRC rc; int major, minor; EGLint cfg; } Ctx;
typedef struct { HWND hwnd; HDC dc; EGLint w, h, cfg; } Surf;  // hwnd 0 = pbuffer

static __thread EGLint t_error = EGL_SUCCESS;
static __thread Ctx *t_ctx;
static __thread Surf *t_draw;
static __thread HDC t_dummy;  // this thread's hidden window, for pbuffers and contexts without a surface
static EGLBoolean fail(EGLint e) { t_error = e; return EGL_FALSE; }

// Every surface and context uses one pixel format (RGBA8, depth 24, stencil 8), so any context fits any DC.
static void set_format(HDC dc) {
    if (!pixel_format) pixel_format = ChoosePixelFormat(dc, &pfd);
    if (!GetPixelFormat(dc)) SetPixelFormat(dc, pixel_format, &pfd);
}
static HDC dummy_dc(void) {
    if (t_dummy) return t_dummy;
    static LONG reg;
    if (!InterlockedExchange(&reg, 1)) {
        WNDCLASSA wc = { .style = CS_OWNDC, .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(0), .lpszClassName = "SkatestrapGL" };
        RegisterClassA(&wc);
    }
    // the window lives as long as this thread, which is as long as anything made current on it
    HWND w = CreateWindowA("SkatestrapGL", "", WS_POPUP, 0, 0, 1, 1, 0, 0, GetModuleHandleA(0), 0);
    t_dummy = GetDC(w);
    set_format(t_dummy);
    return t_dummy;
}

// --- configs: 1 = RGBA8 D24S8, 2 = RGB8 D24S8 (same real pixel format, alpha just not promised) ---
#define NCFG 2
static EGLint cfg_attr(EGLint id, EGLint a) {
    switch (a) {
    case EGL_CONFIG_ID: return id;
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: return 8;
    case EGL_ALPHA_SIZE: return id == 1 ? 8 : 0;
    case EGL_BUFFER_SIZE: return id == 1 ? 32 : 24;
    case EGL_DEPTH_SIZE: return 24;
    case EGL_STENCIL_SIZE: return 8;
    case EGL_SURFACE_TYPE: return EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: return EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
    case EGL_COLOR_BUFFER_TYPE: return EGL_RGB_BUFFER;
    case EGL_CONFIG_CAVEAT: case EGL_TRANSPARENT_TYPE: return EGL_NONE;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: return 16384;
    case EGL_MAX_PBUFFER_PIXELS: return 16384 * 16384;
    case EGL_MAX_SWAP_INTERVAL: return 1;
    default: return 0;  // samples, luminance, native visual, Android extras: none
    }
}
static int cfg_ok(EGLint id, const EGLint *at) {
    for (; at && at[0] != EGL_NONE; at += 2) {
        EGLint a = at[0], v = at[1], c = cfg_attr(id, a);
        if (v == EGL_DONT_CARE) continue;
        switch (a) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_BUFFER_SIZE:
        case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
            if (c < v) return 0; break;
        case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
            if ((c & v) != v) return 0; break;
        case EGL_CONFIG_ID: if (c != v) return 0; break;
        }
    }
    return 1;
}
static EGLint cfg_id(EGLConfig c) { EGLint id = (EGLint)(intptr_t)c; return id >= 1 && id <= NCFG ? id : 0; }

static EGLDisplay e_GetDisplay(EGLNativeDisplayType d) { return (EGLDisplay)1; }
static EGLBoolean e_Initialize(EGLDisplay d, EGLint *ma, EGLint *mi) { if (ma) *ma = 1; if (mi) *mi = 4; return EGL_TRUE; }
static EGLBoolean e_Terminate(EGLDisplay d) { return EGL_TRUE; }
static EGLint e_GetError(void) { EGLint e = t_error; t_error = EGL_SUCCESS; return e; }
static EGLBoolean e_ChooseConfig(EGLDisplay d, const EGLint *at, EGLConfig *out, EGLint size, EGLint *n) {
    if (!n) return fail(EGL_BAD_PARAMETER);
    *n = 0;
    for (EGLint id = 1; id <= NCFG; id++)
        if (cfg_ok(id, at)) { if (out && *n < size) out[*n] = (EGLConfig)(intptr_t)id; if (out ? *n < size : 1) ++*n; }
    return EGL_TRUE;
}
static EGLBoolean e_GetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v) {
    if (!cfg_id(c)) return fail(EGL_BAD_CONFIG);
    *v = cfg_attr(cfg_id(c), a);
    return EGL_TRUE;
}

static EGLContext e_CreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at) {
    if (!cfg_id(c)) { fail(EGL_BAD_CONFIG); return EGL_NO_CONTEXT; }
    int major = 1, minor = 0;
    for (; at && at[0] != EGL_NONE; at += 2)
        if (at[0] == EGL_CONTEXT_CLIENT_VERSION) major = at[1];
        else if (at[0] == EGL_CONTEXT_MINOR_VERSION) minor = at[1];
    if (major < 2) major = 2;
    int wa[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, major, WGL_CONTEXT_MINOR_VERSION_ARB, minor,
                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, 0 };
    HGLRC rc = create_attribs(dummy_dc(), share ? ((Ctx *)share)->rc : 0, wa);
    if (!rc) { printf("native GL: no ES %d.%d context (error 0x%lx)\n", major, minor, GetLastError()); fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
    Ctx *x = calloc(1, sizeof *x);
    x->rc = rc; x->major = major; x->minor = minor; x->cfg = cfg_id(c);
    static LONG said; if (!InterlockedExchange(&said, 1)) printf("native GL: ES %d.%d context from the GPU driver (no ANGLE)\n", major, minor);
    return x;
}
static EGLBoolean e_DestroyContext(EGLDisplay d, EGLContext c) {
    if (!c) return fail(EGL_BAD_CONTEXT);
    // shortcut: freed at once even if still current on another thread (EGL defers); the game releases first
    wglDeleteContext(((Ctx *)c)->rc);
    if (t_ctx == c) t_ctx = 0;
    free(c);
    return EGL_TRUE;
}
static EGLSurface e_CreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *at) {
    if (!cfg_id(c)) { fail(EGL_BAD_CONFIG); return EGL_NO_SURFACE; }
    Surf *s = calloc(1, sizeof *s);
    s->hwnd = (HWND)w; s->dc = GetDC(s->hwnd); s->cfg = cfg_id(c);  // the game window class has CS_OWNDC
    set_format(s->dc);
    return s;
}
static EGLSurface e_CreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *at) {
    if (!cfg_id(c)) { fail(EGL_BAD_CONFIG); return EGL_NO_SURFACE; }
    Surf *s = calloc(1, sizeof *s);
    s->cfg = cfg_id(c);
    for (; at && at[0] != EGL_NONE; at += 2)
        if (at[0] == EGL_WIDTH) s->w = at[1]; else if (at[0] == EGL_HEIGHT) s->h = at[1];
    return s;  // shortcut: drawn into the thread's hidden 1x1 window; fine while the game only uses it to make a context current
}
static EGLBoolean e_DestroySurface(EGLDisplay d, EGLSurface s) {
    if (!s) return fail(EGL_BAD_SURFACE);
    if (t_draw == s) t_draw = 0;
    free(s);
    return EGL_TRUE;
}
static EGLBoolean e_MakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) {
    if (!c) { wglMakeCurrent(0, 0); t_ctx = 0; t_draw = 0; return EGL_TRUE; }
    Surf *s = draw;
    HDC dc = s && s->hwnd ? s->dc : dummy_dc();
    if (!wglMakeCurrent(dc, ((Ctx *)c)->rc)) {
        printf("native GL: wglMakeCurrent failed (error 0x%lx)\n", GetLastError());
        return fail(EGL_BAD_ACCESS);
    }
    t_ctx = c; t_draw = s;
    return EGL_TRUE;
}
static EGLContext e_GetCurrentContext(void) { return t_ctx ? t_ctx : EGL_NO_CONTEXT; }
static EGLSurface e_GetCurrentSurface(EGLint which) { return t_draw ? t_draw : EGL_NO_SURFACE; }
static EGLDisplay e_GetCurrentDisplay(void) { return t_ctx ? (EGLDisplay)1 : EGL_NO_DISPLAY; }
static EGLBoolean e_QuerySurface(EGLDisplay d, EGLSurface sf, EGLint a, EGLint *v) {
    Surf *s = sf;
    if (!s) return fail(EGL_BAD_SURFACE);
    RECT r = { 0 };
    if (s->hwnd) GetClientRect(s->hwnd, &r);
    switch (a) {
    case EGL_WIDTH: *v = s->hwnd ? r.right : s->w; break;
    case EGL_HEIGHT: *v = s->hwnd ? r.bottom : s->h; break;
    case EGL_CONFIG_ID: *v = s->cfg; break;
    case EGL_RENDER_BUFFER: *v = s->hwnd ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER; break;
    case EGL_SWAP_BEHAVIOR: *v = EGL_BUFFER_DESTROYED; break;
    default: *v = 0;
    }
    return EGL_TRUE;
}
static EGLBoolean e_QueryContext(EGLDisplay d, EGLContext c, EGLint a, EGLint *v) {
    Ctx *x = c;
    if (!x) return fail(EGL_BAD_CONTEXT);
    *v = a == EGL_CONTEXT_CLIENT_VERSION ? x->major : a == EGL_CONFIG_ID ? x->cfg : a == EGL_CONTEXT_CLIENT_TYPE ? EGL_OPENGL_ES_API : 0;
    return EGL_TRUE;
}
static EGLBoolean e_SwapBuffers(EGLDisplay d, EGLSurface sf) {
    Surf *s = sf;
    if (!s) return fail(EGL_BAD_SURFACE);
    return s->hwnd ? SwapBuffers(s->dc) : EGL_TRUE;
}
static EGLBoolean e_SwapInterval(EGLDisplay d, EGLint i) { return swap_interval ? swap_interval(i) : EGL_TRUE; }
static const char *e_QueryString(EGLDisplay d, EGLint n) {
    return n == EGL_VENDOR ? "Skatestrap" : n == EGL_VERSION ? "1.4 Skatestrap native" : n == EGL_CLIENT_APIS ? "OpenGL_ES" : "";
}
static EGLBoolean e_true(void) { return EGL_TRUE; }
static void (*finish)(void);  // the driver's own; opengl32.dll exports answer nothing in ES contexts
static EGLBoolean e_WaitGL(void) { if (finish) finish(); return EGL_TRUE; }
static EGLenum e_QueryAPI(void) { return EGL_OPENGL_ES_API; }

static const struct { const char *name; void *f; } egl_procs[] = {
    { "eglGetDisplay", e_GetDisplay }, { "eglInitialize", e_Initialize }, { "eglTerminate", e_Terminate },
    { "eglGetError", e_GetError }, { "eglChooseConfig", e_ChooseConfig }, { "eglGetConfigAttrib", e_GetConfigAttrib },
    { "eglCreateContext", e_CreateContext }, { "eglDestroyContext", e_DestroyContext },
    { "eglCreateWindowSurface", e_CreateWindowSurface }, { "eglCreatePbufferSurface", e_CreatePbufferSurface },
    { "eglDestroySurface", e_DestroySurface }, { "eglMakeCurrent", e_MakeCurrent },
    { "eglGetCurrentContext", e_GetCurrentContext }, { "eglGetCurrentSurface", e_GetCurrentSurface },
    { "eglGetCurrentDisplay", e_GetCurrentDisplay }, { "eglQuerySurface", e_QuerySurface },
    { "eglQueryContext", e_QueryContext }, { "eglSwapBuffers", e_SwapBuffers }, { "eglSwapInterval", e_SwapInterval },
    { "eglQueryString", e_QueryString }, { "eglBindAPI", e_true }, { "eglQueryAPI", e_QueryAPI },
    { "eglReleaseThread", e_true }, { "eglWaitGL", e_WaitGL }, { "eglWaitClient", e_WaitGL }, { "eglWaitNative", e_true },
};
void *wgl_egl_proc(const char *name) {
    for (size_t i = 0; i < sizeof egl_procs / sizeof egl_procs[0]; i++)
        if (!strcmp(egl_procs[i].name, name)) return egl_procs[i].f;
    return 0;
}

// gl* entry points. wglGetProcAddress answers only while a context is current, so start-up resolves
// them all once inside wgl_begin/wgl_end (driver entry points are the same for every context).
static HDC boot_dc; static HGLRC boot_rc;
int wgl_begin(void) {
    opengl32 = LoadLibraryA("opengl32.dll");
    boot_dc = dummy_dc();
    HGLRC legacy = wglCreateContext(boot_dc);
    wglMakeCurrent(boot_dc, legacy);
    create_attribs = (void *)wglGetProcAddress("wglCreateContextAttribsARB");
    const char *(WINAPI *ext)(HDC) = (void *)wglGetProcAddress("wglGetExtensionsStringARB");
    int es = ext && strstr(ext(boot_dc), "WGL_EXT_create_context_es2_profile");
    wglMakeCurrent(0, 0); wglDeleteContext(legacy);
    if (!create_attribs || !es) { printf("native GL: this GPU driver cannot make OpenGL ES contexts (try Vulkan or ANGLE)\n"); return 0; }
    int wa[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 2, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_ES2_PROFILE_BIT_EXT, 0 };
    boot_rc = create_attribs(boot_dc, 0, wa);
    if (!boot_rc || !wglMakeCurrent(boot_dc, boot_rc)) { printf("native GL: ES 3.2 context refused (error 0x%lx)\n", GetLastError()); return 0; }
    swap_interval = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    finish = (void *)wglGetProcAddress("glFinish");
    printf("native GL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return 1;
}
void *wgl_gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || p == (void *)-1) p = (void *)GetProcAddress(opengl32, name);  // shortcut: opengl32 exports return nothing in ES contexts on AMD; fine while drivers give every name directly
    return p;
}
void wgl_end(void) { wglMakeCurrent(0, 0); wglDeleteContext(boot_rc); boot_rc = 0; }


// --- what the game is told about the driver (native mode) ---
// The game picks code paths from the GL version and extension list. On ES 3.2 with every AMD extension it
// took paths ANGLE never offered (glBufferStorage with no wrapper -> call to 0; uint*int shaders that fail).
// So it is shown what ANGLE shows: ES 3.0, and only extensions ANGLE also lists (spike/angle_ext.txt).
#include "shim.h"
void *shim_gl_host(const char *name);
static const char angle_ext[] =
    "GL_AMD_performance_monitor GL_APPLE_clip_distance GL_EXT_EGL_image_external_wrap_modes "
    "GL_EXT_base_instance GL_EXT_blend_func_extended GL_EXT_blend_minmax GL_EXT_clip_control "
    "GL_EXT_clip_cull_distance GL_EXT_color_buffer_float GL_EXT_color_buffer_half_float "
    "GL_EXT_conservative_depth GL_EXT_debug_label GL_EXT_debug_marker GL_EXT_depth_clamp "
    "GL_EXT_discard_framebuffer GL_EXT_disjoint_timer_query GL_EXT_draw_buffers "
    "GL_EXT_draw_buffers_indexed GL_EXT_draw_elements_base_vertex GL_EXT_float_blend GL_EXT_frag_depth "
    "GL_EXT_instanced_arrays GL_EXT_map_buffer_range GL_EXT_multi_draw_indirect "
    "GL_EXT_multisampled_render_to_texture GL_EXT_occlusion_query_boolean GL_EXT_polygon_offset_clamp "
    "GL_EXT_read_format_bgra GL_EXT_render_snorm GL_EXT_robustness GL_EXT_sRGB GL_EXT_shader_texture_lod "
    "GL_EXT_texture_border_clamp GL_EXT_texture_compression_bptc GL_EXT_texture_compression_dxt1 "
    "GL_EXT_texture_compression_rgtc GL_EXT_texture_compression_s3tc_srgb "
    "GL_EXT_texture_filter_anisotropic GL_EXT_texture_format_BGRA8888 GL_EXT_texture_mirror_clamp_to_edge "
    "GL_EXT_texture_norm16 GL_EXT_texture_rg GL_EXT_texture_storage GL_EXT_texture_type_2_10_10_10_REV "
    "GL_EXT_unpack_subimage GL_KHR_debug GL_KHR_parallel_shader_compile "
    "GL_NV_EGL_stream_consumer_external GL_NV_fence GL_NV_framebuffer_blit GL_NV_pack_subimage "
    "GL_NV_pixel_buffer_object GL_NV_shader_noperspective_interpolation GL_OES_EGL_image "
    "GL_OES_EGL_image_external GL_OES_EGL_image_external_essl3 GL_OES_compressed_EAC_R11_signed_texture "
    "GL_OES_compressed_EAC_R11_unsigned_texture GL_OES_compressed_EAC_RG11_signed_texture "
    "GL_OES_compressed_EAC_RG11_unsigned_texture GL_OES_compressed_ETC2_RGB8_texture "
    "GL_OES_compressed_ETC2_RGBA8_texture GL_OES_compressed_ETC2_punchthroughA_RGBA8_texture "
    "GL_OES_compressed_ETC2_punchthroughA_sRGB8_alpha_texture GL_OES_compressed_ETC2_sRGB8_alpha8_texture "
    "GL_OES_compressed_ETC2_sRGB8_texture GL_OES_depth24 GL_OES_depth32 GL_OES_draw_buffers_indexed "
    "GL_OES_draw_elements_base_vertex GL_OES_element_index_uint GL_OES_fbo_render_mipmap "
    "GL_OES_get_program_binary GL_OES_mapbuffer GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 "
    "GL_OES_sample_variables GL_OES_shader_multisample_interpolation GL_OES_standard_derivatives "
    "GL_OES_surfaceless_context GL_OES_texture_border_clamp GL_OES_texture_float "
    "GL_OES_texture_float_linear GL_OES_texture_half_float GL_OES_texture_half_float_linear "
    "GL_OES_texture_npot GL_OES_texture_stencil8 GL_OES_vertex_array_object GL_OVR_multiview "
    "GL_OVR_multiview2 ";
static char ext_str[8192]; static const char *ext_list[256]; static GLint ext_n = -1;
static char ver_str[160];
static const GLubyte *(*h_GetString)(GLenum); static const GLubyte *(*h_GetStringi)(GLenum, GLuint); static void (*h_GetIntegerv)(GLenum, GLint *);
static int in_angle(const char *e) {
    size_t l = strlen(e);
    for (const char *p = angle_ext; (p = strstr(p, e)); p += l)
        if ((p == angle_ext || p[-1] == ' ') && (p[l] == ' ' || !p[l])) return 1;
    return 0;
}
static void ext_build(void) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (ext_n < 0) {
        GLint n = 0, k = 0; h_GetIntegerv(GL_NUM_EXTENSIONS, &n);
        char *w = ext_str;
        for (GLint i = 0; i < n && k < 256; i++) {
            const char *e = (const char *)h_GetStringi(GL_EXTENSIONS, i);
            if (!e || !in_angle(e) || w + strlen(e) + 2 >= ext_str + sizeof ext_str) continue;
            ext_list[k++] = w; strcpy(w, e); w += strlen(e) + 1;
        }
        snprintf(ver_str, sizeof ver_str, "OpenGL ES 3.0 Skatestrap native (%s)", (const char *)h_GetString(GL_VERSION));
        printf("native GL: telling the game ES 3.0 with %d of the driver's %d extensions (ANGLE's set)\n", k, n);
        ext_n = k;
    }
    ReleaseSRWLockExclusive(&lock);
}
static SYSV const GLubyte *n_glGetString(GLenum name) {
    static char joined[8192];
    if (name == GL_VERSION || name == GL_SHADING_LANGUAGE_VERSION || name == GL_EXTENSIONS) ext_build();
    if (name == GL_VERSION) return (const GLubyte *)ver_str;
    if (name == GL_SHADING_LANGUAGE_VERSION) return (const GLubyte *)"OpenGL ES GLSL ES 3.00";
    if (name == GL_EXTENSIONS) {
        if (!joined[0]) { char *w = joined; for (GLint i = 0; i < ext_n; i++) w += sprintf(w, "%s%s", i ? " " : "", ext_list[i]); }
        return (const GLubyte *)joined;
    }
    return h_GetString(name);
}
static SYSV const GLubyte *n_glGetStringi(GLenum name, GLuint i) {
    if (name != GL_EXTENSIONS) return h_GetStringi(name, i);
    ext_build();
    return i < (GLuint)ext_n ? (const GLubyte *)ext_list[i] : 0;
}
static SYSV void n_glGetIntegerv(GLenum name, GLint *v) {
    if (name == GL_MAJOR_VERSION) { *v = 3; return; }
    if (name == GL_MINOR_VERSION) { *v = 0; return; }
    if (name == GL_NUM_EXTENSIONS) { ext_build(); *v = ext_n; return; }
    h_GetIntegerv(name, v);
}
// Game-facing (SysV) replacements, native mode only; 0 = not one of ours.
void *wgl_hook(const char *name) {
    if (!h_GetString) {
        h_GetString = shim_gl_host("glGetString"); h_GetStringi = shim_gl_host("glGetStringi"); h_GetIntegerv = shim_gl_host("glGetIntegerv");
    }
    if (!strcmp(name, "glGetString")) return n_glGetString;
    if (!strcmp(name, "glGetStringi")) return n_glGetStringi;
    if (!strcmp(name, "glGetIntegerv")) return n_glGetIntegerv;
    return 0;
}
