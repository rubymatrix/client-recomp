/* Winsock for 64-bit hosts (R3.1): WS2_32 on the host's BSD sockets (Winsock itself on Windows).
 *
 * FFXiMain talks to the lobby over TCP and to the zone servers over UDP, through the Berkeley calls
 * plus WSAEventSelect/WSAWaitForMultipleEvents. Guest SOCKETs index our own table; the host socket
 * behind each is a real one. Everything crossing the boundary is converted by field - sockaddr_in
 * (macOS has sin_len), fd_set (a count and an array in Winsock, a bitmap on BSD), socket options
 * (SOL_SOCKET and option numbers differ), errors (Winsock's WSAE* from errno).
 *
 * WSAEventSelect is a watcher thread polling every socket that has an event: it records the
 * network events Winsock would (FD_READ, FD_WRITE, FD_ACCEPT, FD_CONNECT, FD_CLOSE), with
 * Winsock's re-enabling rules, and sets the event. Blocking calls release the guest lock. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET host_sock;
#define HOST_INVALID INVALID_SOCKET
#define host_close closesocket
static int host_errno(void) { return WSAGetLastError(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int host_sock;
#define HOST_INVALID (-1)
#define host_close close
static int host_errno(void) { return errno; }
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"
#include "ws2.h"

#define WSAEINTR 10004u
#define WSAEBADF 10009u
#define WSAEACCES 10013u
#define WSAEFAULT 10014u
#define WSAEINVAL 10022u
#define WSAEMFILE 10024u
#define WSAEWOULDBLOCK 10035u
#define WSAEINPROGRESS 10036u
#define WSAEALREADY 10037u
#define WSAENOTSOCK 10038u
#define WSAEMSGSIZE 10040u
#define WSAENOPROTOOPT 10042u
#define WSAEAFNOSUPPORT 10047u
#define WSAEADDRINUSE 10048u
#define WSAEADDRNOTAVAIL 10049u
#define WSAENETDOWN 10050u
#define WSAENETUNREACH 10051u
#define WSAECONNABORTED 10053u
#define WSAECONNRESET 10054u
#define WSAENOBUFS 10055u
#define WSAEISCONN 10056u
#define WSAENOTCONN 10057u
#define WSAESHUTDOWN 10058u
#define WSAETIMEDOUT 10060u
#define WSAECONNREFUSED 10061u
#define WSAEHOSTUNREACH 10065u
#define WSAHOST_NOT_FOUND 11001u

#define GUEST_INVALID_SOCKET 0xFFFFFFFFu
#define GUEST_SOCKET_ERROR 0xFFFFFFFFu

#define FD_READ 0x01u
#define FD_WRITE 0x02u
#define FD_OOB 0x04u
#define FD_ACCEPT 0x08u
#define FD_CONNECT 0x10u
#define FD_CLOSE 0x20u

#define MAX_SOCKS 64
#define SOCK_BASE 0x2000u /* guest SOCKET values: SOCK_BASE + 4 * index */

typedef struct Sock
{
    int used;
    host_sock h;
    int type, nonblocking, listening, connecting;
    uint32_t event, mask; /* WSAEventSelect */
    uint32_t pending;     /* network events recorded, not yet enumerated */
    uint32_t enabled;     /* which events may be recorded again */
    int errors[10];       /* iErrorCode by FD_*_BIT */
} Sock;

static Sock g_socks[MAX_SOCKS];
static volatile uint32_t g_lock_word;
static int g_watcher;

static void lock(void) { while (plat_atomic_cas32(&g_lock_word, 0, 1) != 0) plat_yield(); }
static void unlock(void) { plat_atomic_cas32(&g_lock_word, 1, 0); }

static uint32_t wsa_error(int e)
{
#if defined(_WIN32)
    return (uint32_t)e; /* Winsock's own codes */
#else
    switch (e)
    {
    case EINTR: return WSAEINTR;
    case EBADF: return WSAEBADF;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK; /* Winsock reports a non-blocking connect this way */
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EMSGSIZE: return WSAEMSGSIZE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case EPIPE: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return WSAEINVAL;
    }
#endif
}

static void fail(void) { gt_set_error(wsa_error(host_errno())); }

static Sock* sock(uint32_t s)
{
    uint32_t i = (s - SOCK_BASE) / 4;
    if (s < SOCK_BASE || (s - SOCK_BASE) % 4 || i >= MAX_SOCKS || !g_socks[i].used)
    {
        gt_set_error(WSAENOTSOCK);
        return NULL;
    }
    return &g_socks[i];
}

static uint32_t sock_id(const Sock* s) { return SOCK_BASE + 4u * (uint32_t)(s - g_socks); }

static int set_nonblocking(host_sock h, int on)
{
#if defined(_WIN32)
    u_long v = (u_long)on;
    return ioctlsocket(h, FIONBIO, &v) == 0;
#else
    int f = fcntl(h, F_GETFL, 0);
    return fcntl(h, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK) == 0;
#endif
}

/* sockaddr_in: guest {u16 family, u16 port (BE), u32 addr, 8 zero} */
static int from_guest_addr(uint32_t p, uint32_t len, struct sockaddr_in* a)
{
    if (!p || len < 16 || rd16(p) != 2)
        return 0;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    memcpy(&a->sin_port, GUEST_PTR(p + 2), 2);
    memcpy(&a->sin_addr, GUEST_PTR(p + 4), 4);
    return 1;
}

static void to_guest_addr(const struct sockaddr_in* a, uint32_t p, uint32_t plen)
{
    if (!p)
        return;
    if (plen)
    {
        uint32_t n = rd32(plen);
        wr32(plen, 16);
        if (n < 16)
            return;
    }
    memset(GUEST_PTR(p), 0, 16);
    wr16(p, 2);
    memcpy(GUEST_PTR(p + 2), &a->sin_port, 2);
    memcpy(GUEST_PTR(p + 4), &a->sin_addr, 4);
}

/* --- byte order ---------------------------------------------------------------------------------- */
static uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static void sh_htonl(Guest* g) { RET(bswap32(ARG(0)), 1); }
static void sh_htons(Guest* g) { RET(((ARG(0) & 0xFF) << 8) | ((ARG(0) >> 8) & 0xFF), 1); }

/* --- startup ------------------------------------------------------------------------------------ */
/* WSAStartup(wVersionRequested, lpWSAData): WSADATA {wVersion, wHighVersion, szDescription[257], szSystemStatus[129], ...} */
static void sh_WSAStartup(Guest* g)
{
    uint32_t p = ARG(1);
    memset(GUEST_PTR(p), 0, 400);
    wr16(p, 0x0202);
    wr16(p + 2, 0x0202);
    strcpy((char*)GUEST_PTR(p + 4), "FFXI Winsock");
    strcpy((char*)GUEST_PTR(p + 261), "Running");
    wr16(p + 390, 32767); /* iMaxSockets */
    wr16(p + 392, 65467); /* iMaxUdpDg */
    RET(0, 2);
}

static void sh_WSACleanup(Guest* g) { RET(0, 0); }
static void sh_WSAGetLastError(Guest* g) { RET(gt_get_error(), 0); }

/* --- sockets -------------------------------------------------------------------------------------- */
static void sh_socket(Guest* g)
{
    int af = (int)ARG(0), type = (int)ARG(1), proto = (int)ARG(2);
    if (af != 2)
    {
        gt_set_error(WSAEAFNOSUPPORT);
        RET(GUEST_INVALID_SOCKET, 3);
    }
    host_sock h = socket(AF_INET, type, proto);
    if (h == HOST_INVALID)
    {
        fail();
        RET(GUEST_INVALID_SOCKET, 3);
    }
#if defined(__APPLE__)
    int one = 1;
    setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* a dead peer is an error, not a signal */
#endif
    lock();
    for (int i = 0; i < MAX_SOCKS; ++i)
        if (!g_socks[i].used)
        {
            Sock* s = &g_socks[i];
            memset(s, 0, sizeof *s);
            s->used = 1, s->h = h, s->type = type;
            s->enabled = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;
            unlock();
            RET(sock_id(s), 3);
        }
    unlock();
    host_close(h);
    gt_set_error(WSAEMFILE);
    RET(GUEST_INVALID_SOCKET, 3);
}

static void sh_closesocket(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 1);
    lock();
    host_sock h = s->h;
    s->used = 0;
    unlock();
    host_close(h);
    RET(0, 1);
}

static void sh_bind(Guest* g)
{
    Sock* s = sock(ARG(0));
    struct sockaddr_in a;
    if (!s)
        RET(GUEST_SOCKET_ERROR, 3);
    if (!from_guest_addr(ARG(1), ARG(2), &a))
    {
        gt_set_error(WSAEFAULT);
        RET(GUEST_SOCKET_ERROR, 3);
    }
    if (bind(s->h, (struct sockaddr*)&a, sizeof a) != 0)
    {
        fail();
        RET(GUEST_SOCKET_ERROR, 3);
    }
    RET(0, 3);
}

static void sh_listen(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 2);
    if (listen(s->h, (int)ARG(1)) != 0)
    {
        fail();
        RET(GUEST_SOCKET_ERROR, 2);
    }
    s->listening = 1;
    RET(0, 2);
}

static void sh_connect(Guest* g)
{
    Sock* s = sock(ARG(0));
    struct sockaddr_in a;
    if (!s)
        RET(GUEST_SOCKET_ERROR, 3);
    if (!from_guest_addr(ARG(1), ARG(2), &a))
    {
        gt_set_error(WSAEFAULT);
        RET(GUEST_SOCKET_ERROR, 3);
    }
    int nb = s->nonblocking;
    if (!nb)
        gt_unlock();
    int r = connect(s->h, (struct sockaddr*)&a, sizeof a);
    int e = host_errno();
    if (!nb)
        gt_lock();
    if (r != 0)
    {
        gt_set_error(wsa_error(e));
        if (gt_get_error() == WSAEWOULDBLOCK)
        {
            lock();
            s->connecting = 1;
            unlock();
        }
        RET(GUEST_SOCKET_ERROR, 3);
    }
    RET(0, 3);
}

static void sh_accept(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_INVALID_SOCKET, 3);
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int nb = s->nonblocking;
    if (!nb)
        gt_unlock();
    host_sock h = accept(s->h, (struct sockaddr*)&a, &len);
    int e = host_errno();
    if (!nb)
        gt_lock();
    lock();
    s->enabled |= FD_ACCEPT;
    unlock();
    if (h == HOST_INVALID)
    {
        gt_set_error(wsa_error(e));
        RET(GUEST_INVALID_SOCKET, 3);
    }
    to_guest_addr(&a, ARG(1), ARG(2));
    lock();
    for (int i = 0; i < MAX_SOCKS; ++i)
        if (!g_socks[i].used)
        {
            Sock* n = &g_socks[i];
            memset(n, 0, sizeof *n);
            n->used = 1, n->h = h, n->type = s->type;
            n->enabled = FD_READ | FD_WRITE | FD_OOB | FD_CLOSE;
            if (s->event) /* an accepted socket inherits WSAEventSelect */
                n->event = s->event, n->mask = s->mask, n->nonblocking = 1, set_nonblocking(h, 1);
            unlock();
            RET(sock_id(n), 3);
        }
    unlock();
    host_close(h);
    gt_set_error(WSAEMFILE);
    RET(GUEST_INVALID_SOCKET, 3);
}

/* send/recv(s, buf, len, flags); sendto/recvfrom add the address */
static void io_done(Sock* s, uint32_t bit, int would_block)
{
    lock();
    if (bit == FD_READ || !would_block)
        s->enabled |= bit == FD_READ ? FD_READ | FD_OOB : 0;
    if (bit == FD_WRITE && would_block)
        s->enabled |= FD_WRITE;
    unlock();
}

static int host_flags(uint32_t f) { return (f & 1 ? MSG_OOB : 0) | (f & 2 ? MSG_PEEK : 0); }

static uint8_t g_lobby_hash[16];
static uint16_t g_lobby_ports[2]; /* data, view; 0: no LSB sign-in */

void ws2_set_lobby_session(const uint8_t hash[16], uint16_t data_port, uint16_t view_port)
{
    memcpy(g_lobby_hash, hash, 16);
    g_lobby_ports[0] = data_port, g_lobby_ports[1] = view_port;
}

/* A lobby command: to the login server's data or view port, "IXFF" at +4, and one of
 * the commands the lobby authenticates */
static void lobby_session(Sock* s, uint8_t* buf, uint32_t len)
{
    if (!g_lobby_ports[0] || len < 28 || memcmp(buf + 4, "IXFF", 4))
        return;
    switch (buf[8])
    {
    case 0x07: case 0x14: case 0x1F: case 0x21: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2B: break;
    default: return;
    }
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    if (getpeername(s->h, (struct sockaddr*)&peer, &pl) != 0)
        return;
    uint16_t port = ntohs(peer.sin_port);
    if (port == g_lobby_ports[0] || port == g_lobby_ports[1])
        memcpy(buf + 12, g_lobby_hash, 16);
}

static void sh_send(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 4);
    lobby_session(s, (uint8_t*)ARGP(1), ARG(2));
    int nb = s->nonblocking;
    if (!nb)
        gt_unlock();
    int n = (int)send(s->h, (const char*)ARGP(1), (int)ARG(2), host_flags(ARG(3)));
    int e = host_errno();
    if (!nb)
        gt_lock();
    if (n < 0)
    {
        gt_set_error(wsa_error(e));
        io_done(s, FD_WRITE, gt_get_error() == WSAEWOULDBLOCK);
        RET(GUEST_SOCKET_ERROR, 4);
    }
    RET((uint32_t)n, 4);
}

static void sh_sendto(Guest* g)
{
    Sock* s = sock(ARG(0));
    struct sockaddr_in a;
    if (!s)
        RET(GUEST_SOCKET_ERROR, 6);
    int has = ARG(4) != 0;
    if (has && !from_guest_addr(ARG(4), ARG(5), &a))
    {
        gt_set_error(WSAEFAULT);
        RET(GUEST_SOCKET_ERROR, 6);
    }
    int n = (int)sendto(s->h, (const char*)ARGP(1), (int)ARG(2), host_flags(ARG(3)), has ? (struct sockaddr*)&a : NULL, has ? sizeof a : 0);
    if (n < 0)
    {
        fail();
        io_done(s, FD_WRITE, gt_get_error() == WSAEWOULDBLOCK);
        RET(GUEST_SOCKET_ERROR, 6);
    }
    RET((uint32_t)n, 6);
}

static void sh_recv(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 4);
    int nb = s->nonblocking;
    if (!nb)
        gt_unlock();
    int n = (int)recv(s->h, (char*)ARGP(1), (int)ARG(2), host_flags(ARG(3)));
    int e = host_errno();
    if (!nb)
        gt_lock();
    io_done(s, FD_READ, 0);
    if (n < 0)
    {
        gt_set_error(wsa_error(e));
        RET(GUEST_SOCKET_ERROR, 4);
    }
    RET((uint32_t)n, 4);
}

static void sh_recvfrom(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 6);
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int nb = s->nonblocking;
    if (!nb)
        gt_unlock();
    int n = (int)recvfrom(s->h, (char*)ARGP(1), (int)ARG(2), host_flags(ARG(3)), (struct sockaddr*)&a, &len);
    int e = host_errno();
    if (!nb)
        gt_lock();
    io_done(s, FD_READ, 0);
    if (n < 0)
    {
        gt_set_error(wsa_error(e));
        RET(GUEST_SOCKET_ERROR, 6);
    }
    to_guest_addr(&a, ARG(4), ARG(5));
    RET((uint32_t)n, 6);
}

static void sh_shutdown(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 2);
    if (shutdown(s->h, (int)ARG(1)) != 0)
    {
        fail();
        RET(GUEST_SOCKET_ERROR, 2);
    }
    RET(0, 2);
}

/* ioctlsocket(s, cmd, argp): FIONBIO, FIONREAD */
static void sh_ioctlsocket(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 3);
    if (ARG(1) == 0x8004667Eu) /* FIONBIO */
    {
        int on = rd32(ARG(2)) != 0;
        if (!on && s->event)
        {
            gt_set_error(WSAEINVAL); /* WSAEventSelect keeps a socket non-blocking */
            RET(GUEST_SOCKET_ERROR, 3);
        }
        set_nonblocking(s->h, on);
        s->nonblocking = on;
        RET(0, 3);
    }
    if (ARG(1) == 0x4004667Fu) /* FIONREAD */
    {
#if defined(_WIN32)
        u_long n = 0;
        ioctlsocket(s->h, FIONREAD, &n);
#else
        int n = 0;
        ioctl(s->h, FIONREAD, &n);
#endif
        wr32(ARG(2), (uint32_t)n);
        RET(0, 3);
    }
    gt_set_error(WSAEINVAL);
    RET(GUEST_SOCKET_ERROR, 3);
}

/* socket options: Winsock's (level, name) to the host's */
static int host_option(uint32_t level, uint32_t name, int* hl, int* hn)
{
    if (level == 0xFFFFu) /* SOL_SOCKET */
    {
        *hl = SOL_SOCKET;
        switch (name)
        {
        case 0x0004: *hn = SO_REUSEADDR; return 1;
        case 0x0008: *hn = SO_KEEPALIVE; return 1;
        case 0x0020: *hn = SO_BROADCAST; return 1;
        case 0x0080: *hn = SO_LINGER; return 1;
        case 0x1001: *hn = SO_SNDBUF; return 1;
        case 0x1002: *hn = SO_RCVBUF; return 1;
        case 0x1005: *hn = SO_SNDTIMEO; return 1;
        case 0x1006: *hn = SO_RCVTIMEO; return 1;
        case 0x1007: *hn = SO_ERROR; return 1;
        case 0x1008: *hn = SO_TYPE; return 1;
        default: return 0;
        }
    }
    if (level == 6 && name == 1) /* IPPROTO_TCP, TCP_NODELAY */
    {
        *hl = IPPROTO_TCP, *hn = TCP_NODELAY;
        return 1;
    }
    return 0;
}

static void sh_setsockopt(Guest* g)
{
    Sock* s = sock(ARG(0));
    int hl, hn;
    if (!s)
        RET(GUEST_SOCKET_ERROR, 5);
    if (!host_option(ARG(1), ARG(2), &hl, &hn))
        RET(0, 5); /* an option this host does not have: accepted, as harmless */
    int r;
    if (hn == SO_LINGER)
    {
        struct linger l = { (int)rd16(ARG(3)), (int)rd16(ARG(3) + 2) };
        r = setsockopt(s->h, hl, hn, (const char*)&l, sizeof l);
    }
#if !defined(_WIN32)
    else if (hn == SO_SNDTIMEO || hn == SO_RCVTIMEO) /* milliseconds in Winsock, a timeval here */
    {
        uint32_t ms = rd32(ARG(3));
        struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };
        r = setsockopt(s->h, hl, hn, &tv, sizeof tv);
    }
#endif
    else
    {
        int v = (int)(ARG(4) >= 4 ? rd32(ARG(3)) : rd8(ARG(3)));
        r = setsockopt(s->h, hl, hn, (const char*)&v, sizeof v);
    }
    if (r != 0)
    {
        fail();
        RET(GUEST_SOCKET_ERROR, 5);
    }
    RET(0, 5);
}

static void sh_getsockopt(Guest* g)
{
    Sock* s = sock(ARG(0));
    int hl, hn;
    if (!s)
        RET(GUEST_SOCKET_ERROR, 5);
    if (!host_option(ARG(1), ARG(2), &hl, &hn) || hn == SO_LINGER || hn == SO_SNDTIMEO || hn == SO_RCVTIMEO)
    {
        gt_set_error(WSAENOPROTOOPT);
        RET(GUEST_SOCKET_ERROR, 5);
    }
    int v = 0;
    socklen_t len = sizeof v;
    if (getsockopt(s->h, hl, hn, (char*)&v, &len) != 0)
    {
        fail();
        RET(GUEST_SOCKET_ERROR, 5);
    }
    if (hn == SO_ERROR)
        v = v ? (int)wsa_error(v) : 0;
    wr32(ARG(3), (uint32_t)v);
    wr32(ARG(4), 4);
    RET(0, 5);
}

/* select(nfds, readfds, writefds, exceptfds, timeout): Winsock's fd_set is {u_int count; SOCKET array[64]} */
static void sh_select(Guest* g)
{
    uint32_t sets[3] = { ARG(1), ARG(2), ARG(3) }, tv = ARG(4);
    fd_set hs[3];
    host_sock maxfd = 0;
    int total = 0;
    for (int k = 0; k < 3; ++k)
    {
        FD_ZERO(&hs[k]);
        if (!sets[k])
            continue;
        uint32_t n = rd32(sets[k]);
        for (uint32_t i = 0; i < n && i < 64; ++i)
        {
            Sock* s = sock(rd32(sets[k] + 4 + 4 * i));
            if (!s)
                RET(GUEST_SOCKET_ERROR, 5);
            FD_SET(s->h, &hs[k]);
            if (s->h > maxfd)
                maxfd = s->h;
            total++;
        }
    }
    struct timeval t = { 0, 0 };
    if (tv)
        t.tv_sec = (long)rd32(tv), t.tv_usec = (long)rd32(tv + 4);
    int r;
    gt_unlock();
    if (!total)
    {
        plat_sleep_ms(tv ? (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000) : 0xFFFFFFFFu);
        r = 0;
    }
    else
        r = select((int)maxfd + 1, &hs[0], &hs[1], &hs[2], tv ? &t : NULL);
    int e = host_errno();
    gt_lock();
    if (r < 0)
    {
        gt_set_error(wsa_error(e));
        RET(GUEST_SOCKET_ERROR, 5);
    }
    /* keep only the ready sockets in each guest set */
    for (int k = 0; k < 3; ++k)
    {
        if (!sets[k])
            continue;
        uint32_t n = rd32(sets[k]), o = 0;
        for (uint32_t i = 0; i < n && i < 64; ++i)
        {
            uint32_t gs = rd32(sets[k] + 4 + 4 * i);
            Sock* s = sock(gs);
            if (s && FD_ISSET(s->h, &hs[k]))
                wr32(sets[k] + 4 + 4 * o++, gs);
        }
        wr32(sets[k], o);
    }
    RET((uint32_t)r, 5);
}

/* __WSAFDIsSet(s, set) */
static void sh___WSAFDIsSet(Guest* g)
{
    uint32_t n = rd32(ARG(1));
    for (uint32_t i = 0; i < n && i < 64; ++i)
        if (rd32(ARG(1) + 4 + 4 * i) == ARG(0))
            RET(1, 2);
    RET(0, 2);
}

/* --- names ------------------------------------------------------------------------------------------ */
static uint32_t g_ntoa, g_hostent;

/* inet_ntoa(in_addr by value) */
static void sh_inet_ntoa(Guest* g)
{
    if (!g_ntoa)
        g_ntoa = gheap_alloc(16, 1);
    uint32_t a = ARG(0);
    snprintf((char*)GUEST_PTR(g_ntoa), 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    RET(g_ntoa, 1);
}

static uint32_t g_game_server; /* host byte order; 0: the game's names go to DNS */

void ws2_set_game_server(uint32_t ipv4_host_order)
{
    g_game_server = ipv4_host_order;
}

/* the game's domain or a name under it, any case, with or without the root's trailing dot */
static int is_game_host_name(const char* name)
{
    size_t n = strlen(name);
    if (n && name[n - 1] == '.')
        n--;
    if (n < 7)
        return 0;
    const char* tail = name + n - 7;
    for (int i = 0; i < 7; ++i)
        if ((char)tolower((unsigned char)tail[i]) != "pol.com"[i])
            return 0;
    return n == 7 || tail[-1] == '.';
}

/* --- the calls FINAL FANTASY XIV makes beyond FFXI's ------------------------------------------- */

/* inet_addr("a.b.c.d"): network byte order, INADDR_NONE when it is not one */
static void sh_inet_addr(Guest* g)
{
    struct in_addr a;
    if (!ARG(0) || inet_pton(AF_INET, ARGS(0), &a) != 1)
        RET(0xFFFFFFFFu, 1);
    uint32_t v;
    memcpy(&v, &a, 4);
    RET(v, 1);
}

/* getsockname / getpeername(s, sockaddr*, int* len) */
static void name_common(Guest* g, int peer)
{
    Sock* s = sock(ARG(0));
    if (!s)
    {
        gt_set_error(10038); /* WSAENOTSOCK */
        RET(0xFFFFFFFFu, 3);
    }
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int r = peer ? getpeername(s->h, (struct sockaddr*)&a, &len) : getsockname(s->h, (struct sockaddr*)&a, &len);
    if (r != 0)
    {
        fail();
        RET(0xFFFFFFFFu, 3);
    }
    to_guest_addr(&a, ARG(1), ARG(2));
    RET(0, 3);
}
static void sh_getsockname(Guest* g) { name_common(g, 0); }
static void sh_getpeername(Guest* g) { name_common(g, 1); }

/* getaddrinfo(node, service, hints, ADDRINFOA** result): IPv4 answers as a guest list of
 * ADDRINFOA (32 bytes: flags, family, socktype, protocol, addrlen, canonname, addr, next), each
 * with its sockaddr_in right after it. */
static void sh_getaddrinfo(Guest* g)
{
    char node[256] = "", service[32] = "";
    if (ARG(0))
        snprintf(node, sizeof node, "%s", ARGS(0));
    if (ARG(1))
        snprintf(service, sizeof service, "%s", ARGS(1));
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (ARG(2))
    {
        hints.ai_flags = (int)rd32(ARG(2)) & 0x1; /* AI_PASSIVE */
        hints.ai_socktype = (int)rd32(ARG(2) + 8);
        hints.ai_protocol = (int)rd32(ARG(2) + 12);
    }
    gt_unlock();
    int r = getaddrinfo(ARG(0) ? node : NULL, ARG(1) ? service : NULL, &hints, &res);
    gt_lock();
    if (r != 0 || !res)
    {
        wr32(ARG(3), 0);
        RET(11001, 4); /* WSAHOST_NOT_FOUND */
    }
    uint32_t head = 0, prev = 0;
    for (struct addrinfo* a = res; a; a = a->ai_next)
    {
        if (a->ai_family != AF_INET)
            continue;
        uint32_t e = gheap_alloc(32 + 16, 1);
        wr32(e, (uint32_t)a->ai_flags), wr32(e + 4, 2), wr32(e + 8, (uint32_t)a->ai_socktype);
        wr32(e + 12, (uint32_t)a->ai_protocol), wr32(e + 16, 16), wr32(e + 20, 0), wr32(e + 24, e + 32), wr32(e + 28, 0);
        to_guest_addr((const struct sockaddr_in*)a->ai_addr, e + 32, 0);
        if (prev)
            wr32(prev + 28, e);
        else
            head = e;
        prev = e;
    }
    freeaddrinfo(res);
    wr32(ARG(3), head);
    RET(head ? 0 : 11001, 4);
}
static void sh_freeaddrinfo(Guest* g)
{
    for (uint32_t e = ARG(0); e;)
    {
        uint32_t next = rd32(e + 28);
        gheap_free(e);
        e = next;
    }
    RET(0, 1);
}

/* WSAIoctl: no extension functions or keep-alive tuning on this host */
static void sh_WSAIoctl(Guest* g)
{
    gt_set_error(10045); /* WSAEOPNOTSUPP */
    RET(0xFFFFFFFFu, 9);
}

static const ShimDef WS2_XIV[] = {
    { "ws2_32.dll", "#11", sh_inet_addr },
    { "ws2_32.dll", "inet_addr", sh_inet_addr },
    { "ws2_32.dll", "#5", sh_getpeername },
    { "ws2_32.dll", "getpeername", sh_getpeername },
    { "ws2_32.dll", "#6", sh_getsockname },
    { "ws2_32.dll", "getsockname", sh_getsockname },
    { "ws2_32.dll", "getaddrinfo", sh_getaddrinfo },
    { "ws2_32.dll", "freeaddrinfo", sh_freeaddrinfo },
    { "ws2_32.dll", "WSAIoctl", sh_WSAIoctl },
    { NULL, NULL, NULL },
};

/* gethostbyname(name): one hostent the guest reads before the next call, as Winsock's per-thread one */
static void sh_gethostbyname(Guest* g)
{
    char name[256];
    snprintf(name, sizeof name, "%s", ARGS(0));
    struct addrinfo hints, *res = NULL, fixed;
    struct sockaddr_in fixed_addr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    int from_dns = 1;
    if (g_game_server && is_game_host_name(name))
    {
        /* the game's hosts are ours: one answer, no DNS */
        memset(&fixed, 0, sizeof fixed);
        memset(&fixed_addr, 0, sizeof fixed_addr);
        fixed_addr.sin_family = AF_INET;
        fixed_addr.sin_addr.s_addr = htonl(g_game_server);
        fixed.ai_family = AF_INET;
        fixed.ai_addr = (struct sockaddr*)&fixed_addr;
        res = &fixed;
        from_dns = 0;
        rt_log("[recomp] ws2: %s -> %u.%u.%u.%u\n", name, g_game_server >> 24, (g_game_server >> 16) & 255,
            (g_game_server >> 8) & 255, g_game_server & 255);
    }
    else
    {
        gt_unlock();
        int r = getaddrinfo(name, NULL, &hints, &res);
        gt_lock();
        if (r != 0 || !res)
        {
            gt_set_error(WSAHOST_NOT_FOUND);
            RET(0, 1);
        }
    }
    if (!g_hostent)
        g_hostent = gheap_alloc(512, 1);
    uint32_t h = g_hostent;
    /* hostent {h_name, h_aliases, h_addrtype, h_length, h_addr_list}; then the aliases list, the
     * address list (up to 8), the addresses and the name */
    uint32_t aliases = h + 16, list = h + 20, addrs = h + 56, str = h + 88;
    wr32(aliases, 0);
    unsigned n = 0;
    for (struct addrinfo* a = res; a && n < 8; a = a->ai_next)
        if (a->ai_family == AF_INET)
        {
            memcpy(GUEST_PTR(addrs + 4 * n), &((struct sockaddr_in*)a->ai_addr)->sin_addr, 4);
            wr32(list + 4 * n, addrs + 4 * n);
            n++;
        }
    wr32(list + 4 * n, 0);
    if (from_dns)
        freeaddrinfo(res);
    snprintf((char*)GUEST_PTR(str), 256, "%s", name);
    wr32(h, str);
    wr32(h + 4, aliases);
    wr16(h + 8, 2);
    wr16(h + 10, 4);
    wr32(h + 12, list);
    RET(h, 1);
}

static void sh_gethostname(Guest* g)
{
    char name[256];
    if (gethostname(name, sizeof name) != 0)
        strcpy(name, "ffxi");
    if (strlen(name) + 1 > ARG(1))
    {
        gt_set_error(WSAEFAULT);
        RET(GUEST_SOCKET_ERROR, 2);
    }
    strcpy((char*)ARGP(0), name);
    RET(0, 2);
}

/* --- WSAEventSelect and friends ---------------------------------------------------------------------------- */
static void sh_WSACreateEvent(Guest* g) { RET(k_event(1, 0), 0); } /* manual reset, as WSA events are */
static void sh_WSASetEvent(Guest* g) { RET(k_event_set(ARG(0), 1), 1); }
static void sh_WSAResetEvent(Guest* g) { RET(k_event_set(ARG(0), 0), 1); }
static void sh_WSACloseEvent(Guest* g) { RET(k_close(ARG(0)), 1); }

/* WSAWaitForMultipleEvents(cEvents, lphEvents, fWaitAll, dwTimeout, fAlertable): WAIT_* values match */
static void sh_WSAWaitForMultipleEvents(Guest* g)
{
    uint32_t n = ARG(0), hs[64];
    for (uint32_t i = 0; i < n && i < 64; ++i)
        hs[i] = rd32(ARG(1) + 4 * i);
    RET(k_wait(hs, n < 64 ? n : 64, ARG(2) != 0, ARG(3)), 5);
}

/* the watcher: records network events for sockets with an event, and sets it */
static void watcher(void* arg)
{
    (void)arg;
    for (;;)
    {
        fd_set rd, wr, ex;
        FD_ZERO(&rd), FD_ZERO(&wr), FD_ZERO(&ex);
        host_sock maxfd = 0;
        int any = 0;
        lock();
        for (int i = 0; i < MAX_SOCKS; ++i)
        {
            Sock* s = &g_socks[i];
            if (!s->used || !s->event)
                continue;
            uint32_t want = s->mask & s->enabled;
            if (want & (FD_READ | FD_ACCEPT | FD_CLOSE | FD_OOB))
                FD_SET(s->h, &rd);
            if ((want & FD_WRITE) || (s->connecting && (want & FD_CONNECT)))
                FD_SET(s->h, &wr);
            if (s->connecting)
                FD_SET(s->h, &ex);
            if (s->h > maxfd)
                maxfd = s->h;
            any = 1;
        }
        unlock();
        if (!any)
        {
            plat_sleep_ms(5);
            continue;
        }
        struct timeval t = { 0, 5000 };
        if (select((int)maxfd + 1, &rd, &wr, &ex, &t) <= 0)
            continue;
        lock();
        for (int i = 0; i < MAX_SOCKS; ++i)
        {
            Sock* s = &g_socks[i];
            if (!s->used || !s->event)
                continue;
            uint32_t got = 0;
            uint32_t want = s->mask & s->enabled;
            if (s->connecting && (FD_ISSET(s->h, &wr) || FD_ISSET(s->h, &ex)))
            {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s->h, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                s->connecting = 0;
                if (want & FD_CONNECT)
                {
                    got |= FD_CONNECT, s->errors[4] = err ? (int)wsa_error(err) : 0;
                    s->enabled &= ~FD_CONNECT;
                }
                if (!err && (want & FD_WRITE))
                    got |= FD_WRITE, s->enabled &= ~FD_WRITE, s->errors[1] = 0;
            }
            else if (FD_ISSET(s->h, &wr) && (want & FD_WRITE))
                got |= FD_WRITE, s->enabled &= ~FD_WRITE, s->errors[1] = 0;
            if (FD_ISSET(s->h, &rd))
            {
                if (s->listening)
                {
                    if (want & FD_ACCEPT)
                        got |= FD_ACCEPT, s->enabled &= ~FD_ACCEPT, s->errors[3] = 0;
                }
                else
                {
                    char c;
                    int n = (int)recv(s->h, &c, 1, MSG_PEEK);
                    if (s->type == 1 && n == 0) /* the peer closed (TCP) */
                    {
                        if (want & FD_CLOSE)
                            got |= FD_CLOSE, s->enabled &= ~(FD_CLOSE | FD_READ), s->errors[5] = 0;
                    }
                    else if (n < 0 && s->type == 1 && wsa_error(host_errno()) != WSAEWOULDBLOCK)
                    {
                        if (want & FD_CLOSE)
                            got |= FD_CLOSE, s->enabled &= ~(FD_CLOSE | FD_READ), s->errors[5] = (int)WSAECONNRESET;
                    }
                    else if (want & FD_READ)
                        got |= FD_READ, s->enabled &= ~FD_READ, s->errors[0] = 0;
                }
            }
            if (got)
            {
                s->pending |= got;
                k_event_set(s->event, 1);
            }
        }
        unlock();
    }
}

/* WSAEventSelect(s, hEventObject, lNetworkEvents) */
static void sh_WSAEventSelect(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 3);
    lock();
    s->event = ARG(1);
    s->mask = ARG(2);
    s->pending = 0;
    unlock();
    if (s->event)
    {
        set_nonblocking(s->h, 1);
        s->nonblocking = 1;
        if (!g_watcher)
            g_watcher = plat_thread_start(watcher, NULL);
    }
    RET(0, 3);
}

/* WSAEnumNetworkEvents(s, hEventObject, lpNetworkEvents{lNetworkEvents, iErrorCode[10]}) */
static void sh_WSAEnumNetworkEvents(Guest* g)
{
    Sock* s = sock(ARG(0));
    if (!s)
        RET(GUEST_SOCKET_ERROR, 3);
    lock();
    uint32_t ev = s->pending;
    s->pending = 0;
    uint32_t p = ARG(2);
    wr32(p, ev);
    for (int i = 0; i < 10; ++i)
        wr32(p + 4 + 4 * (uint32_t)i, (uint32_t)s->errors[i]);
    unlock();
    if (ARG(1))
        k_event_set(ARG(1), 0);
    RET(0, 3);
}

static const ShimDef WS2[] = {
    { "ws2_32.dll", "#1", sh_accept },
    { "ws2_32.dll", "#2", sh_bind },
    { "ws2_32.dll", "#3", sh_closesocket },
    { "ws2_32.dll", "#4", sh_connect },
    { "ws2_32.dll", "#7", sh_getsockopt },
    { "ws2_32.dll", "#8", sh_htonl },
    { "ws2_32.dll", "#9", sh_htons },
    { "ws2_32.dll", "#10", sh_ioctlsocket },
    { "ws2_32.dll", "#12", sh_inet_ntoa },
    { "ws2_32.dll", "#13", sh_listen },
    { "ws2_32.dll", "#14", sh_htonl }, /* ntohl */
    { "ws2_32.dll", "#15", sh_htons }, /* ntohs */
    { "ws2_32.dll", "#16", sh_recv },
    { "ws2_32.dll", "#17", sh_recvfrom },
    { "ws2_32.dll", "#18", sh_select },
    { "ws2_32.dll", "#19", sh_send },
    { "ws2_32.dll", "#20", sh_sendto },
    { "ws2_32.dll", "#21", sh_setsockopt },
    { "ws2_32.dll", "#22", sh_shutdown },
    { "ws2_32.dll", "#23", sh_socket },
    { "ws2_32.dll", "#52", sh_gethostbyname },
    { "ws2_32.dll", "#57", sh_gethostname },
    { "ws2_32.dll", "#111", sh_WSAGetLastError },
    { "ws2_32.dll", "#115", sh_WSAStartup },
    { "ws2_32.dll", "#116", sh_WSACleanup },
    { "ws2_32.dll", "#151", sh___WSAFDIsSet },
    { "ws2_32.dll", "WSACreateEvent", sh_WSACreateEvent },
    { "ws2_32.dll", "WSASetEvent", sh_WSASetEvent },
    { "ws2_32.dll", "WSAResetEvent", sh_WSAResetEvent },
    { "ws2_32.dll", "WSACloseEvent", sh_WSACloseEvent },
    { "ws2_32.dll", "WSAWaitForMultipleEvents", sh_WSAWaitForMultipleEvents },
    { "ws2_32.dll", "WSAEventSelect", sh_WSAEventSelect },
    { "ws2_32.dll", "WSAEnumNetworkEvents", sh_WSAEnumNetworkEvents },
    { NULL, NULL, NULL },
};

void ws2_init(void)
{
    thunk_register(WS2_XIV);
#if defined(_WIN32)
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
    thunk_register(WS2);
}
