/* Boot test for FINAL FANTASY XIV 1.23b ffxivgame.exe on the portable runtime (arm64 macOS).
 *
 * Maps the EXE at its fixed base 0x00400000 in the guest window, binds its imports to thunks,
 * gives the main thread its static TLS and a PEB, runs the TLS callbacks, then enters the image
 * at its entry point (the MSVC 2005 CRT's WinMainCRTStartup) and runs until the first import
 * that has no shim yet - or a fault - which rt_fatal reports with the guest address. Each run
 * names the next shim to write.
 *
 * usage: xivboot <ffxivgame.exe> [game folder] [registry .reg file]
 *   The game folder (default: the exe's folder) is mounted at the path the game sees,
 *   C:\Program Files\SquareEnix\FINAL FANTASY XIV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"
#include "vfs.h"

#define GAME_DIR_GUEST "C:\\Program Files\\SquareEnix\\FINAL FANTASY XIV"

void xiv_misc_init(void);
void xiv_set_documents(const char* host_dir);
void user32_init(void);
void ws2_init(void);
void dinput_init(void);
void dsound_init(void);

static char* dir_of(const char* path)
{
    char* d = strdup(path);
    char* slash = strrchr(d, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(d, ".");
    return d;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: xivboot <ffxivgame.exe> [game folder] [registry .reg]\n");
        return 2;
    }
    const char* game_host = argc > 2 ? argv[2] : dir_of(argv[1]);
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    printf("guest window at %p\n", (void*)rt_guest_base);
    vfs_mount(GAME_DIR_GUEST, game_host);
    {
        /* "My Documents": the game's settings, logs and screenshots */
        char docs[1024];
        const char* home = getenv("HOME");
        snprintf(docs, sizeof docs, "%s/Library/Application Support/FFXIV1_0", home ? home : ".");
        plat_mkdir(docs);
        strncat(docs, "/Documents", sizeof docs - strlen(docs) - 1);
        plat_mkdir(docs);
        xiv_set_documents(docs);
    }
    gt_init();
    k32_set_image_name("ffxivgame.exe");
    k32_init(GAME_DIR_GUEST, GAME_DIR_GUEST "\\ffxivgame.exe");
    /* XIV_ARGS: the launch argument (tools/xiv_args.py), keyed to XIV_TICK (default 0x10000) */
    if (getenv("XIV_ARGS"))
    {
        k32_set_command_line(getenv("XIV_ARGS"));
        k32_set_tick_base(getenv("XIV_TICK") ? (uint32_t)strtoul(getenv("XIV_TICK"), NULL, 0) : 0x10000u);
    }
    k32_io_init();
    k32_misc_init();
    k32_xiv_init();
    xiv_misc_init();
    ole_init();
    user32_init();
    dsound_init();
    dinput_init();
    ws2_init();
    vfs_init(GAME_DIR_GUEST);
    if (argc > 3)
    {
        const char* files[1] = { argv[3] };
        reg_init(files, 1, NULL);
    }
    else
        reg_init(NULL, 0, NULL);
    rt_set_native_handler(thunk_dispatch);
    if (!pe_load(argv[1]))
        return 1;

    /* The PEB: what MSVC 2005's CRT and the game may read through fs:[0x30]. */
    gt_lock();
    uint32_t peb = gheap_alloc(0x1000, 1);
    wr32(peb + 0x08, rt_image_base); /* ImageBaseAddress */
    wr32(peb + 0xA4, 5);             /* OSMajorVersion: Windows XP, the game's era */
    wr32(peb + 0xA8, 1);             /* OSMinorVersion */
    wr16(peb + 0xAC, 2600);          /* OSBuildNumber */
    wr32(peb + 0xB0, 2);             /* OSPlatformId: VER_PLATFORM_WIN32_NT */
    wr32(peb + 0x64, 1);             /* NumberOfProcessors */
    gt_set_peb(peb);
    uint32_t tls_start = 0, tls_size = 0, tls_zero = 0;
    uint32_t callbacks = pe_static_tls(rt_image_base, &tls_start, &tls_size, &tls_zero);
    gt_set_static_tls(tls_start, tls_size, tls_zero);
    gt_unlock();
    printf("static TLS: %08x + %u (+%u zero), callbacks at %08x\n", tls_start, tls_size, tls_zero, callbacks);

    if (getenv("XIV_MISSING"))
        printf("imports with no shim: %u\n", thunk_report_missing());

    for (uint32_t cb = callbacks; cb && rd32(cb); cb += 4)
    {
        uint32_t args[3] = { rt_image_base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        printf("TLS callback %08x\n", rd32(cb));
        guest_call(rd32(cb), 3, args);
    }

    printf("entering ffxivgame.exe at %08x\n", rt_image_oep);
    fflush(stdout);
    uint32_t code = guest_call(rt_image_oep, 0, NULL);
    printf("entry point returned %u\n", code);
    return 0;
}
