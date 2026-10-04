/* KERNEL32 for 64-bit hosts, part 3 (R3.1): the imports left after the live R3.0 trace -
 * resources, locale (en-US, code page 1252, as the US client runs), global memory, pointer
 * probes, process calls, and the SEH unwinding the CRT does. Written against plat.h only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "thunk.h"

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define LCID_EN_US 0x0409u

/* --- process ----------------------------------------------------------------------------------- */
static void sh_FatalAppExitA(Guest* g)
{
    rt_log("[recomp] FatalAppExit: %s\n", ARG(1) ? ARGS(1) : "");
    exit(3);
}

static void sh_TerminateProcess(Guest* g)
{
    if (ARG(0) == 0xFFFFFFFFu) /* GetCurrentProcess() */
    {
        rt_log("[recomp] TerminateProcess(self, %u)\n", ARG(1));
        exit((int)ARG(1));
    }
    RET(0, 2);
}

/* CreateProcessA(app, cmdline, ...10 arguments): the game starts nothing we can run */
static void sh_CreateProcessA(Guest* g)
{
    rt_log("[recomp] CreateProcessA(%s, %s): not supported\n", ARG(0) ? ARGS(0) : "-", ARG(1) ? ARGS(1) : "-");
    gt_set_error(ERROR_FILE_NOT_FOUND);
    RET(0, 10);
}

static void sh_SetConsoleCtrlHandler(Guest* g) { RET(1, 2); }
static void sh_SetStdHandle(Guest* g) { RET(1, 2); }
static void sh_SetEnvironmentVariableA(Guest* g) { RET(1, 2); } /* the guest reads no variables back */

static void sh_UnhandledExceptionFilter(Guest* g)
{
    uint32_t rec = ARG(0) ? rd32(ARG(0)) : 0;
    uint32_t ctx = ARG(0) ? rd32(ARG(0) + 4) : 0;
    rt_log("[recomp] UnhandledExceptionFilter: code %08x at %08x\n", rec ? rd32(rec) : 0, rec ? rd32(rec + 12) : 0);
    if (ctx)
    {
        /* The CRT's __invoke_watson (0x9d2194; code c000000d = an _invalid_parameter, e.g. an STL
         * bounds check or a *_s buffer too small) fills CONTEXT with the caller's Esp/Ebp: list the
         * code addresses on that stack, nearest first, so the failing caller can be found (gsym). */
        uint32_t esp = rd32(ctx + 0xc4), ebp = rd32(ctx + 0xb4);
        rt_log("  context eip=%08x esp=%08x ebp=%08x; code addresses on the stack:", rd32(ctx + 0xb8), esp, ebp);
        for (int i = 0, n = 0; i < 512 && n < 24; ++i)
        {
            uint32_t v = rd32(esp + 4u * i);
            if (v >= 0x00401000u && v < 0x00f3e000u)
            {
                rt_log(" %08x", v);
                ++n;
            }
        }
        rt_log("\n");
    }
    RET(1, 1); /* EXCEPTION_EXECUTE_HANDLER */
}

/* XIV_CPUS: the processor count every query reports, when set (0 otherwise: GetSystemInfo says 2,
 * the affinity masks and GetLogicalProcessorInformation 4, as the game ran best) */
unsigned k32_cpus(void)
{
    static int n = -1;
    if (n < 0)
    {
        const char* e = getenv("XIV_CPUS");
        n = e && atoi(e) > 0 && atoi(e) <= 16 ? atoi(e) : 0;
    }
    return (unsigned)n;
}

/* GetSystemInfo: a 32-bit process on an x86 machine, as the game was built for */
static void sh_GetSystemInfo(Guest* g)
{
    uint32_t p = ARG(0);
    memset(GUEST_PTR(p), 0, 36);
    wr32(p + 4, 4096);         /* dwPageSize */
    wr32(p + 8, 0x00010000u);  /* lpMinimumApplicationAddress */
    wr32(p + 12, 0x7FFEFFFFu); /* lpMaximumApplicationAddress */
    unsigned cpus = k32_cpus() ? k32_cpus() : 2;
    wr32(p + 16, (1u << cpus) - 1u); /* dwActiveProcessorMask */
    wr32(p + 20, cpus);        /* dwNumberOfProcessors */
    wr32(p + 24, 586);         /* dwProcessorType: PROCESSOR_INTEL_PENTIUM */
    wr32(p + 28, 0x10000);     /* dwAllocationGranularity */
    wr16(p + 32, 6);           /* wProcessorLevel */
    wr16(p + 34, 0x0801);      /* wProcessorRevision: the synthetic CPUID's family 6 model 8 */
    RET(0, 1);
}

static void sh_MulDiv(Guest* g)
{
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), c = (int32_t)ARG(2);
    if (!c)
        RET(0xFFFFFFFFu, 3);
    int64_t p = a * b, h = (c < 0 ? -c : c) / 2;
    int64_t r = ((p < 0) != (c < 0) ? p - h : p + h) / c; /* rounded half away from zero */
    RET(r > 0x7FFFFFFF || r < -0x7FFFFFFF - 1 ? 0xFFFFFFFFu : (uint32_t)(int32_t)r, 3);
}

/* --- pointer probes: committed pages of the guest window ----------------------------------------- */
static int readable(uint32_t p, uint32_t n)
{
    if (!p)
        return 0;
    if (!n)
        return 1;
    if (p + n - 1 < p)
        return 0;
    for (uint32_t a = p & ~0xFFFu; a <= ((p + n - 1) & ~0xFFFu); a += 0x1000)
    {
        if (!gwin_is_committed(a))
            return 0;
        if (a == 0xFFFFF000u)
            break;
    }
    return 1;
}

static void sh_IsBadReadPtr(Guest* g) { RET(!readable(ARG(0), ARG(1)), 2); }
static void sh_IsBadWritePtr(Guest* g) { RET(!readable(ARG(0), ARG(1)), 2); }
static void sh_IsBadCodePtr(Guest* g) { RET(!readable(ARG(0), 1) && !thunk_name(ARG(0)), 1); }

/* --- global memory: fixed blocks, the handle is the pointer ----------------------------------------- */
static void sh_GlobalAlloc(Guest* g) { RET(gheap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0), 2); } /* GMEM_ZEROINIT */
static void sh_GlobalLock(Guest* g) { RET(ARG(0), 1); }
static void sh_GlobalUnlock(Guest* g) { gt_set_error(0); RET(0, 1); }

/* --- resources ---------------------------------------------------------------------------------- */
static uint32_t module_base(uint32_t h) { return h ? h : rt_image_base; }

static void sh_FindResourceA(Guest* g)
{
    uint32_t r = pe_find_resource(module_base(ARG(0)), ARG(2), ARG(1), 0);
    if (!r)
        gt_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    RET(r, 3);
}

static void sh_FindResourceW(Guest* g)
{
    uint32_t r = pe_find_resource(module_base(ARG(0)), ARG(2), ARG(1), 1);
    if (!r)
        gt_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    RET(r, 3);
}

/* the HRSRC is the data entry: RVA, size */
static void sh_LoadResource(Guest* g) { RET(ARG(1) ? module_base(ARG(0)) + rd32(ARG(1)) : 0, 2); }
static void sh_SizeofResource(Guest* g) { RET(ARG(1) ? rd32(ARG(1) + 4) : 0, 2); }
static void sh_LockResource(Guest* g) { RET(ARG(0), 1); }

/* --- locale: en-US, ANSI code page 1252 ---------------------------------------------------------- */
static void sh_GetUserDefaultLCID(Guest* g) { RET(LCID_EN_US, 0); }
static void sh_IsValidLocale(Guest* g) { RET(ARG(0) == LCID_EN_US, 2); }
static void sh_IsDBCSLeadByte(Guest* g) { RET(0, 1); }

static const char* locale_info(uint32_t type)
{
    switch (type & 0xFFFFu) /* without LOCALE_NOUSEROVERRIDE and friends */
    {
    case 0x0001: return "0409";          /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";           /* LOCALE_SABBREVLANGNAME */
    case 0x0005: return "1";             /* LOCALE_ICOUNTRY */
    case 0x0006: return "United States";
    case 0x0007: return "USA";           /* LOCALE_SABBREVCTRYNAME */
    case 0x000B: return "437";           /* LOCALE_IDEFAULTCODEPAGE */
    case 0x000C: return ",";             /* LOCALE_SLIST */
    case 0x000E: return ".";             /* LOCALE_SDECIMAL */
    case 0x000F: return ",";             /* LOCALE_STHOUSAND */
    case 0x0014: return "$";             /* LOCALE_SCURRENCY */
    case 0x001D: return "/";             /* LOCALE_SDATE */
    case 0x001E: return ":";             /* LOCALE_STIME */
    case 0x001F: return "M/d/yyyy";      /* LOCALE_SSHORTDATE */
    case 0x0059: return "en";            /* LOCALE_SISO639LANGNAME */
    case 0x005A: return "US";            /* LOCALE_SISO3166CTRYNAME */
    case 0x1001: return "English";       /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "United States"; /* LOCALE_SENGCOUNTRY */
    case 0x1004: return "1252";          /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x1009: return "1";             /* LOCALE_IDEFAULTEBCDICCODEPAGE-ish: unused */
    default: return NULL;
    }
}

/* GetLocaleInfoA/W(Locale, LCType, lpLCData, cchData): characters written, with the NUL */
static void locale_common(Guest* g, int wide)
{
    const char* s = locale_info(ARG(1));
    if (!s)
    {
        gt_set_error(ERROR_INVALID_PARAMETER);
        RET(0, 4);
    }
    uint32_t len = (uint32_t)strlen(s) + 1, buf = ARG(2), n = ARG(3);
    if (!n)
        RET(len, 4);
    if (n < len)
    {
        gt_set_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0, 4);
    }
    for (uint32_t i = 0; i < len; ++i)
        if (wide)
            wr16(buf + 2 * i, (uint8_t)s[i]);
        else
            wr8(buf + i, (uint8_t)s[i]);
    RET(len, 4);
}

static void sh_GetLocaleInfoA(Guest* g) { locale_common(g, 0); }
static void sh_GetLocaleInfoW(Guest* g) { locale_common(g, 1); }

/* EnumSystemLocalesA(lpLocaleEnumProc, dwFlags): one locale */
static void sh_EnumSystemLocalesA(Guest* g)
{
    uint32_t s = gheap_strdup("00000409");
    uint32_t a[1] = { s };
    guest_call(ARG(0), 1, a);
    gheap_free(s);
    RET(1, 2);
}

/* CompareStringA/W(Locale, dwCmpFlags, s1, n1, s2, n2): CSTR_LESS_THAN 1, EQUAL 2, GREATER 3.
 * Ordinal comparison, folding ASCII case for NORM_IGNORECASE. */
static void compare_common(Guest* g, int wide)
{
    uint32_t s1 = ARG(2), s2 = ARG(4), unit = wide ? 2 : 1;
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    int fold = ARG(1) & 1;
    if (n1 < 0)
        for (n1 = 0; wide ? rd16(s1 + 2 * (uint32_t)n1) : rd8(s1 + (uint32_t)n1); ++n1) {}
    if (n2 < 0)
        for (n2 = 0; wide ? rd16(s2 + 2 * (uint32_t)n2) : rd8(s2 + (uint32_t)n2); ++n2) {}
    for (int32_t i = 0;; ++i)
    {
        if (i == n1 || i == n2)
            RET(n1 == n2 ? 2 : i == n1 ? 1 : 3, 6);
        uint32_t a = wide ? rd16(s1 + unit * (uint32_t)i) : rd8(s1 + (uint32_t)i);
        uint32_t b = wide ? rd16(s2 + unit * (uint32_t)i) : rd8(s2 + (uint32_t)i);
        if (fold)
        {
            a = a >= 'a' && a <= 'z' ? a - 32 : a;
            b = b >= 'a' && b <= 'z' ? b - 32 : b;
        }
        if (a != b)
            RET(a < b ? 1 : 3, 6);
    }
}

static void sh_CompareStringA(Guest* g) { compare_common(g, 0); }
static void sh_CompareStringW(Guest* g) { compare_common(g, 1); }

/* --- structured exception handling --------------------------------------------------------------- *
 * The CRT's __global_unwind2 calls RtlUnwind to run the __finally/cleanup handlers on the SEH
 * chain (fs:[0], in the TEB) down to a target frame. That works here: handlers are guest
 * functions. What does not yet is RaiseException dispatching to a handler that resumes somewhere
 * else (a C++ catch, an __except block): that needs guest SEH support in the translation (R3 open
 * item), so it stops with the exception code rather than running on wrongly. */
#define EH_UNWINDING 2u
#define EH_EXIT_UNWIND 4u

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) */
static void sh_RtlUnwind(Guest* g)
{
    uint32_t target = ARG(0), rec = ARG(2), ret = ARG(3), own = 0;
    uint32_t teb = gt_self()->teb;
    if (!rec)
    {
        own = rec = gheap_alloc(80, 1);
        wr32(rec, 0xC0000027u); /* STATUS_UNWIND */
    }
    wr32(rec + 4, rd32(rec + 4) | EH_UNWINDING | (target ? 0 : EH_EXIT_UNWIND));
    for (uint32_t frame = rd32(teb); frame != 0xFFFFFFFFu && frame && frame != target; frame = rd32(teb))
    {
        uint32_t a[4] = { rec, frame, 0, 0 };
        guest_call(rd32(frame + 4), 4, a);
        wr32(teb, rd32(frame)); /* unlink it */
    }
    if (own)
        gheap_free(own);
    RET(ret, 4);
}

/* RaiseException(dwExceptionCode, dwExceptionFlags, nNumberOfArguments, lpArguments) */
static void sh_RaiseException(Guest* g)
{
    uint32_t code = ARG(0);
    if (code == 0x406D1388u) /* MS_VC_EXCEPTION: naming a thread for a debugger */
        RET(0, 4);
    char msg[96];
    snprintf(msg, sizeof msg, "RaiseException(%08x): guest SEH dispatch is not supported yet", code);
    rt_fatal(g, 0, msg);
}

static const ShimDef K32_MISC[] = {
    { "kernel32.dll", "FatalAppExitA", sh_FatalAppExitA },
    { "kernel32.dll", "TerminateProcess", sh_TerminateProcess },
    { "kernel32.dll", "CreateProcessA", sh_CreateProcessA },
    { "kernel32.dll", "SetConsoleCtrlHandler", sh_SetConsoleCtrlHandler },
    { "kernel32.dll", "SetStdHandle", sh_SetStdHandle },
    { "kernel32.dll", "SetEnvironmentVariableA", sh_SetEnvironmentVariableA },
    { "kernel32.dll", "UnhandledExceptionFilter", sh_UnhandledExceptionFilter },
    { "kernel32.dll", "GetSystemInfo", sh_GetSystemInfo },
    { "kernel32.dll", "MulDiv", sh_MulDiv },
    { "kernel32.dll", "IsBadReadPtr", sh_IsBadReadPtr },
    { "kernel32.dll", "IsBadWritePtr", sh_IsBadWritePtr },
    { "kernel32.dll", "IsBadCodePtr", sh_IsBadCodePtr },
    { "kernel32.dll", "GlobalAlloc", sh_GlobalAlloc },
    { "kernel32.dll", "GlobalLock", sh_GlobalLock },
    { "kernel32.dll", "GlobalUnlock", sh_GlobalUnlock },
    { "kernel32.dll", "FindResourceA", sh_FindResourceA },
    { "kernel32.dll", "FindResourceW", sh_FindResourceW },
    { "kernel32.dll", "LoadResource", sh_LoadResource },
    { "kernel32.dll", "SizeofResource", sh_SizeofResource },
    { "kernel32.dll", "LockResource", sh_LockResource },
    { "kernel32.dll", "GetUserDefaultLCID", sh_GetUserDefaultLCID },
    { "kernel32.dll", "IsValidLocale", sh_IsValidLocale },
    { "kernel32.dll", "IsDBCSLeadByte", sh_IsDBCSLeadByte },
    { "kernel32.dll", "GetLocaleInfoA", sh_GetLocaleInfoA },
    { "kernel32.dll", "GetLocaleInfoW", sh_GetLocaleInfoW },
    { "kernel32.dll", "EnumSystemLocalesA", sh_EnumSystemLocalesA },
    { "kernel32.dll", "CompareStringA", sh_CompareStringA },
    { "kernel32.dll", "CompareStringW", sh_CompareStringW },
    { "kernel32.dll", "RtlUnwind", sh_RtlUnwind },
    { "kernel32.dll", "RaiseException", sh_RaiseException },
    { NULL, NULL, NULL },
};

void k32_misc_init(void)
{
    thunk_register(K32_MISC);
}
