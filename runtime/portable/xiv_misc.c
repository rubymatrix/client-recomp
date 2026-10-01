/* The small Win32 surface FINAL FANTASY XIV 1.23b reaches beyond kernel32: what each call is for
 * in the game decides how much of it there is here.
 *
 *   crypt32 / wintrust   the client checks its own Authenticode signature: the image is trusted
 *                        (WinVerifyTrust succeeds) and has no embedded signer to read.
 *   version              no version resources are read back (the game's version is game.ver).
 *   shell32              "My Documents" (where the game keeps settings, screenshots and logs) is a
 *                        folder of this host's (xiv_set_documents); ShellExecute opens nothing.
 *   advapi32             the W registry calls run the A shims (reg.c); the event log and the
 *                        user name answer plainly.
 *   oleaut32, comctl32   the VARIANT/SAFEARRAY calls WMI queries make, and window subclassing.
 *   iphlpapi, winhttp    no adapter table; no HTTP (the client's news and banner fetches fail
 *                        the way they do offline).
 *   xinput1_3            by ordinal: no controller connected (SDL input reaches the game through
 *                        DirectInput, input.c).
 *   d3d9 D3DPERF_*       profiling markers: ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"
#include "vfs.h"

#define S_OK 0u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define CRYPT_E_NO_MATCH 0x80092009u
#define DISP_E_TYPEMISMATCH 0x80020005u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u

/* --- crypt32 / wintrust ------------------------------------------------------------------------ */

static void sh_CryptQueryObject(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 11); }
static void sh_CertFindCertificateInStore(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 6); }
static void sh_CertFreeCertificateContext(Guest* g) { RET(1, 1); }
static void sh_CertCloseStore(Guest* g) { RET(1, 2); }
static void sh_CryptMsgGetParam(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 5); }
static void sh_CryptDecodeObject(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 7); }
static void sh_CryptMsgClose(Guest* g) { RET(1, 1); }
static void sh_WinVerifyTrust(Guest* g) { RET(0, 3); } /* ERROR_SUCCESS: trusted */

/* --- version ----------------------------------------------------------------------------------- */

static void sh_GetFileVersionInfoSize(Guest* g) { gt_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(0, 2); }
static void sh_GetFileVersionInfo(Guest* g) { gt_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(0, 4); }
static void sh_VerQueryValue(Guest* g) { RET(0, 4); }

/* --- shell32 ----------------------------------------------------------------------------------- */

#define DOCUMENTS_GUEST "C:\\Users\\Player\\Documents"
#define APPDATA_GUEST "C:\\Users\\Player\\AppData\\Roaming"

void xiv_set_documents(const char* host_dir)
{
    char appdata[1024];
    vfs_mount(DOCUMENTS_GUEST, host_dir);
    snprintf(appdata, sizeof appdata, "%s/AppData", host_dir);
    plat_mkdir(appdata);
    vfs_mount(APPDATA_GUEST, appdata);
}

/* An ITEMIDLIST is a CSIDL here: the pidl SHGetSpecialFolderLocation hands out holds it. */
static const char* csidl_path(uint32_t csidl)
{
    switch (csidl & 0xFF)
    {
    case 0x05: /* CSIDL_PERSONAL */
    case 0x0C: /* CSIDL_MYDOCUMENTS */
        return DOCUMENTS_GUEST;
    case 0x1A: /* CSIDL_APPDATA */
    case 0x1C: /* CSIDL_LOCAL_APPDATA */
    case 0x23: /* CSIDL_COMMON_APPDATA */
        return APPDATA_GUEST;
    default:
        return NULL;
    }
}

static void sh_SHGetSpecialFolderLocation(Guest* g)
{
    if (!csidl_path(ARG(1)))
        RET(E_FAIL, 3);
    uint32_t pidl = gheap_alloc(8, 1);
    wr32(pidl, 0x58495650u); /* "PVIX": our pidl */
    wr32(pidl + 4, ARG(1));
    wr32(ARG(2), pidl);
    RET(S_OK, 3);
}

static void sh_SHGetPathFromIDListW(Guest* g)
{
    uint32_t pidl = ARG(0), buf = ARG(1);
    const char* p = pidl && rd32(pidl) == 0x58495650u ? csidl_path(rd32(pidl + 4)) : NULL;
    if (!p)
        RET(0, 2);
    uint32_t i = 0;
    for (; p[i]; ++i)
        wr16(buf + 2 * i, (uint8_t)p[i]);
    wr16(buf + 2 * i, 0);
    RET(1, 2);
}

/* SHGetMalloc: an IMalloc over the guest heap. */
static void m_QueryInterface(Guest* g) { RET(E_NOTIMPL, 3); }
static void m_AddRef(Guest* g) { RET(1, 1); }
static void m_Release(Guest* g) { RET(1, 1); }
static void m_Alloc(Guest* g) { RET(gheap_alloc(ARG(1) ? ARG(1) : 1, 0), 2); }
static void m_Realloc(Guest* g) { RET(gheap_realloc(ARG(1), ARG(2), 0, 0), 3); }
static void m_Free(Guest* g) { if (ARG(1)) gheap_free(ARG(1)); RET(0, 2); }
static void m_GetSize(Guest* g) { RET(ARG(1) ? gheap_size(ARG(1)) : 0xFFFFFFFFu, 2); }
static void m_DidAlloc(Guest* g) { RET(ARG(1) && gheap_size(ARG(1)) != 0xFFFFFFFFu, 2); }
static void m_HeapMinimize(Guest* g) { RET(0, 1); }

static void sh_SHGetMalloc(Guest* g)
{
    static uint32_t obj;
    if (!obj)
    {
        static const char* const names[] = { "QueryInterface", "AddRef", "Release", "Alloc", "Realloc", "Free",
                                             "GetSize", "DidAlloc", "HeapMinimize" };
        uint32_t vtbl = gheap_alloc(4 * 9, 1);
        for (unsigned i = 0; i < 9; ++i)
        {
            char full[64];
            snprintf(full, sizeof full, "IMalloc::%s", names[i]);
            wr32(vtbl + 4 * i, thunk_for("shell32.dll", full));
        }
        obj = gheap_alloc(4, 1);
        wr32(obj, vtbl);
    }
    wr32(ARG(0), obj);
    RET(S_OK, 1);
}

static void sh_ShellExecute(Guest* g)
{
    rt_log("[recomp] ShellExecute: not opening anything on this host\n");
    RET(2, 6); /* SE_ERR_FNF: <= 32 is a failure */
}

/* --- advapi32 ---------------------------------------------------------------------------------- */

static uint32_t narrow(uint32_t w)
{
    if (!w)
        return 0;
    uint32_t n = 0;
    while (rd16(w + 2 * n))
        n++;
    uint32_t a = gheap_alloc(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(a + n, 0);
    return a;
}

static void forward(Guest* g, const char* dll, const char* name)
{
    Shim s = thunk_shim(dll, name);
    if (!s)
        rt_fatal(g, 0, name);
    s(g);
}

/* RegOpenKeyExW(key, subkey, options, sam, result) */
static void sh_RegOpenKeyExW(Guest* g)
{
    uint32_t sub = narrow(ARG(1));
    wr32(g->esp + 8, sub);
    forward(g, "advapi32.dll", "RegOpenKeyExA");
    if (sub)
        gheap_free(sub);
}

/* RegQueryValueExW(key, name, reserved, type*, data, cb*): string data is widened. */
static void sh_RegQueryValueExW(Guest* g)
{
    uint32_t name = narrow(ARG(1)), type = ARG(3), data = ARG(4), cbp = ARG(5);
    uint32_t cb = cbp ? rd32(cbp) : 0;
    uint32_t tmp = data ? gheap_alloc(cb + 1, 1) : 0, ttype = gheap_alloc(4, 1);
    wr32(g->esp + 8, name);
    wr32(g->esp + 16, ttype);
    wr32(g->esp + 20, tmp);
    forward(g, "advapi32.dll", "RegQueryValueExA");
    uint32_t t = rd32(ttype);
    if (type)
        wr32(type, t);
    if (g->eax == 0 && (t == 1 || t == 2)) /* REG_SZ, REG_EXPAND_SZ: the size doubles */
    {
        uint32_t n = rd32(cbp);
        if (data)
        {
            if (2 * n > cb)
                g->eax = 234; /* ERROR_MORE_DATA */
            else
                for (uint32_t i = 0; i < n; ++i)
                    wr16(data + 2 * i, rd8(tmp + i));
        }
        wr32(cbp, 2 * n);
    }
    else if (g->eax == 0 && data)
        memcpy(GUEST_PTR(data), GUEST_PTR(tmp), rd32(cbp));
    if (tmp)
        gheap_free(tmp);
    gheap_free(ttype);
    if (name)
        gheap_free(name);
}

static void sh_GetUserNameA(Guest* g)
{
    static const char name[] = "Player";
    uint32_t buf = ARG(0), np = ARG(1);
    if (rd32(np) < sizeof name)
    {
        wr32(np, sizeof name);
        gt_set_error(122); /* ERROR_INSUFFICIENT_BUFFER */
        RET(0, 2);
    }
    memcpy(GUEST_PTR(buf), name, sizeof name);
    wr32(np, sizeof name);
    RET(1, 2);
}

static void sh_RegisterEventSourceA(Guest* g) { RET(0x0E7E0001u, 2); }
static void sh_DeregisterEventSource(Guest* g) { RET(1, 1); }
static void sh_ReportEventA(Guest* g) { RET(1, 9); }

/* --- oleaut32 (by ordinal) --------------------------------------------------------------------- */

static void sh_VariantInit(Guest* g) { memset(GUEST_PTR(ARG(0)), 0, 16); RET(0, 1); }
static void sh_VariantClear(Guest* g)
{
    uint32_t v = ARG(0);
    if (rd16(v) == 8 /* VT_BSTR */ && rd32(v + 8))
        gheap_free(rd32(v + 8) - 4);
    memset(GUEST_PTR(v), 0, 16);
    RET(S_OK, 1);
}
static void sh_VariantChangeType(Guest* g) { RET(DISP_E_TYPEMISMATCH, 4); }
static void sh_SafeArrayCreate(Guest* g) { RET(0, 3); }
static void sh_SafeArrayPutElement(Guest* g) { RET(E_FAIL, 3); }

/* --- comctl32 (by ordinal): window subclassing -------------------------------------------------- */

static void sh_SetWindowSubclass(Guest* g) { RET(1, 4); }
static void sh_RemoveWindowSubclass(Guest* g) { RET(1, 3); }
static void sh_DefSubclassProc(Guest* g) { RET(0, 4); }

/* --- iphlpapi, winhttp ------------------------------------------------------------------------- */

static void sh_GetIfTable(Guest* g) { RET(ERROR_NOT_SUPPORTED, 3); }
static void sh_GetIpAddrTable(Guest* g) { RET(ERROR_NOT_SUPPORTED, 3); }

static void sh_WinHttpOpen(Guest* g) { gt_set_error(12029); RET(0, 5); } /* ERROR_WINHTTP_CANNOT_CONNECT */
static void sh_WinHttpConnect(Guest* g) { gt_set_error(12029); RET(0, 4); }
static void sh_WinHttpOpenRequest(Guest* g) { gt_set_error(12029); RET(0, 7); }
static void sh_WinHttpSendRequest(Guest* g) { gt_set_error(12029); RET(0, 7); }
static void sh_WinHttpReceiveResponse(Guest* g) { gt_set_error(12029); RET(0, 2); }
static void sh_WinHttpQueryHeaders(Guest* g) { gt_set_error(12029); RET(0, 6); }
static void sh_WinHttpQueryDataAvailable(Guest* g) { gt_set_error(12029); RET(0, 2); }
static void sh_WinHttpReadData(Guest* g) { gt_set_error(12029); RET(0, 4); }
static void sh_WinHttpWriteData(Guest* g) { gt_set_error(12029); RET(0, 4); }
static void sh_WinHttpSetOption(Guest* g) { RET(1, 4); }
static void sh_WinHttpSetStatusCallback(Guest* g) { RET(0, 4); }
static void sh_WinHttpCloseHandle(Guest* g) { RET(1, 1); }

/* --- xinput1_3 (by ordinal), D3DPERF ----------------------------------------------------------- */

static void sh_XInputGetState(Guest* g) { RET(ERROR_DEVICE_NOT_CONNECTED, 2); }
static void sh_XInputSetState(Guest* g) { RET(ERROR_DEVICE_NOT_CONNECTED, 2); }
static void sh_XInputEnable(Guest* g) { RET(0, 1); }

static void sh_D3DPERF_BeginEvent(Guest* g) { RET(0, 2); }
static void sh_D3DPERF_EndEvent(Guest* g) { RET(0, 0); }
static void sh_D3DPERF_SetOptions(Guest* g) { RET(0, 1); }

/* --- imm32: no input method on this host (SDL's text input supplies characters) ---------------- */

static void sh_ImmDisableTextFrameService(Guest* g) { RET(1, 1); }
static void sh_ImmAssociateContext(Guest* g) { RET(0, 2); }
static void sh_ImmGetCandidateListW(Guest* g) { RET(0, 4); }
static void sh_ImmGetCompositionStringW(Guest* g) { RET(0, 4); }

/* CoGetClassObject(clsid, context, server info, iid, out): no COM servers beyond ole.c's */
static void sh_CoGetClassObject(Guest* g)
{
    if (ARG(4))
        wr32(ARG(4), 0);
    RET(0x80040154u, 5); /* REGDB_E_CLASSNOTREG */
}

static const ShimDef XIV_MISC[] = {
    { "ole32.dll", "CoGetClassObject", sh_CoGetClassObject },
    { "imm32.dll", "ImmDisableTextFrameService", sh_ImmDisableTextFrameService },
    { "imm32.dll", "ImmAssociateContext", sh_ImmAssociateContext },
    { "imm32.dll", "ImmGetCandidateListW", sh_ImmGetCandidateListW },
    { "imm32.dll", "ImmGetCompositionStringW", sh_ImmGetCompositionStringW },
    { "crypt32.dll", "CryptQueryObject", sh_CryptQueryObject },
    { "crypt32.dll", "CertFindCertificateInStore", sh_CertFindCertificateInStore },
    { "crypt32.dll", "CertFreeCertificateContext", sh_CertFreeCertificateContext },
    { "crypt32.dll", "CertCloseStore", sh_CertCloseStore },
    { "crypt32.dll", "CryptMsgGetParam", sh_CryptMsgGetParam },
    { "crypt32.dll", "CryptDecodeObject", sh_CryptDecodeObject },
    { "crypt32.dll", "CryptMsgClose", sh_CryptMsgClose },
    { "wintrust.dll", "WinVerifyTrust", sh_WinVerifyTrust },
    { "version.dll", "GetFileVersionInfoSizeA", sh_GetFileVersionInfoSize },
    { "version.dll", "GetFileVersionInfoSizeW", sh_GetFileVersionInfoSize },
    { "version.dll", "GetFileVersionInfoA", sh_GetFileVersionInfo },
    { "version.dll", "GetFileVersionInfoW", sh_GetFileVersionInfo },
    { "version.dll", "VerQueryValueA", sh_VerQueryValue },
    { "version.dll", "VerQueryValueW", sh_VerQueryValue },
    { "shell32.dll", "SHGetSpecialFolderLocation", sh_SHGetSpecialFolderLocation },
    { "shell32.dll", "SHGetPathFromIDListW", sh_SHGetPathFromIDListW },
    { "shell32.dll", "SHGetMalloc", sh_SHGetMalloc },
    { "shell32.dll", "ShellExecuteA", sh_ShellExecute },
    { "shell32.dll", "ShellExecuteW", sh_ShellExecute },
    { "shell32.dll", "IMalloc::QueryInterface", m_QueryInterface },
    { "shell32.dll", "IMalloc::AddRef", m_AddRef },
    { "shell32.dll", "IMalloc::Release", m_Release },
    { "shell32.dll", "IMalloc::Alloc", m_Alloc },
    { "shell32.dll", "IMalloc::Realloc", m_Realloc },
    { "shell32.dll", "IMalloc::Free", m_Free },
    { "shell32.dll", "IMalloc::GetSize", m_GetSize },
    { "shell32.dll", "IMalloc::DidAlloc", m_DidAlloc },
    { "shell32.dll", "IMalloc::HeapMinimize", m_HeapMinimize },
    { "advapi32.dll", "RegOpenKeyExW", sh_RegOpenKeyExW },
    { "advapi32.dll", "RegQueryValueExW", sh_RegQueryValueExW },
    { "advapi32.dll", "GetUserNameA", sh_GetUserNameA },
    { "advapi32.dll", "RegisterEventSourceA", sh_RegisterEventSourceA },
    { "advapi32.dll", "DeregisterEventSource", sh_DeregisterEventSource },
    { "advapi32.dll", "ReportEventA", sh_ReportEventA },
    { "oleaut32.dll", "#8", sh_VariantInit },
    { "oleaut32.dll", "#9", sh_VariantClear },
    { "oleaut32.dll", "#12", sh_VariantChangeType },
    { "oleaut32.dll", "#15", sh_SafeArrayCreate },
    { "oleaut32.dll", "#26", sh_SafeArrayPutElement },
    { "comctl32.dll", "#410", sh_SetWindowSubclass },
    { "comctl32.dll", "#412", sh_RemoveWindowSubclass },
    { "comctl32.dll", "#413", sh_DefSubclassProc },
    { "iphlpapi.dll", "GetIfTable", sh_GetIfTable },
    { "iphlpapi.dll", "GetIpAddrTable", sh_GetIpAddrTable },
    { "winhttp.dll", "WinHttpOpen", sh_WinHttpOpen },
    { "winhttp.dll", "WinHttpConnect", sh_WinHttpConnect },
    { "winhttp.dll", "WinHttpOpenRequest", sh_WinHttpOpenRequest },
    { "winhttp.dll", "WinHttpSendRequest", sh_WinHttpSendRequest },
    { "winhttp.dll", "WinHttpReceiveResponse", sh_WinHttpReceiveResponse },
    { "winhttp.dll", "WinHttpQueryHeaders", sh_WinHttpQueryHeaders },
    { "winhttp.dll", "WinHttpQueryDataAvailable", sh_WinHttpQueryDataAvailable },
    { "winhttp.dll", "WinHttpReadData", sh_WinHttpReadData },
    { "winhttp.dll", "WinHttpWriteData", sh_WinHttpWriteData },
    { "winhttp.dll", "WinHttpSetOption", sh_WinHttpSetOption },
    { "winhttp.dll", "WinHttpSetStatusCallback", sh_WinHttpSetStatusCallback },
    { "winhttp.dll", "WinHttpCloseHandle", sh_WinHttpCloseHandle },
    { "xinput1_3.dll", "#2", sh_XInputGetState },
    { "xinput1_3.dll", "#3", sh_XInputSetState },
    { "xinput1_3.dll", "#5", sh_XInputEnable },
    { "d3d9.dll", "D3DPERF_BeginEvent", sh_D3DPERF_BeginEvent },
    { "d3d9.dll", "D3DPERF_EndEvent", sh_D3DPERF_EndEvent },
    { "d3d9.dll", "D3DPERF_SetOptions", sh_D3DPERF_SetOptions },
    { NULL, NULL, NULL },
};

void xiv_misc_init(void) { thunk_register(XIV_MISC); }
