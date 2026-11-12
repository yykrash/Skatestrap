// Threads for libroblox.so: pthread_create/join/attr, rwlocks, semaphores, scheduling, thread exit hooks, exit.
// pthread_t is the Windows thread id (matches pthread_self in shim_bulk.c). Layouts follow bionic x86_64.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "shim.h"

int *shim_errno(void);
extern void *shim_key_dtor[1088];
void sv_setjmp(void), sv_longjmp(void);  // setjmp.S
enum { E_PERM = 1, E_SRCH = 3, E_AGAIN = 11, E_INVAL = 22 };

// bionic pthread_attr_t (56 bytes)
typedef struct { uint32_t flags; uint32_t pad; uint64_t stack_base, stack_size, guard_size; int32_t policy, priority; char reserved[16]; } Attr;
#define ATTR_DETACHED 1

typedef SYSV void *(*StartFn)(void *);
typedef struct { DWORD tid; HANDLE h; void *ret; uint64_t lo, hi; int detached; } Slot;
static Slot slots[4096];  // shortcut: fixed table, 4096 live threads max; Roblox runs well under 100
static SRWLOCK slots_lock = SRWLOCK_INIT;

static Slot *find(DWORD tid) {
    for (int i = 0; i < 4096; i++) if (slots[i].tid == tid) return &slots[i];
    return 0;
}
static void stack_of_self(uint64_t *lo, uint64_t *hi) { ULONG_PTR l, h; GetCurrentThreadStackLimits(&l, &h); *lo = l; *hi = h; }

// thread_local destructors (__cxa_thread_atexit_impl), newest first, then pthread key destructors
typedef SYSV void (*DtorFn)(void *);
typedef struct { DtorFn fn; void *obj; } Dtor;
static __thread Dtor *tl_dtors; static __thread int tl_count, tl_cap;
static SYSV int s_cxa_thread_atexit_impl(DtorFn fn, void *obj, void *dso) {
    if (tl_count == tl_cap) {
        int cap = tl_cap ? tl_cap * 2 : 16;
        Dtor *n = tl_dtors ? HeapReAlloc(GetProcessHeap(), 0, tl_dtors, cap * sizeof *n) : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *n);
        if (!n) return -1;
        tl_dtors = n; tl_cap = cap;
    }
    tl_dtors[tl_count++] = (Dtor){ fn, obj };
    return 0;
}
static void thread_exiting(void *ret) {
    while (tl_count) { Dtor d = tl_dtors[--tl_count]; d.fn(d.obj); }
    for (int round = 0; round < 4; round++) {  // PTHREAD_DESTRUCTOR_ITERATIONS
        int again = 0;
        for (int k = 0; k < 1088; k++) {
            if (!shim_key_dtor[k]) continue;
            void *v = TlsGetValue(k);
            if (v) { TlsSetValue(k, 0); ((DtorFn)shim_key_dtor[k])(v); again = 1; }
        }
        if (!again) break;
    }
    AcquireSRWLockExclusive(&slots_lock);
    Slot *s = find(GetCurrentThreadId());
    if (s) { s->ret = ret; if (s->detached) { CloseHandle(s->h); memset(s, 0, sizeof *s); } }
    ReleaseSRWLockExclusive(&slots_lock);
}

typedef struct { StartFn fn; void *arg; HANDLE go; } Boot;
static DWORD WINAPI thread_main(void *p) {
    Boot b = *(Boot *)p;
    WaitForSingleObject(b.go, INFINITE);  // the slot is filled in before the thread body runs
    CloseHandle(b.go);
    HeapFree(GetProcessHeap(), 0, p);
    AcquireSRWLockExclusive(&slots_lock);
    Slot *s = find(GetCurrentThreadId());
    if (s) stack_of_self(&s->lo, &s->hi);
    ReleaseSRWLockExclusive(&slots_lock);
    thread_exiting(b.fn(b.arg));
    return 0;
}
static SYSV int s_pthread_create(uint64_t *out, const Attr *a, StartFn fn, void *arg) {
    // shortcut: a caller-supplied stack (attr stack_base) is ignored; Windows makes the stack, the size is honoured
    SIZE_T size = a && a->stack_size ? a->stack_size : 1 << 20;
    Boot *b = HeapAlloc(GetProcessHeap(), 0, sizeof *b);
    if (!b) return E_AGAIN;
    b->fn = fn; b->arg = arg; b->go = CreateEventA(0, TRUE, FALSE, 0);
    DWORD tid; HANDLE h = CreateThread(0, size, thread_main, b, 0, &tid);  // committed: SysV code has no stack probes, so it can jump past the guard page
    if (!h) { CloseHandle(b->go); HeapFree(GetProcessHeap(), 0, b); return E_AGAIN; }
    AcquireSRWLockExclusive(&slots_lock);
    Slot *s = find(0);
    if (s) *s = (Slot){ tid, h, 0, 0, 0, a && (a->flags & ATTR_DETACHED) };
    ReleaseSRWLockExclusive(&slots_lock);
    if (!s) printf("pthread_create: thread table full\n");
    *out = tid;
    SetEvent(b->go);
    return 0;
}
static SYSV int s_pthread_join(uint64_t t, void **ret) {
    AcquireSRWLockShared(&slots_lock); Slot *s = find((DWORD)t); HANDLE h = s ? s->h : 0; ReleaseSRWLockShared(&slots_lock);
    if (!h) return E_SRCH;
    WaitForSingleObject(h, INFINITE);
    AcquireSRWLockExclusive(&slots_lock);
    if (ret) *ret = s->ret;
    CloseHandle(s->h); memset(s, 0, sizeof *s);
    ReleaseSRWLockExclusive(&slots_lock);
    return 0;
}
static SYSV int s_pthread_detach(uint64_t t) {
    AcquireSRWLockExclusive(&slots_lock);
    Slot *s = find((DWORD)t); int r = s ? 0 : E_SRCH;
    if (s) s->detached = 1;
    ReleaseSRWLockExclusive(&slots_lock);
    return r;
}
static SYSV void s_pthread_exit(void *ret) { thread_exiting(ret); ExitThread(0); }

static SYSV int s_attr_init(Attr *a) { memset(a, 0, sizeof *a); a->stack_size = 1 << 20; a->guard_size = 4096; return 0; }
static SYSV int s_attr_destroy(Attr *a) { return 0; }
static SYSV int s_attr_setdetachstate(Attr *a, int d) { if (d) a->flags |= ATTR_DETACHED; else a->flags &= ~ATTR_DETACHED; return 0; }
static SYSV int s_attr_setstacksize(Attr *a, uint64_t n) { if (n < 16384) return E_INVAL; a->stack_size = n; return 0; }
static SYSV int s_attr_setschedparam(Attr *a, const int *prio) { a->priority = *prio; return 0; }
static SYSV int s_attr_getstack(const Attr *a, void **base, uint64_t *size) { *base = (void *)a->stack_base; *size = a->stack_size; return 0; }
static SYSV int s_getattr_np(uint64_t t, Attr *a) {
    s_attr_init(a);
    uint64_t lo = 0, hi = 0;
    if ((DWORD)t == GetCurrentThreadId()) stack_of_self(&lo, &hi);
    else {
        AcquireSRWLockShared(&slots_lock);
        Slot *s = find((DWORD)t);
        if (s) { lo = s->lo; hi = s->hi; a->flags = s->detached ? ATTR_DETACHED : 0; }
        ReleaseSRWLockShared(&slots_lock);
        if (!s) return E_SRCH;
    }
    a->stack_base = lo; a->stack_size = hi - lo;
    return 0;
}

// Scheduling: no SCHED_FIFO/RR for us; report SCHED_OTHER honestly and refuse realtime.
static SYSV int s_getschedparam(uint64_t t, int *policy, int *prio) { *policy = 0; *prio = 0; return 0; }
static SYSV int s_setschedparam(uint64_t t, int policy, const int *prio) { return policy == 0 ? 0 : E_PERM; }
static SYSV int s_prio_max(int policy) { return policy == 1 || policy == 2 ? 99 : 0; }
static SYSV int s_prio_min(int policy) { return policy == 1 || policy == 2 ? 1 : 0; }
static SYSV int s_sched_getparam(int pid, int *prio) { *prio = 0; return 0; }
static SYSV int s_sched_getscheduler(int pid) { return 0; }
static SYSV int s_sched_setscheduler(int pid, int policy, const int *prio) { if (policy == 0) return 0; *shim_errno() = E_PERM; return -1; }
static SYSV int s_getpriority(int which, int who) { *shim_errno() = 0; return 0; }
// Roblox gives its threads Linux nice values (who = tid, 0 = self). Map them to Windows thread priorities so the
// game's own urgent threads win a busy 4-core laptop. OFF unless SKATESTRAP_PRIO=1: runs pon/poff (12 Nov, 8 threads)
// had it worse (226 vs 239 fps, steady gaps 26-42 vs 8-11 ms): Roblox raises 12 threads and none are lowered.
static SYSV int s_setpriority(int which, int who, int prio) {
    static int off = -1; if (off < 0) { const char *e = getenv("SKATESTRAP_PRIO"); off = !(e && !strcmp(e, "1")); }
    if (off || which != 0) return 0;  // PRIO_PROCESS only (a tid on Linux)
    int w = prio <= -8 ? THREAD_PRIORITY_HIGHEST : prio < 0 ? THREAD_PRIORITY_ABOVE_NORMAL : prio == 0 ? THREAD_PRIORITY_NORMAL
          : prio < 10 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    DWORD tid = who ? (DWORD)who : GetCurrentThreadId();
    HANDLE h = tid == GetCurrentThreadId() ? GetCurrentThread() : OpenThread(THREAD_SET_LIMITED_INFORMATION, 0, tid);
    int ok = h && SetThreadPriority(h, w);
    if (h && h != GetCurrentThread()) CloseHandle(h);
    printf("[prio] thread %lu nice %d -> %d%s\n", tid, prio, w, ok ? "" : " (failed)");
    return 0;
}
static SYSV int s_sigmask(int how, const void *set, void *old) { if (old) memset(old, 0, 8); return 0; }  // no signals on this side
// Handlers are kept and handed back, never run. shortcut: no signal is delivered (crashes go to loader's handler); route SEH faults to them if Roblox's crash reporting is wanted
static uint8_t sig_act[65][32];  // bionic x86_64 struct sigaction is 32 bytes; kept opaque
static SRWLOCK sig_lock = SRWLOCK_INIT;
static SYSV int s_sigaction(int sig, const void *act, void *old) {
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19) { *shim_errno() = 22; return -1; }  // EINVAL; SIGKILL/SIGSTOP can't be caught
    AcquireSRWLockExclusive(&sig_lock);
    if (old) memcpy(old, sig_act[sig], 32);
    if (act) memcpy(sig_act[sig], act, 32);
    ReleaseSRWLockExclusive(&sig_lock);
    return 0;
}
static SYSV void *s_signal(int sig, void *h) {
    uint8_t act[32] = { 0 }, old[32];  // sa_handler is the first field
    memcpy(act, &h, 8);
    if (s_sigaction(sig, act, old)) return (void *)-1;  // SIG_ERR
    void *r; memcpy(&r, old, 8); return r;
}
static SYSV int s_sigaltstack(const void *ss, void *old) { if (old) { memset(old, 0, 24); ((int *)old)[2] = 2; } return 0; }  // SS_DISABLE
static SYSV int s_sigemptyset(uint64_t *s) { *s = 0; return 0; }
static SYSV int s_sigfillset(uint64_t *s) { *s = ~0ull; return 0; }
static SYSV int s_raise(int sig) { printf("  raise(%d): stopping\n", sig); fflush(stdout); TerminateProcess(GetCurrentProcess(), 128 + sig); return 0; }
static SYSV int s_register_atfork(void *a, void *b, void *c, void *dso) { return 0; }  // fork never happens

// rwlock: an SRWLOCK in the first 8 bytes of bionic's 56-byte pthread_rwlock_t (zero init on both sides),
// writer thread id at +8 so unlock knows which side it holds.
typedef struct { SRWLOCK l; volatile DWORD writer; } Rw;
static SYSV int s_rw_init(Rw *r, const void *attr) { memset(r, 0, 56); return 0; }
static SYSV int s_rw_destroy(Rw *r) { return 0; }
static SYSV int s_rw_rdlock(Rw *r) { AcquireSRWLockShared(&r->l); return 0; }
static SYSV int s_rw_wrlock(Rw *r) { AcquireSRWLockExclusive(&r->l); r->writer = GetCurrentThreadId(); return 0; }
static SYSV int s_rw_unlock(Rw *r) {
    if (r->writer == GetCurrentThreadId()) { r->writer = 0; ReleaseSRWLockExclusive(&r->l); }
    else ReleaseSRWLockShared(&r->l);
    return 0;
}

// sem_t: bionic's first word is the count (16 bytes total)
static SYSV int s_sem_init(volatile LONG *s, int shared, unsigned v) { s[0] = v; return 0; }
static SYSV int s_sem_destroy(volatile LONG *s) { return 0; }
static SYSV int s_sem_post(volatile LONG *s) { InterlockedIncrement(s); WakeByAddressSingle((void *)s); return 0; }
static SYSV int s_sem_wait(volatile LONG *s) {
    for (;;) {
        LONG v = *s;
        if (v > 0) { if (InterlockedCompareExchange(s, v - 1, v) == v) return 0; continue; }
        LONG zero = 0; WaitOnAddress(s, &zero, 4, INFINITE);
    }
}

static SYSV void s_exit(int code) { fflush(0); ExitProcess(code); }  // shortcut: __cxa_atexit handlers not run
static SYSV void s__exit(int code) { ExitProcess(code); }

static const struct { const char *name; void *at; } table[] = {
    { "pthread_create", s_pthread_create }, { "pthread_join", s_pthread_join }, { "pthread_detach", s_pthread_detach },
    { "pthread_exit", s_pthread_exit }, { "pthread_attr_init", s_attr_init }, { "pthread_attr_destroy", s_attr_destroy },
    { "pthread_attr_setdetachstate", s_attr_setdetachstate }, { "pthread_attr_setstacksize", s_attr_setstacksize },
    { "pthread_attr_setschedparam", s_attr_setschedparam }, { "pthread_attr_getstack", s_attr_getstack },
    { "pthread_getattr_np", s_getattr_np }, { "pthread_getschedparam", s_getschedparam }, { "pthread_setschedparam", s_setschedparam },
    { "sched_get_priority_max", s_prio_max }, { "sched_get_priority_min", s_prio_min }, { "sched_getparam", s_sched_getparam },
    { "sched_getscheduler", s_sched_getscheduler }, { "sched_setscheduler", s_sched_setscheduler },
    { "getpriority", s_getpriority }, { "setpriority", s_setpriority }, { "pthread_sigmask", s_sigmask }, { "sigaction", s_sigaction }, { "signal", s_signal }, { "sigaltstack", s_sigaltstack },
    { "sigemptyset", s_sigemptyset }, { "sigfillset", s_sigfillset }, { "raise", s_raise },
    { "__register_atfork", s_register_atfork }, { "__cxa_thread_atexit_impl", s_cxa_thread_atexit_impl },
    { "pthread_rwlock_init", s_rw_init }, { "pthread_rwlock_destroy", s_rw_destroy }, { "pthread_rwlock_rdlock", s_rw_rdlock },
    { "pthread_rwlock_wrlock", s_rw_wrlock }, { "pthread_rwlock_unlock", s_rw_unlock },
    { "sem_init", s_sem_init }, { "sem_destroy", s_sem_destroy }, { "sem_post", s_sem_post }, { "sem_wait", s_sem_wait },
    { "exit", s_exit }, { "_exit", s__exit }, { "_Exit", s__exit }, { "setjmp", sv_setjmp }, { "longjmp", sv_longjmp },
};
void *shim_lookup_threads(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}
