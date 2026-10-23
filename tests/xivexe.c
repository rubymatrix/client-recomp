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
#include <mach-o/dyld.h>
#include <mach/machine.h>
#include <spawn.h>
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
void webbrowser_init(void); /* webbrowser.m: after ole_init and ui_appkit_init */
void ui_suppress_controls(const uint32_t* ids, int n);
void ui_yield_activation(const char* bundle_id);
void webbrowser_setup(void);
void winhttp_init(void);    /* winhttp.c: over xiv_misc.c's stubs */
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

/* --- starting the other programs (ShellExecuteExW, CreateProcessW) ---------------------------------
 * ffxivboot starts ffxivlogin and ffxivupdater, ffxivupdater restarts its RebootModule, ffxivlogin
 * starts ffxivgame.exe. Each is the native build beside this one (build/bin/<name>); the game is
 * xivboot, given its argument through XIV_ARGS and the tick count now, so it decrypts the argument
 * in the same tick window it was made in (GetTickCount is the system's here, in every program). */
extern int (*k32_launch_hook)(const char* file, const char* params, const char* dir);
extern char** environ;

static char g_bin_dir[1024];
static char g_install[1024]; /* the host folder mounted as the install (XIV_INSTALL for the programs started) */

static int exists(const char* p) { FILE* f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

static int launch(const char* file, const char* params, const char* dir)
{
    char host[1024];
    if (!vfs_host_path(file, host, sizeof host) || !exists(host))
    {
        printf("launch: %s is not in the install\n", file);
        return 0;
    }
    /* the guest's path may mix separators (ffxivboot: "...\\downloads/ffxivupdater.exe") */
    const char* base = file;
    for (const char* c = file; *c; ++c)
        if (*c == '\\' || *c == '/')
            base = c + 1;
    char guest[1024];
    if (!vfs_full_path(file, guest, sizeof guest))
        snprintf(guest, sizeof guest, "%s", file);
    char name[64], bin[1200];
    snprintf(name, sizeof name, "%s", base);
    char* dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    for (char* c = name; *c; ++c)
        *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    char tick[16], argsenv[4096], tickenv[32], installenv[1100], guestenv[1100];
    char* argv[6] = { NULL };
    /* the environment, with the install it runs in and its own guest path (a program the
     * updater runs may live in Documents\\My Games\\...\\downloads, not the install) */
    char* env[512];
    int ne = 0;
    for (char** e = environ; *e && ne < 505; ++e)
        if (strncmp(*e, "XIV_ARGS=", 9) && strncmp(*e, "XIV_TICK=", 9) && strncmp(*e, "XIV_INSTALL=", 12)
            && strncmp(*e, "XIV_EXE_GUEST=", 14))
            env[ne++] = *e;
    snprintf(installenv, sizeof installenv, "XIV_INSTALL=%s", g_install);
    snprintf(guestenv, sizeof guestenv, "XIV_EXE_GUEST=%s", guest);
    env[ne++] = installenv;
    env[ne++] = guestenv;
    env[ne] = NULL;
    if (!strcmp(name, "ffxivgame"))
    {
        snprintf(bin, sizeof bin, "%s/../FINAL FANTASY XIV.app/Contents/MacOS/xivboot", g_bin_dir);
        if (getenv("XIV_GAME_BIN")) /* another game binary (or a stand-in, for tests) */
            snprintf(bin, sizeof bin, "%s", getenv("XIV_GAME_BIN"));
        else if (!exists(bin))
            snprintf(bin, sizeof bin, "%s/../xivboot", g_bin_dir);
        snprintf(tick, sizeof tick, "0x%x", (uint32_t)(rt_monotonic_ns() / 1000000u));
        snprintf(argsenv, sizeof argsenv, "XIV_ARGS=%s", params);
        snprintf(tickenv, sizeof tickenv, "XIV_TICK=%s", tick);
        env[ne++] = argsenv;
        env[ne++] = tickenv;
        env[ne] = NULL;
        argv[0] = "xivboot", argv[1] = host;
    }
    else
    {
        snprintf(bin, sizeof bin, "%s/%s", g_bin_dir, name);
        argv[0] = name, argv[1] = host, argv[2] = (char*)params;
    }
    /* through tools/xiv_run.py when the sources are beside this build: it rebuilds a program whose
     * exe the updater replaced (XIV_GAME_BIN, a stand-in for tests, is started as it is) */
    char python[1200], runner[1200];
    snprintf(python, sizeof python, "%s/../../.venv/bin/python", g_bin_dir);
    snprintf(runner, sizeof runner, "%s/../../tools/xiv_run.py", g_bin_dir);
    if (!getenv("XIV_GAME_BIN") && exists(python) && exists(runner))
    {
        snprintf(bin, sizeof bin, "%s", python);
        argv[0] = "python3", argv[1] = runner, argv[2] = name, argv[3] = host, argv[4] = (char*)params;
    }
    else if (!exists(bin))
    {
        printf("launch: no native build of %s (%s)\n", base, bin);
        return 0;
    }
    if (!strcmp(name, "ffxivgame"))
        ui_yield_activation("local.ffxiv10.recomp"); /* the app's bundle (tools/build_xiv.py) */
    /* a session of its own: the started program outlives this one (ffxivlogin exits as the game starts) */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    /* the arm64 slice of a universal binary (the runner's Python), whatever this process was
     * started as: its packages are arm64 only */
    cpu_type_t arm64 = CPU_TYPE_ARM64;
    size_t got = 0;
    posix_spawnattr_setbinpref_np(&attr, 1, &arm64, &got);
    pid_t pid;
    int rc = posix_spawn(&pid, bin, NULL, &attr, argv, env);
    posix_spawnattr_destroy(&attr);
    printf("launch: %s -> %s: %s\n", base, bin, rc ? strerror(rc) : "started");
    (void)dir;
    return rc == 0;
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
    static char exe_abs[1024]; /* the install by its absolute path: programs started from here are given it */
    if (realpath(argv[1], exe_abs))
        argv[1] = exe_abs;
    const char* base = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    {
        char self[1024];
        uint32_t n = sizeof self;
        if (!_NSGetExecutablePath(self, &n) && realpath(self, g_bin_dir))
            *strrchr(g_bin_dir, '/') = 0;
        k32_launch_hook = launch;
    }
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    /* the install: where the program that started this one had it, else the exe's own folder */
    snprintf(g_install, sizeof g_install, "%s", getenv("XIV_INSTALL") ? getenv("XIV_INSTALL") : dir_of(argv[1]));
    vfs_mount(GAME_DIR_GUEST, g_install);
    {
        /* "My Documents", shared with the game: My Games\FINAL FANTASY XIV holds the language,
         * region, login cache and patch downloads */
        char docs[1024];
        const char* home = getenv("HOME");
        snprintf(docs, sizeof docs, "%s/Library/Application Support/FFXIV1_0", home ? home : ".");
        plat_mkdir(docs);
        strncat(docs, "/Documents", sizeof docs - strlen(docs) - 1);
        if (getenv("XIV_DOCUMENTS")) /* a folder of its own (tools/xiv_e2e.sh runs in one) */
            snprintf(docs, sizeof docs, "%s", getenv("XIV_DOCUMENTS"));
        plat_mkdir(docs);
        xiv_set_documents(docs);
    }
    if (getenv("XIV_SERIAL") && strcmp(getenv("XIV_SERIAL"), "0"))
        gt_parallel = 0;

    gt_init();
    char exe_guest[512];
    if (getenv("XIV_EXE_GUEST"))
        snprintf(exe_guest, sizeof exe_guest, "%s", getenv("XIV_EXE_GUEST"));
    else
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
    winhttp_init();
    ui_appkit_init();
    if (!strcasecmp(base, "ffxivlogin.exe"))
    {
        /* the login window without its Cancel and Update Page buttons: the page is the whole UI,
         * and the window's close button cancels */
        static const uint32_t ids[] = { 2 /* IDCANCEL */, 1001 /* Update Page */ };
        ui_suppress_controls(ids, 2);
    }
    webbrowser_init();
    vfs_init(GAME_DIR_GUEST);
    reg_init(NULL, 0, NULL);
    rt_set_native_handler(thunk_dispatch);
    if (!pe_load(argv[1]))
        return 1;
    /* XIV_PATCH_CERT=<cert.der>: ffxivboot checks patch torrents' signatures against the X.509
     * certificate in its BINARY resource 111, Square Enix's; the server's own (meteor-web
     * PatchTool's patch_cert.der) goes in its place, the exe's code untouched */
    if (!strcasecmp(base, "ffxivboot.exe") && getenv("XIV_PATCH_CERT"))
    {
        size_t n = 0;
        unsigned char* der = plat_read_file(getenv("XIV_PATCH_CERT"), &n);
        uint32_t type = gheap_strdup("BINARY");
        uint32_t entry = der ? pe_find_resource(rt_image_base, type, 111, 0) : 0;
        if (entry)
        {
            uint32_t data = gheap_alloc((uint32_t)n, 0);
            memcpy(GUEST_PTR(data), der, n);
            wr32(entry, data - rt_image_base); /* IMAGE_RESOURCE_DATA_ENTRY: OffsetToData (an RVA), Size */
            wr32(entry + 4, (uint32_t)n);
            printf("patch certificate: %s (%zu bytes)\n", getenv("XIV_PATCH_CERT"), n);
        }
        else
            printf("patch certificate: cannot use %s\n", getenv("XIV_PATCH_CERT"));
        free(der);
    }

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
    webbrowser_setup();
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
