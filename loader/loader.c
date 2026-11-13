// Skatestrap milestone 1 (11 Nov 2026): load Roblox's Android x86-64 libroblox.so on Windows and run its
// start-up list (INIT_ARRAY). Every import we don't supply yet is a trap that prints its name and stops,
// so the next missing piece shows itself. Roblox's code is only changed where it reads %fs (Windows
// resets the FS base on every pre-emption): those 9-byte reads become the same instruction on a TCB block.
//   clang -O1 loader.c shim.c -o loader.exe && loader.exe ..\extract\libroblox.so ..\patches.bin
#include <windows.h>
#include <timeapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

typedef struct { unsigned char ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint32_t name; unsigned char info, other; uint16_t shndx; uint64_t value, size; } Sym;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT_ARRAY = 25,
       DT_INIT_ARRAYSZ = 27, DT_JMPREL = 23 };
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012

static uint8_t *base;
void lib_callers(const char *tag, uint64_t *fp);
static const Sym *symtab;
static const char *strtab;
uint64_t *skatestrap_tcb;  // fs:0 self pointer, fs:0x28 stack canary (bionic layout)

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) { printf("short read %s\n", path); exit(1); }
    fclose(f);
    return data;
}

// --- imports: shim table, data blocks, traps ------------------------------------------------------
static uint8_t *trap_page; static size_t trap_used;
static SYSV void missing(const char *name) {
    printf("\nMISSING import called: %s\n", name);
    fflush(stdout);
    ExitProcess(3);
}
static void *make_trap(const char *name) {
    uint8_t *t = trap_page + trap_used; trap_used += 32;
    t[0] = 0x48; t[1] = 0xBF; memcpy(t + 2, &name, 8);                           // mov rdi, name
    void *h = (void *)missing; t[10] = 0x48; t[11] = 0xB8; memcpy(t + 12, &h, 8);  // mov rax, missing
    t[20] = 0xFF; t[21] = 0xE0;                                                   // jmp rax
    return t;
}
static uint64_t resolve(uint32_t index) {
    const Sym *s = &symtab[index];
    const char *name = strtab + s->name;
    if (s->shndx != 0) return (uint64_t)base + s->value;  // defined in the library itself
    void *own = shim_lookup(name);
    if (own) return (uint64_t)own;
    int bind = s->info >> 4, type = s->info & 15;
    if (bind == 2 && type == 0) return 0;  // weak, no type: absent (ZSTD_trace_*, __gcov_*, getentropy...)
    if (type == 1) {                         // data object we don't know yet: a zeroed block
        printf("  data import %s -> zero block\n", name);
        return (uint64_t)calloc(1, 4096);
    }
    return (uint64_t)make_trap(name);
}

// --- relocations ------------------------------------------------------------------------------------
static int64_t sleb(const uint8_t **p) {
    int64_t v = 0; int shift = 0; uint8_t b;
    do { b = *(*p)++; v |= (int64_t)(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift);
    return v;
}
static uint64_t *cache; static uint8_t *cached;
static void apply(uint64_t offset, uint64_t info, int64_t addend) {
    uint32_t type = (uint32_t)info, index = (uint32_t)(info >> 32);
    uint64_t *where = (uint64_t *)(base + offset);
    if (type == 8) { *where = (uint64_t)base + addend; return; }  // RELATIVE
    if (!cached[index]) { cache[index] = resolve(index); cached[index] = 1; }
    uint64_t s = cache[index];
    if (type == 6 || type == 7) *where = s;  // GLOB_DAT, JUMP_SLOT
    else if (type == 1) *where = s + addend;  // 64
    else { printf("unhandled reloc type %u at %llx\n", type, (unsigned long long)offset); exit(1); }
}
static size_t unpack_android_rela(const uint8_t *p) {  // bionic's APS2 packed format
    if (memcmp(p, "APS2", 4)) { printf("packed relocs not APS2\n"); exit(1); }
    p += 4;
    int64_t count = sleb(&p), offset = sleb(&p), info = 0, addend = 0;
    size_t done = 0;
    while (done < (size_t)count) {
        int64_t size = sleb(&p), flags = sleb(&p), delta = 0;
        if (flags & 2) delta = sleb(&p);                     // GROUPED_BY_OFFSET_DELTA
        if (flags & 1) info = sleb(&p);                      // GROUPED_BY_INFO
        if ((flags & 8) && (flags & 4)) addend += sleb(&p);  // HAS_ADDEND + GROUPED_BY_ADDEND
        else if (!(flags & 8)) addend = 0;
        for (int64_t i = 0; i < size; i++) {
            offset += (flags & 2) ? delta : sleb(&p);
            if (!(flags & 1)) info = sleb(&p);
            if ((flags & 8) && !(flags & 4)) addend += sleb(&p);
            apply(offset, info, addend);
            done++;
        }
    }
    return done;
}

// --- %fs rewrites -------------------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct { uint64_t at; uint32_t disp; uint8_t rex, op, modrm; } FsSite;
#pragma pack(pop)
static size_t patch_fs(const char *path) {
    size_t size; FsSite *sites = (FsSite *)read_file(path, &size);
    size_t n = size / sizeof(FsSite);
    for (size_t i = 0; i < n; i++) {
        uint8_t *at = base + sites[i].at;
        if (at[0] != 0x64 || at[1] != sites[i].rex || at[2] != sites[i].op || at[3] != sites[i].modrm || at[4] != 0x25) {
            printf("fs site %llx not as listed (different libroblox build?)\n", (unsigned long long)sites[i].at); exit(1);
        }
        if (sites[i].disp == 0) {  // fs:0 is only used as "which thread am I": the TEB self pointer (gs:0x30) is per-thread
            at[0] = 0x65; uint32_t teb_self = 0x30; memcpy(at + 5, &teb_self, 4);
            continue;
        }
        int64_t rel = (int64_t)((uint8_t *)skatestrap_tcb + sites[i].disp) - (int64_t)(at + 7);
        if (rel != (int32_t)rel) { printf("tcb out of rip reach\n"); exit(1); }
        int32_t r32 = (int32_t)rel;
        at[0] = sites[i].rex; at[1] = sites[i].op; at[2] = (sites[i].modrm & 0xF8) | 0x05;
        memcpy(at + 3, &r32, 4); at[7] = 0x90; at[8] = 0x90;
    }
    free(sites);
    return n;
}

// --- crashes: where in libroblox -------------------------------------------------------------------
static uint64_t span;
// debug: SKATESTRAP_BREAK one-shot breakpoints (original byte kept here)
static uint64_t brk_at[8]; static uint8_t brk_orig[8]; static int brk_n;
static void set_breaks(void) {
    const char *s = getenv("SKATESTRAP_BREAK");
    for (char *end; s && *s && brk_n < 8; s = *end ? end + 1 : end) {
        uint64_t off = strtoull(s, &end, 16);
        if (end == s || off >= span) break;
        DWORD old; uint8_t *a = base + off;
        VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &old);
        brk_orig[brk_n] = *a; *a = 0xCC; brk_at[brk_n++] = off;
        VirtualProtect(a, 1, old, &old);
        printf("break set at libroblox+%llx\n", (unsigned long long)off);
    }
}
// Raw `syscall` instructions (APK 2.746, found by scripts/rawsys_find.py). Each `0f 05` becomes `int3; nop`, and the
// handler below answers it as Linux would. Nothing here hides the change: the bytes differ in memory for anyone who reads them.
// shortcut: fixed offsets for this APK build; rerun the script and update the list when the APK changes
static const uint64_t raw_sys[] = { 0x2d6e0bb, 0x2d6e0de, 0x2d6e25d, 0x2d6e2ac, 0x2d6e2d6, 0x2d6e35b, 0x2d6e7e9, 0x668447b, 0x66844fb, 0x66845b8, 0x668547f, 0x6685571, 0x668564f, 0x66856d7, 0x6686226, 0x6686bdd };
static int route_raw_syscalls(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof raw_sys / sizeof *raw_sys; i++)
        if (raw_sys[i] + 2 <= span && base[raw_sys[i]] == 0x0F && base[raw_sys[i] + 1] == 0x05) { base[raw_sys[i]] = 0xCC; base[raw_sys[i] + 1] = 0x90; n++; }
    return n;
}
static LONG CALLBACK on_crash(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
        for (size_t i = 0; i < sizeof raw_sys / sizeof *raw_sys; i++) {
            uint64_t at = (uint64_t)base + raw_sys[i]; CONTEXT *c = e->ContextRecord;
            if (c->Rip != at && c->Rip != at + 1) continue;
            int64_t skatestrap_linux_syscall(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
            c->Rax = (uint64_t)skatestrap_linux_syscall((int64_t)c->Rax, (int64_t)c->Rdi, (int64_t)c->Rsi, (int64_t)c->Rdx, (int64_t)c->R10, (int64_t)c->R8);
            c->Rip = at + 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
        for (int i = 0; i < brk_n; i++) {
            uint64_t at = (uint64_t)base + brk_at[i];
            if (e->ContextRecord->Rip != at && e->ContextRecord->Rip != at + 1) continue;
            DWORD old; VirtualProtect((void *)at, 1, PAGE_EXECUTE_READWRITE, &old);
            *(uint8_t *)at = brk_orig[i];
            VirtualProtect((void *)at, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void *)at, 1);
            e->ContextRecord->Rip = at;
            printf("BREAK libroblox+%llx thread %lu rax %llx rdi %llx rsi %llx", (unsigned long long)brk_at[i], GetCurrentThreadId(), e->ContextRecord->Rax, e->ContextRecord->Rdi, e->ContextRecord->Rsi);
            lib_callers("", (uint64_t *)e->ContextRecord->Rbp);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t rip = e->ContextRecord->Rip;
    // a Windows DLL's own C++ throw (e06d7363, e.g. the AMD driver at shutdown) is first-chance here and gets caught by its
    // owner; calling it a crash killed the process mid-exit and looped to a stack overflow (2026-11-11)
    if (code == 0xE06D7363 && !(rip >= (uint64_t)base && rip < (uint64_t)base + span)) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG crashing;
    if (InterlockedCompareExchange(&crashing, (LONG)GetCurrentThreadId(), 0)) {
        if ((DWORD)crashing != GetCurrentThreadId()) Sleep(INFINITE);  // another thread is printing its report: let it finish
        TerminateProcess(GetCurrentProcess(), 4);  // a fault while reporting one: stop, don't recurse
    }
    if (rip >= (uint64_t)base && rip < (uint64_t)base + span)
        printf("\nCRASH %08lx in libroblox+%llx (rsp %llx)\n", code, (unsigned long long)(rip - (uint64_t)base), (unsigned long long)e->ContextRecord->Rsp);
    else
        {   HMODULE m = 0; char mn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m)) GetModuleFileNameA(m, mn, sizeof mn);
            printf("\nCRASH %08lx at %llx (outside libroblox: %s +%llx)\n", code, (unsigned long long)rip, mn, (unsigned long long)(rip - (uint64_t)m));
            void lib_callers(const char *, uint64_t *); lib_callers("  lib frames", (uint64_t *)e->ContextRecord->Rbp);
        }
    if (code == EXCEPTION_ACCESS_VIOLATION)
        printf("  touching %llx\n", (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    // libroblox keeps frame pointers: walk rbp for the callers
    uint64_t *fp = (uint64_t *)e->ContextRecord->Rbp;
    for (int i = 0; i < 16 && fp && !IsBadReadPtr(fp, 16) && fp[1] - (uint64_t)base < span; i++, fp = (uint64_t *)fp[0])
        printf("  from %llx\n", (unsigned long long)(fp[1] - (fp[1] - (uint64_t)base < span ? (uint64_t)base : 0)));
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);  // not ExitProcess: DLL detach after a crash (or during exit) faults again
    return EXCEPTION_CONTINUE_SEARCH;
}

// exported symbols by name, through the GNU hash table (symtab itself has no count)
static const uint32_t *gnu_hash;
void *skatestrap_export(const char *name) {
    if (!gnu_hash) return 0;
    uint32_t h = 5381;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) h = h * 33 + *c;
    uint32_t nbuckets = gnu_hash[0], symoffset = gnu_hash[1], bloom = gnu_hash[2];
    const uint32_t *buckets = gnu_hash + 4 + bloom * 2, *chain = buckets + nbuckets;
    for (uint32_t i = buckets[h % nbuckets]; i >= symoffset && i; i++) {
        uint32_t h2 = chain[i - symoffset];
        if ((h | 1) == (h2 | 1) && symtab[i].shndx && !strcmp(strtab + symtab[i].name, name)) return base + symtab[i].value;
        if (h2 & 1) break;
    }
    return 0;
}
typedef SYSV void (*InitFn)(int, char **, char **);
static uint64_t *init_array; static size_t init_count;
static DWORD WINAPI run_init(void *arg) {
    static char *argv[] = { "libroblox", 0 }, *envp[] = { 0 };
    for (size_t i = 0; i < init_count; i++) {
        uint64_t f = init_array[i];
        if (f == 0 || f == (uint64_t)-1) continue;
        if (i % 250 == 0) printf("  init %zu / %zu\n", i, init_count);
        ((InitFn)f)(1, argv, envp);
    }
    printf("  init %zu / %zu: all ran\n", init_count, init_count);
    void *jni_vm(void);
    typedef SYSV int (*OnLoad)(void *, void *);
    OnLoad on_load = (OnLoad)skatestrap_export("JNI_OnLoad");
    if (!on_load) { printf("no JNI_OnLoad export\n"); return 0; }
    printf("JNI_OnLoad -> 0x%x\n", on_load(jni_vm(), 0));
    void skatestrap_startup(void); skatestrap_startup();
    return 0;
}

static DWORD WINAPI flusher(void *x) {
    extern volatile long long skate_last_frame_ns; int64_t now_ns(int); void futex_dump(void); int dumped = 0;
    for (;;) {
        Sleep(100);
        long long last = skate_last_frame_ns, idle = now_ns(1) - last;  // set only under SKATESTRAP_FPS
        if (last && idle > 3000000000ll && !dumped) {
            dumped = 1; futex_dump();
            // SKATESTRAP_FREEZE_DUMP=<path>: one minidump (threads + stacks) of the first freeze, for scratchpad mdstk.py
            static int once; const char *dp = getenv("SKATESTRAP_FREEZE_DUMP");
            HMODULE dh = dp && !once++ ? LoadLibraryA("dbghelp.dll") : 0;
            BOOL (WINAPI *mdw)(HANDLE, DWORD, HANDLE, int, void *, void *, void *) = dh ? (void *)GetProcAddress(dh, "MiniDumpWriteDump") : 0;
            HANDLE f = mdw ? CreateFileA(dp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0) : INVALID_HANDLE_VALUE;
            if (f != INVALID_HANDLE_VALUE) { printf("freeze: minidump %s\n", mdw(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000, 0, 0, 0) ? "written" : "failed"); CloseHandle(f); }
        }
        if (idle < 1000000000ll) dumped = 0;
        fflush(stdout);
    }
}
int main(int argc, char **argv) {
    // Buffered, flushed by a thread every 100 ms: unbuffered, each Roblox log line was a blocking WriteFile
    // (a console write is slow) on the game thread that logged it. Exit and crash paths fflush first.
    setvbuf(stdout, 0, _IOFBF, 1 << 16);
    CreateThread(0, 0, flusher, 0, 0, 0);
    if (argc > 1 && !strcmp(argv[1], "--gpu")) { int skate_gpu_kind(void); return skate_gpu_kind(); }
    if (argc > 1 && !strcmp(argv[1], "--selftest")) { int shim_fs_selftest(void); int shim_tm_selftest(void); int shim_asset_selftest(void); int shim_net_selftest(void); int shim_futex_selftest(void); int shim_mem_selftest(void); int shim_audio_selftest(void); int shim_pad_selftest(void); int vk_mem_selftest(void); int shim_tname_selftest(void); int bad = shim_tname_selftest() + vk_mem_selftest() + shim_pad_selftest() + shim_audio_selftest() + shim_mem_selftest() + shim_fs_selftest() + shim_tm_selftest() + shim_asset_selftest() + shim_net_selftest() + shim_futex_selftest(); printf("selftest: %d bad\n", bad); return bad != 0; }
    if (argc < 3) { printf("usage: loader libroblox.so patches.bin\n"); return 1; }
    size_t size; uint8_t *file = read_file(argv[1], &size);
    Ehdr *eh = (Ehdr *)file;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->machine != 62) { printf("not an x86-64 ELF\n"); return 1; }
    Phdr *ph = (Phdr *)(file + eh->phoff);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
    span = (span + 0xFFFF) & ~0xFFFFull;
    base = VirtualAlloc(0, span + 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    skatestrap_tcb = (uint64_t *)(base + span);  // (just past the image: in rip reach of every site)
    trap_page = base + span + 0x1000;           // traps: 31 pages, made executable below
    skatestrap_tcb[0] = (uint64_t)skatestrap_tcb;
    skatestrap_tcb[5] = 0x5ca7e5ca7e000000ull ^ GetTickCount64();
    uint64_t dyn_at = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) memcpy(base + ph[i].vaddr, file + ph[i].offset, ph[i].filesz);
        if (ph[i].type == PT_DYNAMIC) dyn_at = ph[i].vaddr;
    }
    printf("mapped %llu MB at %p\n", (unsigned long long)(span >> 20), (void *)base);
    void prof_start(uint8_t *, uint64_t); prof_start(base, span);  // SKATESTRAP_PROF=1
    { extern uint8_t *skate_lib_base; extern uint64_t skate_lib_span; skate_lib_base = base; skate_lib_span = span; }

    uint64_t rela = 0, jmprel = 0, pltsz = 0;
    for (Dyn *d = (Dyn *)(base + dyn_at); d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_STRTAB: strtab = (const char *)(base + d->val); break;
        case DT_SYMTAB: symtab = (const Sym *)(base + d->val); break;
        case 0x6ffffef5: gnu_hash = (const uint32_t *)(base + d->val); break;  // DT_GNU_HASH
        case DT_ANDROID_RELA: rela = d->val; break;
        case DT_JMPREL: jmprel = d->val; break;
        case DT_PLTRELSZ: pltsz = d->val; break;
        case DT_INIT_ARRAY: init_array = (uint64_t *)(base + d->val); break;
        case DT_INIT_ARRAYSZ: init_count = d->val / 8; break;
        }
    }
    cache = calloc(1 << 16, 8); cached = calloc(1 << 16, 1);
    shim_init(base, span);
    size_t packed = unpack_android_rela(base + rela);
    for (uint64_t i = 0; i < pltsz / 24; i++) {
        uint64_t *r = (uint64_t *)(base + jmprel) + i * 3;
        apply(r[0], r[1], (int64_t)r[2]);
    }
    printf("relocations: %zu packed + %llu plt; %zu traps\n", packed, (unsigned long long)(pltsz / 24), trap_used / 32);
    printf("fs sites rewritten: %zu\n", patch_fs(argv[2]));
    printf("raw syscall sites routed: %d of 16\n", route_raw_syscalls());
    {   // FPS cap above 240: the engine clamps DFIntTaskSchedulerTargetFps to 240 in code
        // (mov eax,[flag]; mov ecx,240; cmp eax,ecx; cmovl ecx,eax). Raise that 240 to the cap asked for.
        const char *v = getenv("SKATESTRAP_FPS_CAP");
        int want = !v ? 0 : strcmp(v, "unlimited") ? atoi(v) : 9999;
        static const uint8_t pat[] = { 0xb9, 0xf0, 0, 0, 0, 0x39, 0xc8, 0x0f, 0x4c, 0xc8 };
        int hits = 0;
        for (int i = 0; want > 240 && i < eh->phnum; i++)
            if (ph[i].type == PT_LOAD && (ph[i].flags & 1))
                for (uint8_t *p = base + ph[i].vaddr + 6, *e = base + ph[i].vaddr + ph[i].filesz - sizeof pat; p < e; p++)
                    if (p[0] == 0xb9 && p[-6] == 0x8b && p[-5] == 0x05 && !memcmp(p, pat, sizeof pat)) { memcpy(p + 1, &want, 4); hits++; }
        if (want > 240) printf("fps limit sites raised to %d: %d (want 1)\n", want, hits);
        // The in-game Max Frame Rate setting (60..240) is a separate "user cap" that wins over the flag.
        // mov ebx,[rax+0x14c] -> mov ebx,-1 ("unlimited"), so the launcher's cap rules.
        static const uint8_t uc[] = { 0x8b, 0x98, 0x4c, 0x01, 0, 0, 0xe8 }, unl[] = { 0xbb, 0xff, 0xff, 0xff, 0xff, 0x90 };
        hits = 0;
        for (int i = 0; want > 240 && i < eh->phnum; i++)
            if (ph[i].type == PT_LOAD && (ph[i].flags & 1))
                for (uint8_t *p = base + ph[i].vaddr, *e = base + ph[i].vaddr + ph[i].filesz - 13; p < e; p++)
                    if (p[0] == 0x8b && !memcmp(p, uc, sizeof uc) && p[11] == 0x80 && p[12] == 0x3d) { memcpy(p, unl, sizeof unl); hits++; }
        if (want > 240) printf("in-game fps setting overridden: %d (want 1)\n", hits);
        // Frame pacer clamps the same flag again: mov eax,240; cmp r15d,eax; cmovae r15d,eax.
        static const uint8_t pc[] = { 0xb8, 0xf0, 0, 0, 0, 0x41, 0x39, 0xc7, 0x44, 0x0f, 0x43, 0xf8 };
        hits = 0;
        for (int i = 0; want > 240 && i < eh->phnum; i++)
            if (ph[i].type == PT_LOAD && (ph[i].flags & 1))
                for (uint8_t *p = base + ph[i].vaddr, *e = base + ph[i].vaddr + ph[i].filesz - sizeof pc; p < e; p++)
                    if (p[0] == 0xb8 && !memcmp(p, pc, sizeof pc)) { memcpy(p + 1, &want, 4); hits++; }
        if (want > 240) printf("fps pacer limit raised: %d (want 1)\n", hits);
    }

    DWORD old;
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && (ph[i].flags & 1)) {
            uint64_t lo = ph[i].vaddr & ~0xFFFull, hi = (ph[i].vaddr + ph[i].memsz + 0xFFF) & ~0xFFFull;
            VirtualProtect(base + lo, hi - lo, PAGE_EXECUTE_READ, &old);
        }
    VirtualProtect(trap_page, 0x1F000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), base, span + 0x20000);
    timeBeginPeriod(1);  // 1 ms timer: Sleep/waits otherwise round up to 15.6 ms (Android sleeps are fine-grained)
    {   // Win11 drops the 1 ms request (and may run us as EcoQoS) when the window is occluded: opt out of both
        PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0x1 | 0x4, 0 };  // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, state off
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    }
    AddVectoredExceptionHandler(1, on_crash);
    set_breaks();

    // (own thread, 16 MB committed up front: SysV code has no stack probes for Windows' guard page)
    HANDLE t = CreateThread(0, 16 << 20, run_init, 0, 0, 0);
    WaitForSingleObject(t, INFINITE);
    // Roblox's own threads keep running; watch them a while, then stop hard (CRT exit would tear down under them)
    Sleep(1000);  // start-up already pumped the main thread for SKATESTRAP_WATCH_MS
    void prof_report(void); prof_report();
    puts("  loader: stopping"); fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

// prints the libroblox return addresses up an rbp chain (libroblox keeps frame pointers)
void lib_callers(const char *tag, uint64_t *fp) {
    printf("%s:", tag);
    // shortcut: heuristic stack scan (any qword pointing just past a call in libroblox), debug only
    int shown = 0;
    // stay inside this thread's stack: IsBadReadPtr's probe fault reached our vectored handler as a "crash"
    ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi);
    int64_t room = (uint64_t)fp >= lo && (uint64_t)fp < hi ? (int64_t)((hi - (uint64_t)fp) / 8) : 0;
    MEMORY_BASIC_INFORMATION mi;  // guest coroutine stacks are plain heap regions: stay inside the committed one
    if (!room && VirtualQuery(fp, &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD))
        room = (int64_t)(((uint64_t)mi.BaseAddress + mi.RegionSize - (uint64_t)fp) / 8);
    for (int i = 0; i < 1024 && i < room && shown < 16; i++) {
        uint64_t r = fp[i];
        if (r - (uint64_t)base >= span || r - (uint64_t)base < 8) continue;
        uint8_t *c = (uint8_t *)r;
        int is_call = c[-5] == 0xe8 || (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10) || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10);
        if (is_call) { printf(" %llx", (unsigned long long)(r - (uint64_t)base)); shown++; }
    }
    printf("\n");
}
void lib_ret(void *r) {
    uint64_t a = (uint64_t)r;
    if (a - (uint64_t)base < span) printf("  ret libroblox+%llx\n", (unsigned long long)(a - (uint64_t)base));
    else printf("  ret %llx (outside)\n", (unsigned long long)a);
}
