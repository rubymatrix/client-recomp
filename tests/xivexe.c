/* Host for FINAL FANTASY XIV 1.23b's launcher programs (ffxivboot.exe, ffxivlogin.exe,
 * ffxivupdater.exe) on the portable runtime (arm64 macOS). One binary per program, each built
 * from that program's translation (tools/build_xiv.py exe --name <program>).
 *
 * Same start as tests/xivboot.c without the game's hooks: map the EXE at its fixed base, bind its
 * imports, give the main thread its TLS and a PEB, run the TLS callbacks, enter the CRT.
 *
 * usage: xivexe <program.exe> [args passed as the guest's command line]
 *   The exe's folder is mounted at C:\Program Files\SquareEnix\FINAL FANTASY XIV, the path the
 *   game sees, so the programs find each other and boot.ver/game.ver beside them.
 */
#include <stdio.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"
#include "vfs.h"

#include <SDL3/SDL.h>

#define GAME_DIR_GUEST "C:\\Program Files\\SquareEnix\\FINAL FANTASY XIV"

void xiv_misc_init(void);
void xiv_set_documents(const char* host_dir);
void sampler_start(void);
void user32_main_tick(void);
void user32_init(void);
void d3d9_init(void);
void d3dx9_init(void);
void fiber_init(void);
void dinput_setup(void);
void dsound_setup(void);
void ws2_init(void);
void k32_launch_init(void); /* k32_launch.c */
void ui_appkit_init(void);  /* ui_appkit.m: after user32_init, whose shims it hands other windows to */
void dinput_init(void);
void dsound_init(void);

/* the game's hooks the shared shims call into: nothing to do outside the game */
void xiv_ui_report(void) {}
void xiv_probe_frame(void) {}
void xiv_discord_frame(void) {}
void xiv_probe_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n) {}
void xiv_discord_net(int out, uint16_t op, uint32_t src, const uint8_t* p, uint32_t n) {}
int xiv_discord_on(void) { return 0; }

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

static void main_thread(void* arg)
{
    uint32_t code = guest_call(rt_image_oep, 0, NULL);
    printf("entry point returned %u\n", code);
    fflush(stdout);
    *(volatile int*)arg = 1;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: xivexe <program.exe> [command line]\n");
        return 2;
    }
    const char* base = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    vfs_mount(GAME_DIR_GUEST, dir_of(argv[1]));
    {
        /* "My Documents", shared with the game: My Games\FINAL FANTASY XIV holds the language,
         * region, login cache and patch downloads */
        char docs[1024];
        const char* home = getenv("HOME");
        snprintf(docs, sizeof docs, "%s/Library/Application Support/FFXIV1_0", home ? home : ".");
        plat_mkdir(docs);
        strncat(docs, "/Documents", sizeof docs - strlen(docs) - 1);
        plat_mkdir(docs);
        xiv_set_documents(docs);
    }
    if (getenv("XIV_SERIAL") && strcmp(getenv("XIV_SERIAL"), "0"))
        gt_parallel = 0;

    gt_init();
    char exe_guest[512];
    snprintf(exe_guest, sizeof exe_guest, GAME_DIR_GUEST "\\%s", base);
    k32_set_image_name(base);
    k32_init(GAME_DIR_GUEST, exe_guest);
    {
        /* the rest of argv is the guest's command line, joined back with spaces */
        static char cmd[8192];
        size_t n = 0;
        for (int i = 2; i < argc && n < sizeof cmd - 2; i++)
            n += (size_t)snprintf(cmd + n, sizeof cmd - n, "%s%s", i > 2 ? " " : "", argv[i]);
        if (argc > 2)
            k32_set_command_line(cmd);
    }
    k32_io_init();
    k32_misc_init();
    k32_xiv_init();
    xiv_misc_init();
    ole_init();
    user32_init();
    d3d9_init();
    d3dx9_init();
    fiber_init();
    dsound_init();
    dinput_init();
    ws2_init();
    k32_launch_init();
    ui_appkit_init();
    vfs_init(GAME_DIR_GUEST);
    reg_init(NULL, 0, NULL);
    rt_set_native_handler(thunk_dispatch);
    if (!pe_load(argv[1]))
        return 1;

    gt_lock();
    uint32_t peb = gheap_alloc(0x1000, 1);
    wr32(peb + 0x08, rt_image_base); /* ImageBaseAddress */
    wr32(peb + 0xA4, 5);             /* OSMajorVersion: Windows XP */
    wr32(peb + 0xA8, 1);             /* OSMinorVersion */
    wr16(peb + 0xAC, 2600);          /* OSBuildNumber */
    wr32(peb + 0xB0, 2);             /* OSPlatformId: VER_PLATFORM_WIN32_NT */
    wr32(peb + 0x64, 1);             /* NumberOfProcessors */
    gt_set_peb(peb);
    uint32_t tls_start = 0, tls_size = 0, tls_zero = 0;
    uint32_t callbacks = pe_static_tls(rt_image_base, &tls_start, &tls_size, &tls_zero);
    gt_set_static_tls(tls_start, tls_size, tls_zero);
    dsound_setup();
    dinput_setup();
    gt_unlock();

    if (getenv("XIV_MISSING"))
        printf("imports with no shim: %u\n", thunk_report_missing());

    for (uint32_t cb = callbacks; cb && rd32(cb); cb += 4)
    {
        uint32_t args[3] = { rt_image_base, 1 /* DLL_PROCESS_ATTACH */, 0 };
        guest_call(rd32(cb), 3, args);
    }

    printf("entering %s at %08x\n", base, rt_image_oep);
    fflush(stdout);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        printf("SDL_Init: %s\n", SDL_GetError());
    static volatile int done;
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    plat_thread_start(main_thread, (void*)&done);
    while (!done)
    {
        SDL_PumpEvents();
        user32_main_tick();
        SDL_Delay(1);
    }
    return 0;
}
