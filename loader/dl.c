// libdl for libroblox.so. The only ELF image is libroblox itself; "system libraries" it dlopens are our shim,
// so dlsym on them goes through shim_lookup. Libraries we don't provide fail honestly with a dlerror message.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

extern uint8_t *shim_image; extern uint64_t shim_image_span;

typedef struct { uint64_t addr; const char *name; const void *phdr; uint16_t phnum; uint64_t adds, subs, tls_modid; void *tls_data; } PhdrInfo;
typedef SYSV int (*PhdrCb)(PhdrInfo *, uint64_t, void *);
static SYSV int s_dl_iterate_phdr(PhdrCb cb, void *data) {
    PhdrInfo i = { (uint64_t)shim_image, "/data/app/com.roblox.client/lib/x86_64/libroblox.so",
                   shim_image + *(uint64_t *)(shim_image + 0x20), *(uint16_t *)(shim_image + 0x38), 1, 0, 0, 0 };
    return cb(&i, sizeof i, data);  // libroblox's first PT_LOAD starts at file offset 0, so its headers are mapped
}

typedef struct { const char *fname; void *fbase; const char *sname; void *saddr; } DlInfo;
static SYSV int s_dladdr(const void *a, DlInfo *info) {
    if ((uint8_t *)a < shim_image || (uint8_t *)a >= shim_image + shim_image_span) return 0;
    info->fname = "/data/app/com.roblox.client/lib/x86_64/libroblox.so"; info->fbase = shim_image;
    info->sname = 0; info->saddr = 0;  // shortcut: no symbol names; add a dynsym search if crash reports need them
    return 1;
}

static __thread const char *last_error;
static const char *const provided[] = { "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
                                        "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libmediandk.so" };
#define SELF ((void *)1)
static SYSV void *s_dlopen(const char *name, int flags) {
    if (!name) return SELF;
    const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    for (size_t i = 0; i < sizeof provided / sizeof *provided; i++)
        if (!strcmp(base, provided[i])) return (void *)provided[i];
    if (getenv("SKATESTRAP_VULKAN") && (!strcmp(base, "libvulkan.so") || !strcmp(base, "libvulkan.so.1"))) return "libvulkan.so";  // vk_hand.c
    printf("dlopen(%s): not provided\n", name);
    static __thread char msg[300];
    snprintf(msg, sizeof msg, "dlopen failed: library \"%s\" not found", name);
    last_error = msg;
    return 0;
}
static SYSV void *s_dlsym(void *h, const char *name) {
    void *r = shim_lookup(name);
    if (!r) {
        printf("dlsym(%s, %s): missing\n", h == SELF ? "self" : h ? (const char *)h : "?", name);
        static __thread char msg[300];
        snprintf(msg, sizeof msg, "undefined symbol: %s", name);
        last_error = msg;
    }
    return r;
}
static SYSV int s_dlclose(void *h) { return 0; }
static SYSV const char *s_dlerror(void) { const char *e = last_error; last_error = 0; return e; }

static const struct { const char *name; void *at; } table[] = {
    { "dl_iterate_phdr", s_dl_iterate_phdr }, { "dladdr", s_dladdr }, { "dlopen", s_dlopen },
    { "dlsym", s_dlsym }, { "dlclose", s_dlclose }, { "dlerror", s_dlerror },
};
void *shim_lookup_dl(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}
