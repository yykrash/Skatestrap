// Can a normal Windows program set the FS base (wrfsbase) and keep it across thread switches?
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>
#include <intrin.h>

static uint64_t block[64];
static volatile LONG done;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    printf("fault %08lx at wrfsbase/rdfsbase: not allowed\n", e->ExceptionRecord->ExceptionCode);
    ExitProcess(2);
}

static DWORD WINAPI spin(void *arg) {
    while (!done) { volatile int x = 0; for (int i = 0; i < 100000; i++) x += i; }
    return 0;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    int r[4];
    __cpuidex(r, 7, 0);
    printf("CPU FSGSBASE bit: %d\n", r[1] & 1);
    AddVectoredExceptionHandler(1, on_fault);
    block[0] = (uint64_t)block;  // bionic-style self pointer at fs:0
    block[5] = 0x1122334455667788ull;  // canary at fs:0x28
    printf("fs base before: %llx\n", (unsigned long long)_readfsbase_u64());
    _writefsbase_u64((uint64_t)block);
    HANDLE t[8];
    for (int i = 0; i < 8; i++) t[i] = CreateThread(0, 0, spin, 0, 0, 0);
    int lost = 0;
    for (int i = 0; i < 2000; i++) {
        Sleep(i % 3);
        uint64_t v;
        __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
        if (v != 0x1122334455667788ull) lost++;
    }
    done = 1;
    WaitForMultipleObjects(8, t, TRUE, INFINITE);
    printf("fs base after: %llx (block %p), canary lost %d / 2000\n", (unsigned long long)_readfsbase_u64(), (void *)block, lost);
    return 0;
}
