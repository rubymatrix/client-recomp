/* FINAL FANTASY XIV's interface (Sqwt, a Silverlight-like toolkit): hooks on its animation code.
 *
 * XIV_ANIMLOG=1 counts calls of the Timeline family's vtable methods (xiv_ui_wraps.h), every 2 s,
 * to find the ones that run each animation tick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "gthread.h"
#include "plat.h"
#include "runtime.h"

/* XIV_ANIMLOG=1: calls of each watched Timeline method per 2 s (xiv_ui_wraps.h) */
static unsigned g_seen;
/* the first calls of a method: where from, with what (this, first argument, the int64 it points at + 8) */
static void seen(Guest* g, uint32_t a)
{
    /* XIV_PROF=logic: the shim profile follows the thread running the logic loop's limiter */
    if (a == 0xcc6d00)
    {
        static int done;
        const char* p = getenv("XIV_PROF");
        if (!done && p && !strcmp(p, "logic"))
        {
            extern uint32_t thunk_prof_thread;
            thunk_prof_thread = plat_thread_id();
            done = 1;
        }
    }
    if (g_seen >= 300 || a != 0x9ac820)
        return;
    g_seen++;
    uint32_t p = rt_arg(g, 0);
    rt_log("[ui] %x from %08x this %08x arg %08x [arg+8]=%08x%08x host %.3f ms thread %u\n", a, rd32(g->esp), g->ecx, p,
           p > 0x10000 ? rd32(p + 12) : 0, p > 0x10000 ? rd32(p + 8) : 0, rt_monotonic_ns() / 1e6, plat_thread_id());
}
#define ANIM(n, a) static uint32_t c_##n; static void w_##n(Guest* g) { c_##n++; seen(g, a); rt_orig_ui_##n(g); }
#include "xiv_ui_wraps.h"
#undef ANIM

void xiv_ui_report(void)
{
    static uint64_t last;
    static int on = -1;
    if (on < 0)
        on = getenv("XIV_ANIMLOG") != NULL;
    if (!on)
        return;
    uint64_t now = rt_monotonic_ns();
    if (now - last < 2000000000ull)
        return;
    last = now;
    char line[2048];
    int o = snprintf(line, sizeof line, "[ui] calls/2s:");
#define ANIM(n, a) if (c_##n && o < (int)sizeof line - 40) o += snprintf(line + o, sizeof line - (size_t)o, " %x:%u", a, c_##n); c_##n = 0;
#include "xiv_ui_wraps.h"
#undef ANIM
    rt_log("%s\n", line);
}

/* The keyboard's auto-repeat (FUN_00d429b0, thiscall (flags, held, repeat), on the input object's
 * {vtable, count i16, _, counters u8*, delay u8, rearm u8}): once per frame it counts each held key
 * (flags[i] & held) and sets `repeat` when the count passes `delay` (8), then rearms it at 7 - the
 * timing of a 30 fps game, twice as fast at 60. Here the count advances at 30 Hz whatever the frame
 * rate: the first repeat after 0.3 s, then 15 a second. */
#define REPEAT_TICK_NS 33333333ull
static void w_key_repeat(Guest* g)
{
    static uint64_t last, acc;
    uint32_t self = g->ecx, flags = rt_arg(g, 0), held = rt_arg(g, 1), repeat = rt_arg(g, 2);
    uint64_t now = rt_monotonic_ns();
    acc += last ? now - last : REPEAT_TICK_NS;
    last = now;
    unsigned ticks = (unsigned)(acc / REPEAT_TICK_NS);
    acc -= ticks * REPEAT_TICK_NS;
    if (ticks > 4)
        ticks = 4;
    int16_t n = (int16_t)rd16(self + 4);
    uint32_t counters = rd32(self + 8);
    uint8_t delay = rd8(self + 0xc), rearm = rd8(self + 0xd);
    for (int i = 0; i < n; ++i)
    {
        uint32_t f = flags + 4u * (uint32_t)i;
        if (!(rd32(f) & held))
        {
            wr8(counters + (uint32_t)i, 0);
            continue;
        }
        uint8_t c = rd8(counters + (uint32_t)i);
        for (unsigned t = 0; t < ticks; ++t)
            if (++c > delay)
            {
                wr32(f, rd32(f) | repeat);
                c = rearm;
            }
        wr8(counters + (uint32_t)i, c);
    }
    rt_return(g, 0, 12);
}

/* The gamepad's auto-repeat (FUN_00d42940, thiscall (held, row) -> repeat bits; also the mouse's)
 * has the same frame count: Rapture's tick limiter asks for 30 Hz but measures from before its own
 * sleep, so with exact sleeps the tick runs at ~60 Hz and held d-pad/stick directions repeated after
 * 0.15 s, then 30 a second. The pad's call (from FUN_00d32a30) counts in time instead, one count per
 * 1/60 s x XIV_PAD_REPEAT_SCALE (default 2: the 30 Hz tick the counter's delay/reset values were
 * made for, as the keyboard gets: first repeat after 0.3 s, then 15 a second). The mouse's call
 * keeps the frame count. */
#define PAD_REPEAT_CALLER 0xd32d2fu
static void w_pad_repeat(Guest* g)
{
    if (rd32(g->esp) != PAD_REPEAT_CALLER)
    {
        rt_orig_pad_repeat(g);
        return;
    }
    static uint64_t tick_ns;
    if (!tick_ns)
    {
        const char* e = getenv("XIV_PAD_REPEAT_SCALE");
        double s = e ? atof(e) : 0;
        tick_ns = (uint64_t)(1e9 / 60 * (s > 0.05 && s < 20 ? s : 2.0));
    }
    static struct
    {
        uint32_t self, row;
        uint64_t last, acc;
    } slot[8];
    uint32_t self = g->ecx, held = rt_arg(g, 0), row = rt_arg(g, 1);
    int k = 0;
    while (k < 7 && slot[k].self && (slot[k].self != self || slot[k].row != row))
        ++k;
    slot[k].self = self, slot[k].row = row;
    uint64_t now = rt_monotonic_ns();
    slot[k].acc += slot[k].last ? now - slot[k].last : tick_ns;
    slot[k].last = now;
    unsigned ticks = (unsigned)(slot[k].acc / tick_ns);
    slot[k].acc -= ticks * tick_ns;
    if (ticks > 4)
        ticks = 4;
    int16_t n = (int16_t)rd16(self + 4);
    uint32_t counters = rd32(self + 8) + (uint32_t)n * row, out = 0;
    uint8_t delay = rd8(self + 0xc), rearm = rd8(self + 0xd);
    for (int i = 0; i < n && i < 32; ++i)
    {
        if (!(held & (1u << i)))
        {
            wr8(counters + (uint32_t)i, 0);
            continue;
        }
        uint8_t c = rd8(counters + (uint32_t)i);
        for (unsigned t = 0; t < ticks; ++t)
            if (++c > delay)
            {
                out |= 1u << i;
                c = rearm;
            }
        wr8(counters + (uint32_t)i, c);
    }
    rt_return(g, out, 8);
}

/* Sqwt Window arrange (FUN_00925490, thiscall (rect)). On a window's first arrange it centres it
 * with its Margin and keeps the offset as Left. The NPC talk window (NpcSayWidget.form: Center/Bottom,
 * Margin 238,0,0,64) therefore sits 119 units right of centre, as in retail. For that first arrange
 * its right margin is made equal to its left, so the box lands centred; the margin is put back after,
 * and later arranges (Left + left margin) keep it there. XIV_RETAIL_TALK=1 keeps retail's place. */
static void w_window_arrange(Guest* g)
{
    static int retail = -1;
    if (retail < 0)
        retail = getenv("XIV_RETAIL_TALK") != NULL;
    uint32_t w = g->ecx;
    if (getenv("XIV_ARRANGELOG") && (rd8(w + 0x444) & 4))
        rt_log("[ui] first arrange %08x vt %08x margin %g,%g,%g,%g align %u,%u startup %u,%u left %g\n", w, rd32(w),
               rdf32(w + 0x198), rdf32(w + 0x19c), rdf32(w + 0x1a0), rdf32(w + 0x1a4), rd8(w + 0x24c), rd8(w + 0x24e),
               rd8(w + 0x3c9), rd8(w + 0x3cb), rdf32(w + 0x37c));
    if (retail || !(rd8(w + 0x444) & 4) || rdf32(w + 0x198) != 238.0f || rdf32(w + 0x19c) != 0.0f ||
        rdf32(w + 0x1a0) != 0.0f || rdf32(w + 0x1a4) != 64.0f)
    {
        rt_orig_window_arrange(g);
        return;
    }
    wr32(w + 0x1a0, rd32(w + 0x198));
    rt_orig_window_arrange(g);
    wr32(w + 0x1a0, 0);
    if (getenv("XIV_ARRANGELOG"))
        rt_log("[ui] talk window %08x centred: left %g top %g\n", w, rdf32(w + 0x37c), rdf32(w + 0x384));
}

void xiv_ui_init(void)
{
    rt_wrap_key_repeat = w_key_repeat;
    rt_wrap_pad_repeat = w_pad_repeat;
    rt_wrap_window_arrange = w_window_arrange;
    if (getenv("XIV_ANIMLOG"))
    {
#define ANIM(n, a) rt_wrap_ui_##n = w_##n;
#include "xiv_ui_wraps.h"
#undef ANIM
    }
}
