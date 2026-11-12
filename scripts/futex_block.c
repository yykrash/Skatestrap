// --- futex + clocks: libroblox calls syscall(SYS_futex) on its own cond words (Mocktail notes the same),
// so every lock below lives in the guest object and parks through this one futex. ---------------------------
typedef struct { int64_t sec, nsec; } Timespec;  // Linux x86-64 struct timespec
enum { E_PERM = 1, E_AGAIN = 11, E_BUSY = 16, E_INVAL = 22, E_DEADLK = 35, E_NOSYS = 38, E_TIMEDOUT = 110 };
static int64_t now_ns(int clock) {  // 0 = CLOCK_REALTIME, anything else monotonic
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
static SYSV int s_clock_gettime(int clock, Timespec *t) {
    int64_t n = now_ns(clock);
    t->sec = n / 1000000000; t->nsec = n % 1000000000;
    return 0;
}
// Returns 0 or a positive errno. WAIT/WAIT_BITSET park while *addr == val; WAKE/WAKE_BITSET wake waiters.
static int futex(volatile uint32_t *addr, int op, uint32_t val, const Timespec *t) {
    int cmd = op & 0x7f;
    if (cmd == 1 || cmd == 10) {  // FUTEX_WAKE, FUTEX_WAKE_BITSET
        if (val == 1) WakeByAddressSingle((void *)addr); else WakeByAddressAll((void *)addr);
        return 0;
    }
    if (cmd != 0 && cmd != 9) { printf("futex op %d not emulated\n", op); return E_NOSYS; }
    if (*addr != val) return E_AGAIN;
    DWORD ms = INFINITE;
    if (t) {
        int64_t ns = t->sec * 1000000000 + t->nsec;
        if (cmd == 9) ns -= now_ns((op & 256) ? 0 : 1);  // BITSET timeouts are absolute
        ms = ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000);
    }
    if (!WaitOnAddress((void *)addr, &val, 4, ms)) return GetLastError() == ERROR_TIMEOUT ? E_TIMEDOUT : E_INVAL;
    return 0;
}
static __declspec(thread) int thread_errno;
static SYSV long s_syscall(long n, long a, long b, long c, long d, long e) {
    switch (n) {
    case 202: {  // futex
        int r = futex((volatile uint32_t *)a, (int)b, (uint32_t)c, (const Timespec *)d);
        if (r) { thread_errno = r; return -1; }
        return 0;
    }
    case 186: return GetCurrentThreadId();  // gettid
    case 39: return GetCurrentProcessId();  // getpid
    }
    printf("syscall %ld not emulated\n", n);
    thread_errno = E_NOSYS;
    return -1;
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
        uint16_t old = __atomic_exchange_n(&m->state, contended, __ATOMIC_ACQUIRE);
        if (old == unlocked) break;
        futex((volatile uint32_t *)m, 0, (*(volatile uint32_t *)m & 0xffff0000u) | contended, 0);
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
    if ((old & M_LOCKMASK) == M_CONTENDED) WakeByAddressSingle((void *)m);
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
static SYSV int s_cond_signal(volatile uint32_t *c) { __atomic_fetch_add(c, 4, __ATOMIC_RELAXED); WakeByAddressSingle((void *)c); return 0; }
static SYSV int s_cond_broadcast(volatile uint32_t *c) { __atomic_fetch_add(c, 4, __ATOMIC_RELAXED); WakeByAddressAll((void *)c); return 0; }
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

