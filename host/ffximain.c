/* The stand-in FFXiMain.dll: exports the four COM server entry points and runs the game as its
 * translation. On first use it maps the retail DLL (loader.c), runs the game's own CRT
 * DllMain(PROCESS_ATTACH) through the bridge, then forwards each export to the retail export's
 * address, whose patched entry leads to the translation.
 *
 * Where the retail DLL is: %FFXI_RECOMP_RETAIL% if set, else FFXiMain.retail.dll next to this DLL.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "bridge.h"
#include "loader.h"

static HMODULE g_self;
static HMODULE g_game;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static uintptr_t g_self_lo, g_self_hi;

/* --- diagnostics: under the retail launcher there is no console ------------------------------------------ */

static void open_log(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "FFXiMain.recomp.log");
    FILE* f = fopen(path, "w");
    if (!f)
    {
        GetTempPathA(MAX_PATH, path);
        strcat_s(path, MAX_PATH, "FFXiMain.recomp.log");
        f = fopen(path, "w");
    }
    rt_set_log(f);
    /* The boundary trace goes next to the log: FFXiMain.trace.txt, FFXiMain.seq.txt. */
    char base[MAX_PATH];
    strcpy_s(base, MAX_PATH, path);
    char* dot = strstr(base, ".recomp.log");
    if (dot)
    {
        *dot = 0;
        bridge_trace_open(base);
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    rt_log("[recomp] %04d-%02d-%02d %02d:%02d:%02d pid %lu, log %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
        t.wSecond, GetCurrentProcessId(), path);
}

static void fatal_box(uint32_t addr, const char* what)
{
    bridge_trace_dump();
    if (GetEnvironmentVariableA("FFXI_RECOMP_NO_DIALOG", NULL, 0))
        return; /* automated runs (tests/boot.exe) */
    char msg[512];
    sprintf_s(msg, sizeof msg, "The recompiled FFXiMain stopped at guest address %08X:\n\n%s\n\nDetails are in FFXiMain.recomp.log.",
        addr, what);
    MessageBoxA(NULL, msg, "FFXIRecompile", MB_OK | MB_ICONERROR);
}

/* Which translated function a host address in this DLL belongs to: the nearest function start
 * below it, from rt_table's host pointers. */
static uint32_t guest_function_at(uintptr_t host)
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

/* A fault inside translated code is always a bug (translated code has no SEH of its own). */
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS* e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    uintptr_t ip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_BREAKPOINT || ip < g_self_lo || ip >= g_self_hi || bridge_probing())
        return EXCEPTION_CONTINUE_SEARCH;
    Guest* g = bridge_guest();
    rt_log("\n[recomp] exception %08lx at host %p, in the translation of guest function %08x\n", code, (void*)ip,
        guest_function_at(ip));
    if (code == EXCEPTION_ACCESS_VIOLATION)
        rt_log("  %s %p\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (void*)e->ExceptionRecord->ExceptionInformation[1]);
    rt_log("  guest state at the last call boundary: eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x\n", g->eax, g->ecx,
        g->edx, g->esp, g->ebp);
    fatal_box(guest_function_at(ip), "exception inside translated code");
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    (void)once; (void)param; (void)ctx;
    open_log();
    rt_set_fatal_hook(fatal_box);
    {
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((unsigned char*)g_self + ((IMAGE_DOS_HEADER*)g_self)->e_lfanew);
        g_self_lo = (uintptr_t)g_self;
        g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    }
    AddVectoredExceptionHandler(0, crash_handler);
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("FFXI_RECOMP_RETAIL", path, MAX_PATH))
    {
        GetModuleFileNameA(g_self, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "FFXiMain.retail.dll");
    }
    rt_log("[recomp] retail image: %s\n", path);
    g_game = rt_load_image(path);
    if (!g_game)
    {
        fatal_box(0, "could not load the retail FFXiMain (see the log)");
        return FALSE;
    }
    /* The game's CRT entry (_DllMainCRTStartup), called like the loader would. */
    BOOL(WINAPI * entry)(HINSTANCE, DWORD, LPVOID) = (BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID))(uintptr_t)(rt_image_oep + rt_reloc_delta);
    if (!entry((HINSTANCE)g_game, DLL_PROCESS_ATTACH, NULL))
    {
        rt_log("[recomp] the game's DllMain(PROCESS_ATTACH) returned FALSE\n");
        return FALSE;
    }
    rt_log("[recomp] FFXiMain translation ready\n");
    return TRUE;
}

static FARPROC game_export(const char* name)
{
    if (!InitOnceExecuteOnce(&g_once, init, NULL, NULL))
        return NULL;
    return GetProcAddress(g_game, name);
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out)
{
    FARPROC fn = game_export("DllGetClassObject");
    return fn ? ((HRESULT(WINAPI*)(REFCLSID, REFIID, void**))fn)(clsid, iid, out) : E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (!g_game)
        return S_FALSE;
    FARPROC fn = game_export("DllCanUnloadNow");
    return fn ? ((HRESULT(WINAPI*)(void))fn)() : S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    FARPROC fn = game_export("DllRegisterServer");
    return fn ? ((HRESULT(WINAPI*)(void))fn)() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    FARPROC fn = game_export("DllUnregisterServer");
    return fn ? ((HRESULT(WINAPI*)(void))fn)() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    else if (reason == DLL_PROCESS_DETACH && g_game)
        bridge_trace_dump();
    return TRUE;
}
