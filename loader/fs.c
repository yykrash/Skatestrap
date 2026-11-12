// Files for libroblox.so. Linux paths live in a sandbox folder next to loader.exe ("root"), so /data/data/...
// becomes <exe dir>\root\data\data\... and nothing can climb out of it. Linux fds are the Windows CRT's fds
// (0-2 line up); /dev/urandom and /dev/random are NUL-backed fds whose reads come from BCryptGenRandom.
// /proc and the rest of /dev are honestly absent (ENOENT): this is not a Linux kernel.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "shim.h"

int *shim_errno(void);
void *shim_malloc(size_t n);
#define SETERR(e) (*shim_errno() = (e))
enum { L_ENOENT = 2, L_EBADF = 9, L_EACCES = 13, L_EEXIST = 17, L_ENOTDIR = 20, L_EINVAL = 22, L_ENOTTY = 25,
       L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39 };

// CRT errno numbers match Linux up to 34; past that they differ
static int linux_errno(int e) {
    switch (e) { case 36: return 35; case 38: return 36; case 40: return 38; case 41: return 39; case 42: return 84; }
    return e;
}
static int fail(void) { SETERR(linux_errno(errno)); return -1; }

// --- paths ----------------------------------------------------------------------------------------------
static wchar_t root[MAX_PATH];
static void make_root(void) {
    if (root[0]) return;
    GetModuleFileNameW(0, root, MAX_PATH);
    *wcsrchr(root, L'\\') = 0;
    wcscat(root, L"\\root");
    // what a fresh install on a phone already has
    static const wchar_t *dirs[] = { L"", L"\\data", L"\\data\\data", L"\\data\\data\\com.roblox.client",
        L"\\data\\data\\com.roblox.client\\files", L"\\data\\data\\com.roblox.client\\cache", L"\\sdcard", L"\\sdcard\\Android",
        L"\\sdcard\\Android\\data", L"\\sdcard\\Android\\data\\com.roblox.client", L"\\sdcard\\Android\\data\\com.roblox.client\\files",
        L"\\sdcard\\Android\\data\\com.roblox.client\\cache", L"\\tmp" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        wchar_t d[MAX_PATH + 64]; wcscpy(d, root); wcscat(d, dirs[i]); CreateDirectoryW(d, 0);
    }
}
// Linux path -> normalised absolute Linux path (no . or .., no doubled /). 0 if too long.
static int normalise(const char *in, char *out, size_t n) {
    char tmp[1024];
    const char *aliases[][2] = { { "/data/user/0/", "/data/data/" }, { "/storage/emulated/0/", "/sdcard/" }, { "/data/local/tmp/", "/tmp/" } };
    for (size_t i = 0; i < 3; i++) {
        size_t a = strlen(aliases[i][0]);
        if (!strncmp(in, aliases[i][0], a) || (!strncmp(in, aliases[i][0], a - 1) && !in[a - 1])) {
            if (snprintf(tmp, sizeof tmp, "%s%s", aliases[i][1], in[a - 1] ? in + a : "") >= (int)sizeof tmp) return 0;
            in = tmp; break;
        }
    }
    size_t len = 0; out[len++] = '/';
    for (const char *p = in; *p;) {
        while (*p == '/') p++;
        const char *e = p; while (*e && *e != '/') e++;
        size_t c = e - p;
        if (c == 0) break;
        if (c == 1 && p[0] == '.') {}
        else if (c == 2 && p[0] == '.' && p[1] == '.') { while (len > 1 && out[len - 1] != '/') len--; if (len > 1) len--; }
        else {
            if (len + c + 2 >= n) return 0;
            if (len > 1) out[len++] = '/';
            memcpy(out + len, p, c); len += c;
        }
        p = e;
    }
    out[len] = 0;
    return 1;
}
// Linux path -> Windows path inside root. Returns 0 (errno set) for paths that don't exist on this side.
static int host_path(const char *path, wchar_t *out) {
    char lp[1024];
    if (!path) { SETERR(14); return 0; }
    if (!normalise(path, lp, sizeof lp)) { SETERR(L_ENAMETOOLONG); return 0; }
    if (!strncmp(lp, "/proc", 5) || !strncmp(lp, "/sys", 4)) { SETERR(L_ENOENT); return 0; }
    for (const char *c = lp; *c; c++) if (strchr("\\:*?\"<>|", *c)) { SETERR(L_ENOENT); return 0; }  // not expressible on NTFS
    {   // a mod file replaces the game's own (asset.c mods_path)
        static const char A[] = "/data/app/com.roblox.client/assets/"; int mods_path(const char *, wchar_t *);
        if (!strncmp(lp, A, sizeof A - 1) && mods_path(lp + sizeof A - 1, out)) return 1;
    }
    make_root();
    wcscpy(out, root);
    size_t w = wcslen(out);
    int k = MultiByteToWideChar(CP_UTF8, 0, lp, -1, out + w, MAX_PATH * 2 - (int)w);
    if (!k) { SETERR(L_ENAMETOOLONG); return 0; }
    for (wchar_t *c = out + w; *c; c++) if (*c == L'/') *c = L'\\';
    if (!out[w + 1]) out[w] = 0;  // "/" itself is root
    return 1;
}

// --- fds --------------------------------------------------------------------------------------------------
static uint8_t proc_fd[8192];  // fd -> 1 when it is one of our /proc files (fstatfs says procfs)
int fs_is_proc(int fd) { return fd >= 0 && fd < 8192 && proc_fd[fd]; }
static uint8_t random_fd[8192];  // fd -> 1 when it stands for /dev/urandom
// sockets, eventfd, timerfd and epoll live in net.c; these fds are NUL placeholders there too
int net_kind(int fd); int64_t net_read(int fd, void *buf, uint64_t n); int64_t net_write(int fd, const void *buf, uint64_t n);
void net_close(int fd); int net_fcntl(int fd, int cmd, uint64_t arg); int net_ioctl(int fd, uint64_t req, void *arg); unsigned net_mode(int fd);
// /proc files Roblox reads for memory stats: written fresh into a delete-on-close temp file at each open
// Roblox polls /sys/.../cpufreq on every core ~4x/s and /proc/self/oom_score: the misses stay ENOENT, unprinted unless SKATESTRAP_TRACE_FS
static int trace_fs(void);
static int quiet_miss(const char *p) { return p && (!strncmp(p, "/sys/", 5) || !strncmp(p, "/proc/", 6)) && !trace_fs(); }
static volatile int threads;  // this process's thread count, for /proc/self/status and stat
static DWORD WINAPI count_threads(void *x) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = { sizeof te }; DWORD pid = GetCurrentProcessId(); int n = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) n += te.th32OwnerProcessID == pid;
    CloseHandle(snap); threads = n; return 0;
}
static int proc_text(const char *path, char *o, size_t n) {
    if (!path || strncmp(path, "/proc/", 6)) return -1;  // every open comes through here: leave before the costly queries
    MEMORYSTATUSEX ms = { sizeof ms }; PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
    GlobalMemoryStatusEx(&ms); K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    uint64_t vm = pm.PrivateUsage, rss = pm.WorkingSetSize, peak_rss = pm.PeakWorkingSetSize, peak_vm = pm.PeakPagefileUsage;
    static ULONGLONG at; DWORD pid = GetCurrentProcessId();
    // the snapshot walks every thread on the PC (10-50 ms): only for the two files that show it, at most each 5 s,
    // and after the first time on a pool thread, so the reader (a game job) never waits on it; it sees the last count
    int want = (!strcmp(path, "/proc/self/status") || !strcmp(path, "/proc/self/stat")) && (!at || GetTickCount64() - at > 5000);
    if (want) { int first = !at; at = GetTickCount64(); if (first) count_threads(0); else QueueUserWorkItem(count_threads, 0, WT_EXECUTEDEFAULT); }
    if (!strcmp(path, "/proc/meminfo"))
        return snprintf(o, n, "MemTotal: %llu kB\nMemFree: %llu kB\nMemAvailable: %llu kB\nBuffers: 0 kB\nCached: 0 kB\nSwapTotal: %llu kB\nSwapFree: %llu kB\n",
                        ms.ullTotalPhys >> 10, ms.ullAvailPhys >> 10, ms.ullAvailPhys >> 10,
                        (ms.ullTotalPageFile - ms.ullTotalPhys) >> 10, ms.ullAvailPageFile > ms.ullAvailPhys ? (ms.ullAvailPageFile - ms.ullAvailPhys) >> 10 : 0);
    if (!strcmp(path, "/proc/self/statm"))
        return snprintf(o, n, "%llu %llu 0 0 0 %llu 0\n", vm >> 12, rss >> 12, vm >> 12);
    if (!strcmp(path, "/proc/self/status"))
        return snprintf(o, n, "Name:\tcom.roblox.client\nState:\tR (running)\nPid:\t%lu\nTracerPid:\t0\nUid:\t10000\t10000\t10000\t10000\n"
                        "VmPeak:\t%llu kB\nVmSize:\t%llu kB\nVmHWM:\t%llu kB\nVmRSS:\t%llu kB\nThreads:\t%d\n",
                        pid, peak_vm >> 10, vm >> 10, peak_rss >> 10, rss >> 10, threads);
    if (!strcmp(path, "/proc/self/stat"))  // fields 1-24: pid comm state ppid ... num_threads(20) ... vsize(23) rss(24)
        return snprintf(o, n, "%lu (com.roblox.client) R 1 %lu %lu 0 -1 0 0 0 0 0 0 0 0 0 20 0 %d 0 0 %llu %llu\n", pid, pid, pid, threads, vm, rss >> 12);
    return -1;
}
extern uint8_t *shim_image; extern uint64_t shim_image_span;
// /proc/self/maps: the true layout of this Windows process in Linux's format. Nothing is left out or renamed:
// Windows modules show their Windows paths, and libroblox shows as what it is here, private memory (dev 00:00, inode 0).
static int maps_text(char *o, int cap) {
    int n = 0; MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *at = 0; VirtualQuery(at, &mi, sizeof mi) && n < cap - 600; at = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT) continue;
        DWORD pr = mi.Protect & 0xff; char name[300] = "";
        int r = pr != PAGE_NOACCESS, w = pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY, x = pr >= PAGE_EXECUTE;
        uint8_t *b = mi.BaseAddress; uint64_t off = 0;
        if (b >= shim_image && b < shim_image + shim_image_span) { strcpy(name, "/data/app/com.roblox.client/lib/x86_64/libroblox.so"); off = b - shim_image; }
        else if (mi.Type == MEM_IMAGE) { GetModuleFileNameA(mi.AllocationBase, name, sizeof name); off = b - (uint8_t *)mi.AllocationBase; }
        n += snprintf(o + n, cap - n, "%llx-%llx %c%c%cp %08llx 00:00 0 %s\n", (uint64_t)b, (uint64_t)b + mi.RegionSize, r ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-', off, name);
    }
    return n;
}
static int proc_open(const char *path) {
    static char text[2048];  // shortcut: one shared buffer under a lock; these files are tiny
    static SRWLOCK l = SRWLOCK_INIT;
    if (path && !strcmp(path, "/proc/self/maps")) {
        enum { CAP = 4 << 20 }; char *m = malloc(CAP); int pp[2], mfd = -1;
        if (m) { int mn = maps_text(m, CAP); if (_pipe(pp, mn + 16, _O_BINARY | _O_NOINHERIT) == 0) { _write(pp[1], m, mn); _close(pp[1]); mfd = pp[0]; } free(m); }
        if (mfd >= 0 && mfd < 8192) proc_fd[mfd] = 1;
        return mfd;
    }
    AcquireSRWLockExclusive(&l);
    int n = proc_text(path, text, sizeof text), fd = -1;
    if (n >= 0) {
        // a pipe, not a temp file: creating and deleting a disk file per read cost 10-50 ms and stalled frames
        // shortcut: no lseek/fstat size on these fds; back them with memory if a reader needs to rewind
        int pp[2];
        if (_pipe(pp, sizeof text, _O_BINARY | _O_NOINHERIT) == 0) { _write(pp[1], text, n); _close(pp[1]); fd = pp[0]; }
    }
    if (fd >= 0 && fd < 8192) proc_fd[fd] = 1;
    ReleaseSRWLockExclusive(&l);
    return fd;
}
static int is_dev_random(const char *p) { return p && (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")); }

static int trace_fs(void) { static int t = -1; if (t < 0) t = getenv("SKATESTRAP_TRACE_FS") != 0; return t; }
static SYSV int s_open(const char *path, int flags, int mode) {
    if (trace_fs()) printf("fs: open(%s, %x)\n", path ? path : "(null)", flags);
    if (is_dev_random(path)) {
        int fd = _open("NUL", _O_RDONLY | _O_BINARY);
        if (fd < 0) return fail();
        if (fd < 8192) random_fd[fd] = 1;
        return fd;
    }
    int pfd = proc_open(path);
    if (pfd >= 0) return pfd;
    wchar_t w[MAX_PATH * 2];
    if (!host_path(path, w)) { if (!quiet_miss(path)) printf("open(%s) -> ENOENT\n", path ? path : "(null)"); return -1; }
    DWORD attr = GetFileAttributesW(w);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (flags & 3) { SETERR(21); return -1; }  // EISDIR
        int fd = _open("NUL", _O_RDONLY | _O_BINARY);  // shortcut: directory fds only support close/fsync
        return fd < 0 ? fail() : fd;
    }
    if (flags & 0x10000) { SETERR(attr == INVALID_FILE_ATTRIBUTES ? L_ENOENT : L_ENOTDIR); return -1; }  // O_DIRECTORY
    int f = (flags & 3) | _O_BINARY | _O_NOINHERIT;
    if (flags & 0x40) f |= _O_CREAT;
    if (flags & 0x80) f |= _O_EXCL;
    if (flags & 0x200) f |= _O_TRUNC;
    if (flags & 0x400) f |= _O_APPEND;
    int fd = _wopen(w, f, _S_IREAD | _S_IWRITE);
    return fd < 0 ? fail() : fd;
}
static SYSV int s_open_2(const char *path, int flags) { return s_open(path, flags, 0); }
static SYSV int s_close(int fd) {
    if (fd >= 0 && fd < 8192) {
        random_fd[fd] = 0; proc_fd[fd] = 0;
        if (net_kind(fd)) {
            static int t = -1; if (t < 0) t = getenv("SKATESTRAP_TRACE_NET") != 0;
            if (t) { void lib_callers(const char *, uint64_t *); void lib_ret(void *); lib_ret(__builtin_return_address(0)); lib_callers("  close from", __builtin_frame_address(0)); }
            net_close(fd);
        }
    }
    return _close(fd) ? fail() : 0;
}
static SYSV int64_t s_read(int fd, void *buf, uint64_t n) {
    if (net_kind(fd)) return net_read(fd, buf, n);
    if (fd >= 0 && fd < 8192 && random_fd[fd]) {
        if (n > 1 << 20) n = 1 << 20;
        return BCryptGenRandom(0, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? (SETERR(5), -1) : (int64_t)n;
    }
    if (n > 0x7fffffff) n = 0x7fffffff;
    int r = _read(fd, buf, (unsigned)n);
    return r < 0 ? fail() : r;
}
static SYSV int64_t s_read_chk(int fd, void *buf, uint64_t n, uint64_t cap) { return s_read(fd, buf, n); }
static SYSV int64_t s_write(int fd, const void *buf, uint64_t n) {
    if (net_kind(fd)) return net_write(fd, buf, n);
    if (n > 0x7fffffff) n = 0x7fffffff;
    int r = _write(fd, buf, (unsigned)n);
    return r < 0 ? fail() : r;
}
static SYSV int64_t s_write_chk(int fd, const void *buf, uint64_t n, uint64_t cap) { return s_write(fd, buf, n); }
typedef struct { void *base; uint64_t len; } IoVec;
static SYSV int64_t s_writev(int fd, const IoVec *v, int cnt) {
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        int64_t r = s_write(fd, v[i].base, v[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((uint64_t)r < v[i].len) break;
    }
    return total;
}
static SYSV int64_t s_lseek(int fd, int64_t off, int whence) { int64_t r = _lseeki64(fd, off, whence); return r < 0 ? fail() : r; }
// shortcut: pread/pwrite save and restore the position, so they are not atomic against another thread on the same fd
static SYSV int64_t s_pread(int fd, void *buf, uint64_t n, int64_t off) {
    // shortcut: a /proc fd re-read with pread sees EOF after the first read. Kept on purpose: when Roblox's statm
    // re-read parses (fresh text per pread), fps drops ~20-30 (A/B 2026-11-11). Revisit once we know what it does with it.
    int64_t was = _lseeki64(fd, 0, SEEK_CUR);
    if (was < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail();
    int64_t r = s_read(fd, buf, n); int e = *shim_errno();
    _lseeki64(fd, was, SEEK_SET); SETERR(e);
    return r;
}
static SYSV int64_t s_pwrite(int fd, const void *buf, uint64_t n, int64_t off) {
    int64_t was = _lseeki64(fd, 0, SEEK_CUR);
    if (was < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail();
    int64_t r = s_write(fd, buf, n); int e = *shim_errno();
    _lseeki64(fd, was, SEEK_SET); SETERR(e);
    return r;
}
static SYSV int s_fsync(int fd) { _commit(fd); return 0; }  // directory fds (NUL) have nothing to flush
static SYSV int s_ftruncate(int fd, int64_t len) { return _chsize_s(fd, len) ? fail() : 0; }
static SYSV int s_posix_fallocate(int fd, int64_t off, int64_t len) {
    int64_t size = _filelengthi64(fd);
    if (size < 0) return linux_errno(errno);
    return size < off + len && _chsize_s(fd, off + len) ? linux_errno(errno) : 0;
}
static SYSV int s_fcntl(int fd, int cmd, uint64_t arg) {
    if ((cmd == 3 || cmd == 4) && net_kind(fd)) return net_fcntl(fd, cmd, arg);
    switch (cmd) {
    case 1: case 2: return 0;            // F_GETFD / F_SETFD (FD_CLOEXEC: nothing is ever exec'd)
    case 3: return 2;                    // F_GETFL: O_RDWR
    case 4: return 0;                    // F_SETFL (O_NONBLOCK on files is a no-op on Linux too)
    case 0: case 1030: { int r = _dup(fd); return r < 0 ? fail() : r; }  // F_DUPFD, F_DUPFD_CLOEXEC
    // POSIX record locks (SQLite's rbx-storage.db cache uses them): one process here, so nothing ever conflicts.
    case 5: case 12: if (arg) *(short *)arg = 2; return 0;   // F_GETLK[64]: report no lock (l_type = F_UNLCK)
    case 6: case 7: case 13: case 14: return 0;              // F_SETLK[W][64]: grant
    }
    printf("fcntl(%d, %d) not emulated\n", fd, cmd);
    SETERR(L_EINVAL); return -1;
}
static SYSV int s_ioctl(int fd, uint64_t req, void *arg) { if (net_kind(fd)) return net_ioctl(fd, req, arg); SETERR(L_ENOTTY); return -1; }
static SYSV int s_tcgetattr(int fd, void *t) { SETERR(L_ENOTTY); return -1; }
static SYSV int s_tcsetattr(int fd, int act, const void *t) { SETERR(L_ENOTTY); return -1; }
static SYSV int s_pipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) ? fail() : 0; }

// --- stat (Linux x86_64 struct stat, 144 bytes) ---------------------------------------------------------
typedef struct { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks;
                 int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, reserved[3]; } LStat;
static void to_linux(const struct _stat64 *s, LStat *l) {
    memset(l, 0, sizeof *l);
    l->dev = s->st_dev; l->ino = s->st_ino; l->nlink = s->st_nlink ? s->st_nlink : 1;
    l->mode = (s->st_mode & _S_IFDIR ? 0040000 | 0771 : s->st_mode & _S_IFCHR ? 0020000 | 0666 : 0100000 | 0660);
    l->uid = l->gid = 10000;  // the app's uid, same as getuid()
    l->size = s->st_size; l->blksize = 4096; l->blocks = (s->st_size + 511) / 512;
    l->atime = s->st_atime; l->mtime = s->st_mtime; l->ctime = s->st_ctime;
}
static SYSV int s_stat(const char *path, LStat *l) {
    if (is_dev_random(path)) { memset(l, 0, sizeof *l); l->mode = 0020000 | 0666; l->nlink = 1; return 0; }
    wchar_t w[MAX_PATH * 2]; struct _stat64 s;
    if (!host_path(path, w)) return -1;
    return _wstat64(w, &s) ? fail() : (to_linux(&s, l), 0);
}
static SYSV int s_fstat(int fd, LStat *l) {
    struct _stat64 s;
    if (_fstat64(fd, &s)) return fail();
    to_linux(&s, l);
    if (fd >= 0 && fd < 8192 && random_fd[fd]) l->mode = 0020000 | 0666;
    if (net_kind(fd)) l->mode = net_mode(fd);
    return 0;
}
static SYSV int s_access(const char *path, int mode) {
    if (is_dev_random(path)) return mode & 2 ? (SETERR(L_EACCES), -1) : 0;
    wchar_t w[MAX_PATH * 2];
    if (!host_path(path, w)) return -1;
    return _waccess(w, mode & 6) ? fail() : 0;  // X_OK: no exec bits on Windows; existence is all we can say
}
typedef struct { uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flag, namemax; uint32_t spare[6]; } LStatvfs;  // bionic LP64: 112 bytes
_Static_assert(sizeof(LStatvfs) == 112, "bionic statvfs");
static SYSV int s_statvfs(const char *path, LStatvfs *v) {
    wchar_t w[MAX_PATH * 2]; ULARGE_INTEGER avail, total, free;
    if (!host_path(path, w)) return -1;
    if (!GetDiskFreeSpaceExW(root, &avail, &total, &free)) { SETERR(5); return -1; }
    memset(v, 0, sizeof *v);
    v->bsize = v->frsize = 4096; v->blocks = total.QuadPart / 4096; v->bfree = free.QuadPart / 4096; v->bavail = avail.QuadPart / 4096;
    v->files = v->ffree = v->favail = 1 << 20; v->namemax = 255;
    return 0;
}

// --- names ------------------------------------------------------------------------------------------------
static SYSV int s_mkdir(const char *path, int mode) { wchar_t w[MAX_PATH * 2]; if (!host_path(path, w)) return -1; return _wmkdir(w) ? fail() : 0; }
static SYSV int s_rmdir(const char *path) { wchar_t w[MAX_PATH * 2]; if (!host_path(path, w)) return -1; return _wrmdir(w) ? fail() : 0; }
static SYSV int s_unlink(const char *path) { wchar_t w[MAX_PATH * 2]; if (!host_path(path, w)) return -1; return _wunlink(w) ? fail() : 0; }
static SYSV int s_remove(const char *path) {
    wchar_t w[MAX_PATH * 2]; if (!host_path(path, w)) return -1;
    DWORD a = GetFileAttributesW(w);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? _wrmdir(w) : _wunlink(w)) ? fail() : 0;
}
static SYSV int s_rename(const char *from, const char *to) {
    wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2];
    if (!host_path(from, a) || !host_path(to, b)) return -1;
    if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING)) return 0;  // Linux rename replaces the target
    DWORD e = GetLastError();
    SETERR(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? L_ENOENT : e == ERROR_ACCESS_DENIED ? L_EACCES : e == ERROR_DIR_NOT_EMPTY ? L_ENOTEMPTY : 5);
    return -1;
}
static SYSV int64_t s_readlink(const char *path, char *buf, uint64_t n) {
    wchar_t w[MAX_PATH * 2];
    if (!host_path(path, w)) return -1;
    SETERR(GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES ? L_ENOENT : L_EINVAL);  // there are no symlinks in the sandbox
    return -1;
}
static SYSV int64_t s_readlink_chk(const char *path, char *buf, uint64_t n, uint64_t cap) { return s_readlink(path, buf, n); }
static SYSV char *s_realpath(const char *path, char *out) {
    char lp[1024]; wchar_t w[MAX_PATH * 2];
    if (!host_path(path, w)) return 0;
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) { SETERR(L_ENOENT); return 0; }
    normalise(path, lp, sizeof lp);
    if (!out) out = shim_malloc(4096);
    if (!out) { SETERR(12); return 0; }
    snprintf(out, 4096, "%s", lp);
    return out;
}
static SYSV char *s_getcwd(char *buf, uint64_t n) {
    if (!buf) { buf = shim_malloc(n ? n : 2); if (!buf) { SETERR(12); return 0; } n = n ? n : 2; }
    if (n < 2) { SETERR(34); return 0; }
    strcpy(buf, "/");
    return buf;
}
static SYSV int s_utime(const char *path, const void *t) { wchar_t w[MAX_PATH * 2]; if (!host_path(path, w)) return -1; return _waccess(w, 0) ? fail() : 0; }  // shortcut: times not set
static SYSV int s_fchmod(int fd, int mode) { return 0; }
static SYSV int s_fchown(int fd, int uid, int gid) { return 0; }

// --- directories (bionic struct dirent: ino, off, reclen, type, name[256]) ---------------------------------
typedef struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } LDirent;
typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int first, done; LDirent ent; } Dir;
static SYSV Dir *s_opendir(const char *path) {
    wchar_t w[MAX_PATH * 2 + 4];
    if (!host_path(path, w)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) { SETERR(L_ENOENT); return 0; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { SETERR(L_ENOTDIR); return 0; }
    wcscat(w, L"\\*");
    Dir *d = calloc(1, sizeof *d);
    if (!d) { SETERR(12); return 0; }
    d->h = FindFirstFileW(w, &d->fd);
    d->first = 1; d->done = d->h == INVALID_HANDLE_VALUE;
    return d;
}
static SYSV LDirent *s_readdir(Dir *d) {
    if (d->done) return 0;
    if (!d->first && !FindNextFileW(d->h, &d->fd)) { d->done = 1; return 0; }
    d->first = 0;
    memset(&d->ent, 0, sizeof d->ent);
    WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->ent.name, 256, 0, 0);
    d->ent.type = d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 4 : 8;  // DT_DIR, DT_REG
    d->ent.ino = ++d->ent.off; d->ent.reclen = sizeof d->ent;
    return &d->ent;
}
static SYSV int s_closedir(Dir *d) { if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h); free(d); return 0; }

// --- FILE streams: host CRT streams on sandbox paths (fmt.c treats any non-std FILE* as one) ------------
static char random_stream;  // the FILE* handed out for /dev/urandom; only fread/fclose/getc know it
#define RANDOM_FILE ((FILE *)&random_stream)
typedef struct { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; } MallInfo;
static SYSV MallInfo s_mallinfo(void) {
    PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    return (MallInfo){ .arena = pm.PrivateUsage, .uordblks = pm.PrivateUsage };
}
static SYSV FILE *s_fopen(const char *path, const char *mode) {
    wchar_t w[MAX_PATH * 2], m[8]; int k = 0;
    if (is_dev_random(path)) return RANDOM_FILE;
    int pfd = proc_open(path);
    if (pfd >= 0) return _fdopen(pfd, "rb");
    if (!host_path(path, w)) { if (!quiet_miss(path)) printf("fopen(%s) -> ENOENT\n", path ? path : "(null)"); return 0; }
    for (const char *c = mode; *c && k < 5; c++) if (strchr("rwa+", *c)) m[k++] = *c;  // drop e (cloexec), x, b
    m[k++] = L'b'; m[k] = 0;
    if (strchr(mode, 'x') && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) { SETERR(L_EEXIST); return 0; }
    FILE *f = _wfopen(w, m);
    if (trace_fs()) printf("fs: fopen(%s, %s) -> %s\n", path, mode, f ? "ok" : "fail");
    if (!f) fail();
    return f;
}
static SYSV FILE *s_fdopen(int fd, const char *mode) {
    char m[8]; int k = 0;
    for (const char *c = mode; *c && k < 5; c++) if (strchr("rwa+", *c)) m[k++] = *c;
    m[k++] = 'b'; m[k] = 0;
    FILE *f = _fdopen(fd, m);
    if (!f) fail();
    return f;
}

FILE *shim_host_file(void *f);  // fmt.c: bionic std streams -> host ones
static SYSV int s_fclose(FILE *f) { if (f == RANDOM_FILE) return 0; return fclose(shim_host_file(f)) ? fail() : 0; }
static SYSV uint64_t s_fread(void *p, uint64_t sz, uint64_t n, FILE *f) {
    if (f == RANDOM_FILE) {
        uint64_t total = sz * n;
        if (!sz || total / sz != n || total > 1 << 24) return 0;
        return BCryptGenRandom(0, p, (ULONG)total, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? 0 : n;
    }
    return fread(p, sz, n, shim_host_file(f));
}
static SYSV uint64_t s_fread_chk(void *p, uint64_t sz, uint64_t n, FILE *f, uint64_t cap) { return s_fread(p, sz, n, f); }
static SYSV uint64_t s_fwrite_chk(const void *p, uint64_t sz, uint64_t n, FILE *f, uint64_t cap) { return f == RANDOM_FILE ? n : fwrite(p, sz, n, shim_host_file(f)); }
static SYSV int s_getc(FILE *f) { unsigned char c; return f == RANDOM_FILE ? (s_fread(&c, 1, 1, f) ? c : -1) : fgetc(shim_host_file(f)); }
static SYSV int s_ungetc(int c, FILE *f) { return f == RANDOM_FILE ? c : ungetc(c, shim_host_file(f)); }
static SYSV char *s_fgets(char *s, int n, FILE *f) { return f == RANDOM_FILE ? 0 : fgets(s, n, shim_host_file(f)); }
static SYSV int s_fseeko(FILE *f, int64_t off, int whence) { return f == RANDOM_FILE ? 0 : _fseeki64(shim_host_file(f), off, whence) ? fail() : 0; }
static SYSV int s_fseek(FILE *f, int64_t off, int whence) { return s_fseeko(f, off, whence); }
static SYSV int64_t s_ftello(FILE *f) { return f == RANDOM_FILE ? 0 : _ftelli64(shim_host_file(f)); }
// shortcut: wide stdio is byte-wise (ASCII only); fine for logs, upgrade with UTF-8 decoding if text breaks
static SYSV uint32_t s_getwc(FILE *f) { int c = s_getc(f); return c < 0 ? 0xffffffffu : (uint32_t)c; }
static SYSV uint32_t s_ungetwc(uint32_t c, FILE *f) { return s_ungetc(c < 128 ? (int)c : '?', f) < 0 ? 0xffffffffu : c; }
static SYSV uint32_t s_fputwc(uint32_t c, FILE *f) { return fputc(c < 128 ? (int)c : '?', shim_host_file(f)) < 0 ? 0xffffffffu : c; }

static const struct { const char *name; void *at; } table[] = {
    { "fclose", s_fclose }, { "fread", s_fread }, { "__fread_chk", s_fread_chk }, { "__fwrite_chk", s_fwrite_chk },
    { "getc", s_getc }, { "ungetc", s_ungetc }, { "fgets", s_fgets }, { "fseek", s_fseek }, { "fseeko", s_fseeko },
    { "ftell", s_ftello }, { "ftello", s_ftello }, { "getwc", s_getwc }, { "ungetwc", s_ungetwc }, { "fputwc", s_fputwc },
    { "open", s_open }, { "__open_2", s_open_2 }, { "close", s_close }, { "read", s_read }, { "__read_chk", s_read_chk },
    { "write", s_write }, { "__write_chk", s_write_chk }, { "writev", s_writev }, { "lseek", s_lseek },
    { "pread", s_pread }, { "pread64", s_pread }, { "pwrite", s_pwrite }, { "fsync", s_fsync }, { "ftruncate", s_ftruncate },
    { "posix_fallocate", s_posix_fallocate }, { "fcntl", s_fcntl }, { "ioctl", s_ioctl }, { "tcgetattr", s_tcgetattr },
    { "tcsetattr", s_tcsetattr }, { "pipe", s_pipe }, { "stat", s_stat }, { "lstat", s_stat }, { "fstat", s_fstat },
    { "access", s_access }, { "statvfs", s_statvfs }, { "mkdir", s_mkdir }, { "rmdir", s_rmdir }, { "unlink", s_unlink },
    { "remove", s_remove }, { "rename", s_rename }, { "readlink", s_readlink }, { "__readlink_chk", s_readlink_chk },
    { "realpath", s_realpath }, { "getcwd", s_getcwd }, { "utime", s_utime }, { "utimes", s_utime }, { "fchmod", s_fchmod },
    { "fchown", s_fchown }, { "opendir", s_opendir }, { "readdir", s_readdir }, { "closedir", s_closedir },
    { "fopen", s_fopen }, { "fdopen", s_fdopen }, { "mallinfo", s_mallinfo },
};
void *shim_lookup_fs(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}

// self-check for the path rules (loader.exe --selftest)
int shim_fs_selftest(void) {
    char o[1024]; int bad = 0;
    struct { const char *in, *want; } t[] = { { "/a/../../b", "/b" }, { "x/./y//z", "/x/y/z" }, { "/data/user/0/files", "/data/data/files" },
                                              { "/data/user/0", "/data/data" }, { "/..", "/" }, { "/", "/" } };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (!normalise(t[i].in, o, sizeof o) || strcmp(o, t[i].want)) { printf("normalise(%s) = %s, want %s\n", t[i].in, o, t[i].want); bad++; }
    const char *pf[] = { "/proc/meminfo", "/proc/self/statm", "/proc/self/status", "/proc/self/stat" }, *key[] = { "MemTotal:", " ", "VmRSS:", "(com.roblox.client)" };
    for (int i = 0; i < 4; i++) {
        char b[2048] = { 0 }; int fd = s_open(pf[i], 0, 0);
        if (fd < 0 || _read(fd, b, sizeof b - 1) <= 0 || !strstr(b, key[i])) { bad++; printf("proc: %s bad\n", pf[i]); }
        if (fd >= 0) _close(fd);
    }
    {   // maps: this process must list its own exe, with sane lines
        static char m[1 << 20]; int fd = s_open("/proc/self/maps", 0, 0), n = fd < 0 ? -1 : _read(fd, m, sizeof m - 1);
        if (n <= 0 || !strstr(m, "loader.exe") || !strstr(m, " r-xp ")) { bad++; printf("proc: maps bad (%d)\n", n); }
        if (fd >= 0) _close(fd);
    }
    if (s_mallinfo().uordblks == 0) { bad++; printf("mallinfo: zero\n"); }
    return bad;
}
