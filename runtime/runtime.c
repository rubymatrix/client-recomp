/* Runtime for generated code: indirect-call dispatch, traps, CPU identification. */
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* rt_table (table.c, generated): every translated function, sorted by guest address. */
int rt_index(uint32_t addr)
{
    unsigned lo = 0, hi = rt_table_count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (rt_table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < rt_table_count && rt_table[lo].addr == addr) ? (int)lo : -1;
}

GuestFn rt_lookup(uint32_t addr)
{
    int i = rt_index(addr);
    return i >= 0 ? rt_table[i].fn : NULL;
}

static RtNativeHandler g_native = NULL;

void rt_set_native_handler(RtNativeHandler h) { g_native = h; }

uint32_t rt_reloc_delta;
uint32_t rt_image_lo, rt_image_hi;
static volatile uint32_t g_lookup_gen = 1; /* rt_lookup_any's cache: bumped when code is placed */

void rt_set_image(uint32_t runtime_base)
{
    g_lookup_gen++;
    rt_reloc_delta = runtime_base - rt_image_base;
    rt_image_lo = runtime_base;
    rt_image_hi = runtime_base + rt_image_size;
}

typedef struct AddedModule
{
    const RtModule* m;
    uint32_t lo, hi;
} AddedModule;

static AddedModule g_modules[8];
static unsigned g_nmodules;

void rt_add_module(const RtModule* m, uint32_t runtime_base)
{
    if (g_nmodules < sizeof g_modules / sizeof g_modules[0])
    {
        g_lookup_gen++;
        *m->delta = runtime_base - m->base;
        g_modules[g_nmodules].m = m;
        g_modules[g_nmodules].lo = runtime_base;
        g_modules[g_nmodules].hi = runtime_base + m->size;
        g_nmodules++;
    }
}

static GuestFn module_lookup(const RtModule* m, uint32_t addr)
{
    unsigned lo = 0, hi = m->count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (m->table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < m->count && m->table[lo].addr == addr) ? m->table[lo].fn : NULL;
}

static GuestFn lookup_uncached(uint32_t target)
{
    /* Only addresses inside an image can be translated functions; anything else is native, and
     * must not be mistaken for one after subtracting a relocation delta. */
    if (target >= rt_image_lo && target < rt_image_hi)
        return rt_lookup(target - rt_reloc_delta);
    for (unsigned i = 0; i < g_nmodules; ++i)
        if (target >= g_modules[i].lo && target < g_modules[i].hi)
            return module_lookup(g_modules[i].m, target - *g_modules[i].m->delta);
    return NULL;
}

/* The binary searches above were 5.7% of the game thread on an Xbox One X (every indirect call and
 * virtual method comes through here), so the translations found are cached: per thread (no locking),
 * direct-mapped, and dropped whenever the image or a module is placed (g_lookup_gen). A target with
 * no translation (an import, a thunk) is not cached: it keeps taking the cheap range checks. */
#if defined(_MSC_VER)
#define LOOKUP_TLS __declspec(thread)
#else
#define LOOKUP_TLS _Thread_local
#endif
#define LOOKUP_CACHE 4096u /* entries; a power of two */
typedef struct
{
    uint32_t target, gen;
    GuestFn fn;
} LookupEnt;
static LOOKUP_TLS LookupEnt t_lookup[LOOKUP_CACHE];

GuestFn rt_lookup_any(uint32_t target)
{
    LookupEnt* e = &t_lookup[(target ^ (target >> 12)) & (LOOKUP_CACHE - 1)];
    if (e->target == target && e->gen == g_lookup_gen && e->fn)
        return e->fn;
    GuestFn fn = lookup_uncached(target);
    if (fn)
        e->target = target, e->gen = g_lookup_gen, e->fn = fn;
    return fn;
}

void rt_call_indirect(Guest* g, uint32_t target)
{
    GuestFn fn = rt_lookup_any(target);
    if (fn)
    {
        fn(g);
        return;
    }
    /* Not a translated function: an import (IAT slot holding a host API address) or a bug.
     * The platform layer decides; without one this is fatal. */
    if (g_native && g_native(g, target))
        return;
    rt_fatal(g, target, "indirect call/jump to an address with no translation");
}

/* Nonzero while a guest thread waits for the guest lock; the platform bridge maintains it and
 * installs the yield. Without a bridge (tests/difftest.c) there is one thread and no lock. */
volatile uint32_t rt_lock_contended;
static void (*g_yield)(void);

void rt_set_yield(void (*yield)(void)) { g_yield = yield; }

void rt_safepoint(void)
{
    if (g_yield)
        g_yield();
}

static FILE* g_log;
static RtFatalHook g_fatal_hook;

void rt_set_log(FILE* f) { g_log = f; }
void rt_set_fatal_hook(RtFatalHook h) { g_fatal_hook = h; }

void rt_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

void rt_fatal(Guest* g, uint32_t addr, const char* what)
{
    rt_log("\n[recomp] FATAL at %08x: %s\n", addr, what);
    rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
        g->eax, g->ecx, g->edx, g->ebx, g->esp, g->ebp, g->esi, g->edi);
    /* The guest stack, as return addresses give it: which guest functions led here. */
    rt_log("  guest stack:");
    for (int i = 0; i < 24; ++i)
        rt_log(" %08x", rd32(g->esp + 4u * i));
    rt_log("\n");
    if (g_fatal_hook)
        g_fatal_hook(addr, what);
    abort();
}

/* The CPU the game is told it runs on: the host's CPUID with every SIMD extension removed
 * (MMX, SSE*, 3DNow!), so the game and D3DX take their x87 paths. The 203 functions that use
 * SIMD are not translated; if one is ever reached it traps (RT_UNIMPL), which is the test of
 * this choice. */
void rt_cpuid(Guest* g)
{
#if defined(_M_IX86)
    int r[4];
    uint32_t leaf = g->eax;
    __cpuidex(r, (int)leaf, (int)g->ecx);
    if (leaf == 1)
    {
        r[3] &= ~((1 << 23) | (1 << 25) | (1 << 26)); /* EDX: MMX, SSE, SSE2 */
        r[2] = 0;                                     /* ECX: SSE3 and later */
    }
    else if (leaf == 7)
    {
        r[1] = r[2] = r[3] = 0; /* AVX2, AVX-512 ... */
    }
    else if (leaf == 0x80000001u)
    {
        r[3] &= ~((1u << 31) | (1 << 30) | (1 << 23) | (1 << 22)); /* 3DNow!, 3DNow!+, MMX, MMX+ */
        r[2] = 0;
    }
    g->eax = (uint32_t)r[0];
    g->ebx = (uint32_t)r[1];
    g->ecx = (uint32_t)r[2];
    g->edx = (uint32_t)r[3];
#else
    /* Every other host (R3: x64 and arm64) reports one fixed CPU, the same everywhere: a
     * Pentium III-class GenuineIntel (family 6, model 8) with FPU, TSC, CX8 and CMOV, and no MMX,
     * SSE, FXSR or extended leaves. */
    uint32_t leaf = g->eax;
    g->eax = g->ebx = g->ecx = g->edx = 0;
    if (leaf == 0)
    {
        g->eax = 1;
        g->ebx = 0x756E6547u; /* "Genu" */
        g->edx = 0x49656E69u; /* "ineI" */
        g->ecx = 0x6C65746Eu; /* "ntel" */
    }
    else if (leaf == 1)
    {
        g->eax = 0x00000683u;
        g->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); /* FPU, TSC, CX8, CMOV */
    }
    else if (leaf == 0x80000000u)
        g->eax = 0x80000000u; /* no extended leaves */
#endif
}

void rt_rdtsc(Guest* g)
{
#if defined(_M_IX86)
    unsigned long long t = __rdtsc();
#else
    unsigned long long t = rt_monotonic_ns() * 2; /* a 2 GHz TSC */
#endif
    g->eax = (uint32_t)t;
    g->edx = (uint32_t)(t >> 32);
}
