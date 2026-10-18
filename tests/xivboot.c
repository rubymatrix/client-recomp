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

static void write_config_sys(const char* path)
{
    static const uint32_t fields[24] = {
        0x20120419, /* magic */
        0, 0xD800, 0,
        0,          /* display mode: windowed */
        2112, 1188, /* window size */
        7, 4, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 1,
    };
    uint8_t buf[0x2AC] = { 0 };
    for (int i = 0; i < 24; i++)
        for (int b = 0; b < 4; b++)
            buf[4 * i + b] = (uint8_t)(fields[i] >> (8 * b));
    const char* font = "MS UI Gothic"; /* wchar[32] at 0x60 */
    for (int i = 0; font[i]; i++)
        buf[0x60 + 2 * i] = (uint8_t)font[i];
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
        printf("config.sys: wrote the defaults (2112x1188 window) to %s\n", path);
    }
}

/* With no config.sys (what ffxivconfig.exe writes) the game opens a 1280x720 window. The first
 * start here writes one instead: the game's own defaults (FUN_00405c90, decomp/docs/config_sys.md)
 * but a 2112x1188 window, 1920x1080 and a tenth. Edit it with tools/xiv_config.py. */
static void write_default_config(const char* docs)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/My Games", docs);
    plat_mkdir(path);
    strncat(path, "/FINAL FANTASY XIV", sizeof path - strlen(path) - 1);
    plat_mkdir(path);
    strncat(path, "/config.sys", sizeof path - strlen(path) - 1);
    FILE* f = fopen(path, "rb");
    if (f)
        fclose(f);
    else
        write_config_sys(path);

    /* config.pad: the gamepad ffxivconfig.exe picked. With none the game opens no gamepad at all
     * (FUN_00d33630 matches each DirectInput device's instance and product GUID against it), so
     * pick the first one as dinput.c reports it: instance "SIGN"+0, an Xbox 360 controller's
     * product GUID. The rest is what the game fills in without the file (FUN_00d34410). */
    snprintf(path + strlen(path) - strlen("config.sys"), sizeof path - (strlen(path) - strlen("config.sys")), "config.pad");
    f = fopen(path, "rb");
    if (f)
    {
        fclose(f);
        return;
    }
    static const uint8_t pad_guids[32] = {
        0x53, 0x49, 0x47, 0x4E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,               /* guidInstance */
        0x5E, 0x04, 0x8E, 0x02, 0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V', 'I', 'D', /* guidProduct */
    };
    uint8_t pad[0x148] = { 0 };
    pad[0] = 0x11, pad[1] = 0x02, pad[2] = 0x10, pad[3] = 0x20; /* magic 0x20100211 */
    memcpy(pad + 4, pad_guids, sizeof pad_guids);
    pad[0x28] = 2;                       /* stick dead zone (scaled into +0x1d0, FUN_00d34410) */
    /* which input drives each of the game's 24 pad functions (FUN_00d32a30; -1 none, the game's
     * default): ffxivconfig.exe's "XInput Compliant Gamepad" preset (.data 0x457408). The inputs
     * are the game's XInput order (FUN_00d337a0): 0 Start 1 Back 2 LS 3 RS 4 LB 5 RB 6 A 7 B 8 X
     * 9 Y; then a -/+ pair per axis from 32: D-pad X, D-pad Y, left X, left Y, right X, right Y,
     * LT (45), RT (47). */
    static const uint8_t xinput_preset[24] = {
        0x22, 0x23, 0x20, 0x21,             /* menu navigation: D-pad up, down, left, right */
        0x26, 0x27, 0x24, 0x25,             /* movement: left stick */
        0x2A, 0x2B, 0x28, 0x29,             /* camera: right stick */
        0x09, 0x07, 0x06, 0x08, 0x04, 0x2D, /* Y B A X LB LT */
        0x02, 0x05, 0x2F, 0x03, 0x00, 0x01, /* LS RB RT RS Start Back */
    };
    memcpy(pad + 0x2C, xinput_preset, sizeof xinput_preset);
    f = fopen(path, "wb");
    if (f)
    {
        fwrite(pad, 1, sizeof pad, f);
        fclose(f);
        printf("config.pad: wrote the first gamepad to %s\n", path);
    }
}

#define GAME_DIR_GUEST "C:\\Program Files\\SquareEnix\\FINAL FANTASY XIV"

void xiv_misc_init(void);
void xiv_ui_init(void);
void xiv_native_init(void); /* xiv_native.c */
void xiv_direct_login_init(void); /* xiv_direct_login.c */
void xiv_groundsnap_init(void);   /* xiv_groundsnap.c */
void xiv_probe_init(void);
void sampler_start(void);
void user32_main_tick(void);
void xiv_set_documents(const char* host_dir);
void user32_init(void);
void d3d9_init(void);
void d3dx9_init(void);
void xaudio2_init(void);
void fiber_init(void);
void dinput_setup(void);
void dsound_setup(void);
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

static void game_thread(void* arg)
{
    uint32_t code = guest_call(rt_image_oep, 0, NULL);
    printf("entry point returned %u\n", code);
    *(volatile int*)arg = 1;
}

extern void (*g9_pace_hook)(uint32_t fps); /* gfx9.h */
uint64_t g9_pace_refresh_ns(void);
static void xiv_pace_fps(uint32_t fps) { wr32(0x137DF00, fps); }

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
        write_default_config(docs);
    }
    /* XIV_SERIAL=1: guest threads take turns under one lock instead of running in parallel */
    if (getenv("XIV_SERIAL") && strcmp(getenv("XIV_SERIAL"), "0"))
        gt_parallel = 0;
    if (getenv("XIV_FLAGCUT"))
    {
        extern uint32_t rt_flag_lo, rt_flag_hi;
        sscanf(getenv("XIV_FLAGCUT"), "%x:%x", &rt_flag_lo, &rt_flag_hi);
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
    xiv_ui_init();
    xiv_native_init();
    xiv_direct_login_init();
    xiv_groundsnap_init();
    xiv_probe_init();
    ole_init();
    user32_init();
    d3d9_init();
    d3dx9_init();
    xaudio2_init();
    fiber_init();
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

    /* The lobby server's host name, in the slot Garlemald Client patches (VA 0x00F90110, 0x14 bytes):
     * XIV_LOBBY, default 127.0.0.1. */
    {
        const char* lobby = getenv("XIV_LOBBY") ? getenv("XIV_LOBBY") : "127.0.0.1";
        size_t n = strlen(lobby);
        if (n + 1 > 0x14)
        {
            printf("XIV_LOBBY is longer than the game's 19-character slot\n");
            return 2;
        }
        memcpy(GUEST_PTR(0x00F90110u), lobby, n + 1);
        printf("lobby server: %s\n", lobby);
    }

    /* The frame rate target, 60 (XIV_FPSCAP=<fps>; 0 uncaps): the render loop's limiter
     * (frame_rate_target in the apply overlay) and the logic loop (worker_tick_period, which also
     * advances the interface's animations) both run at it. */
    {
        const char* cap = getenv("XIV_FPSCAP");
        int fps = cap ? atoi(cap) : 60;
        wr32(0x137DF00, fps > 0 ? (uint32_t)fps : 100000u); /* uncapped: a 10 us period, no Sleep */
        /* without a cap of its own, the limiter follows the rate presents are paced to: a whole
         * number of the display's refreshes per frame */
        if (!cap)
            g9_pace_hook = xiv_pace_fps;
        /* Framework::Idle's frame delta (its QueryPerformanceCounter read) in whole refreshes: the
         * player, camera and animations move by it */
        if (!getenv("XIV_RAWDELTA"))
            k32_quantize_qpc_at(0xA4FE24, g9_pace_refresh_ns);
        /* a frame that ran over its period: the limiter's Sleep(1) becomes a yield */
        k32_skip_sleep_at(0x4E8F72);
        const char* hz = getenv("XIV_TICKHZ");
        int tick = hz && atoi(hz) > 0 ? atoi(hz) : (fps > 0 ? fps : 120);
        wr32(0x137DF04, 1000000u / (uint32_t)(tick > 1000 ? 1000 : tick));
        /* and its sleeps paced exactly: the interface's animations advance on this loop */
        k32_pace_sleep_at(0xCC6D4C, 1000000000ull / (uint32_t)(tick > 1000 ? 1000 : tick));
        k32_pace_sleep_at(0xCC6D62, 1000000000ull / (uint32_t)(tick > 1000 ? 1000 : tick));
    }

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
    /* the COM vtables of the shared shims are guest memory: built now the guest heap is up */
    dsound_setup();
    dinput_setup();
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
    /* The game runs on a thread of its own: the main thread is SDL's (macOS lets only it touch
     * windows), pumping events and running the window calls the game's threads hand it
     * (runtime/portable/sdl_main.h). */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        printf("SDL_Init: %s\n", SDL_GetError());
    static volatile int done;
    sampler_start();
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); /* the main thread: SDL, the window */
#endif
    plat_thread_start(game_thread, (void*)&done);
    while (!done)
    {
        SDL_PumpEvents();
        user32_main_tick();
        SDL_Delay(1);
    }
    return 0;
}
