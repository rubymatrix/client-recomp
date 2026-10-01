/* kernel32 calls FINAL FANTASY XIV 1.23b (ffxivgame.exe) makes that FFXI never did.
 *
 * Most are the wide (W) forms of calls the A shims already implement: the UTF-16 strings are
 * converted to the code page every guest path uses (as k32_io.c's CreateFileW does), the A shim
 * runs (thunk_shim), and string results are widened back into the caller's buffer. The rest are
 * process and console queries a game client asks once and acts on little.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"

#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_INVALID_HANDLE 6u

/* --- strings ----------------------------------------------------------------------------------- */

static uint32_t wlen(uint32_t w)
{
    uint32_t n = 0;
    while (w && rd16(w + 2 * n))
        n++;
    return n;
}

/* A guest copy of a UTF-16 string, narrowed (characters past 0xFF become '?'); 0 for NULL. */
static uint32_t narrow(uint32_t w)
{
    if (!w)
        return 0;
    uint32_t n = wlen(w), a = gheap_alloc(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(a + n, 0);
    return a;
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

static void forward(Guest* g, const char* a_name)
{
    Shim s = thunk_shim("kernel32.dll", a_name);
    if (!s)
        rt_fatal(g, 0, a_name);
    s(g);
}

/* Runs the A shim with the string arguments named by `mask` (bit n: argument n) narrowed. */
static void forward_strings(Guest* g, const char* a_name, unsigned mask)
{
    uint32_t tmp[8] = { 0 };
    uint32_t args = g->esp + 4;
    for (unsigned n = 0; n < 8; ++n)
        if (mask & (1u << n))
        {
            tmp[n] = narrow(rd32(args + 4 * n));
            wr32(args + 4 * n, tmp[n]);
        }
    forward(g, a_name);
    for (unsigned n = 0; n < 8; ++n)
        if (tmp[n])
            gheap_free(tmp[n]);
}

#define W_IN(name, mask) static void sh_##name##W(Guest* g) { forward_strings(g, #name "A", mask); }
W_IN(DeleteFile, 1)
W_IN(CreateDirectory, 1)
W_IN(RemoveDirectory, 1)
W_IN(SetCurrentDirectory, 1)
W_IN(GetFileAttributes, 1)
W_IN(SetFileAttributes, 1)
W_IN(LoadLibrary, 1)
W_IN(CreateMutex, 4)
W_IN(GetDriveType, 1)
W_IN(SetEnvironmentVariable, 3)

/* MoveFileExW(from, to, flags): MOVEFILE_REPLACE_EXISTING is what a rename on POSIX does anyway */
static void sh_MoveFileExW(Guest* g)
{
    uint32_t flags = ARG(2);
    (void)flags;
    forward_strings(g, "MoveFileA", 3); /* pops two arguments */
    g->esp += 4;
}

/* The A shim writes ANSI into a temporary the size of the caller's buffer; the result widens. */
static void sh_GetCurrentDirectoryW(Guest* g)
{
    uint32_t n = ARG(0), wbuf = ARG(1), tmp = gheap_alloc(n ? n : 1, 1);
    wr32(g->esp + 8, tmp);
    forward(g, "GetCurrentDirectoryA");
    if (g->eax && g->eax < n)
        widen(tmp, wbuf, n);
    gheap_free(tmp);
}

static void sh_GetModuleFileNameW(Guest* g)
{
    uint32_t wbuf = ARG(1), n = ARG(2), tmp = gheap_alloc(n ? n : 1, 1);
    wr32(g->esp + 8, tmp);
    forward(g, "GetModuleFileNameA");
    widen(tmp, wbuf, n);
    gheap_free(tmp);
}

/* GetFullPathNameW(name, n, buf, filepart) */
static void sh_GetFullPathNameW(Guest* g)
{
    uint32_t name = narrow(ARG(0)), n = ARG(1), wbuf = ARG(2), part = ARG(3);
    uint32_t tmp = gheap_alloc(n ? n : 1, 1), apart = part ? gheap_alloc(4, 1) : 0;
    wr32(g->esp + 4, name);
    wr32(g->esp + 12, tmp);
    wr32(g->esp + 16, apart);
    forward(g, "GetFullPathNameA");
    if (g->eax && g->eax < n)
    {
        widen(tmp, wbuf, n);
        if (part)
            wr32(part, rd32(apart) ? wbuf + 2 * (rd32(apart) - tmp) : 0);
    }
    gheap_free(tmp);
    gheap_free(name);
    if (apart)
        gheap_free(apart);
}

/* WIN32_FIND_DATAA (320 bytes) -> WIN32_FIND_DATAW (592): the 44-byte head, then the names. */
static void find_data_widen(uint32_t a, uint32_t w)
{
    memcpy(GUEST_PTR(w), GUEST_PTR(a), 44);
    widen(a + 44, w + 44, 260);
    widen(a + 44 + 260, w + 44 + 520, 14);
}

static void sh_FindFirstFileW(Guest* g)
{
    uint32_t wdata = ARG(1), tmp = gheap_alloc(320, 1);
    wr32(g->esp + 8, tmp);
    forward_strings(g, "FindFirstFileA", 1);
    if (g->eax != 0xFFFFFFFFu)
        find_data_widen(tmp, wdata);
    gheap_free(tmp);
}

static void sh_FindNextFileW(Guest* g)
{
    uint32_t wdata = ARG(1), tmp = gheap_alloc(320, 1);
    wr32(g->esp + 8, tmp);
    forward(g, "FindNextFileA");
    if (g->eax)
        find_data_widen(tmp, wdata);
    gheap_free(tmp);
}

static void sh_lstrcpyW(Guest* g)
{
    uint32_t d = ARG(0), s = ARG(1), n = wlen(s);
    memmove(GUEST_PTR(d), GUEST_PTR(s), 2 * (n + 1));
    RET(d, 2);
}

static void sh_lstrcatW(Guest* g)
{
    uint32_t d = ARG(0), s = ARG(1), dn = wlen(d), n = wlen(s);
    memmove(GUEST_PTR(d + 2 * dn), GUEST_PTR(s), 2 * (n + 1));
    RET(d, 2);
}

static void sh_lstrcmpW(Guest* g)
{
    uint32_t a = ARG(0), b = ARG(1);
    for (uint32_t i = 0;; ++i)
    {
        uint16_t x = a ? rd16(a + 2 * i) : 0, y = b ? rd16(b + 2 * i) : 0;
        if (x != y)
            RET(x < y ? (uint32_t)-1 : 1u, 2);
        if (!x)
            RET(0, 2);
    }
}

/* --- files ------------------------------------------------------------------------------------- */

/* SetFilePointerEx(h, LARGE_INTEGER distance (two dwords), PLARGE_INTEGER newpos, method) */
static void sh_SetFilePointerEx(Guest* g)
{
    uint32_t h = ARG(0), lo = ARG(1), hi = ARG(2), out = ARG(3), method = ARG(4);
    uint32_t hip = gheap_alloc(4, 0);
    wr32(hip, hi);
    /* SetFilePointer(h, lo, &hi, method) in the same frame: four of our five argument slots */
    uint32_t args = g->esp + 4;
    wr32(args + 0, h);
    wr32(args + 4, lo);
    wr32(args + 8, hip);
    wr32(args + 12, method);
    forward(g, "SetFilePointer"); /* pops four */
    uint32_t r = g->eax, rhi = rd32(hip);
    gheap_free(hip);
    int ok = !(r == 0xFFFFFFFFu && gt_get_error());
    if (ok && out)
    {
        wr32(out, r);
        wr32(out + 4, rhi);
    }
    g->eax = ok;
    g->esp += 4;
}

/* GetFileSizeEx(h, PLARGE_INTEGER) */
static void sh_GetFileSizeEx(Guest* g)
{
    uint32_t out = ARG(1), hip = gheap_alloc(4, 1);
    wr32(g->esp + 8, hip);
    forward(g, "GetFileSize");
    uint32_t lo = g->eax, hi = rd32(hip);
    gheap_free(hip);
    int ok = !(lo == 0xFFFFFFFFu && gt_get_error());
    if (ok)
    {
        wr32(out, lo);
        wr32(out + 4, hi);
    }
    g->eax = ok;
}

static void sh_SetFileTime(Guest* g) { RET(1, 4); }

/* GetDiskFreeSpaceW(root, sectors/cluster, bytes/sector, free clusters, total clusters) */
static void sh_GetDiskFreeSpaceW(Guest* g)
{
    if (ARG(1)) wr32(ARG(1), 8);
    if (ARG(2)) wr32(ARG(2), 512);
    if (ARG(3)) wr32(ARG(3), 0x01000000u); /* 64 GB free: the game checks before writing */
    if (ARG(4)) wr32(ARG(4), 0x02000000u);
    RET(1, 5);
}

/* GetDiskFreeSpaceExW(root, avail, total, free): ULARGE_INTEGERs */
static void sh_GetDiskFreeSpaceExW(Guest* g)
{
    static const uint64_t avail = 64ull << 30, total = 128ull << 30;
    if (ARG(1)) memcpy(GUEST_PTR(ARG(1)), &avail, 8);
    if (ARG(2)) memcpy(GUEST_PTR(ARG(2)), &total, 8);
    if (ARG(3)) memcpy(GUEST_PTR(ARG(3)), &avail, 8);
    RET(1, 4);
}

/* GetVolumeInformationW(root, name, nameSize, serial, maxComponent, flags, fsName, fsNameSize) */
static void sh_GetVolumeInformationW(Guest* g)
{
    static const char fs[] = "NTFS";
    if (ARG(1) && ARG(2)) wr16(ARG(1), 0);
    if (ARG(3)) wr32(ARG(3), 0x1C0FFEE0u);
    if (ARG(4)) wr32(ARG(4), 255);
    if (ARG(5)) wr32(ARG(5), 0x00000007u); /* case preserved, case sensitive search, unicode */
    if (ARG(6) && ARG(7))
    {
        uint32_t i = 0;
        for (; fs[i] && i + 1 < ARG(7); ++i)
            wr16(ARG(6) + 2 * i, (uint8_t)fs[i]);
        wr16(ARG(6) + 2 * i, 0);
    }
    RET(1, 8);
}

static void sh_GetSystemDirectoryA(Guest* g)
{
    static const char dir[] = "C:\\WINDOWS\\system32";
    uint32_t buf = ARG(0), n = ARG(1), len = sizeof dir - 1;
    if (n <= len)
        RET(len + 1, 2);
    memcpy(GUEST_PTR(buf), dir, len + 1);
    RET(len, 2);
}

static void sh_CreatePipe(Guest* g) { gt_set_error(ERROR_CALL_NOT_IMPLEMENTED); RET(0, 4); }

/* --- process, threads, CPUs -------------------------------------------------------------------- */

/* The CRT and the game XOR pointers with a per-process cookie; identity is a valid cookie. */
static void sh_EncodePointer(Guest* g) { RET(ARG(0), 1); }
static void sh_DecodePointer(Guest* g) { RET(ARG(0), 1); }

/* Fiber-local storage (MSVC 2005's CRT uses it when GetProcAddress finds it): one fiber per
 * thread as far as these slots go, so FLS is TLS. The destructor callback is not run. */
static void sh_FlsAlloc(Guest* g) { forward(g, "TlsAlloc"); g->esp += 4; }
static void sh_FlsGetValue(Guest* g) { forward(g, "TlsGetValue"); }
static void sh_FlsSetValue(Guest* g) { forward(g, "TlsSetValue"); }
static void sh_FlsFree(Guest* g) { forward(g, "TlsFree"); }

static uint32_t cpu_mask(void)
{
    unsigned n = 4; /* GetSystemInfo (k32_misc.c) reports the same count */
    return (1u << n) - 1u;
}
static void sh_GetProcessAffinityMask(Guest* g)
{
    if (ARG(1)) wr32(ARG(1), cpu_mask());
    if (ARG(2)) wr32(ARG(2), cpu_mask());
    RET(1, 3);
}
static void sh_SetProcessAffinityMask(Guest* g) { RET(1, 2); }
static void sh_SetThreadAffinityMask(Guest* g) { RET(cpu_mask(), 2); } /* the previous mask */
static void sh_SetThreadIdealProcessor(Guest* g) { RET(0, 2); }
static void sh_OpenThread(Guest* g) { RET(0xFFFFFFFEu, 3); } /* the current-thread pseudo handle */
static void sh_SwitchToThread(Guest* g) { gt_unlock(); plat_yield(); gt_lock(); RET(1, 0); }
static void sh_SleepEx(Guest* g)
{
    uint32_t ms = ARG(0);
    gt_unlock();
    if (ms)
        plat_sleep_ms(ms);
    else
        plat_yield();
    gt_lock();
    RET(0, 2);
}
static void sh_IsDebuggerPresent(Guest* g) { RET(0, 0); }
static void sh_GetExitCodeProcess(Guest* g) { if (ARG(1)) wr32(ARG(1), 259 /* STILL_ACTIVE */); RET(1, 2); }
static void sh_GlobalFree(Guest* g) { if (ARG(0)) gheap_free(ARG(0)); RET(0, 1); }
static void sh_GetUserDefaultLangID(Guest* g) { RET(0x0409, 0); }
static void sh_GetSystemDefaultLCID(Guest* g) { RET(0x0409, 0); }

/* VirtualQuery(address, MEMORY_BASIC_INFORMATION*, size): one page's state. */
static void sh_VirtualQuery(Guest* g)
{
    uint32_t a = ARG(0) & ~0xFFFu, out = ARG(1), size = ARG(2);
    if (size < 28)
        RET(0, 3);
    int committed = gwin_is_committed(a);
    wr32(out + 0, a);                              /* BaseAddress */
    wr32(out + 4, a);                              /* AllocationBase */
    wr32(out + 8, committed ? 0x04u : 0x01u);      /* AllocationProtect: READWRITE / NOACCESS */
    wr32(out + 12, 0x1000);                        /* RegionSize */
    wr32(out + 16, committed ? 0x1000u : 0x10000u); /* State: MEM_COMMIT / MEM_FREE */
    wr32(out + 20, committed ? 0x04u : 0x01u);     /* Protect */
    wr32(out + 24, committed ? 0x20000u : 0u);     /* Type: MEM_PRIVATE */
    RET(28, 3);
}

/* FormatMessageA(flags, source, id, lang, buffer, size, args): system messages as "Error N". */
static void sh_FormatMessageA(Guest* g)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), size = ARG(5);
    char text[64];
    int n = snprintf(text, sizeof text, "Error %u.\r\n", id);
    if (flags & 0x100) /* FORMAT_MESSAGE_ALLOCATE_BUFFER: buf is a pointer to the result */
    {
        uint32_t p = gheap_alloc((uint32_t)n + 1, 0);
        memcpy(GUEST_PTR(p), text, (size_t)n + 1);
        wr32(buf, p);
        RET((uint32_t)n, 7);
    }
    if (!buf || size <= (uint32_t)n)
        RET(0, 7);
    memcpy(GUEST_PTR(buf), text, (size_t)n + 1);
    RET((uint32_t)n, 7);
}

/* GetTimeFormatA/GetDateFormatA(lcid, flags, SYSTEMTIME*, format, buffer, size): a fixed form. */
static void time_format(Guest* g, int date)
{
    uint32_t st = ARG(2), buf = ARG(4), size = ARG(5);
    char text[32];
    PlatTime t;
    if (st)
    {
        t.year = rd16(st); t.month = rd16(st + 2); t.day = rd16(st + 6);
        t.hour = rd16(st + 8); t.minute = rd16(st + 10); t.second = rd16(st + 12);
    }
    else
        plat_split_time(plat_wall_ms(), 1, &t);
    int n = date ? snprintf(text, sizeof text, "%02d/%02d/%04d", t.month, t.day, t.year)
                 : snprintf(text, sizeof text, "%02d:%02d:%02d", t.hour, t.minute, t.second);
    if (!size)
        RET((uint32_t)n + 1, 6);
    if (size <= (uint32_t)n)
        RET(0, 6);
    memcpy(GUEST_PTR(buf), text, (size_t)n + 1);
    RET((uint32_t)n + 1, 6);
}
static void sh_GetTimeFormatA(Guest* g) { time_format(g, 0); }
static void sh_GetDateFormatA(Guest* g) { time_format(g, 1); }

/* --- console: the game has none; output goes to the log ----------------------------------------- */

static void sh_GetConsoleCP(Guest* g) { RET(1252, 0); }
static void sh_GetConsoleOutputCP(Guest* g) { RET(1252, 0); }
static void sh_GetConsoleMode(Guest* g) { gt_set_error(ERROR_INVALID_HANDLE); RET(0, 2); }
static void sh_SetConsoleMode(Guest* g) { RET(1, 2); }
static void sh_FlushConsoleInputBuffer(Guest* g) { RET(1, 1); }
static void sh_ReadConsoleInputA(Guest* g) { if (ARG(3)) wr32(ARG(3), 0); RET(0, 4); }
static void sh_WriteConsoleA(Guest* g)
{
    rt_log("[console] %.*s", (int)ARG(2), (const char*)GUEST_PTR(ARG(1)));
    if (ARG(3)) wr32(ARG(3), ARG(2));
    RET(1, 5);
}
static void sh_WriteConsoleW(Guest* g)
{
    uint32_t a = gheap_alloc(ARG(2) + 1, 1);
    for (uint32_t i = 0; i < ARG(2); ++i)
    {
        uint16_t c = rd16(ARG(1) + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    rt_log("[console] %s", (const char*)GUEST_PTR(a));
    gheap_free(a);
    if (ARG(3)) wr32(ARG(3), ARG(2));
    RET(1, 5);
}

static const ShimDef K32_XIV[] = {
    { "kernel32.dll", "DeleteFileW", sh_DeleteFileW },
    { "kernel32.dll", "CreateDirectoryW", sh_CreateDirectoryW },
    { "kernel32.dll", "RemoveDirectoryW", sh_RemoveDirectoryW },
    { "kernel32.dll", "SetCurrentDirectoryW", sh_SetCurrentDirectoryW },
    { "kernel32.dll", "GetFileAttributesW", sh_GetFileAttributesW },
    { "kernel32.dll", "SetFileAttributesW", sh_SetFileAttributesW },
    { "kernel32.dll", "LoadLibraryW", sh_LoadLibraryW },
    { "kernel32.dll", "CreateMutexW", sh_CreateMutexW },
    { "kernel32.dll", "GetDriveTypeW", sh_GetDriveTypeW },
    { "kernel32.dll", "SetEnvironmentVariableW", sh_SetEnvironmentVariableW },
    { "kernel32.dll", "MoveFileExW", sh_MoveFileExW },
    { "kernel32.dll", "GetCurrentDirectoryW", sh_GetCurrentDirectoryW },
    { "kernel32.dll", "GetModuleFileNameW", sh_GetModuleFileNameW },
    { "kernel32.dll", "GetFullPathNameW", sh_GetFullPathNameW },
    { "kernel32.dll", "FindFirstFileW", sh_FindFirstFileW },
    { "kernel32.dll", "FindNextFileW", sh_FindNextFileW },
    { "kernel32.dll", "lstrcpyW", sh_lstrcpyW },
    { "kernel32.dll", "lstrcatW", sh_lstrcatW },
    { "kernel32.dll", "lstrcmpW", sh_lstrcmpW },
    { "kernel32.dll", "SetFilePointerEx", sh_SetFilePointerEx },
    { "kernel32.dll", "GetFileSizeEx", sh_GetFileSizeEx },
    { "kernel32.dll", "SetFileTime", sh_SetFileTime },
    { "kernel32.dll", "GetDiskFreeSpaceW", sh_GetDiskFreeSpaceW },
    { "kernel32.dll", "GetDiskFreeSpaceExW", sh_GetDiskFreeSpaceExW },
    { "kernel32.dll", "GetVolumeInformationW", sh_GetVolumeInformationW },
    { "kernel32.dll", "GetSystemDirectoryA", sh_GetSystemDirectoryA },
    { "kernel32.dll", "CreatePipe", sh_CreatePipe },
    { "kernel32.dll", "EncodePointer", sh_EncodePointer },
    { "kernel32.dll", "DecodePointer", sh_DecodePointer },
    { "kernel32.dll", "EncodeSystemPointer", sh_EncodePointer },
    { "kernel32.dll", "DecodeSystemPointer", sh_DecodePointer },
    { "kernel32.dll", "FlsAlloc", sh_FlsAlloc },
    { "kernel32.dll", "FlsGetValue", sh_FlsGetValue },
    { "kernel32.dll", "FlsSetValue", sh_FlsSetValue },
    { "kernel32.dll", "FlsFree", sh_FlsFree },
    { "kernel32.dll", "GetProcessAffinityMask", sh_GetProcessAffinityMask },
    { "kernel32.dll", "SetProcessAffinityMask", sh_SetProcessAffinityMask },
    { "kernel32.dll", "SetThreadAffinityMask", sh_SetThreadAffinityMask },
    { "kernel32.dll", "SetThreadIdealProcessor", sh_SetThreadIdealProcessor },
    { "kernel32.dll", "OpenThread", sh_OpenThread },
    { "kernel32.dll", "SwitchToThread", sh_SwitchToThread },
    { "kernel32.dll", "SleepEx", sh_SleepEx },
    { "kernel32.dll", "IsDebuggerPresent", sh_IsDebuggerPresent },
    { "kernel32.dll", "GetExitCodeProcess", sh_GetExitCodeProcess },
    { "kernel32.dll", "GlobalFree", sh_GlobalFree },
    { "kernel32.dll", "GetUserDefaultLangID", sh_GetUserDefaultLangID },
    { "kernel32.dll", "GetSystemDefaultLCID", sh_GetSystemDefaultLCID },
    { "kernel32.dll", "VirtualQuery", sh_VirtualQuery },
    { "kernel32.dll", "FormatMessageA", sh_FormatMessageA },
    { "kernel32.dll", "GetTimeFormatA", sh_GetTimeFormatA },
    { "kernel32.dll", "GetDateFormatA", sh_GetDateFormatA },
    { "kernel32.dll", "GetConsoleCP", sh_GetConsoleCP },
    { "kernel32.dll", "GetConsoleOutputCP", sh_GetConsoleOutputCP },
    { "kernel32.dll", "GetConsoleMode", sh_GetConsoleMode },
    { "kernel32.dll", "SetConsoleMode", sh_SetConsoleMode },
    { "kernel32.dll", "FlushConsoleInputBuffer", sh_FlushConsoleInputBuffer },
    { "kernel32.dll", "ReadConsoleInputA", sh_ReadConsoleInputA },
    { "kernel32.dll", "WriteConsoleA", sh_WriteConsoleA },
    { "kernel32.dll", "WriteConsoleW", sh_WriteConsoleW },
    { NULL, NULL, NULL },
};

void k32_xiv_init(void) { thunk_register(K32_XIV); }
