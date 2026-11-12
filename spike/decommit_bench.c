// Does VirtualFree(MEM_DECOMMIT)+VirtualAlloc(MEM_COMMIT) (our madvise(DONTNEED)) stall while other threads fault pages?
// usage: decommit_bench <mode 0=decommit+commit 1=MEM_RESET 2=memset> <toucher threads> <seconds>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define ARENA (1ull << 30)
static uint8_t *arena; static volatile LONG stop; static volatile LONG64 touched;
static double us(LARGE_INTEGER a, LARGE_INTEGER b) { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart; }
static DWORD WINAPI toucher(void *x) {  // keeps taking fresh-page faults in its own 256 MB block
    uint64_t s = (uint64_t)(size_t)x * 0x9E3779B97F4A7C15ull + 1; LONG64 n = 0; size_t sz = 256u << 20;
    uint8_t *m = VirtualAlloc(0, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    while (!stop) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; m[(s % (sz >> 12)) << 12]++; n++;
        if ((n & 0xffff) == 0) { VirtualFree(m, sz, MEM_DECOMMIT); VirtualAlloc(m, sz, MEM_COMMIT, PAGE_READWRITE); }
    }
    InterlockedAdd64(&touched, n); return 0;
}
int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0, nt = argc > 2 ? atoi(argv[2]) : 12, secs = argc > 3 ? atoi(argv[3]) : 5;
    arena = VirtualAlloc(0, ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // as s_mmap does
    HANDLE h[64]; for (int i = 0; i < nt; i++) h[i] = CreateThread(0, 0, toucher, (void *)(size_t)(i + 1), 0, 0);
    uint64_t s = 88172645463325252ull; double worst = 0, sum = 0; int n = 0, over1 = 0, over10 = 0;
    DWORD end = GetTickCount() + secs * 1000;
    while ((int)(end - GetTickCount()) > 0) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t len = ((s >> 20) % 64 + 1) << 16;  // 64 KB .. 4 MB
        uint8_t *p = arena + ((s % ((ARENA - len) >> 16)) << 16);
        memset(p, 1, len);  // in use before the purge
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        if (mode == 0) { VirtualFree(p, len, MEM_DECOMMIT); VirtualAlloc(p, len, MEM_COMMIT, PAGE_READWRITE); }
        else if (mode == 1) VirtualAlloc(p, len, MEM_RESET, PAGE_READWRITE);
        else memset(p, 0, len);
        QueryPerformanceCounter(&b);
        double t = us(a, b); sum += t; n++; if (t > worst) worst = t; if (t > 1000) over1++; if (t > 10000) over10++;
        Sleep(10);
    }
    stop = 1; WaitForMultipleObjects(nt, h, TRUE, 2000);
    printf("mode %d, %d touchers: %d calls, mean %.0f us, worst %.0f us, >1ms %d, >10ms %d; touches %.1f M/s\n", mode, nt, n, sum / n, worst, over1, over10, touched / 1e6 / secs);
    return 0;
}
