// Bridge for launchers before 0.0.12: they update loader.exe but not themselves. When the launcher did not say it
// updates itself (SKATESTRAP_SELF_UPDATES unset), a low-priority thread reads the manifest's "launcher" entry and,
// if ..\Skatestrap.exe differs, downloads the new one, checks its SHA-256 and swaps it in (the running exe is
// renamed to .old, which Windows allows). The next start of Skatestrap is the new one; it deletes the .old.
// Everything is streamed in 64 KB chunks: the exe is ~135 MB and some players have 4 GB of RAM.
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { BCRYPT_HASH_HANDLE h; } Sha;
static int sha_open(Sha *s) { return BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &s->h, 0, 0, 0, 0, 0) == 0; }
static void sha_add(Sha *s, const void *p, DWORD n) { BCryptHashData(s->h, (PUCHAR)p, n, 0); }
static void sha_hex(Sha *s, char hex[65]) {
    UCHAR d[32] = {0};
    BCryptFinishHash(s->h, d, 32, 0); BCryptDestroyHash(s->h);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
}

// GET url; the body goes to file `to` (hashed into `hex`) or, when to == 0, into a returned string (small files)
static char *get(const char *url, const char *to, char hex[65]) {
    static char chunk[65536];
    HINTERNET in = InternetOpenA("Skatestrap", INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0), u = 0;
    if (in) u = InternetOpenUrlA(in, url, 0, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | (strncmp(url, "https:", 6) ? 0 : INTERNET_FLAG_SECURE), 0);
    HANDLE f = to ? CreateFileA(to, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0) : INVALID_HANDLE_VALUE;
    Sha s; char *buf = 0; DWORD n = 0, got, wr; int ok = u && (to ? f != INVALID_HANDLE_VALUE && sha_open(&s) : 1);
    while (ok) {
        if (!InternetReadFile(u, chunk, sizeof chunk, &got)) { ok = 0; break; }
        if (!got) break;
        if (to) { sha_add(&s, chunk, got); ok = WriteFile(f, chunk, got, &wr, 0) && wr == got; }
        else if (n + got > (1 << 20)) ok = 0;  // a manifest is a few hundred bytes
        else { char *b = realloc(buf, n + got + 1); if ((ok = b != 0)) { buf = b; memcpy(buf + n, chunk, got); n += got; buf[n] = 0; } }
    }
    if (to && u && f != INVALID_HANDLE_VALUE) sha_hex(&s, hex);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (u) InternetCloseHandle(u);
    if (in) InternetCloseHandle(in);
    if (!ok) { free(buf); return 0; }
    return to ? (char *)to : buf;
}

static int file_sha(const char *path, char hex[65]) {
    static char chunk[65536];
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    Sha s; DWORD rd; int ok = f != INVALID_HANDLE_VALUE && sha_open(&s);
    if (!ok) { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); return 0; }
    while ((ok = ReadFile(f, chunk, sizeof chunk, &rd, 0)) && rd) sha_add(&s, chunk, rd);
    sha_hex(&s, hex); CloseHandle(f);
    return ok;
}

// value of "key":"..." at or after p (no escapes in our manifest)
static int field(const char *p, const char *key, char *out, size_t cap) {
    char k[32]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *s = strstr(p, k);
    if (!s || !(s = strchr(s + strlen(k), '"'))) return 0;
    const char *e = strchr(++s, '"');
    if (!e || (size_t)(e - s) >= cap) return 0;
    memcpy(out, s, e - s); out[e - s] = 0;
    return 1;
}

static DWORD WINAPI run(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    char me[MAX_PATH], exe[MAX_PATH + 32], part[MAX_PATH + 32], old[MAX_PATH + 32], url[512], want[80], have[65], mf[128];
    if (!GetModuleFileNameA(0, me, MAX_PATH)) return 0;
    char *sl = strrchr(me, '\\'); if (!sl) return 0; *sl = 0;          // ...\Skatestrap\loader
    if (!(sl = strrchr(me, '\\'))) return 0; *sl = 0;                   // ...\Skatestrap
    snprintf(exe, sizeof exe, "%s\\Skatestrap.exe", me);
    snprintf(part, sizeof part, "%s.part", exe);
    snprintf(old, sizeof old, "%s.old", exe);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return 0;   // not a Skatestrap install
    const char *ov = getenv("SKATESTRAP_MANIFEST");  // testing only
    if (ov) snprintf(mf, sizeof mf, "%s", ov);
    else snprintf(mf, sizeof mf, "https://krashscripts.xyz/update/manifest.json?t=%llu", (unsigned long long)GetTickCount64());
    char *m = get(mf, 0, 0);
    const char *la = m ? strstr(m, "\"launcher\"") : 0;
    int ok = la && field(la, "url", url, sizeof url) && field(la, "sha256", want, sizeof want) && strlen(want) == 64;
    free(m);
    if (!ok || !file_sha(exe, have)) return 0;
    for (char *c = want; *c; c++) *c = (char)tolower(*c);
    if (!strcmp(have, want)) return 0;                                  // already the published launcher
    if (!get(url, part, have)) { DeleteFileA(part); printf("launcher: update download failed\n"); return 0; }
    if (strcmp(have, want)) { DeleteFileA(part); printf("launcher: update failed the checksum\n"); return 0; }
    if (!MoveFileExA(exe, old, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(part); printf("launcher: could not swap (%lu)\n", GetLastError()); return 0; }
    if (!MoveFileExA(part, exe, 0)) { MoveFileExA(old, exe, 0); DeleteFileA(part); printf("launcher: could not swap (%lu)\n", GetLastError()); return 0; }
    printf("launcher: updated Skatestrap.exe (takes effect next start)\n");
    return 0;
}

int selfupd_now(void) { return (int)run(0); }  // loader.exe --launcher-update (testing)

void selfupd_start(void) {
    if (getenv("SKATESTRAP_SELF_UPDATES")) return;  // the launcher handles it
    CloseHandle(CreateThread(0, 0, run, 0, 0, 0));
}
