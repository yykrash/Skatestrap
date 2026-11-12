// bionic time conversion on the UCRT: gmtime/localtime/mktime/strftime and the tz globals.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shim.h"

// bionic's struct tm: the nine C ints, then long tm_gmtoff and const char *tm_zone (56 bytes)
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; long long gmtoff; const char *zone; } BTm;

long long sv_timezone; int sv_daylight; char *sv_tzname[2] = { "UTC", "UTC" };
static char tz_std[64] = "UTC", tz_dst[64] = "UTC";

static void quiet_invalid(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {}
static SYSV void s_tzset(void) {
    static int once;
    if (!once) { once = 1; _set_invalid_parameter_handler(quiet_invalid); }  // bad strftime formats return 0, not abort
    _tzset();
    long tz; int dl; size_t n;
    _get_timezone(&tz); _get_daylight(&dl);
    _get_tzname(&n, tz_std, sizeof tz_std, 0); _get_tzname(&n, tz_dst, sizeof tz_dst, 1);
    sv_timezone = tz; sv_daylight = dl; sv_tzname[0] = tz_std; sv_tzname[1] = tz_dst;
}
static void to_b(const struct tm *w, BTm *b, long long gmtoff, const char *zone) {
    *b = (BTm){ w->tm_sec, w->tm_min, w->tm_hour, w->tm_mday, w->tm_mon, w->tm_year, w->tm_wday, w->tm_yday, w->tm_isdst, gmtoff, zone };
}
static void to_w(const BTm *b, struct tm *w) {
    *w = (struct tm){ b->sec, b->min, b->hour, b->mday, b->mon, b->year, b->wday, b->yday, b->isdst };
}
static SYSV BTm *s_gmtime_r(const long long *t, BTm *out) {
    struct tm w; __time64_t v = *t;
    if (_gmtime64_s(&w, &v)) return 0;
    to_b(&w, out, 0, "GMT");
    return out;
}
static SYSV BTm *s_localtime_r(const long long *t, BTm *out) {
    s_tzset();
    struct tm w; __time64_t v = *t;
    if (_localtime64_s(&w, &v)) return 0;
    struct tm g = w; long long off = _mkgmtime64(&g) - v;  // seconds east of UTC, DST included (copy: it rewrites isdst)
    to_b(&w, out, off, w.tm_isdst > 0 ? tz_dst : tz_std);
    return out;
}
static __declspec(thread) BTm tm_buf;
static SYSV BTm *s_gmtime(const long long *t) { return s_gmtime_r(t, &tm_buf); }
static SYSV BTm *s_localtime(const long long *t) { return s_localtime_r(t, &tm_buf); }
static SYSV long long s_mktime(BTm *b) {
    s_tzset();
    struct tm w; to_w(b, &w);
    __time64_t v = _mktime64(&w);
    if (v == -1) return -1;
    struct tm g = w; to_b(&w, b, _mkgmtime64(&g) - v, w.tm_isdst > 0 ? tz_dst : tz_std);
    return v;
}
static SYSV long long s_timegm(BTm *b) {
    struct tm w; to_w(b, &w);
    __time64_t v = _mkgmtime64(&w);
    if (v != -1) to_b(&w, b, 0, "GMT");
    return v;
}
static SYSV size_t s_strftime(char *out, size_t max, const char *fmt, const BTm *b) {
    s_tzset();
    struct tm w; to_w(b, &w);
    // shortcut: bionic extras (%s %k %l %P) not translated; add when a caller uses them
    return strftime(out, max, fmt, &w);
}
static SYSV size_t s_strftime_l(char *out, size_t max, const char *fmt, const BTm *b, void *loc) { return s_strftime(out, max, fmt, b); }

static const struct { const char *name; void *at; } table[] = {
    { "gmtime", s_gmtime }, { "gmtime_r", s_gmtime_r }, { "localtime", s_localtime }, { "localtime_r", s_localtime_r },
    { "mktime", s_mktime }, { "timegm", s_timegm }, { "strftime", s_strftime }, { "strftime_l", s_strftime_l },
    { "tzset", s_tzset }, { "timezone", &sv_timezone }, { "daylight", &sv_daylight }, { "tzname", sv_tzname },
};
void *shim_lookup_tm(const char *name) {
    if (!strcmp(name, "tzname")) s_tzset();  // data imports are read before any call: fill them now
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}

int shim_tm_selftest(void) {
    int bad = 0, line = 0; BTm b; long long t = 86400LL * 365;  // 1971-01-01 00:00:00 UTC
    char s[64];
#define CHECK(c) if (!(c)) { bad++; printf("tm selftest fail line %d: %s\n", __LINE__, #c); }
    s_gmtime_r(&t, &b);
    CHECK(b.year == 71 && b.mon == 0 && b.mday == 1 && b.hour == 0 && b.yday == 0)
    CHECK(s_timegm(&b) == t)
    CHECK(s_strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", &b) == 19 && !strcmp(s, "1971-01-01 00:00:00"))
    s_localtime_r(&t, &b);
    CHECK(s_mktime(&b) == t)
    CHECK(s_strftime(s, sizeof s, "%Q", &b) == 0)  // invalid spec: 0, no abort
    return bad + line;
}
