/* WinHTTP for the launcher programs: ffxivboot's version check (GET
 * http://ver01.ffxiv.com:54996/patch/vercheck/...), in WinHTTP's asynchronous mode.
 *
 * Requests are plain HTTP/1.1 over host sockets, "Connection: close", one at a time. In async mode
 * (WINHTTP_FLAG_ASYNC) every call that waits on the network returns at once and its work is queued
 * for a guest thread of our own, which does the I/O without the guest lock and then calls the
 * program's status callback, as WinHTTP's worker threads do: SENDREQUEST_COMPLETE,
 * HEADERS_AVAILABLE, DATA_AVAILABLE, READ_COMPLETE, WRITE_COMPLETE, or REQUEST_ERROR.
 *
 * Square Enix's host names (*.ffxiv.com, *.square-enix.com) go to XIV_SERVER_HOST (default
 * 127.0.0.1), the server the player chose (net_host_map, ws2.c uses it too).
 */
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"

#define FLAG_ASYNC 0x10000000u
#define ST_HANDLE_CLOSING 0x00000800u
#define ST_HEADERS_AVAILABLE 0x00020000u
#define ST_DATA_AVAILABLE 0x00040000u
#define ST_READ_COMPLETE 0x00080000u
#define ST_WRITE_COMPLETE 0x00100000u
#define ST_REQUEST_ERROR 0x00200000u
#define ST_SENDREQUEST_COMPLETE 0x00400000u
#define API_RECEIVE_RESPONSE 1u
#define API_QUERY_DATA_AVAILABLE 2u
#define API_READ_DATA 3u
#define API_WRITE_DATA 4u
#define API_SEND_REQUEST 5u
#define ERR_CANNOT_CONNECT 12029u
#define ERR_CONNECTION_ERROR 12030u
#define ERR_INVALID_HANDLE 6u
#define ERR_INSUFFICIENT_BUFFER 122u
#define ERR_HEADER_NOT_FOUND 12150u

/* --- Square Enix's hosts, to the server's ------------------------------------------------------------ */
const char* net_host_map(const char* host)
{
    size_t n = strlen(host);
    static const char* const ours[] = { ".ffxiv.com", ".square-enix.com" };
    for (int i = 0; i < 2; ++i)
    {
        size_t k = strlen(ours[i]);
        if ((n > k && !strcasecmp(host + n - k, ours[i])) || !strcasecmp(host, ours[i] + 1))
            return getenv("XIV_SERVER_HOST") ? getenv("XIV_SERVER_HOST") : "127.0.0.1";
    }
    return host;
}

/* --- handles ------------------------------------------------------------------------------------------- */
typedef struct
{
    int used, type; /* 1 session, 2 connection, 3 request */
    uint32_t parent, flags, callback, notify, context;
    char agent[128], host[256], verb[16], path[2048];
    int port;
    int sock;
    char* head;        /* the response's status line and headers, through the blank line */
    int status;
    char* buf;         /* body bytes received and not yet read */
    size_t buf_len, buf_cap;
    long long remaining; /* body bytes still to come (Content-Length), -1 until the connection closes */
    int chunked, eof;
} Handle;

#define MAX_H 32
#define H_BASE 0x00C00010u
static Handle g_h[MAX_H];

static Handle* handle(uint32_t h)
{
    if (h < H_BASE || (h - H_BASE) % 16)
        return NULL;
    uint32_t i = (h - H_BASE) / 16;
    return i < MAX_H && g_h[i].used ? &g_h[i] : NULL;
}

static uint32_t new_handle(int type, uint32_t parent)
{
    for (int i = 0; i < MAX_H; ++i)
        if (!g_h[i].used)
        {
            memset(&g_h[i], 0, sizeof g_h[i]);
            g_h[i].used = 1, g_h[i].type = type, g_h[i].parent = parent, g_h[i].sock = -1, g_h[i].remaining = -1;
            Handle* p = handle(parent);
            if (p)
            {
                g_h[i].flags = p->flags, g_h[i].callback = p->callback, g_h[i].notify = p->notify;
                g_h[i].context = p->context;
                memcpy(g_h[i].agent, p->agent, sizeof g_h[i].agent);
                memcpy(g_h[i].host, p->host, sizeof g_h[i].host);
                g_h[i].port = p->port;
            }
            return H_BASE + 16u * (uint32_t)i;
        }
    return 0;
}

static void narrow_w(uint32_t w, char* out, size_t n)
{
    size_t k = 0;
    for (uint16_t c; w && (c = rd16(w + 2 * (uint32_t)k)) && k + 1 < n; ++k)
        out[k] = c > 0x7F ? '?' : (char)c;
    out[k] = 0;
}

/* --- the network (no guest lock held) --------------------------------------------------------------- */
static int net_connect(const char* host, int port)
{
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(net_host_map(host), ps, &hints, &res))
        return -1;
    int s = -1;
    for (struct addrinfo* a = res; a && s < 0; a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s >= 0 && connect(s, a->ai_addr, a->ai_addrlen))
            close(s), s = -1;
    }
    freeaddrinfo(res);
    if (s >= 0)
    {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
    return s;
}

static int send_all(int s, const void* p, size_t n)
{
    const char* c = p;
    while (n)
    {
        ssize_t k = send(s, c, n, 0);
        if (k <= 0)
            return 0;
        c += k, n -= (size_t)k;
    }
    return 1;
}

static void buf_add(Handle* r, const char* p, size_t n)
{
    if (r->buf_len + n > r->buf_cap)
    {
        r->buf_cap = (r->buf_len + n) * 2 + 4096;
        r->buf = realloc(r->buf, r->buf_cap);
    }
    memcpy(r->buf + r->buf_len, p, n);
    r->buf_len += n;
}

/* the headers, then for a chunked body the whole body (decoded); a Content-Length body streams */
static int read_response(Handle* r)
{
    char tmp[16384];
    char* all = NULL;
    size_t len = 0;
    char* end = NULL;
    while (!end)
    {
        ssize_t k = recv(r->sock, tmp, sizeof tmp, 0);
        if (k <= 0)
        {
            free(all);
            return 0;
        }
        all = realloc(all, len + (size_t)k + 1);
        memcpy(all + len, tmp, (size_t)k);
        len += (size_t)k;
        all[len] = 0;
        end = strstr(all, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - all) + 4;
    r->head = strndup(all, head_len);
    r->status = atoi(strchr(r->head, ' ') ? strchr(r->head, ' ') + 1 : "0");
    for (char* line = strstr(r->head, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n"))
    {
        if (!strncasecmp(line + 2, "Content-Length:", 15))
            r->remaining = atoll(line + 17);
        if (!strncasecmp(line + 2, "Transfer-Encoding:", 18) && strcasestr(line + 20, "chunked"))
            r->chunked = 1;
    }
    if (r->status == 204 || r->status == 304 || !strcasecmp(r->verb, "HEAD"))
        r->remaining = 0;
    buf_add(r, all + head_len, len - head_len);
    free(all);
    if (r->chunked) /* read to the end and decode */
    {
        for (ssize_t k; (k = recv(r->sock, tmp, sizeof tmp, 0)) > 0;)
            buf_add(r, tmp, (size_t)k);
        char* out = malloc(r->buf_len + 1);
        size_t o = 0, i = 0;
        while (i < r->buf_len)
        {
            size_t n = strtoul(r->buf + i, NULL, 16);
            char* nl = memchr(r->buf + i, '\n', r->buf_len - i);
            if (!nl || !n)
                break;
            i = (size_t)(nl - r->buf) + 1;
            if (i + n > r->buf_len)
                n = r->buf_len - i;
            memcpy(out + o, r->buf + i, n);
            o += n, i += n + 2;
        }
        free(r->buf);
        r->buf = out, r->buf_len = o, r->buf_cap = o + 1, r->remaining = 0, r->eof = 1;
    }
    else if (r->remaining >= 0)
        r->remaining -= (long long)r->buf_len;
    return 1;
}

/* make bytes available: what is buffered, else one recv; 0 at the body's end */
static size_t fill(Handle* r)
{
    if (r->buf_len || r->eof || r->remaining == 0)
        return r->buf_len;
    char tmp[65536];
    size_t want = r->remaining > 0 && r->remaining < (long long)sizeof tmp ? (size_t)r->remaining : sizeof tmp;
    ssize_t k = recv(r->sock, tmp, want, 0);
    if (k <= 0)
    {
        r->eof = 1;
        return 0;
    }
    buf_add(r, tmp, (size_t)k);
    if (r->remaining > 0)
        r->remaining -= k;
    return r->buf_len;
}

/* --- jobs for the worker ------------------------------------------------------------------------------- */
typedef struct
{
    int kind; /* API_* */
    uint32_t req, buf, n;
    char* send; /* the request bytes (SEND) or data (WRITE) */
    size_t send_len;
} Job;

#define MAX_J 64
static Job g_jobs[MAX_J];
static unsigned g_jhead, g_jcount;
static pthread_mutex_t g_jm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jc = PTHREAD_COND_INITIALIZER;
static int g_worker;

static void notify(Handle* r, uint32_t hreq, uint32_t status, uint32_t info, uint32_t info_len)
{
    if (!r->callback || !(r->notify & status))
        return;
    uint32_t a[5] = { hreq, r->context, status, info, info_len };
    guest_call(r->callback, 5, a);
}

static void fail(Handle* r, uint32_t hreq, uint32_t api, uint32_t err)
{
    uint32_t res = gheap_alloc(8, 1);
    wr32(res, api), wr32(res + 4, err);
    notify(r, hreq, ST_REQUEST_ERROR, res, 8);
    gheap_free(res);
}

/* one job: the I/O without the guest lock, then the callback with it */
static void run_job(Job* j)
{
    Handle* r = handle(j->req);
    if (!r)
        return;
    switch (j->kind)
    {
    case API_SEND_REQUEST:
    {
        gt_unlock();
        int s = net_connect(r->host, r->port);
        int ok = s >= 0 && send_all(s, j->send, j->send_len);
        gt_lock();
        if ((r = handle(j->req)))
        {
            r->sock = s;
            if (ok)
                notify(r, j->req, ST_SENDREQUEST_COMPLETE, 0, 0);
            else
            {
                rt_log("[winhttp] cannot reach %s:%d (%s)\n", net_host_map(r->host), r->port, r->host);
                fail(r, j->req, API_SEND_REQUEST, ERR_CANNOT_CONNECT);
            }
        }
        break;
    }
    case API_WRITE_DATA:
    {
        gt_unlock();
        int ok = r->sock >= 0 && send_all(r->sock, j->send, j->send_len);
        gt_lock();
        if ((r = handle(j->req)))
        {
            uint32_t n = gheap_alloc(4, 1);
            wr32(n, (uint32_t)j->send_len);
            if (ok)
                notify(r, j->req, ST_WRITE_COMPLETE, n, 4);
            else
                fail(r, j->req, API_WRITE_DATA, ERR_CONNECTION_ERROR);
            gheap_free(n);
        }
        break;
    }
    case API_RECEIVE_RESPONSE:
    {
        gt_unlock();
        int ok = read_response(r);
        gt_lock();
        if ((r = handle(j->req)))
        {
            if (ok)
            {
                rt_log("[winhttp] %s http://%s:%d%s -> %d\n", r->verb, r->host, r->port, r->path, r->status);
                notify(r, j->req, ST_HEADERS_AVAILABLE, 0, 0);
            }
            else
                fail(r, j->req, API_RECEIVE_RESPONSE, ERR_CONNECTION_ERROR);
        }
        break;
    }
    case API_QUERY_DATA_AVAILABLE:
    {
        gt_unlock();
        size_t n = fill(r);
        gt_lock();
        if ((r = handle(j->req)))
        {
            uint32_t p = gheap_alloc(4, 1);
            wr32(p, (uint32_t)n);
            notify(r, j->req, ST_DATA_AVAILABLE, p, 4);
            gheap_free(p);
        }
        break;
    }
    case API_READ_DATA:
    {
        gt_unlock();
        size_t n = fill(r);
        gt_lock();
        if ((r = handle(j->req)))
        {
            if (n > j->n)
                n = j->n;
            memcpy(GUEST_PTR(j->buf), r->buf, n);
            memmove(r->buf, r->buf + n, r->buf_len - n);
            r->buf_len -= n;
            notify(r, j->req, ST_READ_COMPLETE, j->buf, (uint32_t)n);
        }
        break;
    }
    }
}

/* the worker: a guest thread started at this shim, running jobs forever */
static void sh_WinHttpWorker(Guest* g)
{
    for (;;)
    {
        gt_unlock();
        pthread_mutex_lock(&g_jm);
        while (!g_jcount)
            pthread_cond_wait(&g_jc, &g_jm);
        Job j = g_jobs[g_jhead];
        g_jhead = (g_jhead + 1) % MAX_J;
        g_jcount--;
        pthread_mutex_unlock(&g_jm);
        gt_lock();
        run_job(&j);
        free(j.send);
    }
    RET(0, 1);
}

/* async: queued for the worker (started on first use); sync: done now */
static void submit(Handle* r, Job* j)
{
    if (!(r->flags & FLAG_ASYNC))
    {
        run_job(j);
        free(j->send);
        return;
    }
    if (!g_worker)
    {
        g_worker = 1;
        uint32_t a[6] = { 0, 0, thunk_for("winhttp.dll", "WinHttpWorker"), 0, 0, 0 };
        guest_call(thunk_for("kernel32.dll", "CreateThread"), 6, a);
    }
    pthread_mutex_lock(&g_jm);
    if (g_jcount < MAX_J)
        g_jobs[(g_jhead + g_jcount++) % MAX_J] = *j;
    pthread_cond_signal(&g_jc);
    pthread_mutex_unlock(&g_jm);
}

/* --- the API --------------------------------------------------------------------------------------------- */
/* WinHttpOpen(agent, access, proxy, bypass, flags) */
static void sh_WinHttpOpen(Guest* g)
{
    uint32_t h = new_handle(1, 0);
    Handle* s = handle(h);
    if (s)
    {
        narrow_w(ARG(0), s->agent, sizeof s->agent);
        s->flags = ARG(4);
    }
    RET(h, 5);
}

/* WinHttpConnect(session, server, port, reserved) */
static void sh_WinHttpConnect(Guest* g)
{
    if (!handle(ARG(0)))
        RET(0, 4);
    uint32_t h = new_handle(2, ARG(0));
    Handle* c = handle(h);
    narrow_w(ARG(1), c->host, sizeof c->host);
    c->port = (int)(ARG(2) ? ARG(2) : 80);
    RET(h, 4);
}

/* WinHttpOpenRequest(connection, verb, object, version, referrer, accept types, flags) */
static void sh_WinHttpOpenRequest(Guest* g)
{
    if (!handle(ARG(0)))
        RET(0, 7);
    uint32_t h = new_handle(3, ARG(0));
    Handle* r = handle(h);
    narrow_w(ARG(1), r->verb, sizeof r->verb);
    if (!r->verb[0])
        strcpy(r->verb, "GET");
    narrow_w(ARG(2), r->path, sizeof r->path);
    if (!r->path[0])
        strcpy(r->path, "/");
    RET(h, 7);
}

/* WinHttpSetOption(handle, option, buffer, length): the callback context; the rest are accepted */
static void sh_WinHttpSetOption(Guest* g)
{
    Handle* x = handle(ARG(0));
    if (x && ARG(1) == 0x2d && ARG(2)) /* WINHTTP_OPTION_CONTEXT_VALUE */
        x->context = rd32(ARG(2));
    RET(1, 4);
}

/* WinHttpSetStatusCallback(handle, callback, flags, reserved): the previous callback */
static void sh_WinHttpSetStatusCallback(Guest* g)
{
    Handle* x = handle(ARG(0));
    if (!x)
        RET(0xFFFFFFFFu, 4); /* WINHTTP_INVALID_STATUS_CALLBACK */
    uint32_t old = x->callback;
    x->callback = ARG(1), x->notify = ARG(2);
    RET(old, 4);
}

/* WinHttpSendRequest(request, headers, headers_len, optional, optional_len, total_len, context) */
static void sh_WinHttpSendRequest(Guest* g)
{
    Handle* r = handle(ARG(0));
    if (!r || r->type != 3)
    {
        gt_set_error(ERR_INVALID_HANDLE);
        RET(0, 7);
    }
    if (ARG(6))
        r->context = ARG(6);
    char extra[4096] = "";
    if (ARG(1))
    {
        uint32_t n = ARG(2) == 0xFFFFFFFFu ? 0 : ARG(2);
        size_t k = 0;
        for (uint32_t i = 0; (n ? i < n : rd16(ARG(1) + 2 * i) != 0) && k + 1 < sizeof extra; ++i)
            extra[k++] = (char)rd16(ARG(1) + 2 * i);
        extra[k] = 0;
    }
    char head[8192];
    int len = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: %s\r\nConnection: close\r\n", r->verb,
        r->path, r->host, r->port, r->agent);
    if (ARG(5) || ARG(4))
        len += snprintf(head + len, sizeof head - (size_t)len, "Content-Length: %u\r\n", ARG(5) ? ARG(5) : ARG(4));
    if (extra[0])
        len += snprintf(head + len, sizeof head - (size_t)len, "%s%s", extra,
            strlen(extra) >= 2 && !strcmp(extra + strlen(extra) - 2, "\r\n") ? "" : "\r\n");
    len += snprintf(head + len, sizeof head - (size_t)len, "\r\n");
    Job j = { API_SEND_REQUEST, ARG(0), 0, 0, NULL, 0 };
    j.send = malloc((size_t)len + ARG(4));
    memcpy(j.send, head, (size_t)len);
    if (ARG(3) && ARG(4))
        memcpy(j.send + len, GUEST_PTR(ARG(3)), ARG(4));
    j.send_len = (size_t)len + (ARG(3) ? ARG(4) : 0);
    submit(r, &j);
    RET(1, 7);
}

/* WinHttpWriteData(request, buffer, length, written*) */
static void sh_WinHttpWriteData(Guest* g)
{
    Handle* r = handle(ARG(0));
    if (!r)
        RET(0, 4);
    Job j = { API_WRITE_DATA, ARG(0), 0, 0, malloc(ARG(2) ? ARG(2) : 1), ARG(2) };
    memcpy(j.send, GUEST_PTR(ARG(1)), ARG(2));
    if (ARG(3))
        wr32(ARG(3), ARG(2));
    submit(r, &j);
    RET(1, 4);
}

static void sh_WinHttpReceiveResponse(Guest* g)
{
    Handle* r = handle(ARG(0));
    if (!r)
        RET(0, 2);
    Job j = { API_RECEIVE_RESPONSE, ARG(0), 0, 0, NULL, 0 };
    submit(r, &j);
    RET(1, 2);
}

/* WinHttpQueryHeaders(request, level, name, buffer, length*, index*): the raw headers
 * (WINHTTP_QUERY_RAW_HEADERS_CRLF) or the status code; the length is in bytes, UTF-16 */
static void sh_WinHttpQueryHeaders(Guest* g)
{
    Handle* r = handle(ARG(0));
    uint32_t level = ARG(1) & 0xFFFF, number = ARG(1) & 0x20000000u, buf = ARG(3), lenp = ARG(4);
    if (!r || !r->head)
    {
        gt_set_error(ERR_HEADER_NOT_FOUND);
        RET(0, 6);
    }
    char text[16384];
    if (level == 22) /* RAW_HEADERS_CRLF */
        snprintf(text, sizeof text, "%s", r->head);
    else if (level == 19) /* STATUS_CODE */
    {
        if (number && buf)
        {
            wr32(buf, (uint32_t)r->status);
            wr32(lenp, 4);
            RET(1, 6);
        }
        snprintf(text, sizeof text, "%d", r->status);
    }
    else
    {
        rt_log("[winhttp] WinHttpQueryHeaders level %u is not served\n", level);
        gt_set_error(ERR_HEADER_NOT_FOUND);
        RET(0, 6);
    }
    uint32_t need = 2 * (uint32_t)(strlen(text) + 1);
    if (!buf || rd32(lenp) < need)
    {
        wr32(lenp, need);
        gt_set_error(ERR_INSUFFICIENT_BUFFER);
        RET(0, 6);
    }
    for (uint32_t i = 0; i < need / 2; ++i)
        wr16(buf + 2 * i, (uint8_t)text[i]);
    wr32(lenp, need - 2);
    RET(1, 6);
}

/* WinHttpQueryDataAvailable(request, available*) */
static void sh_WinHttpQueryDataAvailable(Guest* g)
{
    Handle* r = handle(ARG(0));
    if (!r)
        RET(0, 2);
    if (!(r->flags & FLAG_ASYNC))
    {
        size_t n = fill(r);
        if (ARG(1))
            wr32(ARG(1), (uint32_t)n);
        RET(1, 2);
    }
    Job j = { API_QUERY_DATA_AVAILABLE, ARG(0), 0, 0, NULL, 0 };
    submit(r, &j);
    RET(1, 2);
}

/* WinHttpReadData(request, buffer, length, read*) */
static void sh_WinHttpReadData(Guest* g)
{
    Handle* r = handle(ARG(0));
    if (!r)
        RET(0, 4);
    if (!(r->flags & FLAG_ASYNC))
    {
        size_t n = fill(r);
        if (n > ARG(2))
            n = ARG(2);
        memcpy(GUEST_PTR(ARG(1)), r->buf, n);
        memmove(r->buf, r->buf + n, r->buf_len - n);
        r->buf_len -= n;
        if (ARG(3))
            wr32(ARG(3), (uint32_t)n);
        RET(1, 4);
    }
    if (ARG(3))
        wr32(ARG(3), 0);
    Job j = { API_READ_DATA, ARG(0), ARG(1), ARG(2), NULL, 0 };
    submit(r, &j);
    RET(1, 4);
}

static void sh_WinHttpCloseHandle(Guest* g)
{
    Handle* x = handle(ARG(0));
    if (!x)
        RET(0, 1);
    uint32_t h = ARG(0);
    if (x->sock >= 0)
        close(x->sock);
    free(x->head);
    free(x->buf);
    if (x->callback && (x->notify & ST_HANDLE_CLOSING))
    {
        uint32_t p = gheap_alloc(4, 1);
        wr32(p, h);
        notify(x, h, ST_HANDLE_CLOSING, p, 4);
        gheap_free(p);
    }
    if ((x = handle(h)))
        memset(x, 0, sizeof *x);
    RET(1, 1);
}

static const ShimDef k_shims[] = {
    { "winhttp.dll", "WinHttpOpen", sh_WinHttpOpen },
    { "winhttp.dll", "WinHttpConnect", sh_WinHttpConnect },
    { "winhttp.dll", "WinHttpOpenRequest", sh_WinHttpOpenRequest },
    { "winhttp.dll", "WinHttpSetOption", sh_WinHttpSetOption },
    { "winhttp.dll", "WinHttpSetStatusCallback", sh_WinHttpSetStatusCallback },
    { "winhttp.dll", "WinHttpSendRequest", sh_WinHttpSendRequest },
    { "winhttp.dll", "WinHttpWriteData", sh_WinHttpWriteData },
    { "winhttp.dll", "WinHttpReceiveResponse", sh_WinHttpReceiveResponse },
    { "winhttp.dll", "WinHttpQueryHeaders", sh_WinHttpQueryHeaders },
    { "winhttp.dll", "WinHttpQueryDataAvailable", sh_WinHttpQueryDataAvailable },
    { "winhttp.dll", "WinHttpReadData", sh_WinHttpReadData },
    { "winhttp.dll", "WinHttpCloseHandle", sh_WinHttpCloseHandle },
    { "winhttp.dll", "WinHttpWorker", sh_WinHttpWorker },
    { NULL, NULL, NULL },
};

/* wins over xiv_misc.c's stubs (the game's news and banner fetches, which fail by design) */
void winhttp_init(void) { thunk_override(k_shims); }
