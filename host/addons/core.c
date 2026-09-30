/* The addon host's core: addons, their Lua states, events, errors and crash guards.
 *
 * One LuaJIT state per addon. Setup order: the standard libraries and LuaJIT's, the native xi
 * table (lua_xi.c), xi.lua (events, tasks, print; it returns the hooks table the host calls), the
 * kind's compatibility layer (ashita.lua, windower.lua), then the addon's own file.
 *
 * Every entry into Lua is on a thread holding the guest lock, with the lock's safepoint yields
 * off (gt_noyield): no other guest thread can reach an addon while one runs, so the states need
 * no lock of their own. Lua never calls the game synchronously in a way that raises events
 * (chat lines and commands from Lua are queued), so dispatch never nests into a running state
 * except through the command path, which is handled as a nested call on the same state.
 *
 * A native fault inside an addon (an ffi access out of bounds) is caught by a signal guard around
 * the call: the addon is marked dead and its state abandoned (never closed: it may be corrupt), and
 * the game carries on. */
#include <ctype.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "addons.h"
#include "d3d8.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

const char* const xi_kind_name[XI_KINDS] = { "xi", "ashita", "windower" };

/* --- logging --------------------------------------------------------------------------------- */

void xi_log(const char* fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    rt_log("[addons] %s%s", line, n && line[n - 1] == '\n' ? "" : "\n");
}

void xi_log_once(const char* key, const char* fmt, ...)
{
    static uint64_t seen[4096];
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char* p = (const unsigned char*)key; *p; ++p)
        h = (h ^ *p) * 1099511628211ull;
    h |= 1;
    for (unsigned i = (unsigned)h & 4095, n = 0; n < 4096; i = (i + 1) & 4095, ++n)
    {
        if (seen[i] == h)
            return;
        if (!seen[i])
        {
            seen[i] = h;
            break;
        }
    }
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    xi_log("%s", line);
}

/* --- guest memory ---------------------------------------------------------------------------- */

int xi_mapped(uint32_t a, uint32_t n)
{
    if (a < 0x10000u || (uint64_t)a + n > 0xFFF00000ull)
        return 0;
    if (!n)
        return 1;
    for (uint32_t p = a & ~0xFFFu, end = a + n - 1; ; p += 0x1000)
    {
        if (!gwin_is_committed(p))
            return 0;
        if (p >= (end & ~0xFFFu))
            break;
    }
    return 1;
}

uint32_t xi_guest_addr(double v, int* ok)
{
    if (ok)
        *ok = 1;
    if (v >= 0 && v < 4294967296.0)
        return (uint32_t)v;
    double base = (double)(uintptr_t)rt_guest_base;
    if (v >= base && v < base + 4294967296.0)
        return (uint32_t)(v - base);
    if (ok)
        *ok = 0;
    return 0;
}

double xi_host_addr(uint32_t guest) { return guest ? (double)(uintptr_t)(rt_guest_base + guest) : 0.0; }

void xi_image(uint32_t* base, uint32_t* size, uint32_t* text, uint32_t* text_size)
{
    if (base)
        *base = rt_image_lo;
    if (size)
        *size = rt_image_hi - rt_image_lo;
    if (text)
        *text = rt_image_lo + rt_image_text_rva;
    if (text_size)
        *text_size = rt_image_text_size;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

uint32_t xi_find_pattern(uint32_t start, uint32_t size, const char* pattern, int32_t offset, uint32_t count)
{
    uint8_t want[512], any[512];
    size_t n = 0;
    for (const char* p = pattern; *p && n < sizeof want;)
    {
        if (*p == ' ' || *p == '\t')
        {
            ++p;
            continue;
        }
        if (p[0] == '?')
        {
            any[n] = 1, want[n++] = 0;
            p += p[1] == '?' ? 2 : 1;
            continue;
        }
        int h = hexval(p[0]), l = p[1] ? hexval(p[1]) : -1;
        if (h < 0 || l < 0)
            return 0;
        any[n] = 0, want[n++] = (uint8_t)(h << 4 | l);
        p += 2;
    }
    if (!n || size < n || !xi_mapped(start, size))
        return 0;
    const uint8_t* mem = GUEST_PTR(start);
    for (uint32_t i = 0; i + n <= size; ++i)
    {
        if (mem[i] != want[0] && !any[0])
            continue;
        size_t k = 1;
        while (k < n && (any[k] || mem[i + k] == want[k]))
            ++k;
        if (k == n && count-- == 0)
            return start + i + (uint32_t)offset;
    }
    return 0;
}

/* --- paths ----------------------------------------------------------------------------------- */

static char g_data_dir[1024], g_game_dir[1024], g_kind_root[XI_KINDS][1100];

const char* xi_data_dir(void) { return g_data_dir; }
const char* xi_game_dir(void) { return g_game_dir; }
const char* xi_kind_root(int kind) { return kind >= 0 && kind < XI_KINDS ? g_kind_root[kind] : g_data_dir; }

static void with_slash(char* s, size_t n)
{
    size_t l = strlen(s);
    if (l && s[l - 1] != '/' && l + 1 < n)
        s[l] = '/', s[l + 1] = 0;
}

static void make_dirs(const char* path)
{
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char* c = p + 1; *c; ++c)
        if (*c == '/')
        {
            *c = 0;
            plat_mkdir(p);
            *c = '/';
        }
    plat_mkdir(p);
}

#if !defined(_WIN32)
/* One component that doesn't exist as written, looked up without case in its folder. */
static int fix_case(char* path, size_t start)
{
    struct stat st;
    if (!stat(path, &st))
        return 1;
    char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) < start)
        return 0;
    *slash = 0;
    int parent_ok = fix_case(path, start);
    *slash = '/';
    if (!parent_ok)
        return 0;
    *slash = 0;
    DIR* d = opendir(path[0] ? path : "/");
    *slash = '/';
    if (!d)
        return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, slash + 1))
        {
            memcpy(slash + 1, e->d_name, strlen(e->d_name));
            found = 1;
            break;
        }
    closedir(d);
    return found;
}
#endif

char* xi_host_path(const char* in, char* out, size_t n)
{
    if (!n)
        return out;
    size_t o = 0;
    const char* p = in;
    /* a drive letter an addon put in front of our own paths (they are never Windows paths) */
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
        p += 2;
    for (; *p && o + 1 < n; ++p)
    {
        char c = *p == '\\' ? '/' : *p;
#if defined(_WIN32)
        if (*p == '/')
            c = '\\';
#endif
        if ((c == '/' || c == '\\') && o && (out[o - 1] == '/' || out[o - 1] == '\\'))
            continue; /* "a\\\\b", "dir/" .. "/file" */
        out[o++] = c;
    }
    out[o] = 0;
#if !defined(_WIN32)
    /* strip "./" pieces the concatenations leave; resolve case only below our own folders */
    size_t root = strlen(g_data_dir);
    if (!strncmp(out, g_data_dir, root))
        fix_case(out, root ? root - 1 : 0);
#endif
    return out;
}

/* --- addons ---------------------------------------------------------------------------------- */

#define MAX_ADDONS 256
static Addon* g_addons[MAX_ADDONS];
static unsigned g_naddons;
static int g_depth;             /* Lua calls in progress (nested: a command from an addon) */
static Addon* g_running[64];    /* the addon each depth is running */
static int g_pending_unloads;

unsigned xi_addon_count(void) { return g_naddons; }
Addon* xi_addon_at(unsigned i) { return i < g_naddons ? g_addons[i] : NULL; }
Addon* xi_current(void) { return g_depth > 0 ? g_running[g_depth - 1] : NULL; }

Addon* xi_addon_find(const char* name)
{
    for (unsigned i = 0; i < g_naddons; ++i)
        if (!strcasecmp(g_addons[i]->name, name))
            return g_addons[i];
    return NULL;
}

/* chat mode for the host's own messages: 207 (the system's white on most themes) */
#define XI_CHAT_MODE 207

static void chatf(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    xi_chat_write(XI_CHAT_MODE, line);
}

void xi_addon_error(Addon* a, const char* where, const char* msg)
{
    uint64_t now = rt_monotonic_ns() / 1000000ull;
    const char* nl = strchr(msg, '\n');
    size_t first = nl ? (size_t)(nl - msg) : strlen(msg);
    xi_log("%s: %s: %s", a ? a->name : "?", where, msg);
    if (!a)
        return;
    /* the chat log gets each distinct first line once while it keeps repeating */
    if (strncmp(a->err_text, msg, first) || a->err_text[first < sizeof a->err_text ? first : sizeof a->err_text - 1])
    {
        snprintf(a->err_text, sizeof a->err_text, "%.*s", (int)first, msg);
        a->err_repeats = 0;
        char line[400];
        snprintf(line, sizeof line, "[%s] error: %.300s", a->name, a->err_text);
        xi_chat_write(XI_CHAT_MODE, line);
    }
    else
        a->err_repeats++;
    /* continuous errors (less than a second apart) for 5 s: unloaded */
    if (!a->err_since || now - a->err_last > 1000)
        a->err_since = now;
    a->err_last = now;
    if (now - a->err_since >= 5000 && !a->unloading)
    {
        chatf("[%s] kept failing for 5 seconds; unloaded", a->name);
        xi_addon_unload(a);
    }
}

/* --- the crash guard --------------------------------------------------------------------------- */

#if !defined(_WIN32)
static RT_TLS sigjmp_buf* t_guard;
static RT_TLS volatile int t_fault_sig;
static RT_TLS volatile uintptr_t t_fault_addr;
static struct sigaction g_old_segv, g_old_bus;

static void on_fault(int sig, siginfo_t* si, void* ctx)
{
    if (t_guard)
    {
        t_fault_sig = sig;
        t_fault_addr = (uintptr_t)si->si_addr;
        siglongjmp(*t_guard, 1);
    }
    /* not ours: whoever had it before (the default: a crash report) */
    struct sigaction* old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    if (old->sa_flags & SA_SIGINFO)
        old->sa_sigaction(sig, si, ctx);
    else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN)
        old->sa_handler(sig);
    else
    {
        sigaction(sig, old, NULL);
        raise(sig);
    }
}

static void guard_install(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

static void guard_altstack(void)
{
    /* a stack overflow in an addon (runaway C recursion through ffi) needs somewhere to run */
    static RT_TLS int done;
    if (done)
        return;
    done = 1;
    stack_t ss;
    ss.ss_size = 256 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (ss.ss_sp)
        sigaltstack(&ss, NULL);
}
#endif

/* lua_pcall under the crash guard; returns the pcall status, or -1 when the addon faulted (its
 * state must not be used again). */
static int guarded_pcall(Addon* a, lua_State* L, int nargs, int nres, int errfunc)
{
#if !defined(_WIN32)
    sigjmp_buf jb, *saved = t_guard;
    guard_altstack();
    if (sigsetjmp(jb, 1))
    {
        t_guard = saved;
        char msg[200];
        snprintf(msg, sizeof msg, "native fault (signal %d at %p): the addon is stopped", t_fault_sig, (void*)t_fault_addr);
        a->dead = 1;
        xi_log("%s: %s", a->name, msg);
        chatf("[%s] crashed in native code and was stopped (the game carries on)", a->name);
        return -1;
    }
    t_guard = &jb;
    int r = lua_pcall(L, nargs, nres, errfunc);
    t_guard = saved;
    return r;
#else
    int r = -1;
    __try
    {
        r = lua_pcall(L, nargs, nres, errfunc);
    }
    __except (GetExceptionCode() == 0xE24C4A02 /* LuaJIT's own error */ ? EXCEPTION_CONTINUE_SEARCH
                                                                         : EXCEPTION_EXECUTE_HANDLER)
    {
        a->dead = 1;
        xi_log("%s: native fault %08x: the addon is stopped", a->name, GetExceptionCode());
        chatf("[%s] crashed in native code and was stopped (the game carries on)", a->name);
        r = -1;
    }
    return r;
#endif
}

static int traceback(lua_State* L)
{
    const char* msg = lua_tostring(L, 1);
    if (!msg)
        msg = lua_isnoneornil(L, 1) ? "error (nil)" : luaL_typename(L, 1);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* Calls the function under the top nargs values on a's state, protected and guarded. On error the
 * message is reported; returns 1 on success with nres results left on the stack. */
static int call(Addon* a, int nargs, int nres, const char* where)
{
    lua_State* L = a->L;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    if (g_depth >= (int)(sizeof g_running / sizeof *g_running))
    {
        lua_settop(L, base - 1);
        xi_addon_error(a, where, "calls nested too deeply");
        return 0;
    }
    g_running[g_depth++] = a;
    if (g_depth == 1)
        gt_noyield(1);
    int r = guarded_pcall(a, L, nargs, nres, base);
    if (--g_depth == 0)
        gt_noyield(0);
    if (r == -1)
        return 0; /* dead: the stack is not ours to fix */
    if (r)
    {
        xi_addon_error(a, where, lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
        lua_settop(L, base - 1);
        return 0;
    }
    lua_remove(L, base);
    return 1;
}

/* Pushes hooks[name] (a function) on a's state; 0 if there is none. */
static int push_hook(Addon* a, const char* name)
{
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->hooks);
    lua_getfield(a->L, -1, name);
    lua_remove(a->L, -2);
    if (lua_isfunction(a->L, -1))
        return 1;
    lua_pop(a->L, 1);
    return 0;
}

/* The API an addon's file is written for, from what it names: vana-style detection by counting
 * identifiers not preceded by a word character or '.'. -1: undecided. */
static int detect_kind(const char* src, size_t n)
{
    static const struct
    {
        const char* word;
        int kind;
    } W[] = { { "xi.", XI_KIND_XI }, { "windower.", XI_KIND_WINDOWER }, { "_addon.", XI_KIND_WINDOWER },
              { "ashita.", XI_KIND_ASHITA }, { "AshitaCore", XI_KIND_ASHITA }, { "addon.", XI_KIND_ASHITA } };
    unsigned count[XI_KINDS] = { 0 };
    for (size_t i = 0; i < n; ++i)
    {
        if (i && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_' || src[i - 1] == '.'))
            continue;
        for (unsigned k = 0; k < sizeof W / sizeof *W; ++k)
        {
            size_t l = strlen(W[k].word);
            if (i + l <= n && !memcmp(src + i, W[k].word, l))
                count[W[k].kind]++;
        }
    }
    if (count[XI_KIND_XI] && !count[XI_KIND_ASHITA] && !count[XI_KIND_WINDOWER])
        return XI_KIND_XI;
    if (count[XI_KIND_WINDOWER] > count[XI_KIND_ASHITA])
        return XI_KIND_WINDOWER;
    if (count[XI_KIND_ASHITA] > count[XI_KIND_WINDOWER])
        return XI_KIND_ASHITA;
    return -1;
}

static char* read_file(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = n >= 0 ? (char*)malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n)
        free(b), b = NULL;
    fclose(f);
    if (b)
        b[n] = 0, *size = (size_t)n;
    return b;
}

/* Where an addon of a kind lives: <root>/addons/<name>/<name>.lua */
static int addon_file(int kind, const char* name, char* dir, size_t dn, char* file, size_t fn)
{
    char raw[1200];
    snprintf(raw, sizeof raw, "%saddons/%s/%s.lua", xi_kind_root(kind), name, name);
    xi_host_path(raw, file, fn);
    PlatStat st;
    if (!plat_stat(file, &st))
        return 0;
    snprintf(dir, dn, "%s", file);
    char* slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    return 1;
}

extern int luaopen_socket_core(lua_State* L);
extern int luaopen_mime_core(lua_State* L);
extern int luaopen_lfs(lua_State* L);

static void preload(lua_State* L, const char* name, lua_CFunction f)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
}

/* Runs an embedded script (host/addons/lua/<name>.lua) with nargs arguments under it; leaves
 * nres results. */
static int run_embedded(Addon* a, const char* name, int nargs, int nres)
{
    size_t n;
    const char* src = xi_embedded(name, &n);
    char chunk[80];
    snprintf(chunk, sizeof chunk, "=[xi %s]", name);
    if (!src || luaL_loadbuffer(a->L, src, n, chunk))
    {
        xi_addon_error(a, name, src ? lua_tostring(a->L, -1) : "missing embedded script");
        lua_settop(a->L, 0);
        return 0;
    }
    if (nargs)
        lua_insert(a->L, -1 - nargs);
    return call(a, nargs, nres, name);
}

static void addon_free(Addon* a)
{
    xi_gui_free_owned(a);
    if (a->L && !a->dead)
        lua_close(a->L);
    free(a);
}

int xi_addon_load(const char* name, int kind)
{
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
    {
        chatf("addon: bad name");
        return 0;
    }
    if (xi_addon_find(name))
    {
        chatf("addon: %s is already loaded", name);
        return 0;
    }
    if (g_naddons >= MAX_ADDONS)
    {
        chatf("addon: too many addons");
        return 0;
    }
    Addon* a = (Addon*)calloc(1, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    int found = 0;
    static const int ORDER[] = { XI_KIND_XI, XI_KIND_ASHITA, XI_KIND_WINDOWER };
    for (unsigned i = 0; i < 3 && !found; ++i)
        if (kind < 0 || kind == ORDER[i])
            found = addon_file(ORDER[i], name, a->dir, sizeof a->dir, a->file, sizeof a->file) ? (a->kind = ORDER[i], 1) : 0;
    if (!found)
    {
        chatf("addon: %s not found (looked in %saddons/, %saddons/, %saddons/)", name, xi_kind_root(0), xi_kind_root(1),
            xi_kind_root(2));
        free(a);
        return 0;
    }
    size_t size;
    char* src = read_file(a->file, &size);
    if (!src)
    {
        chatf("addon: cannot read %s", a->file);
        free(a);
        return 0;
    }
    int detected = detect_kind(src, size);
    if (detected >= 0 && detected != a->kind)
    {
        xi_log("%s: in the %s folder but written for %s", name, xi_kind_name[a->kind], xi_kind_name[detected]);
        a->kind = detected;
    }

    lua_State* L = luaL_newstate();
    if (!L)
    {
        free(src);
        free(a);
        return 0;
    }
    a->L = L;
    luaL_openlibs(L);
    preload(L, "socket.core", luaopen_socket_core);
    preload(L, "mime.core", luaopen_mime_core);
    preload(L, "lfs", luaopen_lfs);
    g_addons[g_naddons++] = a; /* registered before it runs: its load code may raise events */

    /* xi.lua(native xi, addon info) -> hooks */
    xi_lua_open(L, a);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "xi");
    lua_createtable(L, 0, 6);
    lua_pushstring(L, a->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, a->dir);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, a->file);
    lua_setfield(L, -2, "file");
    lua_pushstring(L, xi_kind_name[a->kind]);
    lua_setfield(L, -2, "kind");
    lua_pushstring(L, xi_kind_root(a->kind));
    lua_setfield(L, -2, "root");
    lua_pushstring(L, g_data_dir);
    lua_setfield(L, -2, "data_dir");
    int ok = run_embedded(a, "xi", 2, 1) && lua_istable(L, -1);
    if (ok)
        a->hooks = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ok && a->kind != XI_KIND_XI)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, a->hooks);
        ok = run_embedded(a, xi_kind_name[a->kind], 1, 0);
    }
    if (ok && !a->dead)
    {
        if (luaL_loadbuffer(L, src, size, lua_pushfstring(L, "@%s", a->file)))
        {
            xi_addon_error(a, "load", lua_tostring(L, -1));
            ok = 0;
        }
        else
        {
            lua_remove(L, -2); /* the chunk name */
            ok = call(a, 0, 0, "load");
        }
    }
    free(src);
    if (ok && !a->dead && push_hook(a, "load"))
        ok = call(a, 0, 0, "load event");
    if (!ok || a->dead)
    {
        chatf("addon: %s failed to load", name);
        for (unsigned i = 0; i < g_naddons; ++i)
            if (g_addons[i] == a)
            {
                memmove(&g_addons[i], &g_addons[i + 1], (g_naddons - i - 1) * sizeof *g_addons);
                g_naddons--;
                break;
            }
        if (g_depth)
            a->unloading = 1, a->dead = a->dead; /* freed below regardless: nothing of it is running */
        addon_free(a);
        return 0;
    }
    xi_log("loaded %s (%s) from %s", a->name, xi_kind_name[a->kind], a->file);
    chatf("addon: loaded %s (%s)", a->name, xi_kind_name[a->kind]);
    return 1;
}

static void unload_now(Addon* a)
{
    if (!a->dead && a->L && push_hook(a, "unload"))
        call(a, 0, 0, "unload");
    for (unsigned i = 0; i < g_naddons; ++i)
        if (g_addons[i] == a)
        {
            memmove(&g_addons[i], &g_addons[i + 1], (g_naddons - i - 1) * sizeof *g_addons);
            g_naddons--;
            break;
        }
    xi_log("unloaded %s", a->name);
    chatf("addon: unloaded %s", a->name);
    addon_free(a);
}

void xi_addon_unload(Addon* a)
{
    if (!a || a->unloading)
        return;
    a->unloading = 1;
    if (g_depth)
    {
        g_pending_unloads = 1;
        return;
    }
    unload_now(a);
}

void xi_addon_reload(Addon* a)
{
    char name[64];
    int kind = a->kind;
    snprintf(name, sizeof name, "%s", a->name);
    if (g_depth)
    {
        /* later, from the frame: unload and load again */
        a->unloading = 2;
        g_pending_unloads = 1;
        return;
    }
    unload_now(a);
    xi_addon_load(name, kind);
}

static void process_unloads(void)
{
    if (!g_pending_unloads || g_depth)
        return;
    g_pending_unloads = 0;
    for (unsigned i = 0; i < g_naddons;)
    {
        Addon* a = g_addons[i];
        if (a->unloading)
        {
            char name[64];
            int kind = a->kind, again = a->unloading == 2;
            snprintf(name, sizeof name, "%s", a->name);
            unload_now(a);
            if (again)
                xi_addon_load(name, kind);
            i = 0;
            continue;
        }
        ++i;
    }
}

/* --- events ---------------------------------------------------------------------------------- */

static void push_event(lua_State* L, const XiEvent* e)
{
    lua_createtable(L, 0, 16);
    lua_pushstring(L, e->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, e->mode);
    lua_setfield(L, -2, "mode");
    lua_pushboolean(L, e->injected);
    lua_setfield(L, -2, "injected");
    lua_pushboolean(L, e->blocked);
    lua_setfield(L, -2, "blocked");
    if (e->data)
    {
        lua_pushlstring(L, (const char*)e->data, e->size);
        lua_setfield(L, -2, "data");
    }
    if (e->mod_cap)
    {
        lua_pushlstring(L, (const char*)e->mod, e->mod_size);
        lua_setfield(L, -2, "modified");
        lua_pushinteger(L, e->mode_mod);
        lua_setfield(L, -2, "mode_modified");
    }
    if (e->chunk)
    {
        lua_pushlstring(L, (const char*)e->chunk, e->chunk_size);
        lua_setfield(L, -2, "chunk");
    }
    lua_pushinteger(L, e->id);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, e->sequence);
    lua_setfield(L, -2, "sequence");
    if (e->msg || e->x || e->y || e->delta)
    {
        lua_pushinteger(L, e->msg);
        lua_setfield(L, -2, "message");
        lua_pushinteger(L, e->x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, e->y);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, e->delta);
        lua_setfield(L, -2, "delta");
    }
    if (e->key || e->vk)
    {
        lua_pushinteger(L, e->key);
        lua_setfield(L, -2, "key");
        lua_pushinteger(L, e->vk);
        lua_setfield(L, -2, "vk");
        lua_pushinteger(L, e->flags);
        lua_setfield(L, -2, "flags");
        lua_pushboolean(L, e->down);
        lua_setfield(L, -2, "down");
    }
}

/* What the addon changed: blocked, the modified buffer, the mode. */
static void pull_event(lua_State* L, int t, XiEvent* e)
{
    lua_getfield(L, t, "blocked");
    if (lua_toboolean(L, -1))
        e->blocked = 1;
    lua_pop(L, 1);
    if (e->mod_cap)
    {
        size_t n;
        lua_getfield(L, t, "modified");
        const char* s = lua_tolstring(L, -1, &n);
        if (s && (n != e->mod_size || memcmp(s, e->mod, n)))
        {
            if (n > e->mod_cap)
            {
                xi_log_once(e->name, "%s: an addon made the %s longer than the buffer (%zu > %zu); cut", xi_current() ? xi_current()->name : "?",
                    e->name, n, e->mod_cap);
                n = e->mod_cap;
            }
            memcpy(e->mod, s, n);
            e->mod_size = n;
        }
        lua_pop(L, 1);
        lua_getfield(L, t, "mode_modified");
        if (lua_isnumber(L, -1))
            e->mode_mod = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
}

void xi_raise_kind(XiEvent* e, int kind)
{
    for (unsigned i = 0; i < g_naddons; ++i)
    {
        Addon* a = g_addons[i];
        if (a->dead || a->unloading || (kind >= 0 && a->kind != kind))
            continue;
        lua_State* L = a->L;
        int top = lua_gettop(L);
        if (!push_hook(a, "raise"))
            continue;
        push_event(L, e);
        lua_pushvalue(L, -1);
        lua_insert(L, top + 1); /* the table kept under the call */
        if (call(a, 1, 1, e->name))
        {
            if (lua_toboolean(L, -1))
                e->handled = 1;
            lua_pop(L, 1);
            pull_event(L, top + 1, e);
        }
        if (!a->dead)
            lua_settop(L, top);
    }
    process_unloads();
}

void xi_raise(XiEvent* e) { xi_raise_kind(e, -1); }

/* --- frame and lifetime ---------------------------------------------------------------------- */

static int g_started, g_shut;
int xi_headless;

void addons_init(const AddonsSetup* s)
{
    snprintf(g_data_dir, sizeof g_data_dir, "%s", s->data_dir ? s->data_dir : ".");
    with_slash(g_data_dir, sizeof g_data_dir);
    snprintf(g_game_dir, sizeof g_game_dir, "%s", s->game_dir ? s->game_dir : ".");
    with_slash(g_game_dir, sizeof g_game_dir);
    for (int k = 0; k < XI_KINDS; ++k)
    {
        snprintf(g_kind_root[k], sizeof g_kind_root[k], "%s%s/", g_data_dir, xi_kind_name[k]);
        char p[1200];
        snprintf(p, sizeof p, "%saddons", g_kind_root[k]);
        make_dirs(p);
        snprintf(p, sizeof p, "%sscripts", g_kind_root[k]);
        make_dirs(p);
    }
#if !defined(_WIN32)
    guard_install();
#endif
    extern void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n);
    xi_res_setup(g_game_dir, s->dat_overlays, s->ndat_overlays);
    xi_hooks_init();
    xi_input_init();
    xi_gui_init();
    xi_log("data in %s", g_data_dir);
}

void addons_frame(void)
{
    if (g_shut)
        return;
    g_started = 1;
    xi_cmd_frame();
    xi_hooks_frame();
    for (unsigned i = 0; i < g_naddons; ++i)
    {
        Addon* a = g_addons[i];
        if (!a->dead && !a->unloading && push_hook(a, "frame"))
            call(a, 0, 0, "frame");
    }
    process_unloads();
    /* the drawing events, inside the overlay's frame: Ashita's d3d_present, Windower's prerender */
    uint32_t w = 0, h = 0;
    d3d8_backbuffer_size(&w, &h);
    if (xi_headless && !w)
        w = 1920, h = 1080;
    if (w && h)
    {
        xi_gui_begin(w, h);
        xi_frame_draw("present");
        xi_gui_end();
        xi_frame_draw("postrender");
    }
}

/* The frame's drawing events, inside the overlay's frame. */
void xi_frame_draw(const char* name)
{
    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = name;
    xi_raise(&e);
}

void addons_shutdown(void)
{
    if (g_shut)
        return;
    g_shut = 1;
    xi_log("shutting down: unloading %u addons", g_naddons);
    while (g_naddons)
        unload_now(g_addons[g_naddons - 1]);
}
