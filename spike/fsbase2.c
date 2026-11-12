// wrfsbase then read back: immediately, after a busy loop, after Sleep(0), after Sleep(1).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>
static uint64_t block[64];
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    _writefsbase_u64((uint64_t)block);
    printf("right after write: %llx (want %p)\n", (unsigned long long)_readfsbase_u64(), (void *)block);
    _writefsbase_u64((uint64_t)block);
    volatile uint64_t x = 0; for (uint64_t i = 0; i < 3000000000ull; i++) x += i;
    printf("after ~1 s busy loop: %llx\n", (unsigned long long)_readfsbase_u64());
    _writefsbase_u64((uint64_t)block); Sleep(0);
    printf("after Sleep(0): %llx\n", (unsigned long long)_readfsbase_u64());
    _writefsbase_u64((uint64_t)block); Sleep(5);
    printf("after Sleep(5): %llx\n", (unsigned long long)_readfsbase_u64());
    _writefsbase_u64((uint64_t)block); SwitchToThread(); GetTickCount64(); CloseHandle(CreateEventA(0,0,0,0));
    printf("after syscalls: %llx\n", (unsigned long long)_readfsbase_u64());
    return 0;
}
