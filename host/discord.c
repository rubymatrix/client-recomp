/* Discord Rich Presence: the character, job and zone on the player's Discord profile.
 *
 * No SDK: the Discord client listens on a local socket (discord-ipc-0..9 in $XDG_RUNTIME_DIR,
 * $TMPDIR, $TMP, $TEMP or /tmp; \\.\pipe\discord-ipc-N on Windows) for frames of
 *     u32 opcode (LE), u32 length (LE), JSON
 * Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a command, 2 close; Discord answers each
 * with a frame we read and drop. The command here is SET_ACTIVITY. Discord rate-limits it (about 5
 * per 20 s), so an activity goes out only when its text changes, and at most every few seconds.
 *
 * The game state is the addon host's readers (host/addons/game.c, its Ashita memory layout): the
 * name and zone of party member 0 (the player), the jobs from player_t, the zone's name from the
 * DATs (host/addons/res.c). All read on the game's thread from the Present hook. */
#include "discord.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime.h" /* rt_monotonic_ns */
#include "addons/game.h"
#include "addons/res.h"

#if defined(FFXI_UWP)
/* no Discord client on the console */
void discord_init(void) {}
void discord_frame(void) {}
void discord_signin_frame(void) {}
#else

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* The Discord application whose name ("Final Fantasy XI") and art the profile shows. */
#ifndef DISCORD_APP_ID
#define DISCORD_APP_ID "1555045037211910154"
#endif

#define POLL_NS 2000000000ull       /* how often the game state is read */
#define MIN_SEND_NS 5000000000ull   /* between activities, under Discord's rate limit */
#define RETRY_NS 15000000000ull     /* between attempts to reach Discord */

static int g_on, g_show_name = 1;
static char g_app_id[32];
static uint64_t g_next_poll, g_next_try, g_last_send;
static char g_sent[1024];    /* the activity Discord has (its JSON), "" if none */
static char g_pending[1024]; /* the latest one, waiting out the rate limit */
static int64_t g_start;      /* when this character entered the world (Unix seconds) */
static char g_who[32];       /* whose session g_start times */
static unsigned g_nonce;
static int g_in_game; /* the game's frames have started: its memory can be read */

/* ---- the socket ------------------------------------------------------------------------------ */

#ifdef _WIN32
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
#define CONNECTED (g_pipe != INVALID_HANDLE_VALUE)

static int ipc_open(void)
{
    for (int i = 0; i < 10; ++i)
    {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE)
            return 1;
    }
    return 0;
}

static void ipc_close(void)
{
    if (g_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

static int ipc_write(const void* p, uint32_t n)
{
    DWORD done = 0;
    return WriteFile(g_pipe, p, n, &done, NULL) && done == n;
}

/* Reads and drops what Discord sent; 0 if the pipe closed. Never blocks. */
static int ipc_drain(void)
{
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL))
            return 0;
        if (!avail)
            return 1;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(g_pipe, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL) || !got)
            return 0;
    }
}

static long ipc_pid(void) { return (long)GetCurrentProcessId(); }
#else
static int g_fd = -1;
#define CONNECTED (g_fd >= 0)

static int ipc_open(void)
{
    const char* env[] = { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" };
    const char* dirs[6];
    int ndirs = 0;
    for (unsigned k = 0; k < sizeof env / sizeof *env; ++k)
        if (getenv(env[k]) && getenv(env[k])[0])
            dirs[ndirs++] = getenv(env[k]);
    dirs[ndirs++] = "/tmp";
    for (int d = 0; d < ndirs; ++d)
        for (int i = 0; i < 10; ++i)
        {
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof sa);
            sa.sun_family = AF_UNIX;
            size_t len = strlen(dirs[d]);
            const char* sep = len && dirs[d][len - 1] == '/' ? "" : "/";
            if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s%sdiscord-ipc-%d", dirs[d], sep, i) >= (int)sizeof sa.sun_path)
                continue;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0)
                return 0;
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            if (connect(fd, (struct sockaddr*)&sa, sizeof sa) == 0)
            {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                g_fd = fd;
                return 1;
            }
            close(fd);
        }
    return 0;
}

static void ipc_close(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
}

static int ipc_write(const void* p, uint32_t n)
{
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    const char* c = p;
    int spins = 0;
    while (n)
    {
        ssize_t w = send(g_fd, c, n, flags);
        if (w > 0)
            c += w, n -= (uint32_t)w;
        else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++spins < 1000)
            continue; /* a few KB to a local socket: only full if Discord stopped reading */
        else
            return 0;
    }
    return 1;
}

static int ipc_drain(void)
{
    for (;;)
    {
        char buf[4096];
        ssize_t got = recv(g_fd, buf, sizeof buf, 0);
        if (got > 0)
            continue;
        return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
}

static long ipc_pid(void) { return (long)getpid(); }
#endif

static int ipc_send(uint32_t op, const char* json)
{
    uint32_t len = (uint32_t)strlen(json);
    unsigned char head[8] = { (unsigned char)op, (unsigned char)(op >> 8), (unsigned char)(op >> 16), (unsigned char)(op >> 24),
                              (unsigned char)len, (unsigned char)(len >> 8), (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
    return ipc_write(head, 8) && ipc_write(json, len);
}

static void disconnect(void)
{
    ipc_close();
    g_sent[0] = '\0'; /* a new connection starts with no activity */
}

static int connect_discord(void)
{
    if (!ipc_open())
        return 0;
    char hello[96];
    snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", g_app_id);
    if (!ipc_send(0, hello))
    {
        disconnect();
        return 0;
    }
    fprintf(stderr, "discord: connected\n");
    return 1;
}

/* ---- the activity ---------------------------------------------------------------------------- */

/* `s` as the inside of a JSON string, cut to at most `max` bytes (Discord's limit is 128). */
static void json_str(char* out, size_t cap, const char* s, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; s[i] && i < max && o + 7 < cap; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            out[o++] = '\\', out[o++] = (char)c;
        else if (c < 0x20)
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else
            out[o++] = (char)c;
    }
    out[o] = '\0';
}

static int read_field(int sid, const char* name, uint32_t base, xi_value* v)
{
    uint32_t extra = 0;
    const xi_field* f = base ? xi_game_lookup(sid, name, &extra) : NULL;
    return f && xi_game_read(f, base, extra, -1, v) != XI_V_NIL;
}

static int read_num(int sid, const char* name, uint32_t base)
{
    xi_value v;
    return read_field(sid, name, base, &v) && v.kind == XI_V_NUM ? (int)v.num : -1;
}

static const char* job_abbr(int id)
{
    const ResJob* j = id > 0 ? res_job((uint32_t)id) : NULL;
    return j && j->abbr[0] && j->abbr[0][0] ? j->abbr[0] : NULL;
}

/* The activity's JSON for what the game shows now, "" for none. */
static void build_activity(char* out, size_t cap)
{
    static int s_party = -2, s_player = -2;
    if (s_party == -2)
        s_party = xi_game_struct_id("partymember_t"), s_player = xi_game_struct_id("player_t");

    char details[160] = "", state[160] = "";
    int status = g_in_game ? xi_game_login_status() : 0;
    uint32_t me = status ? xi_game_party_member(0) : 0;
    xi_value name;
    name.str[0] = '\0';
    if (status == 0 || !read_field(s_party, "Name", me, &name) || !name.str[0])
    {
        g_who[0] = '\0';
        snprintf(details, sizeof details, "Signing in");
    }
    else
    {
        if (strcmp(g_who, name.str))
        {
            snprintf(g_who, sizeof g_who, "%s", name.str);
            g_start = (int64_t)time(NULL);
        }
        uint32_t pl = xi_game_player();
        const char* main = job_abbr(read_num(s_player, "MainJob", pl));
        const char* sub = job_abbr(read_num(s_player, "SubJob", pl));
        int lv = read_num(s_player, "MainJobLevel", pl), slv = read_num(s_player, "SubJobLevel", pl);
        char job[48] = "";
        if (main && lv > 0)
        {
            if (sub && slv > 0)
                snprintf(job, sizeof job, "%s%d/%s%d", main, lv, sub, slv);
            else
                snprintf(job, sizeof job, "%s%d", main, lv);
        }
        if (g_show_name)
            snprintf(details, sizeof details, job[0] ? "%s (%s)" : "%s", name.str, job);
        else
            snprintf(details, sizeof details, "%s", job[0] ? job : "In Vana'diel");

        int zone = read_num(s_party, "Zone", me);
        const ResZone* z = zone > 0 ? res_zone((uint32_t)zone) : NULL;
        if (status == 1)
            snprintf(state, sizeof state, "Zoning");
        else if (z && z->name[0] && z->name[0][0])
            snprintf(state, sizeof state, "%s", z->name[0]);
    }

    char d[260], s[260], t[48] = "";
    json_str(d, sizeof d, details, 128);
    json_str(s, sizeof s, state, 128);
    if (g_who[0] && g_start)
        snprintf(t, sizeof t, ",\"timestamps\":{\"start\":%lld}", (long long)g_start);
    snprintf(out, cap,
             "{\"details\":\"%s\"%s%s%s%s,\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Final Fantasy XI\"}}",
             d, s[0] ? ",\"state\":\"" : "", s, s[0] ? "\"" : "", t);
}

static int send_activity(const char* activity)
{
    char msg[1400];
    snprintf(msg, sizeof msg, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":%s},\"nonce\":\"%u\"}",
             ipc_pid(), activity[0] ? activity : "null", ++g_nonce);
    return ipc_send(1, msg);
}

/* ---- the hooks ------------------------------------------------------------------------------- */

void discord_init(void)
{
    const char* off = getenv("FFXI_DISCORD");
    const char* id = getenv("FFXI_DISCORD_APP_ID");
    const char* nm = getenv("FFXI_DISCORD_NAME");
    snprintf(g_app_id, sizeof g_app_id, "%s", id && id[0] ? id : DISCORD_APP_ID);
    g_show_name = !(nm && !strcmp(nm, "0"));
    g_on = !(off && !strcmp(off, "0")) && g_app_id[0];
    if (!g_app_id[0] && !(off && !strcmp(off, "0")))
        fprintf(stderr, "discord: no application id (FFXI_DISCORD_APP_ID); rich presence is off\n");
}

static void tick(void)
{
    if (!g_on)
        return;
    uint64_t now = rt_monotonic_ns();
    if (now < g_next_poll)
        return;
    g_next_poll = now + POLL_NS;

    if (!CONNECTED)
    {
        if (now < g_next_try)
            return;
        g_next_try = now + RETRY_NS;
        connect_discord();
        return; /* the activity goes out once Discord has answered the handshake */
    }
    if (!ipc_drain())
    {
        fprintf(stderr, "discord: disconnected\n");
        disconnect();
        return;
    }
    build_activity(g_pending, sizeof g_pending);
    if (!strcmp(g_pending, g_sent) || (g_last_send && now - g_last_send < MIN_SEND_NS))
        return;
    if (!send_activity(g_pending))
    {
        disconnect();
        return;
    }
    snprintf(g_sent, sizeof g_sent, "%s", g_pending);
    g_last_send = now;
}

void discord_frame(void)
{
    g_in_game = 1;
    tick();
}

void discord_signin_frame(void) { tick(); }
#endif
