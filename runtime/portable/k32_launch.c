/* kernel32, advapi32, comctl32 and shell32 calls the launcher programs (ffxivboot.exe,
 * ffxivlogin.exe, ffxivupdater.exe) make that the game does not: wide (W) forms of calls the A
 * shims implement, as in k32_xiv.c, and the few the patch installer adds (FormatMessageW with an
 * allocated buffer, file identity, registry writes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"

#define ERROR_INVALID_HANDLE 6u

static uint32_t wlen(uint32_t w)
{
    uint32_t n = 0;
    while (w && rd16(w + 2 * n))
        n++;
    return n;
}

/* A guest copy of a UTF-16 string, narrowed (characters past 0xFF become '?'); 0 for NULL or an
 * integer atom/resource id (below 0x10000). */
static uint32_t narrow(uint32_t w)
{
    if (w < 0x10000u)
        return w;
    uint32_t n = wlen(w), a = gheap_alloc(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(a + n, 0);
    return a;
}

static void unnarrow(uint32_t a)
{
    if (a >= 0x10000u)
        gheap_free(a);
}

/* Widens the ANSI string at a into w, at most cap characters including the terminator. */
static void widen(uint32_t a, uint32_t w, uint32_t cap)
{
    if (!cap)
        return;
    uint32_t i = 0;
    for (; i + 1 < cap; ++i)
    {
        uint8_t c = rd8(a + i);
        if (!c)
            break;
        wr16(w + 2 * i, c);
    }
    wr16(w + 2 * i, 0);
}

static void forward(Guest* g, const char* dll, const char* name)
{
    Shim s = thunk_shim(dll, name);
    if (!s)
        rt_fatal(g, 0, name);
    s(g);
}

/* Runs dll!a_name with the string arguments named by `mask` (bit n: argument n) narrowed. */
static void forward_strings(Guest* g, const char* dll, const char* a_name, unsigned mask)
{
    uint32_t tmp[12] = { 0 };
    uint32_t args = g->esp + 4;
    for (unsigned n = 0; n < 12; ++n)
        if (mask & (1u << n))
        {
            tmp[n] = narrow(rd32(args + 4 * n));
            wr32(args + 4 * n, tmp[n]);
        }
    forward(g, dll, a_name);
    for (unsigned n = 0; n < 12; ++n)
        unnarrow(tmp[n]);
}

/* --- kernel32 ------------------------------------------------------------------------------------ */

static void sh_GetStartupInfoW(Guest* g) { forward(g, "kernel32.dll", "GetStartupInfoA"); }

static void sh_GetCommandLineW(Guest* g)
{
    static uint32_t w;
    if (!w)
    {
        forward(g, "kernel32.dll", "GetCommandLineA");
        g->esp -= 4; /* GetCommandLineA popped the return address: put it back for RET below */
        uint32_t a = g->eax, n = 0;
        while (rd8(a + n))
            n++;
        w = gheap_alloc(2 * (n + 1), 1);
        widen(a, w, n + 1);
    }
    RET(w, 0);
}

static void sh_GetModuleHandleW(Guest* g) { forward_strings(g, "kernel32.dll", "GetModuleHandleA", 1); }
static void sh_LoadLibraryExW(Guest* g) { forward_strings(g, "kernel32.dll", "LoadLibraryExA", 1); }

/* LocalAlloc'd memory is the guest heap here (FormatMessageW's FORMAT_MESSAGE_ALLOCATE_BUFFER) */
static void sh_LocalFree(Guest* g)
{
    if (ARG(0))
        gheap_free(ARG(0));
    RET(0, 1);
}

static void sh_LocalAlloc(Guest* g) { RET(gheap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40u) != 0), 2); }

static void sh_lstrcmpiW(Guest* g)
{
    uint32_t a = ARG(0), b = ARG(1);
    for (uint32_t i = 0;; ++i)
    {
        uint16_t x = rd16(a + 2 * i), y = rd16(b + 2 * i);
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y || !x)
            RET(x < y ? (uint32_t)-1 : x > y ? 1u : 0u, 2);
    }
}

/* FormatMessageW(flags, source, id, lang, buffer, size, args): no system message table, so the
 * text is the number. FORMAT_MESSAGE_ALLOCATE_BUFFER (0x100): buffer receives a pointer. */
static void sh_FormatMessageW(Guest* g)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), size = ARG(5);
    char text[48];
    int n = snprintf(text, sizeof text, "Error %u (0x%08X).\r\n", id, id);
    uint32_t out = buf;
    if (flags & 0x100u)
    {
        out = gheap_alloc(2 * ((uint32_t)n + 1), 1);
        wr32(buf, out);
    }
    else if (size < (uint32_t)n + 1)
        RET(0, 7);
    for (int i = 0; i <= n; ++i)
        wr16(out + 2 * (uint32_t)i, (uint8_t)text[i]);
    RET((uint32_t)n, 7);
}

static void sh_GetThreadLocale(Guest* g) { RET(0x0409u, 0); } /* en-US */
static void sh_FlushInstructionCache(Guest* g) { RET(1, 3); } /* ATL thunks: runtime.c reads them */

/* GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*): attributes, size, one link. The
 * volume serial and file index make the identity; the handle value stands in for the index. */
static void sh_GetFileInformationByHandle(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    uint32_t p = ARG(1);
    if (!f)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0, 2);
    }
    int64_t s = plat_file_size(f);
    memset(GUEST_PTR(p), 0, 52);
    wr32(p, 0x80u);                         /* FILE_ATTRIBUTE_NORMAL */
    wr32(p + 28, 0x1234ABCDu);              /* dwVolumeSerialNumber */
    wr32(p + 32, (uint32_t)((uint64_t)s >> 32));
    wr32(p + 36, (uint32_t)s);
    wr32(p + 40, 1);                        /* nNumberOfLinks */
    wr32(p + 48, ARG(0));                   /* nFileIndexLow */
    gt_set_error(0);
    RET(1, 2);
}

/* --- advapi32: registry writes ----------------------------------------------------------------- */

/* RegCreateKeyExW(key, sub, reserved, class, options, sam, security, result, disposition) */
static void sh_RegCreateKeyExW(Guest* g) { forward_strings(g, "advapi32.dll", "RegCreateKeyExA", 2 | 8); }
static void sh_RegDeleteKeyW(Guest* g) { forward_strings(g, "advapi32.dll", "RegDeleteKeyA", 2); }
static void sh_RegDeleteValueW(Guest* g) { forward_strings(g, "advapi32.dll", "RegDeleteValueA", 2); }

/* RegSetValueExW(key, name, reserved, type, data, cb): REG_SZ data is narrowed with its size */
static void sh_RegSetValueExW(Guest* g)
{
    uint32_t type = ARG(3), data = ARG(4), sdata = 0;
    if ((type == 1 || type == 2) && data)
    {
        sdata = narrow(data);
        wr32(g->esp + 20, sdata);
        wr32(g->esp + 24, wlen(data) + 1);
    }
    forward_strings(g, "advapi32.dll", "RegSetValueExA", 2);
    unnarrow(sdata);
}

/* RegEnumKeyExW(key, index, name, cch*, reserved, class, cchclass*, time) */
static void sh_RegEnumKeyExW(Guest* g)
{
    uint32_t name = ARG(2), cchp = ARG(3), cch = cchp ? rd32(cchp) : 0;
    uint32_t tmp = gheap_alloc(cch + 1, 1);
    wr32(g->esp + 12, tmp);
    wr32(g->esp + 24, 0); /* no class */
    wr32(g->esp + 28, 0);
    forward(g, "advapi32.dll", "RegEnumKeyExA");
    if (g->eax == 0 && name)
        widen(tmp, name, cch);
    gheap_free(tmp);
}

/* RegQueryInfoKeyW: the class buffer (argument 1) is not filled */
static void sh_RegQueryInfoKeyW(Guest* g)
{
    if (ARG(1) && ARG(2) && rd32(ARG(2)))
        wr16(ARG(1), 0);
    wr32(g->esp + 8, 0);
    wr32(g->esp + 12, 0);
    forward(g, "advapi32.dll", "RegQueryInfoKeyA");
}

/* --- comctl32, shell32 ----------------------------------------------------------------------------- */

static void sh_InitCommonControlsEx(Guest* g) { RET(1, 1); }

/* ShellExecuteExW(SHELLEXECUTEINFOW*): the updater starts its RebootModule this way. Programs are
 * started by k32_launch_hook when the host sets one; anything else is logged and refused. */
int (*k32_launch_hook)(const char* file, const char* params, const char* dir);

static char* host_narrow(uint32_t w)
{
    uint32_t n = wlen(w);
    char* s = malloc(n + 1);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        s[i] = c > 0x7F ? '?' : (char)c;
    }
    s[n] = 0;
    return s;
}

static void sh_ShellExecuteExW(Guest* g)
{
    uint32_t p = ARG(0);
    char* file = host_narrow(rd32(p + 16));
    char* params = host_narrow(rd32(p + 20));
    char* dir = host_narrow(rd32(p + 24));
    int ok = k32_launch_hook ? k32_launch_hook(file, params, dir) : 0;
    rt_log("[recomp] ShellExecuteExW(%s, %.60s%s, in %s): %s\n", file, params, strlen(params) > 60 ? "..." : "", dir,
        ok ? "started" : "not started");
    wr32(p + 32, ok ? 33u : 2u); /* hInstApp: > 32 success, SE_ERR_FNF */
    wr32(p + 56, 0);             /* hProcess */
    free(file);
    free(params);
    free(dir);
    gt_set_error(ok ? 0 : 2);
    RET(ok, 1);
}

static const ShimDef k_shims[] = {
    { "kernel32.dll", "GetStartupInfoW", sh_GetStartupInfoW },
    { "kernel32.dll", "GetCommandLineW", sh_GetCommandLineW },
    { "kernel32.dll", "GetModuleHandleW", sh_GetModuleHandleW },
    { "kernel32.dll", "LoadLibraryExW", sh_LoadLibraryExW },
    { "kernel32.dll", "LocalFree", sh_LocalFree },
    { "kernel32.dll", "LocalAlloc", sh_LocalAlloc },
    { "kernel32.dll", "lstrcmpiW", sh_lstrcmpiW },
    { "kernel32.dll", "FormatMessageW", sh_FormatMessageW },
    { "kernel32.dll", "GetThreadLocale", sh_GetThreadLocale },
    { "kernel32.dll", "FlushInstructionCache", sh_FlushInstructionCache },
    { "kernel32.dll", "GetFileInformationByHandle", sh_GetFileInformationByHandle },
    { "advapi32.dll", "RegCreateKeyExW", sh_RegCreateKeyExW },
    { "advapi32.dll", "RegDeleteKeyW", sh_RegDeleteKeyW },
    { "advapi32.dll", "RegDeleteValueW", sh_RegDeleteValueW },
    { "advapi32.dll", "RegSetValueExW", sh_RegSetValueExW },
    { "advapi32.dll", "RegEnumKeyExW", sh_RegEnumKeyExW },
    { "advapi32.dll", "RegQueryInfoKeyW", sh_RegQueryInfoKeyW },
    { "comctl32.dll", "InitCommonControlsEx", sh_InitCommonControlsEx },
    { "shell32.dll", "ShellExecuteExW", sh_ShellExecuteExW },
    { NULL, NULL, NULL },
};

void k32_launch_init(void) { thunk_register(k_shims); }
