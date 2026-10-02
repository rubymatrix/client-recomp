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

void xiv_ui_init(void)
{
    rt_wrap_key_repeat = w_key_repeat;
    if (getenv("XIV_ANIMLOG"))
    {
#define ANIM(n, a) rt_wrap_ui_##n = w_##n;
#include "xiv_ui_wraps.h"
#undef ANIM
    }
}
