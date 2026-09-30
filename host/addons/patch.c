/* Code patches: addons that write into FFXiMain's code (Ashita's libs/memory/patch.lua, ffi pointers,
 * write_array over a pattern's address).
 *
 * The bytes land in guest memory, but the recompiled C is what runs, so by themselves they change
 * nothing. Every patch is reported in one machine-readable line,
 *
 *   [addons] codepatch <addon> at=<addr> fn=<entry> old=<hex> new=<hex>
 *
 * so the known ones can be given translated variants (the function with the patch applied, chosen
 * while the patched bytes are there). Writes through xi.memory are reported as they happen, with
 * the addon that made them; anything else (a raw ffi store) is found by comparing the code with a
 * snapshot every half second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static uint8_t* g_snap; /* the code as last seen */
static uint32_t g_text, g_size;
static uint32_t g_pending_lo = 0xFFFFFFFFu, g_pending_hi; /* written through xi.memory: variants to check */

static void snapshot(void)
{
    if (g_snap)
        return;
    xi_image(NULL, NULL, &g_text, &g_size);
    if (!g_size || !xi_mapped(g_text, g_size))
        return;
    g_snap = (uint8_t*)malloc(g_size);
    if (g_snap)
        memcpy(g_snap, GUEST_PTR(g_text), g_size);
}

/* The translated function whose entry is the nearest at or below a code address. */
static uint32_t function_of(uint32_t a)
{
    uint32_t best = 0;
    unsigned lo = 0, hi = rt_table_count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (rt_table[mid].addr <= a)
            best = rt_table[mid].addr, lo = mid + 1;
        else
            hi = mid;
    }
    return best;
}

/* The groups whose bytes are all in place select their translated variants; the rest don't. */
static void update_variants(uint32_t lo, uint32_t hi)
{
    for (unsigned i = 0; i < rt_patch_count; ++i)
    {
        const RtPatch* p = &rt_patches[i];
        if (!p->group || p->addr + p->size <= lo || p->addr >= hi)
            continue;
        int all = 1;
        for (unsigned j = 0; j < rt_patch_count && all; ++j)
        {
            const RtPatch* q = &rt_patches[j];
            if (q->group && !strcmp(q->group, p->group) && memcmp(GUEST_PTR(q->addr), q->bytes, q->size))
                all = 0;
        }
        if (*p->on != all)
        {
            *p->on = all;
            xi_log("code patch %s %s: its translated variant %s", p->group, all ? "in place" : "gone",
                all ? "runs" : "no longer runs");
        }
    }
}

static void report(const char* who, uint32_t a, const uint8_t* old, const uint8_t* now, uint32_t n)
{
    char oh[2 * 64 + 4], nh[2 * 64 + 4];
    uint32_t k = n > 64 ? 64 : n;
    for (uint32_t i = 0; i < k; ++i)
    {
        snprintf(oh + 2 * i, 3, "%02x", old[i]);
        snprintf(nh + 2 * i, 3, "%02x", now[i]);
    }
    if (n > k)
        strcat(oh, ".."), strcat(nh, "..");
    xi_log("codepatch %s at=%08x fn=%08x old=%s new=%s", who, a, function_of(a), k ? oh : "", k ? nh : "");
}

void xi_code_patch(Addon* a, uint32_t addr, const uint8_t* bytes, uint32_t n)
{
    snapshot();
    if (!g_snap || !n || addr + n <= g_text || addr >= g_text + g_size)
        return;
    uint32_t lo = addr < g_text ? g_text : addr, hi = addr + n > g_text + g_size ? g_text + g_size : addr + n;
    const uint8_t* old = GUEST_PTR(lo);
    if (!memcmp(old, bytes + (lo - addr), hi - lo))
        return; /* the same bytes again */
    report(a ? a->name : "?", lo, old, bytes + (lo - addr), hi - lo);
    /* the snapshot follows, so the watcher doesn't report it a second time */
    memcpy(g_snap + (lo - g_text), bytes + (lo - addr), hi - lo);
    g_pending_lo = lo < g_pending_lo ? lo : g_pending_lo;
    g_pending_hi = hi > g_pending_hi ? hi : g_pending_hi;
}

void xi_patch_watch(void)
{
    static unsigned frame;
    snapshot(); /* the first frame, before any addon has run */
    if (g_pending_hi)
    {
        /* after the write (xi_code_patch runs before it): the variants the new bytes select */
        update_variants(g_pending_lo, g_pending_hi);
        g_pending_lo = 0xFFFFFFFFu, g_pending_hi = 0;
    }
    if (++frame % 30)
        return;
    if (!g_snap)
        return;
    const uint8_t* now = GUEST_PTR(g_text);
    if (!memcmp(now, g_snap, g_size))
        return;
    for (uint32_t i = 0; i < g_size;)
    {
        if (now[i] == g_snap[i])
        {
            ++i;
            continue;
        }
        uint32_t j = i;
        while (j < g_size && (now[j] != g_snap[j] || (j + 1 < g_size && now[j + 1] != g_snap[j + 1])))
            ++j;
        report("?", g_text + i, g_snap + i, now + i, j - i);
        memcpy(g_snap + i, now + i, j - i);
        update_variants(g_text + i, g_text + j);
        i = j;
    }
}
