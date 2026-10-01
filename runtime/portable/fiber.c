/* Win32 fibers (kernel32 ConvertThreadToFiber, CreateFiber(Ex), SwitchToFiber, DeleteFiber) for
 * FINAL FANTASY XIV's job system.
 *
 * Translated code runs on the host stack as well as the guest's, so a fiber cannot be a stack
 * switch: each fiber is a host thread of its own, with its own guest registers and guest stack,
 * that runs only while it is the current fiber. SwitchToFiber wakes the target and parks the
 * caller (the guest lock passes between them), so for the game exactly one fiber of a thread
 * runs at a time, as on Windows.
 *
 * A fiber keeps the identity of the thread it runs for: its TEB (fs:), TLS slots and thread id.
 * Like Windows, a switch saves the leaving fiber's exception list and stack bounds from the TEB
 * and loads the target's, and keeps the current fiber at TEB+0x10 (GetCurrentFiber; the fiber
 * object's first dword is its parameter, GetFiberData).
 */
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"

#define FIBER_MAGIC 0x52424946u /* "FIBR" */
#define MAX_FIBERS 1024

typedef struct Fiber
{
    uint32_t guest;          /* the fiber object: [0] parameter, [4] magic, [8] our index */
    uint32_t start;          /* LPFIBER_START_ROUTINE, 0 for a converted thread */
    GThread* gt;             /* its guest state */
    volatile uint32_t wake;  /* 1: it is the current fiber and may run */
    int started, deleted;
    uint32_t seh, stack_base, stack_limit; /* its TEB fields while it is not running */
} Fiber;

static Fiber* g_fibers[MAX_FIBERS];

static Fiber* fiber(uint32_t p)
{
    if (!p || rd32(p + 4) != FIBER_MAGIC)
        return NULL;
    uint32_t i = rd32(p + 8);
    return i < MAX_FIBERS && g_fibers[i] && g_fibers[i]->guest == p ? g_fibers[i] : NULL;
}

static Fiber* fiber_new(uint32_t param, uint32_t start)
{
    for (uint32_t i = 1; i < MAX_FIBERS; ++i)
        if (!g_fibers[i])
        {
            Fiber* f = (Fiber*)calloc(1, sizeof *f);
            f->guest = gheap_alloc(16, 1);
            wr32(f->guest, param);
            wr32(f->guest + 4, FIBER_MAGIC);
            wr32(f->guest + 8, i);
            f->start = start;
            g_fibers[i] = f;
            return f;
        }
    rt_log("[recomp] out of fibers\n");
    return NULL;
}

static void fiber_main(void* arg)
{
    Fiber* f = (Fiber*)arg;
    gt_adopt(f->gt);
    while (!f->wake)
        plat_wait32(&f->wake, 0);
    gt_lock();
    uint32_t a[1] = { rd32(f->guest) };
    guest_call(f->start, 1, a);
    /* a fiber routine that returns ends its thread (ExitThread): here, nothing runs it again */
    rt_log("[recomp] fiber %08x returned: its thread is left parked\n", f->guest);
    gt_unlock();
    for (;;)
        plat_sleep_ms(1000);
}

/* ConvertThreadToFiber(param) / ConvertThreadToFiberEx(param, flags) */
static uint32_t convert(uint32_t param)
{
    GThread* t = gt_self();
    uint32_t cur = rd32(t->teb + 0x10);
    if (fiber(cur))
        return cur; /* already a fiber */
    Fiber* f = fiber_new(param, 0);
    if (!f)
        return 0;
    f->gt = t;
    f->started = 1;
    f->wake = 1;
    wr32(t->teb + 0x10, f->guest);
    return f->guest;
}
static void sh_ConvertThreadToFiber(Guest* g) { RET(convert(ARG(0)), 1); }
static void sh_ConvertThreadToFiberEx(Guest* g) { RET(convert(ARG(0)), 2); }
static void sh_ConvertFiberToThread(Guest* g)
{
    wr32(gt_self()->teb + 0x10, 0);
    RET(1, 0);
}

/* CreateFiberEx(commit, reserve, flags, start, param) / CreateFiber(stack, start, param) */
static uint32_t create(uint32_t reserve, uint32_t start, uint32_t param)
{
    Fiber* f = fiber_new(param, start);
    if (!f)
        return 0;
    GThread* owner = gt_self();
    uint32_t size = reserve > (1u << 20) ? (reserve + 0xFFFFu) & ~0xFFFFu : (1u << 20);
    GThread* t = (GThread*)calloc(1, sizeof *t);
    t->stack_lo = gwin_alloc(size);
    t->stack_hi = t->stack_lo + size;
    t->teb = owner->teb;
    t->tls = owner->tls;
    t->tid = owner->tid;
    t->g.esp = t->stack_hi - 64;
    t->g.fs_base = owner->teb;
    t->g.fcw = 0x027F;
    t->g.mxcsr = 0x1F80;
    f->gt = t;
    f->seh = 0xFFFFFFFFu;
    f->stack_base = t->stack_hi;
    f->stack_limit = t->stack_lo;
    return f->guest;
}
static void sh_CreateFiberEx(Guest* g) { RET(create(ARG(1), ARG(3), ARG(4)), 5); }
static void sh_CreateFiber(Guest* g) { RET(create(ARG(0), ARG(1), ARG(2)), 3); }

static void sh_SwitchToFiber(Guest* g)
{
    uint32_t teb = gt_self()->teb;
    Fiber* cur = fiber(rd32(teb + 0x10));
    Fiber* to = fiber(ARG(0));
    if (!to || to == cur)
        RET(0, 1);
    if (!cur)
    {
        rt_log("[recomp] SwitchToFiber from a thread that is not a fiber\n");
        RET(0, 1);
    }
    g->esp += 8; /* the shim returns here when this fiber runs again: pop its frame first */
    g->eax = 0;
    /* the TEB as the target left it */
    cur->seh = rd32(teb), cur->stack_base = rd32(teb + 4), cur->stack_limit = rd32(teb + 8);
    wr32(teb, to->seh), wr32(teb + 4, to->stack_base), wr32(teb + 8, to->stack_limit);
    wr32(teb + 0x10, to->guest);
    cur->wake = 0;
    if (!to->started)
    {
        to->started = 1;
        to->wake = 1;
        plat_thread_start(fiber_main, to);
    }
    else
    {
        to->wake = 1;
        plat_wake_all32(&to->wake);
    }
    gt_unlock();
    while (!cur->wake)
        plat_wait32(&cur->wake, 0);
    gt_lock();
}

static void sh_DeleteFiber(Guest* g)
{
    Fiber* f = fiber(ARG(0));
    if (f)
        f->deleted = 1; /* its parked host thread is left; the object stays valid for the game's reads */
    RET(0, 1);
}

static const ShimDef FIBERS[] = {
    { "kernel32.dll", "ConvertThreadToFiber", sh_ConvertThreadToFiber },
    { "kernel32.dll", "ConvertThreadToFiberEx", sh_ConvertThreadToFiberEx },
    { "kernel32.dll", "ConvertFiberToThread", sh_ConvertFiberToThread },
    { "kernel32.dll", "CreateFiberEx", sh_CreateFiberEx },
    { "kernel32.dll", "CreateFiber", sh_CreateFiber },
    { "kernel32.dll", "SwitchToFiber", sh_SwitchToFiber },
    { "kernel32.dll", "DeleteFiber", sh_DeleteFiber },
    { NULL, NULL, NULL },
};

void fiber_init(void) { thunk_register(FIBERS); }
