/* Guest threads, the guest lock, host->guest calls. See gthread.h. */
#include <stdio.h>
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

/* Parallel guest execution (default; XIV_SERIAL=1 turns it off): translated code runs without the
 * guest lock, on as many host threads as the game has; the lock is held only while a shim (host
 * code) runs (thunk_dispatch), and released while a shim calls back into the guest (call). Atomic
 * guest instructions are real atomics (recomp/x86c.py), as are the Interlocked and critical
 * section shims, which run without the lock. */
int gt_parallel = 1;
uint32_t rt_flag_lo, rt_flag_hi; /* guest.h REGS_STORE_CUT (bisecting) */
static RT_TLS uint64_t t_wait_ns; /* this thread's time waiting for the lock (gt_wait_ns) */
static RT_TLS uint64_t t_out_ns, t_released_at; /* and without it, in all (gt_out_ns) */

/* XIV_LOCKLOG: who holds the guest lock, how long, how often (gt_lock_report) */
#define HOLDERS 32
static struct { uint32_t tid; uint64_t ns, n; } g_holders[HOLDERS];
static RT_TLS int t_holder = -1;

/* Each ticket waits on its own slot (ticket % LOCK_SLOTS): an unlock wakes the one thread whose
 * turn it is, not every waiter, and only when that thread is asleep. Waiters spin on the CPU's
 * yield hint for a few microseconds first (most handoffs land within it), never sched_yield: with a
 * dozen guest threads queued, waking them all and their yield storms were most of the process's
 * time in busy scenes. */
#define LOCK_SLOTS 64
static struct
{
    volatile uint32_t gen, sleepers;
    char pad[120];
} g_slot[LOCK_SLOTS] __attribute__((aligned(128)));

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#endif
}

/* Lock statistics and the serial-mode quantum need the clock and shared counters on every lock and
 * unlock: in parallel mode, with shims taking the lock millions of times a second, those clock
 * reads and cache-line round trips were a fifth of the render thread. Only XIV_LOCKLOG/XIV_PROF
 * (or serial mode) pay for them. */
static int g_lockstats = -1;
static inline int lockstats(void)
{
    if (g_lockstats < 0)
        g_lockstats = !gt_parallel || getenv("XIV_LOCKLOG") || getenv("XIV_PROF");
    return g_lockstats;
}

static void gt_unlock_stats(void);
static void gt_unlock_fast(void);

void gt_lock(void)
{
    int stats = lockstats();
    uint64_t t0 = 0;
    if (stats)
    {
        plat_atomic_add32(&rt_lock_contended, 1);
        t0 = rt_monotonic_ns();
    }
    uint32_t t = plat_atomic_add32(&g_next_ticket, 1);
    if (__atomic_load_n(&g_now_serving, __ATOMIC_ACQUIRE) != t)
    {
        /* shim holds are short: spin a few microseconds before sleeping */
        unsigned spin = 0;
        while (spin < 4096 && __atomic_load_n(&g_now_serving, __ATOMIC_ACQUIRE) != t)
            cpu_relax(), spin++;
        if (spin == 4096)
        {
            volatile uint32_t* gen = &g_slot[t % LOCK_SLOTS].gen;
            volatile uint32_t* sleepers = &g_slot[t % LOCK_SLOTS].sleepers;
            for (;;)
            {
                uint32_t gv = __atomic_load_n(gen, __ATOMIC_ACQUIRE);
                if (__atomic_load_n(&g_now_serving, __ATOMIC_ACQUIRE) == t)
                    break;
                __atomic_fetch_add(sleepers, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&g_now_serving, __ATOMIC_SEQ_CST) != t)
                    plat_wait32(gen, gv);
                __atomic_fetch_sub(sleepers, 1, __ATOMIC_SEQ_CST);
            }
        }
    }
    t_held = 1;
    if (stats)
    {
        plat_atomic_add32(&rt_lock_contended, (uint32_t)-1);
        g_acquired_at = rt_monotonic_ns();
        t_wait_ns += g_acquired_at - t0;
        if (t_released_at)
            t_out_ns += g_acquired_at - t_released_at;
    }
}

uint64_t gt_out_ns(void) { return t_out_ns; }

/* Passing the held lock to another host thread without releasing it (a fiber switch: on Windows
 * it never leaves the thread, so it must not queue behind every other thread for the lock). */
static uint64_t g_handoff_quantum;

void gt_handoff_out(void)
{
    g_handoff_quantum = t_quantum_ns;
    t_held = 0;
    t_released_at = rt_monotonic_ns();
}

void gt_handoff_in(void)
{
    t_quantum_ns = g_handoff_quantum;
    t_held = 1;
    /* the holder's quantum runs on from the switch: g_acquired_at is left as it was */
    if (t_released_at)
        t_out_ns += rt_monotonic_ns() - t_released_at;
}

uint64_t gt_wait_ns(void) { return t_wait_ns; }

void gt_unlock(void)
{
    t_held = 0;
    if (lockstats())
        gt_unlock_stats();
    gt_unlock_fast();
}

static void gt_unlock_stats(void)
{
    t_released_at = rt_monotonic_ns();
    if (t_holder < 0)
    {
        uint32_t tid = plat_thread_id();
        for (int i = 0; i < HOLDERS; ++i)
            if (!g_holders[i].tid || g_holders[i].tid == tid)
            {
                g_holders[i].tid = tid;
                t_holder = i;
                break;
            }
    }
    if (t_holder >= 0)
        g_holders[t_holder].ns += t_released_at - g_acquired_at, g_holders[t_holder].n++;
}

static void gt_unlock_fast(void)
{
    uint32_t next = __atomic_add_fetch(&g_now_serving, 1, __ATOMIC_SEQ_CST);
    /* the next ticket's holder: woken only if it went to sleep */
    if (__atomic_load_n(&g_slot[next % LOCK_SLOTS].sleepers, __ATOMIC_SEQ_CST))
    {
        __atomic_fetch_add(&g_slot[next % LOCK_SLOTS].gen, 1, __ATOMIC_SEQ_CST);
        plat_wake_all32(&g_slot[next % LOCK_SLOTS].gen);
    }
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

/* 2 ms: long enough to keep the render thread off the lock's queue most of a frame, short enough
 * that the logic thread (the interface's animations) is not held back a whole tick */
void gt_set_long_quantum(void) { t_quantum_ns = 2000000ull; }

void gt_init(void)
{
    t_quantum_ns = GAME_QUANTUM_NS;
    rt_set_yield(yield_if_due);
}

/* --- threads ----------------------------------------------------------------------------------- */

static uint32_t g_peb, g_tls_start, g_tls_size, g_tls_zero;

/* This thread's copy of the image's static TLS, at ThreadLocalStoragePointer[0]. */
static void give_static_tls(uint32_t teb)
{
    if (!(g_tls_size + g_tls_zero) || rd32(teb + 0x2C))
        return;
    uint32_t block = gheap_alloc(g_tls_size + g_tls_zero, 1), slots = gheap_alloc(64 * 4, 1);
    memcpy(GUEST_PTR(block), GUEST_PTR(g_tls_start), g_tls_size);
    wr32(slots, block);
    wr32(teb + 0x2C, slots);
}

/* Both also apply to the calling thread when its guest state already exists (the host's main
 * thread may have run the runtime's own startup before the image was mapped). */
void gt_set_peb(uint32_t peb)
{
    g_peb = peb;
    if (t_self)
        wr32(t_self->teb + 0x30, peb);
}

void gt_set_static_tls(uint32_t start, uint32_t size, uint32_t zero_fill)
{
    g_tls_start = start;
    g_tls_size = size;
    g_tls_zero = zero_fill;
    if (t_self)
        give_static_tls(t_self->teb);
}

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
    t->tls = t->tls_own;
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
    t->g.mxcsr = 0x1F80; /* SSE: all exceptions masked, round to nearest */
    wr32(teb + 0x30, g_peb);
    give_static_tls(teb);
    t_self = t;
    return t;
}

void gt_adopt(GThread* t) { t_self = t; }

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

static uint32_t call(uint32_t fn, int regs, uint32_t ecx, uint32_t edx, unsigned nargs, const uint32_t* args,
    uint32_t* edx_out, double* st0_out);

uint32_t guest_call(uint32_t fn, unsigned nargs, const uint32_t* args)
{
    return call(fn, 0, 0, 0, nargs, args, NULL, NULL);
}

uint32_t guest_thiscall(uint32_t fn, uint32_t self, unsigned nargs, const uint32_t* args)
{
    return call(fn, 1, self, 0, nargs, args, NULL, NULL);
}

uint32_t guest_call_full(uint32_t fn, int regs, uint32_t ecx, uint32_t edx, unsigned nargs, const uint32_t* args,
    uint32_t* edx_out, double* st0_out)
{
    return call(fn, regs, ecx, edx, nargs, args, edx_out, st0_out);
}

static uint32_t call(uint32_t fn, int regs, uint32_t ecx, uint32_t edx, unsigned nargs, const uint32_t* args,
    uint32_t* edx_out, double* st0_out)
{
    int took = !t_held;
    if (took)
        gt_lock();
    GThread* t = gt_self();
    /* parallel: the guest runs without the lock (a shim calling back in lets go of it meanwhile) */
    int let_go = gt_parallel;
    if (let_go)
        gt_unlock();
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
    if (regs >= 1)
        g->ecx = ecx;
    if (regs >= 2)
        g->edx = edx;
    /* any guest function pointer: a translation, or a thunk to a shim (a method of our own gamecore
     * object, say) - exactly as guest code's own indirect calls resolve */
    rt_call_indirect(g, fn);
    if (let_go)
        gt_lock();
    uint32_t eax = g->eax;
    if (edx_out)
        *edx_out = g->edx;
    if (st0_out)
        *st0_out = g->st[g->top & 7u];
    uint16_t fcw = g->fcw;
    *g = saved;
    g->fcw = fcw;
    if (took)
        gt_unlock();
    return eax;
}

void gt_lock_report(uint64_t window_ns)
{
    char line[1024];
    int o = snprintf(line, sizeof line, "[lock] holders:");
    for (int i = 0; i < HOLDERS && g_holders[i].tid; ++i)
    {
        if (g_holders[i].n && o < (int)sizeof line - 64)
            o += snprintf(line + o, sizeof line - (size_t)o, " %u:%.0f%%/%llu", g_holders[i].tid,
                          100.0 * g_holders[i].ns / (double)window_ns, (unsigned long long)g_holders[i].n);
        g_holders[i].ns = g_holders[i].n = 0;
    }
    rt_log("%s\n", line);
}
