// printf / scanf for libroblox.so: its varargs arrive as a SysV va_list, so the format is walked here
// one conversion at a time and each piece is handed to the Windows C library with the right host types.
// Also the stdio FILE layer (stdin/stdout/stderr only until the file system exists).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

int *shim_errno(void);
void *shim_malloc(size_t n);
#define SETERR(e) (*shim_errno() = (e))

// --- SysV va_list -----------------------------------------------------------------------------------
typedef struct { uint32_t gp, fp; uint8_t *over, *save; } VaList;
static uint64_t va_gp(VaList *v) {
    uint64_t x;
    if (v->gp < 48) { memcpy(&x, v->save + v->gp, 8); v->gp += 8; }
    else { memcpy(&x, v->over, 8); v->over += 8; }
    return x;
}
static double va_fp(VaList *v) {
    double x;
    if (v->fp < 176) { memcpy(&x, v->save + v->fp, 8); v->fp += 16; }
    else { memcpy(&x, v->over, 8); v->over += 8; }
    return x;
}

// --- FILE: bionic's __sF[3] array or the stdin/stdout/stderr pointers both land on the host streams -----
static uint8_t sF[3 * 152];  // bionic sizeof(FILE) on LP64
void *shim_stdin = sF, *shim_stdout = sF + 152, *shim_stderr = sF + 304;
void *shim_sF = sF;
FILE *shim_host_file(void *f);
static FILE *host_file(void *f) { return shim_host_file(f); }
FILE *shim_host_file(void *f) {
    if ((uint8_t *)f >= sF && (uint8_t *)f < sF + sizeof sF) {
        int i = (int)(((uint8_t *)f - sF) / 152);
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return (FILE *)f;  // anything else came from fopen/fdopen in fs.c: a host CRT stream
}

// --- output sink ---------------------------------------------------------------------------------------
typedef struct { char *buf; size_t cap, len; FILE *out; } Sink;
static void put(Sink *s, const char *p, size_t n) {
    if (s->out) fwrite(p, 1, n, s->out);
    else if (s->cap && s->len < s->cap - 1) {
        size_t k = s->cap - 1 - s->len;
        memcpy(s->buf + s->len, p, n < k ? n : k);
    }
    s->len += n;
}
static size_t utf8_of(char *d, uint32_t c) {
    if (c < 0x80) { d[0] = (char)c; return 1; }
    if (c < 0x800) { d[0] = (char)(0xc0 | c >> 6); d[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000) { d[0] = (char)(0xe0 | c >> 12); d[1] = (char)(0x80 | (c >> 6 & 0x3f)); d[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    d[0] = (char)(0xf0 | c >> 18); d[1] = (char)(0x80 | (c >> 12 & 0x3f)); d[2] = (char)(0x80 | (c >> 6 & 0x3f)); d[3] = (char)(0x80 | (c & 0x3f)); return 4;
}
static char *wide_to_utf8(const uint32_t *w) {
    size_t n = 0; while (w[n]) n++;
    char *d = malloc(n * 4 + 1), *p = d;
    for (size_t i = 0; i < n; i++) p += utf8_of(p, w[i]);
    *p = 0;
    return d;
}

// Format one conversion with the host printf: spec is "%[flags][width][.prec]" without length or conversion.
static void host_piece(Sink *s, const char *spec, const char *len, char conv, int kind, uint64_t i, double d, const char *str) {
    char f[64], tmp[512], *out = tmp;
    snprintf(f, sizeof f, "%s%s%c", spec, len, conv);
    int n = kind == 0 ? snprintf(tmp, sizeof tmp, f, i) : kind == 1 ? snprintf(tmp, sizeof tmp, f, d) : snprintf(tmp, sizeof tmp, f, str);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) {
        out = malloc((size_t)n + 1);
        kind == 0 ? snprintf(out, (size_t)n + 1, f, i) : kind == 1 ? snprintf(out, (size_t)n + 1, f, d) : snprintf(out, (size_t)n + 1, f, str);
    }
    put(s, out, (size_t)n);
    if (out != tmp) free(out);
}

static int format(Sink *s, const char *fmt, VaList *v) {
    while (*fmt) {
        if (*fmt != '%') { const char *e = strchr(fmt, '%'); size_t n = e ? (size_t)(e - fmt) : strlen(fmt); put(s, fmt, n); fmt += n; continue; }
        const char *start = fmt++;
        if (*fmt == '%') { put(s, "%", 1); fmt++; continue; }
        char spec[48]; size_t k = 0;
        spec[k++] = '%';
        while (strchr("-+ #0'", *fmt) && *fmt) { if (*fmt != '\'' && k < 8) spec[k++] = *fmt; fmt++; }
        if (*fmt == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)va_gp(v)); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { if (k < 30) spec[k++] = *fmt; fmt++; }
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { int p = (int)va_gp(v); k += (size_t)snprintf(spec + k, sizeof spec - k, p >= 0 ? ".%d" : "", p); fmt++; }
            else { spec[k++] = '.'; while (*fmt >= '0' && *fmt <= '9') { if (k < 40) spec[k++] = *fmt; fmt++; } }
        }
        spec[k] = 0;
        int lng = 0;  // -2 hh, -1 h, 0 none, 1 l, 2 ll/j/z/t/q (all 64-bit on Linux), 3 L
        for (;; fmt++) {
            if (*fmt == 'h') lng = lng == -1 ? -2 : -1;
            else if (*fmt == 'l') lng = lng == 1 ? 2 : 1;
            else if (strchr("jztq", *fmt) && *fmt) lng = 2;
            else if (*fmt == 'L') lng = 3;
            else break;
        }
        char c = *fmt ? *fmt++ : 0;
        switch (c) {
        case 'd': case 'i': {
            int64_t x = (int64_t)va_gp(v);
            x = lng == -2 ? (int64_t)(signed char)x : lng == -1 ? (int64_t)(short)x : lng == 0 ? (int64_t)(int)x : x;
            host_piece(s, spec, "ll", c, 0, (uint64_t)x, 0, 0); break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t x = va_gp(v);
            x = lng == -2 ? (uint8_t)x : lng == -1 ? (uint16_t)x : lng == 0 ? (uint32_t)x : x;
            host_piece(s, spec, "ll", c, 0, x, 0, 0); break;
        }
        case 'c': {
            char b[5] = { 0 };
            uint32_t ch = (uint32_t)va_gp(v);
            if (lng == 1) b[utf8_of(b, ch)] = 0; else b[0] = (char)ch;
            spec[0] = '%'; host_piece(s, spec, "", 's', 2, 0, 0, b); break;
        }
        case 's': {
            const void *p = (const void *)va_gp(v);
            if (!p) { host_piece(s, spec, "", 's', 2, 0, 0, "(null)"); break; }
            if (lng == 1) { char *u = wide_to_utf8(p); host_piece(s, spec, "", 's', 2, 0, 0, u); free(u); }
            else host_piece(s, spec, "", 's', 2, 0, 0, p);
            break;
        }
        case 'p': { char b[24]; snprintf(b, sizeof b, "0x%llx", (unsigned long long)va_gp(v)); spec[1] = 0; host_piece(s, "%", "", 's', 2, 0, 0, b); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (lng == 3) { printf("\n%%L%c (long double) not emulated\n", c); va_fp(v); put(s, "?", 1); break; }  // shortcut: quad long double
            host_piece(s, spec, "", c, 1, 0, va_fp(v), 0); break;
        case 'n': {
            void *p = (void *)va_gp(v);
            if (lng == -2) *(signed char *)p = (signed char)s->len; else if (lng == -1) *(short *)p = (short)s->len;
            else if (lng >= 1) *(int64_t *)p = (int64_t)s->len; else *(int *)p = (int)s->len;
            break;
        }
        case 'm': { const char *e = strerror(*shim_errno()); put(s, e, strlen(e)); break; }
        default: put(s, start, (size_t)(fmt - start)); break;  // unknown: print it as written
        }
    }
    return (int)s->len;
}

SYSV int sv_vsnprintf(char *buf, size_t n, const char *fmt, VaList *v) {
    Sink s = { buf, n, 0, 0 }; VaList c = *v;
    int r = format(&s, fmt, &c);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return r;
}
SYSV int sv_vsprintf(char *buf, const char *fmt, VaList *v) { return sv_vsnprintf(buf, (size_t)INT32_MAX, fmt, v); }
SYSV int sv_vfprintf(void *f, const char *fmt, VaList *v) {
    FILE *h = host_file(f);
    if (!h) { SETERR(9); return -1; }
    Sink s = { 0, 0, 0, h }; VaList c = *v;
    return format(&s, fmt, &c);
}
SYSV int sv_vprintf(const char *fmt, VaList *v) { return sv_vfprintf(shim_stdout, fmt, v); }
static SYSV int c_vsnprintf(char *buf, size_t n, int flags, size_t sl, const char *fmt, VaList *v) {
    if (n > sl) { printf("\nfortify: vsnprintf overflow\n"); fflush(stdout); ExitProcess(7); }
    return sv_vsnprintf(buf, n, fmt, v);
}
static SYSV int c_vsprintf(char *buf, int flags, size_t sl, const char *fmt, VaList *v) {
    int r = sv_vsnprintf(buf, sl, fmt, v);
    if ((size_t)r >= sl) { printf("\nfortify: vsprintf overflow\n"); fflush(stdout); ExitProcess(7); }
    return r;
}
static SYSV int s_vasprintf(char **out, const char *fmt, VaList *v) {
    int n = sv_vsnprintf(0, 0, fmt, v);
    *out = shim_malloc((size_t)n + 1);  // the game frees it with our free(), so it must come from our malloc
    return sv_vsnprintf(*out, (size_t)n + 1, fmt, v);
}

// --- android log / syslog --------------------------------------------------------------------------------
SYSV int sv_log_vprint(int prio, const char *tag, const char *fmt, VaList *v) {
    char b[1024]; sv_vsnprintf(b, sizeof b, fmt, v);
    printf("[log %d %s] %s\n", prio, tag ? tag : "", b);
    return 1;
}
SYSV void sv_log_vassert(const char *cond, const char *tag, const char *fmt, VaList *v) {
    char b[1024]; if (fmt) sv_vsnprintf(b, sizeof b, fmt, v); else b[0] = 0;
    printf("\n[assert %s] %s %s\n", tag ? tag : "", cond ? cond : "", b);
    fflush(stdout); ExitProcess(6);
}
SYSV void sv_vsyslog(int prio, const char *fmt, VaList *v) { sv_log_vprint(prio, "syslog", fmt, v); }

// --- scanf: each conversion runs through the host sscanf with a trailing %n to learn how far it read --------
static int scan(const char *str, const char *fmt, VaList *v, size_t *eaten) {
    VaList c = *v;
    const char *in = str;
    int count = 0;
    while (*fmt) {
        if (*fmt == ' ' || (*fmt >= '\t' && *fmt <= '\r')) { while (*in == ' ' || (*in >= '\t' && *in <= '\r')) in++; fmt++; continue; }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (*in != *fmt) break;
            in++; fmt++; continue;
        }
        fmt++;
        int skip = 0; char spec[96]; size_t k = 0;
        spec[k++] = '%';
        if (*fmt == '*') { skip = 1; spec[k++] = '*'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { if (k < 20) spec[k++] = *fmt; fmt++; }
        int lng = 0;
        for (;; fmt++) {
            if (*fmt == 'h') lng = lng == -1 ? -2 : -1;
            else if (*fmt == 'l') lng = lng == 1 ? 2 : 1;
            else if (strchr("jztq", *fmt) && *fmt) lng = 2;
            else if (*fmt == 'L') lng = 3;
            else break;
        }
        char conv = *fmt;
        if (!conv) break;
        if (strchr("diuoxXn", conv)) {
            const char *h = lng == -2 ? "hh" : lng == -1 ? "h" : lng == 0 ? "" : "ll";
            k += (size_t)snprintf(spec + k, sizeof spec - k, "%s%c", h, conv); fmt++;
        } else if (strchr("eEfgGaA", conv)) {
            if (lng == 3) { printf("\nscanf %%L%c not emulated\n", conv); break; }
            k += (size_t)snprintf(spec + k, sizeof spec - k, "%s%c", lng ? "l" : "", conv); fmt++;
        } else if (conv == '[') {
            const char *e = fmt + 1;
            if (*e == '^') e++;
            if (*e == ']') e++;
            while (*e && *e != ']') e++;
            if (!*e || (size_t)(e - fmt) + k + 4 > sizeof spec) break;
            memcpy(spec + k, fmt, (size_t)(e - fmt + 1)); k += (size_t)(e - fmt + 1); spec[k] = 0;
            fmt = e + 1;
        } else if (conv == 's' || conv == 'c') {
            if (lng == 1) { printf("\nscanf %%l%c not emulated\n", conv); break; }
            spec[k++] = conv; spec[k] = 0; fmt++;
        } else break;
        if (conv == 'n') { if (!skip) { void *p = (void *)va_gp(&c); int64_t n = in - str;
            if (lng == 2) *(int64_t *)p = n; else *(int *)p = (int)n; } continue; }
        strcat(spec, "%n");
        int used = -1, r;
        if (skip) r = sscanf(in, spec, &used);
        else r = sscanf(in, spec, (void *)va_gp(&c), &used);
        if (used < 0) { if (r == EOF && count == 0) return -1; break; }
        in += used;
        if (!skip) count++;
    }
    if (eaten) *eaten = (size_t)(in - str);
    return count;
}
SYSV int sv_vsscanf(const char *str, const char *fmt, VaList *v) { return scan(str, fmt, v, 0); }
// fscanf: read what is left of the stream, scan it as a string, then seek back to just past what was used.
// shortcut: on a pipe (our /proc files) the seek fails and the unread rest is lost; fine for one-shot reads like statm
SYSV int sv_vfscanf(void *f, const char *fmt, VaList *v) {
    FILE *h = host_file(f);
    if (!h) { SETERR(9); return -1; }
    static char b[65536]; static SRWLOCK l = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&l);
    long long at = _ftelli64(h); size_t n = fread(b, 1, sizeof b - 1, h), eaten = 0; b[n] = 0;
    int r = n ? scan(b, fmt, v, &eaten) : -1;
    if (at >= 0) { clearerr(h); _fseeki64(h, at + (long long)eaten, SEEK_SET); }
    ReleaseSRWLockExclusive(&l);
    return r;
}

// --- simple stdio on the three host streams ---------------------------------------------------------------
static SYSV int s_fputs(const char *str, void *f) { FILE *h = host_file(f); if (!h) { SETERR(9); return -1; } return fputs(str, h); }
static SYSV int s_puts(const char *str) { return puts(str); }
static SYSV int s_fputc(int ch, void *f) { FILE *h = host_file(f); if (!h) { SETERR(9); return -1; } return fputc(ch, h); }
static SYSV size_t s_fwrite(const void *p, size_t sz, size_t n, void *f) { FILE *h = host_file(f); if (!h) { SETERR(9); return 0; } return fwrite(p, sz, n, h); }
static SYSV int s_fflush(void *f) { if (!f) { fflush(stdout); fflush(stderr); return 0; } FILE *h = host_file(f); return h ? fflush(h) : 0; }
static SYSV int s_fileno(void *f) { return _fileno(host_file(f)); }
static SYSV int s_setvbuf(void *f, char *b, int mode, size_t n) { return 0; }
static SYSV int s_ferror(void *f) { return ferror(host_file(f)); }
static SYSV int s_feof(void *f) { return feof(host_file(f)); }
static SYSV void s_clearerr(void *f) { clearerr(host_file(f)); }

SYSV int va_snprintf(), va_sprintf(), va_printf(), va_fprintf(), va_sscanf(), va_fscanf(), va_log_print(), va_syslog(), va_log_assert();
static const struct { const char *name; void *at; } table[] = {
    { "snprintf", va_snprintf }, { "sprintf", va_sprintf }, { "printf", va_printf }, { "fprintf", va_fprintf },
    { "sscanf", va_sscanf }, { "fscanf", va_fscanf }, { "__android_log_print", va_log_print }, { "syslog", va_syslog },
    { "__android_log_assert", va_log_assert },
    { "vsnprintf", sv_vsnprintf }, { "vsprintf", sv_vsprintf }, { "vfprintf", sv_vfprintf }, { "vprintf", sv_vprintf },
    { "__vsnprintf_chk", c_vsnprintf }, { "__vsprintf_chk", c_vsprintf }, { "vasprintf", s_vasprintf }, { "vsscanf", sv_vsscanf },
    { "fputs", s_fputs }, { "puts", s_puts }, { "fputc", s_fputc }, { "fwrite", s_fwrite }, { "fflush", s_fflush },
    { "fileno", s_fileno }, { "setvbuf", s_setvbuf }, { "ferror", s_ferror }, { "feof", s_feof }, { "clearerr", s_clearerr },
    { "stdin", &shim_stdin }, { "stdout", &shim_stdout }, { "stderr", &shim_stderr }, { "__sF", sF },
};

void *shim_lookup_fmt(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}
