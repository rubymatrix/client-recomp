/* Win32 fibers (kernel32 ConvertThreadToFiber, CreateFiber(Ex), SwitchToFiber, DeleteFiber) for
 * FINAL FANTASY XIV's job system.
 *
 * A fiber is a real one: its own guest state (registers, guest stack) and its own host stack, and
 * SwitchToFiber swaps the host's callee-saved registers and stack pointer (fiber_swap, below) on
 * the calling thread, as Windows does. The game switches tens of thousands of times a second;
 * fibers as host threads handing over to each other cost two scheduler round trips each.
 *
 * A fiber runs with the identity of the thread it runs on: its TEB (fs:), TLS slots and thread id
 * are taken from the switching thread. Like Windows, a switch saves the leaving fiber's exception
 * list and stack bounds from the TEB and loads the target's, and keeps the current fiber at
 * TEB+0x10 (GetCurrentFiber; the fiber object's first dword is its parameter, GetFiberData).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "gthread.h"
#include "gwin.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"

#define FIBER_MAGIC 0x52424946u /* "FIBR" */
#define MAX_FIBERS 1024

typedef struct Fiber
{
    uint32_t guest;          /* the fiber object: [0] parameter, [4] magic, [8] our index */
    uint32_t start;          /* LPFIBER_START_ROUTINE, 0 for a converted thread */
    GThread* gt;             /* its guest state */
    void* sp;                /* its host stack pointer while it is not running (fiber_swap) */
    void* stack;             /* its host stack (none for a converted thread) */
    int started, deleted, running;
    uint32_t seh, stack_base, stack_limit; /* its TEB fields while it is not running */
} Fiber;

#define HOST_STACK (4u << 20)

/* fiber_swap(save, load): saves the callee-saved registers on this stack and its pointer at *save,
 * then resumes the stack at load. A new fiber's stack is made to resume in fiber_trampoline, which
 * calls xiv_fiber_entry(the Fiber in x19). */
void fiber_swap(void** save, void* load);
__asm__(
    ".text\n.p2align 2\n.globl _fiber_swap\n_fiber_swap:\n"
    "  sub sp, sp, #160\n"
    "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
    "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
    "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
    "  mov x2, sp\n  str x2, [x0]\n  mov sp, x1\n"
    "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
    "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
    "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #160\n  ret\n"
    ".globl _fiber_trampoline\n_fiber_trampoline:\n"
    "  mov x0, x19\n  bl _xiv_fiber_entry\n  brk #0\n");
void fiber_trampoline(void);

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
    /* DeleteFiber keeps the slot (the game may still read the deleted object), so once every slot
     * has been used, take one whose fiber was deleted and has stopped. The old guest object stays
     * readable but no longer resolves (fiber() checks guest == p). Without this the job system's
     * fiber churn ran the table out after ~40 minutes of play. */
    for (int pass = 0; pass < 2; ++pass)
    for (uint32_t i = 1; i < MAX_FIBERS; ++i)
        if (!g_fibers[i] || (pass && g_fibers[i]->deleted && !g_fibers[i]->running && !g_fibers[i]->stack))
        {
            if (g_fibers[i])
                free(g_fibers[i]);
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

void xiv_fiber_entry(Fiber* f)
{
    uint32_t a[1] = { rd32(f->guest) };
    guest_call(f->start, 1, a);
    /* a fiber routine that returns ends its thread (ExitThread) */
    rt_log("[recomp] fiber %08x returned: ending its thread\n", f->guest);
    k_thread_exit(0);
    for (;;)
        plat_sleep_ms(1000);
}

/* the host stack a new fiber starts on: fiber_swap's frame, resuming in fiber_trampoline */
static void* first_frame(Fiber* f)
{
    f->stack = mmap(NULL, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->stack == MAP_FAILED)
    {
        f->stack = NULL;
        return NULL;
    }
    uint64_t* frame = (uint64_t*)((char*)f->stack + HOST_STACK - 160);
    memset(frame, 0, 160);
    frame[0] = (uint64_t)(uintptr_t)f;                    /* x19 */
    frame[10] = 0;                                        /* x29 */
    frame[11] = (uint64_t)(uintptr_t)fiber_trampoline;    /* x30 */
    return frame;
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
    f->running = 1;
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
    GThread* me = gt_self();
    uint32_t teb = me->teb;
    Fiber* cur = fiber(rd32(teb + 0x10));
    Fiber* to = fiber(ARG(0));
    if (!to || to == cur)
        RET(0, 1);
    if (!cur)
    {
        rt_log("[recomp] SwitchToFiber from a thread that is not a fiber\n");
        RET(0, 1);
    }
    if (!to->started && !(to->sp = first_frame(to)))
    {
        rt_log("[recomp] SwitchToFiber: no host stack for fiber %08x\n", to->guest);
        RET(0, 1);
    }
    to->started = 1;
    g->esp += 8; /* the shim returns here when this fiber runs again: pop its frame first */
    g->eax = 0;
    /* the TEB as the target left it */
    cur->seh = rd32(teb), cur->stack_base = rd32(teb + 4), cur->stack_limit = rd32(teb + 8);
    wr32(teb, to->seh), wr32(teb + 4, to->stack_base), wr32(teb + 8, to->stack_limit);
    wr32(teb + 0x10, to->guest);
    /* the target runs on this thread, as this thread */
    GThread* t = to->gt;
    t->teb = me->teb;
    t->tls = me->tls;
    t->tid = me->tid;
    t->g.fs_base = me->g.fs_base;
    cur->running = 0;
    to->running = 1;
    gt_adopt(t);
    fiber_swap(&cur->sp, to->sp);
    /* back: a switch to this fiber, on whichever thread made it, adopted this fiber's state */
}

static void sh_DeleteFiber(Guest* g)
{
    Fiber* f = fiber(ARG(0));
    if (f && !f->running && f->stack)
    {
        munmap(f->stack, HOST_STACK);
        f->stack = NULL;
        f->started = 0; /* the object stays valid for the game's reads */
    }
    if (f)
        f->deleted = 1;
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
