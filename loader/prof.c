// SKATESTRAP_PROF=1: sampling profiler. Every ~1 ms each thread is paused and its rip recorded; at exit
// prof_report prints per-thread busy/wait split and the hottest libroblox / loader addresses.
// Debug only. Read-only on the target threads (no locks taken while one is paused).
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAXT 256
#define HOT 65536
typedef struct { DWORD id; HANDLE h; uint32_t n, wait, lib, ldr, other; long long lp; uint64_t orip; } T;
static T th[MAXT]; static int nth;
static uint8_t *lib; static uint64_t libspan;
static uint64_t ldr0, ldr1, nt0, nt1, kb0, kb1;
static struct { uint64_t rip; uint32_t n; } hot[HOT];
static volatile LONG stop;
extern volatile DWORD skate_render_tid;
static struct { uint64_t rip; uint32_t n; } rhot[4096]; static uint32_t rn, rwait, rother;
static void rcount(uint64_t rip) {
    uint32_t i = (uint32_t)((rip >> 2) * 2654435761u) & 4095;
    for (int j = 0; j < 64; j++, i = (i + 1) & 4095) {
        if (rhot[i].rip == rip) { rhot[i].n++; return; }
        if (!rhot[i].rip) { rhot[i].rip = rip; rhot[i].n = 1; return; }
    }
}
static struct { uint64_t base; uint32_t n; } rmod[32];  // render-thread samples outside our code, by module
static void modcount(uint64_t r) {
    HMODULE hm; if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)r, &hm)) hm = 0;
    for (int i = 0; i < 32; i++) {
        if (rmod[i].base == (uint64_t)hm) { rmod[i].n++; return; }
        if (!rmod[i].n) { rmod[i].base = (uint64_t)hm; rmod[i].n = 1; return; }
    }
}
// F8 (window focused or not): forget everything so far, so the report covers one screen
static void reset(void) {
    for (int k = 0; k < nth; k++) { th[k].n = th[k].wait = th[k].lib = th[k].ldr = th[k].other = 0; th[k].orip = 0; }
    memset(hot, 0, sizeof hot); memset(rhot, 0, sizeof rhot); memset(rmod, 0, sizeof rmod); rn = rwait = rother = 0;
    printf("prof: F8 reset, counting from now\n"); fflush(stdout);
}
static int ours(uint64_t r) { return r - (uint64_t)lib < libspan || (r >= ldr0 && r < ldr1); }

static void mod_range(const wchar_t *name, uint64_t *a, uint64_t *b) {
    MODULEINFO mi; HMODULE m = GetModuleHandleW(name);
    if (m && GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) { *a = (uint64_t)mi.lpBaseOfDll; *b = *a + mi.SizeOfImage; }
}
static void refresh(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 e = { sizeof e };
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(s, &e); ok; ok = Thread32Next(s, &e)) {
        if (e.th32OwnerProcessID != pid || e.th32ThreadID == me) continue;
        int k = 0; while (k < nth && th[k].id != e.th32ThreadID) k++;
        if (k == nth && nth < MAXT) { th[nth].id = e.th32ThreadID; th[nth].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, 0, e.th32ThreadID); nth++; }
    }
    CloseHandle(s);
}
static void count(uint64_t rip) {
    uint32_t i = (uint32_t)((rip >> 2) * 2654435761u) & (HOT - 1);
    for (int j = 0; j < 64; j++, i = (i + 1) & (HOT - 1)) {
        if (hot[i].rip == rip) { hot[i].n++; return; }
        if (!hot[i].rip) { hot[i].rip = rip; hot[i].n = 1; return; }
    }
}
static DWORD WINAPI sampler(void *x) {
    timeBeginPeriod(1);
    for (int tick = 0; !stop; tick++) {
        if (tick % 500 == 0) refresh();
        if (GetAsyncKeyState(VK_F8) & 1) reset();
        // SKATESTRAP_PROF=render: stall stacks of the render thread only (others not suspended, so fps stays near normal)
        static int ronly = -1; if (ronly < 0) ronly = getenv("SKATESTRAP_PROF")[0] == 'r';
        extern volatile DWORD skate_render_tid;
        for (int k = 0; k < nth; k++) {
            // SKATESTRAP_PROF_THREAD=Main: that named thread too (exact pthread name, leading spaces kept)
            const char *skate_tname(DWORD); static const char *pt; if (!pt) { pt = getenv("SKATESTRAP_PROF_THREAD"); if (!pt) pt = ""; }
            if (ronly && th[k].id != skate_render_tid && !(*pt && !strcmp(skate_tname(th[k].id), pt))) continue;
            if (!th[k].h || SuspendThread(th[k].h) == (DWORD)-1) continue;
            CONTEXT c = { .ContextFlags = CONTEXT_CONTROL };
            BOOL ok = GetThreadContext(th[k].h, &c);
            ResumeThread(th[k].h);
            if (!ok) continue;
            uint64_t r = c.Rip; T *t = &th[k]; t->n++;
            extern volatile DWORD skate_poster_tid; extern volatile long long skate_last_frame_ns; int64_t now_ns(int);
            // SKATESTRAP_PROF=poster: profile the thread that posts frames, only while no frame came for 60 ms
            static int all = -1; if (all < 0) all = getenv("SKATESTRAP_PROF")[0] == 'a';  // =all: every thread's stack during a stall
            static ULONGLONG t0; if (!t0) t0 = GetTickCount64();
            // SKATESTRAP_PROF_STALL_MS (60): no frame for this long = a stall; SKATESTRAP_PROF_EVERY_MS (25): gap between a thread's stall stacks
            static long long stall_ns, every_ns; if (!stall_ns) { const char *e = getenv("SKATESTRAP_PROF_STALL_MS"), *g = getenv("SKATESTRAP_PROF_EVERY_MS");
                stall_ns = (e ? atoi(e) : 60) * 1000000ll; every_ns = (g ? atoi(g) : 25) * 1000000ll; }
            int poster = (all ? skate_last_frame_ns != 0 && GetTickCount64() - t0 > (ULONGLONG)(getenv("SKATESTRAP_PROF_SKIP_MS") ? atoi(getenv("SKATESTRAP_PROF_SKIP_MS")) : 20000) :  // skip the loading stalls
                ronly ? skate_last_frame_ns && GetTickCount64() - t0 > 60000 : skate_poster_tid && th[k].id == skate_poster_tid) && now_ns(1) - skate_last_frame_ns > stall_ns;
            if (poster) {  // call stack of the stalled poster: return addresses that follow a call, top 14
                static int shown; uint64_t fr[14]; int nf = 0, nex = 0; long long tn = now_ns(1);
                // past the cap: 1/s, so a late freeze still shows
                if ((shown < (all || ronly ? 200000 : 120) ? tn - t->lp > every_ns : tn - t->lp > 1000000000) && SuspendThread(th[k].h) != (DWORD)-1) {
                    CONTEXT c2 = { .ContextFlags = CONTEXT_FULL };
                    if (GetThreadContext(th[k].h, &c2)) {
                        fr[nf++] = c2.Rip;
                        // exact frames first: Windows DLLs and the loader have unwind tables, so walk them for real down to
                        // the first libroblox return address. Only libroblox (no PE unwind data) is guessed by the scan below,
                        // which can pick up stale return addresses (2026-11-11: two "hot" libroblox frames were stale).
                        while (nf < 14 && c2.Rip - (uint64_t)lib >= libspan) {
                            DWORD64 base; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c2.Rip, &base, 0);
                            if (!fe) break;  // JIT code or unknown: fall back to the scan
                            void *hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c2.Rip, fe, &c2, &hd, &ef, 0);
                            if (!c2.Rip) break;
                            fr[nf++] = c2.Rip;
                        }
                        nex = nf;
                        uint64_t *sp = (uint64_t *)c2.Rsp; MEMORY_BASIC_INFORMATION mi; int lim = 0;
                        if (VirtualQuery(sp, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) lim = (int)(((uint64_t)mi.BaseAddress + mi.RegionSize - (uint64_t)sp) / 8);
                        for (int q = 0; q < 4096 && q < lim && nf < 14; q++) {
                            uint64_t v = sp[q]; uint8_t *cc = (uint8_t *)v;
                            if (v >= ldr0 + 0x1000 && v < ldr1) { if (cc[-5] == 0xe8) fr[nf++] = v; }
                            else if (v - (uint64_t)lib < libspan && v - (uint64_t)lib > 8 && (cc[-5] == 0xe8 || (cc[-2] == 0xff && (cc[-1] & 0x38) == 0x10) || (cc[-3] == 0xff && (cc[-2] & 0x38) == 0x10) || (cc[-6] == 0xff && (cc[-5] & 0x38) == 0x10))) fr[nf++] = v;
                        }
                    }
                    ResumeThread(th[k].h);
                    shown++; t->lp = tn;
                    const char *skate_tname(DWORD);
                    printf("stall %lld ms t%lu(%s) @%lld:", (tn - skate_last_frame_ns) / 1000000, th[k].id, skate_tname(th[k].id), tn / 1000000);
                    for (int q = 0; q < nf; q++) { uint64_t v = fr[q]; if (q && q == nex) printf(" |"); if (v - (uint64_t)lib < libspan) printf(" %llx", (unsigned long long)(v - (uint64_t)lib)); else if (v >= ldr0 && v < ldr1) printf(" L%llx", (unsigned long long)(v - ldr0)); else printf(" win"); }
                    printf("\n");
                }
            } else if (!skate_poster_tid && th[k].id == skate_render_tid) {
                rn++;
                if (!ours(r)) modcount(r);
                if (ours(r)) rcount(r);
                else {  // in Windows/ANGLE/driver: tag with the first of our return addresses up the stack (bit 63 = "under")
                    if ((r >= nt0 && r < nt1) || (r >= kb0 && r < kb1)) rwait++; else rother++;
                    uint64_t *sp = (uint64_t *)c.Rsp;
                    for (int q = 0; q < 2048; q++) if (ours(sp[q])) { rcount(sp[q] | (1ull << 63)); break; }
                }
            }
            if ((r >= nt0 && r < nt1) || (r >= kb0 && r < kb1)) t->wait++;
            else if (r - (uint64_t)lib < libspan) { t->lib++; count(r); }
            else if (r >= ldr0 && r < ldr1) { t->ldr++; count(r); }
            else { t->other++; t->orip = r; }
        }
        Sleep(1);
    }
    return 0;
}
void prof_start(uint8_t *b, uint64_t span) {
    if (!getenv("SKATESTRAP_PROF")) return;
    lib = b; libspan = span;
    mod_range(0, &ldr0, &ldr1); mod_range(L"ntdll.dll", &nt0, &nt1); mod_range(L"KernelBase.dll", &kb0, &kb1);
    CreateThread(0, 0, sampler, 0, 0, 0);
}
static int by_n(const void *a, const void *b) { return (int)(((const uint32_t *)b)[2]) - (int)(((const uint32_t *)a)[2]); }
void prof_report(void) {
    if (!lib || !getenv("SKATESTRAP_PROF")) return;
    stop = 1; Sleep(20);
    printf("prof: thread  samples  wait%%  libroblox%%  loader%%  other%%   (busy threads only)\n");
    for (int k = 0; k < nth; k++) {
        T *t = &th[k];
        if (!t->n || t->wait * 10 > t->n * 9) continue;
        char mod[MAX_PATH] = "-"; HMODULE hm;  // module of the last "other" sample
        if (t->orip && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)t->orip, &hm)) GetModuleFileNameA(hm, mod, sizeof mod);
        printf("prof: %6lu %8u %5u %10u %8u %7u  %s\n", t->id, t->n, t->wait * 100 / t->n, t->lib * 100 / t->n, t->ldr * 100 / t->n, t->other * 100 / t->n, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod);
    }
    {
        printf("prof: render thread %lu: %u samples, %u%% in Windows waits, %u%% in other modules\n", skate_render_tid, rn, rn ? rwait * 100 / rn : 0, rn ? rother * 100 / rn : 0);
        for (int i = 0; i < 32 && rmod[i].n; i++) {
            char mod[MAX_PATH] = "?"; if (rmod[i].base) GetModuleFileNameA((HMODULE)rmod[i].base, mod, sizeof mod);
            printf("prof:  M %5u %s\n", rmod[i].n, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod);
        }
        static struct { uint64_t rip; uint32_t n, pad; } rt[4096]; int rm = 0;
        for (int i = 0; i < 4096; i++) if (rhot[i].rip) { rt[rm].rip = rhot[i].rip; rt[rm].n = rhot[i].n; rm++; }
        qsort(rt, rm, sizeof *rt, by_n);
        for (int i = 0; i < rm && i < 30; i++) {
            uint64_t r = rt[i].rip & ~(1ull << 63); const char *u2 = rt[i].rip >> 63 ? " (under a wait/other call)" : "";
            if (r - (uint64_t)lib < libspan) printf("prof:  R %5u libroblox+%llx%s\n", rt[i].n, (unsigned long long)(r - (uint64_t)lib), u2);
            else printf("prof:  R %5u loader rva %llx%s\n", rt[i].n, (unsigned long long)(r - ldr0), u2);
        }
    }
    static struct { uint64_t rip; uint32_t n, pad; } top[HOT]; int m = 0;
    for (int i = 0; i < HOT; i++) if (hot[i].rip) { top[m].rip = hot[i].rip; top[m].n = hot[i].n; m++; }
    qsort(top, m, sizeof *top, by_n);
    for (int i = 0; i < m && i < 40; i++) {
        uint64_t r = top[i].rip;
        if (r - (uint64_t)lib < libspan) printf("prof: %6u libroblox+%llx\n", top[i].n, (unsigned long long)(r - (uint64_t)lib));
        else printf("prof: %6u loader rva %llx\n", top[i].n, (unsigned long long)(r - ldr0));
    }
}
