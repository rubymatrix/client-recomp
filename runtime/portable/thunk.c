/* Synthetic import addresses and shim dispatch. See thunk.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "plat.h"
#include "thunk.h"

#if defined(_MSC_VER)
#define stricmp_ _stricmp
#else
#include <strings.h>
#define stricmp_ strcasecmp
#endif

typedef struct Thunk
{
    char name[96]; /* DLL!name */
    Shim shim;
    int nolock; /* thread-safe on its own: runs without the guest lock (gthread.c, parallel mode) */
    uint32_t locks; /* XIV_LOCKSHIMS: calls that took the guest lock since the last report */
    int d3d;        /* Direct3D (d3d9, d3dx9): serialized by the D3D lock */
    uint64_t prof_ns, prof_calls; /* the game thread's time in it since the last report */
} Thunk;

/* The shims that run without the guest lock in parallel mode: hot, and safe with every guest thread
 * in them at once (atomics, per-thread state, the clock). */
static const char* const kNoLock[] = {
    "InterlockedIncrement", "InterlockedDecrement", "InterlockedExchange", "InterlockedExchangeAdd",
    "InterlockedCompareExchange", "EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection",
    "TlsGetValue", "TlsSetValue", "FlsGetValue", "FlsSetValue", "GetLastError", "SetLastError",
    "GetCurrentThreadId", "GetCurrentThread", "GetCurrentProcess", "GetCurrentProcessId",
    "timeGetTime", "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount",
    "GetSystemTimeAsFileTime", "SwitchToThread", "Sleep", "D3DPERF_BeginEvent", "D3DPERF_EndEvent",
    /* kernel objects: kobj.c locks itself, and lets go of the guest lock only if held */
    "WaitForSingleObject", "WaitForMultipleObjects", "SetEvent", "ResetEvent", "ReleaseSemaphore",
    "ReleaseMutex",
    /* a stack switch on the calling thread (fiber.c): touches only this thread's TEB */
    "SwitchToFiber",
    /* Direct3D 9's per-draw state and draws: a device is used by one thread (the game's renderer;
     * not D3DCREATE_MULTITHREADED), and these touch only its state and the Metal back end */
    "IDirect3DDevice9::SetPixelShaderConstantF", "IDirect3DDevice9::SetVertexShaderConstantF",
    "IDirect3DDevice9::SetPixelShaderConstantI", "IDirect3DDevice9::SetVertexShaderConstantI",
    "IDirect3DDevice9::SetPixelShaderConstantB", "IDirect3DDevice9::SetVertexShaderConstantB",
    "IDirect3DDevice9::SetTexture", "IDirect3DDevice9::SetSamplerState", "IDirect3DDevice9::SetStreamSource",
    "IDirect3DDevice9::SetStreamSourceFreq", "IDirect3DDevice9::SetVertexDeclaration",
    "IDirect3DDevice9::DrawIndexedPrimitive", "IDirect3DDevice9::DrawPrimitive", "IDirect3DDevice9::SetIndices",
    "IDirect3DDevice9::SetRenderState", "IDirect3DDevice9::SetVertexShader", "IDirect3DDevice9::SetPixelShader",
    "IDirect3DDevice9::SetScissorRect", "IDirect3DDevice9::SetViewport", "IDirect3DDevice9::SetFVF",
    NULL };

static int g_lockcount = -1;

static int is_nolock(const char* name)
{
    for (int i = 0; kNoLock[i]; ++i)
        if (!strcmp(name, kNoLock[i]))
            return 1;
    return 0;
}

static Thunk* g_thunks;
static unsigned g_count, g_cap;
static const ShimDef* g_tables[32];
static unsigned g_ntables;

void thunk_register(const ShimDef* defs)
{
    if (g_ntables < sizeof g_tables / sizeof g_tables[0])
        g_tables[g_ntables++] = defs;
}

static Shim find_shim(const char* dll, const char* name)
{
    for (unsigned t = 0; t < g_ntables; ++t)
        for (const ShimDef* d = g_tables[t]; d->name; ++d)
            if (!strcmp(d->name, name) && (!d->dll || !stricmp_(d->dll, dll)))
                return d->fn;
    return NULL;
}

Shim thunk_shim(const char* dll, const char* name) { return find_shim(dll, name); }

void thunk_override(const ShimDef* defs)
{
    if (g_ntables < sizeof g_tables / sizeof g_tables[0])
    {
        memmove(&g_tables[1], &g_tables[0], g_ntables * sizeof g_tables[0]);
        g_tables[0] = defs;
        g_ntables++;
    }
}

uint32_t thunk_for(const char* dll, const char* name)
{
    char full[96];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    for (unsigned i = 0; i < g_count; ++i)
        if (!stricmp_(g_thunks[i].name, full))
            return THUNK_BASE + i * THUNK_STRIDE;
    if (!g_thunks) /* all of it at once: guest threads read the table without a lock */
    {
        g_cap = THUNK_MAX;
        g_thunks = (Thunk*)calloc(g_cap, sizeof *g_thunks);
    }
    if (g_count == g_cap)
        return 0;
    Thunk* t = &g_thunks[g_count];
    memset(t, 0, sizeof *t);
    strcpy(t->name, full);
    t->shim = find_shim(dll, name);
    t->nolock = is_nolock(name);
    /* the D3DPERF markers touch no device state: the game thread calls them while the renderer holds the lock */
    t->d3d = !strncmp(dll, "d3d", 3) && strncmp(name, "D3DPERF_", 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return THUNK_BASE + g_count++ * THUNK_STRIDE;
}

const char* thunk_name(uint32_t addr)
{
    if (addr < THUNK_BASE || (addr - THUNK_BASE) % THUNK_STRIDE)
        return NULL;
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < g_count ? g_thunks[i].name : NULL;
}

/* FFXI_RECOMP_TRACE=1: every shim call, with its call site, first four arguments and the result
 * (the portable counterpart of R2's boundary trace, runtime/win32/bridge.c). */
static int g_trace = -1;

void (*thunk_timer)(uint64_t ns);
uint32_t thunk_prof_thread;
static _Thread_local int t_depth;

/* the profiled thread's time by call site (the address a shim returns to) */
#define SITES 1024
static struct { uint32_t site, thunk; uint64_t ns, calls; } g_sites[SITES];

static void site_add(uint32_t site, uint32_t thunk, uint64_t ns)
{
    for (uint32_t h = (site * 2654435761u) >> 22, k = 0; k < SITES; ++k, h = (h + 1) & (SITES - 1))
        if (g_sites[h].site == site || !g_sites[h].site)
        {
            g_sites[h].site = site, g_sites[h].thunk = thunk;
            g_sites[h].ns += ns, g_sites[h].calls++;
            return;
        }
}

static void sites_report(void)
{
    for (int r = 0; r < 6; ++r)
    {
        int best = -1;
        for (int i = 0; i < SITES; ++i)
            if (g_sites[i].site && g_sites[i].ns && (best < 0 || g_sites[i].ns > g_sites[best].ns))
                best = i;
        if (best < 0)
            break;
        rt_log("[gfx]     %08x %s %.0f ms/%llu\n", g_sites[best].site, g_thunks[g_sites[best].thunk].name,
               g_sites[best].ns * 1e-6, (unsigned long long)g_sites[best].calls);
        g_sites[best].ns = 0;
    }
    memset(g_sites, 0, sizeof g_sites);
}

/* the shims the game thread spent the most time in since the last call, one line, then reset */
void thunk_prof_report(void)
{
    sites_report();
    unsigned top[8] = { 0 }, n = 0;
    for (unsigned i = 0; i < g_count; ++i)
    {
        if (!g_thunks[i].prof_calls)
            continue;
        unsigned k = n < 8 ? n++ : 8;
        while (k > 0 && g_thunks[top[k - 1]].prof_ns < g_thunks[i].prof_ns)
        {
            if (k < 8)
                top[k] = top[k - 1];
            k--;
        }
        if (k < 8)
            top[k] = i;
    }
    if (!n)
        return;
    char line[1024];
    int o = snprintf(line, sizeof line, "[gfx]   API time (2 s):");
    for (unsigned k = 0; k < n && o < (int)sizeof line - 100; ++k)
    {
        const Thunk* t = &g_thunks[top[k]];
        const char* nm = strchr(t->name, '!');
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f ms/%llu", nm ? nm + 1 : t->name, (double)t->prof_ns * 1e-6,
            (unsigned long long)t->prof_calls);
    }
    rt_log("%s\n", line);
    for (unsigned i = 0; i < g_count; ++i)
        g_thunks[i].prof_ns = g_thunks[i].prof_calls = 0;
}

static void thunk_run(Guest* g, uint32_t target, const char* name, Shim s);

/* the D3D lock: recursive (a D3DX call may come back through d3d9), spinning briefly first */
static volatile uint32_t g_d3d_owner;
static uint32_t g_d3d_depth;
static volatile uint32_t g_d3d_word, g_d3d_sleepers;

static void d3d_lock(void)
{
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&g_d3d_owner, __ATOMIC_RELAXED) == me)
    {
        g_d3d_depth++;
        return;
    }
    for (unsigned spin = 0;; ++spin)
    {
        uint32_t z = 0;
        if (__atomic_compare_exchange_n(&g_d3d_word, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        if (spin < 2000)
        {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        else
        {
            __atomic_fetch_add(&g_d3d_sleepers, 1, __ATOMIC_SEQ_CST);
            plat_wait32(&g_d3d_word, 1);
            __atomic_fetch_sub(&g_d3d_sleepers, 1, __ATOMIC_SEQ_CST);
        }
    }
    g_d3d_owner = me;
    g_d3d_depth = 1;
}

static void d3d_unlock(void)
{
    if (--g_d3d_depth)
        return;
    g_d3d_owner = 0;
    __atomic_store_n(&g_d3d_word, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_d3d_sleepers, __ATOMIC_SEQ_CST))
        plat_wake_all32(&g_d3d_word);
}

int thunk_dispatch(Guest* g, uint32_t target)
{
    const char* name = thunk_name(target);
    if (!name)
        return 0;
    Shim s = g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].shim;
    if (!s)
    {
        char buf[160];
        snprintf(buf, sizeof buf, "no shim yet for %s (called from %08x)", name, rd32(g->esp));
        rt_fatal(g, target, buf);
    }
    if (g_trace < 0)
    {
        const char* e = getenv("FFXI_RECOMP_TRACE");
        g_trace = e && *e == '1';
        g_lockcount = getenv("XIV_LOCKSHIMS") != NULL;
    }
    /* parallel mode: the guest calls in without the lock; a shim runs with it unless it is safe without */
    /* Direct3D: one thread in it at a time (the renderer and the resource loader both use it), on a
     * lock of its own, so the hot per-draw calls (lock-free below) never wait on the guest lock. It
     * is taken before the guest lock, never after: a D3D call that lets go of the guest lock and
     * takes it back (around file reads, callbacks) must not wait for it while another thread holding
     * it waits for Direct3D. */
    int d3d = gt_parallel && g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].d3d;
    int held = d3d ? gt_release() : 0;
    if (d3d)
        d3d_lock();
    gt_reacquire(held);
    int lock = gt_parallel && !g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].nolock && !gt_holds();
    if (lock)
    {
        if (g_lockcount)
            __atomic_fetch_add(&g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].locks, 1, __ATOMIC_RELAXED);
        gt_lock();
    }
    thunk_run(g, target, name, s);
    if (lock && gt_holds())
        gt_unlock();
    if (d3d)
        d3d_unlock();
    return 1;
}

static void thunk_run(Guest* g, uint32_t target, const char* name, Shim s)
{
    if (g_trace)
    {
        uint32_t site = rd32(g->esp), a0 = rd32(g->esp + 4), a1 = rd32(g->esp + 8), a2 = rd32(g->esp + 12), a3 = rd32(g->esp + 16);
        s(g);
        rt_log("[trace] %08x %-44s (%08x %08x %08x %08x) = %08x\n", site, name, a0, a1, a2, a3, g->eax);
        return;
    }
    if (thunk_timer && !t_depth)
    {
        uint32_t site_of_call = rd32(g->esp);
        uint64_t t0 = rt_monotonic_ns();
        t_depth++;
        s(g);
        t_depth--;
        uint64_t ns = rt_monotonic_ns() - t0;
        thunk_timer(ns);
        if (plat_thread_id() == thunk_prof_thread)
        {
            Thunk* t = &g_thunks[(target - THUNK_BASE) / THUNK_STRIDE];
            t->prof_ns += ns, t->prof_calls++;
            site_add(site_of_call, (uint32_t)(t - g_thunks), ns);
        }
        return;
    }
    t_depth++;
    s(g);
    t_depth--;
    return;
}

unsigned thunk_report_missing(void)
{
    /* gamecore's 1,560 table slots are thunks too; most are never called, so they are only counted */
    unsigned n = 0, slots = 0;
    for (unsigned i = 0; i < g_count; ++i)
        if (!g_thunks[i].shim)
        {
            if (!strncmp(g_thunks[i].name, "gamecore!", 12))
                slots++;
            else
            {
                rt_log("[recomp]   no shim: %s\n", g_thunks[i].name);
                n++;
            }
        }
    rt_log("[recomp] %u bound imports have no shim yet (and %u gamecore slots)\n", n, slots);
    return n;
}

/* XIV_LOCKSHIMS=1: the shims that took the guest lock most since the last call, one line */
void thunk_lock_report(void)
{
    if (g_lockcount <= 0)
        return;
    char line[1024];
    int o = snprintf(line, sizeof line, "[lock] shims:");
    for (int r = 0; r < 10; ++r)
    {
        unsigned best = 0;
        for (unsigned i = 1; i < g_count; ++i)
            if (g_thunks[i].locks > g_thunks[best].locks)
                best = i;
        if (!g_thunks[best].locks || o > (int)sizeof line - 80)
            break;
        const char* nm = strchr(g_thunks[best].name, '!');
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %u", nm ? nm + 1 : g_thunks[best].name, g_thunks[best].locks);
        g_thunks[best].locks = 0;
    }
    for (unsigned i = 0; i < g_count; ++i)
        g_thunks[i].locks = 0;
    rt_log("%s\n", line);
}
