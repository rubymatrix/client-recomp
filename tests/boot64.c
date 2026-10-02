/* Boot test for the R3.1 portable runtime on a 64-bit host (Windows x64 now, arm64 macOS later).
 *
 * The same steps as tests/boot.c - the game's CRT DllMain(PROCESS_ATTACH), then
 * DllGetClassObject(GameMain), CreateInstance and Release - but with no x86 anywhere: the retail
 * image is mapped into the 4 GB guest window by our own loader, every import is a thunk to a
 * shim, and the host reaches guest code only through guest_call.
 *
 * usage: boot64.exe <retail FFXiMain.dll> [game folder]
 */
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "gamecore.h"
#include "gamecore_config.h"
#include "thunk.h"
#include "vfs.h"

static const uint8_t CLSID_GameMain[16] = { 0x46, 0xDC, 0x27, 0x10, 0x0D, 0x75, 0x1F, 0x4B, 0x88, 0x34, 0x1D, 0x25, 0xB8, 0xBE, 0xBA, 0xB8 };
static const uint8_t IID_IClassFactory[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
static const uint8_t IID_IUnknown[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };

/* input.c (SDL) is not linked here; ole.c's WMI gamepad answer asks it for the pad count. */
int input_pad_count(void) { return 0; }

static uint32_t guest_bytes(const void* p, uint32_t n)
{
    uint32_t a = gheap_alloc(n, 1);
    memcpy(GUEST_PTR(a), p, n);
    return a;
}

/* A COM method of a guest object: vtable slot `slot`, `this` first. */
static uint32_t com_call(uint32_t obj, unsigned slot, unsigned nargs, const uint32_t* args)
{
    uint32_t all[8] = { obj };
    for (unsigned i = 0; i < nargs && i < 7; ++i)
        all[i + 1] = args[i];
    return guest_call(rd32(rd32(obj) + 4u * slot), nargs + 1, all);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: boot64.exe <retail FFXiMain.dll> [game folder]\n");
        return 2;
    }
    const char* game_dir = argc > 2 ? argv[2] : "C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\FINAL FANTASY XI";
    if (plat_path_sep != '\\' && argc > 2)
    {
        /* not Windows: the game sees the install at a Windows path (as host64 mounts it) */
        vfs_mount("C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI", argv[2]);
        game_dir = "C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI";
    }
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    printf("guest window at %p\n", (void*)rt_guest_base);
    gt_init();
    k32_init(game_dir, "C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\PlayOnlineViewer\\pol.exe");
    k32_io_init();
    k32_misc_init();
    ole_init();
    vfs_init(game_dir);
    if (argc > 3)
    {
        /* a `reg export` of the game's registry keys: FFXI's settings must read back */
        const char* files[1] = { argv[3] };
        reg_init(files, 1, NULL);
        uint32_t v = 0;
        int found = reg_get_dword("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI", "0001", &v);
        printf("registry: FinalFantasyXI\\0001 %s %u\n", found ? "=" : "missing", v);
        if (!found)
            return 1;
    }
    else
        reg_init(NULL, 0, NULL);
    rt_set_native_handler(thunk_dispatch);
    if (!pe_load(argv[1]))
        return 1;

    uint32_t dllmain[3] = { rt_image_base, 1 /* DLL_PROCESS_ATTACH */, 0 };
    uint32_t ok = guest_call(rt_image_oep, 3, dllmain);
    printf("DllMain(PROCESS_ATTACH) -> %u\n", ok);
    if (!ok)
        return 1;

    gt_lock();
    uint32_t clsid = guest_bytes(CLSID_GameMain, 16), iid_cf = guest_bytes(IID_IClassFactory, 16),
             iid_unk = guest_bytes(IID_IUnknown, 16), out = gheap_alloc(4, 1);
    gt_unlock();

    uint32_t gco[3] = { clsid, iid_cf, out };
    uint32_t hr = guest_call(pe_export("DllGetClassObject"), 3, gco);
    uint32_t cf = rd32(out);
    printf("DllGetClassObject(GameMain): %08x %08x\n", hr, cf);
    if (hr || !cf)
        return 1;

    uint32_t ci[3] = { 0, iid_unk, out };
    hr = com_call(cf, 3, 3, ci); /* IClassFactory::CreateInstance */
    uint32_t obj = rd32(out);
    printf("CreateInstance(IUnknown): %08x %08x\n", hr, obj);
    if (!hr && obj)
        printf("Release -> %u\n", com_call(obj, 2, 0, NULL));
    printf("factory Release -> %u\n", com_call(cf, 2, 0, NULL));

    /* our own gamecore: the object answers the US ICoreCom and hands out the table */
    gamecore_slots_init();
    gamecore_init();
    static const uint8_t IID_ICoreCom_US[16] = { 0x54, 0x66, 0x51, 0xE0, 0x77, 0xEF, 0x5D, 0x43,
                                                    0xAA, 0x7D, 0x50, 0xD2, 0xC0, 0x69, 0xCE, 0x34 };
    gt_lock();
    uint32_t iid_core = guest_bytes(IID_ICoreCom_US, 16);
    gt_unlock();
    uint32_t qi[2] = { iid_core, out };
    uint32_t hr_core = com_call(gamecore_object(), 0, 2, qi);
    uint32_t core = rd32(out);
    uint32_t gcft[1] = { out };
    com_call(core, 7, 1, gcft); /* GetCommonFunctionTable */
    printf("gamecore QI(US): %08x %08x, table %08x (expected %08x)\n", hr_core, core, rd32(out), gamecore_table());
    if (hr_core || rd32(out) != gamecore_table())
        hr = 1;

    /* slots through the table, as FFXiMain's stubs call them (cdecl): the UDP port, and the lobby
     * resolve, which must answer 127.0.0.1 in host byte order at +4 */
    uint32_t table = gamecore_table(), zero[1] = { 0 }, poll[2] = { 0, 0 };
    uint32_t port = guest_call(rd32(table + 0x10e0), 1, zero);
    gt_lock();
    poll[1] = gheap_alloc(0x14, 1);
    gt_unlock();
    uint32_t done = guest_call(rd32(table + 0x378), 2, poll);
    printf("gamecore slots: udp port %u, lobby resolve %u -> %08x\n", port, done, rd32(poll[1] + 4));
    if (port != 54090 || done != 1 || rd32(poll[1] + 4) != 0x7F000001u)
        hr = 1;
    printf("BOOT64 %s\n", hr ? "FAILED" : "OK");
    return hr ? 1 : 0;
}
