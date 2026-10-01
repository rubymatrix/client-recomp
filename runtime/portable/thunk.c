/* Synthetic import addresses and shim dispatch. See thunk.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint64_t prof_ns, prof_calls; /* the game thread's time in it since the last report */
} Thunk;

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

uint32_t thunk_for(const char* dll, const char* name)
{
    char full[96];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    for (unsigned i = 0; i < g_count; ++i)
        if (!stricmp_(g_thunks[i].name, full))
            return THUNK_BASE + i * THUNK_STRIDE;
    if (g_count == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 512;
        if (g_cap > THUNK_MAX)
            return 0;
        g_thunks = (Thunk*)realloc(g_thunks, g_cap * sizeof *g_thunks);
    }
    Thunk* t = &g_thunks[g_count];
    memset(t, 0, sizeof *t);
    strcpy(t->name, full);
    t->shim = find_shim(dll, name);
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

/* the shims the game thread spent the most time in since the last call, one line, then reset */
void thunk_prof_report(void)
{
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
    }
    if (g_trace)
    {
        uint32_t site = rd32(g->esp), a0 = rd32(g->esp + 4), a1 = rd32(g->esp + 8), a2 = rd32(g->esp + 12), a3 = rd32(g->esp + 16);
        s(g);
        rt_log("[trace] %08x %-44s (%08x %08x %08x %08x) = %08x\n", site, name, a0, a1, a2, a3, g->eax);
        return 1;
    }
    if (thunk_timer && !t_depth)
    {
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
        }
        return 1;
    }
    t_depth++;
    s(g);
    t_depth--;
    return 1;
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
