/* KERNEL32 for 64-bit hosts (R3.1): the imports the game's CRT start-up and class factory reach
 * (the R3.0 boundary trace of tests/boot.exe), implemented on the platform layer only, so they
 * run unchanged on macOS. Anything missing traps with its name (thunk.h).
 *
 * What the guest sees: Windows XP SP3 (5.1.2600), code page 1252, a CPU without SIMD (runtime.c),
 * no console. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "kobj.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"

#if defined(_MSC_VER)
#define stricmp_ _stricmp
#else
#include <strings.h>
#define stricmp_ strcasecmp
#endif

#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_ENVVAR_NOT_FOUND 203u

static char g_game_dir[512], g_exe_path[600];

/* --- modules -----------------------------------------------------------------------------------
 * The game's own module is the mapped image (0x10000000). Every other module is a synthetic
 * handle at the top of the window (never mapped), naming the DLL for GetProcAddress. */
#define MODULE_BASE 0xFFFF0000u
#define MODULE_STRIDE 0x100u
#define EXE_HANDLE 0x00400000u

static const char* const KNOWN_DLLS[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ws2_32.dll", "winmm.dll", "ole32.dll",
    "oleaut32.dll", "imm32.dll", "dsound.dll", "dinput8.dll", "d3d8.dll", "version.dll", "shell32.dll",
    "ntdll.dll", "wsock32.dll",
    /* DirectX 8.1's NAT helper: FFXiMain only checks that it loads (0x100160b0), as its
     * "DirectX 8.1 is installed" test, and quits without it */
    "dpnhpast.dll",
    /* the game folder's XInput wrapper; its exports are dinput.c's XInput shims */
    "xinputdll.dll",
    NULL,
};

static const char* base_name(const char* path)
{
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

/* The known DLL a name refers to ("KERNEL32", "kernel32.dll", a full path), or -1. */
static int known_dll(const char* name)
{
    char n[64];
    snprintf(n, sizeof n, "%s", base_name(name));
    if (!strchr(n, '.') && strlen(n) + 4 < sizeof n)
        strcat(n, ".dll");
    for (int i = 0; KNOWN_DLLS[i]; ++i)
        if (!stricmp_(KNOWN_DLLS[i], n))
            return i;
    return -1;
}

/* Translated modules mapped besides FFXiMain (FFXi.dll): their handle is their load base. */
static struct
{
    char name[64];
    uint32_t base;
} g_mods[4];
static unsigned g_nmods;

void k32_add_module(const char* name, uint32_t base)
{
    if (g_nmods < 4)
    {
        snprintf(g_mods[g_nmods].name, sizeof g_mods[0].name, "%s", name);
        g_mods[g_nmods++].base = base;
    }
}

static const char* added_module(uint32_t h)
{
    for (unsigned i = 0; i < g_nmods; ++i)
        if (g_mods[i].base == h)
            return g_mods[i].name;
    return NULL;
}

/* The file name of the translated image: FFXiMain.dll, or the EXE itself (k32_set_image_name). */
static const char* g_image_name = "FFXiMain.dll";

void k32_set_image_name(const char* name) { g_image_name = name; }

static uint32_t module_handle(const char* name)
{
    if (!stricmp_(base_name(name), g_image_name))
        return rt_image_base;
    for (unsigned i = 0; i < g_nmods; ++i)
        if (!stricmp_(base_name(name), g_mods[i].name))
            return g_mods[i].base;
    int k = known_dll(name);
    return k < 0 ? 0 : MODULE_BASE + (uint32_t)k * MODULE_STRIDE;
}

static void sh_GetModuleHandleA(Guest* g)
{
    if (!ARG(0))
        RET(EXE_HANDLE, 1);
    uint32_t h = module_handle(ARGS(0));
    if (!h)
        gt_set_error(ERROR_MOD_NOT_FOUND);
    RET(h, 1);
}

/* whether a missing library was reported already: the game retries some (xinputdll.dll) every
 * few frames */
static int reported_missing(const char* name)
{
    static char seen[16][260];
    static unsigned n;
    for (unsigned i = 0; i < n; ++i)
        if (!strcmp(seen[i], name))
            return 1;
    if (n < 16)
        snprintf(seen[n++], sizeof seen[0], "%s", name);
    return 0;
}

static void sh_LoadLibraryA(Guest* g)
{
    uint32_t h = module_handle(ARGS(0));
    if (!h)
    {
        if (!reported_missing(ARGS(0)))
            rt_log("[recomp] LoadLibraryA(%s): not available on this host (reported once)\n", ARGS(0));
        gt_set_error(ERROR_MOD_NOT_FOUND);
    }
    RET(h, 1);
}

/* LoadLibraryExA(name, hFile, dwFlags): as LoadLibraryA, then its two extra arguments */
static void sh_LoadLibraryExA(Guest* g)
{
    sh_LoadLibraryA(g);
    g->esp += 8;
}

static void sh_FreeLibrary(Guest* g) { RET(1, 1); }

static void sh_GetProcAddress(Guest* g)
{
    uint32_t h = ARG(0), name = ARG(1);
    char ord[16];
    const char* fn = (const char*)GUEST_PTR(name);
    if (name <= 0xFFFF)
    {
        snprintf(ord, sizeof ord, "#%u", name);
        fn = ord;
    }
    if (h == rt_image_base || added_module(h))
        RET(name > 0xFFFF ? pe_export_at(h, fn) : 0, 2);
    if (h < MODULE_BASE || (h - MODULE_BASE) % MODULE_STRIDE || (h - MODULE_BASE) / MODULE_STRIDE >= sizeof KNOWN_DLLS / sizeof KNOWN_DLLS[0] - 1)
    {
        gt_set_error(ERROR_INVALID_PARAMETER);
        RET(0, 2);
    }
    RET(thunk_for(KNOWN_DLLS[(h - MODULE_BASE) / MODULE_STRIDE], fn), 2);
}

static void sh_GetModuleFileNameA(Guest* g)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2);
    char path[700];
    if (h == rt_image_base)
        snprintf(path, sizeof path, "%s\\%s", g_game_dir, g_image_name);
    else if (added_module(h))
        snprintf(path, sizeof path, "%s\\%s", g_game_dir, added_module(h));
    else if (!h || h == EXE_HANDLE)
        snprintf(path, sizeof path, "%s", g_exe_path);
    else
    {
        gt_set_error(ERROR_MOD_NOT_FOUND);
        RET(0, 3);
    }
    uint32_t len = (uint32_t)strlen(path);
    if (!n)
        RET(0, 3);
    uint32_t copy = len < n ? len : n - 1;
    memcpy(GUEST_PTR(buf), path, copy);
    wr8(buf + copy, 0);
    RET(copy, 3);
}

static void sh_DisableThreadLibraryCalls(Guest* g) { RET(1, 1); }

/* --- heaps and pages ---------------------------------------------------------------------------
 * One allocator (gwin.h) behind every heap handle. Handles are small unmapped addresses. */
#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define PROCESS_HEAP 0x000A0000u
static uint32_t g_next_heap = PROCESS_HEAP + 0x1000u;

static void sh_GetProcessHeap(Guest* g) { RET(PROCESS_HEAP, 0); }
static void sh_HeapCreate(Guest* g) { uint32_t h = g_next_heap; g_next_heap += 0x1000u; RET(h, 3); }
static void sh_HeapDestroy(Guest* g) { RET(1, 1); }
static void sh_HeapAlloc(Guest* g) { RET(gheap_alloc(ARG(2), (ARG(1) & HEAP_ZERO_MEMORY) != 0), 3); }
static void sh_HeapFree(Guest* g) { gheap_free(ARG(2)); RET(1, 3); }
static void sh_HeapSize(Guest* g) { RET(gheap_size(ARG(2)), 3); }
static void sh_HeapValidate(Guest* g) { RET(1, 3); }
static void sh_HeapCompact(Guest* g) { RET(0, 2); }

static void sh_HeapReAlloc(Guest* g)
{
    uint32_t flags = ARG(1);
    RET(gheap_realloc(ARG(2), ARG(3), (flags & HEAP_ZERO_MEMORY) != 0, (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0), 4);
}

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u

static void sh_VirtualAlloc(Guest* g)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    uint32_t a = addr;
    if (type & MEM_RESERVE)
    {
        a = gwin_reserve(addr & ~0xFFFFu, size + (addr & 0xFFFFu));
        if (!a)
            RET(0, 4);
        if (addr)
            a = addr & ~0xFFFu;
    }
    if (type & MEM_COMMIT)
    {
        if (!a || !gwin_commit(a, size))
            RET(0, 4);
        a &= ~0xFFFu;
    }
    RET(a, 4);
}

static void sh_VirtualFree(Guest* g)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type & MEM_RELEASE)
        gwin_release(addr);
    else if (type & MEM_DECOMMIT)
        gwin_decommit(addr, size);
    RET(1, 3);
}

static void sh_VirtualProtect(Guest* g)
{
    if (ARG(3))
        wr32(ARG(3), 0x04); /* PAGE_READWRITE */
    RET(1, 4);
}

/* --- thread-local storage, errors, ids --------------------------------------------------------- */
static uint64_t g_tls_used;

static void sh_TlsAlloc(Guest* g)
{
    for (uint32_t i = 0; i < GT_TLS_SLOTS; ++i)
        if (!(g_tls_used & (1ull << i)))
        {
            g_tls_used |= 1ull << i;
            RET(i, 0);
        }
    RET(0xFFFFFFFFu, 0);
}

static void sh_TlsFree(Guest* g) { g_tls_used &= ~(1ull << (ARG(0) & 63)); RET(1, 1); }

static void sh_TlsGetValue(Guest* g)
{
    uint32_t i = ARG(0);
    if (i >= GT_TLS_SLOTS)
    {
        gt_set_error(ERROR_INVALID_PARAMETER);
        RET(0, 1);
    }
    gt_set_error(0); /* TlsGetValue clears the last error on success */
    RET(gt_self()->tls[i], 1);
}

static void sh_TlsSetValue(Guest* g)
{
    uint32_t i = ARG(0);
    if (i >= GT_TLS_SLOTS)
        RET(0, 2);
    gt_self()->tls[i] = ARG(1);
    RET(1, 2);
}

static void sh_GetLastError(Guest* g) { RET(gt_get_error(), 0); }
static void sh_SetLastError(Guest* g) { gt_set_error(ARG(0)); RET(0, 1); }
static void sh_GetCurrentThreadId(Guest* g) { RET(gt_self()->tid, 0); }
static void sh_GetCurrentThread(Guest* g) { RET(0xFFFFFFFEu, 0); }
static void sh_GetCurrentProcess(Guest* g) { RET(0xFFFFFFFFu, 0); }
static void sh_GetCurrentProcessId(Guest* g) { RET(0x1000u, 0); }

/* --- critical sections ---------------------------------------------------------------------------
 * The CRITICAL_SECTION stays in guest memory with its Win32 layout (+8 RecursionCount,
 * +12 OwningThread). Guest threads run in parallel (gthread.c), so the owner is taken with a
 * compare-and-swap; the recursion count is only touched by the owner. A thread that must wait
 * spins briefly, then sleeps on g_cs_epoch, which every release of a contended section advances.
 * These shims run with or without the guest lock. */
static volatile uint32_t g_cs_epoch, g_cs_waiters;

static int cs_try(uint32_t cs, uint32_t tid)
{
    volatile uint32_t* owner = (volatile uint32_t*)GUEST_PTR(cs + 12);
    uint32_t expect = 0;
    if (__atomic_compare_exchange_n(owner, &expect, tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        wr32(cs + 8, 1);
        return 1;
    }
    if (expect == tid)
    {
        wr32(cs + 8, rd32(cs + 8) + 1);
        return 1;
    }
    return 0;
}

static void sh_InitializeCriticalSection(Guest* g)
{
    memset(GUEST_PTR(ARG(0)), 0, 24);
    wr32(ARG(0) + 4, 0xFFFFFFFFu); /* LockCount */
    RET(0, 1);
}

static void sh_InitializeCriticalSectionAndSpinCount(Guest* g)
{
    memset(GUEST_PTR(ARG(0)), 0, 24);
    wr32(ARG(0) + 4, 0xFFFFFFFFu);
    RET(1, 2);
}

static void sh_DeleteCriticalSection(Guest* g) { RET(0, 1); }

static void sh_EnterCriticalSection(Guest* g)
{
    uint32_t cs = ARG(0), tid = gt_self()->tid;
    for (unsigned spin = 0; !cs_try(cs, tid); ++spin)
    {
        if (spin < 100)
        {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
            continue;
        }
        uint32_t e = __atomic_load_n(&g_cs_epoch, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
        if (!cs_try(cs, tid))
        {
            int held = gt_release();
            plat_wait32(&g_cs_epoch, e);
            gt_reacquire(held);
            __atomic_fetch_sub(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        __atomic_fetch_sub(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
        break;
    }
    RET(0, 1);
}

static void sh_TryEnterCriticalSection(Guest* g) { RET(cs_try(ARG(0), gt_self()->tid), 1); }

static void sh_LeaveCriticalSection(Guest* g)
{
    uint32_t cs = ARG(0), n = rd32(cs + 8);
    if (n > 1)
        wr32(cs + 8, n - 1);
    else
    {
        wr32(cs + 8, 0);
        __atomic_store_n((volatile uint32_t*)GUEST_PTR(cs + 12), 0, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_cs_waiters, __ATOMIC_SEQ_CST))
        {
            __atomic_fetch_add(&g_cs_epoch, 1, __ATOMIC_SEQ_CST);
            plat_wake_all32(&g_cs_epoch);
        }
    }
    RET(0, 1);
}

/* --- interlocked: real atomics (guest threads run in parallel) ---------------------------------- */
#define IPTR(a) ((volatile uint32_t*)GUEST_PTR(a))
static void sh_InterlockedIncrement(Guest* g) { RET(__atomic_add_fetch(IPTR(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }
static void sh_InterlockedDecrement(Guest* g) { RET(__atomic_sub_fetch(IPTR(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }
static void sh_InterlockedExchange(Guest* g) { RET(__atomic_exchange_n(IPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }
static void sh_InterlockedExchangeAdd(Guest* g) { RET(__atomic_fetch_add(IPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }

static void sh_InterlockedCompareExchange(Guest* g)
{
    uint32_t expect = ARG(2);
    __atomic_compare_exchange_n(IPTR(ARG(0)), &expect, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(expect, 3); /* the value it held: equal to the comparand when the exchange happened */
}

/* --- code pages and character types: Windows-1252 ----------------------------------------------
 * Bytes map to the code points of the same value (1252's 0x80-0x9F punctuation is not
 * distinguished yet). */
static void sh_GetACP(Guest* g) { RET(1252, 0); }
static void sh_GetOEMCP(Guest* g) { RET(437, 0); }
static void sh_IsValidCodePage(Guest* g) { RET(1, 1); }

static void sh_GetCPInfo(Guest* g)
{
    uint32_t p = ARG(1);
    memset(GUEST_PTR(p), 0, 20);
    wr32(p, 1);         /* MaxCharSize */
    wr8(p + 4, '?');    /* DefaultChar */
    RET(1, 2);
}

static void sh_MultiByteToWideChar(Guest* g)
{
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0)
        n = (int32_t)strlen((const char*)GUEST_PTR(src)) + 1;
    if (!cap)
        RET(n, 6);
    if (cap < n)
    {
        gt_set_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0, 6);
    }
    for (int32_t i = 0; i < n; ++i)
        wr16(dst + 2u * (uint32_t)i, rd8(src + (uint32_t)i));
    RET(n, 6);
}

static void sh_WideCharToMultiByte(Guest* g)
{
    uint32_t src = ARG(2), dst = ARG(4), used = ARG(7);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0)
    {
        n = 0;
        while (rd16(src + 2u * (uint32_t)n))
            n++;
        n++;
    }
    if (used)
        wr32(used, 0);
    if (!cap)
        RET(n, 8);
    if (cap < n)
    {
        gt_set_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0, 8);
    }
    for (int32_t i = 0; i < n; ++i)
    {
        uint16_t w = rd16(src + 2u * (uint32_t)i);
        if (w > 0xFF && used)
            wr32(used, 1);
        wr8(dst + (uint32_t)i, w > 0xFF ? '?' : (uint8_t)w);
    }
    RET(n, 8);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

static unsigned fold(unsigned c, uint32_t flags)
{
    int upper = (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7);
    int lower = (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
    if ((flags & LCMAP_LOWERCASE) && upper)
        return c + 0x20;
    if ((flags & LCMAP_UPPERCASE) && lower)
        return c - 0x20;
    return c;
}

static void lcmap(Guest* g, int wide)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    uint32_t w = wide ? 2u : 1u;
    if (n < 0)
    {
        n = 0;
        while (wide ? rd16(src + 2u * (uint32_t)n) : rd8(src + (uint32_t)n))
            n++;
        n++;
    }
    if (!cap)
        RET(n, 6);
    if (cap < n)
    {
        gt_set_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0, 6);
    }
    for (int32_t i = 0; i < n; ++i)
    {
        unsigned c = wide ? rd16(src + w * (uint32_t)i) : rd8(src + (uint32_t)i);
        c = fold(c, flags);
        if (wide)
            wr16(dst + w * (uint32_t)i, (uint16_t)c);
        else
            wr8(dst + (uint32_t)i, (uint8_t)c);
    }
    RET(n, 6);
}

static void sh_LCMapStringW(Guest* g) { lcmap(g, 1); }
static void sh_LCMapStringA(Guest* g) { lcmap(g, 0); }

/* CT_CTYPE1 classes */
static uint16_t ctype1(unsigned c)
{
    uint16_t t = 0;
    if (c < 0x20 || c == 0x7F)
        t |= 0x20; /* C1_CNTRL */
    if (c == ' ' || (c >= 0x09 && c <= 0x0D))
        t |= 0x08; /* C1_SPACE */
    if (c == ' ' || c == '\t')
        t |= 0x40; /* C1_BLANK */
    if (c >= '0' && c <= '9')
        t |= 0x04 | 0x80; /* C1_DIGIT, C1_XDIGIT */
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        t |= 0x80;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        t |= 0x01 | 0x100; /* C1_UPPER, C1_ALPHA */
    if ((c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7))
        t |= 0x02 | 0x100; /* C1_LOWER, C1_ALPHA */
    if ((c > 0x20 && c < 0x7F && !(t & (0x04 | 0x100))) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7)
        t |= 0x10; /* C1_PUNCT */
    return t;
}

static void string_type(uint32_t type, uint32_t src, int32_t n, uint32_t out, int wide)
{
    if (n < 0)
    {
        n = 0;
        while (wide ? rd16(src + 2u * (uint32_t)n) : rd8(src + (uint32_t)n))
            n++;
        n++;
    }
    for (int32_t i = 0; i < n; ++i)
    {
        unsigned c = wide ? rd16(src + 2u * (uint32_t)i) : rd8(src + (uint32_t)i);
        wr16(out + 2u * (uint32_t)i, type == 1 && c <= 0xFF ? ctype1(c) : 0);
    }
}

static void sh_GetStringTypeW(Guest* g) { string_type(ARG(0), ARG(1), (int32_t)ARG(2), ARG(3), 1); RET(1, 4); }
static void sh_GetStringTypeA(Guest* g) { string_type(ARG(1), ARG(2), (int32_t)ARG(3), ARG(4), 0); RET(1, 5); }

/* --- process environment ------------------------------------------------------------------------ */
static uint32_t g_cmdline, g_env_a, g_env_w;

static const char* g_cmdline_args; /* after the quoted exe path (k32_set_command_line) */

void k32_set_command_line(const char* args) { g_cmdline_args = args; }

static void sh_GetCommandLineA(Guest* g)
{
    if (!g_cmdline)
    {
        char s[1024];
        snprintf(s, sizeof s, "\"%s\"%s%s", g_exe_path, g_cmdline_args ? " " : "", g_cmdline_args ? g_cmdline_args : "");
        g_cmdline = gheap_strdup(s);
    }
    RET(g_cmdline, 0);
}

static void sh_GetEnvironmentStrings(Guest* g)
{
    if (!g_env_a)
        g_env_a = gheap_alloc(2, 1);
    RET(g_env_a, 0);
}

static void sh_GetEnvironmentStringsW(Guest* g)
{
    if (!g_env_w)
        g_env_w = gheap_alloc(4, 1);
    RET(g_env_w, 0);
}

static void sh_FreeEnvironmentStrings(Guest* g) { RET(1, 1); }
static void sh_GetEnvironmentVariableA(Guest* g) { gt_set_error(ERROR_ENVVAR_NOT_FOUND); RET(0, 3); }

static void sh_GetStartupInfoA(Guest* g)
{
    memset(ARGP(0), 0, 68);
    wr32(ARG(0), 68);
    RET(0, 1);
}

static void sh_GetStdHandle(Guest* g) { RET(0, 1); } /* a GUI process: no console */
static void sh_SetHandleCount(Guest* g) { RET(ARG(0), 1); }

/* Windows XP SP3 */
static void sh_GetVersion(Guest* g) { RET((2600u << 16) | (1u << 8) | 5u, 0); }

static void sh_GetVersionExA(Guest* g)
{
    uint32_t p = ARG(0), size = rd32(p);
    if (size != 148 && size != 156)
    {
        gt_set_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0, 1);
    }
    memset(GUEST_PTR(p + 4), 0, size - 4);
    wr32(p + 4, 5);     /* major */
    wr32(p + 8, 1);     /* minor */
    wr32(p + 12, 2600); /* build */
    wr32(p + 16, 2);    /* VER_PLATFORM_WIN32_NT */
    strcpy((char*)GUEST_PTR(p + 20), "Service Pack 3");
    if (size == 156)
        wr16(p + 148, 3); /* wServicePackMajor */
    RET(1, 1);
}

static uint32_t g_top_filter;
static void sh_SetUnhandledExceptionFilter(Guest* g) { uint32_t old = g_top_filter; g_top_filter = ARG(0); RET(old, 1); }
static void sh_IsProcessorFeaturePresent(Guest* g) { RET(0, 1); } /* the same CPU as rt_cpuid: no SIMD */
static void sh_OutputDebugStringA(Guest* g) { plat_debug(ARGS(0)); RET(0, 1); }
static void sh_ExitProcess(Guest* g) { rt_log("[recomp] ExitProcess(%u)\n", ARG(0)); exit((int)ARG(0)); }

/* --- time --------------------------------------------------------------------------------------- */
/* GetTickCount starts at a value the host may choose (k32_set_tick_base): ffxivgame.exe decrypts
 * its launch argument with a key derived from it. */
static uint32_t g_tick_base;
static uint64_t g_tick_start_ns;

void k32_set_tick_base(uint32_t tick)
{
    g_tick_base = tick;
    g_tick_start_ns = rt_monotonic_ns();
}

static void sh_GetTickCount(Guest* g)
{
    if (g_tick_start_ns)
        RET(g_tick_base + (uint32_t)((rt_monotonic_ns() - g_tick_start_ns) / 1000000u), 0);
    RET((uint32_t)(rt_monotonic_ns() / 1000000u), 0);
}

static void sh_QueryPerformanceFrequency(Guest* g)
{
    wr64(ARG(0), 10000000u); /* 100 ns ticks */
    RET(1, 1);
}

/* A frame clock: QueryPerformanceCounter called from one place (a game's frame-delta read) moves
 * in whole display refreshes, staying within half of one of the real clock. Motion is integrated
 * over the delta, and a frame is on screen a whole number of refreshes: a delta measured on the CPU
 * between frames the display shows evenly moved things unevenly. */
static uint32_t g_qpc_at;
static uint64_t (*g_qpc_quantum)(void);

void k32_quantize_qpc_at(uint32_t ret, uint64_t (*quantum_ns)(void))
{
    g_qpc_at = ret;
    g_qpc_quantum = quantum_ns;
}

static uint64_t frame_clock(uint64_t now)
{
    static uint64_t virt;
    uint64_t q = g_qpc_quantum();
    if (!q || !virt || now < virt || now - virt > 1000000000ull)
        return virt = now;
    virt += (now - virt + q / 2) / q * q;
    return virt;
}

static void sh_QueryPerformanceCounter(Guest* g)
{
    uint64_t now = rt_monotonic_ns();
    if (g_qpc_at && rd32(g->esp) == g_qpc_at)
        now = frame_clock(now);
    wr64(ARG(0), now / 100u);
    RET(1, 1);
}

static void sh_GetSystemTimeAsFileTime(Guest* g)
{
    wr64(ARG(0), plat_wall_ms() * 10000u + 116444736000000000ull);
    RET(0, 1);
}

/* Sleeps the host lifts: by the address the call returns to (a game's frame limiter, say) */
static uint32_t g_no_sleep[8];
static int g_nno_sleep;

void k32_skip_sleep_at(uint32_t ret)
{
    if (g_nno_sleep < (int)(sizeof g_no_sleep / sizeof g_no_sleep[0]))
        g_no_sleep[g_nno_sleep++] = ret;
}

/* Sleeps the host paces itself: a loop's limiter whose whole-millisecond sleeps would make its
 * cadence uneven sleeps instead until exactly one period after it last woke there */
static uint32_t g_paced[4];
static int g_npaced;
static uint64_t g_pace_ns, g_paced_woke;

void k32_pace_sleep_at(uint32_t ret, uint64_t period_ns)
{
    if (g_npaced < (int)(sizeof g_paced / sizeof g_paced[0]))
        g_paced[g_npaced++] = ret;
    g_pace_ns = period_ns;
}

/* XIV_FRAMELOG: the paced loop's tick-to-tick times, every 600 ticks */
static void tick_log(uint64_t ns)
{
    static int on = -1;
    static uint64_t n, sum, lo = ~0ull, hi, late;
    if (on < 0)
        on = getenv("XIV_FRAMELOG") && strcmp(getenv("XIV_FRAMELOG"), "0");
    if (!on || !ns)
        return;
    n++, sum += ns, lo = ns < lo ? ns : lo, hi = ns > hi ? ns : hi;
    if (ns > g_pace_ns + g_pace_ns / 4)
        late++;
    if (n == 600)
    {
        rt_log("[pace] tick ms: period %.2f mean %.2f min %.2f max %.2f, %llu late by over a quarter period\n",
               g_pace_ns / 1e6, sum / 600 / 1e6, lo / 1e6, hi / 1e6, (unsigned long long)late);
        n = sum = hi = late = 0, lo = ~0ull;
    }
}

static int paced_sleep(uint32_t ret)
{
    for (int i = 0; i < g_npaced; ++i)
        if (g_paced[i] == ret)
        {
            uint64_t now = rt_monotonic_ns(), until = g_paced_woke + g_pace_ns;
            int held = gt_release();
            if (g_paced_woke && until > now && until - now <= g_pace_ns)
            {
                struct timespec t = { (time_t)((until - now) / 1000000000u), (long)((until - now) % 1000000000u) };
                while (nanosleep(&t, &t) != 0)
                    ;
            }
            else
                plat_yield();
            uint64_t woke = rt_monotonic_ns();
            tick_log(g_paced_woke ? woke - g_paced_woke : 0);
            g_paced_woke = woke;
            gt_reacquire(held);
            return 1;
        }
    return 0;
}

static void sh_Sleep(Guest* g)
{
    uint32_t ms = ARG(0), ret = rd32(g->esp);
    /* XIV_PROF=logic: the shim-time profile follows the logic loop's thread (its limiter sleeps here) */
    static int prof_logic = -1;
    if (prof_logic < 0)
        prof_logic = getenv("XIV_PROF") && !strcmp(getenv("XIV_PROF"), "logic");
    if (prof_logic == 1 && (ret == 0xcc6d4c || ret == 0xcc6d62))
    {
        extern uint32_t thunk_prof_thread;
        thunk_prof_thread = plat_thread_id();
        prof_logic = 2;
    }
    if (paced_sleep(ret))
        RET(0, 1);
    for (int i = 0; i < g_nno_sleep; ++i)
        if (g_no_sleep[i] == ret)
            ms = 0;
    int held = gt_release();
    if (ms)
        plat_sleep_ms(ms);
    else
        plat_yield();
    gt_reacquire(held);
    RET(0, 1);
}

/* --- lstr*: the C string functions KERNEL32 exports (code page 1252: bytes) ---------------------- */
static int ci(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }

static void sh_lstrlenA(Guest* g) { RET(ARG(0) ? (uint32_t)strlen(ARGS(0)) : 0, 1); }
static void sh_lstrcpyA(Guest* g) { strcpy((char*)ARGP(0), ARGS(1)); RET(ARG(0), 2); }
static void sh_lstrcatA(Guest* g) { strcat((char*)ARGP(0), ARGS(1)); RET(ARG(0), 2); }

static void sh_lstrcpynA(Guest* g)
{
    int32_t n = (int32_t)ARG(2);
    if (n > 0)
    {
        strncpy((char*)ARGP(0), ARGS(1), (size_t)n - 1);
        ((char*)ARGP(0))[n - 1] = 0;
    }
    RET(ARG(0), 3);
}

static void sh_lstrcmpA(Guest* g)
{
    int r = strcmp(ARGS(0), ARGS(1));
    RET((uint32_t)(r < 0 ? -1 : r > 0), 2);
}

static void sh_lstrcmpiA(Guest* g)
{
    const unsigned char *a = (const unsigned char*)ARGS(0), *b = (const unsigned char*)ARGS(1);
    while (*a && ci(*a) == ci(*b))
        a++, b++;
    int r = ci(*a) - ci(*b);
    RET((uint32_t)(r < 0 ? -1 : r > 0), 2);
}

static void sh_lstrlenW(Guest* g)
{
    uint32_t s = ARG(0), n = 0;
    while (s && rd16(s + 2 * n))
        n++;
    RET(n, 1);
}

static const ShimDef K32[] = {
    { "kernel32.dll", "lstrlenA", sh_lstrlenA },
    { "kernel32.dll", "lstrcpyA", sh_lstrcpyA },
    { "kernel32.dll", "lstrcatA", sh_lstrcatA },
    { "kernel32.dll", "lstrcpynA", sh_lstrcpynA },
    { "kernel32.dll", "lstrcmpA", sh_lstrcmpA },
    { "kernel32.dll", "lstrcmpiA", sh_lstrcmpiA },
    { "kernel32.dll", "lstrlenW", sh_lstrlenW },
    { "kernel32.dll", "GetModuleHandleA", sh_GetModuleHandleA },
    { "kernel32.dll", "LoadLibraryA", sh_LoadLibraryA },
    { "kernel32.dll", "LoadLibraryExA", sh_LoadLibraryExA },
    { "kernel32.dll", "FreeLibrary", sh_FreeLibrary },
    { "kernel32.dll", "GetProcAddress", sh_GetProcAddress },
    { "kernel32.dll", "GetModuleFileNameA", sh_GetModuleFileNameA },
    { "kernel32.dll", "DisableThreadLibraryCalls", sh_DisableThreadLibraryCalls },
    { "kernel32.dll", "GetProcessHeap", sh_GetProcessHeap },
    { "kernel32.dll", "HeapCreate", sh_HeapCreate },
    { "kernel32.dll", "HeapDestroy", sh_HeapDestroy },
    { "kernel32.dll", "HeapAlloc", sh_HeapAlloc },
    { "kernel32.dll", "HeapFree", sh_HeapFree },
    { "kernel32.dll", "HeapReAlloc", sh_HeapReAlloc },
    { "kernel32.dll", "HeapSize", sh_HeapSize },
    { "kernel32.dll", "HeapValidate", sh_HeapValidate },
    { "kernel32.dll", "HeapCompact", sh_HeapCompact },
    { "kernel32.dll", "VirtualAlloc", sh_VirtualAlloc },
    { "kernel32.dll", "VirtualFree", sh_VirtualFree },
    { "kernel32.dll", "VirtualProtect", sh_VirtualProtect },
    { "kernel32.dll", "TlsAlloc", sh_TlsAlloc },
    { "kernel32.dll", "TlsFree", sh_TlsFree },
    { "kernel32.dll", "TlsGetValue", sh_TlsGetValue },
    { "kernel32.dll", "TlsSetValue", sh_TlsSetValue },
    { "kernel32.dll", "GetLastError", sh_GetLastError },
    { "kernel32.dll", "SetLastError", sh_SetLastError },
    { "kernel32.dll", "GetCurrentThreadId", sh_GetCurrentThreadId },
    { "kernel32.dll", "GetCurrentThread", sh_GetCurrentThread },
    { "kernel32.dll", "GetCurrentProcess", sh_GetCurrentProcess },
    { "kernel32.dll", "GetCurrentProcessId", sh_GetCurrentProcessId },
    { "kernel32.dll", "InitializeCriticalSection", sh_InitializeCriticalSection },
    { "kernel32.dll", "InitializeCriticalSectionAndSpinCount", sh_InitializeCriticalSectionAndSpinCount },
    { "kernel32.dll", "DeleteCriticalSection", sh_DeleteCriticalSection },
    { "kernel32.dll", "EnterCriticalSection", sh_EnterCriticalSection },
    { "kernel32.dll", "TryEnterCriticalSection", sh_TryEnterCriticalSection },
    { "kernel32.dll", "LeaveCriticalSection", sh_LeaveCriticalSection },
    { "kernel32.dll", "InterlockedIncrement", sh_InterlockedIncrement },
    { "kernel32.dll", "InterlockedDecrement", sh_InterlockedDecrement },
    { "kernel32.dll", "InterlockedExchange", sh_InterlockedExchange },
    { "kernel32.dll", "InterlockedExchangeAdd", sh_InterlockedExchangeAdd },
    { "kernel32.dll", "InterlockedCompareExchange", sh_InterlockedCompareExchange },
    { "kernel32.dll", "GetACP", sh_GetACP },
    { "kernel32.dll", "GetOEMCP", sh_GetOEMCP },
    { "kernel32.dll", "IsValidCodePage", sh_IsValidCodePage },
    { "kernel32.dll", "GetCPInfo", sh_GetCPInfo },
    { "kernel32.dll", "MultiByteToWideChar", sh_MultiByteToWideChar },
    { "kernel32.dll", "WideCharToMultiByte", sh_WideCharToMultiByte },
    { "kernel32.dll", "LCMapStringW", sh_LCMapStringW },
    { "kernel32.dll", "LCMapStringA", sh_LCMapStringA },
    { "kernel32.dll", "GetStringTypeW", sh_GetStringTypeW },
    { "kernel32.dll", "GetStringTypeA", sh_GetStringTypeA },
    { "kernel32.dll", "GetCommandLineA", sh_GetCommandLineA },
    { "kernel32.dll", "GetEnvironmentStrings", sh_GetEnvironmentStrings },
    { "kernel32.dll", "GetEnvironmentStringsA", sh_GetEnvironmentStrings },
    { "kernel32.dll", "GetEnvironmentStringsW", sh_GetEnvironmentStringsW },
    { "kernel32.dll", "FreeEnvironmentStringsA", sh_FreeEnvironmentStrings },
    { "kernel32.dll", "FreeEnvironmentStringsW", sh_FreeEnvironmentStrings },
    { "kernel32.dll", "GetEnvironmentVariableA", sh_GetEnvironmentVariableA },
    { "kernel32.dll", "GetStartupInfoA", sh_GetStartupInfoA },
    { "kernel32.dll", "GetStdHandle", sh_GetStdHandle },
    { "kernel32.dll", "SetHandleCount", sh_SetHandleCount },
    { "kernel32.dll", "GetVersion", sh_GetVersion },
    { "kernel32.dll", "GetVersionExA", sh_GetVersionExA },
    { "kernel32.dll", "SetUnhandledExceptionFilter", sh_SetUnhandledExceptionFilter },
    { "kernel32.dll", "IsProcessorFeaturePresent", sh_IsProcessorFeaturePresent },
    { "kernel32.dll", "OutputDebugStringA", sh_OutputDebugStringA },
    { "kernel32.dll", "ExitProcess", sh_ExitProcess },
    { "kernel32.dll", "GetTickCount", sh_GetTickCount },
    { "kernel32.dll", "QueryPerformanceFrequency", sh_QueryPerformanceFrequency },
    { "kernel32.dll", "QueryPerformanceCounter", sh_QueryPerformanceCounter },
    { "kernel32.dll", "GetSystemTimeAsFileTime", sh_GetSystemTimeAsFileTime },
    { "kernel32.dll", "Sleep", sh_Sleep },
    { NULL, NULL, NULL },
};

void k32_init(const char* game_dir, const char* exe_path)
{
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game_dir);
    snprintf(g_exe_path, sizeof g_exe_path, "%s", exe_path);
    thunk_register(K32);
}

/* --- imports the translator inlines at their call sites (recomp/x86c.py INLINE_IMPORTS): the hot
 * ones, called millions of times a second, without the thunk dispatch -------------------------- */
void xi_cs_enter(uint32_t cs)
{
    uint32_t tid = gt_self()->tid;
    if (cs_try(cs, tid))
        return;
    for (unsigned spin = 0; !cs_try(cs, tid); ++spin)
    {
        if (spin < 100)
        {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
            continue;
        }
        uint32_t e = __atomic_load_n(&g_cs_epoch, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
        if (!cs_try(cs, tid))
        {
            int held = gt_release();
            plat_wait32(&g_cs_epoch, e);
            gt_reacquire(held);
            __atomic_fetch_sub(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        __atomic_fetch_sub(&g_cs_waiters, 1, __ATOMIC_SEQ_CST);
        return;
    }
}

void xi_cs_leave(uint32_t cs)
{
    uint32_t n = rd32(cs + 8);
    if (n > 1)
    {
        wr32(cs + 8, n - 1);
        return;
    }
    wr32(cs + 8, 0);
    __atomic_store_n((volatile uint32_t*)GUEST_PTR(cs + 12), 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_cs_waiters, __ATOMIC_SEQ_CST))
    {
        __atomic_fetch_add(&g_cs_epoch, 1, __ATOMIC_SEQ_CST);
        plat_wake_all32(&g_cs_epoch);
    }
}

uint32_t xi_tls_get(uint32_t i)
{
    if (i >= GT_TLS_SLOTS)
    {
        gt_set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    GThread* t = gt_self();
    wr32(t->teb + 0x34, 0);
    return t->tls[i];
}

uint32_t xi_tls_set(uint32_t i, uint32_t v)
{
    if (i >= GT_TLS_SLOTS)
        return 0;
    gt_self()->tls[i] = v;
    return 1;
}

uint32_t xi_get_error(void) { return gt_get_error(); }
void xi_set_error(uint32_t e) { gt_set_error(e); }
uint32_t xi_wait1(uint32_t h, uint32_t ms) { return k_wait(&h, 1, 0, ms); }
uint32_t xi_set_event(uint32_t h, int state) { return (uint32_t)k_event_set(h, state); }
uint32_t xi_release_semaphore(uint32_t h, uint32_t n, uint32_t prev)
{
    int32_t p = 0;
    int ok = k_semaphore_release(h, (int32_t)n, &p);
    if (ok && prev)
        wr32(prev, (uint32_t)p);
    return (uint32_t)ok;
}
