/* Signing in to a LandSandBoat server.
 *
 * gamecore is our own and Winsock is our shim, so each piece of the sign-in has a direct home:
 *
 *   1. Auth (TLS, port 54231, xi_connect): one JSON request - username, password, OTP, the loader
 *      version, command 0x10 (login) - and one JSON reply: result 1 with account_id and the 16-byte
 *      session_hash, or error_message.
 *   2. The login data connection (TCP 54230): we send 0xFE + the hash, then answer the server for
 *      the rest of the run - 0x01 with 0xA1 (account id, server address, hash), 0x02 / 0x15 with
 *      0xA2 and the server's fixed key, 0x03 (the character list) with nothing: our gamecore
 *      builds its character records from FFXiMain's own table.
 *   3. The lobby (TCP 54001 and 54230): every lobby command FFXiMain sends carries the session
 *      hash at +12, where the server looks for it (ws2_set_lobby_session).
 *   4. gamecore: there is no account-service session, so the session value is zero, and LSB's
 *      zone key is derived from exactly that (the 0xA2 key is MD5 input the client also computes),
 *      so our gamecore reports 16 zero bytes. Its command line carries the view port.
 *   5. The game's lobby host name resolves to the server (host64's --server). */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if !defined(FFXI_UWP) /* UWP apps have no SChannel and no console: uwp_bridge.h */
#include <security.h>
#include <schannel.h>
#include <conio.h>
#include <io.h>
#endif
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#if !defined(_WIN32) /* Windows has its own TLS: SChannel */
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#endif
#endif

#include "lsb_login.h"
#include "plat.h"
#if defined(FFXI_UWP)
#include "uwp_bridge.h"
#endif
#include "gamecore_config.h"
#include "ws2.h"

int lsb_parse_version(const char* s, int out[3])
{
    if (!s)
        return 0;
    for (int i = 0; i < 3; ++i)
    {
        if (*s < '0' || *s > '9')
            return 0;
        char* end;
        long v = strtol(s, &end, 10);
        if (v > 65535 || *end != (i < 2 ? '.' : 0))
            return 0;
        out[i] = (int)v;
        s = end + (i < 2);
    }
    return 1;
}

#define TIMEOUT_MS 15000

static sock_t tcp_connect(uint32_t server, uint16_t port, int timeout_ms, char* err, size_t errn)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD)
    {
        snprintf(err, errn, "no socket");
        return SOCK_BAD;
    }
    if (timeout_ms)
    {
#if defined(_WIN32)
        DWORD tv = (DWORD)timeout_ms;
#else
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(server);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0)
    {
        snprintf(err, errn, "cannot reach %u.%u.%u.%u:%u", server >> 24, (server >> 16) & 255, (server >> 8) & 255, server & 255, port);
        sock_close(s);
        return SOCK_BAD;
    }
    return s;
}

/* --- TLS over our own socket ----------------------------------------------------------------------- */
#if defined(FFXI_UWP)
/* the app's (Windows.Networking.Sockets) */
static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    return uwp_tls_exchange(server, port, request, reply, replyn, err, errn);
}
#elif defined(_WIN32)
/* SChannel: the handshake by hand over the socket, then one encrypted request and one reply */
static int send_all(sock_t s, const void* p, size_t n)
{
    for (size_t done = 0; done < n;)
    {
        int k = send(s, (const char*)p + done, (int)(n - done), 0);
        if (k <= 0)
            return 0;
        done += (size_t)k;
    }
    return 1;
}

static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    sock_t s = tcp_connect(server, port, TIMEOUT_MS, err, errn);
    if (s == SOCK_BAD)
        return 0;
    int ok = 0, have_cred = 0, have_ctx = 0;
    CredHandle cred;
    CtxtHandle ctx;
    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    /* private servers present self-signed certificates */
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    if (AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &cred, NULL) != SEC_E_OK)
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    have_cred = 1;
    static char in[32768];
    size_t got = 0;
    for (;;)
    {
        SecBuffer ib[2] = { { (unsigned long)got, SECBUFFER_TOKEN, in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBuffer ob[1] = { { 0, SECBUFFER_TOKEN, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib }, od = { SECBUFFER_VERSION, 1, ob };
        unsigned long flags = ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM;
        SECURITY_STATUS st = InitializeSecurityContextA(&cred, have_ctx ? &ctx : NULL, have_ctx ? NULL : (SEC_CHAR*)"ffxi", flags, 0, 0,
            have_ctx ? &id : NULL, 0, have_ctx ? NULL : &ctx, &od, &flags, NULL);
        have_ctx = 1;
        if (ib[1].BufferType == SECBUFFER_EXTRA)
        {
            memmove(in, in + (got - ib[1].cbBuffer), ib[1].cbBuffer);
            got = ib[1].cbBuffer;
        }
        else if (st != SEC_E_INCOMPLETE_MESSAGE)
            got = 0;
        if (ob[0].pvBuffer)
        {
            int sent = !ob[0].cbBuffer || send_all(s, ob[0].pvBuffer, ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!sent)
                st = SEC_E_INTERNAL_ERROR;
        }
        if (st == SEC_E_OK)
            break;
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_INCOMPLETE_MESSAGE)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (0x%08lx)", (unsigned long)st);
            goto out;
        }
        int n = got < sizeof in ? recv(s, in + got, (int)(sizeof in - got), 0) : 0;
        if (n <= 0)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (connection closed)");
            goto out;
        }
        got += (size_t)n;
    }
    SecPkgContext_StreamSizes sz;
    if (QueryContextAttributesA(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz) != SEC_E_OK)
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    /* the request, one record at a time */
    size_t n = strlen(request);
    char* rec = (char*)malloc(sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
    for (size_t done = 0; done < n;)
    {
        size_t part = n - done < sz.cbMaximumMessage ? n - done : sz.cbMaximumMessage;
        memcpy(rec + sz.cbHeader, request + done, part);
        SecBuffer b[4] = { { sz.cbHeader, SECBUFFER_STREAM_HEADER, rec }, { (unsigned long)part, SECBUFFER_DATA, rec + sz.cbHeader },
            { sz.cbTrailer, SECBUFFER_STREAM_TRAILER, rec + sz.cbHeader + part }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK || !send_all(s, rec, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))
        {
            SecureZeroMemory(rec, sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
            free(rec);
            snprintf(err, errn, "could not send the login request");
            goto out;
        }
        done += part;
    }
    SecureZeroMemory(rec, sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
    free(rec);
    /* the reply: the first record with data in it */
    for (;;)
    {
        if (got)
        {
            SecBuffer b[4] = { { (unsigned long)got, SECBUFFER_DATA, in }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL },
                { 0, SECBUFFER_EMPTY, NULL } };
            SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
            SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, NULL);
            if (st == SEC_E_OK)
            {
                SecBuffer *data = NULL, *extra = NULL;
                for (int i = 1; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_DATA)
                        data = &b[i];
                    else if (b[i].BufferType == SECBUFFER_EXTRA)
                        extra = &b[i];
                size_t k = 0;
                if (data)
                {
                    k = data->cbBuffer < replyn - 1 ? data->cbBuffer : replyn - 1;
                    memcpy(reply, data->pvBuffer, k);
                }
                if (extra)
                    memmove(in, in + (got - extra->cbBuffer), extra->cbBuffer), got = extra->cbBuffer;
                else
                    got = 0;
                if (k)
                {
                    reply[k] = 0;
                    ok = 1;
                    break;
                }
                continue; /* a record with nothing for us (a session ticket) */
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE)
            {
                snprintf(err, errn, "the login server did not reply");
                break;
            }
        }
        int k = got < sizeof in ? recv(s, in + got, (int)(sizeof in - got), 0) : 0;
        if (k <= 0)
        {
            snprintf(err, errn, "the login server did not reply");
            break;
        }
        got += (size_t)k;
    }
out:
    if (have_ctx)
        DeleteSecurityContext(&ctx);
    if (have_cred)
        FreeCredentialsHandle(&cred);
    sock_close(s);
    return ok;
}
#else
static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    long n = (long)send(*(sock_t*)ctx, (const char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    long n = (long)recv(*(sock_t*)ctx, (char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_TIMEOUT : n == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : (int)n;
}

/* one request, one reply (NUL-terminated); 0 with a message on failure */
static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    sock_t s = tcp_connect(server, port, TIMEOUT_MS, err, errn);
    if (s == SOCK_BAD)
        return 0;
    int ok = 0, r;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"lsblogin", 8);
#endif
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    /* private servers present self-signed certificates */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#endif
    if (mbedtls_ssl_setup(&ssl, &conf))
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    mbedtls_ssl_set_bio(&ssl, &s, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (-0x%04x)", (unsigned)-r);
            goto out;
        }
    size_t n = strlen(request), done = 0;
    while (done < n)
    {
        r = mbedtls_ssl_write(&ssl, (const unsigned char*)request + done, n - done);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0)
        {
            snprintf(err, errn, "could not send the login request");
            goto out;
        }
        done += (size_t)r;
    }
    do
        r = mbedtls_ssl_read(&ssl, (unsigned char*)reply, replyn - 1);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
           || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
    );
    if (r <= 0)
    {
        snprintf(err, errn, "the login server did not reply");
        goto out;
    }
    reply[r] = 0;
    ok = 1;
    mbedtls_ssl_close_notify(&ssl);
out:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
#endif
    sock_close(s);
    return ok;
}
#endif

/* --- the little JSON xi_connect speaks ------------------------------------------------------------- */
static void json_string(char* out, size_t n, const char* s)
{
    size_t o = 0;
    out[o++] = '"';
    for (; *s && o + 7 < n; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            out[o++] = '\\', out[o++] = (char)c;
        else if (c < 0x20)
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else
            out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
}

/* the value after "key": in a flat object, or NULL */
static const char* json_find(const char* j, const char* key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = strstr(j, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    return p;
}

static int json_int(const char* j, const char* key, long long* out)
{
    const char* p = json_find(j, key);
    if (!p || !((*p >= '0' && *p <= '9') || *p == '-'))
        return 0;
    *out = strtoll(p, NULL, 10);
    return 1;
}

/* a JSON string's text, unescaped (\uXXXX beyond ASCII as '?') */
static int json_str(const char* j, const char* key, char* out, size_t n)
{
    const char* p = json_find(j, key);
    if (!p || *p != '"')
        return 0;
    size_t o = 0;
    for (++p; *p && *p != '"' && o + 1 < n; ++p)
    {
        if (*p == '\\' && p[1])
        {
            ++p;
            if (*p == 'u')
            {
                /* \uXXXX: ASCII as itself, anything else as '?' */
                char hex[5] = { 0 };
                int k = 0;
                for (; k < 4 && isxdigit((unsigned char)p[1]); ++k)
                    hex[k] = *++p;
                unsigned long c = strtoul(hex, NULL, 16);
                out[o++] = k == 4 && c < 0x80 ? (char)c : '?';
            }
            else
                out[o++] = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : *p;
        }
        else
            out[o++] = *p;
    }
    out[o] = 0;
    return 1;
}

/* --- the login data connection --------------------------------------------------------------------- */
typedef struct DataConn
{
    sock_t s;
    uint32_t account, server;
    uint8_t hash[16];
} DataConn;

static DataConn g_data;

static void data_thread(void* arg)
{
    DataConn* d = (DataConn*)arg;
    uint8_t in[4096], out[28];
    for (;;)
    {
        long n = (long)recv(d->s, (char*)in, (int)sizeof in, 0);
        if (n <= 0)
        {
            fprintf(stderr, "[lsb] login data connection closed\n");
            return;
        }
        memset(out, 0, sizeof out);
        switch (in[0])
        {
        case 0x01: /* who is this: the account, the server as the client sees it, the hash */
            out[0] = 0xA1;
            for (int i = 0; i < 4; ++i)
                out[1 + i] = (uint8_t)(d->account >> (8 * i));
            for (int i = 0; i < 4; ++i) /* in_addr bytes: a.b.c.d */
                out[5 + i] = (uint8_t)(d->server >> (24 - 8 * i));
            memcpy(out + 12, d->hash, 16);
            break;
        case 0x02:
        case 0x15: /* the key: the constant a zero session value gives */
            out[0] = 0xA2;
            out[17] = 0x58, out[18] = 0xE0, out[19] = 0x5D, out[20] = 0xAD;
            break;
        default: /* 0x03 the character list, and anything else: no answer */
            continue;
        }
        if (send(d->s, (const char*)out, sizeof out, 0) <= 0)
        {
            fprintf(stderr, "[lsb] login data connection lost\n");
            return;
        }
    }
}

int net_resolve_ipv4(const char* name, uint32_t* out)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res)
        return 0;
    *out = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 1;
}

int read_secret(const char* prompt, char* out, size_t n)
{
#if defined(FFXI_UWP)
    (void)prompt, (void)out, (void)n;
    return 0; /* no terminal: the app asks */
#elif defined(_WIN32)
    if (!_isatty(_fileno(stdin)))
        return 0;
    fputs(prompt, stderr);
    size_t o = 0;
    for (int c; (c = _getch()) != '\r' && c != '\n' && c != EOF;)
        if (c == 8 && o)
            o--;
        else if (c >= 32 && o + 1 < n)
            out[o++] = (char)c;
    out[o] = 0;
    fputs("\n", stderr);
    return 1;
#else
    if (!isatty(STDIN_FILENO))
        return 0;
    fputs(prompt, stderr);
    fflush(stderr);
    struct termios old, quiet;
    tcgetattr(STDIN_FILENO, &old);
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    int ok = fgets(out, (int)n, stdin) != NULL;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    fputs("\n", stderr);
    if (!ok)
        return 0;
    out[strcspn(out, "\r\n")] = 0;
    return 1;
#endif
}

/* The version a refusal names, an x (or *, or a missing patch) as 0. Servers word it differently -
 * "Unsupported ... version 1.0.0.\nThis server requires version 2.2.x.", "Please update to version
 * '2.0.x'", "Needs 2.2.x" - so every major.minor[.patch] in the message is a candidate and
 * the last one other than the version we sent wins (the requirement follows the echo of ours). */
static int server_version(const char* message, const int sent[3], int out[3])
{
    int found = 0;
    for (const char* p = message; *p; ++p)
    {
        if (*p < '0' || *p > '9' || (p > message && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9'))))
            continue;
        int v[3] = { 0, 0, 0 }, parts = 0;
        const char* q = p;
        while (parts < 3)
        {
            if (*q >= '0' && *q <= '9')
            {
                char* end;
                long n = strtol(q, &end, 10);
                if (n > 65535)
                    break;
                v[parts++] = (int)n;
                q = end;
            }
            else if (parts == 2 && (*q == 'x' || *q == 'X' || *q == '*'))
                v[parts++] = 0, ++q;
            else
                break;
            if (parts == 3 || *q != '.' || !q[1] || !(q[1] == 'x' || q[1] == 'X' || q[1] == '*' || (q[1] >= '0' && q[1] <= '9')))
                break;
            ++q;
        }
        if (parts < 2)
            continue;
        if (memcmp(v, sent, sizeof v) || !found)
        {
            memcpy(out, v, sizeof v);
            found = memcmp(v, sent, sizeof v) ? 2 : 1;
        }
        p = q - 1;
    }
    return found == 2;
}

int lsb_login(const LsbLogin* l, char* err, size_t errn)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int version[3];
    const char* v = l->version && *l->version ? l->version : LSB_LOADER_VERSION;
    if (!lsb_parse_version(v, version))
    {
        snprintf(err, errn, "the loader version \"%s\" is not major.minor.patch", v);
        return 0;
    }
    char user[160], pass[160], otp[64], token[600], req[1400];
    json_string(user, sizeof user, l->user);
    json_string(pass, sizeof pass, l->password ? l->password : "");
    json_string(otp, sizeof otp, l->otp ? l->otp : "");
    /* login_token, which some servers add: their launcher's single-use token
     * stands in for the password and the OTP */
    token[0] = 0;
    if (l->login_token && *l->login_token)
    {
        char t[560];
        json_string(t, sizeof t, l->login_token);
        snprintf(token, sizeof token, "\"login_token\":%s,", t);
    }
    if (l->version_used)
        l->version_used[0] = 0;
    char reply[8192], message[512];
    int adopted = 0;
    for (int attempt = 0;; ++attempt)
    {
        snprintf(req, sizeof req,
            "{\"command\":16,%s\"new_password\":\"\",\"otp\":%s,\"password\":%s,\"trust_this_computer\":false,"
            "\"trust_token\":\"\",\"username\":%s,\"version\":[%d,%d,%d]}",
            token, otp, pass, user, version[0], version[1], version[2]);
        int ok = tls_exchange(l->server, l->auth_port, req, reply, sizeof reply, err, errn);
        memset(req, 0, sizeof req);
        if (!ok)
        {
            memset(pass, 0, sizeof pass);
            return 0;
        }
        /* A version the server refuses names the one it wants ("This server requires version
         * 2.2.x."). Sign in again with that (x as 0) and report it for next time; a second refusal
         * naming yet another version is followed too, up to a few tries. */
        int want[3];
        if (attempt < 3 && json_str(reply, "error_message", message, sizeof message)
            && server_version(message, version, want))
        {
            fprintf(stderr, "[lsb] the server wants loader version %d.%d.%d, not %d.%d.%d: signing in with that\n",
                want[0], want[1], want[2], version[0], version[1], version[2]);
            memcpy(version, want, sizeof version);
            adopted = 1;
            continue;
        }
        break;
    }
    memset(pass, 0, sizeof pass);

    if (json_str(reply, "error_message", message, sizeof message) && message[0])
    {
        /* its lines (\r\n) and other control characters as single spaces: the message is shown on
         * one line, in the log and on the sign-in screen */
        size_t o = 0;
        for (const char* p = message; *p; ++p)
            if ((unsigned char)*p < 0x20 || *p == ' ')
            {
                if (o && message[o - 1] != ' ')
                    message[o++] = ' ';
            }
            else
                message[o++] = *p;
        while (o && message[o - 1] == ' ')
            --o;
        message[o] = 0;
        snprintf(err, errn, "the server says: %s", message);
        return 0;
    }
    long long result = 0, account = 0;
    if (!json_int(reply, "result", &result))
    {
        snprintf(err, errn, "the login server's reply has no result");
        return 0;
    }
    if (result == 2)
    {
        snprintf(err, errn, "invalid username or password");
        return 0;
    }
    if (result == 0x14)
    {
        snprintf(err, errn, "the launch token is invalid or expired: get a new one from the server's launcher");
        return 0;
    }
    if (result != 1 || !json_int(reply, "account_id", &account))
    {
        snprintf(err, errn, "the login server answered %lld", result);
        return 0;
    }
    /* session_hash: 16 numbers (signed chars) */
    const char* p = json_find(reply, "session_hash");
    uint8_t hash[16];
    int k = 0;
    if (p && *p == '[')
        for (++p; k < 16 && *p && *p != ']';)
        {
            char* end;
            long v = strtol(p, &end, 10);
            if (end == p)
                break;
            hash[k++] = (uint8_t)v;
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
    if (k != 16)
    {
        snprintf(err, errn, "the login server sent no session hash");
        return 0;
    }

    /* the data connection, answered for the rest of the run */
    g_data.s = tcp_connect(l->server, l->data_port, 0, err, errn);
    if (g_data.s == SOCK_BAD)
        return 0;
    g_data.account = (uint32_t)account, g_data.server = l->server;
    memcpy(g_data.hash, hash, 16);
    uint8_t first[28] = { 0xFE };
    memcpy(first + 12, hash, 16);
    if (send(g_data.s, (const char*)first, sizeof first, 0) <= 0 || !plat_thread_start(data_thread, &g_data))
    {
        snprintf(err, errn, "the login data connection failed");
        sock_close(g_data.s);
        return 0;
    }

    ws2_set_lobby_session(hash, l->data_port, l->view_port);
    static const uint8_t zero[16] = { 0 };
    gamecore_set_session(zero);
    char cmd[64];
    snprintf(cmd, sizeof cmd, " /game eAZcFcB -net 3 -port %u", l->view_port);
    gamecore_set_cmdline(cmd);
    fprintf(stderr, "[lsb] signed in to LSB as %s (account %lld)\n", l->user, account);
    if (adopted && l->version_used)
        snprintf(l->version_used, 24, "%d.%d.%d", version[0], version[1], version[2]);
    return 1;
}
