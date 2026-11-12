// The plain parts of bionic for libroblox.so: libm, strings, wide chars (32-bit wchar_t, UTF-8 only),
// locale stubs (one C.UTF-8 locale), memory mapping, clocks and identity. SysV calling convention throughout.
// Identity answers are honest: this is Skatestrap on Windows, not a real phone.
#include <windows.h>
#include <io.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <wctype.h>
#include "shim.h"

int *shim_errno(void);
#define SETERR(e) (*shim_errno() = (e))

// --- libm (SysV and Windows differ once ints or pointers mix with doubles, so every one gets a wrapper) ------
#define D1(n) static SYSV double m_##n(double x) { return n(x); }
#define D2(n) static SYSV double m_##n(double x, double y) { return n(x, y); }
#define F1(n) static SYSV float m_##n(float x) { return n(x); }
#define F2(n) static SYSV float m_##n(float x, float y) { return n(x, y); }
#define D1S(X) X(acos) X(asin) X(atan) X(cbrt) X(cos) X(cosh) X(exp) X(exp2) X(expm1) X(log) X(log10) X(log2) X(round) X(sin) X(sinh) X(tan) X(tanh)
#define D2S(X) X(atan2) X(fmod) X(hypot) X(pow)
#define F1S(X) X(acosf) X(asinf) X(atanf) X(cbrtf) X(ceilf) X(cosf) X(coshf) X(erfcf) X(erff) X(exp2f) X(expf) X(floorf) X(log10f) X(log2f) X(logf) X(sinf) X(sinhf) X(tanf) X(tanhf)
#define F2S(X) X(atan2f) X(fmodf) X(hypotf) X(powf) X(nextafterf) X(remainderf)
D1S(D1) D2S(D2) F1S(F1) F2S(F2)
static SYSV double m_frexp(double x, int *e) { return frexp(x, e); }
static SYSV float m_frexpf(float x, int *e) { return frexpf(x, e); }
static SYSV double m_ldexp(double x, int e) { return ldexp(x, e); }
static SYSV float m_ldexpf(float x, int e) { return ldexpf(x, e); }
static SYSV double m_scalbn(double x, int e) { return scalbn(x, e); }
static SYSV int m_ilogb(double x) { return ilogb(x); }
static SYSV int64_t m_llround(double x) { return llround(x); }
static SYSV int64_t m_llroundf(float x) { return llroundf(x); }
static SYSV double m_modf(double x, double *i) { return modf(x, i); }
static SYSV float m_modff(float x, float *i) { return modff(x, i); }
static SYSV double m_nan(const char *s) { return nan(s); }
static SYSV int m_finitef(float x) { return isfinite(x); }
static SYSV void m_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static SYSV void m_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static SYSV float m_remquof(float x, float y, int *q) { return remquof(x, y, q); }

// --- strings ---------------------------------------------------------------------------------------------
static SYSV char *s_strcat(char *d, const char *s) { return strcat(d, s); }
static SYSV char *s_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static SYSV size_t s_strcspn(const char *a, const char *b) { return strcspn(a, b); }
static SYSV size_t s_strspn(const char *a, const char *b) { return strspn(a, b); }
static SYSV char *s_strpbrk(const char *a, const char *b) { return (char *)strpbrk(a, b); }
static SYSV size_t s_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static SYSV int s_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static SYSV int s_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
static SYSV void *s_memrchr(const void *p, int c, size_t n) {
    const uint8_t *s = p;
    while (n--) if (s[n] == (uint8_t)c) return (void *)(s + n);
    return 0;
}
static SYSV int s_tolower(int c) { return tolower(c); }
static SYSV int s_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static SYSV int s_atoi(const char *s) { return atoi(s); }
static SYSV int64_t s_atoll(const char *s) { return _atoi64(s); }
static SYSV double s_atof(const char *s) { return atof(s); }
static SYSV double s_strtod(const char *s, char **e) { return strtod(s, e); }
static SYSV float s_strtof(const char *s, char **e) { return strtof(s, e); }
static SYSV int64_t s_strtoll(const char *s, char **e, int b) { return _strtoi64(s, e, b); }
static SYSV uint64_t s_strtoull(const char *s, char **e, int b) { return _strtoui64(s, e, b); }
static SYSV int64_t s_strtoll_l(const char *s, char **e, int b, void *l) { return _strtoi64(s, e, b); }
static SYSV uint64_t s_strtoull_l(const char *s, char **e, int b, void *l) { return _strtoui64(s, e, b); }
static SYSV int s_strcoll_l(const char *a, const char *b, void *l) { return strcmp(a, b); }
static SYSV size_t s_strxfrm_l(char *d, const char *s, size_t n, void *l) {
    size_t len = strlen(s);
    if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k); d[k] = 0; }
    return len;
}

// _FORTIFY_SOURCE checks: bionic aborts on overflow; so do we
static void chk(int ok, const char *what) { if (!ok) { printf("\nfortify: %s overflow\n", what); fflush(stdout); ExitProcess(7); } }
static SYSV void *c_memcpy(void *d, const void *s, size_t n, size_t dl) { chk(n <= dl, "memcpy"); return memcpy(d, s, n); }
static SYSV void *c_memmove(void *d, const void *s, size_t n, size_t dl) { chk(n <= dl, "memmove"); return memmove(d, s, n); }
static SYSV void *c_memset(void *d, int c, size_t n, size_t dl) { chk(n <= dl, "memset"); return memset(d, c, n); }
static SYSV size_t c_strlen(const char *s, size_t sl) { size_t n = strlen(s); chk(n < sl, "strlen"); return n; }
static SYSV char *c_strchr(const char *s, int c, size_t sl) { return (char *)strchr(s, c); }
static SYSV char *c_strcpy(char *d, const char *s, size_t dl) { chk(strlen(s) < dl, "strcpy"); return strcpy(d, s); }
static SYSV char *c_strcat(char *d, const char *s, size_t dl) { chk(strlen(d) + strlen(s) < dl, "strcat"); return strcat(d, s); }
static SYSV char *c_strncpy(char *d, const char *s, size_t n, size_t dl) { chk(n <= dl, "strncpy"); return strncpy(d, s, n); }
static SYSV char *c_strncpy2(char *d, const char *s, size_t n, size_t dl, size_t sl) { chk(n <= dl, "strncpy"); return strncpy(d, s, n); }

// SysV comparators called from the Windows qsort/bsearch through a per-thread bridge
typedef SYSV int (*Cmp)(const void *, const void *);
static __declspec(thread) Cmp cur_cmp;
static int cmp_bridge(const void *a, const void *b) { return cur_cmp(a, b); }
static SYSV void s_qsort(void *b, size_t n, size_t sz, Cmp c) { Cmp old = cur_cmp; cur_cmp = c; qsort(b, n, sz, cmp_bridge); cur_cmp = old; }
static SYSV void *s_bsearch(const void *k, const void *b, size_t n, size_t sz, Cmp c) {
    Cmp old = cur_cmp; cur_cmp = c; void *r = bsearch(k, b, n, sz, cmp_bridge); cur_cmp = old; return r;
}

// --- wide chars: Linux wchar_t is 32-bit; multibyte is UTF-8 -------------------------------------------
// shortcut: mbstate_t is ignored, so a character split across two calls fails; fine until a stream decoder needs it
typedef uint32_t wch;
#define WEOF_ 0xffffffffu
static size_t utf8_dec(wch *out, const uint8_t *s, size_t n) {  // bytes used, 0 for NUL, -1 bad, -2 short
    if (!n) return (size_t)-2;
    uint32_t c = s[0], len = c < 0x80 ? 1 : c < 0xc2 ? 0 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : c < 0xf5 ? 4 : 0;
    if (!len) return (size_t)-1;
    c &= len == 1 ? 0x7f : len == 2 ? 0x1f : len == 3 ? 0x0f : 0x07;
    for (uint32_t i = 1; i < len; i++) {
        if (i >= n) return (size_t)-2;
        if ((s[i] & 0xc0) != 0x80) return (size_t)-1;
        c = c << 6 | (s[i] & 0x3f);
    }
    if (out) *out = c;
    return c ? len : 0;
}
static size_t utf8_enc(char *d, wch c) {
    if (c < 0x80) { d[0] = (char)c; return 1; }
    if (c < 0x800) { d[0] = (char)(0xc0 | c >> 6); d[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c >= 0xd800 && c < 0xe000) return (size_t)-1;
    if (c < 0x10000) { d[0] = (char)(0xe0 | c >> 12); d[1] = (char)(0x80 | (c >> 6 & 0x3f)); d[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    if (c < 0x110000) { d[0] = (char)(0xf0 | c >> 18); d[1] = (char)(0x80 | (c >> 12 & 0x3f)); d[2] = (char)(0x80 | (c >> 6 & 0x3f)); d[3] = (char)(0x80 | (c & 0x3f)); return 4; }
    return (size_t)-1;
}
static SYSV size_t s_mbrtowc(wch *pwc, const char *s, size_t n, void *ps) {
    if (!s) return 0;
    size_t r = utf8_dec(pwc, (const uint8_t *)s, n);
    if (r == (size_t)-1) SETERR(84);  // EILSEQ
    return r;
}
static SYSV int s_mbtowc(wch *pwc, const char *s, size_t n) { if (!s) return 0; size_t r = s_mbrtowc(pwc, s, n, 0); return r > 4 ? -1 : (int)r; }
static SYSV size_t s_mbrlen(const char *s, size_t n, void *ps) { return s_mbrtowc(0, s, n, ps); }
static SYSV size_t s_wcrtomb(char *s, wch c, void *ps) {
    char tmp[4];
    if (!s) return 1;
    size_t r = utf8_enc(s, c);
    if (r == (size_t)-1) SETERR(84);
    (void)tmp;
    return r;
}
static SYSV wch s_btowc(int c) { return c >= 0 && c < 0x80 ? (wch)c : WEOF_; }
static SYSV int s_wctob(wch c) { return c < 0x80 ? (int)c : -1; }
static SYSV size_t s_mbsnrtowcs(wch *d, const char **src, size_t nms, size_t len, void *ps) {
    const uint8_t *s = (const uint8_t *)*src, *end = s + nms;
    size_t n = 0;
    while (!d || n < len) {
        wch c;
        size_t r = utf8_dec(&c, s, (size_t)(end - s));
        if (r == (size_t)-2) break;  // input ends mid-character: stop before it
        if (r == (size_t)-1) { if (d) *src = (const char *)s; SETERR(84); return (size_t)-1; }
        if (d) d[n] = c;
        if (r == 0) { if (d) *src = 0; return n; }
        s += r; n++;
    }
    if (d) *src = (const char *)s;
    return n;
}
static SYSV size_t s_mbsrtowcs(wch *d, const char **src, size_t len, void *ps) { return s_mbsnrtowcs(d, src, (size_t)-1 / 2, len, ps); }
static SYSV size_t s_wcsnrtombs(char *d, const wch **src, size_t nwc, size_t len, void *ps) {
    const wch *s = *src;
    size_t n = 0;
    for (size_t i = 0; i < nwc; i++) {
        char tmp[4];
        if (s[i] == 0) { if (d) { if (n >= len) { *src = s + i; return n; } d[n] = 0; *src = 0; } return n; }
        size_t r = utf8_enc(tmp, s[i]);
        if (r == (size_t)-1) { if (d) *src = s + i; SETERR(84); return (size_t)-1; }
        if (d) { if (n + r > len) { *src = s + i; return n; } memcpy(d + n, tmp, r); }
        n += r;
    }
    if (d) *src = s + nwc;
    return n;
}
static SYSV size_t s_wcslen(const wch *s) { size_t n = 0; while (s[n]) n++; return n; }
static SYSV wch *s_wmemchr(const wch *s, wch c, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] == c) return (wch *)(s + i); return 0; }
static SYSV int s_wmemcmp(const wch *a, const wch *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static SYSV int s_wcscoll_l(const wch *a, const wch *b, void *l) { while (*a && *a == *b) a++, b++; return *a < *b ? -1 : *a > *b; }
static SYSV size_t s_wcsxfrm_l(wch *d, const wch *s, size_t n, void *l) {
    size_t len = s_wcslen(s);
    if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k * 4); d[k] = 0; }
    return len;
}
// classification: Windows wctype covers the BMP; above it only letters/printables are guessed true
#define WCLS(name, fn, hi) static SYSV int s_##name##_l(wch c, void *l) { return c < 0x10000 ? fn((wint_t)c) != 0 : hi; }
WCLS(iswalpha, iswalpha, 1) WCLS(iswblank, iswblank, 0) WCLS(iswcntrl, iswcntrl, 0) WCLS(iswdigit, iswdigit, 0)
WCLS(iswlower, iswlower, 0) WCLS(iswprint, iswprint, 1) WCLS(iswpunct, iswpunct, 0) WCLS(iswspace, iswspace, 0)
WCLS(iswupper, iswupper, 0) WCLS(iswxdigit, iswxdigit, 0)
static SYSV wch s_towlower_l(wch c, void *l) { return c < 0x10000 ? towlower((wint_t)c) : c; }
static SYSV wch s_towupper_l(wch c, void *l) { return c < 0x10000 ? towupper((wint_t)c) : c; }

// --- locale: one C.UTF-8 locale for everyone ---------------------------------------------------------
static char the_locale[16];
static SYSV void *s_newlocale(int mask, const char *name, void *base) { return the_locale; }
static SYSV void s_freelocale(void *l) {}
static SYSV void *s_uselocale(void *l) { return (void *)-1; }  // LC_GLOBAL_LOCALE
static SYSV size_t s_mb_cur_max(void) { return 4; }
static struct { char *s[10]; char c[14]; } lconv_c = {
    { ".", "", "", "", "", "", "", "", "", "" }, { 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127 } };
static SYSV void *s_localeconv(void) { return &lconv_c; }

// --- memory mapping (anonymous only for now) -----------------------------------------------------------
static DWORD page_prot(int p) {
    switch (p & 7) {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 4: return PAGE_EXECUTE;
    case 5: return PAGE_EXECUTE_READ;
    case 6: case 7: return PAGE_EXECUTE_READWRITE;
    default: return PAGE_READWRITE;
    }
}
static SYSV void *s_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    if (!(flags & 0x20)) {
        if ((flags & 1) && (prot & 2)) {  // MAP_SHARED + PROT_WRITE: a real Windows file view, so writes reach the file
            // shortcut: no MAP_FIXED, offset must be a 64K multiple, and a file shorter than off+len grows to it (Linux leaves it)
            HANDLE fh = (HANDLE)_get_osfhandle(fd), mh = 0; void *v = 0;
            uint64_t end = (uint64_t)off + len;
            if (fh != INVALID_HANDLE_VALUE && !(flags & 0x10) && !(off & 0xffff)) mh = CreateFileMappingW(fh, 0, PAGE_READWRITE, (DWORD)(end >> 32), (DWORD)end, 0);
            if (mh) { v = MapViewOfFile(mh, FILE_MAP_READ | FILE_MAP_WRITE, (DWORD)((uint64_t)off >> 32), (DWORD)off, len); CloseHandle(mh); }
            if (!v) { printf("mmap: shared writable map of fd %d failed (off %lld len %zu, error %lu)\n", fd, (long long)off, len, GetLastError()); SETERR(19); return (void *)-1; }
            return v;
        }
        // other file mappings = private copy of the bytes
        uint8_t *p = VirtualAlloc((flags & 0x10) ? addr : 0, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p && (flags & 0x10)) p = VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE);
        if (!p) { SETERR(12); return (void *)-1; }
        int64_t was = _lseeki64(fd, 0, SEEK_CUR);
        if (was < 0 || _lseeki64(fd, off, SEEK_SET) < 0) { VirtualFree(p, 0, MEM_RELEASE); SETERR(19); return (void *)-1; }
        size_t got = 0; int r;
        while (got < len && (r = _read(fd, p + got, (unsigned)(len - got > (1u << 30) ? 1u << 30 : len - got))) > 0) got += r;
        _lseeki64(fd, was, SEEK_SET);
        if (got < len) memset(p + got, 0, len - got);  // past end of file reads as zero (Linux would SIGBUS past the last page)
        DWORD old; VirtualProtect(p, len, page_prot(prot), &old);
        return p;
    }
    // PROT_NONE and not fixed = address space only (a 16 GB commit took 33-198 ms and its commit charge); s_mprotect commits on first use
    void *p = VirtualAlloc((flags & 0x10) ? addr : 0, len, (prot & 7) || (flags & 0x10) ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE, page_prot(prot));
    if (!p && (flags & 0x10)) p = VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot));  // MAP_FIXED over our own block
    if (!p) { SETERR(12); return (void *)-1; }
    if (flags & 0x10) memset(p, 0, len);
    return p;
}
static SYSV int s_munmap(void *addr, size_t len) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(addr, &m, sizeof m)) { SETERR(22); return -1; }
    // release the whole block only when the unmap covers all of it; otherwise decommit the pages
    if (m.Type == MEM_MAPPED) { UnmapViewOfFile(m.AllocationBase); return 0; }  // shared file view (whole view goes; partial unmaps not emulated)
    uint8_t *base = m.AllocationBase, *p = base;
    while (VirtualQuery(p, &m, sizeof m) && m.AllocationBase == base) p += m.RegionSize;
    if ((uint8_t *)addr == base && (uint8_t *)addr + len >= p) VirtualFree(base, 0, MEM_RELEASE);
    else VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}
static SYSV int s_msync(void *addr, size_t len, int flags) { FlushViewOfFile(addr, len); return 0; }  // no-op on private copies
static __declspec(thread) void *none_addr; static __declspec(thread) size_t none_len;  // this thread's last mprotect(PROT_NONE): see s_madvise
// memory turned executable after start-up = native Luau code (printed at exit: is the game's code really compiled on this PC?)
volatile long long skate_exec_bytes, skate_exec_calls, skate_exec_fails;
static SYSV int s_mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    // fails on pages s_mmap only reserved: commit them (new pages are zero, committed ones keep their bytes)
    if (!VirtualProtect(addr, len, page_prot(prot), &old) && !VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot))) {
        if (prot & 4) { InterlockedIncrement64(&skate_exec_fails); printf("mprotect exec FAILED: %zu bytes, error %lu\n", len, GetLastError()); }
        SETERR(12); return -1; }
    if (prot & 4) { InterlockedAdd64(&skate_exec_bytes, (LONG64)len); InterlockedIncrement64(&skate_exec_calls); }
    if (!(prot & 7)) { none_addr = addr; none_len = len; } else none_addr = 0;
    return 0;
}
// loader.exe --selftest: PROT_NONE map is reserve-only, mprotect commits zero pages and keeps bytes, trim + unmap work
int shim_mem_selftest(void) {
    int bad = 0; size_t n = 1 << 20; MEMORY_BASIC_INFORMATION m;
    uint8_t *p = s_mmap(0, 4 * n, 0, 0x22, -1, 0);
    if (p == (void *)-1) { printf("mem selftest: map\n"); return 1; }
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_RESERVE) { bad++; printf("mem selftest: not reserve-only\n"); }
    if (s_mprotect(p + n, n, 3) || p[n] || p[2 * n - 1]) { bad++; printf("mem selftest: commit\n"); } else p[n] = 7;
    if (s_mprotect(p, 3 * n, 3) || p[n] != 7 || p[0]) { bad++; printf("mem selftest: mixed range\n"); }
    if (s_munmap(p + 3 * n, n) || s_mprotect(p, n, 0) || s_munmap(p, 4 * n)) { bad++; printf("mem selftest: trim/unmap\n"); }
    if (VirtualQuery(p, &m, sizeof m) && m.State != MEM_FREE) { bad++; printf("mem selftest: not released\n"); }
    return bad;
}
// SKATESTRAP_FPS: a memory call that took over 2 ms is printed (wall time, and CPU cycles: few cycles = it was blocked, not working)
static int mem_fps(void) { static int on = -1; if (on < 0) on = getenv("SKATESTRAP_FPS") != 0; return on; }
static void mem_slow(const char *what, int64_t t0, ULONG64 c0, size_t len) {
    int64_t now_ns(int); int64_t d = now_ns(1) - t0; if (d < 2000000) return;
    ULONG64 c1 = 0; QueryThreadCycleTime(GetCurrentThread(), &c1);
    printf("slow %s: %.1f ms, %.1f M cycles, %zu KB, t%lu\n", what, d / 1e6, (c1 - c0) / 1e6, len >> 10, GetCurrentThreadId());
}
#define MEM_TIMED(what, len, call) do { if (!mem_fps()) return call; int64_t now_ns(int); int64_t t0 = now_ns(1); ULONG64 c0 = 0; \
    QueryThreadCycleTime(GetCurrentThread(), &c0); __typeof__(call) r = call; mem_slow(what, t0, c0, len); return r; } while (0)
static SYSV void *t_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) { MEM_TIMED("mmap", len, s_mmap(addr, len, prot, flags, fd, off)); }
static SYSV int t_munmap(void *addr, size_t len) { MEM_TIMED("munmap", len, s_munmap(addr, len)); }
static SYSV int t_mprotect(void *addr, size_t len, int prot) { MEM_TIMED("mprotect", len, s_mprotect(addr, len, prot)); }
volatile LONG64 skate_madv_n, skate_madv_kb, skate_madv_us, skate_madv_worst;  // SKATESTRAP_FPS report: DONTNEED calls, KB, total and worst microseconds
static SYSV int s_madvise(void *addr, size_t len, int advice) {
    if (advice == 4) {  // MADV_DONTNEED: anonymous pages read back as zero
        // (~80 calls/s, ~15 MB/s while riding: zeroing in place instead was tried 2026-11-11, hitches unchanged)
        // Decommit + commit took ~5 ms per call in the game (up to 74 ms, mostly blocked; ~800 ms of every 5 s) and was the
        // cause of the 50-340 ms hitches. MEM_RESET takes ~10 us: pages stay mapped, Windows drops them when short of memory.
        // A/B 2026-11-11: 222.6 fps / worst gap 344 ms -> 238.0 fps / worst gap 16 ms.
        // shortcut: MEM_RESET leaves old bytes, not zeros. Fine for mimalloc (the 4 call sites at libroblox+1f32adb..1f32cf9);
        // a range this thread just made PROT_NONE (libroblox+42fb54a then +42fb561) still gets the exact decommit. Upgrade if a new caller needs zeros.
        // SKATESTRAP_MADV=decommit: always the old exact way; =noop: do nothing.
        static int mode = -1; if (mode < 0) { const char *e = getenv("SKATESTRAP_MADV"); mode = !e ? 1 : e[0] == 'd' ? 0 : e[0] == 'n' ? 2 : 1; }
        int on = mem_fps(); int64_t now_ns(int); int64_t t0 = on ? now_ns(1) : 0; ULONG64 c0 = 0; if (on) QueryThreadCycleTime(GetCurrentThread(), &c0);
        if (mode != 2) {
            // no VirtualQuery here: it is as slow as the decommit in the game (run madvD: 234 fps, gaps to 75 ms with it)
            int none = none_addr == addr && none_len == len;  // the caller just made this exact range PROT_NONE
            if (mode == 1 && !none) VirtualAlloc(addr, len, MEM_RESET, PAGE_READWRITE);
            else {
                MEMORY_BASIC_INFORMATION m = { 0 }; if (!none) VirtualQuery(addr, &m, sizeof m);
                VirtualFree(addr, len, MEM_DECOMMIT);
                VirtualAlloc(addr, len, MEM_COMMIT, none ? PAGE_NOACCESS : m.Protect ? m.Protect : PAGE_READWRITE);
            }
        }
        if (on) {
            LONG64 us = (now_ns(1) - t0) / 1000;
            InterlockedIncrement64(&skate_madv_n); InterlockedAdd64(&skate_madv_kb, (LONG64)(len >> 10)); InterlockedAdd64(&skate_madv_us, us);
            if (us > skate_madv_worst) skate_madv_worst = us;
            mem_slow("madvise", t0, c0, len);
        }
    }
    return 0;
}
static SYSV int s_mlock(const void *a, size_t n) { return 0; }

// --- time ----------------------------------------------------------------------------------------------
typedef struct { int64_t sec, usec; } Timeval;
static int64_t unix_100ns(void) {
    FILETIME f; GetSystemTimePreciseAsFileTime(&f);
    return (int64_t)((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ull);
}
static SYSV int64_t s_time(int64_t *t) { int64_t s = unix_100ns() / 10000000; if (t) *t = s; return s; }
static SYSV int s_gettimeofday(Timeval *tv, void *tz) { int64_t n = unix_100ns(); if (tv) { tv->sec = n / 10000000; tv->usec = n % 10000000 / 10; } return 0; }
static SYSV int s_nanosleep(const int64_t *req, int64_t *rem) {
    int64_t ns = req[0] * 1000000000 + req[1];
    int64_t now_ns(int), t0 = now_ns(1); void wait_trace(const char *, int64_t, int64_t, uint64_t *);
    void hr_sleep(int64_t), lw_note(char, int64_t, int64_t, DWORD, int64_t); hr_sleep(ns); lw_note('s', t0, ns, 0, 0);  // high-res waitable timer: Linux-like sub-ms precision
    wait_trace("nanosleep", (ns + 999999) / 1000000, t0, __builtin_frame_address(0));
    if (rem) rem[0] = rem[1] = 0;
    return 0;
}
static SYSV int64_t s_clock(void) { return (int64_t)clock() * (1000000 / CLOCKS_PER_SEC); }
static SYSV double s_difftime(int64_t a, int64_t b) { return (double)(a - b); }

// --- identity / system -----------------------------------------------------------------------------------
static SYSV int s_sysinfo(uint64_t *si) {  // Linux struct sysinfo, 112 bytes
    MEMORYSTATUSEX m = { sizeof m }; GlobalMemoryStatusEx(&m);
    memset(si, 0, 112);
    si[0] = GetTickCount64() / 1000;                     // uptime
    si[4] = m.ullTotalPhys; si[5] = m.ullAvailPhys;      // totalram, freeram
    si[8] = m.ullTotalPageFile - m.ullTotalPhys; si[9] = m.ullAvailPageFile > m.ullAvailPhys ? m.ullAvailPageFile - m.ullAvailPhys : 0;
    ((uint16_t *)si)[40] = 1;                            // procs
    ((uint32_t *)si)[26] = 1;                            // mem_unit
    return 0;
}
// shortcut: no file system yet; every open logs its path and fails with ENOENT
static SYSV int s_getpagesize(void) { return 4096; }
static SYSV int64_t s_sysconf(int name) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    switch (name) {
    case 0x06: return 100;                            // _SC_CLK_TCK
    case 0x27: case 0x28: return 4096;                // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x60: case 0x61: {  // _SC_NPROCESSORS_CONF / _ONLN (reporting 6 to get fewer workers: 206 vs 229 fps, bigger gaps, 2026-11-11)
        DWORD_PTR pm, sm;  // the CPUs this process may run on (start /affinity), so a limited run looks like a smaller PC
        return GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm ? __builtin_popcountll(pm) : (int64_t)si.dwNumberOfProcessors; }
    case 0x62: case 0x63: { MEMORYSTATUSEX m = { sizeof m }; GlobalMemoryStatusEx(&m);
        return (int64_t)((name == 0x62 ? m.ullTotalPhys : m.ullAvailPhys) / 4096); }
    }
    printf("sysconf(0x%x) not emulated\n", name);
    SETERR(22); return -1;
}
static SYSV uint64_t s_getauxval(uint64_t type) { return type == 6 ? 4096 : 0; }  // AT_PAGESZ; no hwcaps claimed
static SYSV int s_getuid(void) { return 10000; }  // first Android app uid
static SYSV int s_getppid(void) { return 1; }
static SYSV int s_gettid(void) { return (int)GetCurrentThreadId(); }
static SYSV int s_getpid(void) { return (int)GetCurrentProcessId(); }
static SYSV int s_sched_getcpu(void) { return (int)GetCurrentProcessorNumber(); }
static SYSV int s_sched_yield(void) { SwitchToThread(); return 0; }
static SYSV int s_rand(void) { return rand(); }
static SYSV void s_srand(unsigned s) { srand(s); }
static SYSV void s_random_buf(void *b, size_t n) { BCryptGenRandom(0, b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }
static SYSV int s_getentropy(void *b, size_t n) { if (n > 256) { SETERR(5); return -1; } s_random_buf(b, n); return 0; }
static SYSV int s_property_get(const char *name, char *value) {
    static const char *props[][2] = {
        { "ro.build.version.sdk", "35" }, { "ro.build.version.release", "15" },
        { "ro.product.cpu.abi", "x86_64" }, { "ro.product.cpu.abilist", "x86_64" }, { "ro.product.cpu.abilist64", "x86_64" },
        { "ro.product.manufacturer", "Skatestrap" }, { "ro.product.model", "Skatestrap Windows" },
        { "ro.product.brand", "Skatestrap" }, { "ro.product.device", "skatestrap" },
    };
    const char *r = "";
    for (size_t i = 0; name && i < sizeof props / sizeof props[0]; i++)
        if (!strcmp(name, props[i][0])) r = props[i][1];
    strncpy(value, r, 91); value[91] = 0;
    return (int)strlen(value);
}
static SYSV uint64_t s_pthread_self(void) { return GetCurrentThreadId(); }  // shortcut: thread id, not a bionic pthread_internal_t
static SYSV int s_pthread_equal(uint64_t a, uint64_t b) { return a == b; }
// thread names (pthread_setname_np, PR_SET_NAME) so the frame-gap and stall traces can say which Roblox job a tid is.
// shortcut: hashed table, a dead thread's slot is overwritten when full; names can tear while being set (traces only)
static struct { volatile DWORD tid; char n[16]; } tnames[512];
uint8_t *skate_lib_base; uint64_t skate_lib_span;  // libroblox mapping (loader.c), for start-function names in threads.c
void tname_set(DWORD tid, const char *n) {
    if (!n) return;
    unsigned h = (tid >> 2) & 511, i = h;
    for (int p = 0; p < 16; p++, i = (i + 1) & 511) {
        DWORD o = tnames[i].tid;
        if (o == tid || (!o && InterlockedCompareExchange((volatile LONG *)&tnames[i].tid, (LONG)tid, 0) == 0)) break;
        if (p == 15) { i = h; tnames[i].tid = tid; }
    }
    strncpy(tnames[i].n, n, 15); tnames[i].n[15] = 0;
}
const char *skate_tname(DWORD tid) {
    for (unsigned p = 0, i = (tid >> 2) & 511; p < 16; p++, i = (i + 1) & 511) if (tnames[i].tid == tid) return tnames[i].n;
    return "?";
}
static SYSV int s_pthread_setname_np(uint64_t t, const char *n) { tname_set((DWORD)t, n); return 0; }
int shim_tname_selftest(void) {  // set, rename, long name cut to 15, colliding tids, unknown tid
    tname_set(4, "a"); tname_set(4, "TaskSchedulerWorker"); tname_set(4 + 512 * 4, "b");
    int bad = strcmp(skate_tname(4), "TaskSchedulerWo") != 0 || strcmp(skate_tname(4 + 512 * 4), "b") != 0 || strcmp(skate_tname(8), "?") != 0;
    if (bad) printf("tname selftest: BAD\n");
    tnames[4 >> 2 & 511].tid = 0; tnames[(4 >> 2) + 1 & 511].tid = 0;  // free the test slots
    return bad;
}

// Linux errno texts (bionic's wording); numbers are Linux's, not the Windows CRT's.
static const char *const errtext[] = {
    [0] = "Success", [1] = "Operation not permitted", [2] = "No such file or directory", [3] = "No such process",
    [4] = "Interrupted system call", [5] = "I/O error", [6] = "No such device or address", [7] = "Argument list too long",
    [8] = "Exec format error", [9] = "Bad file descriptor", [10] = "No child processes", [11] = "Try again",
    [12] = "Out of memory", [13] = "Permission denied", [14] = "Bad address", [16] = "Device or resource busy",
    [17] = "File exists", [18] = "Cross-device link", [19] = "No such device", [20] = "Not a directory",
    [21] = "Is a directory", [22] = "Invalid argument", [23] = "File table overflow", [24] = "Too many open files",
    [25] = "Not a typewriter", [27] = "File too large", [28] = "No space left on device", [29] = "Illegal seek",
    [30] = "Read-only file system", [32] = "Broken pipe", [33] = "Math argument out of domain of func",
    [34] = "Math result not representable", [35] = "Resource deadlock would occur", [36] = "File name too long",
    [38] = "Function not implemented", [39] = "Directory not empty", [61] = "No data available", [62] = "Timer expired",
    [75] = "Value too large for defined data type", [84] = "Illegal byte sequence", [88] = "Socket operation on non-socket",
    [95] = "Operation not supported on transport endpoint", [97] = "Address family not supported by protocol",
    [98] = "Address already in use", [99] = "Cannot assign requested address", [100] = "Network is down",
    [101] = "Network is unreachable", [103] = "Software caused connection abort", [104] = "Connection reset by peer",
    [105] = "No buffer space available", [106] = "Transport endpoint is already connected",
    [107] = "Transport endpoint is not connected", [110] = "Connection timed out", [111] = "Connection refused",
    [113] = "No route to host", [114] = "Operation already in progress", [115] = "Operation now in progress",
    [125] = "Operation Canceled",
};
static const char *errmsg(int e, char *tmp, size_t n) {
    if (e >= 0 && e < (int)(sizeof errtext / sizeof *errtext) && errtext[e]) return errtext[e];
    snprintf(tmp, n, "Unknown error %d", e); return tmp;
}
static SYSV char *s_strerror(int e) { static __thread char t[32]; return (char *)errmsg(e, t, sizeof t); }
static SYSV int s_strerror_r(int e, char *buf, size_t n) {  // POSIX form (bionic's strerror_r)
    char t[32]; const char *m = errmsg(e, t, sizeof t);
    if (!n) return 34;
    snprintf(buf, n, "%s", m);
    return strlen(m) >= n ? 34 : 0;
}
static SYSV char *s_gnu_strerror_r(int e, char *buf, size_t n) { s_strerror_r(e, buf, n); return buf; }
static SYSV int s_prctl(int op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (op == 15) { tname_set(GetCurrentThreadId(), (const char *)a); return 0; }  // PR_SET_NAME
    if (op == 16) { const char *n = skate_tname(GetCurrentThreadId()); if (strcmp(n, "?") && n[0] != '@') { strncpy((char *)a, n, 15); ((char *)a)[15] = 0; } return 0; }  // PR_GET_NAME (16-byte buffer); unnamed: left as before
    if (op == 41 || op == 42) return 0;  // PR_SET/GET_NO_NEW_PRIVS
    if (op == 0x53564d41) { SETERR(22); return -1; }               // PR_SET_VMA: older kernels say EINVAL too
    printf("prctl(%d) not emulated\n", op);
    SETERR(22); return -1;
}

#define M(n) { #n, m_##n },
static const struct { const char *name; void *at; } table[] = {
    D1S(M) D2S(M) F1S(M) F2S(M)
    M(frexp) M(frexpf) M(ldexp) M(ldexpf) M(scalbn) M(ilogb) M(llround) M(llroundf) M(modf) M(modff) M(nan) M(finitef)
    M(sincos) M(sincosf) M(remquof)
    { "lround", m_llround }, { "lroundf", m_llroundf },
    { "strcat", s_strcat }, { "strncat", s_strncat }, { "strcspn", s_strcspn }, { "strspn", s_strspn }, { "strpbrk", s_strpbrk },
    { "strnlen", s_strnlen }, { "strcasecmp", s_strcasecmp }, { "strncasecmp", s_strncasecmp }, { "memrchr", s_memrchr },
    { "tolower", s_tolower }, { "isspace", s_isspace }, { "atoi", s_atoi }, { "atol", s_atoll }, { "atoll", s_atoll },
    { "atof", s_atof }, { "strtod", s_strtod }, { "strtof", s_strtof }, { "strtol", s_strtoll }, { "strtoll", s_strtoll },
    { "strtoul", s_strtoull }, { "strtoull", s_strtoull }, { "strtoll_l", s_strtoll_l }, { "strtoull_l", s_strtoull_l },
    { "strcoll_l", s_strcoll_l }, { "strxfrm_l", s_strxfrm_l },
    { "__memcpy_chk", c_memcpy }, { "__memmove_chk", c_memmove }, { "__memset_chk", c_memset }, { "__strlen_chk", c_strlen },
    { "__strchr_chk", c_strchr }, { "__strcpy_chk", c_strcpy }, { "__strcat_chk", c_strcat }, { "__strncpy_chk", c_strncpy },
    { "__strncpy_chk2", c_strncpy2 }, { "qsort", s_qsort }, { "bsearch", s_bsearch },
    { "mbrtowc", s_mbrtowc }, { "mbtowc", s_mbtowc }, { "mbrlen", s_mbrlen }, { "wcrtomb", s_wcrtomb }, { "btowc", s_btowc },
    { "wctob", s_wctob }, { "mbsnrtowcs", s_mbsnrtowcs }, { "mbsrtowcs", s_mbsrtowcs }, { "wcsnrtombs", s_wcsnrtombs },
    { "wcslen", s_wcslen }, { "wmemchr", s_wmemchr }, { "wmemcmp", s_wmemcmp }, { "wcscoll_l", s_wcscoll_l }, { "wcsxfrm_l", s_wcsxfrm_l },
    { "iswalpha_l", s_iswalpha_l }, { "iswblank_l", s_iswblank_l }, { "iswcntrl_l", s_iswcntrl_l }, { "iswdigit_l", s_iswdigit_l },
    { "iswlower_l", s_iswlower_l }, { "iswprint_l", s_iswprint_l }, { "iswpunct_l", s_iswpunct_l }, { "iswspace_l", s_iswspace_l },
    { "iswupper_l", s_iswupper_l }, { "iswxdigit_l", s_iswxdigit_l }, { "towlower_l", s_towlower_l }, { "towupper_l", s_towupper_l },
    { "newlocale", s_newlocale }, { "freelocale", s_freelocale }, { "uselocale", s_uselocale },
    { "__ctype_get_mb_cur_max", s_mb_cur_max }, { "localeconv", s_localeconv },
    { "mmap", t_mmap }, { "munmap", t_munmap }, { "msync", s_msync }, { "mprotect", t_mprotect }, { "madvise", s_madvise }, { "mlock", s_mlock },
    { "time", s_time }, { "gettimeofday", s_gettimeofday }, { "nanosleep", s_nanosleep }, { "clock", s_clock }, { "difftime", s_difftime },
    { "getpagesize", s_getpagesize }, { "sysconf", s_sysconf }, { "getauxval", s_getauxval }, { "getuid", s_getuid },
    { "geteuid", s_getuid }, { "getppid", s_getppid }, { "gettid", s_gettid }, { "getpid", s_getpid },
    { "sched_getcpu", s_sched_getcpu }, { "sched_yield", s_sched_yield }, { "rand", s_rand }, { "srand", s_srand },
    { "arc4random_buf", s_random_buf }, { "getentropy", s_getentropy }, { "__system_property_get", s_property_get },
    { "pthread_self", s_pthread_self }, { "pthread_equal", s_pthread_equal }, { "pthread_setname_np", s_pthread_setname_np },
    { "prctl", s_prctl }, { "strerror", s_strerror }, { "strerror_r", s_strerror_r }, { "__gnu_strerror_r", s_gnu_strerror_r }, { "sysinfo", s_sysinfo },
};

void *shim_lookup_bulk(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}
