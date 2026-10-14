/* Dev login: title screen straight into the world, no lobby menus or character select.
 *
 * XIV_DIRECT_LOGIN=<character id> turns it on; unset, nothing here runs. The client still has the
 * developers' ticket login (NetworkModule's "net_debug_*" config, a lobby request 0x1F5 that
 * answers with the World address), but nothing calls it in retail. This supplies the call:
 *
 *   - NetworkModule's constructor: before it copies its config category, point the lobby at
 *     XIV_LOBBY_HOST:XIV_LOBBY_PORT, the ticket at the character id, the world name at
 *     XIV_WORLD_NAME and the lobby-less fallback at XIV_FRONT_HOST:XIV_FRONT_PORT (World), with few
 *     retries so a lobby that does not answer falls back to World quickly;
 *   - NetworkModule::Update: once BootupManager's title is up and idle, start the ticket login;
 *     when it has opened the World connection, post BootupManager's "enter game" command (what
 *     the game posts after character select: bootup UI teardown, then PhaseGame), or change to
 *     PhaseGame directly when the bootup objects that command touches are missing.
 *
 * Decomp notes: decomp/docs/lobby_protocol.md, "Patching the ticket login in recomp". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "runtime.h"

#define CFG_NET 4 /* SystemConfig category of the net_* keys */
#define PHASE_BOOTUP 10
#define PHASE_GAME 0x15
#define CMD_ENTER_GAME 0x39

static struct
{
    int on;
    uint32_t chara;
    int stage; /* 0 wait for the title, 1 login started, 2 game phase asked for, 3 done */
    uint64_t ready_since, stage_t;
    uint32_t last_state, last_mod_state, last_phase;
    int used_fallback;
} S = { .last_state = ~0u, .last_mod_state = ~0u, .last_phase = ~0u };

static const char* env_or(const char* name, const char* def)
{
    const char* v = getenv(name);
    return v && *v ? v : def;
}

/* thiscall into the translation; the stack below esp is free at a wrap's entry */
static uint32_t call_this(Guest* g, GuestFn fn, uint32_t self, int n, const uint32_t* args)
{
    Guest save = *g;
    g->ecx = self;
    for (int i = n - 1; i >= 0; --i)
    {
        g->esp -= 4;
        wr32(g->esp, args[i]);
    }
    g->esp -= 4;
    wr32(g->esp, 0); /* return address: the translation returns to us in C */
    fn(g);
    uint32_t r = g->eax;
    *g = save;
    return r;
}

static void cfg_int(Guest* g, uint32_t cfg, uint32_t idx, uint32_t v)
{
    call_this(g, f_00443dd0, cfg, 3, (uint32_t[]){ CFG_NET, idx, v });
}

static void cfg_str(Guest* g, uint32_t cfg, uint32_t idx, const char* v)
{
    size_t n = strlen(v);
    if (n > 0xff)
        n = 0xff;
    Guest save = *g;
    g->esp = (g->esp - 0x100 - 4) & ~3u;
    uint32_t sva = g->esp + 4;
    memcpy(GUEST_PTR(sva), v, n);
    *GUEST_PTR(sva + (uint32_t)n) = 0;
    uint32_t s = call_this(g, f_00443e70, cfg, 3, (uint32_t[]){ CFG_NET, idx, 0 });
    if (s)
        call_this(g, f_004489c0, s, 1, (uint32_t[]){ sva });
    *g = save;
    if (!s)
        rt_log("[direct-login] config %u/%u is not registered\n", CFG_NET, idx);
}

static void w_net_ctor(Guest* g)
{
    uint32_t cfg = rt_arg(g, 1);
    char ticket[16];
    snprintf(ticket, sizeof ticket, "%u", S.chara);
    const char* lobby = env_or("XIV_LOBBY_HOST", "127.0.0.1");
    uint32_t lport = (uint32_t)atoi(env_or("XIV_LOBBY_PORT", "54994"));
    const char* world = env_or("XIV_WORLD_NAME", "Hyperion");
    const char* front = env_or("XIV_FRONT_HOST", "127.0.0.1");
    uint32_t fport = (uint32_t)atoi(env_or("XIV_FRONT_PORT", "54992"));
    cfg_int(g, cfg, 2, lport);   /* net_lobby_port */
    cfg_str(g, cfg, 3, lobby);   /* net_lobby_host */
    cfg_int(g, cfg, 4, 3);       /* net_debug_lobby_retry_count */
    cfg_int(g, cfg, 5, 2);       /* net_debug_lobby_retry_interval, seconds */
    cfg_int(g, cfg, 6, fport);   /* net_debug_front_port */
    cfg_str(g, cfg, 7, front);   /* net_debug_front_host */
    cfg_str(g, cfg, 8, "TCP");   /* net_debug_front_protocol */
    cfg_str(g, cfg, 10, ticket); /* net_debug_ticket: World's hello id */
    cfg_str(g, cfg, 11, world);  /* net_debug_world */
    cfg_int(g, cfg, 12, 0);      /* zone id, name and city unset: request 0x1F5, not 0x1F6 */
    cfg_str(g, cfg, 13, "");
    cfg_int(g, cfg, 14, 0);
    rt_log("[direct-login] config set: ticket %s, lobby %s:%u, world \"%s\", fallback World %s:%u\n", ticket, lobby, lport,
           world, front, fport);
    rt_orig_net_ctor(g);
}

/* BootupManager's phase interface (the controller's current phase object) when the title runs */
static uint32_t bootup_phase(uint32_t ctl)
{
    if (rd32(ctl + 0x94) != PHASE_BOOTUP || rd32(ctl + 0x98))
        return 0;
    return rd32(ctl + 0xa0);
}

/* nothing running or queued in BootupManager's command queue */
static int bootup_idle(uint32_t bp) { return !rd32(bp + 0x44) && !rd32(bp + 0x50); }

static void enter_game(Guest* g, uint32_t ctl)
{
    uint32_t bp = bootup_phase(ctl);
    /* the enter-game command tears down title/lobby UI through these: the UI and model managers
     * (title start, loaded by its first command), the fade and the owning MainModule */
    int ok = bp && bootup_idle(bp) && rd32(bp + 0x24) && rd32(bp + 0x2c) && rd32(bp + 0x30) && rd32(bp - 0x30);
    if (ok)
    {
        uint32_t bm = bp - 0x34;
        call_this(g, f_006a9620, bm, 0, NULL); /* close the bootup loaders, as the game does first */
        call_this(g, f_006ae490, bm, 1, (uint32_t[]){ CMD_ENTER_GAME });
        rt_log("[direct-login] posted bootup command 0x%x (enter game)\n", CMD_ENTER_GAME);
    }
    else
    {
        rt_log("[direct-login] bootup not ready for its enter-game command (phase %u, cmd %u): changing to PhaseGame\n",
               rd32(ctl + 0x94), bp ? rd32(bp + 0x44) : 0);
        call_this(g, f_00565aa0, ctl, 1, (uint32_t[]){ PHASE_GAME });
        S.used_fallback = 1;
    }
}

static void w_net_tick(Guest* g)
{
    uint32_t nm = g->ecx;
    uint32_t main_ = rd32(nm + 8), ctl = main_ ? rd32(main_ + 0x4bc) : 0;
    uint64_t now = rt_monotonic_ns();
    if (S.stage == 0 && ctl)
    {
        uint32_t bp = bootup_phase(ctl);
        /* the title's first command has run (its UI managers are fetched) and nothing is queued */
        if (bp && rd32(bp + 0x24) && rd32(bp + 0x30) && bootup_idle(bp))
        {
            if (!S.ready_since)
            {
                S.ready_since = now;
                rt_log("[direct-login] title up\n");
            }
            if (now - S.ready_since > 1500000000ull)
            {
                uint32_t ok = call_this(g, f_004dfaa0, nm, 1, (uint32_t[]){ nm + 0x254 }) & 0xff;
                rt_log("[direct-login] ticket login started: %s, state %u, %s\n", ok ? "ok" : "refused", rd32(nm + 0x250),
                       rd32(nm + 0x23c) ? "via lobby" : "no lobby, straight to World");
                S.stage = ok ? 1 : 3;
                S.stage_t = now;
            }
        }
        else
            S.ready_since = 0;
    }
    rt_orig_net_tick(g);
    if (S.stage == 0 || S.stage == 3)
        return;
    uint32_t st = rd32(nm + 0x250), mod = rd32(nm + 0x23c), ms = mod ? rd32(mod + 0x394) : ~0u;
    if (st != S.last_state)
    {
        rt_log("[direct-login] network state %u -> %u\n", S.last_state, st);
        S.last_state = st;
    }
    if (ms != S.last_mod_state)
    {
        if (mod)
            rt_log("[direct-login] lobby request state %u (0 send, 1 sent, 2 answered, 3/4 closed)\n", ms);
        else if (S.last_mod_state != ~0u)
            rt_log("[direct-login] lobby done%s\n", rd32(nm + 0x2a8) ? ", falling back to the front host" : "");
        S.last_mod_state = ms;
    }
    if (S.stage == 1 && st >= 3)
    {
        if (!rd32(nm + 0x234))
        {
            rt_log("[direct-login] no World connection was opened; staying at the title\n");
            S.stage = 3;
            return;
        }
        enter_game(g, ctl);
        S.stage = 2;
        S.stage_t = now;
    }
    else if (S.stage == 2 && ctl)
    {
        uint32_t ph = rd32(ctl + 0x94);
        if (ph != S.last_phase)
        {
            rt_log("[direct-login] phase %u\n", ph);
            S.last_phase = ph;
        }
        if (ph == PHASE_GAME)
        {
            rt_log("[direct-login] in PhaseGame\n");
            S.stage = 3;
        }
        else if (!S.used_fallback && rd32(ctl + 0x98) != PHASE_GAME && now - S.stage_t > 10000000000ull)
        {
            rt_log("[direct-login] still in phase %u after 10 s: changing to PhaseGame\n", ph);
            call_this(g, f_00565aa0, ctl, 1, (uint32_t[]){ PHASE_GAME });
            S.used_fallback = 1;
        }
    }
}

void xiv_direct_login_init(void)
{
    const char* v = getenv("XIV_DIRECT_LOGIN");
    if (!v || !*v)
        return;
    char* end;
    unsigned long id = strtoul(v, &end, 10);
    if (*end || !id || id > 0xffffffffu)
    {
        rt_log("[direct-login] XIV_DIRECT_LOGIN=%s is not a character id; off\n", v);
        return;
    }
    S.on = 1;
    S.chara = (uint32_t)id;
    rt_wrap_net_ctor = w_net_ctor;
    rt_wrap_net_tick = w_net_tick;
    rt_log("[direct-login] on: character %u\n", S.chara);
}
