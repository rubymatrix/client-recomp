/* Host <-> guest bridges for the R2 target (32-bit Windows).
 *
 * Every guest thread has its own guest stack and guest TEB. Calls cross the boundary by copying
 * a bounded window of stack arguments, and the number of bytes the callee popped (stdcall) is
 * measured from the stack pointer afterwards, so no signatures are needed:
 *
 *   host -> guest  An entry stub (`push guest_addr ; jmp enter_asm`) sits behind every patched
 *                  function entry. enter_c copies the caller's arguments onto the guest stack,
 *                  runs the translation, then rebuilds the host return: popped bytes, eax/edx,
 *                  callee-saved registers, and an x87 return value if one was left in ST(0).
 *   guest -> host  rt_call_indirect hands any target with no translation (an import, a D3D
 *                  method, a GetProcAddress result) to bridge_native, which copies the guest's
 *                  arguments onto the host stack, calls, and applies the result to the guest.
 *
 * Both directions keep the x87 control word in step: native code changes it (Direct3D 8 sets
 * single precision on its thread) and the original game code ran under whatever it was set to.
 *
 * Nesting is supported: a native call can call back into the guest (window procedures), which
 * builds below the guest stack pointer of the suspended call and restores it on return.
 */
#include <windows.h>

#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "bridge.h"
#include "build.h" /* FFXI_PRESENT_SITE */

#define GUEST_STACK_SIZE (4u << 20)
#define ARG_WINDOW 32u /* dwords copied across the boundary: enough for any Win32 / COM call */

static __declspec(thread) Guest* t_guest;
static volatile LONG g_live_threads; /* guest threads with state allocated, for the trace header */

/* --- the guest lock ---------------------------------------------------------------------------
 *
 * A ticket lock, so hand-off is FIFO: a thread yielding at a safepoint queues behind the waiter
 * instead of barging back in. Held while translated code runs; released around every native
 * call (bridge_native) and taken on every host->guest entry (enter_c), including callbacks nested
 * inside a native call. t_held says whether this thread holds it. */
static volatile LONG g_next_ticket, g_now_serving;
static __declspec(thread) int t_held;

/* Hand-off policy. Handing the lock over at every native call convoys: the render thread makes
 * thousands of D3D calls a frame, and with a worker waiting each one became a context switch
 * (the in-combat freeze, 2026-09-24). So the lock is a time slice, as on the single-core CPUs
 * this code was written for: short, non-blocking native calls ("leaf" calls, is_leaf) keep it,
 * and a holder gives it to a waiter only once it has held it for QUANTUM, at a safepoint or a
 * leaf call. Calls that may block still always release it, or a guest thread waiting on another
 * would deadlock. */
static unsigned long long g_quantum_ticks; /* rdtsc ticks in ~2 ms, calibrated in bridge_init */
static unsigned long long g_acquired_at;   /* rdtsc when the holder took the lock */
static volatile DWORD g_holder_tid;
static Guest* volatile g_holder_guest;

static void set_waiting(int w);

static void lock_acquire(void)
{
    set_waiting(1);
    InterlockedIncrement((volatile LONG*)&rt_lock_contended);
    LONG t = InterlockedExchangeAdd(&g_next_ticket, 1);
    for (unsigned spin = 0;; ++spin)
    {
        LONG cur = g_now_serving;
        if (cur == t)
            break;
        if (spin < 128)
            YieldProcessor();
        else
            WaitOnAddress(&g_now_serving, &cur, sizeof cur, INFINITE);
    }
    InterlockedDecrement((volatile LONG*)&rt_lock_contended);
    set_waiting(0);
    t_held = 1;
    g_acquired_at = __rdtsc();
    g_holder_tid = GetCurrentThreadId();
    g_holder_guest = t_guest;
}

static void lock_release(void)
{
    t_held = 0;
    g_holder_tid = 0;
    InterlockedIncrement(&g_now_serving);
    if (rt_lock_contended)
        WakeByAddressAll((PVOID)&g_now_serving);
}

/* Give the lock to a waiter if this thread has had its quantum; the ticket order puts the waiter
 * first. Run at safepoints and around leaf calls. */
static void lock_yield(void)
{
    if (!t_held || !rt_lock_contended || __rdtsc() - g_acquired_at < g_quantum_ticks)
        return;
    lock_release();
    lock_acquire();
}

/* --- trace of the boundary --------------------------------------------------------------------
 *
 * Every guest->host call (import, COM method, gamecore function-table slot, FFXi.dll) counted by
 * (target, guest call site), every host->guest entry by (guest function, host return address),
 * and, in order with their first four arguments, the first SEQ_REPEAT occurrences of each pair
 * (so rare calls - a connect, a registry read - are logged however long the run; frame-rate
 * traffic stops after SEQ_REPEAT), up to SEQ_MAX lines. It is the list of
 * shims R3.1 needs and the reference trace later builds are diffed against. Updated under the
 * guest lock, so the tables need no atomics. tools/trace_report.py resolves the addresses. */
typedef struct TraceSlot
{
    uint32_t target, site, count;
} TraceSlot;

#define CALLS_BITS 16
#define ENTRIES_BITS 14
#define SEQ_MAX 2000000u
#define SEQ_REPEAT 32u
static TraceSlot g_calls[1u << CALLS_BITS], g_entries[1u << ENTRIES_BITS];
static unsigned g_calls_used, g_entries_used, g_trace_dropped;
static FILE* g_seq;
static unsigned g_seq_n;
static char g_trace_path[MAX_PATH];
static ULONGLONG g_trace_last_dump;

typedef struct ImportName
{
    uint32_t addr;
    char strings; /* 'A' or 'W': the API takes strings of that width; 0 otherwise */
    char name[80];
} ImportName;
static ImportName g_imports[600];
static unsigned g_import_count;
static uint32_t g_enter_cs;    /* EnterCriticalSection as the game's IAT holds it */
static volatile LONG g_frames; /* Present calls, for the profiler */
static volatile DWORD g_render_tid; /* the thread that calls Present */

void bridge_note_import(uint32_t addr, const char* dll, const char* name)
{
    if (g_import_count < sizeof g_imports / sizeof g_imports[0])
    {
        ImportName* i = &g_imports[g_import_count++];
        size_t n = strlen(name);
        i->addr = addr;
        i->strings = !strcmp(name, "GetProcAddress") ? 'A' : (n > 1 && (name[n - 1] == 'A' || name[n - 1] == 'W')) ? name[n - 1] : 0;
        sprintf_s(i->name, sizeof i->name, "%s!%s", dll, name);
        if (!strcmp(name, "EnterCriticalSection"))
            g_enter_cs = addr;
    }
}

/* In the sequence log, the string arguments of string-taking imports: which files, registry
 * keys, modules and message texts the game uses. Pointers are only read if they look like one,
 * under __try, and only for the first SEQ_MAX calls. */
/* Set while seq_strings probes a pointer: its faults are expected and handled by its own __try,
 * so the stand-in's crash handler must not report them (bridge_probing). */
static __declspec(thread) int t_probing;

int bridge_probing(void) { return t_probing; }

static void seq_strings(uint32_t target, const uint32_t* a, uint32_t n)
{
    char kind = 0;
    for (unsigned i = 0; i < g_import_count; ++i)
        if (g_imports[i].addr == target)
        {
            kind = g_imports[i].strings;
            /* connect(s, sockaddr*, len): where the game dials */
            if (!strcmp(g_imports[i].name, "WS2_32.dll!#4") && n > 1 && a[1] >= 0x10000u)
            {
                const uint8_t* sa = (const uint8_t*)(uintptr_t)a[1];
                fprintf(g_seq, "s 1 \"sockaddr family %u %u.%u.%u.%u:%u\"\n", sa[0] | (sa[1] << 8), sa[4], sa[5], sa[6], sa[7],
                    (sa[2] << 8) | sa[3]);
            }
            break;
        }
    if (!kind)
        return;
    for (uint32_t k = 0; k < n && k < 6; ++k)
    {
        if (a[k] < 0x10000u)
            continue;
        char buf[200];
        unsigned len = 0;
        t_probing = 1;
        __try
        {
            for (; len < sizeof buf - 1; ++len)
            {
                unsigned c = kind == 'W' ? ((const uint16_t*)(uintptr_t)a[k])[len] : ((const uint8_t*)(uintptr_t)a[k])[len];
                if (!c)
                    break;
                if (c < 0x20 || c > 0x7E)
                {
                    len = 0; /* not text */
                    break;
                }
                buf[len] = (char)c;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            len = 0;
        }
        t_probing = 0;
        if (len >= 2)
        {
            buf[len] = 0;
            fprintf(g_seq, "s %u \"%s\"\n", k, buf);
        }
    }
}

/* --- leaf calls ---------------------------------------------------------------------------------
 *
 * A leaf call is short and never waits for another guest thread, so the guest lock can stay
 * held across it. Leaf: the named imports below, and anything in the D3D8, DirectSound and
 * DirectInput modules or outside every module (D3D8's heap-built state-setter stubs, 9 M calls in
 * one session). Everything else - Sleep, waits, sockets, file I/O, gamecore, message pumps -
 * releases the lock. Classified once per target, cached; updated under the guest lock. */
static const char* const LEAF_IMPORTS[] = {
    "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapSize", "TlsGetValue", "TlsSetValue", "GetLastError", "SetLastError",
    "InterlockedIncrement", "InterlockedDecrement", "InterlockedExchange", "InterlockedCompareExchange",
    "InterlockedExchangeAdd", "GetTickCount", "QueryPerformanceCounter", "QueryPerformanceFrequency", "timeGetTime",
    "LeaveCriticalSection", "MultiByteToWideChar", "WideCharToMultiByte", "GetCurrentThreadId", "GetCurrentThread",
    "GetCurrentProcess", "GetCurrentProcessId", "LCMapStringA", "LCMapStringW", "GetStringTypeA", "GetStringTypeW",
    "CompareStringA", "CompareStringW", "lstrlenA", "lstrcpyA", "lstrcmpA", "lstrcmpiA", "GetSystemTime", "GetLocalTime",
    "FileTimeToLocalFileTime", "FileTimeToSystemTime", "SystemTimeToFileTime", "GetKeyState", "GetAsyncKeyState",
    "GetKeyboardState", "GetCursorPos", "ScreenToClient", "ClientToScreen", "GetClientRect", "GetWindowRect",
    "SetEvent", "ResetEvent", "ReleaseSemaphore", "ReleaseMutex", "GetSystemMetrics", "IsProcessorFeaturePresent",
    NULL,
};

typedef struct LeafSlot
{
    uint32_t target, site;
    int8_t leaf; /* 1 leaf, 0 not; slot empty when target == 0 */
} LeafSlot;
#define LEAF_SLOTS 16384u
static LeafSlot g_leaf[LEAF_SLOTS];
static unsigned long long g_slow_ticks; /* a leaf call longer than this (~0.5 ms) is demoted */
static unsigned g_demoted;

static int classify_leaf(uint32_t target)
{
    for (unsigned i = 0; i < g_import_count; ++i)
        if (g_imports[i].addr == target)
        {
            const char* bang = strchr(g_imports[i].name, '!');
            const char* name = bang ? bang + 1 : g_imports[i].name;
            for (const char* const* l = LEAF_IMPORTS; *l; ++l)
                if (!strcmp(*l, name))
                    return 1;
            return 0;
        }
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)(uintptr_t)target, &m))
        return 1; /* runtime-generated code: D3D8's state stubs */
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    return !_stricmp(base, "d3d8.dll") || !_stricmp(base, "d3d9.dll") || !_stricmp(base, "dsound.dll") ||
        !_stricmp(base, "dinput8.dll");
}

/* Classified per (target, call site): the same D3D method is short at one site and waits for the
 * GPU at another. A leaf pair observed to run longer than g_slow_ticks - Present waiting for
 * vsync, a LockRect waiting for the GPU (R1's per-frame readback) - is demoted for good: holding
 * the lock through it stalled every other guest thread (the Lower Jeuno lag, 2026-09-24). */
static LeafSlot* leaf_slot(uint32_t target, uint32_t site)
{
    unsigned i = ((target * 2654435761u) ^ (site * 0x9E3779B1u)) >> 18;
    for (unsigned k = 0; k < LEAF_SLOTS; ++k, i = (i + 1) & (LEAF_SLOTS - 1))
    {
        if (g_leaf[i].target == target && g_leaf[i].site == site)
            return &g_leaf[i];
        if (!g_leaf[i].target)
        {
            g_leaf[i].target = target;
            g_leaf[i].site = site;
            g_leaf[i].leaf = (int8_t)classify_leaf(target);
            return &g_leaf[i];
        }
    }
    return NULL; /* full: treated as not leaf */
}

/* Returns how many times the pair has now been seen (0 if the table is full). */
static uint32_t trace_count(TraceSlot* table, unsigned bits, unsigned* used, uint32_t target, uint32_t site)
{
    unsigned mask = (1u << bits) - 1, i = ((target * 2654435761u) ^ (site * 0x9E3779B1u)) >> (32 - bits);
    for (;; i = (i + 1) & mask)
    {
        TraceSlot* s = &table[i];
        if (s->count && s->target == target && s->site == site)
        {
            return ++s->count;
        }
        if (!s->count)
        {
            if (*used >= mask - (mask >> 3))
            {
                g_trace_dropped++;
                return 0;
            }
            s->target = target;
            s->site = site;
            s->count = 1;
            (*used)++;
            return 1;
        }
    }
}

static int by_count(const void* a, const void* b)
{
    uint32_t x = ((const TraceSlot*)a)->count, y = ((const TraceSlot*)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void dump_table(FILE* f, const char* tag, const TraceSlot* table, unsigned n)
{
    TraceSlot* sorted = (TraceSlot*)malloc(n * sizeof *sorted);
    unsigned k = 0;
    for (unsigned i = 0; i < n; ++i)
        if (table[i].count)
            sorted[k++] = table[i];
    qsort(sorted, k, sizeof *sorted, by_count);
    for (unsigned i = 0; i < k; ++i)
        fprintf(f, "%s %08x %08x %u\n", tag, sorted[i].target, sorted[i].site, sorted[i].count);
    free(sorted);
}

/* Aggregate trace: loaded modules, the import map, then both count tables (most frequent first).
 * Call sites are static guest addresses (relocation delta removed); targets are host addresses. */
void bridge_trace_dump(void)
{
    if (!g_trace_path[0])
        return;
    if (g_seq)
        fflush(g_seq);
    char tmp[MAX_PATH + 4];
    sprintf_s(tmp, sizeof tmp, "%s.tmp", g_trace_path);
    FILE* f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# FFXIRecompile boundary trace. image %08x delta %08x; calls %u entries %u dropped %u; guest threads live %ld\n",
        rt_image_lo, rt_reloc_delta, g_calls_used, g_entries_used, g_trace_dropped, g_live_threads);
    HMODULE mods[512];
    DWORD need = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
    {
        for (unsigned i = 0; i < need / sizeof mods[0] && i < 512; ++i)
        {
            MODULEINFO mi;
            char path[MAX_PATH];
            if (K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) &&
                GetModuleFileNameA(mods[i], path, MAX_PATH))
                fprintf(f, "module %08x %08x %s\n", (uint32_t)(uintptr_t)mi.lpBaseOfDll, (uint32_t)mi.SizeOfImage, path);
        }
    }
    for (unsigned i = 0; i < g_import_count; ++i)
        fprintf(f, "import %08x %s\n", g_imports[i].addr, g_imports[i].name);
    dump_table(f, "call", g_calls, 1u << CALLS_BITS);
    dump_table(f, "entry", g_entries, 1u << ENTRIES_BITS);
    fclose(f);
    MoveFileExA(tmp, g_trace_path, MOVEFILE_REPLACE_EXISTING);
    g_trace_last_dump = GetTickCount64();
}

/* base: path prefix; writes <base>.trace.txt (aggregate, rewritten) and <base>.seq.txt (in order). */
void bridge_trace_open(const char* base)
{
    sprintf_s(g_trace_path, sizeof g_trace_path, "%s.trace.txt", base);
    char seq[MAX_PATH];
    sprintf_s(seq, sizeof seq, "%s.seq.txt", base);
    g_seq = fopen(seq, "w");
    if (g_seq)
        fprintf(g_seq, "# c <n> <ms> <tid> <target> <site> <a0..a3>  then  r <n> <eax>   |   e <n> <ms> <tid> <guest fn> <host ret>\n");
    g_trace_last_dump = GetTickCount64();
}

static uint16_t host_fcw(void)
{
    uint16_t w;
    __asm fnstcw w
    return w;
}

static void set_host_fcw(uint16_t w)
{
    __asm fldcw w
}

/* A thread's guest state and what backs it. The game starts short-lived worker threads by the
 * hundred (0x102480d0 alone, once per request), so all of it is freed when the thread exits: a
 * fiber-local-storage destructor runs on every thread exit, including ExitThread from guest code.
 * (TerminateThread skips it; ov_TerminateThread logs those.) */
typedef struct GuestThread
{
    Guest g; /* first: t_guest points here */
    unsigned char* stack;
    uint32_t* teb;
    DWORD tid;
    volatile uint32_t native_target, native_site; /* the non-leaf native call it is in, or 0 */
    volatile ULONGLONG native_since;
    volatile int waiting_lock; /* in lock_acquire, for the profiler */
    volatile uint32_t leaf_target; /* the leaf native call it is in (lock held), or 0 */
} GuestThread;

static void set_waiting(int w)
{
    if (t_guest)
        ((GuestThread*)t_guest)->waiting_lock = w;
}

/* Every live guest thread, for the watchdog. */
#define MAX_GUEST_THREADS 512
static GuestThread* volatile g_threads[MAX_GUEST_THREADS];
static SRWLOCK g_threads_lock = SRWLOCK_INIT;

static void register_thread(GuestThread* t, int add)
{
    AcquireSRWLockExclusive(&g_threads_lock);
    for (unsigned i = 0; i < MAX_GUEST_THREADS; ++i)
        if (add ? !g_threads[i] : g_threads[i] == t)
        {
            g_threads[i] = add ? t : NULL;
            break;
        }
    ReleaseSRWLockExclusive(&g_threads_lock);
}

static DWORD g_fls = FLS_OUT_OF_INDEXES;

static void WINAPI guest_thread_exit(PVOID p)
{
    GuestThread* t = (GuestThread*)p;
    if (!t)
        return;
    if (t_guest == &t->g)
        t_guest = NULL;
    register_thread(t, 0);
    VirtualFree(t->stack, 0, MEM_RELEASE);
    VirtualFree(t->teb, 0, MEM_RELEASE);
    free(t);
    InterlockedDecrement(&g_live_threads);
}

Guest* bridge_guest(void)
{
    if (!t_guest)
    {
        GuestThread* t = (GuestThread*)calloc(1, sizeof *t);
        Guest* g = &t->g;
        unsigned char* stack = (unsigned char*)VirtualAlloc(NULL, GUEST_STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint32_t* teb = (uint32_t*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!t || !stack || !teb)
        {
            rt_log("[recomp] out of memory creating a guest thread (%ld live)\n", g_live_threads);
            abort();
        }
        t->stack = stack;
        t->teb = teb;
        t->tid = GetCurrentThreadId();
        register_thread(t, 1);
        if (g_fls != FLS_OUT_OF_INDEXES)
            FlsSetValue(g_fls, t);
        InterlockedIncrement(&g_live_threads);
        uint32_t top = (uint32_t)(uintptr_t)stack + GUEST_STACK_SIZE;
        teb[0] = 0xFFFFFFFFu;                    /* fs:[0x00] SEH chain: empty */
        teb[1] = top;                            /* fs:[0x04] stack base */
        teb[2] = (uint32_t)(uintptr_t)stack;     /* fs:[0x08] stack limit */
        teb[6] = (uint32_t)(uintptr_t)teb;       /* fs:[0x18] self */
        teb[8] = GetCurrentProcessId();          /* fs:[0x20] */
        teb[9] = GetCurrentThreadId();           /* fs:[0x24] */
        g->esp = top - 64;
        g->fs_base = (uint32_t)(uintptr_t)teb;
        g->fcw = host_fcw();
        t_guest = g;
    }
    return t_guest;
}

/* --- host -> guest --------------------------------------------------------------------------- */

/* pushad layout (lowest address first), then the stub's push and the caller's return. */
typedef struct EntryFrame
{
    uint32_t edi, esi, ebp, esp_, ebx, edx, ecx, eax;
    uint32_t gaddr; /* on return: bytes the callee popped */
    uint32_t ret;
    uint32_t args[1];
} EntryFrame;

static void __cdecl enter_c(EntryFrame* f)
{
    /* Nested inside a native call (a window procedure, say), this thread released the lock in
     * bridge_native; a fresh entry (a thread start, a COM call from the host) never held it. */
    int took = !t_held;
    if (took)
        lock_acquire();
    Guest* g = bridge_guest();
    Guest saved = *g;
    GuestFn fn = rt_lookup(f->gaddr);
    if (!fn)
        rt_fatal(g, f->gaddr, "host called a guest address with no translation");
    uint32_t seen = trace_count(g_entries, ENTRIES_BITS, &g_entries_used, f->gaddr, f->ret);
    if (g_seq && g_seq_n < SEQ_MAX && seen <= SEQ_REPEAT)
        fprintf(g_seq, "e %u %llu %lu %08x %08x\n", g_seq_n++, GetTickCount64(), GetCurrentThreadId(), f->gaddr, f->ret);

    /* Copy the argument window, bounded by the host stack base (fs:[4]). */
    uint32_t src = (uint32_t)(uintptr_t)f->args;
    uint32_t host_base = __readfsdword(4);
    uint32_t n = (host_base - src) / 4;
    if (n > ARG_WINDOW)
        n = ARG_WINDOW;
    uint32_t sp = g->esp - n * 4;
    memcpy(GUEST_PTR(sp), (void*)(uintptr_t)src, n * 4);
    sp -= 4;
    wr32(sp, 0xFEEDF00Du); /* the guest's return address: never used, the translation returns to us */

    g->esp = sp;
    g->eax = f->eax;
    g->ecx = f->ecx;
    g->edx = f->edx;
    g->ebx = f->ebx;
    g->esi = f->esi;
    g->edi = f->edi;
    g->ebp = f->ebp;
    g->df = 0;
    g->fcw = host_fcw();
    uint32_t top = g->top;

    fn(g);

    uint32_t popped = g->esp - (sp + 4);
    if (popped > n * 4)
        rt_fatal(g, f->gaddr, "guest function popped more arguments than were copied");
    f->eax = g->eax;
    f->edx = g->edx;
    f->ecx = g->ecx;
    f->ebx = g->ebx;
    f->esi = g->esi;
    f->edi = g->edi;
    f->ebp = g->ebp;
    uint16_t fcw = g->fcw;
    int fret = g->top != top;
    double ret_st0 = ST(0);

    /* Move the return address up over the popped arguments; enter_asm pops `popped` and returns. */
    *(uint32_t*)((unsigned char*)&f->ret + popped) = f->ret;
    f->gaddr = popped;

    *g = saved;
    if (took)
        lock_release();
    set_host_fcw(fcw);
    if (fret) /* a float/double result goes back in the host's ST(0) */
        __asm fld ret_st0
}

__declspec(naked) void enter_asm(void)
{
    __asm {
        pushad
        push esp
        call enter_c
        add esp, 4
        popad
        add esp, 4          ; drop the guest-address slot (now: popped byte count)
        add esp, [esp - 4]  ; drop the arguments the callee popped
        ret
    }
}

/* --- guest -> host --------------------------------------------------------------------------- */

static __declspec(noinline) uint32_t native_call(uint32_t target, const uint32_t* args, uint32_t nargs, uint32_t ecx_in,
    uint32_t* edx_out, uint32_t* popped_out)
{
    uint32_t r_eax, r_edx, popped;
    __asm {
        mov esi, args
        mov ecx, nargs
        mov ebx, esp
        lea eax, [ecx * 4]
        sub esp, eax
        mov edi, esp
        rep movsd
        mov edi, esp
        mov ecx, ecx_in
        call target
        mov r_eax, eax
        mov r_edx, edx
        mov eax, esp
        sub eax, edi
        mov popped, eax
        mov esp, ebx
    }
    *edx_out = r_edx;
    *popped_out = popped;
    return r_eax;
}

int bridge_native(Guest* g, uint32_t target)
{
    if (target >= rt_image_lo && target < rt_image_hi)
        return 0; /* inside the game image but not translated: a bug, let the caller report it */

    /* [esp] is the return address the guest call pushed; the arguments follow. */
    uint32_t args = g->esp + 4;
    uint32_t stack_top = rd32(g->fs_base + 4);
    uint32_t n = (stack_top - args) / 4;
    if (n > ARG_WINDOW)
        n = ARG_WINDOW;
    Guest saved = *g;

    uint32_t site = rd32(g->esp) - rt_reloc_delta;
    if (site == FFXI_PRESENT_SITE) /* IDirect3DDevice8::Present, from the game's wrapper (0x100035b0) */
    {
        g_frames++;
        g_render_tid = GetCurrentThreadId();
    }
    uint32_t seen = trace_count(g_calls, CALLS_BITS, &g_calls_used, target, site);
    unsigned seq = ~0u;
    if (g_seq && g_seq_n < SEQ_MAX && seen <= SEQ_REPEAT)
    {
        seq = g_seq_n++;
        const uint32_t* a = (const uint32_t*)GUEST_PTR(args);
        fprintf(g_seq, "c %u %llu %lu %08x %08x %08x %08x %08x %08x\n", seq, GetTickCount64(), GetCurrentThreadId(), target,
            site, n > 0 ? a[0] : 0, n > 1 ? a[1] : 0, n > 2 ? a[2] : 0, n > 3 ? a[3] : 0);
        seq_strings(target, a, n);
        if ((seq & 0xFF) == 0)
            fflush(g_seq); /* a killed process keeps all but the last few lines */
    }
    if (GetTickCount64() - g_trace_last_dump > 30000)
        bridge_trace_dump();

    /* EnterCriticalSection: take it without letting go of the guest lock when it is free; only
     * a section another thread holds makes this a blocking call. */
    if (target == g_enter_cs && n > 0 && TryEnterCriticalSection((CRITICAL_SECTION*)(uintptr_t)rd32(args)))
    {
        if (seq != ~0u)
            fprintf(g_seq, "r %u %08x\n", seq, 0u);
        g->esp = args + 4;
        return 1;
    }

    LeafSlot* ls = leaf_slot(target, site);
    int leaf = ls && ls->leaf;
    GuestThread* self = (GuestThread*)g;
    uint32_t edx, popped;
    unsigned long long t0 = 0;
    set_host_fcw(g->fcw);
    if (leaf)
    {
        lock_yield(); /* hands over only once this thread's quantum is used */
        self->leaf_target = target;
        t0 = __rdtsc();
    }
    else
    {
        self->native_target = target;
        self->native_site = site;
        self->native_since = GetTickCount64();
        lock_release();
    }
    uint32_t eax = native_call(target, (const uint32_t*)GUEST_PTR(args), n, g->ecx, &edx, &popped);
    uint16_t fcw = host_fcw();
    if (!leaf)
    {
        lock_acquire();
        self->native_target = 0;
    }
    else if (self->leaf_target = 0, __rdtsc() - t0 > g_slow_ticks)
    {
        ls->leaf = 0;
        if (g_demoted++ < 64)
            rt_log("[recomp] leaf call %08x from %08x took %llu us: now released around\n", target, site,
                (__rdtsc() - t0) * 2000 / g_quantum_ticks);
    }
    if (seq != ~0u)
        fprintf(g_seq, "r %u %08x\n", seq, eax);

    *g = saved; /* callbacks into the guest during the call ran on this Guest and restored it */
    g->eax = eax;
    g->edx = edx;
    g->esp = args + popped;
    g->fcw = fcw;
    return 1;
}

/* --- import overrides ------------------------------------------------------------------------ */

/* The game must see the same CPU as rt_cpuid: no MMX/SSE/3DNow!. */
static BOOL WINAPI ov_IsProcessorFeaturePresent(DWORD feature)
{
    switch (feature)
    {
    case PF_MMX_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI_INSTRUCTIONS_AVAILABLE:
    case PF_3DNOW_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE:
    case PF_SSE3_INSTRUCTIONS_AVAILABLE:
        return FALSE;
    default:
        return IsProcessorFeaturePresent(feature);
    }
}

/* Guest exceptions must be dispatched along the guest's SEH chain (fs:[0] in the guest TEB), not
 * the host's. Not built yet: stop loudly so we learn whether the game raises any. */
static void WINAPI ov_RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args)
{
    (void)flags; (void)nargs; (void)args;
    char buf[96];
    sprintf(buf, "RaiseException(%08lx) from the guest: guest SEH dispatch is not implemented", code);
    rt_fatal(bridge_guest(), 0, buf);
}

static void WINAPI ov_RtlUnwind(void* frame, void* ip, void* rec, void* ret)
{
    (void)frame; (void)ip; (void)rec; (void)ret;
    rt_fatal(bridge_guest(), 0, "RtlUnwind from the guest: guest SEH unwinding is not implemented");
}

/* A thread killed while it waits for the guest lock leaves its ticket unserved and stalls every
 * other guest thread. Log each use so we learn whether the game ever does that outside shutdown. */
static BOOL WINAPI ov_TerminateThread(HANDLE thread, DWORD code)
{
    rt_log("[recomp] TerminateThread(tid %lu, %lu) from tid %lu\n", GetThreadId(thread), code, GetCurrentThreadId());
    return TerminateThread(thread, code);
}

/* APIs the game resolves at run time get the same overrides as its imports (the CRT fetches
 * IsProcessorFeaturePresent this way). */
static FARPROC WINAPI ov_GetProcAddress(HMODULE mod, LPCSTR name)
{
    if ((uintptr_t)name > 0xFFFF)
        for (const BridgeOverride* o = bridge_overrides; o->name; ++o)
            if (!strcmp(o->name, name))
                return (FARPROC)o->fn;
    return GetProcAddress(mod, name);
}

/* The game reads its own module file: it checksums `.\FFXiMain.dll` (and `.\FFXi.dll`) byte by
 * byte (0x100185b0, called from 0x1001142b) and loads it with LoadLibraryA for its menu string
 * and icon (0x100165c0). Inside the retail launcher that path is now the stand-in, so every access to it by
 * name is redirected to the retail file: the game sees the bytes and resources it shipped with.
 * The Mac platform layer does the same. */
static char g_retail_path[MAX_PATH];
static HMODULE g_retail_module;

void bridge_set_retail(const char* path, HMODULE mapped)
{
    strcpy_s(g_retail_path, sizeof g_retail_path, path);
    g_retail_module = mapped;
}

static int is_game_module(LPCSTR path)
{
    if ((uintptr_t)path <= 0xFFFF || !g_retail_path[0])
        return 0;
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    return !_stricmp(base, "FFXiMain.dll");
}

static HANDLE WINAPI ov_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
    HANDLE tmpl)
{
    return CreateFileA(is_game_module(name) ? g_retail_path : name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI ov_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
{
    if (!is_game_module(name))
        return FindFirstFileA(name, data);
    HANDLE h = FindFirstFileA(g_retail_path, data);
    if (h != INVALID_HANDLE_VALUE)
    {
        strcpy_s(data->cFileName, sizeof data->cFileName, "FFXiMain.dll");
        data->cAlternateFileName[0] = 0;
    }
    return h;
}

static HMODULE WINAPI ov_LoadLibraryA(LPCSTR name)
{
    if (is_game_module(name) && g_retail_module)
        return LoadLibraryExA(g_retail_path, NULL, DONT_RESOLVE_DLL_REFERENCES); /* the mapped retail image, one more reference */
    return LoadLibraryA(name);
}

/* Which translated function a host code address is in (nearest function start below it). */
static uint32_t guest_fn_at(uintptr_t host)
{
    uintptr_t best = 0;
    uint32_t guest = 0;
    for (unsigned i = 0; i < rt_table_count; ++i)
    {
        uintptr_t h = (uintptr_t)rt_table[i].fn;
        if (h <= host && h > best)
        {
            best = h;
            guest = rt_table[i].addr;
        }
    }
    return guest;
}

/* Watchdog: when one thread has held the guest lock for over 5 s while others wait - a stall -
 * log every guest thread: the holder's translated function (its instruction pointer, sampled),
 * and for the rest the native call each is blocked in. Once per stall. */
static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    unsigned long long reported = 0;
    for (;;)
    {
        Sleep(1000);
        unsigned long long at = g_acquired_at;
        DWORD holder = g_holder_tid;
        if (!holder || !rt_lock_contended || __rdtsc() - at < g_quantum_ticks * 2500 || at == reported)
            continue;
        reported = at;
        rt_log("\n[recomp] watchdog: thread %lu has held the guest lock for %llu ms with %u waiting\n", holder,
            (__rdtsc() - at) / (g_quantum_ticks / 2), rt_lock_contended);
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, holder);
        if (h)
        {
            for (int sample = 0; sample < 5; ++sample)
            {
                CONTEXT c;
                c.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(h) != (DWORD)-1)
                {
                    if (GetThreadContext(h, &c))
                        rt_log("  sample %d: eip %08lx in the translation of f_%08x\n", sample, c.Eip, guest_fn_at(c.Eip));
                    ResumeThread(h);
                }
                Sleep(50);
            }
            CloseHandle(h);
        }
        AcquireSRWLockShared(&g_threads_lock);
        ULONGLONG now = GetTickCount64();
        for (unsigned i = 0; i < MAX_GUEST_THREADS; ++i)
        {
            GuestThread* t = g_threads[i];
            if (!t)
                continue;
            uint32_t nt = t->native_target;
            if (t->tid == holder)
                rt_log("  thread %lu: holds the lock; last call boundary esp %08x\n", t->tid, t->g.esp);
            else if (nt)
                rt_log("  thread %lu: in native %08x from guest %08x for %llu ms\n", t->tid, nt, t->native_site, now - t->native_since);
            else
                rt_log("  thread %lu: running or waiting for the lock\n", t->tid);
        }
        ReleaseSRWLockShared(&g_threads_lock);
        bridge_trace_dump();
    }
}

/* --- sampling profiler ------------------------------------------------------------------------
 *
 * Every millisecond, each guest thread is sampled: blocked in a native call (by call target),
 * waiting for the guest lock, or running - suspended just long enough to read its instruction
 * pointer, which is attributed to a translated function (the .xlat section, recomp.py), the
 * runtime (the rest of this module), or a native module. Nothing is allocated and no lock taken
 * while a thread is suspended. Every PROF_WINDOW ms the window's totals are appended to
 * <base>.profile.txt with the frame rate (Present calls), then reset. */
#define PROF_WINDOW 5000u
#define PROF_FNS 8192u
#define PROF_BLOCK 1024u
#define PROF_MODS 256u

typedef struct ProfHost
{
    uintptr_t host;
    uint32_t guest;
} ProfHost;
static ProfHost* g_prof_fns; /* translated functions sorted by host address */
static uintptr_t g_xlat_lo, g_xlat_hi, g_self_lo, g_self_hi;
static TraceSlot g_prof_xlat[PROF_FNS], g_prof_block[PROF_BLOCK];
static TraceSlot g_prof_leaf[PROF_BLOCK]; /* render thread samples inside a leaf call, by target */
static struct
{
    uintptr_t lo, hi;
    char name[48];
    uint32_t count;
} g_prof_mods[PROF_MODS];
static unsigned g_prof_nmods;
static uint32_t g_prof_runtime, g_prof_lockwait, g_prof_unknown, g_prof_samples;
static uint32_t g_render_samples, g_render_lockwait, g_render_blocked; /* the render thread alone */
static char g_prof_path[MAX_PATH];

static int by_host(const void* a, const void* b)
{
    uintptr_t x = ((const ProfHost*)a)->host, y = ((const ProfHost*)b)->host;
    return x < y ? -1 : x > y;
}

static void prof_modules(void)
{
    HMODULE mods[PROF_MODS];
    DWORD need = 0;
    g_prof_nmods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
        return;
    for (unsigned i = 0; i < need / sizeof mods[0] && i < PROF_MODS; ++i)
    {
        MODULEINFO mi;
        char path[MAX_PATH];
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) || !GetModuleFileNameA(mods[i], path, MAX_PATH))
            continue;
        const char* base = strrchr(path, '\\');
        unsigned k = g_prof_nmods++;
        g_prof_mods[k].lo = (uintptr_t)mi.lpBaseOfDll;
        g_prof_mods[k].hi = g_prof_mods[k].lo + mi.SizeOfImage;
        strncpy_s(g_prof_mods[k].name, sizeof g_prof_mods[k].name, base ? base + 1 : path, _TRUNCATE);
        g_prof_mods[k].count = 0;
    }
}

static void prof_count(TraceSlot* table, unsigned size, uint32_t key)
{
    unsigned i = (key * 2654435761u) % size;
    for (unsigned k = 0; k < size; ++k, i = (i + 1) % size)
    {
        if (table[i].count && table[i].target == key)
        {
            table[i].count++;
            return;
        }
        if (!table[i].count)
        {
            table[i].target = key;
            table[i].count = 1;
            return;
        }
    }
}

static void prof_attribute(uintptr_t eip)
{
    if (eip >= g_xlat_lo && eip < g_xlat_hi)
    {
        unsigned lo = 0, hi = rt_table_count;
        while (hi - lo > 1)
        {
            unsigned mid = (lo + hi) / 2;
            if (g_prof_fns[mid].host <= eip)
                lo = mid;
            else
                hi = mid;
        }
        prof_count(g_prof_xlat, PROF_FNS, g_prof_fns[lo].guest);
        return;
    }
    if (eip >= g_self_lo && eip < g_self_hi)
    {
        g_prof_runtime++;
        return;
    }
    for (unsigned i = 0; i < g_prof_nmods; ++i)
        if (eip >= g_prof_mods[i].lo && eip < g_prof_mods[i].hi)
        {
            g_prof_mods[i].count++;
            return;
        }
    g_prof_unknown++; /* runtime-generated code: D3D8's state stubs */
}

static void prof_top(FILE* f, const char* title, TraceSlot* table, unsigned size, unsigned n, int guest)
{
    TraceSlot top[40] = { 0 };
    for (unsigned i = 0; i < size; ++i)
    {
        if (!table[i].count)
            continue;
        for (unsigned k = 0; k < n && k < 40; ++k)
            if (table[i].count > top[k].count)
            {
                memmove(&top[k + 1], &top[k], (n - 1 - k) * sizeof top[0]);
                top[k] = table[i];
                break;
            }
    }
    fprintf(f, "  %s\n", title);
    for (unsigned k = 0; k < n && top[k].count; ++k)
        fprintf(f, guest ? "    %6.2f%%  f_%08x\n" : "    %6.2f%%  %08x\n", 100.0 * top[k].count / g_prof_samples, top[k].target);
}

static void prof_report(unsigned ms, LONG frames)
{
    FILE* f = fopen(g_prof_path, "a");
    if (f && g_prof_samples)
    {
        uint32_t xlat = 0, blocked = 0, native = 0;
        for (unsigned i = 0; i < PROF_FNS; ++i)
            xlat += g_prof_xlat[i].count;
        for (unsigned i = 0; i < PROF_BLOCK; ++i)
            blocked += g_prof_block[i].count;
        for (unsigned i = 0; i < g_prof_nmods; ++i)
            native += g_prof_mods[i].count;
        fprintf(f, "\n== %u ms, %.1f fps, %u thread samples: translated %.1f%%, native %.1f%%, runtime %.1f%%, "
                   "lock wait %.1f%%, generated code %.1f%%, blocked %.1f%%\n",
            ms, frames * 1000.0 / ms, g_prof_samples, 100.0 * xlat / g_prof_samples, 100.0 * native / g_prof_samples,
            100.0 * g_prof_runtime / g_prof_samples, 100.0 * g_prof_lockwait / g_prof_samples,
            100.0 * g_prof_unknown / g_prof_samples, 100.0 * blocked / g_prof_samples);
        if (g_render_samples)
            fprintf(f, "  render thread: running %.1f%%, waiting for the guest lock %.1f%%, blocked %.1f%%\n",
                100.0 * (g_render_samples - g_render_lockwait - g_render_blocked) / g_render_samples,
                100.0 * g_render_lockwait / g_render_samples, 100.0 * g_render_blocked / g_render_samples);
        prof_top(f, "translated functions (self)", g_prof_xlat, PROF_FNS, 30, 1);
        fprintf(f, "  native modules\n");
        for (unsigned i = 0; i < g_prof_nmods; ++i)
            if (g_prof_mods[i].count * 200u >= g_prof_samples)
                fprintf(f, "    %6.2f%%  %s\n", 100.0 * g_prof_mods[i].count / g_prof_samples, g_prof_mods[i].name);
        prof_top(f, "blocked in native call (target)", g_prof_block, PROF_BLOCK, 10, 0);
        prof_top(f, "render thread inside a leaf call, lock held (target)", g_prof_leaf, PROF_BLOCK, 12, 0);
    }
    if (f)
        fclose(f);
    memset(g_prof_xlat, 0, sizeof g_prof_xlat);
    memset(g_prof_block, 0, sizeof g_prof_block);
    memset(g_prof_leaf, 0, sizeof g_prof_leaf);
    g_prof_runtime = g_prof_lockwait = g_prof_unknown = g_prof_samples = 0;
    g_render_samples = g_render_lockwait = g_render_blocked = 0;
    prof_modules();
}

static DWORD WINAPI profiler(LPVOID unused)
{
    (void)unused;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER due;
    due.QuadPart = -10000; /* 1 ms */
    SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE);
    prof_modules();
    ULONGLONG window_start = GetTickCount64();
    LONG frames_start = g_frames;
    HANDLE handles[MAX_GUEST_THREADS];
    for (;;)
    {
        WaitForSingleObject(timer, INFINITE);
        /* snapshot the threads, then sample each without holding the registry lock */
        unsigned n = 0;
        int state[MAX_GUEST_THREADS];
        uint32_t target[MAX_GUEST_THREADS], leaf[MAX_GUEST_THREADS];
        DWORD tids[MAX_GUEST_THREADS];
        AcquireSRWLockShared(&g_threads_lock);
        for (unsigned i = 0; i < MAX_GUEST_THREADS; ++i)
        {
            GuestThread* t = g_threads[i];
            if (!t)
                continue;
            tids[n] = t->tid;
            target[n] = t->native_target;
            leaf[n] = t->leaf_target;
            state[n] = t->waiting_lock ? 1 : target[n] ? 2 : 0;
            n++;
        }
        ReleaseSRWLockShared(&g_threads_lock);
        for (unsigned i = 0; i < n; ++i)
        {
            g_prof_samples++;
            if (tids[i] == g_render_tid)
            {
                g_render_samples++;
                g_render_lockwait += state[i] == 1;
                g_render_blocked += state[i] == 2;
            }
            if (state[i] == 1)
            {
                g_prof_lockwait++;
                continue;
            }
            if (state[i] == 2)
            {
                prof_count(g_prof_block, PROF_BLOCK, target[i]);
                continue;
            }
            if (leaf[i] && tids[i] == g_render_tid)
                prof_count(g_prof_leaf, PROF_BLOCK, leaf[i]);
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tids[i]);
            handles[i] = h;
            if (!h)
                continue;
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            uintptr_t eip = 0;
            if (SuspendThread(h) != (DWORD)-1)
            {
                if (GetThreadContext(h, &c))
                    eip = c.Eip;
                ResumeThread(h);
            }
            CloseHandle(h);
            if (eip)
                prof_attribute(eip);
        }
        ULONGLONG now = GetTickCount64();
        if (now - window_start >= PROF_WINDOW)
        {
            LONG frames = g_frames;
            prof_report((unsigned)(now - window_start), frames - frames_start);
            window_start = now;
            frames_start = frames;
        }
    }
}

static void profiler_start(void)
{
    if (!g_trace_path[0] || g_prof_fns)
        return;
    strcpy_s(g_prof_path, sizeof g_prof_path, g_trace_path);
    char* dot = strstr(g_prof_path, ".trace.txt");
    if (!dot)
        return;
    strcpy_s(dot, sizeof g_prof_path - (dot - g_prof_path), ".profile.txt");
    DeleteFileA(g_prof_path);

    g_prof_fns = (ProfHost*)malloc(rt_table_count * sizeof *g_prof_fns);
    for (unsigned i = 0; i < rt_table_count; ++i)
    {
        g_prof_fns[i].host = (uintptr_t)rt_table[i].fn;
        g_prof_fns[i].guest = rt_table[i].addr;
    }
    qsort(g_prof_fns, rt_table_count, sizeof *g_prof_fns, by_host);

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)profiler_start, &self);
    unsigned char* base = (unsigned char*)self;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    g_self_lo = (uintptr_t)base;
    g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!memcmp(sec[i].Name, ".xlat", 6))
        {
            g_xlat_lo = g_self_lo + sec[i].VirtualAddress;
            g_xlat_hi = g_xlat_lo + sec[i].Misc.VirtualSize;
        }
    if (!g_xlat_lo)
        rt_log("[recomp] profiler: no .xlat section; translated code will count as runtime\n");
    CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
}

void bridge_init(void)
{
    rt_set_yield(lock_yield);
    {
        /* rdtsc ticks per 2 ms, measured against the performance counter */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        unsigned long long t0 = __rdtsc();
        do
            QueryPerformanceCounter(&b);
        while (b.QuadPart - a.QuadPart < f.QuadPart / 50); /* 20 ms */
        g_quantum_ticks = (__rdtsc() - t0) / 10;
        g_slow_ticks = g_quantum_ticks / 4;
    }
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    profiler_start();
    if (g_fls == FLS_OUT_OF_INDEXES)
        g_fls = FlsAlloc(guest_thread_exit);
}

const BridgeOverride bridge_overrides[] = {
    { "CreateFileA", (void*)ov_CreateFileA },
    { "FindFirstFileA", (void*)ov_FindFirstFileA },
    { "LoadLibraryA", (void*)ov_LoadLibraryA },
    { "TerminateThread", (void*)ov_TerminateThread },
    { "GetProcAddress", (void*)ov_GetProcAddress },
    { "IsProcessorFeaturePresent", (void*)ov_IsProcessorFeaturePresent },
    { "RaiseException", (void*)ov_RaiseException },
    { "RtlUnwind", (void*)ov_RtlUnwind },
    { NULL, NULL },
};
