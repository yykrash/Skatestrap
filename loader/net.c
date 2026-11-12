// Sockets, eventfd, timerfd, epoll, poll and select for libroblox.so, on Winsock.
// Every one of these is a Linux fd: a CRT fd opened on NUL (so the number is real and unique) plus an entry in
// obj[] saying what it stands for. fs.c sends read/write/close/fcntl/ioctl/fstat on these fds here.
// eventfds own a loopback UDP socket that holds one datagram while the counter is non-zero, so one WSAPoll
// waits on sockets and eventfds together.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "shim.h"

int *shim_errno(void);
int64_t now_ns(int clock);
void hr_sleep(int64_t ns);
// SKATESTRAP_OLD_WAITS=1: the old whole-ms waits (Sleep, WSAPoll rounded up), for A/B runs
static int old_waits(void) { static int t = -1; if (t < 0) { const char *e = getenv("SKATESTRAP_OLD_WAITS"); t = e && *e == '1'; } return t; }
#define SETERR(e) (*shim_errno() = (e))
enum { L_EINTR = 4, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EEXIST = 17,
       L_EINVAL = 22, L_EMFILE = 24, L_ENOTTY = 25, L_ENOENT = 2, L_ENOSYS = 38, L_ENOTSOCK = 88, L_EOPNOTSUPP = 95 };
enum { K_SOCK = 1, K_EVENT, K_TIMER, K_EPOLL };
#define MAXFD 8192

typedef struct { int fd; uint32_t events; uint64_t data; } EpItem;
typedef struct {
    int kind, nonblock;
    SOCKET s;                       // the socket (K_SOCK), or the eventfd's wake socket (K_EVENT)
    CRITICAL_SECTION lock;          // K_EVENT, K_TIMER, K_EPOLL
    uint64_t count; int semaphore;  // K_EVENT
    HANDLE ev;                      // K_EVENT: manual-reset, set while count > 0 (the sub-ms waits use it)
    int clock; int64_t next, interval;  // K_TIMER: next expiry in now_ns(1) time, 0 = disarmed
    EpItem *items; int nitems, cap; // K_EPOLL
} Obj;
static Obj *obj[MAXFD];

int net_kind(int fd) { return fd >= 0 && fd < MAXFD && obj[fd] ? obj[fd]->kind : 0; }
static Obj *get(int fd, int kind) {
    Obj *o = fd >= 0 && fd < MAXFD ? obj[fd] : 0;
    if (!o) { SETERR(L_EBADF); return 0; }
    if (kind && o->kind != kind) { SETERR(kind == K_SOCK ? L_ENOTSOCK : L_EINVAL); return 0; }
    return o;
}
static int new_fd(Obj *o) {
    int fd = _open("NUL", _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd < 0 || fd >= MAXFD) { if (fd >= 0) _close(fd); SETERR(L_EMFILE); return -1; }
    InitializeCriticalSection(&o->lock);
    obj[fd] = o;
    return fd;
}

static INIT_ONCE wsa_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK wsa_start(PINIT_ONCE o, void *p, void **c) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }
static void wsa(void) { InitOnceExecuteOnce(&wsa_once, wsa_start, 0, 0); }

static int linux_err(int w) {
    switch (w) {
    case WSAEWOULDBLOCK: return L_EAGAIN;   case WSAEINPROGRESS: return 115; case WSAEALREADY: return 114;
    case WSAENOTSOCK: return 88;            case WSAEDESTADDRREQ: return 89; case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;          case WSAENOPROTOOPT: return 92;  case WSAEPROTONOSUPPORT: return 93;
    case WSAESOCKTNOSUPPORT: return 94;     case WSAEOPNOTSUPP: return 95;   case WSAEAFNOSUPPORT: return 97;
    case WSAEADDRINUSE: return 98;          case WSAEADDRNOTAVAIL: return 99; case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;        case WSAENETRESET: return 102;   case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return 104;         case WSAENOBUFS: return 105;     case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;           case WSAESHUTDOWN: return 108;   case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;       case WSAEHOSTDOWN: return 112;   case WSAEHOSTUNREACH: return 113;
    case WSAEINTR: return L_EINTR;          case WSAEBADF: return L_EBADF;   case WSAEACCES: return L_EACCES;
    case WSAEFAULT: return L_EFAULT;        case WSAEINVAL: return L_EINVAL; case WSAEMFILE: return L_EMFILE;
    }
    return 5;  // EIO
}
static int wfail(void) { SETERR(linux_err(WSAGetLastError())); return -1; }

// sockaddr: same layout on both sides; only AF_INET6 differs (Linux 10, Windows 23)
static int fam_in(int f) { return f == 10 ? AF_INET6 : f; }
static int fam_out(int f) { return f == AF_INET6 ? 10 : f; }
static int addr_in(const void *a, int len, SOCKADDR_STORAGE *out) {
    if (!a) return 0;
    if (len < 2 || len > (int)sizeof *out) { SETERR(L_EINVAL); return -1; }
    memcpy(out, a, len);
    out->ss_family = fam_in(((const uint16_t *)a)[0]);
    return len;
}
static void addr_out(const SOCKADDR_STORAGE *s, int slen, void *a, uint32_t *alen) {
    if (!a || !alen) return;
    SOCKADDR_STORAGE t = *s;
    t.ss_family = fam_out(s->ss_family);
    memcpy(a, &t, (uint32_t)slen < *alen ? (uint32_t)slen : *alen);
    *alen = slen;
}

// --- sockets ---------------------------------------------------------------------------------------------
static int set_nonblock(Obj *o, int on) { u_long v = on; o->nonblock = on; return ioctlsocket(o->s, FIONBIO, &v); }
static int wrap_socket(SOCKET s, int flags) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = K_SOCK; o->s = s;
    int fd = new_fd(o);
    if (fd < 0) { closesocket(s); free(o); return -1; }
    if (flags & 0x800) set_nonblock(o, 1);  // SOCK_NONBLOCK
    return fd;
}
static int trace_net(void);
static SYSV int s_socket(int domain, int type, int proto) {
    wsa();
    SOCKET s = socket(fam_in(domain), type & 0xf, proto);
    if (s == INVALID_SOCKET) return wfail();
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return wrap_socket(s, type);
}
static SYSV int s_connect(int fd, const void *a, uint32_t len) {
    Obj *o = get(fd, K_SOCK); SOCKADDR_STORAGE w; int n;
    if (!o || (n = addr_in(a, len, &w)) < 0) return -1;
    int r = connect(o->s, (SOCKADDR *)&w, n), e = r ? WSAGetLastError() : 0;
    if (trace_net()) {
        char host[64] = "?"; DWORD hl = sizeof host;
        WSAAddressToStringA((SOCKADDR *)&w, n, 0, host, &hl);
        printf("net: [%lu] connect(%d, %s) -> %d\n", GetTickCount() % 100000, fd, host, e);
    }
    if (!r) return 0;
    if (e == WSAEWOULDBLOCK) { SETERR(115); return -1; }  // EINPROGRESS, as Linux says for non-blocking connect
    if (e == WSAEINVAL && o->nonblock) { SETERR(114); return -1; }  // EALREADY
    SETERR(linux_err(e)); return -1;
}
static SYSV int s_bind(int fd, const void *a, uint32_t len) {
    Obj *o = get(fd, K_SOCK); SOCKADDR_STORAGE w; int n;
    if (!o || (n = addr_in(a, len, &w)) < 0) return -1;
    return bind(o->s, (SOCKADDR *)&w, n) ? wfail() : 0;
}
static SYSV int s_listen(int fd, int backlog) { Obj *o = get(fd, K_SOCK); return !o ? -1 : listen(o->s, backlog) ? wfail() : 0; }
static SYSV int s_accept4(int fd, void *a, uint32_t *alen, int flags) {
    Obj *o = get(fd, K_SOCK);
    if (!o) return -1;
    SOCKADDR_STORAGE w; int n = sizeof w;
    SOCKET s = accept(o->s, (SOCKADDR *)&w, &n);
    if (s == INVALID_SOCKET) return wfail();
    addr_out(&w, n, a, alen);
    u_long off = 0; ioctlsocket(s, FIONBIO, &off);  // Windows copies the listener's mode; Linux never does
    return wrap_socket(s, flags);
}
static SYSV int s_accept(int fd, void *a, uint32_t *alen) { return s_accept4(fd, a, alen, 0); }
static SYSV int s_getsockname(int fd, void *a, uint32_t *alen) {
    Obj *o = get(fd, K_SOCK); SOCKADDR_STORAGE w; int n = sizeof w;
    if (!o) return -1;
    if (getsockname(o->s, (SOCKADDR *)&w, &n)) return wfail();
    addr_out(&w, n, a, alen); return 0;
}
static SYSV int s_getpeername(int fd, void *a, uint32_t *alen) {
    Obj *o = get(fd, K_SOCK); SOCKADDR_STORAGE w; int n = sizeof w;
    if (!o) return -1;
    if (getpeername(o->s, (SOCKADDR *)&w, &n)) return wfail();
    addr_out(&w, n, a, alen); return 0;
}
static SYSV int s_shutdown(int fd, int how) {
    if (trace_net()) printf("net: shutdown(%d, %d)\n", fd, how); Obj *o = get(fd, K_SOCK); return !o ? -1 : shutdown(o->s, how) ? wfail() : 0; }

// MSG_ flags: OOB/PEEK/DONTROUTE match; WAITALL 0x100 -> 8; DONTWAIT 0x40 is done by flipping the mode;
// NOSIGNAL 0x4000 needs nothing (Windows has no SIGPIPE).
static int msg_flags(int f) { return (f & 7) | (f & 0x100 ? 8 : 0); }
static int dontwait_begin(Obj *o, int f) { if ((f & 0x40) && !o->nonblock) { u_long v = 1; ioctlsocket(o->s, FIONBIO, &v); return 1; } return 0; }
static void dontwait_end(Obj *o, int on) { if (on) { u_long v = 0; ioctlsocket(o->s, FIONBIO, &v); } }  // shortcut: races a blocking call on another thread

static int trace_net(void) { static int t = -1; if (t < 0) t = getenv("SKATESTRAP_TRACE_NET") != 0; return t; }
static int64_t xfer(Obj *o, WSABUF *b, int nb, int flags, int sending, void *a, uint32_t *alen, const void *to, uint32_t tolen) {
    DWORD done = 0, fl = msg_flags(flags);
    SOCKADDR_STORAGE w; int wl = sizeof w, tl = 0;
    if (sending && to && (tl = addr_in(to, tolen, &w)) < 0) return -1;
    int dw = dontwait_begin(o, flags), r;
    if (sending) r = WSASendTo(o->s, b, nb, &done, fl, tl ? (SOCKADDR *)&w : 0, tl, 0, 0);
    else r = WSARecvFrom(o->s, b, nb, &done, &fl, a ? (SOCKADDR *)&w : 0, a ? &wl : 0, 0, 0);
    int e = r ? WSAGetLastError() : 0;
    dontwait_end(o, dw);
    if (e == WSAEMSGSIZE && !sending) e = 0;  // Linux returns the truncated datagram
    if (trace_net()) printf("net: [%lu] %s(%d bytes) -> %ld err %d\n", GetTickCount() % 100000, sending ? "send" : "recv", (int)b[0].len, (long)done, e);
    if (e) { SETERR(linux_err(e)); return -1; }
    if (!sending && a) addr_out(&w, wl, a, alen);
    return done;
}
// Android answers "localhost" for every app
static SYSV int s_gethostname(char *name, uint64_t n) { if (n < 10) return -1; memcpy(name, "localhost", 10); return 0; }
static SYSV int64_t s_sendto(int fd, const void *buf, uint64_t n, int flags, const void *to, uint32_t tolen) {
    Obj *o = get(fd, K_SOCK); WSABUF b = { (ULONG)(n > 0x7fffffff ? 0x7fffffff : n), (char *)buf };
    return o ? xfer(o, &b, 1, flags, 1, 0, 0, to, tolen) : -1;
}
static SYSV int64_t s_recvfrom(int fd, void *buf, uint64_t n, int flags, void *a, uint32_t *alen) {
    Obj *o = get(fd, K_SOCK); WSABUF b = { (ULONG)(n > 0x7fffffff ? 0x7fffffff : n), buf };
    return o ? xfer(o, &b, 1, flags, 0, a, alen, 0, 0) : -1;
}
typedef struct { void *name; uint32_t namelen; struct { void *base; uint64_t len; } *iov; uint64_t iovlen; void *control; uint64_t controllen; int flags; } LMsg;
typedef struct { LMsg hdr; uint32_t len; } LMMsg;
static int64_t msg(Obj *o, LMsg *m, int flags, int sending) {
    WSABUF b[64];
    if (m->iovlen > 64) { SETERR(L_EINVAL); return -1; }  // shortcut: 64 buffers per message, raise if hit
    for (uint64_t i = 0; i < m->iovlen; i++) { b[i].buf = m->iov[i].base; b[i].len = (ULONG)m->iov[i].len; }
    if (sending && m->controllen) { static int once; if (!once++) printf("sendmsg: control data dropped\n"); }
    int64_t r = xfer(o, b, (int)m->iovlen, flags, sending, m->name, &m->namelen, m->name, m->namelen);
    if (!sending && r >= 0) { m->controllen = 0; m->flags = 0; }
    return r;
}
static SYSV int64_t s_sendmsg(int fd, LMsg *m, int flags) { Obj *o = get(fd, K_SOCK); return o ? msg(o, m, flags, 1) : -1; }
static SYSV int64_t s_recvmsg(int fd, LMsg *m, int flags) { Obj *o = get(fd, K_SOCK); return o ? msg(o, m, flags, 0) : -1; }
static int mmsg(int fd, LMMsg *v, unsigned n, int flags, int sending) {
    Obj *o = get(fd, K_SOCK);
    if (!o) return -1;
    unsigned i = 0;
    for (; i < n; i++) {
        int64_t r = msg(o, &v[i].hdr, i && !sending ? flags | 0x40 : flags, sending);  // after the first, take only what is there
        if (r < 0) return i ? (int)i : -1;
        v[i].len = (uint32_t)r;
    }
    return (int)i;
}
static SYSV int s_sendmmsg(int fd, LMMsg *v, unsigned n, int flags) { return mmsg(fd, v, n, flags, 1); }
static SYSV int s_recvmmsg(int fd, LMMsg *v, unsigned n, int flags, void *timeout) { return mmsg(fd, v, n, flags & ~0x10000, 0); }

// socket options: SOL_SOCKET and option numbers differ; IPPROTO_TCP/IP/IPV6 partly
static int opt_map(int level, int name, int *wl, int *wn) {
    *wl = level; *wn = name;
    if (level == 1) {  // SOL_SOCKET
        *wl = SOL_SOCKET;
        switch (name) {
        case 2: *wn = SO_REUSEADDR; return 1;  case 3: *wn = SO_TYPE; return 1;    case 4: *wn = SO_ERROR; return 1;
        case 6: *wn = SO_BROADCAST; return 1;  case 7: *wn = SO_SNDBUF; return 1;  case 8: *wn = SO_RCVBUF; return 1;
        case 9: *wn = SO_KEEPALIVE; return 1;  case 13: *wn = SO_LINGER; return 1; case 20: *wn = SO_RCVTIMEO; return 1;
        case 21: *wn = SO_SNDTIMEO; return 1;  case 30: *wn = SO_ACCEPTCONN; return 1;
        }
        return 0;
    }
    if (level == IPPROTO_TCP) switch (name) { case 1: return 1; case 4: *wn = 3; return 1; case 5: *wn = 17; return 1; case 6: *wn = 16; return 1; }
    if (level == IPPROTO_IP) switch (name) { case 1: *wn = IP_TOS; return 1; case 2: *wn = IP_TTL; return 1;
                                             case 32: *wn = IP_MULTICAST_IF; return 1; case 33: *wn = IP_MULTICAST_TTL; return 1;
                                             case 34: *wn = IP_MULTICAST_LOOP; return 1; }
    if (level == IPPROTO_IPV6) switch (name) { case 26: *wn = IPV6_V6ONLY; return 1; case 16: *wn = IPV6_UNICAST_HOPS; return 1; }
    return 0;
}
static SYSV int s_setsockopt(int fd, int level, int name, const void *v, uint32_t len) {
    Obj *o = get(fd, K_SOCK); int wl, wn;
    if (!o) return -1;
    if (!opt_map(level, name, &wl, &wn)) { printf("setsockopt(%d, %d) not on Windows: ignored\n", level, name); return 0; }
    if (wl == SOL_SOCKET && (wn == SO_RCVTIMEO || wn == SO_SNDTIMEO)) {  // struct timeval -> DWORD ms
        const int64_t *tv = v; DWORD ms = (DWORD)(tv[0] * 1000 + tv[1] / 1000);
        return setsockopt(o->s, wl, wn, (char *)&ms, sizeof ms) ? wfail() : 0;
    }
    if (wl == SOL_SOCKET && wn == SO_LINGER) {
        const int *l = v; struct linger w = { (u_short)!!l[0], (u_short)l[1] };
        return setsockopt(o->s, wl, wn, (char *)&w, sizeof w) ? wfail() : 0;
    }
    return setsockopt(o->s, wl, wn, v, len) ? wfail() : 0;
}
static SYSV int s_getsockopt(int fd, int level, int name, void *v, uint32_t *len) {
    Obj *o = get(fd, K_SOCK); int wl, wn;
    if (!o) return -1;
    if (!opt_map(level, name, &wl, &wn)) { printf("getsockopt(%d, %d) not on Windows\n", level, name); SETERR(92); return -1; }
    char buf[64]; int n = sizeof buf;
    if (getsockopt(o->s, wl, wn, buf, &n)) return wfail();
    if (wl == SOL_SOCKET && (wn == SO_RCVTIMEO || wn == SO_SNDTIMEO)) {
        DWORD ms = *(DWORD *)buf; int64_t tv[2] = { ms / 1000, (ms % 1000) * 1000 };
        memcpy(v, tv, *len < 16 ? *len : 16); *len = 16; return 0;
    }
    if (wl == SOL_SOCKET && wn == SO_LINGER) {
        struct linger *w = (struct linger *)buf; int l[2] = { w->l_onoff, w->l_linger };
        memcpy(v, l, *len < 8 ? *len : 8); *len = 8; return 0;
    }
    if (wl == SOL_SOCKET && wn == SO_ERROR) *(int *)buf = *(int *)buf ? linux_err(*(int *)buf) : 0;
    if (trace_net()) printf("net: getsockopt(%d, %d, %d) -> %d\n", fd, level, name, *(int *)buf);
    if (n < 4) { int x = n == 1 ? *(uint8_t *)buf : 0; memcpy(buf, &x, 4); n = 4; }  // BOOL-as-char answers
    memcpy(v, buf, *len < (uint32_t)n ? *len : (uint32_t)n); *len = n;
    return 0;
}
static SYSV int s_socketpair(int domain, int type, int proto, int sv[2]) {
    // shortcut: a connected loopback TCP (or UDP) pair stands in for AF_UNIX; fine for in-process pipes
    wsa();
    int dgram = (type & 0xf) == 2;
    SOCKET l = socket(AF_INET, dgram ? SOCK_DGRAM : SOCK_STREAM, 0), a = INVALID_SOCKET, b = INVALID_SOCKET;
    struct sockaddr_in in = { AF_INET }; int n = sizeof in;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == INVALID_SOCKET || bind(l, (SOCKADDR *)&in, n) || getsockname(l, (SOCKADDR *)&in, &n)) goto bad;
    b = socket(AF_INET, dgram ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (dgram) {
        struct sockaddr_in bi = { AF_INET }; int bn = sizeof bi; bi.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (b == INVALID_SOCKET || bind(b, (SOCKADDR *)&bi, bn) || getsockname(b, (SOCKADDR *)&bi, &bn) ||
            connect(b, (SOCKADDR *)&in, n) || connect(l, (SOCKADDR *)&bi, bn)) goto bad;
        a = l; l = INVALID_SOCKET;
    } else {
        if (listen(l, 1) || b == INVALID_SOCKET || connect(b, (SOCKADDR *)&in, n)) goto bad;
        a = accept(l, 0, 0);
        if (a == INVALID_SOCKET) goto bad;
        closesocket(l); l = INVALID_SOCKET;
        BOOL on = 1; setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof on); setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof on);
    }
    sv[0] = wrap_socket(a, type); sv[1] = wrap_socket(b, type);
    return sv[0] < 0 || sv[1] < 0 ? -1 : 0;
bad:
    { int e = wfail(); if (l != INVALID_SOCKET) closesocket(l); if (a != INVALID_SOCKET) closesocket(a); if (b != INVALID_SOCKET) closesocket(b); return e; }
}

// --- eventfd -----------------------------------------------------------------------------------------------
static void wake_drain(Obj *o) { char c[8]; while (recv(o->s, c, sizeof c, 0) > 0) {} }
static SYSV int s_eventfd(unsigned initval, int flags) {
    wsa();
    Obj *o = calloc(1, sizeof *o);
    o->kind = K_EVENT; o->count = initval; o->semaphore = flags & 1; o->nonblock = !!(flags & 0x800);
    struct sockaddr_in in = { AF_INET }; int n = sizeof in; u_long on = 1;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    o->s = socket(AF_INET, SOCK_DGRAM, 0);
    if (o->s == INVALID_SOCKET || bind(o->s, (SOCKADDR *)&in, n) || getsockname(o->s, (SOCKADDR *)&in, &n) ||
        connect(o->s, (SOCKADDR *)&in, n) || ioctlsocket(o->s, FIONBIO, &on)) {
        int e = wfail(); if (o->s != INVALID_SOCKET) closesocket(o->s); free(o); return e;
    }
    if (initval) send(o->s, "", 1, 0);
    o->ev = CreateEventW(0, TRUE, initval != 0, 0);
    int fd = new_fd(o);
    if (fd < 0) { closesocket(o->s); CloseHandle(o->ev); free(o); }
    return fd;
}
static int64_t event_read(Obj *o, void *buf, uint64_t n) {
    if (n < 8) { SETERR(L_EINVAL); return -1; }
    for (;;) {
        EnterCriticalSection(&o->lock);
        if (o->count) {
            uint64_t v = o->semaphore ? 1 : o->count;
            o->count -= v;
            if (!o->count) { wake_drain(o); ResetEvent(o->ev); }
            LeaveCriticalSection(&o->lock);
            memcpy(buf, &v, 8);
            return 8;
        }
        LeaveCriticalSection(&o->lock);
        if (o->nonblock) { SETERR(L_EAGAIN); return -1; }
        WSAPOLLFD p = { o->s, POLLRDNORM };
        WSAPoll(&p, 1, -1);
    }
}
static int64_t event_write(Obj *o, const void *buf, uint64_t n) {
    uint64_t v;
    if (n < 8) { SETERR(L_EINVAL); return -1; }
    memcpy(&v, buf, 8);
    if (v == ~0ull) { SETERR(L_EINVAL); return -1; }
    EnterCriticalSection(&o->lock);
    if (!o->count && v) { send(o->s, "", 1, 0); SetEvent(o->ev); }  // one datagram while count > 0
    o->count += v;  // shortcut: no blocking at 2^64-2, nothing realistic gets there
    LeaveCriticalSection(&o->lock);
    return 8;
}

// --- timerfd -----------------------------------------------------------------------------------------------
typedef struct { int64_t sec, nsec; } Ts;
typedef struct { Ts interval, value; } ITs;
static int64_t ts_ns(const Ts *t) { return t->sec * 1000000000 + t->nsec; }
static Ts ns_ts(int64_t n) { return (Ts){ n / 1000000000, n % 1000000000 }; }
static SYSV int s_timerfd_create(int clock, int flags) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = K_TIMER; o->clock = clock; o->nonblock = !!(flags & 0x800);
    int fd = new_fd(o);
    if (fd < 0) free(o);
    return fd;
}
static SYSV int s_timerfd_settime(int fd, int flags, const ITs *nv, ITs *old) {
    Obj *o = get(fd, K_TIMER);
    if (!o) return -1;
    { static int seen; if (getenv("SKATESTRAP_TRACE_WAIT") && seen++ < 40) printf("stimer fd=%d flags=%d value=%lld ns interval=%lld ns tid=%lu\n", fd, flags, (long long)ts_ns(&nv->value), (long long)ts_ns(&nv->interval), GetCurrentThreadId()); }
    EnterCriticalSection(&o->lock);
    int64_t now = now_ns(1);
    if (old) { old->interval = ns_ts(o->interval); old->value = ns_ts(o->next ? (o->next > now ? o->next - now : 1) : 0); }
    int64_t v = ts_ns(&nv->value);
    if (!v) o->next = 0;
    else if (flags & 1) o->next = now + (v - now_ns(o->clock == 0 ? 0 : 1));  // TFD_TIMER_ABSTIME on the timer's clock
    else o->next = now + v;
    if (o->next && o->next <= now) o->next = now;  // already due: fires on the next check (0 means disarmed)
    o->interval = ts_ns(&nv->interval);
    LeaveCriticalSection(&o->lock);
    return 0;
}
static uint64_t timer_take(Obj *o) {  // expirations since the last read, re-arming as it goes
    int64_t now = now_ns(1);
    if (!o->next || now < o->next) return 0;
    uint64_t n = 1;
    if (o->interval) { n += (now - o->next) / o->interval; o->next += n * o->interval; } else o->next = 0;
    return n;
}
static int timer_due(Obj *o, int64_t now) { return o->next && now >= o->next; }
static int64_t timer_read(Obj *o, void *buf, uint64_t n) {
    if (n < 8) { SETERR(L_EINVAL); return -1; }
    for (;;) {
        EnterCriticalSection(&o->lock);
        uint64_t v = timer_take(o); int64_t next = o->next;
        LeaveCriticalSection(&o->lock);
        if (v) { memcpy(buf, &v, 8); return 8; }
        if (o->nonblock) { SETERR(L_EAGAIN); return -1; }
        // shortcut: a re-arm to an earlier time is seen at the old expiry, wait on an event if a reader hangs on that
        if (next && !old_waits()) { int64_t d = next - now_ns(1); hr_sleep(d <= 50000 ? 0 : d > 600000 ? d - 500000 : d < 100000 ? d : 100000); continue; }  // as core_poll
        int64_t wait = next ? (next - now_ns(1)) / 1000000 + 1 : 100;  // disarmed: re-check every 100 ms
        Sleep((DWORD)(wait < 1 ? 1 : wait));
    }
}

// --- poll (core of epoll and select too) --------------------------------------------------------------------
typedef struct { int fd; short events, revents; } LPoll;
enum { P_IN = 1, P_PRI = 2, P_OUT = 4, P_ERR = 8, P_HUP = 0x10, P_NVAL = 0x20 };
// hr_sleep that also ends as soon as one of h[0..nh) (eventfd events) is set; h needs room for one more
static void fine_wait(HANDLE *h, int nh, int64_t ns) {
    static __declspec(thread) HANDLE t;  // shortcut: one timer handle per thread, never closed; fine while threads are long-lived
    if (nh && !t) t = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = { .QuadPart = -(ns + 99) / 100 };
    if (!nh || !t || !SetWaitableTimerEx(t, &due, 0, 0, 0, 0, 0)) { hr_sleep(ns); return; }
    h[nh] = t; WaitForMultipleObjects(nh + 1, h, FALSE, INFINITE);
}
static int core_poll(LPoll *p, unsigned n, int64_t tns) {  // tns: timeout in ns, < 0 = none
    WSAPOLLFD stack[64], *w = n <= 64 ? stack : malloc(n * sizeof *w);
    int *map = malloc((n ? n : 1) * sizeof *map);
    int64_t deadline = tns < 0 ? -1 : now_ns(1) + tns;
    int ready;
    for (;;) {
        int nw = 0, any_pipe = 0, nh = 0; int64_t now = now_ns(1), next_timer = -1; HANDLE hev[64];
        ready = 0;
        for (unsigned i = 0; i < n; i++) {
            p[i].revents = 0; map[i] = -1;
            int fd = p[i].fd;
            if (fd < 0) continue;
            Obj *o = fd < MAXFD ? obj[fd] : 0;
            if (o && (o->kind == K_SOCK || o->kind == K_EVENT)) {
                w[nw] = (WSAPOLLFD){ o->s, (SHORT)(o->kind == K_EVENT ? POLLRDNORM : ((p[i].events & P_IN ? POLLRDNORM : 0) | (p[i].events & P_OUT ? POLLWRNORM : 0))) };
                if (o->kind == K_EVENT) p[i].revents |= p[i].events & P_OUT;  // always writable
                if (o->kind == K_EVENT && (p[i].events & P_IN) && nh < 63) hev[nh++] = o->ev;  // shortcut: past 63 eventfds the rest wake at the slice end
                map[i] = nw++;
            } else if (o && o->kind == K_TIMER) {
                if (timer_due(o, now)) p[i].revents = p[i].events & P_IN;
                else if (o->next && (next_timer < 0 || o->next < next_timer)) next_timer = o->next;
            } else if (o) {
                p[i].revents = P_NVAL;  // shortcut: epoll fds inside poll are not supported
            } else {
                intptr_t h = _get_osfhandle(fd);
                if (h == -1) p[i].revents = P_NVAL;
                else if (GetFileType((HANDLE)h) == FILE_TYPE_PIPE) {
                    DWORD avail = 0; any_pipe = 1;
                    if (!PeekNamedPipe((HANDLE)h, 0, 0, 0, &avail, 0)) p[i].revents = (p[i].events & P_IN) | P_HUP;
                    else p[i].revents = (avail ? p[i].events & P_IN : 0) | (p[i].events & P_OUT);
                } else p[i].revents = p[i].events & (P_IN | P_OUT);  // files are always ready
            }
            if (p[i].revents) ready++;
        }
        int64_t left, fine = 0;
        if (old_waits()) {
            left = deadline < 0 ? -1 : (deadline - now) / 1000000;
            if (left < -1 || (deadline >= 0 && left < 0)) left = 0;
            if (ready) left = 0;
            if (next_timer >= 0) { int64_t t = (next_timer - now) / 1000000 + 1; if (left < 0 || t < left) left = t; }
            if (any_pipe && (left < 0 || left > 10)) left = 10;  // shortcut: pipes can't be waited on with sockets; re-check every 10 ms
        } else {
            int64_t wait = ready ? 0 : deadline < 0 ? -1 : deadline > now ? deadline - now : 0;
            if (next_timer >= 0 && (wait < 0 || next_timer - now < wait)) wait = next_timer - now;
            if (any_pipe && (wait < 0 || wait > 10000000)) wait = 10000000;  // pipes: re-check every 10 ms, as above
            // WSAPoll waits whole ticks and can overshoot ~1.5 ms: it takes all but the last 2-3 ms, high-res slices finish the rest
            left = wait < 0 ? -1 : nw && wait >= 3000000 ? wait / 1000000 - 2 : 0;
            // a sleep ends on a system tick (0.5 ms here): 100 us slices stop at the last tick before the deadline, the last <= 50 us spins
            fine = left || wait <= 50000 ? 0 : !nw && wait > 600000 ? wait - 500000 : wait < 100000 ? wait : 100000;
        }
        if (nw) {
            int k = WSAPoll(w, nw, (INT)left);
            if (k < 0) { int e = wfail(); if (w != stack) free(w); free(map); return e; }
            if (!k && fine) fine_wait(hev, nh, fine);
            for (unsigned i = 0; i < n; i++) {
                if (map[i] < 0) continue;
                SHORT r = w[map[i]].revents; short add = 0;
                Obj *o = obj[p[i].fd];
                if (o->kind == K_EVENT) add = (r & POLLRDNORM) && o->count ? (p[i].events & P_IN) : 0;
                else add = (r & (POLLRDNORM | POLLRDBAND) ? P_IN & p[i].events : 0) | (r & POLLWRNORM ? P_OUT & p[i].events : 0) |
                           (r & POLLERR ? P_ERR : 0) | (r & POLLHUP ? P_HUP | (p[i].events & P_IN) : 0) | (r & POLLNVAL ? P_NVAL : 0);
                if (add && !p[i].revents) ready++;
                p[i].revents |= add;
            }
        } else if (left) Sleep(left < 0 ? INFINITE : (DWORD)left);
        else if (fine) hr_sleep(fine);
        if (ready || (deadline >= 0 && now_ns(1) >= deadline)) break;
        // timers may have come due during the wait: loop re-checks everything
        int timer_woke = 0;
        for (unsigned i = 0; i < n; i++) { Obj *o = p[i].fd >= 0 && p[i].fd < MAXFD ? obj[p[i].fd] : 0; if (o && o->kind == K_TIMER && timer_due(o, now_ns(1))) timer_woke = 1; }
        if (!timer_woke && !nw && !any_pipe && deadline < 0 && next_timer < 0) continue;
    }
    if (trace_net()) {
        int socks = 0;
        for (unsigned i = 0; i < n; i++) socks += p[i].fd >= 0 && p[i].fd < MAXFD && obj[p[i].fd] && obj[p[i].fd]->kind == K_SOCK;
        if (socks) {
            printf("net: poll(t=%d)", (int)(tns < 0 ? -1 : tns / 1000000));
            for (unsigned i = 0; i < n; i++) printf(" %d:%x>%x", p[i].fd, p[i].events, p[i].revents);
            printf(" -> %d\n", ready);
        }
    }
    if (w != stack) free(w);
    free(map);
    return ready;
}
void wait_trace(const char *, int64_t, int64_t, uint64_t *);
static SYSV int s_poll(LPoll *p, uint64_t n, int timeout) {
    int64_t t0 = now_ns(1); int r = core_poll(p, (unsigned)n, timeout < 0 ? -1 : timeout * 1000000ll);
    static int seen; int k;
    if (getenv("SKATESTRAP_TRACE_WAIT") && timeout <= 3 && ((k = seen++) < 30 || (k > 5000 && k < 5030))) {
        printf("spoll t=%d n=%d r=%d %.3fms tid=%lu:", timeout, (int)n, r, (now_ns(1) - t0) / 1e6, GetCurrentThreadId());
        for (unsigned i = 0; i < n && i < 8; i++) { Obj *o = p[i].fd >= 0 && p[i].fd < MAXFD ? obj[p[i].fd] : 0; printf(" %d(k%d):%x>%x", p[i].fd, o ? (int)o->kind : -1, p[i].events, p[i].revents); }
        printf("\n");
    }
    wait_trace("poll", timeout, t0, __builtin_frame_address(0)); return r;
}

typedef struct { uint64_t bits[16]; } LFdSet;  // Linux fd_set: 1024 bits
static SYSV int s_select(int nfds, LFdSet *r, LFdSet *wr, LFdSet *ex, int64_t *tv) {
    LPoll p[1024]; unsigned n = 0;
    if (nfds > 1024) nfds = 1024;
    for (int fd = 0; fd < nfds; fd++) {
        uint64_t m = 1ull << (fd & 63); short ev = 0;
        if (r && r->bits[fd >> 6] & m) ev |= P_IN;
        if (wr && wr->bits[fd >> 6] & m) ev |= P_OUT;
        if (ex && ex->bits[fd >> 6] & m) ev |= P_PRI;
        if (ev) p[n++] = (LPoll){ fd, ev, 0 };
    }
    int64_t tns = !tv ? -1 : old_waits() ? (tv[0] * 1000 + tv[1] / 1000) * 1000000 : tv[0] * 1000000000 + tv[1] * 1000;  // timeval to ns; old: whole ms
    int c = core_poll(p, n, tns);
    if (c < 0) return c;
    if (r) memset(r, 0, sizeof *r);
    if (wr) memset(wr, 0, sizeof *wr);
    if (ex) memset(ex, 0, sizeof *ex);
    int total = 0;
    for (unsigned i = 0; i < n; i++) {
        int fd = p[i].fd; uint64_t m = 1ull << (fd & 63);
        if (r && p[i].revents & (P_IN | P_HUP | P_ERR) && p[i].events & P_IN) { r->bits[fd >> 6] |= m; total++; }
        if (wr && p[i].revents & (P_OUT | P_ERR) && p[i].events & P_OUT) { wr->bits[fd >> 6] |= m; total++; }
        if (ex && p[i].revents & P_PRI) { ex->bits[fd >> 6] |= m; total++; }
    }
    return total;
}

// bionic FORTIFY variants of the FD_* macros (fd_set is 1024 bits, so these fire on the select path)
static SYSV void s_fd_set_chk(int fd, LFdSet *s, uint64_t n) { if (s && (unsigned)fd < 1024) s->bits[fd >> 6] |= 1ull << (fd & 63); }
static SYSV void s_fd_clr_chk(int fd, LFdSet *s, uint64_t n) { if (s && (unsigned)fd < 1024) s->bits[fd >> 6] &= ~(1ull << (fd & 63)); }
static SYSV int  s_fd_isset_chk(int fd, LFdSet *s, uint64_t n) { return s && (unsigned)fd < 1024 && (s->bits[fd >> 6] >> (fd & 63) & 1); }

// --- epoll -------------------------------------------------------------------------------------------------
#pragma pack(push, 4)
typedef struct { uint32_t events; uint64_t data; } LEpEv;  // x86-64 Linux packs this (12 bytes)
#pragma pack(pop)
static SYSV int s_epoll_create1(int flags) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = K_EPOLL;
    int fd = new_fd(o);
    if (fd < 0) free(o);
    return fd;
}
static SYSV int s_epoll_create(int size) { if (size <= 0) { SETERR(L_EINVAL); return -1; } return s_epoll_create1(0); }
static SYSV int s_epoll_ctl(int epfd, int op, int fd, LEpEv *ev) {
    if (trace_net()) printf("net: epoll_ctl(%d, op %d, fd %d, ev %x)\n", epfd, op, fd, ev ? ev->events : 0);
    Obj *o = get(epfd, K_EPOLL);
    if (!o) return -1;
    if (fd == epfd || (fd >= 0 && fd < MAXFD && obj[fd] && obj[fd]->kind == K_EPOLL)) { SETERR(L_EINVAL); return -1; }  // shortcut: nested epoll
    if (_get_osfhandle(fd) == -1) { SETERR(L_EBADF); return -1; }
    EnterCriticalSection(&o->lock);
    int i = 0, r = 0;
    while (i < o->nitems && o->items[i].fd != fd) i++;
    if (op == 1) {  // EPOLL_CTL_ADD
        if (i < o->nitems) { r = L_EEXIST; goto out; }
        if (o->nitems == o->cap) { o->cap = o->cap ? o->cap * 2 : 16; o->items = realloc(o->items, o->cap * sizeof *o->items); }
        o->items[o->nitems++] = (EpItem){ fd, ev->events | P_ERR | P_HUP, ev->data };
    } else if (i == o->nitems) r = L_ENOENT;
    else if (op == 2) o->items[i] = o->items[--o->nitems];  // EPOLL_CTL_DEL
    else if (op == 3) o->items[i] = (EpItem){ fd, ev->events | P_ERR | P_HUP, ev->data };  // EPOLL_CTL_MOD
    else r = L_EINVAL;
out:
    LeaveCriticalSection(&o->lock);
    if (r) { SETERR(r); return -1; }
    return 0;
}
void net_forget_fd(int fd) {  // closing an fd removes it from every epoll set, as on Linux
    for (int e = 0; e < MAXFD; e++) {
        Obj *o = obj[e];
        if (!o || o->kind != K_EPOLL) continue;
        EnterCriticalSection(&o->lock);
        for (int i = 0; i < o->nitems; i++) if (o->items[i].fd == fd) { o->items[i] = o->items[--o->nitems]; break; }
        LeaveCriticalSection(&o->lock);
    }
}
static SYSV int s_epoll_wait(int epfd, LEpEv *out, int max, int timeout) {
    Obj *o = get(epfd, K_EPOLL);
    if (!o) return -1;
    if (max <= 0) { SETERR(L_EINVAL); return -1; }
    EnterCriticalSection(&o->lock);
    int n = o->nitems;
    EpItem *it = malloc((n ? n : 1) * sizeof *it);
    memcpy(it, o->items, n * sizeof *it);
    LeaveCriticalSection(&o->lock);
    LPoll *p = malloc((n ? n : 1) * sizeof *p);
    for (int i = 0; i < n; i++) p[i] = (LPoll){ it[i].events ? it[i].fd : -1, (short)(it[i].events & 0xffff), 0 };
    // shortcut: EPOLLET is treated as level-triggered; callers that drain to EAGAIN behave the same
    int64_t t0 = now_ns(1);
    int r = core_poll(p, n, timeout < 0 ? -1 : timeout * 1000000ll), c = 0;
    { static int seen; if (getenv("SKATESTRAP_TRACE_WAIT") && seen++ < 60) {
        printf("sepoll ep=%d t=%d r=%d %.1fms tid=%lu:", epfd, timeout, r, (now_ns(1) - t0) / 1e6, GetCurrentThreadId());
        for (int i = 0; i < n && i < 8; i++) { Obj *q = p[i].fd >= 0 && p[i].fd < MAXFD ? obj[p[i].fd] : 0; printf(" %d(k%d):%x>%x", p[i].fd, q ? (int)q->kind : -1, p[i].events, p[i].revents); }
        printf("\n"); } }
    wait_trace("epoll_wait", timeout, t0, __builtin_frame_address(0));
    for (int i = 0; i < n && r > 0 && c < max; i++) {
        if (!p[i].revents) continue;
        out[c].events = (uint16_t)p[i].revents; out[c].data = it[i].data; c++;
        if (it[i].events & (1u << 30)) {  // EPOLLONESHOT: disarm until the next MOD
            EnterCriticalSection(&o->lock);
            for (int j = 0; j < o->nitems; j++) if (o->items[j].fd == it[i].fd) o->items[j].events = 0;
            LeaveCriticalSection(&o->lock);
        }
    }
    free(it); free(p);
    return r < 0 ? r : c;
}

// --- names ---------------------------------------------------------------------------------------------------
// bionic's addrinfo: flags/family/socktype/protocol, socklen_t addrlen, canonname, addr, next (BSD order).
// AI_* and NI_* values are the BSD ones, same as Windows. EAI_* are bionic's positive numbers.
typedef struct LAi { int flags, family, socktype, protocol; uint32_t addrlen; char *canonname; void *addr; struct LAi *next; } LAi;
static int eai(int w) {
    switch (w) {
    case 0: return 0; case WSATRY_AGAIN: return 2; case WSAEINVAL: return 3; case WSANO_RECOVERY: return 4;
    case WSAEAFNOSUPPORT: return 5; case WSA_NOT_ENOUGH_MEMORY: return 6; case WSANO_DATA: return 7;
    case WSAHOST_NOT_FOUND: return 8; case WSATYPE_NOT_FOUND: return 9; case WSAESOCKTNOSUPPORT: return 10;
    }
    return 4;  // EAI_FAIL
}
static SYSV void s_freeaddrinfo(LAi *a) { while (a) { LAi *n = a->next; free(a); a = n; } }
static SYSV int s_getaddrinfo(const char *node, const char *service, const LAi *hints, LAi **res) {
    wsa();
    ADDRINFOA h = { 0 }, *w = 0;
    if (hints) { h.ai_flags = hints->flags; h.ai_family = fam_in(hints->family); h.ai_socktype = hints->socktype; h.ai_protocol = hints->protocol; }
    int e = getaddrinfo(node, service, hints ? &h : 0, &w);
    if (trace_net()) printf("net: getaddrinfo(%s, %s) -> %d\n", node ? node : "-", service ? service : "-", e);
    if (e) return eai(e);
    LAi *head = 0, **tail = &head;
    for (ADDRINFOA *a = w; a; a = a->ai_next) {
        size_t cn = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
        LAi *l = calloc(1, sizeof *l + sizeof(SOCKADDR_STORAGE) + cn);
        if (!l) { s_freeaddrinfo(head); freeaddrinfo(w); return 6; }
        l->flags = a->ai_flags; l->family = fam_out(a->ai_family); l->socktype = a->ai_socktype; l->protocol = a->ai_protocol;
        l->addrlen = (uint32_t)a->ai_addrlen; l->addr = l + 1;
        memcpy(l->addr, a->ai_addr, a->ai_addrlen);
        ((uint16_t *)l->addr)[0] = (uint16_t)fam_out(a->ai_addr->sa_family);
        if (cn) { l->canonname = (char *)l->addr + sizeof(SOCKADDR_STORAGE); memcpy(l->canonname, a->ai_canonname, cn); }
        *tail = l; tail = &l->next;
    }
    freeaddrinfo(w);
    *res = head;
    return 0;
}
static SYSV const char *s_gai_strerror(int e) {
    static const char *m[] = { "Success", "Address family for hostname not supported", "Temporary failure in name resolution",
        "Invalid value for ai_flags", "Non-recoverable failure in name resolution", "ai_family not supported",
        "Memory allocation failure", "No address associated with hostname", "hostname nor servname provided, or not known",
        "servname not supported for ai_socktype", "ai_socktype not supported", "System error returned in errno" };
    return e >= 0 && e < (int)(sizeof m / sizeof *m) ? m[e] : "Unknown error";
}
static SYSV int s_getnameinfo(const void *sa, uint32_t salen, char *host, uint32_t hostlen, char *serv, uint32_t servlen, int flags) {
    wsa();
    SOCKADDR_STORAGE w; int n = addr_in(sa, salen, &w);
    if (n < 0) return 5;
    return eai(getnameinfo((SOCKADDR *)&w, n, host, hostlen, serv, servlen, flags));
}
typedef struct { char *name; char **aliases; int addrtype, length; char **addr_list; } LHostent;
static SYSV LHostent *s_gethostbyname(const char *name) {
    wsa();
    static __declspec(thread) LHostent l;
    struct hostent *h = gethostbyname(name);  // its storage is per-thread in Winsock too
    if (!h) return 0;
    l = (LHostent){ h->h_name, h->h_aliases, fam_out(h->h_addrtype), h->h_length, h->h_addr_list };
    return &l;
}
static SYSV const char *s_inet_ntop(int af, const void *src, char *dst, uint32_t size) {
    const char *r = inet_ntop(fam_in(af), src, dst, size);
    if (!r) wfail();
    return r;
}
static SYSV int s_inet_pton(int af, const char *src, void *dst) {
    int r = inet_pton(fam_in(af), src, dst);
    if (r < 0) wfail();
    return r;
}
static SYSV unsigned s_if_nametoindex(const char *name) { return if_nametoindex(name); }
static SYSV char *s_if_indextoname(unsigned i, char *name) { return if_indextoname(i, name); }

// --- fd calls from fs.c ------------------------------------------------------------------------------------
int64_t net_read(int fd, void *buf, uint64_t n) {
    Obj *o = obj[fd];
    if (o->kind == K_EVENT) return event_read(o, buf, n);
    if (o->kind == K_TIMER) return timer_read(o, buf, n);
    if (o->kind == K_EPOLL) { SETERR(L_EINVAL); return -1; }
    return s_recvfrom(fd, buf, n, 0, 0, 0);
}
int64_t net_write(int fd, const void *buf, uint64_t n) {
    Obj *o = obj[fd];
    if (o->kind == K_EVENT) return event_write(o, buf, n);
    if (o->kind != K_SOCK) { SETERR(L_EINVAL); return -1; }
    return s_sendto(fd, buf, n, 0, 0, 0);
}
void net_close(int fd) {
    Obj *o = obj[fd];
    obj[fd] = 0;
    if (o->kind == K_SOCK && trace_net()) printf("net: [%lu] close(%d)\n", GetTickCount() % 100000, fd);
    net_forget_fd(fd);
    if (o->kind == K_SOCK || o->kind == K_EVENT) closesocket(o->s);
    if (o->ev) CloseHandle(o->ev);
    DeleteCriticalSection(&o->lock);
    free(o->items); free(o);
}
int net_fcntl(int fd, int cmd, uint64_t arg) {
    Obj *o = obj[fd];
    if (cmd == 3) return 2 | (o->nonblock ? 0x800 : 0);  // F_GETFL
    if (o->kind == K_SOCK) return set_nonblock(o, !!(arg & 0x800)) ? wfail() : 0;  // F_SETFL
    o->nonblock = !!(arg & 0x800);
    return 0;
}
int net_ioctl(int fd, uint64_t req, void *arg) {
    Obj *o = obj[fd];
    if (req == 0x5421) { if (o->kind == K_SOCK) return set_nonblock(o, !!*(int *)arg) ? wfail() : 0; o->nonblock = !!*(int *)arg; return 0; }  // FIONBIO
    if (req == 0x541b && o->kind == K_SOCK) { u_long v; if (ioctlsocket(o->s, FIONREAD, &v)) return wfail(); *(int *)arg = (int)v; return 0; }  // FIONREAD
    SETERR(L_ENOTTY); return -1;
}
unsigned net_mode(int fd) { return obj[fd]->kind == K_SOCK ? 0140000 | 0777 : 0600; }

static const struct { const char *name; void *at; } table[] = {
    { "socket", s_socket }, { "connect", s_connect }, { "bind", s_bind }, { "listen", s_listen }, { "accept", s_accept },
    { "accept4", s_accept4 }, { "getsockname", s_getsockname }, { "getpeername", s_getpeername }, { "shutdown", s_shutdown },
    { "gethostname", s_gethostname }, { "sendto", s_sendto }, { "recvfrom", s_recvfrom }, { "sendmsg", s_sendmsg }, { "recvmsg", s_recvmsg },
    { "sendmmsg", s_sendmmsg }, { "recvmmsg", s_recvmmsg }, { "setsockopt", s_setsockopt }, { "getsockopt", s_getsockopt },
    { "socketpair", s_socketpair }, { "eventfd", s_eventfd }, { "timerfd_create", s_timerfd_create },
    { "timerfd_settime", s_timerfd_settime }, { "poll", s_poll }, { "select", s_select }, { "epoll_create", s_epoll_create },
    { "__FD_SET_chk", s_fd_set_chk }, { "__FD_CLR_chk", s_fd_clr_chk }, { "__FD_ISSET_chk", s_fd_isset_chk },
    { "epoll_create1", s_epoll_create1 }, { "epoll_ctl", s_epoll_ctl }, { "epoll_wait", s_epoll_wait },
    { "getaddrinfo", s_getaddrinfo }, { "freeaddrinfo", s_freeaddrinfo }, { "gai_strerror", s_gai_strerror },
    { "getnameinfo", s_getnameinfo }, { "gethostbyname", s_gethostbyname }, { "inet_ntop", s_inet_ntop },
    { "inet_pton", s_inet_pton }, { "if_nametoindex", s_if_nametoindex }, { "if_indextoname", s_if_indextoname },
};
void *shim_lookup_net(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].at;
    return 0;
}

// loader.exe --selftest: eventfd, timerfd, epoll and a loopback socket pair, no outside network
static volatile int64_t st_wrote; static DWORD st_ms = 2;
static DWORD WINAPI st_writer(void *fd) { uint64_t one = 1; Sleep(st_ms); st_wrote = now_ns(1); net_write((int)(intptr_t)fd, &one, 8); return 0; }
int shim_net_selftest(void) {
    int bad = 0; uint64_t v = 3, r = 0;
#define NCHECK(c) if (!(c)) { bad++; printf("net selftest fail line %d: %s\n", __LINE__, #c); }
    int e = s_eventfd(0, 0x800);
    NCHECK(net_read(e, &r, 8) == -1 && *shim_errno() == L_EAGAIN)
    net_write(e, &v, 8); net_write(e, &v, 8);
    int ep = s_epoll_create1(0);
    LEpEv ev = { P_IN, 42 }, got[4];
    NCHECK(s_epoll_ctl(ep, 1, e, &ev) == 0)
    NCHECK(s_epoll_wait(ep, got, 4, 0) == 1 && got[0].data == 42)
    NCHECK(net_read(e, &r, 8) == 8 && r == 6)
    int sem = s_eventfd(2, 1);
    NCHECK(net_read(sem, &r, 8) == 8 && r == 1 && net_read(sem, &r, 8) == 8 && r == 1)
    net_close(sem); _close(sem);
    NCHECK(s_epoll_wait(ep, got, 4, 0) == 0)
    int64_t t0 = now_ns(1);
    NCHECK(s_epoll_wait(ep, got, 4, 30) == 0 && now_ns(1) - t0 >= 25000000)
    int tf = s_timerfd_create(1, 0x800);
    ITs it = { { 0, 0 }, { 0, 20000000 } };
    s_timerfd_settime(tf, 0, &it, 0);
    LEpEv tev = { P_IN, 7 };
    s_epoll_ctl(ep, 1, tf, &tev);
    t0 = now_ns(1);
    NCHECK(s_epoll_wait(ep, got, 4, 1000) == 1 && got[0].data == 7 && now_ns(1) - t0 < 500000000)
    NCHECK(net_read(tf, &r, 8) == 8 && r == 1)
    int sv[2];
    NCHECK(s_socketpair(1, 1, 0, sv) == 0)
    LEpEv sev = { P_IN, 9 };
    s_epoll_ctl(ep, 1, sv[1], &sev);
    NCHECK(net_write(sv[0], "hi", 2) == 2)
    NCHECK(s_epoll_wait(ep, got, 4, 1000) == 1 && got[0].data == 9)
    char b[4] = { 0 };
    NCHECK(net_read(sv[1], b, 4) == 2 && !memcmp(b, "hi", 2))
    // precise waits: a 500 us timerfd and a 5 ms poll timeout, each beside an idle eventfd (the WSAPoll path)
    timeBeginPeriod(1);  // the game runs with a 1 ms tick (loader.c); --selftest exits before that
    int tf2 = s_timerfd_create(1, 0x800);
    LPoll pt[2] = { { tf2, P_IN }, { e, P_IN } };
    ITs it2 = { { 0, 0 }, { 0, 500000 } };
    t0 = now_ns(1); s_timerfd_settime(tf2, 0, &it2, 0);
    int pr = s_poll(pt, 2, -1); int64_t late = now_ns(1) - t0 - 500000;
    printf("net selftest: timerfd 500 us via poll: %lld us late\n", (long long)(late / 1000));
    NCHECK(pr == 1 && pt[0].revents == P_IN && (old_waits() || late < 1500000))
    NCHECK(net_read(tf2, &r, 8) == 8 && r == 1)
    t0 = now_ns(1); s_timerfd_settime(tf2, 0, &it2, 0);
    pr = s_poll(pt, 1, -1); late = now_ns(1) - t0 - 500000;  // no sockets: no WSAPoll, only sleeps
    printf("net selftest: timerfd 500 us alone via poll: %lld us late\n", (long long)(late / 1000));
    NCHECK(pr == 1 && (old_waits() || late < 1500000))
    t0 = now_ns(1); pr = s_poll(pt + 1, 1, 5); late = now_ns(1) - t0 - 5000000;
    printf("net selftest: poll 5 ms timeout: %lld us late\n", (long long)(late / 1000));
    NCHECK(pr == 0 && late >= 0 && (old_waits() || late < 1500000))
    // an eventfd written from another thread wakes a poll(-1) waiter
    st_wrote = 0;
    HANDLE th = CreateThread(0, 0, st_writer, (void *)(intptr_t)e, 0, 0);
    pr = s_poll(pt + 1, 1, -1); late = now_ns(1) - st_wrote;
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    printf("net selftest: eventfd write wakes poll: %lld us after the write\n", (long long)(late / 1000));
    NCHECK(pr == 1 && st_wrote && late < 1000000 && net_read(e, &r, 8) == 8 && r == 1)
    st_wrote = 0; st_ms = 1;  // same inside a 3 ms timeout, waited wholly in high-res slices
    th = CreateThread(0, 0, st_writer, (void *)(intptr_t)e, 0, 0);
    pr = s_poll(pt + 1, 1, 3); late = now_ns(1) - st_wrote;
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    printf("net selftest: eventfd write wakes poll(3 ms): %lld us after the write\n", (long long)(late / 1000));
    NCHECK(pr == 1 && st_wrote && late < 1000000 && net_read(e, &r, 8) == 8 && r == 1)
    net_close(tf2); _close(tf2); timeEndPeriod(1);
    for (int fd, i = 0; i < 5; i++) { fd = (int[]){ e, ep, tf, sv[0], sv[1] }[i]; net_close(fd); _close(fd); }
    LAi hints = { 4, 2, 1 }, *res = 0;  // AI_NUMERICHOST, AF_INET, SOCK_STREAM: no DNS traffic
    NCHECK(s_getaddrinfo("127.0.0.1", "80", &hints, &res) == 0 && res && res->family == 2 && res->addrlen == 16)
    s_freeaddrinfo(res);
    return bad;
}
