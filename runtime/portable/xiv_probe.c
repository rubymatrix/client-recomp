/* xivprobe: a localhost JSON endpoint into the running client for automated tests (XIV_PROBE=<port>).
 *
 * Observe: every call the client's Lua scripts make into the engine's C++ bindings (the `_name_cpp`
 * methods: log lines, widgets, talk text, menus, cutscenes, targeting; xiv_probe_binds.h), the game
 * messages on the world connection (event start/update/end, RunEventFunction, game messages, zone
 * changes), and the local player's position and target. Act: lines for the scripted-input autopilot
 * (autoinput.c), e.g. face + target + interact an actor by id. See decomp/docs/xivprobe.md.
 *
 *   GET  /state                      snapshot: player, zone, event, message, cutscene, loading, fatal
 *   GET  /events?since=N&wait=MS&type=lua,net,input,log,state&limit=N
 *   GET  /lua                        call counts per binding, and which are traced
 *   POST /trace?lua=default|all|none|+name,-name   choose the bindings that make events
 *   POST /act?op=...                 see act() below
 *
 * Off unless XIV_PROBE is set; binds 127.0.0.1 only. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "funcs.h"
#include "gwin.h"
#include "runtime.h"

/* autoinput.c */
int autoinput_actor(uint32_t id, float* x, float* y, float* z, float* r, uint32_t* target);

static int g_on;
static volatile int g_auto_advance;  /* clear every message wait as soon as it is seen */
static volatile int g_want_advance;  /* one message wait */
static volatile int g_want_skip;     /* skip the playing cutscene */
static uint64_t g_skip_deadline;
static char g_script[1024];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_t0;

static double now_ms(void) { return (double)(rt_monotonic_ns() - g_t0) / 1e6; }

/* --- text building ------------------------------------------------------------------------------ */
typedef struct
{
    char* p;
    size_t n, cap;
} Sb;

static void sb_grow(Sb* s, size_t add)
{
    if (s->n + add + 1 > s->cap)
    {
        s->cap = (s->n + add + 1) * 2 + 64;
        s->p = (char*)realloc(s->p, s->cap);
    }
}
static void sb_put(Sb* s, const char* t, size_t n)
{
    sb_grow(s, n);
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void sb_printf(Sb* s, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0)
        return;
    if ((size_t)k < sizeof tmp)
    {
        sb_put(s, tmp, (size_t)k);
        return;
    }
    sb_grow(s, (size_t)k);
    va_start(ap, fmt);
    vsnprintf(s->p + s->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    s->n += (size_t)k;
}
/* a JSON string from bytes: UTF-8 kept, control bytes and stray high bytes as \u00XX */
static void sb_jstr(Sb* s, const char* t, size_t n)
{
    sb_put(s, "\"", 1);
    const unsigned char* u = (const unsigned char*)t;
    for (size_t i = 0; i < n; ++i)
    {
        unsigned c = u[i];
        if (c == '"' || c == '\\')
            sb_printf(s, "\\%c", c);
        else if (c == '\n')
            sb_put(s, "\\n", 2);
        else if (c < 0x20 || c == 0x7f)
            sb_printf(s, "\\u%04x", c);
        else if (c < 0x80)
            sb_put(s, (const char*)&u[i], 1);
        else
        {
            int k = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
            int ok = k > 0 && i + (size_t)k < n;
            for (int j = 1; ok && j <= k; ++j)
                ok = (u[i + (size_t)j] & 0xc0) == 0x80;
            if (ok)
            {
                sb_put(s, (const char*)&u[i], (size_t)k + 1);
                i += (size_t)k;
            }
            else
                sb_printf(s, "\\u%04x", c);
        }
    }
    sb_put(s, "\"", 1);
}
static void sb_jcstr(Sb* s, const char* t) { sb_jstr(s, t, strlen(t)); }

/* --- guest memory, checked --------------------------------------------------------------------- */
static int ok_va(uint32_t va, uint32_t n)
{
    if (va < 0x10000u || va + n < va)
        return 0;
#if defined(RT_GUEST_WINDOW)
    for (uint32_t p = va & ~0xfffu; p < va + n; p += 0x1000u)
        if (!gwin_is_committed(p))
            return 0;
#endif
    return 1;
}
static int r32(uint32_t va, uint32_t* o)
{
    if (!ok_va(va, 4))
        return 0;
    memcpy(o, GUEST_PTR(va), 4);
    return 1;
}
static uint32_t r32z(uint32_t va)
{
    uint32_t v = 0;
    r32(va, &v);
    return v;
}
/* a NUL-terminated guest string that looks like text (no control bytes but tab/newline and the
 * client's text-macro introducer 0x02..0x03); its length, or -1 */
static int gstr(uint32_t va, char* out, int cap)
{
    if (!ok_va(va, 1))
        return -1;
    int n = 0;
    for (;; ++n)
    {
        if (n >= cap - 1 || ((va + (uint32_t)n) & 0xfff) == 0 ? !ok_va(va + (uint32_t)n, 1) && n < cap - 1 : 0)
            return -1;
        if (n >= cap - 1)
            break;
        unsigned char c = *GUEST_PTR(va + (uint32_t)n);
        if (!c)
            break;
        if (c < 0x20 && c != '\t' && c != '\n' && c != 2 && c != 3)
            return -1;
        out[n] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* MSVC RTTI: the class of a C++ object, "Ns::Ns::Class" (from the mangled ".?AVClass@Ns@Ns@@") */
static int rtti_name(uint32_t obj, char* out, int cap)
{
    uint32_t vt, col, td, sig;
    char m[160];
    if (!r32(obj, &vt) || vt < 0x400000u || vt > 0x1400000u || !r32(vt - 4, &col) || !r32(col, &sig) || sig ||
        !r32(col + 12, &td) || gstr(td + 8, m, sizeof m) < 5 || strncmp(m, ".?AV", 4))
        return 0;
    /* reverse the @-separated scopes */
    char* parts[16];
    int np = 0;
    for (char* p = strtok(m + 4, "@"); p && np < 16; p = strtok(NULL, "@"))
        parts[np++] = p;
    out[0] = 0;
    for (int i = np - 1; i >= 0; --i)
        snprintf(out + strlen(out), (size_t)(cap - (int)strlen(out)), "%s%s", i == np - 1 ? "" : "::", parts[i]);
    return 1;
}
static const char* short_cls(const char* full)
{
    const char* s = strrchr(full, ':');
    return s ? s + 1 : full;
}

/* --- events ------------------------------------------------------------------------------------- */
#define RING 16384
typedef struct
{
    uint64_t seq;
    char type[8];
    char* json;
} Ev;
static Ev g_ring[RING];
static uint64_t g_seq;

/* body: the fields after "seq","t","type" (no braces), e.g. "\"name\":\"x\"" */
static void ev_push(const char* type, const char* body)
{
    Sb s = { 0 };
    pthread_mutex_lock(&g_mu);
    uint64_t seq = ++g_seq;
    sb_printf(&s, "{\"seq\":%llu,\"t\":%.1f,\"type\":\"%s\"%s%s}", (unsigned long long)seq, now_ms(), type,
        body && *body ? "," : "", body ? body : "");
    Ev* e = &g_ring[seq % RING];
    free(e->json);
    e->seq = seq;
    snprintf(e->type, sizeof e->type, "%s", type);
    e->json = s.p;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* --- state -------------------------------------------------------------------------------------- */
typedef struct
{
    char text[512];
    uint32_t id; /* text id when the call carried one */
} Line;
static struct
{
    /* player (frame tick) */
    uint32_t me, target;
    float x, y, z, rot;
    int have_pos;
    /* zone (0x0005 SetMap, 0x000C music, 0x000D weather) */
    uint32_t region, place, music, weather;
    int loading;            /* zone change seen, zone-in complete (out 0x0007 -1) not yet */
    double loading_since;
    /* event (net) */
    int ev_active;
    char ev_name[48], ev_func[48], ev_class[48];
    uint32_t ev_owner, ev_trigger;
    uint8_t ev_type;
    double ev_start, ev_last_func, ev_end;
    char ev_params[512];
    char ev_last_end[48];
    char ev_kick[48];
    /* client Lua (bindings) */
    char cut_file[160];
    int cut_playing, cut_skipped, locked;
    double cut_since, cut_loaded, cut_ended;
    uint32_t cut_obj, cut_actor;
    char msg[1024];
    uint32_t msg_count;
    int msg_wait;
    double msg_wait_since;
    char widget_last[160];
    char fatal[256];
    uint64_t lua_calls;
    uint32_t desktop; /* the DesktopWidget C++ object (self of its bindings) */
} S;

static void state_json(Sb* s)
{
    pthread_mutex_lock(&g_mu);
    sb_printf(s, "{\"t\":%.1f,\"seq\":%llu,", now_ms(), (unsigned long long)g_seq);
    sb_printf(s, "\"player\":{\"actor\":\"%08X\",\"target\":\"%08X\"", S.me, S.target);
    if (S.have_pos)
        sb_printf(s, ",\"pos\":[%.2f,%.2f,%.2f],\"rot\":%.3f", S.x, S.y, S.z, S.rot);
    sb_printf(s, "},\"zone\":{\"region\":%u,\"place\":%u,\"music\":%u,\"weather\":%u,\"loading\":%s},", S.region, S.place,
        S.music, S.weather, S.loading ? "true" : "false");
    sb_printf(s, "\"event\":{\"active\":%s,\"name\":", S.ev_active ? "true" : "false");
    sb_jcstr(s, S.ev_name);
    sb_printf(s, ",\"type\":%u,\"owner\":\"%08X\",\"trigger\":\"%08X\",\"class\":", S.ev_type, S.ev_owner, S.ev_trigger);
    sb_jcstr(s, S.ev_class);
    sb_printf(s, ",\"function\":");
    sb_jcstr(s, S.ev_func);
    sb_printf(s, ",\"params\":[%s],\"started\":%.1f,\"last_function\":%.1f,\"ended\":%.1f,\"last_ended\":", S.ev_params,
        S.ev_start, S.ev_last_func, S.ev_end);
    sb_jcstr(s, S.ev_last_end);
    sb_printf(s, ",\"kick\":");
    sb_jcstr(s, S.ev_kick);
    sb_printf(s, "},\"cutscene\":{\"playing\":%s,\"file\":", S.cut_playing ? "true" : "false");
    sb_jcstr(s, S.cut_file);
    sb_printf(s, ",\"actor\":\"%08X\",\"started\":%.1f,\"ended\":%.1f,\"skipped\":%s},", S.cut_actor, S.cut_since, S.cut_ended,
        S.cut_skipped ? "true" : "false");
    sb_printf(s, "\"message\":{\"waiting\":%s,\"waiting_since\":%.1f,\"count\":%u,\"last\":%s},", S.msg_wait ? "true" : "false",
        S.msg_wait_since, S.msg_count, S.msg[0] ? S.msg : "null");
    sb_printf(s, "\"player_control_locked\":%s,\"auto_advance\":%s,\"widget_last\":", S.locked ? "true" : "false",
        g_auto_advance ? "true" : "false");
    sb_jcstr(s, S.widget_last);
    if (S.desktop)
    {
        sb_printf(s, ",\"desktop\":{\"obj\":\"%08X\",\"b70\":\"", S.desktop);
        for (uint32_t k = 0x70; k < 0x80; ++k)
            sb_printf(s, "%02x", ok_va(S.desktop + k, 1) ? *GUEST_PTR(S.desktop + k) : 0);
        sb_put(s, "\"}", 2);
    }
    sb_printf(s, ",\"lua_calls\":%llu,\"fatal\":", (unsigned long long)S.lua_calls);
    if (S.fatal[0])
        sb_jcstr(s, S.fatal);
    else
        sb_put(s, "null", 4);
    sb_put(s, "}", 1);
    pthread_mutex_unlock(&g_mu);
}

/* --- Lua bindings -------------------------------------------------------------------------------
 * Each binding's adaptor is called as f(ctx) (cdecl/thiscall, ctx = first stack argument). The Lua
 * arguments: v = [[ctx+0x10]+4]; v+4 .. v+8 is a vector of 16-byte values, the type tag a byte at
 * +0xc and the payload at +8: 0 integer, 3 string (payload -> the engine string), 4 object
 * (payload -> the C++ object). Established from the adaptors and value getters (FUN_005819a0,
 * FUN_0071ce70, FUN_0071bb80); see decomp/docs/xivprobe.md. */
enum
{
#define LB(a, c, n) LBI_##a,
#include "xiv_probe_binds.h"
#undef LB
    LB_COUNT
};
static const struct
{
    uint32_t va;
    const char *cls, *name;
} LBT[LB_COUNT] = {
#define LB(a, c, n) { 0x##a, c, n },
#include "xiv_probe_binds.h"
#undef LB
};
static uint8_t g_trace[LB_COUNT];
static uint8_t g_model[LB_COUNT]; /* feeds /state even when not traced */
static uint32_t g_count[LB_COUNT];
static uint32_t g_burst[LB_COUNT]; /* events this second */
static uint64_t g_burst_sec[LB_COUNT];
static int g_raw; /* XIV_PROBE_RAW=1: raw words of each value and of ctx */

static const char* const DEFAULT_TRACE[] = {
    /* log, messages, text */
    "_appendLogPool", "_appendMessagePool", "_clearLogPool", "_printLog", "_printText", "_printWarning", "_printDebugLog", "_chat",
    "_parseTextCommand",
    /* widgets */
    "_createWidgetInWidgetContainer", "_deleteCreatingWidgetInWidgetContainer", "_reserveWidgetContainer", "_loadForm",
    "_setTextProperty", "_setListTextProperty", "_addList", "_addItem", "_clearAllList", "_removeList", "_removeItem", "_updateList",
    "_setKeyboardFocusedWidget", "_setKeyboardFocusedControl", "_sendStoryboardCommand", "_setUICommandCondition",
    "_setUICommandTemplateCondition",
    /* cutscenes, fades, loading */
    "_loadCutScene", "_play", "_replay", "_skip", "_fadeIn", "_fadeOut", "_fadeInAfterWarp",
    "_fadeInNowLoadingForNoticeEventJustInArea", "_waitForMapLoaded", "_cancelFading", "_resetFade",
    /* talk / push / notice / emote / command */
    "_executeTalk", "_executeEmote", "_executeCommand", "_callServerOnTalk", "_callServerOnPush", "_callServerOnEmote",
    "_callServerOnCommand", "_doServerOnTalk", "_doServerOnPush", "_doServerOnEmote", "_doServerOnCommand", "_cancelTalk",
    "_breakTalk", "_breakPush", "_breakNotice", "_breakEmote", "_breakCommand", "_cancelPush", "_cancelNotice", "_cancelEmote",
    "_cancelCommand", "_lockPlayerControl", "_unlockPlayerControl", "_setTargetCharacter", "_setTargetNearestCharacter",
    "_setTargetCharacterByDisplayName", "_setMusic", "_setWeather", "_turn", "_achieveTrophy",
    /* actors and classes (widget ids -> Lua class) */
    "_createActor", "_delete", "_getText",
};

static const char* const MODEL[] = { "_createActor", "_play", "_replay", "_skip", "_delete", "_appendMessagePool",
    "_appendLogPool", "_createWidgetInWidgetContainer", "_loadForm", "_lockPlayerControl", "_unlockPlayerControl" };

static void trace_default(void)
{
    memset(g_trace, 0, sizeof g_trace);
    for (int i = 0; i < LB_COUNT; ++i)
        for (size_t k = 0; k < sizeof DEFAULT_TRACE / sizeof DEFAULT_TRACE[0]; ++k)
            if (!strcmp(LBT[i].name, DEFAULT_TRACE[k]))
                g_trace[i] = 1;
}

/* C++ object -> actor id, learned from calls whose `self` is that object (ctx+0xc holds self's id) */
#define OBJMAP 8192
static uint32_t g_obj_key[OBJMAP], g_obj_id[OBJMAP];
static void obj_learn(uint32_t obj, uint32_t id)
{
    if (!obj || !id)
        return;
    uint32_t h = (obj >> 4) * 2654435761u % OBJMAP;
    for (int k = 0; k < 16; ++k, h = (h + 1) % OBJMAP)
        if (g_obj_key[h] == obj || !g_obj_key[h])
        {
            g_obj_id[h] = id;
            g_obj_key[h] = obj;
            return;
        }
    g_obj_key[h] = obj, g_obj_id[h] = id; /* full run: overwrite */
}
static uint32_t obj_id(uint32_t obj)
{
    uint32_t h = (obj >> 4) * 2654435761u % OBJMAP;
    for (int k = 0; k < 16; ++k, h = (h + 1) % OBJMAP)
        if (g_obj_key[h] == obj)
            return g_obj_id[h];
        else if (!g_obj_key[h])
            return 0;
    return 0;
}

static void put_obj(Sb* s, uint32_t obj)
{
    char cls[160];
    sb_printf(s, "{\"obj\":\"%08X\"", obj);
    if (rtti_name(obj, cls, sizeof cls))
    {
        sb_put(s, ",\"cls\":", 7);
        sb_jcstr(s, short_cls(cls));
    }
    uint32_t id = obj_id(obj);
    if (id)
        sb_printf(s, ",\"id\":\"%08X\"", id);
    sb_put(s, "}", 1);
}

/* The engine string {char* p, cap, len+1, ...} (p points into the object for short strings) */
static int engine_str(uint32_t obj, char* buf, int cap)
{
    uint32_t p = 0;
    if (!r32(obj, &p))
        return -1;
    return gstr(p, buf, cap);
}

/* One Lua value as JSON: 16 bytes, payload in word 0, type in the low byte of word 3:
 * 0 integer, 3 string (-> engine string), 4 C++ object, 6 nil. *str gets a string value. */
static void put_val(Sb* s, uint32_t v, char* str, int strcap)
{
    uint32_t w[4] = { 0 };
    for (int i = 0; i < 4; ++i)
        r32(v + 4u * (uint32_t)i, &w[i]);
    unsigned tag = w[3] & 0xff;
    char buf[1024];
    if (str)
        str[0] = 0;
    if (g_raw)
        sb_put(s, "{\"v\":", 5);
    int n;
    if (tag == 0)
        sb_printf(s, "%d", (int32_t)w[0]);
    else if (tag == 3 && (n = engine_str(w[0], buf, sizeof buf)) >= 0)
    {
        sb_jstr(s, buf, (size_t)n);
        if (str)
            snprintf(str, (size_t)strcap, "%s", buf);
    }
    else if (tag == 4 && w[0])
        put_obj(s, w[0]);
    else if (tag == 6)
        sb_put(s, "null", 4);
    else
        sb_printf(s, "{\"tag\":%u,\"w0\":\"%08X\"}", tag, w[0]);
    if (g_raw)
        sb_printf(s, ",\"raw\":\"%08x %08x %08x %08x\"}", w[0], w[1], w[2], w[3]);
}

/* --- the client's own fast paths, run on a guest thread (from a binding wrapper) --------------
 * advance: what the message window's accept does (FUN_00575900 -> FUN_006dca80): DesktopWidget+0x79 = 1,
 *   which the AppendMessageResumeChecker (check FUN_00713a20) waits for after _appendMessagePool.
 * skip: what the CutScene _skip binding does (FUN_006e1970): CutScene+0xc1 = 1, then the cutscene
 *   player's stop with mode 3 (FUN_00563c40) on [[0x01356e10]+0x4c8], while it plays (+0x98 == 1). */
#define ACTOR_MGR 0x01356e10u

static uint32_t cut_player(void)
{
    uint32_t mgr = 0, pl = 0;
    if (!r32(ACTOR_MGR, &mgr) || !r32(mgr + 0x4c8, &pl))
        return 0;
    return pl;
}

static void guest_work(Guest* g)
{
    if (S.desktop && (g_auto_advance || g_want_advance) && ok_va(S.desktop + 0x79, 1) && !*GUEST_PTR(S.desktop + 0x79))
    {
        *GUEST_PTR(S.desktop + 0x79) = 1;
        if (g_want_advance)
            g_want_advance = 0;
        ev_push("state", "\"what\":\"message_advanced\"");
    }
    if (g_want_skip)
    {
        uint32_t pl = cut_player(), st = 0;
        if (pl && r32(pl + 0x98, &st) && st == 1)
        {
            g_want_skip = 0;
            if (S.cut_obj && ok_va(S.cut_obj + 0xc1, 1))
                *GUEST_PTR(S.cut_obj + 0xc1) = 1;
            S.cut_skipped = 1;
            Guest save = *g;
            g->ecx = pl;
            g->esp -= 4;
            wr32(g->esp, 3);
            g->esp -= 4;
            wr32(g->esp, 0); /* return address: the translation returns to us in C */
            f_00563c40(g);
            *g = save;
            char b[96];
            snprintf(b, sizeof b, "\"what\":\"cutscene_skipped\",\"player\":\"%08X\"", pl);
            ev_push("state", b);
        }
        else if (rt_monotonic_ns() > g_skip_deadline)
        {
            g_want_skip = 0;
            ev_push("state", "\"what\":\"cutscene_skip_expired\"");
        }
    }
}

/* The state model fed by bindings */
typedef struct
{
    unsigned tag;
    uint32_t w0;
} Arg;
static void arg_json(Sb* s, const Arg* a, const char* str)
{
    if (a->tag == 0)
        sb_printf(s, "%d", (int32_t)a->w0);
    else if (a->tag == 3)
        sb_jcstr(s, str);
    else if (a->tag == 4)
    {
        uint32_t id = obj_id(a->w0);
        if (id)
            sb_printf(s, "{\"actor\":\"%08X\"}", id);
        else
            sb_printf(s, "{\"obj\":\"%08X\"}", a->w0);
    }
    else if (a->tag == 2)
        sb_put(s, a->w0 ? "true" : "false", a->w0 ? 4 : 5);
    else
        sb_put(s, "null", 4);
}

static void model_lua(int i, uint32_t self, uint32_t self_id, int argc, char args[][256], const Arg* av)
{
    const char* n = LBT[i].name;
    char ev[1024] = "";
    pthread_mutex_lock(&g_mu);
    S.lua_calls++;
    if (!strcmp(LBT[i].cls, "DesktopWidget") && self && self != S.desktop)
        S.desktop = self;
    if (!strcmp(n, "_createActor") && argc >= 4 && av[1].tag == 3 && !strcmp(args[1], "CutScene"))
    {
        snprintf(S.cut_file, sizeof S.cut_file, "%s", args[3]);
        S.cut_loaded = now_ms();
    }
    else if (!strcmp(LBT[i].cls, "Sequence") && (!strcmp(n, "_play") || !strcmp(n, "_replay")))
    {
        S.cut_playing = 1, S.cut_since = now_ms(), S.cut_obj = self, S.cut_actor = self_id, S.cut_skipped = 0;
        snprintf(ev, sizeof ev, "\"what\":\"cutscene_play\",\"file\":\"%s\",\"actor\":\"%08X\"", S.cut_file, self_id);
    }
    else if (!strcmp(LBT[i].cls, "Sequence") && !strcmp(n, "_skip"))
        S.cut_skipped = 1;
    else if (!strcmp(n, "_delete") && self_id && self_id == S.cut_actor && S.cut_playing)
    {
        S.cut_playing = 0, S.cut_ended = now_ms(), S.cut_obj = 0;
        snprintf(ev, sizeof ev, "\"what\":\"cutscene_end\",\"file\":\"%s\",\"secs\":%.1f", S.cut_file, (S.cut_ended - S.cut_since) / 1000.0);
    }
    else if (!strcmp(n, "_appendMessagePool"))
    {
        /* (speaker, kind, text owner, text id, params...): speaker an actor or a display-name id */
        Sb s = { 0 };
        sb_put(&s, "{\"speaker\":", 11);
        if (argc > 0)
            arg_json(&s, &av[0], args[0]);
        else
            sb_put(&s, "null", 4);
        sb_printf(&s, ",\"kind\":%d,\"owner\":", argc > 1 ? (int32_t)av[1].w0 : 0);
        if (argc > 2)
            arg_json(&s, &av[2], args[2]);
        else
            sb_put(&s, "null", 4);
        sb_printf(&s, ",\"text_id\":%d,\"params\":[", argc > 3 ? (int32_t)av[3].w0 : 0);
        for (int k = 4; k < argc; ++k)
        {
            if (k > 4)
                sb_put(&s, ",", 1);
            arg_json(&s, &av[k], args[k]);
        }
        sb_printf(&s, "],\"t\":%.1f}", now_ms());
        snprintf(S.msg, sizeof S.msg, "%s", s.p);
        S.msg_count++;
        snprintf(ev, sizeof ev, "\"what\":\"message\",\"message\":%s", s.p);
        free(s.p);
    }
    else if (!strcmp(n, "_appendLogPool"))
    {
        Sb s = { 0 };
        sb_put(&s, "\"what\":\"log\",\"args\":[", 21);
        for (int k = 0; k < argc; ++k)
        {
            if (k)
                sb_put(&s, ",", 1);
            arg_json(&s, &av[k], args[k]);
        }
        sb_put(&s, "]", 1);
        snprintf(ev, sizeof ev, "%s", s.p);
        free(s.p);
    }
    else if (!strcmp(n, "_createWidgetInWidgetContainer") || !strcmp(n, "_loadForm"))
    {
        for (int k = 0; k < argc; ++k)
            if (args[k][0])
            {
                snprintf(S.widget_last, sizeof S.widget_last, "%s", args[k]);
                break;
            }
    }
    else if (!strcmp(n, "_lockPlayerControl"))
        S.locked = 1;
    else if (!strcmp(n, "_unlockPlayerControl"))
        S.locked = 0;
    pthread_mutex_unlock(&g_mu);
    if (ev[0])
        ev_push("state", ev);
}

static void lb_hit(Guest* g, int i)
{
    __atomic_fetch_add(&g_count[i], 1, __ATOMIC_RELAXED);
    if (g_auto_advance || g_want_advance || g_want_skip)
        guest_work(g);
    {
        uint32_t c0 = rt_arg(g, 0), self_id = 0;
        if (g->ecx > 0x10000u && r32(c0 + 0xc, &self_id) && self_id && obj_id(g->ecx) != self_id)
            obj_learn(g->ecx, self_id);
    }
    if (!g_trace[i] && !g_model[i])
        return;
    uint64_t sec = rt_monotonic_ns() / 1000000000ull;
    if (g_burst_sec[i] != sec)
        g_burst_sec[i] = sec, g_burst[i] = 0;
    int quiet = ++g_burst[i] > 40; /* a getter in a loop: the counts say how often */
    if (quiet && !g_model[i])
        return;
    uint32_t ctx = rt_arg(g, 0), v = 0, vec = 0, b = 0, e = 0;
    if (r32(ctx + 0x10, &v) && r32(v + 4, &vec))
        r32(vec + 4, &b), r32(vec + 8, &e);
    int argc = b && e > b && e - b <= 16u * 32 && !((e - b) & 15) ? (int)((e - b) / 16) : 0;
    Sb s = { 0 };
    sb_printf(&s, "\"cls\":\"%s\",\"name\":\"%s\",\"self\":", LBT[i].cls, LBT[i].name);
    char cls[160];
    if (g->ecx > 0x10000u && rtti_name(g->ecx, cls, sizeof cls))
        put_obj(&s, g->ecx);
    else
        sb_put(&s, "null", 4);
    sb_put(&s, ",\"args\":[", 9);
    static __thread char args[12][256];
    static __thread Arg av[12];
    int na = argc < 12 ? argc : 12;
    for (int k = 0; k < na; ++k)
    {
        if (k)
            sb_put(&s, ",", 1);
        uint32_t va = b + 16u * (uint32_t)k, w3 = 0;
        av[k].w0 = r32z(va);
        r32(va + 12, &w3);
        av[k].tag = w3 & 0xff;
        put_val(&s, va, args[k], sizeof args[k]);
    }
    sb_put(&s, "]", 1);
    if (g_raw)
    {
        sb_printf(&s, ",\"ctx\":\"%08X\",\"ctxw\":[", ctx);
        for (int k = 0; k < 8; ++k)
        {
            uint32_t w = r32z(ctx + 4u * (uint32_t)k);
            if (k)
                sb_put(&s, ",", 1);
            if (rtti_name(w, cls, sizeof cls))
            {
                sb_printf(&s, "\"%08X ", w);
                sb_put(&s, short_cls(cls), strlen(short_cls(cls)));
                sb_put(&s, "\"", 1);
            }
            else
                sb_printf(&s, "\"%08X\"", w);
        }
        sb_put(&s, "]", 1);
    }
    model_lua(i, g->ecx, r32z(ctx + 0xc), na, args, av);
    if (g_trace[i] && !quiet)
        ev_push("lua", s.p);
    free(s.p);
}

#define LB(a, c, n) \
    static void w_##a(Guest* g) \
    { \
        lb_hit(g, LBI_##a); \
        rt_orig_lb_##a(g); \
    }
#include "xiv_probe_binds.h"
#undef LB

/* --- network (autoinput.c's world-connection tap) ------------------------------------------------ */
static int pstr(const uint8_t* p, uint32_t n, uint32_t off, uint32_t max, char* out, int cap)
{
    int k = 0;
    for (uint32_t i = off; i < n && i < off + max && p[i] && k < cap - 1; ++i)
        out[k++] = (char)p[i];
    out[k] = 0;
    return k;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

/* Meteor's LuaParam encoding (Map Server/Lua/LuaUtils.cs): tag byte, big-endian payloads, 0x0F ends */
static void lua_params(Sb* s, const uint8_t* p, uint32_t n, uint32_t off)
{
    int first = 1;
    for (uint32_t i = off; i < n;)
    {
        uint8_t t = p[i++];
        if (t == 0x0F)
            break;
        if (!first)
            sb_put(s, ",", 1);
        first = 0;
        if ((t == 0 || t == 1 || t == 6) && i + 4 <= n)
        {
            uint32_t v = be32(p + i);
            i += 4;
            if (t == 6)
                sb_printf(s, "{\"actor\":\"%08X\"}", v);
            else
                sb_printf(s, "%d", (int32_t)v);
        }
        else if (t == 2)
        {
            char str[256];
            int k = pstr(p, n, i, 255, str, sizeof str);
            i += (uint32_t)k + 1;
            sb_jstr(s, str, (size_t)k);
        }
        else if (t == 3 || t == 4)
            sb_put(s, t == 3 ? "true" : "false", t == 3 ? 4 : 5);
        else if (t == 5)
            sb_put(s, "null", 4);
        else if (t == 7 && i + 7 <= n)
        {
            sb_printf(s, "{\"itemref\":\"%08X\",\"slot\":%u,\"pkg\":%u}", be32(p + i), (unsigned)p[i + 4] << 8 | p[i + 5], p[i + 6]);
            i += 7;
        }
        else if (t == 9 && i + 16 <= n)
        {
            sb_printf(s, "{\"t9\":[\"%08X%08X\",\"%08X%08X\"]}", be32(p + i), be32(p + i + 4), be32(p + i + 8), be32(p + i + 12));
            i += 16;
        }
        else if (t == 0xA && i + 9 <= n)
        {
            sb_printf(s, "{\"item\":%u,\"qty\":%u,\"hq\":%u}", be32(p + i), be32(p + i + 4), p[i + 8]);
            i += 9;
        }
        else if (t == 0xC && i + 1 <= n)
            sb_printf(s, "%u", p[i++]);
        else if (t == 0x1B || t == 8)
            sb_printf(s, "{\"tag\":%u}", t);
        else
        {
            sb_printf(s, "{\"tag\":%u,\"stop\":true}", t);
            break;
        }
    }
}

void xiv_probe_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n)
{
    if (!g_on)
        return;
    Sb s = { 0 };
    char a[64], b[64];
    int emit = 1;
    sb_printf(&s, "\"dir\":\"%s\",\"op\":\"%04X\",\"src\":\"%08X\"", out ? "out" : "in", op, src);
    pthread_mutex_lock(&g_mu);
    if (!out && op == 0x0130 && n >= 0x49) /* RunEventFunction */
    {
        pstr(p, n, 9, 0x20, a, sizeof a);
        pstr(p, n, 0x29, 0x20, b, sizeof b);
        Sb ps = { 0 };
        lua_params(&ps, p, n, 0x49);
        sb_printf(&s, ",\"name\":\"RunEventFunction\",\"trigger\":\"%08X\",\"owner\":\"%08X\",\"etype\":%u,\"event\":", le32(p),
            le32(p + 4), p[8]);
        sb_jcstr(&s, a);
        sb_put(&s, ",\"function\":", 12);
        sb_jcstr(&s, b);
        sb_printf(&s, ",\"params\":[%s]", ps.p ? ps.p : "");
        S.ev_active = 1;
        snprintf(S.ev_class, sizeof S.ev_class, "%s", a);
        snprintf(S.ev_func, sizeof S.ev_func, "%s", b);
        snprintf(S.ev_params, sizeof S.ev_params, "%s", ps.p ? ps.p : "");
        S.ev_last_func = now_ms();
        free(ps.p);
    }
    else if (!out && op == 0x0131 && n >= 9) /* EndEvent */
    {
        pstr(p, n, 9, 0x20, a, sizeof a);
        sb_printf(&s, ",\"name\":\"EndEvent\",\"player\":\"%08X\",\"etype\":%u,\"event\":", le32(p), p[8]);
        sb_jcstr(&s, a);
        S.ev_active = 0;
        S.ev_end = now_ms();
        if (a[0])
            snprintf(S.ev_last_end, sizeof S.ev_last_end, "%s", a);
    }
    else if (!out && op == 0x012F && n >= 0x10) /* KickEvent */
    {
        pstr(p, n, 0x10, 0x20, a, sizeof a);
        Sb ps = { 0 };
        lua_params(&ps, p, n, 0x30);
        sb_printf(&s, ",\"name\":\"KickEvent\",\"trigger\":\"%08X\",\"owner\":\"%08X\",\"etype\":%u,\"event\":", le32(p),
            le32(p + 4), p[8]);
        sb_jcstr(&s, a);
        sb_printf(&s, ",\"params\":[%s]", ps.p ? ps.p : "");
        snprintf(S.ev_kick, sizeof S.ev_kick, "%s", a);
        free(ps.p);
    }
    else if (out && op == 0x012D && n >= 0x11) /* EventStart */
    {
        pstr(p, n, 0x11, 0x20, a, sizeof a);
        Sb ps = { 0 };
        lua_params(&ps, p, n, 0x31);
        sb_printf(&s, ",\"name\":\"EventStart\",\"trigger\":\"%08X\",\"owner\":\"%08X\",\"etype\":%u,\"event\":", le32(p),
            le32(p + 4), p[0x10]);
        sb_jcstr(&s, a);
        sb_printf(&s, ",\"params\":[%s]", ps.p ? ps.p : "");
        S.ev_active = 1;
        snprintf(S.ev_name, sizeof S.ev_name, "%s", a);
        S.ev_owner = le32(p + 4), S.ev_trigger = le32(p), S.ev_type = p[0x10];
        S.ev_start = now_ms();
        S.ev_class[0] = S.ev_func[0] = S.ev_params[0] = 0;
        free(ps.p);
    }
    else if (out && op == 0x012E && n >= 0x11) /* EventUpdate: the player's answer */
    {
        Sb ps = { 0 };
        lua_params(&ps, p, n, 0x11);
        sb_printf(&s, ",\"name\":\"EventUpdate\",\"trigger\":\"%08X\",\"etype\":%u,\"params\":[%s]", le32(p), p[0x10],
            ps.p ? ps.p : "");
        free(ps.p);
    }
    else if (!out && op >= 0x0157 && op <= 0x016A) /* GameMessage */
    {
        uint32_t textId = 0, owner = 0, actor = 0, log = 0, poff = 0;
        a[0] = 0;
        if (op <= 0x015B && n >= 12)
            actor = le32(p), owner = le32(p + 4), textId = (uint32_t)(p[8] | p[9] << 8), log = (uint32_t)(p[10] | p[11] << 8), poff = 12;
        else if (op <= 0x0160 && n >= 8)
        {
            owner = le32(p), textId = (uint32_t)(p[4] | p[5] << 8), log = (uint32_t)(p[6] | p[7] << 8), poff = 0x28;
            pstr(p, n, 8, 0x20, a, sizeof a);
        }
        else if (op <= 0x0165 && n >= 12)
            actor = le32(p), owner = le32(p + 4), textId = (uint32_t)(p[8] | p[9] << 8), log = (uint32_t)(p[10] | p[11] << 8), poff = 12;
        else if (n >= 8)
            owner = le32(p), textId = (uint32_t)(p[4] | p[5] << 8), log = (uint32_t)(p[6] | p[7] << 8), poff = 8;
        sb_printf(&s, ",\"name\":\"GameMessage\",\"text_id\":%u,\"owner\":\"%08X\",\"actor\":\"%08X\",\"log\":%u", textId, owner,
            actor, log);
        if (a[0])
        {
            sb_put(&s, ",\"sender\":", 10);
            sb_jcstr(&s, a);
        }
        if (poff && poff < n && op != 0x0157 && op != 0x015C && op != 0x0161 && op != 0x0166)
        {
            Sb ps = { 0 };
            lua_params(&ps, p, n, poff);
            sb_printf(&s, ",\"params\":[%s]", ps.p ? ps.p : "");
            free(ps.p);
        }
    }
    else if (!out && op == 0x0003 && n >= 0x24) /* SendMessage (chat / system text) */
    {
        char msg[0x201];
        pstr(p, n, 0, 0x20, a, sizeof a);
        int k = pstr(p, n, 0x24, 0x200, msg, sizeof msg);
        sb_printf(&s, ",\"name\":\"SendMessage\",\"kind\":%u,\"sender\":", le32(p + 0x20));
        sb_jcstr(&s, a);
        sb_put(&s, ",\"text\":", 8);
        sb_jstr(&s, msg, (size_t)k);
    }
    else if (!out && op == 0x0005 && n >= 8) /* SetMap */
    {
        S.region = le32(p), S.place = le32(p + 4);
        S.loading = 1, S.loading_since = now_ms();
        sb_printf(&s, ",\"name\":\"SetMap\",\"region\":%u,\"place\":%u", S.region, S.place);
    }
    else if (!out && op == 0x000C && n >= 4)
        S.music = le32(p) & 0xffff, emit = 0;
    else if (!out && op == 0x000D && n >= 4)
        S.weather = le32(p) & 0xffff, emit = 0;
    else if (!out && op == 0x0007)
        sb_printf(&s, ",\"name\":\"DeleteAllActors\"");
    else if (!out && op == 0x00E2)
        sb_printf(&s, ",\"name\":\"0x00E2\"");
    else if (!out && op == 0x017B && n >= 1)
        sb_printf(&s, ",\"name\":\"SetActorIsZoning\",\"on\":%u", p[0]);
    else if (out && op == 0x0007 && n >= 8) /* zone-in complete when +4 = -1 */
    {
        int32_t code = (int32_t)le32(p + 4);
        sb_printf(&s, ",\"name\":\"ZoneInComplete\",\"code\":%d", code);
        if (code == -1)
            S.loading = 0;
    }
    else if (out && op == 0x00CD && n >= 4)
        sb_printf(&s, ",\"name\":\"SetTarget\",\"target\":\"%08X\"", le32(p));
    else if (out && op == 0x00CE)
        sb_printf(&s, ",\"name\":\"LockTarget\"");
    else
        emit = 0;
    pthread_mutex_unlock(&g_mu);
    if (emit)
        ev_push("net", s.p);
    free(s.p);
}

/* --- rt_log lines (autoinput results, FATAL) ---------------------------------------------------- */
static void on_log(const char* line)
{
    if (!g_on)
        return;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    while (n && *line == '\n')
        line++, n--;
    if (!n)
        return;
    const char* type = !strncmp(line, "[input]", 7) ? "input" : strstr(line, "FATAL") ? "fatal" : NULL;
    if (!type)
        return;
    if (*type == 'f')
    {
        pthread_mutex_lock(&g_mu);
        snprintf(S.fatal, sizeof S.fatal, "%.*s", (int)n, line);
        pthread_mutex_unlock(&g_mu);
    }
    Sb s = { 0 };
    sb_put(&s, "\"line\":", 7);
    sb_jstr(&s, line, n);
    ev_push(type, s.p);
    free(s.p);
}

/* --- frame tick (present thread) ---------------------------------------------------------------- */
void xiv_probe_frame(void)
{
    if (!g_on)
        return;
    /* a message waits for the player while DesktopWidget+0x79 is 0 (after _appendMessagePool) */
    if (S.desktop && S.msg_count && ok_va(S.desktop + 0x79, 1))
    {
        int w = !*GUEST_PTR(S.desktop + 0x79);
        if (w != S.msg_wait)
        {
            pthread_mutex_lock(&g_mu);
            S.msg_wait = w;
            if (w)
                S.msg_wait_since = now_ms();
            pthread_mutex_unlock(&g_mu);
            ev_push("state", w ? "\"what\":\"message_wait\"" : "\"what\":\"message_done\"");
        }
    }
    float x, y, z, r;
    uint32_t t = 0;
    int have = autoinput_actor(0, &x, &y, &z, &r, &t);
    pthread_mutex_lock(&g_mu);
    if (have)
    {
        uint32_t old = S.target;
        S.x = x, S.y = y, S.z = z, S.rot = r, S.have_pos = 1, S.target = t;
        uint32_t me = 0;
        autoinput_actor(0xFFFFFFFFu, NULL, NULL, NULL, NULL, &me);
        S.me = me;
        pthread_mutex_unlock(&g_mu);
        if (old != t)
        {
            char b[64];
            snprintf(b, sizeof b, "\"what\":\"target\",\"target\":\"%08X\"", t);
            ev_push("state", b);
        }
        return;
    }
    S.have_pos = 0;
    pthread_mutex_unlock(&g_mu);
}

/* --- acting: lines for the autopilot ------------------------------------------------------------- */
/* line: an autopilot command, run now; "+MS cmd" runs it MS after the previous line */
static int script_line(const char* line)
{
    FILE* f = fopen(g_script, "a");
    if (!f)
        return 0;
    fprintf(f, "%s%s\n", line[0] == '+' ? "" : "+0 ", line);
    fclose(f);
    Sb s = { 0 };
    sb_put(&s, "\"line\":", 7);
    sb_jcstr(&s, line);
    ev_push("act", s.p);
    free(s.p);
    return 1;
}

static const char* qarg(const char* q, const char* key, char* out, int cap)
{
    size_t kl = strlen(key);
    for (const char* p = q; p && *p;)
    {
        if (!strncmp(p, key, kl) && p[kl] == '=')
        {
            const char* v = p + kl + 1;
            int k = 0;
            for (; *v && *v != '&' && k < cap - 1; ++v)
            {
                if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2]))
                {
                    char h[3] = { v[1], v[2], 0 };
                    out[k++] = (char)strtol(h, NULL, 16);
                    v += 2;
                }
                else
                    out[k++] = *v == '+' ? ' ' : *v;
            }
            out[k] = 0;
            return out;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return NULL;
}

/* POST /act?op=...
 *   key&key=Return[&hold=ms]   one key press (SDL key name or VK code)
 *   advance                    accept the waiting message line (as the message window does), else Return
 *   auto_advance&on=1|0        accept every message line as soon as it waits
 *   cancel                     Escape
 *   choose&index=N             menu item N, 1-based (Down N-1 times, Return)
 *   face&actor=HEX | &x=&z=    turn toward an actor (its position from the client) or a point
 *   target&actor=HEX           face it, then Tab until the client targets it
 *   interact&actor=HEX         face, target, then Return and wait for the client's EventStart
 *   untarget                   Escape until the client sends "no target"
 *   walkto&x=&z=[&tol=]        walk there (autopilot walkto)
 *   skip_cutscene[&wait=ms]    stop the playing cutscene as the CutScene _skip binding does
 *   input&line=...             any autopilot line (recomp/docs/scripted-input.md) */
static void act(const char* q, Sb* out)
{
    char op[32] = "", v[256], v2[64], line[400];
    qarg(q, "op", op, sizeof op);
    int ok = 1;
    const char* err = NULL;
    if (!strcmp(op, "key"))
    {
        if (!qarg(q, "key", v, sizeof v))
            err = "key=NAME";
        else
        {
            snprintf(line, sizeof line, "key %s%s%s", v, qarg(q, "hold", v2, sizeof v2) ? " " : "", qarg(q, "hold", v2, sizeof v2) ? v2 : "");
            ok = script_line(line);
        }
    }
    else if (!strcmp(op, "advance"))
    {
        /* a message waiting: accept it the way the message window does; otherwise press Return */
        if (S.msg_wait)
            g_want_advance = 1, sb_put(out, "\"how\":\"message\",", 18);
        else
            ok = script_line("key Return"), sb_put(out, "\"how\":\"key\",", 14);
    }
    else if (!strcmp(op, "auto_advance"))
    {
        g_auto_advance = qarg(q, "on", v, sizeof v) ? atoi(v) != 0 : 1;
        sb_printf(out, "\"auto_advance\":%s,", g_auto_advance ? "true" : "false");
    }
    else if (!strcmp(op, "cancel"))
        ok = script_line("key Escape");
    else if (!strcmp(op, "choose"))
    {
        if (!qarg(q, "index", v, sizeof v) || atoi(v) < 1)
            err = "index=N (1-based)";
        else
        {
            snprintf(line, sizeof line, "choose %d", atoi(v));
            ok = script_line(line);
        }
    }
    else if (!strcmp(op, "untarget"))
        ok = script_line("untarget");
    else if (!strcmp(op, "face") || !strcmp(op, "target") || !strcmp(op, "interact"))
    {
        float x, y, z, r, mx, my, mz, mr;
        uint32_t t;
        int have_actor = qarg(q, "actor", v, sizeof v) != NULL;
        uint32_t id = have_actor ? (uint32_t)strtoul(v, NULL, 16) : 0;
        if (have_actor && !id)
            err = "actor=HEX";
        else if (have_actor && autoinput_actor(id, &x, &y, &z, &r, &t))
        {
            if (autoinput_actor(0, &mx, &my, &mz, &mr, &t) && (fabsf(mx - x) > 0.05f || fabsf(mz - z) > 0.05f))
            {
                snprintf(line, sizeof line, "face %.2f %.2f", x, z);
                ok = script_line(line);
            }
            sb_printf(out, "\"actor_pos\":[%.2f,%.2f,%.2f],\"dist\":%.2f,", x, y, z,
                sqrtf((mx - x) * (mx - x) + (mz - z) * (mz - z)));
        }
        else if (!have_actor && !strcmp(op, "face") && qarg(q, "x", v, sizeof v) && qarg(q, "z", v2, sizeof v2))
        {
            snprintf(line, sizeof line, "face %s %s", v, v2);
            ok = script_line(line);
        }
        else if (have_actor)
            sb_put(out, "\"warning\":\"actor not in the client's actor map; not facing it\",", 63);
        else if (!strcmp(op, "face"))
            err = "actor=HEX or x=&z=";
        if (!err && id && strcmp(op, "face"))
        {
            snprintf(line, sizeof line, "target %08X", id);
            ok = ok && script_line(line);
            if (!strcmp(op, "interact"))
                ok = ok && script_line("interact");
        }
        else if (!err && !id && strcmp(op, "face"))
        {
            if (!strcmp(op, "interact"))
                ok = script_line("interact");
            else
                err = "actor=HEX";
        }
    }
    else if (!strcmp(op, "walkto"))
    {
        if (!qarg(q, "x", v, sizeof v) || !qarg(q, "z", v2, sizeof v2))
            err = "x=&z=";
        else
        {
            char tol[32] = "1.5";
            qarg(q, "tol", tol, sizeof tol);
            snprintf(line, sizeof line, "walkto %s %s %s", v, v2, tol);
            ok = script_line(line);
        }
    }
    else if (!strcmp(op, "skip_cutscene"))
    {
        /* the CutScene _skip binding's effect, as soon as a cutscene plays (within `wait` ms, default 30000) */
        int ms = qarg(q, "wait", v, sizeof v) ? atoi(v) : 30000;
        g_skip_deadline = rt_monotonic_ns() + (uint64_t)ms * 1000000ull;
        g_want_skip = 1;
        sb_printf(out, "\"playing\":%s,", S.cut_playing ? "true" : "false");
    }
    else if (!strcmp(op, "input"))
    {
        if (!qarg(q, "line", v, sizeof v))
            err = "line=...";
        else
            ok = script_line(v);
    }
    else
        err = "op: key advance auto_advance cancel choose face target interact untarget walkto skip_cutscene input";
    if (err)
    {
        sb_put(out, "\"ok\":false,\"error\":", 19);
        sb_jcstr(out, err);
    }
    else
        sb_printf(out, "\"ok\":%s,\"script\":", ok ? "true" : "false"), sb_jcstr(out, g_script);
}

/* --- HTTP ----------------------------------------------------------------------------------------- */
static void reply(int fd, int code, const char* body, size_t n)
{
    char h[256];
    int k = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code,
        code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", n);
    send(fd, h, (size_t)k, 0);
    for (size_t o = 0; o < n;)
    {
        ssize_t w = send(fd, body + o, n - o, 0);
        if (w <= 0)
            break;
        o += (size_t)w;
    }
}

static int type_ok(const char* types, const char* t)
{
    if (!types || !*types)
        return 1;
    size_t tl = strlen(t);
    for (const char* p = types; *p;)
    {
        const char* c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == tl && !strncmp(p, t, l))
            return 1;
        if (!c)
            break;
        p = c + 1;
    }
    return 0;
}

static void events(const char* q, Sb* s)
{
    char v[256], types[128] = "";
    uint64_t since = qarg(q, "since", v, sizeof v) ? strtoull(v, NULL, 10) : 0;
    int wait = qarg(q, "wait", v, sizeof v) ? atoi(v) : 0;
    int limit = qarg(q, "limit", v, sizeof v) ? atoi(v) : 500;
    qarg(q, "type", types, sizeof types);
    if (limit <= 0 || limit > 5000)
        limit = 5000;
    pthread_mutex_lock(&g_mu);
    if (wait > 0)
    {
        uint64_t end = rt_monotonic_ns() + (uint64_t)wait * 1000000ull;
        for (;;)
        {
            int any = 0;
            for (uint64_t k = since + 1; k <= g_seq && !any; ++k)
                if (g_seq - k < RING && type_ok(types, g_ring[k % RING].type))
                    any = 1;
            uint64_t nw = rt_monotonic_ns();
            if (any || nw >= end)
                break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t left = end - nw;
            ts.tv_sec += (time_t)(left / 1000000000ull);
            ts.tv_nsec += (long)(left % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L)
                ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
    }
    uint64_t first = since + 1;
    if (g_seq >= RING && first <= g_seq - RING)
        first = g_seq - RING + 1;
    sb_printf(s, "{\"first\":%llu,\"events\":[", (unsigned long long)first);
    int n = 0;
    uint64_t last = since;
    for (uint64_t k = first; k <= g_seq && n < limit; ++k)
    {
        Ev* e = &g_ring[k % RING];
        last = k;
        if (e->seq != k || !e->json || !type_ok(types, e->type))
            continue;
        if (n++)
            sb_put(s, ",", 1);
        sb_put(s, e->json, strlen(e->json));
    }
    sb_printf(s, "],\"next\":%llu}", (unsigned long long)last);
    pthread_mutex_unlock(&g_mu);
}

static void trace_set(const char* q, Sb* s)
{
    char v[2048];
    if (qarg(q, "lua", v, sizeof v))
    {
        if (!strcmp(v, "default"))
            trace_default();
        else if (!strcmp(v, "all"))
            memset(g_trace, 1, sizeof g_trace);
        else if (!strcmp(v, "none"))
            memset(g_trace, 0, sizeof g_trace);
        else
            for (char* p = strtok(v, ","); p; p = strtok(NULL, ","))
            {
                int on = *p != '-';
                if (*p == '-' || *p == '+')
                    p++;
                for (int i = 0; i < LB_COUNT; ++i)
                    if (!strcmp(LBT[i].name, p))
                        g_trace[i] = (uint8_t)on;
            }
    }
    if (qarg(q, "raw", v, sizeof v))
        g_raw = atoi(v);
    int n = 0;
    for (int i = 0; i < LB_COUNT; ++i)
        n += g_trace[i];
    sb_printf(s, "{\"traced\":%d,\"raw\":%d}", n, g_raw);
}

static void lua_counts(Sb* s)
{
    sb_put(s, "{\"bindings\":[", 13);
    int first = 1;
    for (int i = 0; i < LB_COUNT; ++i)
    {
        uint32_t c = g_count[i];
        if (!c && !g_trace[i])
            continue;
        sb_printf(s, "%s{\"cls\":\"%s\",\"name\":\"%s\",\"va\":\"%08X\",\"calls\":%u,\"traced\":%s}", first ? "" : ",", LBT[i].cls,
            LBT[i].name, LBT[i].va, c, g_trace[i] ? "true" : "false");
        first = 0;
    }
    sb_put(s, "]}", 2);
}

/* GET /mem?va=HEX&n=BYTES: guest memory as hex (debugging; n <= 4096) */
static void mem_dump(const char* q, Sb* s)
{
    char v[32];
    uint32_t va = qarg(q, "va", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 16) : 0;
    int n = qarg(q, "n", v, sizeof v) ? atoi(v) : 64;
    if (n <= 0 || n > 4096)
        n = 64;
    sb_printf(s, "{\"va\":\"%08X\",\"hex\":\"", va);
    for (int i = 0; i < n; ++i)
    {
        if (!ok_va(va + (uint32_t)i, 1))
            break;
        sb_printf(s, "%02x", *GUEST_PTR(va + (uint32_t)i));
    }
    char cls[160];
    sb_put(s, "\"", 1);
    if (rtti_name(va, cls, sizeof cls))
    {
        sb_put(s, ",\"cls\":", 7);
        sb_jcstr(s, cls);
    }
    if (obj_id(va))
        sb_printf(s, ",\"id\":\"%08X\"", obj_id(va));
    sb_put(s, "}", 1);
}

static void serve(int fd)
{
    char req[8192];
    int n = 0;
    while (n < (int)sizeof req - 1)
    {
        ssize_t k = recv(fd, req + n, sizeof req - 1 - (size_t)n, 0);
        if (k <= 0)
            break;
        n += (int)k;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[n] = 0;
    char method[8] = "", path[4096] = "";
    if (sscanf(req, "%7s %4095s", method, path) != 2)
    {
        close(fd);
        return;
    }
    char* q = strchr(path, '?');
    if (q)
        *q++ = 0;
    else
        q = path + strlen(path);
    Sb s = { 0 };
    int code = 200;
    if (!strcmp(path, "/mem"))
        mem_dump(q, &s);
    else if (!strcmp(path, "/state"))
        state_json(&s);
    else if (!strcmp(path, "/events"))
        events(q, &s);
    else if (!strcmp(path, "/lua"))
        lua_counts(&s);
    else if (!strcmp(path, "/trace") && !strcmp(method, "POST"))
        trace_set(q, &s);
    else if (!strcmp(path, "/act") && !strcmp(method, "POST"))
    {
        sb_put(&s, "{", 1);
        act(q, &s);
        sb_put(&s, "}", 1);
    }
    else if (!strcmp(path, "/"))
        sb_printf(&s, "{\"xivprobe\":1,\"endpoints\":[\"GET /state\",\"GET /events?since=&wait=&type=&limit=\",\"GET /lua\","
                      "\"POST /trace?lua=&raw=\",\"POST /act?op=\"]}");
    else
        code = 404, sb_printf(&s, "{\"error\":\"not found\"}");
    reply(fd, code, s.p ? s.p : "", s.n);
    free(s.p);
    close(fd);
}

static void* server(void* arg)
{
    int port = (int)(intptr_t)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 16))
    {
        rt_log("[probe] cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        close(ls);
        return NULL;
    }
    rt_log("[probe] listening on 127.0.0.1:%d (autopilot timeline %s)\n", port, g_script);
    for (;;)
    {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0)
            continue;
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        pthread_t t;
        /* a thread per request: /events may long-poll */
        if (pthread_create(&t, NULL, (void* (*)(void*))(void*)serve, (void*)(intptr_t)fd))
            close(fd);
        else
            pthread_detach(t);
    }
    return NULL;
}

void xiv_probe_init(void)
{
    const char* p = getenv("XIV_PROBE");
    if (!p || !*p || !atoi(p))
        return;
    g_on = 1;
    g_t0 = rt_monotonic_ns();
    g_raw = getenv("XIV_PROBE_RAW") ? atoi(getenv("XIV_PROBE_RAW")) : 0;
    /* acting goes through the autopilot's timeline: use the run's, or start an empty one */
    const char* sc = getenv("XIV_INPUT_SCRIPT");
    if (sc && *sc)
        snprintf(g_script, sizeof g_script, "%s", sc);
    else
    {
        const char* tmp = getenv("TMPDIR");
        snprintf(g_script, sizeof g_script, "%s/xivprobe-%d.txt", tmp && *tmp ? tmp : "/tmp", (int)getpid());
        FILE* f = fopen(g_script, "w");
        if (f)
            fclose(f);
        setenv("XIV_INPUT_SCRIPT", g_script, 1);
    }
    trace_default();
    for (int i = 0; i < LB_COUNT; ++i)
        for (size_t k = 0; k < sizeof MODEL / sizeof MODEL[0]; ++k)
            if (!strcmp(LBT[i].name, MODEL[k]))
                g_model[i] = 1;
    g_auto_advance = getenv("XIV_PROBE_AUTO_ADVANCE") ? atoi(getenv("XIV_PROBE_AUTO_ADVANCE")) : 0;
    const char* tr = getenv("XIV_PROBE_TRACE");
    if (tr && *tr)
    {
        char q[2048];
        snprintf(q, sizeof q, "lua=%s", tr);
        Sb s = { 0 };
        trace_set(q, &s);
        free(s.p);
    }
#define LB(a, c, n) rt_wrap_lb_##a = w_##a;
#include "xiv_probe_binds.h"
#undef LB
    rt_set_log_hook(on_log);
    pthread_t t;
    pthread_create(&t, NULL, server, (void*)(intptr_t)atoi(p));
    pthread_detach(t);
}
