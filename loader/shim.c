// bionic libc / libm / libdl / liblog pieces for libroblox.so, on Windows. SysV calling convention throughout.
// Grown in the order the game's start-up asks for them (loader.c traps every import not listed here).
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

void *shim_lookup_bulk(const char *name), *shim_lookup_fmt(const char *name),
     *shim_lookup_egl_hand(const char *name), *shim_lookup_threads(const char *name), *shim_lookup_dl(const char *name), *shim_lookup_fs(const char *name), *shim_lookup_tm(const char *name), *shim_lookup_asset(const char *name), *shim_lookup_net(const char *name), *shim_lookup_gl(const char *name), *shim_lookup_vk(const char *name);
static uint8_t *image; static uint64_t image_span;
uint8_t *shim_image; uint64_t shim_image_span;  // for dl.c
void shim_init(uint8_t *i, uint64_t s) { image = i; image_span = s; shim_image = i; shim_image_span = s; }

// --- memory: every block carries its real start, so free() works for aligned ones too ---------------
typedef struct { void *raw; size_t size; } Head;
static void *alloc_aligned(size_t align, size_t size) {
    if (align < 16) align = 16;
    uint8_t *raw = malloc(size + align + sizeof(Head));
    if (!raw) return 0;
    uintptr_t p = ((uintptr_t)raw + sizeof(Head) + align - 1) & ~(uintptr_t)(align - 1);
    ((Head *)p)[-1] = (Head){ raw, size };
    return (void *)p;
}
static SYSV void *s_malloc(size_t n) { return alloc_aligned(16, n); }
void *shim_malloc(size_t n) { return alloc_aligned(16, n); }
static SYSV void s_free(void *p) { if (p) free(((Head *)p)[-1].raw); }
static SYSV void *s_calloc(size_t a, size_t b) { void *p = alloc_aligned(16, a * b); if (p) memset(p, 0, a * b); return p; }
static SYSV void *s_realloc(void *p, size_t n) {
    if (!p) return s_malloc(n);
    void *q = s_malloc(n);
    if (q) { size_t old = ((Head *)p)[-1].size; memcpy(q, p, old < n ? old : n); }
    s_free(p);
    return q;
}
static SYSV void *s_memalign(size_t a, size_t n) { return alloc_aligned(a, n); }
static SYSV int s_posix_memalign(void **out, size_t a, size_t n) { *out = alloc_aligned(a, n); return *out ? 0 : 12; }
static SYSV size_t s_malloc_usable_size(void *p) { return p ? ((Head *)p)[-1].size : 0; }

// --- strings / memory (plain C semantics, only the calling convention differs) ---------------------
static SYSV void *s_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static SYSV void *s_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static SYSV void *s_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static SYSV int s_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static SYSV void *s_memchr(const void *a, int c, size_t n) { return (void *)memchr(a, c, n); }
static SYSV size_t s_strlen(const char *s) { return strlen(s); }
static SYSV int s_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static SYSV int s_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static SYSV char *s_strcpy(char *d, const char *s) { return strcpy(d, s); }
static SYSV char *s_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static SYSV char *s_strchr(const char *s, int c) { return (char *)strchr(s, c); }
static SYSV char *s_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
static SYSV char *s_strstr(const char *a, const char *b) { return (char *)strstr(a, b); }
static SYSV char *s_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = s_malloc(n); memcpy(d, s, n); return d; }

// --- process / errors -----------------------------------------------------------------------------------
static __declspec(thread) int thread_errno;
static SYSV int *s_errno(void) { return &thread_errno; }
int *shim_errno(void) { return &thread_errno; }
static SYSV int s_cxa_atexit(void *f, void *arg, void *dso) { return 0; }  // shortcut: exit-time destructors never run; fine until clean shutdown matters
static SYSV void s_cxa_finalize(void *dso) {}
void lib_ret(void *r), lib_callers(const char *tag, uint64_t *fp);
static SYSV void s_stack_chk_fail(void) {
    printf("\nstack smashing detected (__stack_chk_fail)\n");
    lib_ret(__builtin_return_address(0)); lib_callers("  callers", __builtin_frame_address(0));
    fflush(stdout); ExitProcess(5);
}
static SYSV void s_abort(void) { printf("\nabort() called\n"); fflush(stdout); ExitProcess(6); }
static SYSV char *s_getenv(const char *name) { return 0; }

// --- futex + clocks: libroblox calls syscall(SYS_futex) on its own cond words (Mocktail notes the same),
// so every lock below lives in the guest object and parks through this one futex. ---------------------------
typedef struct { int64_t sec, nsec; } Timespec;  // Linux x86-64 struct timespec
enum { E_PERM = 1, E_AGAIN = 11, E_BUSY = 16, E_INVAL = 22, E_DEADLK = 35, E_NOSYS = 38, E_TIMEDOUT = 110 };
int64_t now_ns(int clock) {  // 0 = CLOCK_REALTIME, anything else monotonic
    if (clock == 0) {
        FILETIME f; GetSystemTimePreciseAsFileTime(&f);
        uint64_t t = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
        return (int64_t)(t - 116444736000000000ull) * 100;
    }
    static LARGE_INTEGER freq; LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
volatile LONG skate_clock_n[12];  // calls per clock id (SKATESTRAP_FPS report)
static int64_t cpu_ns(HANDLE h, int thread) {  // CPU time used: Linux clocks 2 (process) and 3 (thread)
    FILETIME c, e, k, us;
    if (!(thread ? GetThreadTimes(h, &c, &e, &k, &us) : GetProcessTimes(h, &c, &e, &k, &us))) return 0;
    return (int64_t)((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime)) * 100;
}
static SYSV int s_clock_gettime(int clock, Timespec *t) {
    // (counted only with SKATESTRAP_FPS: ~1M calls/s from every thread made the shared counter a contended cache line)
    static int on = -1; if (on < 0) on = getenv("SKATESTRAP_FPS") != 0;
    if (on && clock >= 0 && clock < 12) InterlockedIncrement(&skate_clock_n[clock]);
    // CPU-time clocks answered with wall time made every job look as costly as its wall time
    int64_t n = clock == 2 ? cpu_ns(GetCurrentProcess(), 0) : clock == 3 ? cpu_ns(GetCurrentThread(), 1) : now_ns(clock);
    t->sec = n / 1000000000; t->nsec = n % 1000000000;
    return 0;
}
// Returns 0 or a positive errno. WAIT/WAIT_BITSET park while *addr == val; WAKE/WAKE_BITSET wake waiters.
// SKATESTRAP_TRACE_WAIT=1: long or oversleeping blocking calls (who paces the frames?), first 80
void wait_trace(const char *kind, int64_t req_ms, int64_t t0, uint64_t *fp) {
    static int on = -1; if (on < 0) on = getenv("SKATESTRAP_TRACE_WAIT") != 0;
    if (!on) return;
    if (req_ms >= 0 && req_ms <= 3) {  // short sleeps: Windows rounds them up; print the cost every 20000
        static volatile LONG64 n, us; static volatile LONG64 byk[4];
        InterlockedAdd64(&us, (now_ns(1) - t0) / 1000); InterlockedIncrement64(&byk[kind[0] == 'f' ? 0 : kind[0] == 'n' ? 1 : kind[0] == 'p' ? 2 : 3]);
        if (InterlockedIncrement64(&n) % 20000 == 0) {
            printf("short waits (asked <= 3 ms): %lld so far, %.2f ms each on average; futex %lld nanosleep %lld poll %lld epoll %lld\n",
                   (long long)n, us / 1000.0 / n, (long long)byk[0], (long long)byk[1], (long long)byk[2], (long long)byk[3]);
            void lib_callers(const char *, uint64_t *); lib_callers("  e.g. from", fp);
        }
    }
    int64_t got = (now_ns(1) - t0) / 1000000;
    if (got < 100 && (req_ms < 0 || got - req_ms < 30)) return;
    static volatile LONG shown; if (InterlockedIncrement(&shown) > 80) return;
    printf("wait %s tid %lu asked %lld ms got %lld ms\n", kind, GetCurrentThreadId(), (long long)req_ms, (long long)got);
    void lib_callers(const char *, uint64_t *); lib_callers("  from", fp);
}
// Sub-ms sleep: Linux nanosleep/futex timeouts are ~50 us precise; Sleep/WaitOnAddress round up to whole timer ticks.
// High-resolution waitable timer (Win10 1803+), one per thread.
static __declspec(thread) HANDLE hr_timer;  // shared by hr_sleep and timed futex waits (one wait at a time per thread)
static HANDLE thread_timer(void) {
    if (!hr_timer) hr_timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return hr_timer;
}
void hr_sleep(int64_t ns) {
    if (ns <= 0) { SwitchToThread(); return; }  // nanosleep(0): a yield, as Sleep(0) was
    HANDLE t = thread_timer();
    LARGE_INTEGER due = { .QuadPart = -(ns + 99) / 100 };
    if (t && SetWaitableTimerEx(t, &due, 0, 0, 0, 0, 0)) WaitForSingleObject(t, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}
volatile void *skate_render_addr;  // last futex word the render thread parked on (SKATESTRAP_FPS trace)
// Futex as Linux has it: a waiter table keyed by address. Each waiter blocks on its own event (and a high-res timer
// when timed), so a wake lands at once and a timeout to ~0.1 ms; WaitOnAddress only times out in whole timer ticks.
// The word is checked under the bucket lock, and wakers take that lock, so no wake between check and sleep is lost.
// shortcut: per-thread event and timer handles are never closed when a thread exits; fine for a game's thread count
typedef struct FW { volatile uint32_t *addr; HANDLE ev; struct FW *next; volatile LONG woken, asleep; uint32_t val; DWORD tid; int64_t t0, wake_ns; DWORD waker; } FW;
// SKATESTRAP_FPS waker chain: each thread's last 4 finished waits (futex / sleep), so a long render wait can be traced
// back: render woken by A, A's wait ended because B woke it (or its timeout ran out), ...  Read racily; trace only.
typedef struct { int64_t t0, end, wake_ns, tmo; DWORD waker; char kind; } LW;
static struct { volatile LONG tid; LONG n; LW r[4]; } lw[4096];
static LW *lw_slot(DWORD tid) {
    for (unsigned i = (tid >> 2) * 2654435761u >> 20, j = 0; j < 4096; j++, i = (i + 1) & 4095) {
        LONG t = lw[i].tid;
        if (t == (LONG)tid || (!t && !InterlockedCompareExchange(&lw[i].tid, (LONG)tid, 0))) return lw[i].r + (lw[i].n++ & 3);
    }
    return 0;
}
void lw_note(char kind, int64_t t0, int64_t tmo, DWORD waker, int64_t wake_ns) {
    static int on = -1; if (on < 0) on = getenv("SKATESTRAP_FPS") != 0;
    LW *r = on ? lw_slot(GetCurrentThreadId()) : 0;
    if (r) { r->t0 = t0; r->end = now_ns(1); r->tmo = tmo; r->waker = waker; r->wake_ns = wake_ns; r->kind = kind; }
}
static const LW *lw_find(DWORD tid, int64_t before) {  // tid's last wait that ended by `before`
    for (unsigned i = (tid >> 2) * 2654435761u >> 20, j = 0; j < 4096; j++, i = (i + 1) & 4095) {
        if (lw[i].tid == (LONG)tid) { const LW *best = 0; for (int k = 0; k < 4; k++) if (lw[i].r[k].end && lw[i].r[k].end <= before && (!best || lw[i].r[k].end > best->end)) best = &lw[i].r[k]; return best; }
        if (!lw[i].tid) return 0;
    }
    return 0;
}
static void lw_chain(char *o, size_t n, int64_t t0, DWORD waker, int64_t wake_ns) {
    int64_t now = now_ns(1); size_t k = 0; const char *skate_tname(DWORD);
#define P(...) (k += k < n ? snprintf(o + k, n - k, __VA_ARGS__) : 0)
    P("render waited %.1f ms, woken by %lu(%s) %lld us after posting", (now - t0) / 1e6, waker, skate_tname(waker), waker ? (long long)((now - wake_ns) / 1000) : 0);
    for (int hop = 0; hop < 6 && waker; hop++) {
        const LW *r = lw_find(waker, wake_ns);
        if (!r) { P("; %lu(%s): no wait seen (poll/sem/IO?)", waker, skate_tname(waker)); break; }
        P("; %lu(%s): %s %.1f ms, then ran %.1f ms", waker, skate_tname(waker), r->kind == 's' ? "slept" : "futex", (r->end - r->t0) / 1e6, (wake_ns - r->end) / 1e6);
        if (r->kind == 's' || !r->waker) { if (r->tmo >= 0) P(" (timeout %.1f ms)", r->tmo / 1e6); else P(" (no waker?)"); break; }
        P(" woken by %lu +%lld us", r->waker, (long long)((r->end - r->wake_ns) / 1000));
        waker = r->waker; wake_ns = r->wake_ns;
    }
#undef P
}
static struct { SRWLOCK l; FW *head; } fbk[256];
static __declspec(thread) HANDLE fw_event;
static int old_waits(void) { static int o = -1; if (o < 0) { const char *e = getenv("SKATESTRAP_OLD_WAITS"); o = e && e[0] == '1'; } return o; }  // same rule as net.c
// Untimed waits park with NtWaitForAlertByThreadId (what WaitOnAddress uses inside): no event object, so a wake is
// one cheap alert instead of SetEvent + WaitForMultipleObjects. Roblox's task kernel hands off ~100k untimed waits/s
// (stall stacks 2026-11-11). Timed waits keep the event + high-res timer. SKATESTRAP_FUTEX_EVENT=1: events for all.
typedef LONG (NTAPI *NtAlertFn)(HANDLE); typedef LONG (NTAPI *NtWaitAlertFn)(void *, LARGE_INTEGER *);
static NtAlertFn nt_alert; static NtWaitAlertFn nt_wait_alert;
static int use_alerts(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SKATESTRAP_FUTEX_EVENT"); HMODULE nt = GetModuleHandleA("ntdll.dll");
        nt_alert = (NtAlertFn)GetProcAddress(nt, "NtAlertThreadByThreadId"); nt_wait_alert = (NtWaitAlertFn)GetProcAddress(nt, "NtWaitForAlertByThreadId");
        on = !(e && e[0] == '1') && nt_alert && nt_wait_alert;
    }
    return on;
}
static int fbucket(volatile uint32_t *a) { return (int)((((uintptr_t)a >> 2) * 2654435761u) >> 24) & 255; }
static int futex_wake(volatile uint32_t *addr, uint32_t n) {
    int b = fbucket(addr), woke = 0;
    AcquireSRWLockExclusive(&fbk[b].l);
    for (FW **pp = &fbk[b].head; *pp && (uint32_t)woke < n;) {
        FW *w = *pp;
        if (w->addr != addr) { pp = &w->next; continue; }
        *pp = w->next; w->waker = GetCurrentThreadId(); w->wake_ns = now_ns(1);
        { extern volatile DWORD skate_render_tid;  // last wakers of the render thread: SKATESTRAP_PROF=poster picks its thread from these
          if (w->tid == skate_render_tid) { extern volatile DWORD skate_wk_tid[8]; extern volatile LONG skate_wk_n; extern volatile long long skate_wk_ns[8];
            LONG q = InterlockedIncrement(&skate_wk_n) & 7; skate_wk_tid[q] = w->waker; skate_wk_ns[q] = w->wake_ns;
            extern volatile DWORD skate_pump_tid; extern volatile LONG skate_pump_handoffs;  // the pump skips its nap while it hands work to render
            if (w->waker == skate_pump_tid) InterlockedIncrement(&skate_pump_handoffs); } }
        InterlockedExchange(&w->woken, 1);  // full barrier; pairs with the waiter's asleep store (one of us sees the other)
        if (w->asleep) { if (w->ev) SetEvent(w->ev); else nt_alert((HANDLE)(uintptr_t)w->tid); }  // still spinning: it sees woken, no kernel call. w lives while we hold the lock
        woke++;
    }
    ReleaseSRWLockExclusive(&fbk[b].l);
    return woke;
}
// ns < 0: no timeout. Returns 0 (woken, or the word already moved on: E_AGAIN) or E_TIMEDOUT.
// SKATESTRAP_FPS frame-gap trace (egl_hand.c skate_frame): render thread parks since the last frame, and the waker chain
// of the longest one. (A spin before parking was tried: Roblox re-waits on words that keep moving, so it only burned cores.)
volatile long long skate_gp_n, skate_gp_ns, skate_gp_max; char skate_gp_chain[600];
static int futex_park(volatile uint32_t *addr, uint32_t val, int64_t ns) {
    if (!fw_event) fw_event = CreateEventW(0, FALSE, FALSE, 0);
    int b = fbucket(addr); FW w = { addr, ns < 0 && use_alerts() ? 0 : fw_event, 0, 0, 0, val, GetCurrentThreadId(), now_ns(1) };
    AcquireSRWLockExclusive(&fbk[b].l);
    if (*addr != val) { ReleaseSRWLockExclusive(&fbk[b].l); return E_AGAIN; }
    // newest first (LIFO). Linux wakes oldest first, but FIFO here was much worse in game (177 vs 229 fps, 2026-11-11):
    // it wakes the coldest thread. LIFO wakes one still warm.
    w.next = fbk[b].head; fbk[b].head = &w;
    ReleaseSRWLockExclusive(&fbk[b].l);
    // Roblox's job workers hand off through futexes ~100k times/s, a few us each. A Windows sleep + wake costs ~7 us
    // a round, so first spin on our own woken flag (SKATESTRAP_FUTEX_SPIN us, default 0 = off: spinning made the long hitches worse on a 6-core, A/B 2026-11-11). Only a real wake ends
    // it, as on Linux; spinning on the word itself returned on every change and Roblox just re-waited, burning cores.
    static int spin_us = -1; if (spin_us < 0) { const char *e = getenv("SKATESTRAP_FUTEX_SPIN"); spin_us = e ? atoi(e) : 0; }
    int64_t spin_end = w.t0 + (ns >= 0 && ns < spin_us * 1000ll ? ns : spin_us * 1000ll);
    for (int i = 0; !w.woken && spin_us; i++) { _mm_pause(); if ((i & 63) == 63 && now_ns(1) >= spin_end) break; }
    InterlockedExchange(&w.asleep, 1);
    HANDLE t = 0;
    if (!w.ev) { while (!w.woken) nt_wait_alert((void *)addr, 0); }  // a stale alert from an earlier wake just loops once
    else if (!w.woken) {
        t = ns >= 0 ? thread_timer() : 0; HANDLE hs[2] = { w.ev, t };
        int64_t left = ns >= 0 ? ns - (now_ns(1) - w.t0) : 0; if (ns >= 0 && left < 1) left = 1;
        LARGE_INTEGER due = { .QuadPart = -(left + 99) / 100 };
        if (t && !SetWaitableTimerEx(t, &due, 0, 0, 0, 0, 0)) t = 0;
        if (ns >= 0 && !t) WaitForSingleObject(w.ev, (DWORD)((left + 999999) / 1000000));  // no timer: whole-ms timeout
        else WaitForMultipleObjects(t ? 2 : 1, hs, FALSE, INFINITE);
    }
    AcquireSRWLockExclusive(&fbk[b].l);
    if (!w.woken) { for (FW **pp = &fbk[b].head; *pp; pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; break; } }
    else if (w.ev) ResetEvent(w.ev);  // woken while the timer also fired: drop the event's pending signal
    ReleaseSRWLockExclusive(&fbk[b].l);
    if (t && w.woken) CancelWaitableTimer(t);
    lw_note('f', w.t0, ns, w.woken ? w.waker : 0, w.wake_ns);
    { extern volatile DWORD skate_render_tid; int64_t d = now_ns(1) - w.t0;
      if (w.tid == skate_render_tid) { skate_gp_n++; skate_gp_ns += d; if (d > skate_gp_max) { skate_gp_max = d; if (w.woken) lw_chain(skate_gp_chain, sizeof skate_gp_chain, w.t0, w.waker, w.wake_ns); else snprintf(skate_gp_chain, sizeof skate_gp_chain, "timed out after %.1f ms", d / 1e6); } } }
    return w.woken ? 0 : E_TIMEDOUT;
}
static int futex_old(volatile uint32_t *addr, int op, uint32_t val, const Timespec *t);
// Freeze watchdog (loader.c flusher, no frame for 3 s): every parked waiter. "MOVED" = its word no longer holds the
// value it parked on, yet nobody woke it: a lost wake in this futex. Waiters parked over 1 s only.
void futex_dump(void) {
    extern volatile DWORD skate_render_tid; int64_t now = now_ns(1); int n = 0, moved = 0;
    printf("freeze: no frame for 3 s; render tid %lu; futex waiters parked > 1 s:\n", skate_render_tid);
    for (int b = 0; b < 256; b++) {
        AcquireSRWLockShared(&fbk[b].l);
        for (FW *w = fbk[b].head; w; w = w->next) {
            if (now - w->t0 < 1000000000) continue;
            uint32_t cur = *w->addr; n++; moved += cur != w->val;
            if (n <= 60) printf("  tid %lu addr %p want %08x now %08x for %lld ms%s%s (+4: %d)\n", w->tid, (void *)w->addr, w->val, cur,
                                (long long)((now - w->t0) / 1000000), cur != w->val ? " MOVED" : "", w->tid == skate_render_tid ? " (render)" : "",
                                ((volatile int32_t *)w->addr)[1]);  // mutex owner tid when the word is a bionic mutex
        }
        ReleaseSRWLockShared(&fbk[b].l);
    }
    printf("freeze: %d waiters, %d with a moved word\n", n, moved);
}
static int futex(volatile uint32_t *addr, int op, uint32_t val, const Timespec *t) {
    if (old_waits()) return futex_old(addr, op, val, t);
    int cmd = op & 0x7f;
    if (cmd == 1 || cmd == 10) { futex_wake(addr, val); return 0; }  // FUTEX_WAKE, FUTEX_WAKE_BITSET (bitset ignored: all match)
    if (cmd != 0 && cmd != 9) { printf("futex op %d not emulated\n", op); return E_NOSYS; }
    int64_t ns = -1, tw = now_ns(1);
    if (t) {
        ns = t->sec * 1000000000 + t->nsec;
        if (cmd == 9) ns -= now_ns((op & 256) ? 0 : 1);  // BITSET timeouts are absolute
        if (ns <= 0) return *addr != val ? E_AGAIN : E_TIMEDOUT;
    }
    extern volatile DWORD skate_render_tid;
    int render = skate_render_tid == GetCurrentThreadId();
    if (render) skate_render_addr = addr;
    int r = futex_park(addr, val, ns);
    if (ns >= 0) wait_trace("futex", ns / 1000000, tw, __builtin_frame_address(0));
    if (render) {
        extern volatile long long skate_rw_ns, skate_rw_n, skate_rw_to; skate_rw_ns += now_ns(1) - tw; skate_rw_n++; if (r == E_TIMEDOUT) skate_rw_to++;
        if (now_ns(1) - tw > 30000000) {
            printf("render wait futex %lld ms (timeout %lld ms)\n", (long long)((now_ns(1) - tw) / 1000000), (long long)(ns / 1000000));
            void lib_callers(const char *, uint64_t *); lib_callers("  at", __builtin_frame_address(0));
        }
    }
    return r;
}
// loader.exe --selftest: futex timeout precision, cross-thread wake, value mismatch
static volatile uint32_t ft_word; static volatile int64_t ft_woke_ns;
static DWORD WINAPI ft_waiter(void *x) { futex(&ft_word, 0, 0, 0); ft_woke_ns = now_ns(1); return 0; }
static DWORD WINAPI ft_pong(void *x) {
    for (uint32_t i = 1; i < 20000; i += 2) { while (ft_word < i) futex(&ft_word, 0, ft_word, 0); ft_word = i + 1; futex(&ft_word, 1, 1, 0); }
    return 0;
}
int shim_futex_selftest(void) {
    int bad = 0; if (old_waits()) return 0;
    timeBeginPeriod(1);  // --selftest exits before loader.c sets the 1 ms tick
    Timespec ts = { 0, 500000 }; int64_t worst = 0;
    for (int i = 0; i < 5; i++) { int64_t t0 = now_ns(1); int r = futex(&ft_word, 0, 0, &ts); int64_t late = now_ns(1) - t0 - 500000;
        if (r != E_TIMEDOUT || late < 0) { bad++; printf("futex selftest: timed wait r=%d late=%lld\n", r, (long long)late); } if (late > worst) worst = late; }
    if (worst > 1500000) { bad++; printf("futex selftest: 500 us timeout up to %lld us late\n", (long long)(worst / 1000)); }
    if (futex(&ft_word, 0, 1, &ts) != E_AGAIN) { bad++; printf("futex selftest: mismatch not E_AGAIN\n"); }
    HANDLE h = CreateThread(0, 0, ft_waiter, 0, 0, 0); Sleep(20);
    int64_t tw = now_ns(1); ft_word = 1; futex(&ft_word, 1, 1, 0);
    if (WaitForSingleObject(h, 1000)) { bad++; printf("futex selftest: wake lost\n"); }
    else if (ft_woke_ns - tw > 2000000) { bad++; printf("futex selftest: wake took %lld us\n", (long long)((ft_woke_ns - tw) / 1000)); }
    CloseHandle(h); ft_word = 0;
    // handoff cost: two threads ping-pong untimed waits, as Roblox's task kernel does (SKATESTRAP_FUTEX_EVENT=1 to compare)
    h = CreateThread(0, 0, ft_pong, 0, 0, 0); int64_t tp = now_ns(1);
    for (uint32_t i = 0; i < 20000; i += 2) { ft_word = i + 1; futex(&ft_word, 1, 1, 0); while (ft_word == i + 1) futex(&ft_word, 0, i + 1, 0); }
    printf("futex selftest: ping-pong %.1f us per handoff (%s)\n", (now_ns(1) - tp) / 20000 / 1e3, use_alerts() ? "alerts" : "events");
    WaitForSingleObject(h, 1000); CloseHandle(h); ft_word = 0; timeEndPeriod(1);
    return bad;
}
// SKATESTRAP_OLD_WAITS=1: the previous WaitOnAddress futex, kept for A/B runs
static int futex_old(volatile uint32_t *addr, int op, uint32_t val, const Timespec *t) {
    int cmd = op & 0x7f;
    extern volatile DWORD skate_render_tid;
    if (cmd == 1 || cmd == 10) {  // FUTEX_WAKE, FUTEX_WAKE_BITSET
        if (skate_render_addr == addr && skate_render_tid) {  // SKATESTRAP_FPS: who posts work to the render thread (not the main pump)
            extern volatile DWORD skate_wk_tid[8]; extern volatile LONG skate_wk_n; extern volatile long long skate_wk_ns[8];
            LONG k = InterlockedIncrement(&skate_wk_n) & 7; skate_wk_tid[k] = GetCurrentThreadId(); skate_wk_ns[k] = now_ns(1);
            static volatile LONG shown; static DWORD first;
            if (!first) first = GetCurrentThreadId();  // shortcut: first waker seen is taken as the main pump
            if (GetCurrentThreadId() != first && InterlockedIncrement(&shown) <= 24) {
                printf("render waker tid %lu\n", GetCurrentThreadId());
                void lib_callers(const char *, uint64_t *); lib_callers("  wake at", __builtin_frame_address(0));
            }
        }
        if (val == 1) WakeByAddressSingle((void *)addr); else WakeByAddressAll((void *)addr);
        return 0;
    }
    if (cmd != 0 && cmd != 9) { printf("futex op %d not emulated\n", op); return E_NOSYS; }
    if (*addr != val) return E_AGAIN;
    DWORD ms = INFINITE;
    if (t) {
        int64_t ns = t->sec * 1000000000 + t->nsec;
        if (cmd == 9) ns -= now_ns((op & 256) ? 0 : 1);  // BITSET timeouts are absolute
        if (ns <= 0) return E_TIMEDOUT;
        // Whole ms on WaitOnAddress (rounded down), the rest in 200 us high-res slices, checking the word between.
        // shortcut: a wake that leaves the word unchanged is seen only at the deadline; fine for bionic (cond counter, mutex state change).
        int64_t end = now_ns(1) + ns, tw = now_ns(1);
        for (;;) {
            int64_t left = end - now_ns(1);
            if (*addr != val) { wait_trace("futex", ns / 1000000, tw, __builtin_frame_address(0)); return 0; }
            if (left <= 0) { wait_trace("futex", ns / 1000000, tw, __builtin_frame_address(0)); return E_TIMEDOUT; }
            if (left >= 2000000) { if (WaitOnAddress((void *)addr, &val, 4, (DWORD)(left / 1000000 - 1))) return 0; }
            else hr_sleep(left < 200000 ? left : 200000);
        }
    }
    extern volatile DWORD skate_render_tid;
    int64_t t0 = skate_render_tid == GetCurrentThreadId() ? now_ns(1) : 0;
    if (t0) {
        skate_render_addr = addr;
    }
    int64_t tw = now_ns(1);
    BOOL ok = WaitOnAddress((void *)addr, &val, 4, ms);
    if (ms != INFINITE) wait_trace("futex", ms, tw, __builtin_frame_address(0));
    if (t0) { extern volatile long long skate_rw_ns, skate_rw_n, skate_rw_to; skate_rw_ns += now_ns(1) - t0; skate_rw_n++; if (!ok) skate_rw_to++; }
    if (t0 && now_ns(1) - t0 > 30000000) {  // render thread parked > 30 ms: who waited, for what
        printf("render wait futex %lld ms (timeout %ld)\n", (long long)((now_ns(1) - t0) / 1000000), (long)ms);
        void lib_callers(const char *, uint64_t *); lib_callers("  at", __builtin_frame_address(0));
    }
    if (!ok) return GetLastError() == ERROR_TIMEOUT ? E_TIMEDOUT : E_INVAL;
    return 0;
}
// Linux long is 64-bit; Windows long is not, so every syscall argument is int64_t
static SYSV int64_t s_syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    switch (n) {
    case 202: {  // futex
        int r = futex((volatile uint32_t *)a, (int)b, (uint32_t)c, (const Timespec *)d);
        if (r) { thread_errno = r; return -1; }
        return 0;
    }
    case 186: return GetCurrentThreadId();  // gettid
    case 39: return GetCurrentProcessId();  // getpid
    case 318: {  // getrandom(buf, len, flags): the system RNG never blocks
        ULONG n = (ULONG)(b > 0x2000000 ? 0x2000000 : b);
        return BCryptGenRandom(0, (void *)a, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? (thread_errno = 5, -1) : (int64_t)n;
    }
    }
    printf("syscall %lld not emulated\n", (long long)n);
    thread_errno = E_NOSYS;
    return -1;
}
// Raw `syscall` instructions in libroblox (16 sites, loader.c routes them here). On Windows the instruction would enter
// the NT kernel as an unrelated call, so each is answered by the same shim its libc twin uses. Returns -errno like Linux.
int64_t skatestrap_linux_syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    typedef SYSV int64_t (*F3)(int64_t, int64_t, int64_t); typedef SYSV int64_t (*F4)(int64_t, int64_t, int64_t, int64_t);
    static volatile LONG shown; int fs_is_proc(int);
    int say = InterlockedIncrement(&shown) <= 60;  // debug: what the raw sites ask for
    const char *name = 0; int64_t r;
    thread_errno = 0;
    switch (n) {
    case 0: name = "read"; break;
    case 2: name = "open"; break;
    case 3: name = "close"; break;
    case 5: name = "fstat"; break;
    case 8: name = "lseek"; break;
    case 17: name = "pread64"; break;
    case 89: name = "readlink"; break;
    case 257:  // openat: only absolute paths or AT_FDCWD, which is all open() can answer
        if (b && (*(const char *)b == '/' || (int)a == -100)) { name = "open"; a = b; b = c; c = d; break; }
        if (say) printf("raw syscall openat(dirfd %d): relative, not emulated\n", (int)a);
        return -E_NOSYS;
    case 138: {  // fstatfs(fd, struct statfs *): 120 bytes, f_type first, f_bsize second, f_namelen at 72, f_frsize at 80
        // shortcut: only type and block sizes are filled; fill counts from statvfs if a reader needs them
        int64_t *o = (int64_t *)b; memset(o, 0, 120);
        o[0] = fs_is_proc((int)a) ? 0x9fa0 : 0xEF53; o[1] = 4096; o[9] = 255; o[10] = 4096;
        if (say) printf("raw syscall fstatfs(%d) -> type %llx\n", (int)a, (long long)o[0]);
        return 0;
    }
    default:
        r = s_syscall(n, a, b, c, d, e);  // futex, gettid, getpid, getrandom; prints the number when unknown
        return r < 0 && thread_errno ? -thread_errno : r;
    }
    void *f = shim_lookup(name);
    if (!f) return -E_NOSYS;
    r = n == 17 ? ((F4)f)(a, b, c, d) : ((F3)f)(a, b, c);
    if (r < 0 && thread_errno) r = -thread_errno;
    if (say) { if (n == 2 || n == 257 || n == 89) printf("raw syscall %s(%s) -> %lld\n", name, (const char *)a, (long long)r); else printf("raw syscall %s(%d) -> %lld\n", name, (int)a, (long long)r); }
    return r;
}

// --- pthread mutex: bionic's own 40-byte layout (16-bit state word, owner tid at +4), after Mocktail's
// bionic_abi_exports.cc (Apache-2.0). State: bits 0-1 lock, 2-12 recursion count, 13 shared, 14-15 type.
enum { M_LOCKED = 1, M_CONTENDED = 2, M_LOCKMASK = 3, M_COUNT = 0x1ffc, M_ONE = 4, M_TYPE = 0xc000, M_ERRCHK = 0x8000 };
typedef struct { volatile uint16_t state; uint16_t pad; volatile int32_t owner; } Mutex;
static int32_t tid(void) { return (int32_t)GetCurrentThreadId(); }
static SYSV int s_mutex_init(Mutex *m, const int32_t *attr) {
    memset(m, 0, 40);
    if (attr) m->state = (uint16_t)((*attr & 3) << 14);
    return 0;
}
static SYSV int s_mutex_trylock(Mutex *m) {
    uint16_t old = m->state, type = old & M_TYPE, shared = old & 0x2000;
    if (type && m->owner == tid()) {
        if (type == M_ERRCHK) return E_BUSY;
        if ((old & M_COUNT) == M_COUNT) return E_AGAIN;
        __atomic_fetch_add(&m->state, M_ONE, __ATOMIC_RELAXED);
        return 0;
    }
    uint16_t want = type | shared;
    if (!__atomic_compare_exchange_n(&m->state, &want, (uint16_t)(type | shared | M_LOCKED), 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return E_BUSY;
    if (type) m->owner = tid();
    return 0;
}
static SYSV int s_mutex_lock(Mutex *m) {
    uint16_t type = m->state & M_TYPE, shared = m->state & 0x2000;
    if (type == M_ERRCHK && m->owner == tid()) return E_DEADLK;
    int r = s_mutex_trylock(m);
    if (r != E_BUSY) return r;
    uint16_t unlocked = type | shared, contended = type | shared | M_CONTENDED;
    for (;;) {  // park on the guest word, never spin (a spinning locker steals a game core)
        // CAS, not exchange: a held recursive mutex keeps its owner's count in the state; overwriting it made the
        // owner's next inner unlock release the lock outright (mutual exclusion lost, then deadlock). As bionic does.
        uint16_t old = m->state;
        if (!(old & M_LOCKMASK)) { if (__atomic_compare_exchange_n(&m->state, &old, contended, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break; continue; }
        uint16_t c = (uint16_t)((old & ~M_LOCKMASK) | M_CONTENDED);
        if (old != c && !__atomic_compare_exchange_n(&m->state, &old, c, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        futex((volatile uint32_t *)m, 0, (*(volatile uint32_t *)m & 0xffff0000u) | c, 0);
    }
    if (type) m->owner = tid();
    return 0;
}
static SYSV int s_mutex_unlock(Mutex *m) {
    uint16_t old = m->state, type = old & M_TYPE, shared = old & 0x2000;
    if (type) {
        if (m->owner != tid()) return E_PERM;
        if (old & M_COUNT) { __atomic_fetch_sub(&m->state, M_ONE, __ATOMIC_RELAXED); return 0; }
        m->owner = 0;
    }
    old = __atomic_exchange_n(&m->state, (uint16_t)(type | shared), __ATOMIC_RELEASE);
    if ((old & M_LOCKMASK) == M_CONTENDED) futex((volatile uint32_t *)m, 1, 1, 0);
    return 0;
}
static SYSV int s_mutex_destroy(Mutex *m) { return 0; }
static SYSV int s_mutexattr_init(int32_t *a) { *a = 0; return 0; }
static SYSV int s_mutexattr_settype(int32_t *a, int type) { *a = type; return 0; }
static SYSV int s_mutexattr_destroy(int32_t *a) { return 0; }

// --- pthread cond: bionic's generation counter (+4 per pulse; bit 0 shared, bit 1 = monotonic clock) ------
static SYSV int s_condattr_init(uint32_t *a) { *a = 0; return 0; }
static SYSV int s_condattr_destroy(uint32_t *a) { return 0; }
static SYSV int s_condattr_setclock(uint32_t *a, int clock) { *a = (*a & ~2u) | ((uint32_t)clock << 1 & 2u); return 0; }
static SYSV int s_cond_init(uint32_t *c, const uint32_t *a) { memset(c, 0, 48); *c = a ? *a & 3 : 0; return 0; }
static SYSV int s_cond_destroy(uint32_t *c) { return 0; }
static SYSV int s_cond_signal(volatile uint32_t *c) { __atomic_fetch_add(c, 4, __ATOMIC_RELAXED); futex(c, 1, 1, 0); return 0; }
static SYSV int s_cond_broadcast(volatile uint32_t *c) { __atomic_fetch_add(c, 4, __ATOMIC_RELAXED); futex(c, 1, 0x7fffffff, 0); return 0; }
static int cond_wait(volatile uint32_t *c, Mutex *m, const Timespec *abstime) {
    uint32_t seen = *c;
    int r = s_mutex_unlock(m);
    if (r) return r;
    // bit 1 clear = CLOCK_REALTIME (256 = FUTEX_CLOCK_REALTIME); WAIT_BITSET takes an absolute time
    r = futex(c, abstime ? 9 | ((seen & 2) ? 0 : 256) : 0, seen, abstime);
    int lr = s_mutex_lock(m);
    if (lr) return lr;
    return r == E_TIMEDOUT ? E_TIMEDOUT : 0;
}
static SYSV int s_cond_wait(volatile uint32_t *c, Mutex *m) { return cond_wait(c, m, 0); }
static SYSV int s_cond_timedwait(volatile uint32_t *c, Mutex *m, const Timespec *t) { return t ? cond_wait(c, m, t) : E_INVAL; }

typedef SYSV void (*OnceFn)(void);
static SYSV int s_once(volatile LONG *once, OnceFn f) {
    if (*once == 2) return 0;
    if (InterlockedCompareExchange(once, 1, 0) == 0) { f(); InterlockedExchange(once, 2); return 0; }
    while (*once != 2) SwitchToThread();
    return 0;
}
void *shim_key_dtor[1088];  // per TLS index; threads.c runs them when a thread it started exits
static SYSV int s_key_create(uint32_t *key, void *destructor) {  // shortcut: main-thread key destructors never run
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES || k >= 1088) return 11;
    shim_key_dtor[k] = destructor;
    *key = k;
    return 0;
}
static SYSV int s_key_delete(uint32_t key) { TlsFree(key); return 0; }
static SYSV void *s_getspecific(uint32_t key) { return TlsGetValue(key); }
static SYSV int s_setspecific(uint32_t key, const void *v) { TlsSetValue(key, (void *)v); return 0; }

// --- liblog ---------------------------------------------------------------------------------------------
static SYSV int s_log_write(int prio, const char *tag, const char *text) { printf("[log %d %s] %s\n", prio, tag ? tag : "", text); return 1; }
static SYSV int s_log_print(int prio, const char *tag, const char *fmt, ...) {  // shortcut: format not expanded (SysV varargs)
    printf("[log %d %s] %s\n", prio, tag ? tag : "", fmt);
    return 1;
}

static const struct { const char *name; void *at; } table[] = {
    { "malloc", s_malloc }, { "free", s_free }, { "calloc", s_calloc }, { "realloc", s_realloc },
    { "memalign", s_memalign }, { "posix_memalign", s_posix_memalign }, { "malloc_usable_size", s_malloc_usable_size },
    { "memcpy", s_memcpy }, { "memmove", s_memmove }, { "memset", s_memset }, { "memcmp", s_memcmp }, { "memchr", s_memchr },
    { "strlen", s_strlen }, { "strcmp", s_strcmp }, { "strncmp", s_strncmp }, { "strcpy", s_strcpy }, { "strncpy", s_strncpy },
    { "strchr", s_strchr }, { "strrchr", s_strrchr }, { "strstr", s_strstr }, { "strdup", s_strdup },
    { "__errno", s_errno }, { "__cxa_atexit", s_cxa_atexit }, { "__cxa_finalize", s_cxa_finalize },
    { "__stack_chk_fail", s_stack_chk_fail }, { "abort", s_abort }, { "getenv", s_getenv },
    { "pthread_mutex_init", s_mutex_init }, { "pthread_mutex_lock", s_mutex_lock }, { "pthread_mutex_trylock", s_mutex_trylock },
    { "pthread_mutex_unlock", s_mutex_unlock }, { "pthread_mutex_destroy", s_mutex_destroy },
    { "pthread_mutexattr_init", s_mutexattr_init }, { "pthread_mutexattr_settype", s_mutexattr_settype },
    { "pthread_mutexattr_destroy", s_mutexattr_destroy }, { "pthread_once", s_once },
    { "pthread_key_create", s_key_create }, { "pthread_key_delete", s_key_delete },
    { "pthread_getspecific", s_getspecific }, { "pthread_setspecific", s_setspecific },
    { "pthread_cond_init", s_cond_init }, { "pthread_cond_destroy", s_cond_destroy }, { "pthread_cond_signal", s_cond_signal },
    { "pthread_cond_broadcast", s_cond_broadcast }, { "pthread_cond_wait", s_cond_wait }, { "pthread_cond_timedwait", s_cond_timedwait },
    { "pthread_condattr_init", s_condattr_init }, { "pthread_condattr_destroy", s_condattr_destroy },
    { "pthread_condattr_setclock", s_condattr_setclock }, { "syscall", s_syscall }, { "clock_gettime", s_clock_gettime },
    { "__android_log_write", s_log_write }
};

void *shim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].at;
    if (!strcmp(name, "__stack_chk_guard")) return &skatestrap_tcb[5];
    void *r = shim_lookup_fmt(name);
    if (!r) r = shim_lookup_egl_hand(name);
    if (!r) r = shim_lookup_threads(name);
    if (!r) r = shim_lookup_dl(name);
    if (!r) r = shim_lookup_fs(name);
    if (!r) r = shim_lookup_tm(name);
    if (!r) r = shim_lookup_asset(name);
    if (!r) r = shim_lookup_net(name);
    if (!r) r = shim_lookup_vk(name);
    if (!r) r = shim_lookup_gl(name);
    return r ? r : shim_lookup_bulk(name);
}
