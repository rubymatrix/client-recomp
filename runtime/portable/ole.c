/* ole32 / OLEAUT32 for 64-bit hosts (R3.1): the COM plumbing FFXi.dll and FFXiMain.dll use.
 *
 * CoCreateInstance knows the classes the translated modules implement - FFXiMain's GameMain and
 * FFXi.dll's FxFileManager, which FFXi.dll creates - and reaches them the way COM would through
 * the registry: the module's DllGetClassObject, then IClassFactory::CreateInstance. Every other
 * class is "not registered" (REGDB_E_CLASSNOTREG); that covers the DirectShow graph FFXiMain builds for its intro movie,
 * which retail also survives without. Memory APIs use the guest heap; BSTRs keep their Win32
 * layout (a byte length before the characters). */
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "input.h"
#include "k32.h"
#include "pe.h"
#include "thunk.h"

#define S_OK 0u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define REGDB_E_CLASSNOTREG 0x80040154u

static const uint8_t IID_IClassFactory[16] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

/* The classes the translated modules serve, and where: registered by the host (ole_register_class),
 * as InprocServer32 entries would say on Windows. */
static struct
{
    uint8_t clsid[16];
    uint32_t module;
} g_classes[8];
static unsigned g_nclasses;

void ole_register_class(const uint8_t clsid[16], uint32_t module_base)
{
    if (g_nclasses < 8)
    {
        memcpy(g_classes[g_nclasses].clsid, clsid, 16);
        g_classes[g_nclasses++].module = module_base;
    }
}

static void sh_CoInitialize(Guest* g) { RET(S_OK, 1); }
static void sh_CoInitializeEx(Guest* g) { RET(S_OK, 2); }
static void sh_CoUninitialize(Guest* g) { RET(0, 0); }
static void sh_CoFreeUnusedLibraries(Guest* g) { RET(0, 0); }
static void sh_CoTaskMemAlloc(Guest* g) { RET(gheap_alloc(ARG(0), 0), 1); }
static void sh_CoTaskMemFree(Guest* g) { gheap_free(ARG(0)); RET(0, 1); }
static void sh_CoTaskMemRealloc(Guest* g) { RET(gheap_realloc(ARG(0), ARG(1), 0, 0), 2); }

static const uint8_t CLSID_WbemLocator[16] = { 0x11, 0xF8, 0x90, 0x45, 0x3A, 0x1D, 0xD0, 0x11,
                                               0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 };
static uint32_t wmi_locator(void); /* below: the WMI IsXInputDevice asks */

/* CoCreateInstance(rclsid, outer, clsctx, riid, ppv) */
/* Other files' COM classes (xaudio2.c): 1 and the new object if the class is theirs. */
int (*ole_class_hook)(uint32_t clsid, uint32_t riid, uint32_t* obj);

static void sh_CoCreateInstance(Guest* g)
{
    uint32_t clsid = ARG(0), riid = ARG(3), ppv = ARG(4);
    if (ppv)
        wr32(ppv, 0);
    uint32_t made = 0;
    if (ole_class_hook && ole_class_hook(clsid, riid, &made))
    {
        if (ppv)
            wr32(ppv, made);
        RET(made ? S_OK : 0x80004002u /* E_NOINTERFACE */, 5);
    }
    if (!memcmp(GUEST_PTR(clsid), CLSID_WbemLocator, 16))
    {
        if (ppv)
            wr32(ppv, wmi_locator());
        RET(S_OK, 5);
    }
    uint32_t module = 0;
    for (unsigned i = 0; i < g_nclasses; ++i)
        if (!memcmp(GUEST_PTR(clsid), g_classes[i].clsid, 16))
            module = g_classes[i].module;
    if (!module)
    {
        const uint8_t* c = GUEST_PTR(clsid);
        rt_log("[recomp] CoCreateInstance({%02X%02X%02X%02X-...}): class not registered on this host\n", c[3], c[2], c[1], c[0]);
        RET(REGDB_E_CLASSNOTREG, 5);
    }
    uint32_t get = pe_export_at(module, "DllGetClassObject");
    uint32_t iid_cf = gheap_alloc(16, 0), factory = gheap_alloc(4, 1);
    memcpy(GUEST_PTR(iid_cf), IID_IClassFactory, 16);
    uint32_t a[3] = { clsid, iid_cf, factory };
    uint32_t hr = guest_call(get, 3, a);
    if (!hr && rd32(factory))
    {
        uint32_t cf = rd32(factory);
        uint32_t ci[4] = { cf, 0, riid, ppv };
        hr = guest_call(rd32(rd32(cf) + 12), 4, ci); /* IClassFactory::CreateInstance */
        uint32_t rel[1] = { cf };
        guest_call(rd32(rd32(cf) + 8), 1, rel); /* Release */
    }
    gheap_free(iid_cf);
    gheap_free(factory);
    RET(hr, 5);
}

/* --- OLEAUT32 ----------------------------------------------------------------------------------- */
static uint32_t bstr_alloc(uint32_t src, uint32_t chars)
{
    uint32_t p = gheap_alloc(4 + 2 * chars + 2, 0);
    if (!p)
        return 0;
    wr32(p, 2 * chars);
    if (src)
        memcpy(GUEST_PTR(p + 4), GUEST_PTR(src), 2 * chars);
    wr16(p + 4 + 2 * chars, 0);
    return p + 4;
}

static void sh_SysAllocString(Guest* g)
{
    uint32_t s = ARG(0), n = 0;
    if (!s)
        RET(0, 1);
    while (rd16(s + 2 * n))
        n++;
    RET(bstr_alloc(s, n), 1);
}

static void sh_SysAllocStringLen(Guest* g) { RET(bstr_alloc(ARG(0), ARG(1)), 2); }
static void sh_SysFreeString(Guest* g) { if (ARG(0)) gheap_free(ARG(0) - 4); RET(0, 1); }
static void sh_SysStringLen(Guest* g) { RET(ARG(0) ? rd32(ARG(0) - 4) / 2 : 0, 1); }

/* type libraries: only used to (un)register the COM servers, which nothing here needs */
static void sh_LoadTypeLib(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(E_NOTIMPL, 2); }
static void sh_RegisterTypeLib(Guest* g) { RET(E_NOTIMPL, 3); }

/* VarUI4FromStr(strIn, lcid, flags, pulOut): a decimal number */
static void sh_VarUI4FromStr(Guest* g)
{
    uint32_t s = ARG(0), v = 0;
    for (uint16_t c; (c = rd16(s)) >= '0' && c <= '9'; s += 2)
        v = v * 10 + (c - '0');
    wr32(ARG(3), v);
    RET(S_OK, 4);
}

/* USER32!CharNextA: code page 1252 has no lead bytes */
static void sh_CharNextA(Guest* g) { RET(rd8(ARG(0)) ? ARG(0) + 1 : ARG(0), 1); }

/* DCOM security on a proxy: there are no proxies here */
static void sh_CoSetProxyBlanket(Guest* g) { RET(S_OK, 8); }

/* --- WMI: just enough for Microsoft's IsXInputDevice ----------------------------------------------------
 * With XInput on, FFXiMain asks WMI which PnP devices are XInput devices (a DeviceID with "IG_"),
 * reads their VID_ and PID_, and leaves any DirectInput device whose guidProduct.Data1 is
 * MAKELONG(VID, PID) to XInput. Without an answer the Xbox controller stayed a DirectInput device,
 * read with the XInput button layout. Here root\cimv2's Win32_PNPEntity has one entry per gamepad,
 * an Xbox 360 controller (VID 045E, PID 028E): the product GUID dinput.c reports for every pad. The
 * objects are never freed; the game asks once per device enumeration. */
#define WBEM_S_FALSE 1u
#define WBEM_E_NOT_FOUND 0x80041002u
static uint32_t g_vtbl_locator, g_vtbl_services, g_vtbl_enum, g_vtbl_object;

static uint32_t wmi_object(uint32_t vtbl, uint32_t field)
{
    uint32_t o = gheap_alloc(8, 1);
    wr32(o, vtbl);
    wr32(o + 4, field);
    return o;
}

/* n slots; the named ones at their indexes, every other slot a thunk that traps with its name */
static uint32_t wmi_vtbl(const char* iface, unsigned n, const unsigned* at, const char* const* names)
{
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (unsigned i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::#%u", iface, i);
        for (unsigned k = 0; names[k]; ++k)
            if (at[k] == i)
                snprintf(full, sizeof full, "%s::%s", iface, names[k]);
        wr32(v + 4 * i, thunk_for("wbemprox.dll", full));
    }
    return v;
}

static uint32_t wmi_locator(void)
{
    if (!g_vtbl_locator)
    {
        static const char* const loc[] = { "AddRef", "Release", "ConnectServer", NULL };
        static const char* const svc[] = { "AddRef", "Release", "CreateInstanceEnum", NULL };
        static const char* const en[] = { "AddRef", "Release", "Next", NULL };
        static const char* const obj[] = { "AddRef", "Release", "Get", NULL };
        static const unsigned loc_at[] = { 1, 2, 3 }, svc_at[] = { 1, 2, 18 }, en_at[] = { 1, 2, 4 }, obj_at[] = { 1, 2, 4 };
        g_vtbl_locator = wmi_vtbl("IWbemLocator", 4, loc_at, loc);
        g_vtbl_services = wmi_vtbl("IWbemServices", 37, svc_at, svc);
        g_vtbl_enum = wmi_vtbl("IEnumWbemClassObject", 9, en_at, en);
        g_vtbl_object = wmi_vtbl("IWbemClassObject", 24, obj_at, obj);
    }
    return wmi_object(g_vtbl_locator, 0);
}

static void WMI_AddRef(Guest* g) { RET(1, 1); }
static void WMI_Release(Guest* g) { RET(1, 1); }

/* IWbemLocator::ConnectServer(this, strNetworkResource, strUser, strPassword, strLocale,
 * lSecurityFlags, strAuthority, pCtx, ppNamespace) */
static void WMI_ConnectServer(Guest* g)
{
    wr32(ARG(8), wmi_object(g_vtbl_services, 0));
    RET(S_OK, 9);
}

/* IWbemServices::CreateInstanceEnum(this, strFilter, lFlags, pCtx, ppEnum): Win32_PNPEntity */
static void WMI_CreateInstanceEnum(Guest* g)
{
    static int said;
    if (!said++)
        rt_log("[recomp] wmi: Win32_PNPEntity asked for (IsXInputDevice): %d gamepad(s) listed as XInput\n", input_pad_count());
    wr32(ARG(4), wmi_object(g_vtbl_enum, 0));
    RET(S_OK, 5);
}

/* IEnumWbemClassObject::Next(this, lTimeout, uCount, apObjects, puReturned): a gamepad each, from the
 * enumerator's cursor */
static void WMI_Next(Guest* g)
{
    uint32_t self = ARG(0), want = ARG(2), out = ARG(3), got = 0;
    uint32_t at = rd32(self + 4), pads = (uint32_t)input_pad_count();
    while (got < want && at < pads)
        wr32(out + 4 * got++, wmi_object(g_vtbl_object, at++));
    wr32(self + 4, at);
    if (ARG(4))
        wr32(ARG(4), got);
    RET(got == want ? S_OK : WBEM_S_FALSE, 5);
}

/* IWbemClassObject::Get(this, wszName, lFlags, pVal, pType, plFlavor): only "DeviceID" */
static void WMI_Get(Guest* g)
{
    uint32_t name = ARG(1), var = ARG(3);
    static const char want[] = "DeviceID";
    int is_id = name != 0;
    for (unsigned i = 0; is_id && i < sizeof want; ++i)
        is_id = rd16(name + 2 * i) == (uint16_t)(uint8_t)want[i];
    if (!is_id || !var)
        RET(WBEM_E_NOT_FOUND, 6);
    char id[64];
    uint32_t pad = rd32(ARG(0) + 4);
    snprintf(id, sizeof id, "USB\\VID_045E&PID_028E&IG_%02u\\XIRECOMP&%u", pad, pad);
    uint32_t n = (uint32_t)strlen(id), b = bstr_alloc(0, n);
    for (uint32_t i = 0; i < n; ++i)
        wr16(b + 2 * i, (uint16_t)(uint8_t)id[i]);
    memset(GUEST_PTR(var), 0, 16);
    wr16(var, 8);  /* VARIANT: vt VT_BSTR */
    wr32(var + 8, b);
    if (ARG(4))
        wr32(ARG(4), 8); /* CIM_STRING */
    if (ARG(5))
        wr32(ARG(5), 0);
    RET(S_OK, 6);
}

/* CLSIDFromString(L"{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}", pclsid) */
static void sh_CLSIDFromString(Guest* g)
{
    uint32_t s = ARG(0), out = ARG(1);
    uint8_t v[16];
    int k = 0;
    unsigned nib = 0, acc = 0;
    if (!s || rd16(s) != '{')
        RET(0x800401F3u, 2); /* CO_E_CLASSSTRING */
    for (uint32_t p = s + 2; k < 16; p += 2)
    {
        uint16_t c = rd16(p);
        if (c == '-')
            continue;
        unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (d == 16)
            RET(0x800401F3u, 2);
        acc = acc << 4 | d;
        if (++nib == 2)
            v[k++] = (uint8_t)acc, nib = 0, acc = 0;
    }
    /* Data1..Data3 are little-endian in memory */
    uint8_t guid[16] = { v[3], v[2], v[1], v[0], v[5], v[4], v[7], v[6] };
    memcpy(guid + 8, v + 8, 8);
    memcpy(GUEST_PTR(out), guid, 16);
    RET(S_OK, 2);
}

static const ShimDef OLE[] = {
    { "ole32.dll", "CoInitialize", sh_CoInitialize },
    { "ole32.dll", "CoInitializeEx", sh_CoInitializeEx },
    { "ole32.dll", "CoUninitialize", sh_CoUninitialize },
    { "ole32.dll", "CoFreeUnusedLibraries", sh_CoFreeUnusedLibraries },
    { "ole32.dll", "CoTaskMemAlloc", sh_CoTaskMemAlloc },
    { "ole32.dll", "CoTaskMemFree", sh_CoTaskMemFree },
    { "ole32.dll", "CoTaskMemRealloc", sh_CoTaskMemRealloc },
    { "ole32.dll", "CoCreateInstance", sh_CoCreateInstance },
    { "oleaut32.dll", "SysAllocString", sh_SysAllocString },
    { "oleaut32.dll", "SysAllocStringLen", sh_SysAllocStringLen },
    { "oleaut32.dll", "SysFreeString", sh_SysFreeString },
    { "oleaut32.dll", "SysStringLen", sh_SysStringLen },
    { "oleaut32.dll", "LoadTypeLib", sh_LoadTypeLib },
    { "oleaut32.dll", "RegisterTypeLib", sh_RegisterTypeLib },
    { "oleaut32.dll", "VarUI4FromStr", sh_VarUI4FromStr },
    /* FFXiMain imports OLEAUT32 by ordinal */
    { "oleaut32.dll", "#2", sh_SysAllocString },
    { "oleaut32.dll", "#4", sh_SysAllocStringLen },
    { "oleaut32.dll", "#6", sh_SysFreeString },
    { "oleaut32.dll", "#7", sh_SysStringLen },
    { "oleaut32.dll", "#161", sh_LoadTypeLib },
    { "oleaut32.dll", "#163", sh_RegisterTypeLib },
    { "oleaut32.dll", "#277", sh_VarUI4FromStr },
    { "ole32.dll", "CoSetProxyBlanket", sh_CoSetProxyBlanket },
    { "ole32.dll", "CLSIDFromString", sh_CLSIDFromString },
    { "user32.dll", "CharNextA", sh_CharNextA },
    { "wbemprox.dll", "IWbemLocator::AddRef", WMI_AddRef },
    { "wbemprox.dll", "IWbemLocator::Release", WMI_Release },
    { "wbemprox.dll", "IWbemLocator::ConnectServer", WMI_ConnectServer },
    { "wbemprox.dll", "IWbemServices::AddRef", WMI_AddRef },
    { "wbemprox.dll", "IWbemServices::Release", WMI_Release },
    { "wbemprox.dll", "IWbemServices::CreateInstanceEnum", WMI_CreateInstanceEnum },
    { "wbemprox.dll", "IEnumWbemClassObject::AddRef", WMI_AddRef },
    { "wbemprox.dll", "IEnumWbemClassObject::Release", WMI_Release },
    { "wbemprox.dll", "IEnumWbemClassObject::Next", WMI_Next },
    { "wbemprox.dll", "IWbemClassObject::AddRef", WMI_AddRef },
    { "wbemprox.dll", "IWbemClassObject::Release", WMI_Release },
    { "wbemprox.dll", "IWbemClassObject::Get", WMI_Get },
    { NULL, NULL, NULL },
};

void ole_init(void)
{
    thunk_register(OLE);
}
