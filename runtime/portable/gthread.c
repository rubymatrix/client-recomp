/* Guest threads, the guest lock, host->guest calls. See gthread.h. */
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "plat.h"

static RT_TLS GThread* t_self;
static RT_TLS int t_held;
static RT_TLS int t_noyield;

/* --- the guest lock: a FIFO ticket lock with a quantum (bridge.c has the history) ----------- */
static volatile uint32_t g_next_ticket, g_now_serving;
static uint64_t g_acquired_at;
/* How long a thread keeps the lock while others wait. The game thread (the one that called gt_init,
 * which runs GameStart) keeps it longest: each time it yields it waits out every other waiting
 * thread's turn, which on slow cores (an Xbox One's) was 15% of its time with equal turns. */
#define QUANTUM_NS 1000000ull
#define GAME_QUANTUM_NS 8000000ull
static RT_TLS uint64_t t_quantum_ns;

void gt_lock(void)
{
    plat_atomic_add32(&rt_lock_contended, 1);
    uint32_t t = plat_atomic_add32(&g_next_ticket, 1);
    for (unsigned spin = 0;; ++spin)
    {
        uint32_t cur = g_now_serving;
        if (cur == t)
            break;
        if (spin < 128)
            plat_yield();
        else
            plat_wait32(&g_now_serving, cur);
    }
    plat_atomic_add32(&rt_lock_contended, (uint32_t)-1);
    t_held = 1;
    g_acquired_at = rt_monotonic_ns();
}

void gt_unlock(void)
{
    t_held = 0;
    plat_atomic_add32(&g_now_serving, 1);
    if (rt_lock_contended)
        plat_wake_all32(&g_now_serving);
}

int gt_holds(void)
{
    return t_held;
}

void gt_noyield(int on) { t_noyield += on ? 1 : -1; }

/* Called at every loop's back edge. The clock is read only every 32nd call while another thread
 * waits: reading it at each was 8% of the game thread's time where the clock is slow (a virtual
 * machine's QueryPerformanceCounter), and 32 back edges are far shorter than the quantum. */
static void yield_if_due(void)
{
    static RT_TLS unsigned t_skip;
    if (!t_held || t_noyield || !rt_lock_contended || (++t_skip & 31u) ||
        rt_monotonic_ns() - g_acquired_at < (t_quantum_ns ? t_quantum_ns : QUANTUM_NS))
        return;
    gt_unlock();
    gt_lock();
}

void gt_init(void)
{
    t_quantum_ns = GAME_QUANTUM_NS;
    rt_set_yield(yield_if_due);
}

/* --- threads ----------------------------------------------------------------------------------- */

GThread* gt_self(void)
{
    if (t_self)
        return t_self;
    GThread* t = (GThread*)calloc(1, sizeof *t);
    uint32_t stack = gwin_alloc(GT_STACK_SIZE);
    uint32_t teb = gheap_alloc(0x1000, 1);
    if (!t || !stack || !teb)
    {
        rt_log("[recomp] out of memory creating a guest thread\n");
        abort();
    }
    t->stack_lo = stack;
    t->stack_hi = stack + GT_STACK_SIZE;
    t->teb = teb;
    t->tid = plat_thread_id();
    wr32(teb + 0x00, 0xFFFFFFFFu); /* SEH chain: empty */
    wr32(teb + 0x04, t->stack_hi); /* stack base */
    wr32(teb + 0x08, t->stack_lo); /* stack limit */
    wr32(teb + 0x18, teb);         /* self */
    wr32(teb + 0x20, 0x1000);      /* process id, as the guest sees it */
    wr32(teb + 0x24, t->tid);      /* thread id */
    t->g.esp = t->stack_hi - 64;
    t->g.fs_base = teb;
    t->g.fcw = 0x027F; /* the Win32 default: 53-bit precision, round to nearest, all masked */
    t_self = t;
    return t;
}

void gt_exit_self(void)
{
    GThread* t = t_self;
    if (!t)
        return;
    int took = !t_held;
    if (took)
        gt_lock();
    gwin_release(t->stack_lo);
    gheap_free(t->teb);
    t_self = NULL;
    free(t);
    if (took)
        gt_unlock();
}

void gt_set_error(uint32_t e)
{
    wr32(gt_self()->teb + 0x34, e);
}

uint32_t gt_get_error(void)
{
    return rd32(gt_self()->teb + 0x34);
}

/* --- host -> guest ----------------------------------------------------------------------------- */

static uint32_t call(uint32_t fn, int thiscall, uint32_t self, unsigned nargs, const uint32_t* args);

uint32_t guest_call(uint32_t fn, unsigned nargs, const uint32_t* args)
{
    return call(fn, 0, 0, nargs, args);
}

uint32_t guest_thiscall(uint32_t fn, uint32_t self, unsigned nargs, const uint32_t* args)
{
    return call(fn, 1, self, nargs, args);
}

static uint32_t call(uint32_t fn, int thiscall, uint32_t self, unsigned nargs, const uint32_t* args)
{
    int took = !t_held;
    if (took)
        gt_lock();
    GThread* t = gt_self();
    Guest* g = &t->g;
    /* A call nested inside a shim (a window procedure, say) builds below the suspended call's
     * stack pointer; the whole Guest is restored afterwards except the x87 control word, which
     * is per-thread state the callee may legitimately change. */
    Guest saved = *g;
    uint32_t sp = g->esp - 4u * nargs;
    for (unsigned i = 0; i < nargs; ++i)
        wr32(sp + 4u * i, args[i]);
    sp -= 4;
    wr32(sp, 0xFEEDF00Du); /* return address: never used, the translation returns to us */
    g->esp = sp;
    g->df = 0;
    if (thiscall)
        g->ecx = self;
    /* any guest function pointer: a translation, or a thunk to a shim (a method of our own gamecore
     * object, say) - exactly as guest code's own indirect calls resolve */
    rt_call_indirect(g, fn);
    uint32_t eax = g->eax;
    uint16_t fcw = g->fcw;
    *g = saved;
    g->fcw = fcw;
    if (took)
        gt_unlock();
    return eax;
}
