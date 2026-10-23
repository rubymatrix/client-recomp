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
static void sh_ShellExecuteExW(Guest* g)
{
    uint32_t p = ARG(0);
    char* file = host_narrow(rd32(p + 16));
    char* params = host_narrow(rd32(p + 20));
    char* dir = host_narrow(rd32(p + 24));
    int ok = k32_launch_hook ? k32_launch_hook(file, params, dir) : 0;
    rt_log("[recomp] ShellExecuteExW(%s, %s, in %s): %s\n", file, params, dir, ok ? "started" : "not started");
    wr32(p + 32, ok ? 33u : 2u); /* hInstApp: > 32 success, SE_ERR_FNF */
    wr32(p + 56, 0);             /* hProcess */
    free(file);
    free(params);
    free(dir);
    gt_set_error(ok ? 0 : 2);
    RET(ok, 1);
}


/* --- ffxivlogin's MFC: kernel32 ------------------------------------------------------------------ */

/* side-by-side activation contexts (MFC asks for comctl32 v6 through its manifest): none here */
static void sh_CreateActCtxW(Guest* g) { gt_set_error(1814); RET(0xFFFFFFFFu, 1); }
static void sh_ActivateActCtx(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(1, 2); }
static void sh_DeactivateActCtx(Guest* g) { RET(1, 2); }
static void sh_ReleaseActCtx(Guest* g) { RET(0, 1); }

/* IsDBCSLeadByteEx(codepage, byte): Shift-JIS (932) lead bytes; the single-byte pages have none */
static void sh_IsDBCSLeadByteEx(Guest* g)
{
    uint32_t b = ARG(1) & 0xFF;
    RET(ARG(0) == 932 && ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)), 2);
}

/* ExpandEnvironmentStringsA(src, dst, n): %NAME% from the host's environment */
static void sh_ExpandEnvironmentStringsA(Guest* g)
{
    const char* src = (const char*)GUEST_PTR(ARG(0));
    char out[4096];
    size_t k = 0;
    for (const char* p = src; *p && k < sizeof out - 1;)
    {
        const char* end = *p == '%' ? strchr(p + 1, '%') : NULL;
        if (end)
        {
            char name[256];
            snprintf(name, sizeof name, "%.*s", (int)(end - p - 1), p + 1);
            const char* v = getenv(name);
            if (v)
            {
                k += (size_t)snprintf(out + k, sizeof out - k, "%s", v);
                p = end + 1;
                continue;
            }
        }
        out[k++] = *p++;
    }
    out[k] = 0;
    if (ARG(1) && ARG(2) > k)
        memcpy(GUEST_PTR(ARG(1)), out, k + 1);
    RET((uint32_t)k + 1, 3);
}

static void sh_LockSetForegroundWindow(Guest* g) { RET(1, 1); }
/* no input method editor here */
static void sh_ImmGetDefaultIMEWnd(Guest* g) { RET(0, 1); }
static void sh_ImmIsIME(Guest* g) { RET(0, 1); }
static void sh_ImmGetIMEFileNameA(Guest* g) { RET(0, 3); }

static void sh_GetUILanguage(Guest* g) { RET(0x0409u, 0); }
static void sh_HeapSetInformation(Guest* g) { RET(1, 4); }
static void sh_SetDllDirectoryW(Guest* g) { RET(1, 1); }
/* GetModuleHandleExW(flags, name, HMODULE*): the image itself, whatever is asked */
static void sh_GetModuleHandleExW(Guest* g) { if (ARG(2)) wr32(ARG(2), rt_image_base); RET(1, 3); }

static void sh_ConvertDefaultLocale(Guest* g) { RET(ARG(0) ? ARG(0) : 0x0409u, 1); }
static void sh_GetSystemDefaultLangID(Guest* g) { RET(0x0409u, 0); }
static void sh_SetErrorMode(Guest* g) { RET(0, 1); }
static void sh_FreeResource(Guest* g) { RET(0, 1); }
static void sh_GlobalFlags(Guest* g) { RET(0, 1); }
static void sh_GlobalHandle(Guest* g) { RET(ARG(0), 1); } /* GlobalAlloc's handles are the memory here */
static void sh_LockFile(Guest* g) { RET(1, 5); }
static void sh_UnlockFile(Guest* g) { RET(1, 5); }
static void sh_WritePrivateProfileStringW(Guest* g) { RET(1, 4); }

/* GlobalReAlloc(h, size, flags) / LocalReAlloc: GMEM_ZEROINIT 0x40 */
static void sh_GlobalReAlloc(Guest* g) { RET(gheap_realloc(ARG(0), ARG(1) ? ARG(1) : 1, (ARG(2) & 0x40u) != 0, 0), 3); }

/* EnumResourceLanguagesW(module, type, name, proc, param): one language, US English */
static void sh_EnumResourceLanguagesW(Guest* g)
{
    uint32_t a[5] = { ARG(0), ARG(1), ARG(2), 0x0409u, ARG(4) };
    guest_call(ARG(3), 5, a);
    RET(1, 5);
}

/* GetVersionExW(OSVERSIONINFOW*): Windows XP SP3, as GetVersionExA says */
static void sh_GetVersionExW(Guest* g)
{
    uint32_t p = ARG(0), size = rd32(p);
    memset(GUEST_PTR(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 5);
    wr32(p + 8, 1);
    wr32(p + 12, 2600);
    wr32(p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    static const char sp[] = "Service Pack 3";
    for (uint32_t i = 0; i < sizeof sp; ++i)
        wr16(p + 20 + 2 * i, (uint8_t)sp[i]);
    if (size >= 284) /* OSVERSIONINFOEXW: service pack 3.0, a workstation */
    {
        wr16(p + 276, 3);
        wr8(p + 282, 1);
    }
    RET(1, 1);
}

/* GetComputerNameExW(format, buffer, size*) */
static void sh_GetComputerNameExW(Guest* g)
{
    static const char name[] = "EORZEA";
    uint32_t buf = ARG(1), sizep = ARG(2), cap = rd32(sizep);
    if (cap < sizeof name)
    {
        wr32(sizep, sizeof name);
        gt_set_error(234); /* ERROR_MORE_DATA */
        RET(0, 3);
    }
    for (uint32_t i = 0; i < sizeof name; ++i)
        wr16(buf + 2 * i, (uint8_t)name[i]);
    wr32(sizep, sizeof name - 1);
    RET(1, 3);
}

static void sh_GetFileTime(Guest* g)
{
    for (int i = 1; i <= 3; ++i)
        if (ARG(i))
            wr32(ARG(i), 0), wr32(ARG(i) + 4, 0);
    RET(1, 4);
}

/* atoms: GlobalAddAtomW / GlobalFindAtomW / GlobalDeleteAtom over a small table (0xC000 up) */
static char g_atoms[64][64];

static uint32_t atom_find(uint32_t w)
{
    if (w < 0x10000u)
        return w;
    char s[64];
    uint32_t n = 0;
    for (; n < 63 && rd16(w + 2 * n); ++n)
        s[n] = (char)rd16(w + 2 * n);
    s[n] = 0;
    for (int i = 0; i < 64; ++i)
        if (g_atoms[i][0] && !strcasecmp(g_atoms[i], s))
            return 0xC000u + (uint32_t)i;
    return 0;
}

static void sh_GlobalFindAtomW(Guest* g) { RET(atom_find(ARG(0)), 1); }

static void sh_GlobalAddAtomW(Guest* g)
{
    uint32_t a = atom_find(ARG(0));
    if (a)
        RET(a, 1);
    for (int i = 0; i < 64; ++i)
        if (!g_atoms[i][0])
        {
            uint32_t n = 0;
            for (; n < 63 && rd16(ARG(0) + 2 * n); ++n)
                g_atoms[i][n] = (char)rd16(ARG(0) + 2 * n);
            g_atoms[i][n] = 0;
            RET(0xC000u + (uint32_t)i, 1);
        }
    RET(0, 1);
}

static void sh_GlobalDeleteAtom(Guest* g)
{
    if (ARG(0) >= 0xC000u && ARG(0) < 0xC040u)
        g_atoms[ARG(0) - 0xC000u][0] = 0;
    RET(0, 1);
}

/* CreateProcessW(app, cmdline, ...10 arguments): ffxivlogin starts the game this way. The command
 * line is "<exe> <args>", unquoted; k32_launch_hook decides. */
static void sh_CreateProcessW(Guest* g)
{
    char* app = ARG(0) ? host_narrow(ARG(0)) : NULL;
    char* cmd = ARG(1) ? host_narrow(ARG(1)) : NULL;
    const char* file = app ? app : cmd ? cmd : "";
    const char* params = "";
    char* split = NULL;
    if (!app && cmd)
    {
        /* the program is the longest prefix that names an .exe (CreateProcess's own search) */
        char* e = strcasestr(cmd, ".exe");
        if (e)
        {
            split = strndup(cmd, (size_t)(e + 4 - cmd));
            file = split;
            params = e[4] == ' ' ? e + 5 : e + 4;
        }
    }
    int ok = k32_launch_hook ? k32_launch_hook(file, params, "") : 0;
    rt_log("[recomp] CreateProcessW(%s, %.60s...): %s\n", file, params, ok ? "started" : "not started");
    uint32_t pi = ARG(9);
    if (ok && pi)
    {
        wr32(pi, 0x00000F01u); /* hProcess, hThread: handles nothing waits on */
        wr32(pi + 4, 0x00000F02u);
        wr32(pi + 8, 1000);
        wr32(pi + 12, 1001);
    }
    free(app);
    free(cmd);
    free(split);
    gt_set_error(ok ? 0 : 2);
    RET(ok, 10);
}

/* --- advapi32: the old registry calls MFC keeps -------------------------------------------------- */
static void sh_RegOpenKeyW(Guest* g) { forward_strings(g, "advapi32.dll", "RegOpenKeyA", 2); }
static void sh_RegQueryValueW(Guest* g) { RET(2, 4); } /* ERROR_FILE_NOT_FOUND */
static void sh_RegEnumKeyW(Guest* g) { RET(259, 4); } /* ERROR_NO_MORE_ITEMS */

/* --- shlwapi, comdlg32, winspool, urlmon, oledlg ------------------------------------------------- */
static uint32_t last_sep(uint32_t w)
{
    uint32_t n = wlen(w), r = w;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        if (c == '\\' || c == '/' || c == ':')
            r = w + 2 * (i + 1);
    }
    return r;
}

static void sh_PathFindFileNameW(Guest* g) { RET(last_sep(ARG(0)), 1); }

static void sh_PathFindExtensionW(Guest* g)
{
    uint32_t f = last_sep(ARG(0)), n = wlen(f), dot = f + 2 * n;
    for (uint32_t i = 0; i < n; ++i)
        if (rd16(f + 2 * i) == '.')
            dot = f + 2 * i;
    RET(dot, 1);
}

static void sh_PathIsUNCW(Guest* g) { RET(rd16(ARG(0)) == '\\' && rd16(ARG(0) + 2) == '\\', 1); }

/* PathStripToRootW: "C:\..." keeps "C:\" */
static void sh_PathStripToRootW(Guest* g)
{
    uint32_t p = ARG(0);
    if (rd16(p) && rd16(p + 2) == ':')
    {
        wr16(p + 4 + (rd16(p + 4) == '\\' ? 2 : 0), 0);
        RET(1, 1);
    }
    RET(0, 1);
}

/* GetFileTitleW(path, buf, n): the file name */
static void sh_GetFileTitleW(Guest* g)
{
    uint32_t f = last_sep(ARG(0)), n = wlen(f);
    if (!ARG(1) || ARG(2) <= n)
        RET(n + 1, 3);
    for (uint32_t i = 0; i <= n; ++i)
        wr16(ARG(1) + 2 * i, rd16(f + 2 * i));
    RET(0, 3);
}

static void sh_OpenPrinterW(Guest* g) { RET(0, 3); }
static void sh_ClosePrinter(Guest* g) { RET(1, 1); }
static void sh_DocumentPropertiesW(Guest* g) { RET((uint32_t)-1, 6); }
static void sh_OleUIBusyW(Guest* g) { RET(0, 1); }

/* UrlMkSetSessionOption(URLMON_OPTION_USERAGENT 0x10000001, buffer, length, 0): ffxivlogin's
 * SQEXAuthor User-Agent, kept for the browser (webbrowser.m) */
char k32_user_agent[256];

static void sh_UrlMkSetSessionOption(Guest* g)
{
    if (ARG(0) == 0x10000001u && ARG(1))
    {
        uint32_t n = ARG(2) < sizeof k32_user_agent - 1 ? ARG(2) : sizeof k32_user_agent - 1;
        memcpy(k32_user_agent, GUEST_PTR(ARG(1)), n);
        k32_user_agent[n] = 0;
        rt_log("[recomp] User-Agent: %s\n", k32_user_agent);
    }
    RET(0, 4);
}

/* --- ole32, oleaut32 ------------------------------------------------------------------------------- */
static void sh_OleInitialize(Guest* g) { RET(0, 1); }
static void sh_OleUninitialize(Guest* g) { RET(0, 0); }
static void sh_CoRegisterMessageFilter(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(0, 2); }
static void sh_CoRevokeClassObject(Guest* g) { RET(0, 1); }
static void sh_OleFlushClipboard(Guest* g) { RET(0, 0); }
static void sh_OleIsCurrentClipboard(Guest* g) { RET(1, 1); } /* S_FALSE */
static void sh_CLSIDFromProgID(Guest* g) { RET(0x800401F3u, 2); } /* CO_E_CLASSSTRING */
/* compound storage: MFC's fallback when a control has no persisted state is InitNew */
static void sh_CreateILockBytesOnHGlobal(Guest* g) { if (ARG(2)) wr32(ARG(2), 0); RET(0x80004005u, 3); }
static void sh_StgCreateDocfileOnILockBytes(Guest* g) { if (ARG(3)) wr32(ARG(3), 0); RET(0x80004005u, 4); }
static void sh_StgOpenStorageOnILockBytes(Guest* g) { if (ARG(5)) wr32(ARG(5), 0); RET(0x80004005u, 6); }

/* VARIANTs are 16 bytes: vt at 0, the value at 8. BSTRs (vt 8) are copied, interfaces (9, 13)
 * AddRef'd. */
static void sh_VariantInit(Guest* g) { memset(GUEST_PTR(ARG(0)), 0, 16); RET(0, 1); }

static void sh_VariantClear(Guest* g)
{
    uint32_t v = ARG(0);
    uint16_t vt = rd16(v);
    if (vt == 8 && rd32(v + 8))
        gheap_free(rd32(v + 8) - 4);
    else if ((vt == 9 || vt == 13) && rd32(v + 8))
    {
        uint32_t o = rd32(v + 8), a[1] = { o };
        guest_call(rd32(rd32(o) + 8), 1, a); /* Release */
    }
    memset(GUEST_PTR(v), 0, 16);
    RET(0, 1);
}

static void sh_VariantCopy(Guest* g)
{
    uint32_t d = ARG(0), s = ARG(1);
    uint16_t vt = rd16(s);
    memcpy(GUEST_PTR(d), GUEST_PTR(s), 16);
    if (vt == 8 && rd32(s + 8))
    {
        uint32_t src = rd32(s + 8), n = rd32(src - 4), p = gheap_alloc(4 + n + 2, 0);
        wr32(p, n);
        memcpy(GUEST_PTR(p + 4), GUEST_PTR(src), n + 2);
        wr32(d + 8, p + 4);
    }
    else if ((vt == 9 || vt == 13) && rd32(s + 8))
    {
        uint32_t o = rd32(s + 8), a[1] = { o };
        guest_call(rd32(rd32(o) + 4), 1, a); /* AddRef */
    }
    RET(0, 2);
}

/* VariantChangeType(dst, src, flags, vt): VT_BYREF sources are read through; strings, integers
 * and booleans convert among themselves; an interface converts only to itself */
static uint32_t bstr_from(const char* s)
{
    uint32_t n = (uint32_t)strlen(s), p = gheap_alloc(4 + 2 * n + 2, 0);
    wr32(p, 2 * n);
    for (uint32_t i = 0; i <= n; ++i)
        wr16(p + 4 + 2 * i, (uint8_t)s[i]);
    return p + 4;
}

static void sh_VariantChangeType(Guest* g)
{
    uint32_t dst = ARG(0), src = ARG(1), want = ARG(3) & 0xFFFF;
    uint16_t vt = rd16(src);
    uint32_t val = rd32(src + 8);
    while (vt & 0x4000) /* VT_BYREF */
    {
        if ((vt & 0xFFF) == 12) /* VT_VARIANT: the variant it points at */
        {
            src = val;
            vt = rd16(src);
            val = rd32(src + 8);
            continue;
        }
        vt &= 0xFFF;
        val = vt == 2 || vt == 11 ? rd16(val) : rd32(val);
        break;
    }
    int32_t num = 0;
    int is_num = 1;
    switch (vt)
    {
    case 2: case 11: num = (int16_t)val; break;  /* VT_I2, VT_BOOL */
    case 3: case 19: case 22: case 23: num = (int32_t)val; break;
    case 0: case 1: num = 0; break;              /* VT_EMPTY, VT_NULL */
    case 8:
    {
        is_num = 0;
        char tmp[32];
        uint32_t i = 0;
        for (; i < 31 && val && rd16(val + 2 * i); ++i)
            tmp[i] = (char)rd16(val + 2 * i);
        tmp[i] = 0;
        num = (int32_t)strtol(tmp, NULL, 10);
        break;
    }
    default: is_num = -1;
    }
    uint32_t out = 0;
    if (want == vt && vt != 8)
        out = val;
    else if (want == 8 && vt == 8)
    {
        uint32_t n = val ? rd32(val - 4) : 0, p = gheap_alloc(4 + n + 2, 0);
        wr32(p, n);
        if (val)
            memcpy(GUEST_PTR(p + 4), GUEST_PTR(val), n);
        wr16(p + 4 + n, 0);
        out = p + 4;
    }
    else if (want == 8 && is_num == 1)
    {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", vt == 11 ? (num ? -1 : 0) : num);
        out = bstr_from(tmp);
    }
    else if ((want == 3 || want == 19 || want == 22 || want == 23 || want == 2) && is_num >= 0)
        out = (uint32_t)num;
    else if (want == 11 && is_num >= 0)
        out = num ? 0xFFFFu : 0;
    else
        RET(0x80020005u, 4); /* DISP_E_TYPEMISMATCH */
    if (dst == ARG(1))
        ; /* in place: the old value is not freed (the caller still owns it) */
    memset(GUEST_PTR(dst), 0, 16);
    wr16(dst, (uint16_t)want);
    if (want == 2 || want == 11)
        wr16(dst + 8, (uint16_t)out);
    else
        wr32(dst + 8, out);
    RET(0, 4);
}

static void sh_oleaut_fail2(Guest* g) { RET(0x80004001u, 2); }
static void sh_SafeArrayDestroy(Guest* g) { RET(0, 1); }
static void sh_OleCreateFontIndirect(Guest* g) { if (ARG(2)) wr32(ARG(2), 0); RET(0x80004001u, 3); }

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
    { "kernel32.dll", "CreateActCtxW", sh_CreateActCtxW },
    { "kernel32.dll", "ActivateActCtx", sh_ActivateActCtx },
    { "kernel32.dll", "DeactivateActCtx", sh_DeactivateActCtx },
    { "kernel32.dll", "ReleaseActCtx", sh_ReleaseActCtx },
    { "kernel32.dll", "IsDBCSLeadByteEx", sh_IsDBCSLeadByteEx },
    { "kernel32.dll", "ExpandEnvironmentStringsA", sh_ExpandEnvironmentStringsA },
    { "user32.dll", "LockSetForegroundWindow", sh_LockSetForegroundWindow },
    { "imm32.dll", "ImmGetDefaultIMEWnd", sh_ImmGetDefaultIMEWnd },
    { "imm32.dll", "ImmIsIME", sh_ImmIsIME },
    { "imm32.dll", "ImmGetIMEFileNameA", sh_ImmGetIMEFileNameA },
    { "kernel32.dll", "GetUserDefaultUILanguage", sh_GetUILanguage },
    { "kernel32.dll", "GetSystemDefaultUILanguage", sh_GetUILanguage },
    { "kernel32.dll", "HeapSetInformation", sh_HeapSetInformation },
    { "kernel32.dll", "SetDllDirectoryW", sh_SetDllDirectoryW },
    { "kernel32.dll", "GetModuleHandleExW", sh_GetModuleHandleExW },
    { "kernel32.dll", "ConvertDefaultLocale", sh_ConvertDefaultLocale },
    { "kernel32.dll", "GetSystemDefaultLangID", sh_GetSystemDefaultLangID },
    { "kernel32.dll", "SetErrorMode", sh_SetErrorMode },
    { "kernel32.dll", "FreeResource", sh_FreeResource },
    { "kernel32.dll", "GlobalFlags", sh_GlobalFlags },
    { "kernel32.dll", "GlobalHandle", sh_GlobalHandle },
    { "kernel32.dll", "GlobalReAlloc", sh_GlobalReAlloc },
    { "kernel32.dll", "LocalReAlloc", sh_GlobalReAlloc },
    { "kernel32.dll", "LockFile", sh_LockFile },
    { "kernel32.dll", "UnlockFile", sh_UnlockFile },
    { "kernel32.dll", "WritePrivateProfileStringW", sh_WritePrivateProfileStringW },
    { "kernel32.dll", "EnumResourceLanguagesW", sh_EnumResourceLanguagesW },
    { "kernel32.dll", "GetVersionExW", sh_GetVersionExW },
    { "kernel32.dll", "GetComputerNameExW", sh_GetComputerNameExW },
    { "kernel32.dll", "GetFileTime", sh_GetFileTime },
    { "kernel32.dll", "GlobalAddAtomW", sh_GlobalAddAtomW },
    { "kernel32.dll", "GlobalFindAtomW", sh_GlobalFindAtomW },
    { "kernel32.dll", "GlobalDeleteAtom", sh_GlobalDeleteAtom },
    { "kernel32.dll", "CreateProcessW", sh_CreateProcessW },
    { "advapi32.dll", "RegOpenKeyW", sh_RegOpenKeyW },
    { "advapi32.dll", "RegQueryValueW", sh_RegQueryValueW },
    { "advapi32.dll", "RegEnumKeyW", sh_RegEnumKeyW },
    { "shlwapi.dll", "PathFindFileNameW", sh_PathFindFileNameW },
    { "shlwapi.dll", "PathFindExtensionW", sh_PathFindExtensionW },
    { "shlwapi.dll", "PathIsUNCW", sh_PathIsUNCW },
    { "shlwapi.dll", "PathStripToRootW", sh_PathStripToRootW },
    { "comdlg32.dll", "GetFileTitleW", sh_GetFileTitleW },
    { "winspool.drv", "OpenPrinterW", sh_OpenPrinterW },
    { "winspool.drv", "ClosePrinter", sh_ClosePrinter },
    { "winspool.drv", "DocumentPropertiesW", sh_DocumentPropertiesW },
    { "oledlg.dll", "OleUIBusyW", sh_OleUIBusyW },
    { "urlmon.dll", "UrlMkSetSessionOption", sh_UrlMkSetSessionOption },
    { "ole32.dll", "OleInitialize", sh_OleInitialize },
    { "ole32.dll", "OleUninitialize", sh_OleUninitialize },
    { "ole32.dll", "CoRegisterMessageFilter", sh_CoRegisterMessageFilter },
    { "ole32.dll", "CoRevokeClassObject", sh_CoRevokeClassObject },
    { "ole32.dll", "OleFlushClipboard", sh_OleFlushClipboard },
    { "ole32.dll", "OleIsCurrentClipboard", sh_OleIsCurrentClipboard },
    { "ole32.dll", "CLSIDFromProgID", sh_CLSIDFromProgID },
    { "ole32.dll", "CreateILockBytesOnHGlobal", sh_CreateILockBytesOnHGlobal },
    { "ole32.dll", "StgCreateDocfileOnILockBytes", sh_StgCreateDocfileOnILockBytes },
    { "ole32.dll", "StgOpenStorageOnILockBytes", sh_StgOpenStorageOnILockBytes },
    { "oleaut32.dll", "#16", sh_SafeArrayDestroy },
    { "oleaut32.dll", "#184", sh_oleaut_fail2 },
    { "oleaut32.dll", "#185", sh_oleaut_fail2 },
    { "oleaut32.dll", "#420", sh_OleCreateFontIndirect },
    { NULL, NULL, NULL },
};

/* the VARIANT calls win over xiv_misc.c's (which only the game's few uses need) */
static const ShimDef k_overrides[] = {
    { "oleaut32.dll", "#8", sh_VariantInit },
    { "oleaut32.dll", "#9", sh_VariantClear },
    { "oleaut32.dll", "#10", sh_VariantCopy },
    { "oleaut32.dll", "#12", sh_VariantChangeType },
    { "oleaut32.dll", "VariantInit", sh_VariantInit },
    { "oleaut32.dll", "VariantClear", sh_VariantClear },
    { "oleaut32.dll", "VariantCopy", sh_VariantCopy },
    { "oleaut32.dll", "VariantChangeType", sh_VariantChangeType },
    { NULL, NULL, NULL },
};

void k32_launch_init(void)
{
    thunk_register(k_shims);
    thunk_override(k_overrides);
}
