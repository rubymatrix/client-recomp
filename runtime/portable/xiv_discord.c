/* Discord Rich Presence: "Playing FINAL FANTASY XIV" on the player's Discord profile, with where
 * they are: "At the main menu", "At character select", or the character ("Josh Dev Lv 50 PLD").
 *
 * No SDK: the Discord client listens on a local socket (discord-ipc-0..9 in $XDG_RUNTIME_DIR,
 * $TMPDIR, $TMP, $TEMP or /tmp) for frames of
 *     u32 opcode (LE), u32 length (LE), JSON
 * Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a command, 2 close. The command here is
 * SET_ACTIVITY. Discord rate-limits it (about 5 per 20 s), so an activity goes out only when its
 * text changes, and at most every few seconds. Activity type 0 is "Playing <application name>".
 *
 * Where the game is (NetworkModule's tick, which we wrap):
 *   NetworkModule+0x8 MainModule; MainModule+0x10+0x4bc the phase controller, +0x94 its phase.
 *   Phase 21 is PhaseGame (in the world); the title and the lobby both run in BootupManager.
 *   NetworkModule+0x240 is the LobbyClientModule, there from the title's login on: the
 *   character list. See client/docs/lobby_protocol.md.
 * Who the player is (the world connection's packets, autoinput.c's tap, from the server):
 *   0x013D SetActorName   +0 display name id, +4 the name (ASCII)
 *   0x0137 SetActorProperty  +0 byte count, then entries {u8 size 1/2/4, u32 id, value} up to the
 *                         target marker; ids are MurmurHash2 of the property path
 *   0x01A4 SetCurrentJob  +0 u32 job id (0: none, the class shows)
 * kept per source actor, and shown for my player's actor id (actor manager +0x1783c).
 *
 * XIV_DISCORD=0 turns it off; XIV_DISCORD_APP_ID picks another application; XIV_DISCORD_NAME=0
 * leaves the character's name out. The activity clears when the game exits (Discord sees the
 * socket close). */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "funcs.h"
#include "runtime.h"

#define DISCORD_APP_ID "1562613046025650267"

#define POLL_NS 2000000000ull     /* how often the game state is read */
#define MIN_SEND_NS 5000000000ull /* between activities, under Discord's rate limit */
#define RETRY_NS 15000000000ull   /* between attempts to reach Discord */

#define PHASE_GAME 21
#define PROP_MAIN_SKILL 0x7532ce24u       /* charaWork.parameterSave.state_mainSkill[0] */
#define PROP_MAIN_SKILL_LEVEL 0x96063588u /* charaWork.parameterSave.state_mainSkillLevel */

int autoinput_actor(uint32_t id, float* x, float* y, float* z, float* r, uint32_t* target); /* autoinput.c */

static int g_on, g_show_name = 1;
static char g_app_id[32];
static uint64_t g_next_poll, g_next_try, g_last_send;
static char g_sent[1024];    /* the activity Discord has (its JSON), "" if none */
static char g_pending[1024]; /* the latest one, waiting out the rate limit */
static int64_t g_start;      /* when the game started (Unix seconds) */
static unsigned g_nonce;
static int g_fd = -1;

/* from NetworkModule's tick (the game's thread) */
static GuestFn g_prev_tick;
static volatile uint32_t g_phase;
static volatile int g_lobby, g_ticked;

/* ---- who: the packets ------------------------------------------------------------------------ */

typedef struct
{
    uint32_t id;
    char name[32];
    int cls, lv, job;
} Who;

#define WHO_MAX 32
static Who g_who[WHO_MAX];
static unsigned g_who_next;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static Who* who_of(uint32_t id)
{
    for (int i = 0; i < WHO_MAX; ++i)
        if (g_who[i].id == id)
            return &g_who[i];
    Who* w = &g_who[g_who_next++ % WHO_MAX];
    memset(w, 0, sizeof *w);
    w->id = id;
    w->cls = w->lv = -1;
    return w;
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int xiv_discord_on(void) { return g_on; }

/* autoinput.c: every game message on the world connection */
void xiv_discord_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n)
{
    if (!g_on || out || !src)
        return;
    if (op == 0x013D && n > 4)
    {
        char name[32];
        uint32_t k = 0;
        for (; k < sizeof name - 1 && 4 + k < n && p[4 + k] >= 0x20 && p[4 + k] < 0x7f; ++k)
            name[k] = (char)p[4 + k];
        name[k] = '\0';
        if (!k)
            return;
        pthread_mutex_lock(&g_mu);
        snprintf(who_of(src)->name, sizeof name, "%s", name);
        pthread_mutex_unlock(&g_mu);
    }
    else if (op == 0x0137 && n > 1)
    {
        int cls = -1, lv = -1;
        for (uint32_t o = 1; o + 5 <= n;)
        {
            uint8_t sz = p[o];
            if ((sz != 1 && sz != 2 && sz != 4) || o + 5 + sz > n)
                break; /* the target marker, or a buffer */
            uint32_t id = le32(p + o + 1), v = sz == 1 ? p[o + 5] : sz == 2 ? (uint32_t)(p[o + 5] | p[o + 6] << 8) : le32(p + o + 5);
            if (id == PROP_MAIN_SKILL)
                cls = (int)v;
            else if (id == PROP_MAIN_SKILL_LEVEL)
                lv = (int)v;
            o += 5 + sz;
        }
        if (cls < 0 && lv < 0)
            return;
        pthread_mutex_lock(&g_mu);
        Who* w = who_of(src);
        if (cls >= 0)
            w->cls = cls;
        if (lv >= 0)
            w->lv = lv;
        pthread_mutex_unlock(&g_mu);
    }
    else if (op == 0x01A4 && n >= 4)
    {
        pthread_mutex_lock(&g_mu);
        who_of(src)->job = (int)le32(p);
        pthread_mutex_unlock(&g_mu);
    }
}

/* ---- where: NetworkModule's tick ------------------------------------------------------------- */

static void w_net_tick(Guest* g)
{
    uint32_t nm = g->ecx;
    (g_prev_tick ? g_prev_tick : rt_orig_net_tick)(g);
    uint32_t main_ = rd32(nm + 8), ctl = main_ ? rd32(main_ + 0x10 + 0x4bc) : 0;
    g_phase = ctl ? rd32(ctl + 0x94) : 0;
    g_lobby = rd32(nm + 0x240) != 0;
    g_ticked = 1;
}

/* ---- the socket ------------------------------------------------------------------------------ */

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
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
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

static int ipc_write(const void* p, uint32_t n)
{
    const char* c = p;
    int spins = 0;
    while (n)
    {
        ssize_t w = send(g_fd, c, n, 0);
        if (w > 0)
            c += w, n -= (uint32_t)w;
        else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++spins < 1000)
            continue; /* a few KB to a local socket: only full if Discord stopped reading */
        else
            return 0;
    }
    return 1;
}

/* Reads what Discord sent and logs its errors; 0 if the socket closed. Never blocks. */
static int ipc_drain(void)
{
    for (;;)
    {
        char buf[4096];
        ssize_t got = recv(g_fd, buf, sizeof buf - 1, 0);
        if (got > 0)
        {
            buf[got] = '\0';
            if (got > 8 && strstr(buf + 8, "\"ERROR\""))
                rt_log("[discord] %s\n", buf + 8);
            continue;
        }
        return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
}

static int ipc_send(uint32_t op, const char* json)
{
    uint32_t len = (uint32_t)strlen(json);
    unsigned char head[8] = { (unsigned char)op, (unsigned char)(op >> 8), (unsigned char)(op >> 16), (unsigned char)(op >> 24),
                              (unsigned char)len, (unsigned char)(len >> 8), (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
    return ipc_write(head, 8) && ipc_write(json, len);
}

static void disconnect(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
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
    rt_log("[discord] connected\n");
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

/* The ClassJob abbreviations (classes 2-8, 22-23, 29-41; jobs 15-19, 26-27). */
static const char* class_abbr(int id)
{
    static const char* const t[] = { [2] = "PGL", [3] = "GLA", [4] = "MRD", [7] = "ARC", [8] = "LNC", [15] = "MNK",
                                     [16] = "PLD", [17] = "WAR", [18] = "BRD", [19] = "DRG", [22] = "THM", [23] = "CNJ",
                                     [26] = "BLM", [27] = "WHM", [29] = "CRP", [30] = "BSM", [31] = "ARM", [32] = "GSM",
                                     [33] = "LTW", [34] = "WVR", [35] = "ALC", [36] = "CUL", [39] = "MIN", [40] = "BTN",
                                     [41] = "FSH" };
    return id > 0 && id < (int)(sizeof t / sizeof *t) ? t[id] : NULL;
}

/* The activity's JSON for what the game shows now. */
static void build_activity(char* out, size_t cap)
{
    char details[160];
    if (!g_ticked || g_phase != PHASE_GAME)
        snprintf(details, sizeof details, g_ticked && g_lobby ? "At character select" : "At the main menu");
    else
    {
        uint32_t me = 0;
        Who w = { 0 };
        w.cls = w.lv = -1;
        if (autoinput_actor(0xFFFFFFFFu, NULL, NULL, NULL, NULL, &me) && me)
        {
            pthread_mutex_lock(&g_mu);
            for (int i = 0; i < WHO_MAX; ++i)
                if (g_who[i].id == me)
                    w = g_who[i];
            pthread_mutex_unlock(&g_mu);
        }
        const char* job = class_abbr(w.job > 0 ? w.job : w.cls);
        char lvjob[32] = "";
        if (w.lv > 0 && job)
            snprintf(lvjob, sizeof lvjob, "Lv %d %s", w.lv, job);
        else if (job)
            snprintf(lvjob, sizeof lvjob, "%s", job);
        if (!w.name[0])
            snprintf(details, sizeof details, "Entering the world");
        else if (g_show_name)
            snprintf(details, sizeof details, lvjob[0] ? "%s %s" : "%s", w.name, lvjob);
        else
            snprintf(details, sizeof details, "%s", lvjob[0] ? lvjob : "In Eorzea");
    }
    char d[260];
    json_str(d, sizeof d, details, 128);
    snprintf(out, cap, "{\"type\":0,\"details\":\"%s\",\"timestamps\":{\"start\":%lld}}", d, (long long)g_start);
}

static int send_activity(const char* activity)
{
    char msg[1400];
    snprintf(msg, sizeof msg, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":%s},\"nonce\":\"%u\"}",
             (long)getpid(), activity, ++g_nonce);
    return ipc_send(1, msg);
}

/* ---- the hooks ------------------------------------------------------------------------------- */

void xiv_discord_init(void)
{
    const char* off = getenv("XIV_DISCORD");
    const char* id = getenv("XIV_DISCORD_APP_ID");
    const char* nm = getenv("XIV_DISCORD_NAME");
    if (off && !strcmp(off, "0"))
        return;
    snprintf(g_app_id, sizeof g_app_id, "%s", id && id[0] ? id : DISCORD_APP_ID);
    g_show_name = !(nm && !strcmp(nm, "0"));
    g_start = (int64_t)time(NULL);
    g_prev_tick = rt_wrap_net_tick; /* xiv_direct_login.c's, when it is on */
    rt_wrap_net_tick = w_net_tick;
    g_on = 1;
}

/* From the Present hook (d3d9.c), once a frame. */
void xiv_discord_frame(void)
{
    if (!g_on)
        return;
    uint64_t now = rt_monotonic_ns();
    if (now < g_next_poll)
        return;
    g_next_poll = now + POLL_NS;

    if (g_fd < 0)
    {
        if (now < g_next_try)
            return;
        g_next_try = now + RETRY_NS;
        connect_discord();
        return; /* the activity goes out once Discord has answered the handshake */
    }
    if (!ipc_drain())
    {
        rt_log("[discord] disconnected\n");
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
    rt_log("[discord] %s\n", g_pending);
    snprintf(g_sent, sizeof g_sent, "%s", g_pending);
    g_last_send = now;
}
