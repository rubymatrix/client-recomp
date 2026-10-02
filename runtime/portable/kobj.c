/* Win32 kernel objects for the guest. See kobj.h. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "kobj.h"
#include "plat.h"

#define MAX_HANDLES 8192u
#define HANDLE_BASE 0x100u

struct KObject
{
    KKind kind;
    uint32_t refs;
    void* data;
    void (*close)(void* data);
    /* waitable state */
    int signaled, manual;
    int32_t count, max;
    uint32_t owner, recursion;
    uint32_t wslot; /* its wait word in g_wait (stable: objects are freed, the words are not) */
};

static KObject* g_slots[MAX_HANDLES];
static volatile uint32_t g_lock;         /* guards g_slots and every object's state */
static volatile uint32_t g_epoch;        /* advanced by every signal; multi-object waiters sleep on it */
static volatile uint32_t g_epoch_sleepers;

/* A single-object wait sleeps on its object's own word, and a signal wakes only that word's
 * sleepers, and only if there are any: one global wake for every signal woke every waiting thread
 * in the process, hundreds of thousands of times a second in busy scenes (the game's render
 * command queue is a semaphore released once per command). */
#define WAIT_WORDS 1024u
static struct
{
    volatile uint32_t gen, sleepers;
} g_wait[WAIT_WORDS];
static volatile uint32_t g_next_wslot;
/* XIV_KWAIT_PEROBJECT=1: single-object waits on their own word (off: everything on the epoch, which
 * every signal advances; wakes still only when someone sleeps on it) */
static int g_per_object = -1;

static void wake_slot(uint32_t ws)
{
    __atomic_fetch_add(&g_wait[ws].gen, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_wait[ws].sleepers, __ATOMIC_SEQ_CST))
        plat_wake_all32(&g_wait[ws].gen);
    if (__atomic_load_n(&g_epoch_sleepers, __ATOMIC_SEQ_CST))
    {
        __atomic_fetch_add(&g_epoch, 1, __ATOMIC_SEQ_CST);
        plat_wake_all32(&g_epoch);
    }
}

static void lock(void)
{
    /* held for a few instructions: spin on the CPU's hint first, the scheduler only after */
    for (unsigned spin = 0; plat_atomic_cas32(&g_lock, 0, 1) != 0; ++spin)
        if (spin < 200)
        {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        else
            plat_yield();
}

static void unlock(void)
{
    plat_atomic_cas32(&g_lock, 1, 0);
}

void k_poke(void)
{
    __atomic_fetch_add(&g_epoch, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_epoch_sleepers, __ATOMIC_SEQ_CST))
        plat_wake_all32(&g_epoch);
}

static KObject* get(uint32_t h)
{
    if (h < HANDLE_BASE || (h & 3))
        return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4;
    return i < MAX_HANDLES ? g_slots[i] : NULL;
}

static uint32_t insert(KObject* o)
{
    lock();
    for (uint32_t i = 0; i < MAX_HANDLES; ++i)
        if (!g_slots[i])
        {
            g_slots[i] = o;
            o->refs++;
            unlock();
            return HANDLE_BASE + 4 * i;
        }
    unlock();
    return 0;
}

uint32_t k_new(KKind kind, void* data, void (*close)(void* data))
{
    KObject* o = (KObject*)calloc(1, sizeof *o);
    o->kind = kind;
    o->data = data;
    o->close = close;
    o->wslot = __atomic_fetch_add(&g_next_wslot, 1, __ATOMIC_RELAXED) % WAIT_WORDS;
    uint32_t h = insert(o);
    if (!h)
        free(o);
    return h;
}

void* k_data(uint32_t h, KKind kind)
{
    lock();
    KObject* o = get(h);
    void* d = o && o->kind == kind ? o->data : NULL;
    unlock();
    return d;
}

static void release(KObject* o)
{
    /* called with the lock held */
    if (--o->refs)
        return;
    if (o->close)
        o->close(o->data);
    free(o);
}

int k_close(uint32_t h)
{
    lock();
    KObject* o = get(h);
    if (!o)
    {
        unlock();
        return 0;
    }
    g_slots[(h - HANDLE_BASE) / 4] = NULL;
    release(o);
    unlock();
    return 1;
}

uint32_t k_duplicate(uint32_t h)
{
    lock();
    KObject* o = get(h);
    if (!o)
    {
        unlock();
        return 0;
    }
    o->refs++; /* insert takes another */
    unlock();
    uint32_t d = insert(o);
    lock();
    o->refs--;
    unlock();
    return d;
}

/* --- waitables --------------------------------------------------------------------------------- */

uint32_t k_event(int manual, int initial)
{
    uint32_t h = k_new(K_EVENT, NULL, NULL);
    lock();
    KObject* o = get(h);
    o->manual = manual;
    o->signaled = initial;
    unlock();
    return h;
}

int k_event_set(uint32_t h, int state)
{
    lock();
    KObject* o = get(h);
    if (!o || o->kind != K_EVENT)
    {
        unlock();
        return 0;
    }
    o->signaled = state;
    uint32_t ws = o->wslot;
    unlock();
    if (state)
        wake_slot(ws);
    return 1;
}

uint32_t k_mutex(int owned)
{
    uint32_t h = k_new(K_MUTEX, NULL, NULL);
    lock();
    KObject* o = get(h);
    if (owned)
    {
        o->owner = plat_thread_id();
        o->recursion = 1;
    }
    unlock();
    return h;
}

int k_mutex_release(uint32_t h)
{
    lock();
    KObject* o = get(h);
    if (!o || o->kind != K_MUTEX || o->owner != plat_thread_id())
    {
        unlock();
        return 0;
    }
    if (--o->recursion == 0)
        o->owner = 0;
    int freed = !o->owner;
    uint32_t ws = o->wslot;
    unlock();
    if (freed)
        wake_slot(ws);
    return 1;
}

uint32_t k_semaphore(int32_t initial, int32_t max)
{
    uint32_t h = k_new(K_SEMAPHORE, NULL, NULL);
    lock();
    KObject* o = get(h);
    o->count = initial;
    o->max = max;
    unlock();
    return h;
}

int k_semaphore_release(uint32_t h, int32_t n, int32_t* previous)
{
    lock();
    KObject* o = get(h);
    if (!o || o->kind != K_SEMAPHORE || n <= 0 || o->count + n > o->max)
    {
        unlock();
        return 0;
    }
    if (previous)
        *previous = o->count;
    o->count += n;
    uint32_t ws = o->wslot;
    unlock();
    wake_slot(ws);
    return 1;
}

/* with the lock held */
static int ready(KObject* o, uint32_t tid)
{
    switch (o->kind)
    {
    case K_EVENT:
    case K_THREAD: return o->signaled;
    case K_MUTEX: return !o->owner || o->owner == tid;
    case K_SEMAPHORE: return o->count > 0;
    default: return 0;
    }
}

static void take(KObject* o, uint32_t tid)
{
    switch (o->kind)
    {
    case K_EVENT:
        if (!o->manual)
            o->signaled = 0;
        break;
    case K_MUTEX:
        o->owner = tid;
        o->recursion++;
        break;
    case K_SEMAPHORE: o->count--; break;
    default: break;
    }
}

uint32_t k_wait(const uint32_t* handles, uint32_t n, int all, uint32_t ms)
{
    if (g_per_object < 0)
        g_per_object = getenv("XIV_KWAIT_PEROBJECT") != NULL;
    uint32_t tid = plat_thread_id();
    uint64_t deadline = ms == ~0u ? ~0ull : rt_monotonic_ns() + (uint64_t)ms * 1000000u;
    for (;;)
    {
        lock();
        KObject* objs[64];
        if (!n || n > 64)
        {
            unlock();
            return K_WAIT_FAILED;
        }
        uint32_t readies = 0, first = ~0u;
        for (uint32_t i = 0; i < n; ++i)
        {
            objs[i] = get(handles[i]);
            if (!objs[i] || objs[i]->kind < K_EVENT || objs[i]->kind > K_THREAD)
            {
                unlock();
                return K_WAIT_FAILED;
            }
            if (ready(objs[i], tid))
            {
                readies++;
                if (first == ~0u)
                    first = i;
            }
        }
        if (all ? readies == n : readies > 0)
        {
            if (all)
                for (uint32_t i = 0; i < n; ++i)
                    take(objs[i], tid);
            else
                take(objs[first], tid);
            unlock();
            return all ? K_WAIT_OBJECT_0 : K_WAIT_OBJECT_0 + first;
        }
        /* one object: its own word; several: the epoch every signal advances */
        volatile uint32_t* word = (n == 1 && g_per_object) ? &g_wait[objs[0]->wslot].gen : &g_epoch;
        volatile uint32_t* sleepers = (n == 1 && g_per_object) ? &g_wait[objs[0]->wslot].sleepers : &g_epoch_sleepers;
        uint32_t e = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        unlock();
        if (ms == 0)
            return K_WAIT_TIMEOUT;
        /* a producer signalling once per item (the game's render command semaphore: hundreds of
         * thousands a second) is usually back within microseconds: watch for its signal before
         * sleeping, so neither side pays a wake syscall and a scheduler round trip per item */
        {
            unsigned spin = 0;
            while (spin < 3000 && __atomic_load_n(word, __ATOMIC_ACQUIRE) == e)
            {
#if defined(__aarch64__)
                __asm__ volatile("yield");
#endif
                spin++;
            }
            if (spin < 3000)
                continue; /* something was signalled: look again */
        }
        uint64_t now = rt_monotonic_ns();
        if (now >= deadline)
            return K_WAIT_TIMEOUT;
        uint64_t left = deadline == ~0ull ? ~0u : (deadline - now + 999999u) / 1000000u;
        int held = gt_holds();
        if (held)
            gt_unlock();
        __atomic_fetch_add(sleepers, 1, __ATOMIC_SEQ_CST);
        plat_wait32_ms(word, e, left > 0xFFFFFFFEu ? ~0u : (uint32_t)left);
        __atomic_fetch_sub(sleepers, 1, __ATOMIC_SEQ_CST);
        if (held)
            gt_lock();
    }
}

/* --- threads ------------------------------------------------------------------------------------ */
typedef struct ThreadCtx
{
    uint32_t handle, start, param;
    volatile uint32_t tid;
    volatile uint32_t suspend;
    uint32_t exit_code;
    jmp_buf exit;
} ThreadCtx;

static RT_TLS ThreadCtx* t_ctx;

static void thread_main(void* p)
{
    ThreadCtx* c = (ThreadCtx*)p;
    t_ctx = c;
    c->tid = plat_thread_id();
    plat_wake_all32(&c->tid);
    for (uint32_t s = c->suspend; s; s = c->suspend)
        plat_wait32(&c->suspend, s);
    uint32_t code;
    if (!setjmp(c->exit))
        code = guest_call(c->start, 1, &c->param);
    else
        code = c->exit_code; /* ExitThread: k_thread_exit released the guest lock before jumping here */
    gt_exit_self();
    lock();
    KObject* o = get(c->handle);
    uint32_t ws = ~0u;
    if (o && o->kind == K_THREAD && o->data == c)
    {
        o->signaled = 1;
        c->exit_code = code;
        ws = o->wslot;
    }
    unlock();
    if (ws != ~0u)
        wake_slot(ws);
    k_poke();
    k_close(c->handle); /* the thread's own reference */
}

static void thread_close(void* p)
{
    free(p);
}

uint32_t k_thread_create(uint32_t start, uint32_t param, int suspended, uint32_t* tid)
{
    ThreadCtx* c = (ThreadCtx*)calloc(1, sizeof *c);
    c->start = start;
    c->param = param;
    c->suspend = suspended ? 1 : 0;
    c->exit_code = 0x103; /* STILL_ACTIVE */
    uint32_t h = k_new(K_THREAD, c, thread_close);
    c->handle = k_duplicate(h); /* the thread holds one reference, the creator the other */
    if (!plat_thread_start(thread_main, c))
    {
        k_close(c->handle);
        k_close(h);
        return 0;
    }
    int held = gt_holds();
    if (held)
        gt_unlock();
    while (!c->tid)
        plat_wait32(&c->tid, 0);
    if (held)
        gt_lock();
    if (tid)
        *tid = c->tid;
    return h;
}

int k_thread_resume(uint32_t h, uint32_t* previous)
{
    ThreadCtx* c = (ThreadCtx*)k_data(h, K_THREAD);
    if (!c)
        return 0;
    uint32_t s = c->suspend;
    if (previous)
        *previous = s;
    if (s)
    {
        c->suspend = s - 1;
        plat_wake_all32(&c->suspend);
    }
    return 1;
}

int k_thread_exit_code(uint32_t h, uint32_t* code)
{
    ThreadCtx* c = (ThreadCtx*)k_data(h, K_THREAD);
    if (!c)
        return 0;
    *code = c->exit_code;
    return 1;
}

void k_thread_exit(uint32_t code)
{
    ThreadCtx* c = t_ctx;
    if (!c)
    {
        rt_log("[recomp] ExitThread(%u) on a thread the guest did not create: ending the process\n", code);
        exit((int)code);
    }
    c->exit_code = code;
    if (gt_holds())
        gt_unlock();
    longjmp(c->exit, 1); /* out through the translated frames, which are plain C */
}
