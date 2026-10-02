/* The guest window: pages and heap. See gwin.h. */
#include <stdlib.h>
#include <string.h>

#include "gwin.h"
#include "plat.h"
#include "runtime.h"

unsigned char* rt_guest_base;

#define GRANULE 0x10000u
#define GRANULES 0x10000u /* 4 GB / 64 KB */
#define PAGE 0x1000u
#define LOW_RESERVED 0x00100000u   /* never handed out: null pointer region */
#define HIGH_RESERVED 0xFFF00000u  /* never handed out: the thunk range and the top */

static uint32_t g_res[GRANULES];   /* granules in the reservation starting here, else 0 */
static uint8_t g_used[GRANULES];   /* granule belongs to a reservation */
static uint8_t g_committed[GRANULES * (GRANULE / PAGE) / 8];
/* pages mapped read-write on the host: a decommit only zeroes them (Windows hands committed pages
 * back zeroed), so the game's allocator committing and decommitting the same pages all the time
 * costs no system calls and no fresh page faults */
static uint8_t g_mapped[GRANULES * (GRANULE / PAGE) / 8];
#define BIT(m, p) (((m)[(p) >> 3] >> ((p) & 7)) & 1)
static volatile uint32_t g_lock;
static uint32_t g_hint = LOW_RESERVED / GRANULE;

static void lock(void)
{
    while (plat_atomic_cas32(&g_lock, 0, 1) != 0)
        plat_yield();
}

static void unlock(void)
{
    plat_atomic_cas32(&g_lock, 1, 0);
}

int gwin_init(void)
{
    /* 4 GB plus a guard page: an unaligned access at 0xFFFFFFFF reaches past the window. */
#if defined(RT_GUEST_FIXED)
    rt_guest_base = (unsigned char*)plat_reserve_at((void*)(uintptr_t)(RT_GUEST_FIXED), 0x100000000ull + PAGE);
    if (rt_guest_base != (unsigned char*)(uintptr_t)(RT_GUEST_FIXED))
    {
        rt_log("[recomp] the guest window could not be placed at %#llx (the build expects it there)\n",
               (unsigned long long)(RT_GUEST_FIXED));
        return 0;
    }
#else
    rt_guest_base = (unsigned char*)plat_reserve(0x100000000ull + PAGE);
#endif
    if (!rt_guest_base)
        return 0;
    for (uint32_t g = 0; g < LOW_RESERVED / GRANULE; ++g)
        g_used[g] = 1;
    for (uint32_t g = HIGH_RESERVED / GRANULE; g < GRANULES; ++g)
        g_used[g] = 1;
    return 1;
}

static uint32_t granules_for(uint32_t size)
{
    return (uint32_t)(((uint64_t)size + GRANULE - 1) / GRANULE);
}

uint32_t gwin_reserve(uint32_t addr, uint32_t size)
{
    uint32_t n = granules_for(size);
    if (!n)
        return 0;
    lock();
    uint32_t start = 0;
    if (addr)
    {
        start = addr / GRANULE;
        for (uint32_t g = start; g < start + n; ++g)
            if (g >= GRANULES || g_used[g])
            {
                unlock();
                return 0;
            }
    }
    else
    {
        /* first fit from the hint, then from the bottom */
        for (int pass = 0; pass < 2 && !start; ++pass)
        {
            uint32_t run = 0;
            for (uint32_t g = pass ? LOW_RESERVED / GRANULE : g_hint; g < GRANULES; ++g)
            {
                run = g_used[g] ? 0 : run + 1;
                if (run == n)
                {
                    start = g + 1 - n;
                    break;
                }
            }
        }
        if (!start)
        {
            unlock();
            return 0;
        }
        g_hint = start + n;
    }
    for (uint32_t g = start; g < start + n; ++g)
        g_used[g] = 1;
    g_res[start] = n;
    unlock();
    return start * GRANULE;
}

int gwin_commit(uint32_t addr, uint32_t size)
{
    uint32_t lo = addr & ~(PAGE - 1), hi = (uint32_t)(((uint64_t)addr + size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    int mapped = 1;
    for (uint32_t p = lo / PAGE; p < hi / PAGE && mapped; ++p)
        mapped = BIT(g_mapped, p);
    if (!mapped && !plat_commit(rt_guest_base + lo, (size_t)hi - lo))
        return 0;
    lock();
    for (uint32_t p = lo / PAGE; p < hi / PAGE; ++p)
    {
        g_committed[p >> 3] |= (uint8_t)(1u << (p & 7));
        g_mapped[p >> 3] |= (uint8_t)(1u << (p & 7));
    }
    unlock();
    return 1;
}

void gwin_decommit(uint32_t addr, uint32_t size)
{
    uint32_t lo = addr & ~(PAGE - 1), hi = (uint32_t)(((uint64_t)addr + size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    lock();
    for (uint32_t p = lo / PAGE; p < hi / PAGE; ++p)
    {
        if (BIT(g_committed, p) && BIT(g_mapped, p))
            memset(rt_guest_base + (size_t)p * PAGE, 0, PAGE);
        g_committed[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
    unlock();
}

/* a release gives the pages back to the host */
static void unmap_range(uint32_t lo, uint32_t hi)
{
    plat_decommit(rt_guest_base + lo, (size_t)hi - lo);
    lock();
    for (uint32_t p = lo / PAGE; p < hi / PAGE; ++p)
    {
        g_committed[p >> 3] &= (uint8_t)~(1u << (p & 7));
        g_mapped[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
    unlock();
}

void gwin_release(uint32_t addr)
{
    uint32_t start = addr / GRANULE, n = g_res[start];
    if (!n)
        return;
    unmap_range(start * GRANULE, start * GRANULE + n * GRANULE);
    lock();
    for (uint32_t g = start; g < start + n; ++g)
        g_used[g] = 0;
    g_res[start] = 0;
    if (start < g_hint)
        g_hint = start;
    unlock();
}

uint32_t gwin_alloc(uint32_t size)
{
    uint32_t a = gwin_reserve(0, size);
    if (a && !gwin_commit(a, size))
    {
        gwin_release(a);
        return 0;
    }
    return a;
}

int gwin_is_committed(uint32_t addr)
{
    uint32_t p = addr / PAGE;
    return (g_committed[p >> 3] >> (p & 7)) & 1;
}

/* --- heap -------------------------------------------------------------------------------------
 *
 * Each block has an 8-byte header just below it: the requested size, and a tag with the size
 * class (CLASS_LARGE for blocks that own their own reservation). Small blocks are carved from
 * 256 KB runs per class and recycled through a per-class free list threaded through the blocks.
 * Blocks are 8-byte aligned, as the Win32 heap guarantees on x86. */
#define TAG 0x48500000u
#define CLASS_LARGE 0xFFFFu
#define RUN 0x40000u

static const uint32_t CLASS_SIZE[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
    384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768,
};
#define CLASSES (sizeof CLASS_SIZE / sizeof CLASS_SIZE[0])

static uint32_t g_free[CLASSES], g_bump[CLASSES], g_bump_end[CLASSES];

static unsigned class_for(uint32_t total)
{
    for (unsigned c = 0; c < CLASSES; ++c)
        if (total <= CLASS_SIZE[c])
            return c;
    return CLASS_LARGE;
}

uint32_t gheap_alloc(uint32_t size, int zero)
{
    uint64_t total = (uint64_t)size + 8;
    if (total > 0xFFF00000u)
        return 0;
    unsigned c = class_for((uint32_t)total);
    uint32_t h;
    if (c == CLASS_LARGE)
    {
        h = gwin_alloc((uint32_t)total);
        if (!h)
            return 0;
    }
    else if (g_free[c])
    {
        h = g_free[c];
        g_free[c] = rd32(h + 8);
    }
    else
    {
        if (g_bump[c] + CLASS_SIZE[c] > g_bump_end[c])
        {
            uint32_t run = gwin_alloc(RUN);
            if (!run)
                return 0;
            g_bump[c] = run;
            g_bump_end[c] = run + RUN;
        }
        h = g_bump[c];
        g_bump[c] += CLASS_SIZE[c];
    }
    wr32(h, size);
    wr32(h + 4, TAG | c);
    if (zero)
        memset(GUEST_PTR(h + 8), 0, size);
    return h + 8;
}

static int live(uint32_t p)
{
    return p >= 8 && gwin_is_committed(p - 8) && (rd32(p - 4) & 0xFFFF0000u) == TAG;
}

void gheap_free(uint32_t p)
{
    if (!p || !live(p))
        return;
    uint32_t h = p - 8;
    unsigned c = rd32(h + 4) & 0xFFFFu;
    wr32(h + 4, 0); /* no longer live: a double free is ignored */
    if (c == CLASS_LARGE)
        gwin_release(h);
    else
    {
        wr32(h + 8, g_free[c]);
        g_free[c] = h;
    }
}

uint32_t gheap_size(uint32_t p)
{
    return live(p) ? rd32(p - 8) : ~0u;
}

uint32_t gheap_realloc(uint32_t p, uint32_t size, int zero, int in_place_only)
{
    if (!p)
        return in_place_only ? 0 : gheap_alloc(size, zero);
    if (!live(p))
        return 0;
    uint32_t old = rd32(p - 8);
    unsigned c = rd32(p - 4) & 0xFFFFu;
    uint64_t capacity = c == CLASS_LARGE ? (uint64_t)g_res[(p - 8) / GRANULE] * GRANULE : CLASS_SIZE[c];
    if ((uint64_t)size + 8 <= capacity)
    {
        if (c == CLASS_LARGE && !gwin_commit(p - 8, size + 8)) /* the reservation's tail may not be committed yet */
            return 0;
        if (zero && size > old)
            memset(GUEST_PTR(p + old), 0, size - old);
        wr32(p - 8, size);
        return p;
    }
    if (in_place_only)
        return 0;
    uint32_t q = gheap_alloc(size, zero);
    if (!q)
        return 0;
    memcpy(GUEST_PTR(q), GUEST_PTR(p), old < size ? old : size);
    gheap_free(p);
    return q;
}

uint32_t gheap_strdup(const char* s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = gheap_alloc(n, 0);
    if (p)
        memcpy(GUEST_PTR(p), s, n);
    return p;
}
