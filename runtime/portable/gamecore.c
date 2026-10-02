/* Our own gamecore: the ICoreCom object and the common function table. See gamecore.h. */
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "gamecore.h"

#define E_NOINTERFACE 0x80004002u
#define S_OK 0u
#define EXE_HANDLE 0x00400000u /* hInstance of the host process, as the guest sees it (k32.c) */

/* IID_ICoreCom, US: {E0516654-EF77-435D-AA7D-50D2C069CE34}. FFXi.dll also tries JP
 * {9A30D565-...} before it and EU {DFEC2E93-...} after it; neither is answered. */
static const uint8_t IID_ICoreCom_US[16] = { 0x54, 0x66, 0x51, 0xE0, 0x77, 0xEF, 0x5D, 0x43,
                                                0xAA, 0x7D, 0x50, 0xD2, 0xC0, 0x69, 0xCE, 0x34 };
static const uint8_t IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static uint32_t g_object, g_table, g_cmdline;
static uint32_t g_refs = 1;

/* --- ICoreCom ------------------------------------------------------------------------------
 * The methods FFXi.dll calls (FFXI.gamecore-surface.txt §1): QueryInterface, Release,
 * GethInstance, GetlpCmdLine, GetCommonFunctionTable, ViewerExec. All stdcall with `this`. */
static void m_QueryInterface(Guest* g)
{
    const uint8_t* iid = (const uint8_t*)ARGP(1);
    uint32_t out = ARG(2);
    if (!memcmp(iid, IID_ICoreCom_US, 16) || !memcmp(iid, IID_IUnknown, 16))
    {
        g_refs++;
        wr32(out, g_object);
        RET(S_OK, 3);
    }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

static void m_AddRef(Guest* g) { RET(++g_refs, 1); }
static void m_Release(Guest* g) { RET(g_refs > 1 ? --g_refs : 1, 1); } /* the object lives as long as the process */
static void m_GethInstance(Guest* g) { wr32(ARG(1), EXE_HANDLE); RET(S_OK, 2); }
static char g_cmdline_text[256];

void gamecore_set_cmdline(const char* text)
{
    snprintf(g_cmdline_text, sizeof g_cmdline_text, "%s", text);
}

static void m_GetlpCmdLine(Guest* g) { wr32(ARG(1), g_cmdline); RET(S_OK, 2); }
static void m_GetCommonFunctionTable(Guest* g) { wr32(ARG(1), g_table); RET(S_OK, 2); }
static void m_ViewerExec(Guest* g) { RET(S_OK, 2); }

/* vtable slots 0..35 (client-gamecore-interface.md §2-3; 32-35 are ATL's IDispatch) */
static const char* const METHODS[36] = {
    "QueryInterface", "AddRef", "Release", "GethInstance", "GetlpCmdLine", "SetParamInit", "GetWindowsType",
    "GetCommonFunctionTable", "ViewerExec", "GetWindowsVersion", "PressAnyKey",
    "ConSetEnableWakeupFuncFlag", "CreateInput", "UpdateInputState", "GetPadRepeat", "GetPadOn",
    "FinalCleanup", "SetParamInitW", "GetlpCmdLineW", "PaintFriendList", "CreateFriendList",
    "DestroyFriendList", "SetMaskWindowHandle", "GetRegKeyNameW", "GetRegKeyNameA",
    "GetSquareEnixRegKeyNameW", "GetSquareEnixRegKeyNameA", "SetAreaCode", "GetAreaCode", "HideMaskWindow",
    "ShowMaskWindow", "IsVisibleMaskWindow", "GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke",
};

static const ShimDef OBJECT[] = {
    { "gamecore", "ICoreCom::QueryInterface", m_QueryInterface },
    { "gamecore", "ICoreCom::AddRef", m_AddRef },
    { "gamecore", "ICoreCom::Release", m_Release },
    { "gamecore", "ICoreCom::GethInstance", m_GethInstance },
    { "gamecore", "ICoreCom::GetlpCmdLine", m_GetlpCmdLine },
    { "gamecore", "ICoreCom::GetCommonFunctionTable", m_GetCommonFunctionTable },
    { "gamecore", "ICoreCom::ViewerExec", m_ViewerExec },
    { NULL, NULL, NULL },
};

/* --- the common function table ----------------------------------------------------------------- */
static ShimDef g_slot_defs[GAMECORE_SLOTS + 1];
static char g_slot_names[GAMECORE_SLOTS][12];
static unsigned g_nslot_defs;
static int g_registered;

void gamecore_register(const GamecoreSlot* slots)
{
    for (const GamecoreSlot* s = slots; s->fn; ++s)
    {
        if (s->disp / 4 >= GAMECORE_SLOTS || g_nslot_defs >= GAMECORE_SLOTS)
            continue;
        char* name = g_slot_names[g_nslot_defs];
        snprintf(name, sizeof g_slot_names[0], "+0x%x", s->disp);
        g_slot_defs[g_nslot_defs].dll = "gamecore";
        g_slot_defs[g_nslot_defs].name = name;
        g_slot_defs[g_nslot_defs].fn = s->fn;
        g_nslot_defs++;
    }
}

void gamecore_init(void)
{
    if (!g_registered)
    {
        g_registered = 1;
        thunk_register(OBJECT);
        thunk_register(g_slot_defs); /* filled by gamecore_register calls made before this */
    }
    int took = !gt_holds();
    if (took)
        gt_lock();
    g_cmdline = gheap_strdup(g_cmdline_text);
    /* the object: a vtable pointer and nothing else the guest reads */
    uint32_t vtbl = gheap_alloc(4 * 36, 1);
    for (unsigned i = 0; i < 36; ++i)
    {
        char name[64];
        snprintf(name, sizeof name, "ICoreCom::%s", METHODS[i]);
        wr32(vtbl + 4 * i, thunk_for("gamecore", name));
    }
    g_object = gheap_alloc(16, 1);
    wr32(g_object, vtbl);
    /* the table: every slot a thunk, implemented or trapping */
    g_table = gheap_alloc(4 * GAMECORE_SLOTS, 1);
    for (unsigned i = 0; i < GAMECORE_SLOTS; ++i)
    {
        char name[16];
        snprintf(name, sizeof name, "+0x%x", 4 * i);
        wr32(g_table + 4 * i, thunk_for("gamecore", name));
    }
    if (took)
        gt_unlock();
    rt_log("[recomp] gamecore: object %08x, function table %08x, %u slots implemented\n", g_object, g_table, g_nslot_defs);
}

uint32_t gamecore_object(void) { return g_object; }
uint32_t gamecore_table(void) { return g_table; }
