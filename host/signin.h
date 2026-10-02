/* The sign-in screen host64 shows before the game (signin.c): the game's own UI art, a Settings
 * screen to pick the LandSandBoat server, whether to remember the password, and the window theme. It signs in on a worker thread; what it opened (the window,
 * the graphics back end on it, the sign-in's connections) carries on into the game. */
#pragma once

#include <stdint.h>

typedef struct SigninSetup
{
    const char* host_game; /* the FINAL FANTASY XI folder */
    const char* data_dir;  /* where signin.cfg and settings.reg live; NULL: the user's app data */
    /* from the command line, over what was saved; 0 / NULL for none */
    const char* server;
    const char* user;
    const char* password;
    const char* otp;
    uint16_t auth_port, data_port, view_port;
    const char* loader_version; /* "major.minor.patch" sent at sign-in */
    /* first-run defaults (an app bundle's, appdefaults.h), under what the player saved; 0 / NULL
     * for the built-in ones (LandSandBoat on 127.0.0.1, a 1920x1080 window) */
    const char* default_server;
    int default_mode;                /* settings.reg's 0034, -1 for none */
    int default_w, default_h;        /* 0001 x 0002 */
    int default_menu_w, default_menu_h; /* 0037 x 0038 */
    const char* default_background;  /* a picture, when the player has none */
    int default_space;               /* full screen in a macOS Space of its own: 1, 0, -1 for none */
    const char* user_dir;            /* the game's USER folder (host64 --user-dir); NULL: the install's */
} SigninSetup;

typedef struct SigninResult
{
    void* window;             /* the SDL window, for the game to take over (user32_adopt_window) */
    uint32_t server;          /* the lobby's IPv4, host byte order */
    char settings_reg[1024];  /* the display settings host64 loads when given none (--reg-final) */
    char data_dir[1024];      /* the folder those live in: host64's --data-dir when given none */
} SigninResult;

/* Shows the screen until the player signs in (1) or quits (0). -1 when there is nothing to draw
 * with: host64 then signs in from its command line as before. */
int signin_run(const SigninSetup* setup, SigninResult* out);
