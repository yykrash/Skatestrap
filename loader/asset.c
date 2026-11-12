// Android's AAssetManager and AConfiguration (NDK) for libroblox.so.
// Assets are the APK's assets/ folder, extracted inside the sandbox at /data/app/com.roblox.client/assets (or env SKATESTRAP_ASSETS).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include "shim.h"

typedef struct { uint8_t *buf; int64_t len, pos; wchar_t path[MAX_PATH]; } Asset;
static int mgr_tag;  // AAssetManager* is this address; there is only one

// Mods (Bloxstrap/Voidstrap "Modifications"): SKATESTRAP_MODS mirrors assets/ (content\textures\..., content\sounds\...).
// A file there replaces the game's file of the same path; nothing else changes. rel: path under assets/ with '/'.
static wchar_t mods_dir[MAX_PATH]; static int mods_state;  // 0 unread, 1 on, 2 off (selftest resets it)
int mods_path(const char *rel, wchar_t *out) {
    wchar_t *dir = mods_dir; int state = mods_state;
    if (!state) mods_state = state =GetEnvironmentVariableW(L"SKATESTRAP_MODS", dir, MAX_PATH) ? 1 : 2;
    if (state != 1 || !rel || !*rel || strstr(rel, "..") || strchr(rel, ':')) return 0;
    wchar_t wn[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, rel, -1, wn, MAX_PATH)) return 0;
    for (wchar_t *c = wn; *c; c++) if (*c == L'/') *c = L'\\';
    if (_snwprintf(out, MAX_PATH, L"%s\\%s", dir, wn) <= 0) return 0;
    DWORD at = GetFileAttributesW(out);
    return at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY);
}

static int asset_path(const char *name, wchar_t *out) {
    if (!name || !*name || name[0] == '/' || strstr(name, "..") || strchr(name, ':')) return 0;  // stay inside the folder
    if (mods_path(name, out)) return 1;
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"SKATESTRAP_ASSETS", dir, MAX_PATH)) {
        GetModuleFileNameW(0, dir, MAX_PATH);
        *wcsrchr(dir, L'\\') = 0;
        wcscat(dir, L"\\root\\data\\app\\com.roblox.client\\assets");
    }
    wchar_t wn[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, MAX_PATH)) return 0;
    for (wchar_t *c = wn; *c; c++) if (*c == L'/') *c = L'\\';
    return _snwprintf(out, MAX_PATH, L"%s\\%s", dir, wn) > 0;
}

static SYSV void *s_mgr_from_java(void *env, void *obj) { return &mgr_tag; }
static SYSV Asset *s_open(void *mgr, const char *name, int mode) {
    Asset *a = calloc(1, sizeof *a);
    if (!asset_path(name, a->path)) { free(a); return 0; }
    FILE *f = _wfopen(a->path, L"rb");
    if (!f) { printf("AAsset_open(%s) -> missing\n", name); free(a); return 0; }
    _fseeki64(f, 0, SEEK_END); a->len = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    a->buf = malloc(a->len ? a->len : 1);
    if (fread(a->buf, 1, a->len, f) != (size_t)a->len) { fclose(f); free(a->buf); free(a); return 0; }
    fclose(f);
    return a;
}
static SYSV void s_close(Asset *a) { if (a) { free(a->buf); free(a); } }
static SYSV const void *s_get_buffer(Asset *a) { return a->buf; }
static SYSV int64_t s_get_length(Asset *a) { return a->len; }
static SYSV int s_read(Asset *a, void *buf, size_t n) {
    int64_t left = a->len - a->pos;
    if ((int64_t)n > left) n = left;
    memcpy(buf, a->buf + a->pos, n); a->pos += n;
    return (int)n;
}
static SYSV int64_t s_seek(Asset *a, int64_t off, int whence) {
    int64_t p = whence == 0 ? off : whence == 1 ? a->pos + off : a->len + off;
    if (p < 0 || p > a->len) return -1;
    return a->pos = p;
}
// Android hands back the APK's fd plus the asset's offset; here the asset is its own file, so offset 0.
static SYSV int s_open_fd(Asset *a, int64_t *start, int64_t *len) {
    int fd = _wopen(a->path, _O_RDONLY | _O_BINARY);
    if (fd < 0) return -1;
    *start = 0; *len = a->len;
    return fd;
}

// AConfiguration: values a desktop screen at density 160 would report (1 dp = 1 px)
typedef struct { int unused; } Config;
static SYSV Config *s_cfg_new(void) { return calloc(1, sizeof(Config)); }
static SYSV void s_cfg_delete(Config *c) { free(c); }
static SYSV void s_cfg_from_mgr(Config *c, void *mgr) {}
static SYSV void s_cfg_country(Config *c, char *out) { out[0] = 'U'; out[1] = 'S'; }
static SYSV void s_cfg_language(Config *c, char *out) { out[0] = 'e'; out[1] = 'n'; }
static SYSV int s_cfg_nav_hidden(Config *c) { return 0; }  // NAVHIDDEN_ANY
static SYSV int s_cfg_width_dp(Config *c) { return GetSystemMetrics(SM_CXSCREEN); }
static SYSV int s_cfg_height_dp(Config *c) { return GetSystemMetrics(SM_CYSCREEN); }
static SYSV int s_cfg_screen_size(Config *c) { return 4; }  // SCREENSIZE_XLARGE

static const struct { const char *name; void *at; } table[] = {
    { "AAssetManager_fromJava", s_mgr_from_java }, { "AAssetManager_open", s_open }, { "AAsset_close", s_close },
    { "AAsset_getBuffer", s_get_buffer }, { "AAsset_getLength", s_get_length }, { "AAsset_read", s_read },
    { "AAsset_seek", s_seek }, { "AAsset_openFileDescriptor", s_open_fd },
    { "AConfiguration_new", s_cfg_new }, { "AConfiguration_delete", s_cfg_delete },
    { "AConfiguration_fromAssetManager", s_cfg_from_mgr }, { "AConfiguration_getCountry", s_cfg_country },
    { "AConfiguration_getLanguage", s_cfg_language }, { "AConfiguration_getNavHidden", s_cfg_nav_hidden },
    { "AConfiguration_getScreenWidthDp", s_cfg_width_dp }, { "AConfiguration_getScreenHeightDp", s_cfg_height_dp },
    { "AConfiguration_getScreenSize", s_cfg_screen_size },
};
void *shim_lookup_asset(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}

int shim_asset_selftest(void) {
    int bad = 0;
    Asset *a = s_open(0, "PublicSuffixDatabase.list", 0);
    if (!a || a->len < 1000) { bad++; printf("asset selftest: open failed\n"); return bad; }
    char b[4]; int64_t start, len;
    if (s_read(a, b, 4) != 4 || s_seek(a, 0, 2) != a->len || s_read(a, b, 4) != 0) { bad++; printf("asset selftest: read/seek\n"); }
    int fd = s_open_fd(a, &start, &len);
    if (fd < 0 || len != a->len) { bad++; printf("asset selftest: fd\n"); } else _close(fd);
    s_close(a);
    if (s_open(0, "../loader/loader.c", 0)) { bad++; printf("asset selftest: escaped folder\n"); }
    {   // mods: a file in SKATESTRAP_MODS wins; ".." and missing files fall through (mods_path reads the env once)
        wchar_t d[MAX_PATH], f[MAX_PATH], o[MAX_PATH]; GetTempPathW(MAX_PATH, d); wcscat(d, L"skatestrap_mods_test");
        CreateDirectoryW(d, 0); _snwprintf(f, MAX_PATH, L"%s\\m.txt", d);
        FILE *h = _wfopen(f, L"wb"); if (h) { fputs("x", h); fclose(h); }
        SetEnvironmentVariableW(L"SKATESTRAP_MODS", d); mods_state = 0;
        if (!mods_path("m.txt", o) || mods_path("../m.txt", o) || mods_path("none.txt", o)) { bad++; printf("asset selftest: mods\n"); }
        SetEnvironmentVariableW(L"SKATESTRAP_MODS", 0); mods_state = 0; DeleteFileW(f); RemoveDirectoryW(d);
    }
    return bad;
}
