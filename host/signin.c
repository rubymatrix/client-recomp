/* The sign-in screen. See signin.h.
 *
 * Two windows drawn as the game's Config pages are: Sign in (the LandSandBoat username, the
 * password, the one-time code, Sign in and Settings) and Settings (the server, remembering the
 * password, the window theme, full screen). Keys: Tab and Up/Down move between rows, Left/Right along a row's buttons, Enter or Space
 * picks, Escape goes back or quits; the mouse clicks.
 *
 * What it remembers is in signin.cfg (key=value lines) beside settings.reg, the password in the
 * keychain (keychain.h). The sign-in runs on a worker thread so the window keeps drawing. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datui.h"
#include "keychain.h"
#include "lsb_login.h"
#include "plat.h"
#include "sewave.h"
#include "signin.h"
#include "discord.h"
#include "uidraw.h"


/* ---- what is remembered ---- */

typedef struct Config
{
    char server[128];
    char user[64];
    int remember;
    int theme;
    int space; /* full screen in a macOS Space of its own (1) or in place (0) */
    uint16_t auth_port, data_port, view_port;
    char loader_version[24]; /* "": LSB_LOADER_VERSION */
    char background[1024]; /* a picture behind the screen; "": background.png/.jpg beside signin.cfg */
} Config;

static void config_path(const char* dir, const char* name, char* out, size_t n)
{
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/", name);
}

/* ---- the texture pack's font ----
 * host64 --textures (default <data dir>/textures) can hold a 4x drawing of the game's font: the
 * pack's entry for a 1024x2048 texture (the size of font/moji and of no other texture the game
 * uploads), a DXT5 DDS whose colour is premultiplied (tools/make_texpack.py --additive). This screen
 * uses it for both its fonts: the outlined one as it is, the dark text's cores from its brightness
 * (the fill is white and the outline black, so brightness is the fill's coverage). The metrics stay
 * the DAT's: the glyphs sit in the same cells. */
static uint8_t* dxt5_decode(const uint8_t* src, uint32_t w, uint32_t h)
{
    uint8_t* out = malloc((size_t)w * h * 4);
    if (!out)
        return NULL;
    for (uint32_t by = 0; by < h / 4; ++by)
        for (uint32_t bx = 0; bx < w / 4; ++bx, src += 16)
        {
            uint8_t a[8] = { src[0], src[1] };
            for (int i = 2; i < 8; ++i)
                a[i] = a[0] > a[1] ? (uint8_t)(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                     : i < 6       ? (uint8_t)(((6 - i) * a[0] + (i - 1) * a[1]) / 5)
                                   : (uint8_t)(i == 6 ? 0 : 255);
            uint64_t ab = 0;
            for (int i = 0; i < 6; ++i)
                ab |= (uint64_t)src[2 + i] << (8 * i);
            uint16_t c0 = (uint16_t)(src[8] | src[9] << 8), c1 = (uint16_t)(src[10] | src[11] << 8);
            uint8_t pal[4][3];
            for (int k = 0; k < 2; ++k)
            {
                uint16_t c = k ? c1 : c0;
                pal[k][0] = (uint8_t)((c >> 11 & 31) * 255 / 31), pal[k][1] = (uint8_t)((c >> 5 & 63) * 255 / 63),
                pal[k][2] = (uint8_t)((c & 31) * 255 / 31);
            }
            for (int k = 0; k < 3; ++k)
                pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3), pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
            uint32_t bits = (uint32_t)src[12] | src[13] << 8 | src[14] << 16 | (uint32_t)src[15] << 24;
            for (int i = 0; i < 16; ++i)
            {
                uint8_t* d = out + (((size_t)(by * 4 + (uint32_t)i / 4)) * w + bx * 4 + (uint32_t)i % 4) * 4;
                const uint8_t* c = pal[bits >> (2 * i) & 3];
                d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = a[ab >> (3 * i) & 7];
            }
        }
    return out;
}

/* The pack's entry for an uploaded texture of w by h (its name's "_<w>x<h>"; with hash, that
 * texture's alone), decoded, at its own size (a multiple of w by h). NULL when there is none. */
static uint8_t* pack_image(const char* dir, const char* hash, uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph,
    char* path, size_t n)
{
    char folder[1100], size[32];
    config_path(dir, "textures", folder, sizeof folder);
    snprintf(size, sizeof size, "_%ux%u", w, h);
    path[0] = 0;
    PlatDir* d = plat_dir_open(folder);
    for (const char* name; d && (name = plat_dir_next(d));)
        if (strstr(name, size) && strstr(name, ".dds") && (!hash || !strncmp(name, hash, strlen(hash))))
            snprintf(path, n, "%s/%s", folder, name);
    if (d)
        plat_dir_close(d);
    size_t bytes = 0;
    unsigned char* f = path[0] ? plat_read_file(path, &bytes) : NULL;
    uint32_t fw = f && bytes >= 128 ? (uint32_t)f[16] | f[17] << 8 | f[18] << 16 | (uint32_t)f[19] << 24 : 0;
    uint32_t fh = f && bytes >= 128 ? (uint32_t)f[12] | f[13] << 8 | f[14] << 16 | (uint32_t)f[15] << 24 : 0;
    uint8_t* px = fw && fh && fw % w == 0 && fh * w == fw * h && !memcmp(f + 84, "DXT5", 4) && bytes >= 128 + (size_t)fw * fh
        ? dxt5_decode(f + 128, fw, fh) : NULL;
    free(f);
    *pw = fw, *ph = fh;
    return px;
}

static void hires_font(UiTexSet* set, const char* dir)
{
    char path[1400];
    uint32_t w, h;
    uint8_t* px = pack_image(dir, NULL, 1024, 2048, &w, &h, path, sizeof path);
    if (!px)
        return;
    uint8_t* ink = malloc((size_t)w * h * 4);
    for (size_t i = 0; ink && i < (size_t)w * h; ++i)
    {
        uint8_t* p = px + i * 4;
        unsigned l = (p[0] + p[1] + p[2]) / 3;
        ink[i * 4 + 0] = ink[i * 4 + 1] = ink[i * 4 + 2] = 255, ink[i * 4 + 3] = (uint8_t)l;
        for (int k = 0; k < 3; ++k) /* the outlined font: its colour back from premultiplied */
            p[k] = p[3] ? (uint8_t)(p[k] * 255u / p[3] > 255 ? 255 : p[k] * 255u / p[3]) : 0;
    }
    if (ink && uidraw_load_rgba_scaled(set, "moji", px, w, h, 1024, 2048) &&
        uidraw_load_rgba_scaled(set, "mojiink", ink, w, h, 1024, 2048))
        fprintf(stderr, "[signin] font from %s (%ux%u)\n", path, w, h);
    free(ink);
    free(px);
}

/* "menu/gauge" drawn anew (the pack's entry for its upload: the track, the bookends' bead), its
 * alpha 0-255: the fields' tracks and bookends, the title strip */
static void hires_gauge(UiTexSet* set, const char* dir)
{
    char path[1400];
    uint32_t w, h;
    uint8_t* px = pack_image(dir, "62246ee7e150d7ba", 64, 64, &w, &h, path, sizeof path);
    if (px && uidraw_load_rgba_scaled(set, "gauge", px, w, h, 64, 64))
        fprintf(stderr, "[signin] gauge from %s (%ux%u)\n", path, w, h);
    free(px);
}

static void config_load(const char* path, Config* c)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char* v = strchr(line, '=');
        if (!v)
            continue;
        *v++ = 0;
        if (!strcmp(line, "server"))
            SDL_strlcpy(c->server, v, sizeof c->server);
        else if (!strcmp(line, "user"))
            SDL_strlcpy(c->user, v, sizeof c->user);
        else if (!strcmp(line, "remember"))
            c->remember = atoi(v) != 0;
        else if (!strcmp(line, "theme"))
            c->theme = atoi(v);
        else if (!strcmp(line, "fullscreen_space"))
            c->space = atoi(v) != 0;
        else if (!strcmp(line, "background"))
            SDL_strlcpy(c->background, v, sizeof c->background);
        else if (!strcmp(line, "auth_port"))
            c->auth_port = (uint16_t)atoi(v);
        else if (!strcmp(line, "data_port"))
            c->data_port = (uint16_t)atoi(v);
        else if (!strcmp(line, "view_port"))
            c->view_port = (uint16_t)atoi(v);
        else if (!strcmp(line, "loader_version"))
            SDL_strlcpy(c->loader_version, v, sizeof c->loader_version);
    }
    fclose(f);
}

static void config_save(const char* path, const Config* c)
{
    FILE* f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "[signin] cannot write %s\n", path);
        return;
    }
    fprintf(f, "server=%s\nuser=%s\nremember=%d\ntheme=%d\n", c->server, c->user, c->remember, c->theme);
    fprintf(f, "fullscreen_space=%d\n", c->space);
    if (c->background[0])
        fprintf(f, "background=%s\n", c->background);
    if (c->auth_port || c->data_port || c->view_port)
        fprintf(f, "auth_port=%u\ndata_port=%u\nview_port=%u\n", c->auth_port, c->data_port, c->view_port);
    if (c->loader_version[0])
        fprintf(f, "loader_version=%s\n", c->loader_version);
    fclose(f);
}

static void keychain_key(const Config* c, char* out, size_t n)
{
    snprintf(out, n, "lsb:%s:%s", c->server, c->user);
}

/* The game's display settings when nothing else gives them: a window of 1920x1080, as the
 * launcher's defaults, or the app's own. Written once; the player's own edits to it stay. */
static void default_settings(const char* path, const SigninSetup* su)
{
    FILE* f = fopen(path, "r");
    if (f)
    {
        fclose(f);
        return;
    }
    f = fopen(path, "w");
    if (!f)
        return;
    static const struct
    {
        const char* name;
        uint32_t v;
    } VALUES[] = {
        { "0000", 6 }, { "0001", 1920 }, { "0002", 1080 }, { "0003", 4096 }, { "0004", 4096 }, { "0007", 1 },
        { "0011", 1 }, { "0017", 1 }, { "0018", 1 }, { "0019", 1 }, { "0021", 0 }, { "0022", 1 },
        { "0023", 0 }, { "0029", 12 }, { "0034", 1 }, { "0035", 1 }, { "0036", 0 }, { "0037", 960 },
        { "0038", 540 }, { "0040", 0 },
    };
    fprintf(f, "REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
    for (size_t i = 0; i < sizeof VALUES / sizeof *VALUES; ++i)
    {
        uint32_t v = VALUES[i].v;
        const char* n = VALUES[i].name;
        if (!strcmp(n, "0034") && su->default_mode >= 0 && su->default_mode <= 3)
            v = (uint32_t)su->default_mode;
        else if (!strcmp(n, "0001") && su->default_w >= 640)
            v = (uint32_t)su->default_w;
        else if (!strcmp(n, "0002") && su->default_h >= 480)
            v = (uint32_t)su->default_h;
        else if (!strcmp(n, "0037") && su->default_menu_w >= 512)
            v = (uint32_t)su->default_menu_w;
        else if (!strcmp(n, "0038") && su->default_menu_h >= 384)
            v = (uint32_t)su->default_menu_h;
        fprintf(f, "\"%s\"=dword:%08x\r\n", n, v);
    }
    fclose(f);
}

/* A DWORD of the display settings (settings.reg: "name"=dword:hex), or dflt. */
static uint32_t settings_dword(const char* path, const char* name, uint32_t dflt)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return dflt;
    char line[256], want[16];
    snprintf(want, sizeof want, "\"%s\"=dword:", name);
    uint32_t v = dflt;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, strlen(want)))
            v = (uint32_t)strtoul(line + strlen(want), NULL, 16);
    fclose(f);
    return v;
}

/* ---- the sign-in, on a worker thread ---- */

enum
{
    JOB_IDLE,
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED,
};

typedef struct Job
{
    SDL_AtomicInt state;
    char server[128], user[64], password[128], otp[32];
    uint16_t auth_port, data_port, view_port;
    char loader_version[24], version_used[24];
    uint32_t ip;
    char error[512];
} Job;

static Job g_job;

static void job_thread(void* arg)
{
    Job* j = arg;
    int ok = 0;
    if (!net_resolve_ipv4(j->server, &j->ip))
        snprintf(j->error, sizeof j->error, "Cannot find the server \"%s\".", j->server);
    else
    {
        LsbLogin l = { j->ip,
            j->auth_port ? j->auth_port : 54231,
            j->data_port ? j->data_port : 54230,
            j->view_port ? j->view_port : 54001,
            j->user,
            j->password,
            j->otp,
            NULL,
            j->loader_version,
            j->version_used };
        ok = lsb_login(&l, j->error, sizeof j->error);
    }
    memset(j->password, 0, sizeof j->password);
    if (ok)
        fprintf(stderr, "[signin] signed in as %s on %s\n", j->user, j->server);
    else
        fprintf(stderr, "[signin] sign-in as %s on %s failed: %s\n", j->user, j->server, j->error);
    SDL_SetAtomicInt(&j->state, ok ? JOB_DONE : JOB_FAILED);
}

/* ---- the keychain, off the main thread: macOS asks the player whether this app may read or
 * change a saved password (again after every rebuild: an ad-hoc signature changes), and the
 * window has to keep drawing while it asks ---- */

typedef struct Keyjob
{
    SDL_AtomicInt state; /* 0 idle, 1 reading, 2 read */
    char key[256], password[128];
    int found;
} Keyjob;

static Keyjob g_keyread;

static void keyread_thread(void* arg)
{
    Keyjob* k = arg;
    k->found = keychain_get(k->key, k->password, sizeof k->password);
    SDL_SetAtomicInt(&k->state, 2);
}

typedef struct Keysave
{
    char key[256], password[128];
    int remember;
} Keysave;

static void keysave_thread(void* arg)
{
    Keysave* k = arg;
    if (k->remember)
        keychain_set(k->key, k->password);
    else
        keychain_delete(k->key);
    memset(k->password, 0, sizeof k->password);
    free(k);
}

/* ---- the screen ---- */

typedef struct Dat
{
    DatFile file;
    UiTexSet tex;
} Dat;

static int open_dat_at(const char* host_game, const char* rel, Dat* d, const char* category, const char* const* names)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", host_game, rel);
    memset(d, 0, sizeof *d);
    if (!dat_read(path, &d->file))
    {
        fprintf(stderr, "[signin] cannot read %s\n", path);
        return 0;
    }
    uidraw_load(&d->tex, &d->file, category, names);
    return 1;
}

static int open_dat(const char* host_game, int n, Dat* d, const char* category, const char* const* names)
{
    char rel[32];
    snprintf(rel, sizeof rel, "ROM/0/%d.DAT", n);
    return open_dat_at(host_game, rel, d, category, names);
}

static void close_dat(Dat* d)
{
    uidraw_free(&d->tex);
    dat_free(&d->file);
}

enum
{
    W_TEXT,
    W_SECRET,
    W_CHOICE, /* a button an option, as a Config page's row */
    W_BUTTON,
};

enum
{
    SCREEN_SIGNIN,
    SCREEN_SETTINGS,
};

/* What a control does: its id, for the code that acts on it */
enum
{
    ID_USER,
    ID_PASSWORD,
    ID_OTP,
    ID_SIGNIN,
    ID_SETTINGS,
    ID_SERVER,
    ID_REMEMBER,
    ID_THEME,
    ID_SPACE,
    ID_BACK,
};

enum
{
    MAX_OPTS = 8,
};

typedef struct Widget
{
    int kind, id;
    const char* label;
    char* text; /* W_TEXT / W_SECRET: the buffer */
    size_t cap;
    int nopt; /* W_CHOICE: its options */
    const char* opt[MAX_OPTS];
    float x0, y0, x1, y1; /* where it was drawn, for the mouse */
    float ox0[MAX_OPTS], ox1[MAX_OPTS];
} Widget;

typedef struct Ui
{
    Config cfg;
    char password[128], otp[32];
    int screen, focus, sub; /* sub: the option of a choice the cursor is on */
    Widget w[8];
    int nw;
    char status[512];
    int status_error;
    Dat frame, fonts, menu, pc, keytop;
    DatSheet win00, lobbywin;
    int have_art; /* the PC title's art (lobbywin's titlwin) */
    UiFont font, ink; /* the game's font, and its glyphs' cores alone ("mojiink"): typed text */
    const char* host_game;
    int theme_loaded;
    Uint64 focus_time;
    Uint64 opened;              /* when the screen came up: a keychain wait says so after a moment */
    Uint64 anim_at;             /* when the window began opening, or closing */
    int closing, close_result;  /* closing: then signin_run returns close_result */
    Uint64 press_at;            /* the last button pressed: when, which control, which option */
    int press_w, press_o;
    SePlayer* se;               /* the menus' sounds (SND_*), if there is audio */
    SeWave snd[4];
    float se_gain;              /* at the player's volume */
} Ui;

/* The menus' sounds, as the game's menus play them: their effect ids (ROM/0/0.DAT's "0001".."0004") */
enum
{
    SND_MOVE,    /* the cursor to another item: se000001 */
    SND_CONFIRM, /* an item picked: se000002 */
    SND_CANCEL,  /* back, or the window closed: se000003 */
    SND_ERROR,   /* what cannot be done: se000004 */
};

static void sound(Ui* u, int which)
{
    se_play(u->se, which, &u->snd[which], u->se_gain);
}

/* Colours at 255 = the texel as it is, as the sheets have them (0x7f of 0x80) */
static const uint8_t WHITE[4] = { 254, 254, 254, 254 }, RED[4] = { 255, 140, 120, 254 },
                     FIELD[4] = { 254, 254, 254, 64 }, FIELD_ON[4] = { 254, 254, 254, 100 };

static int load_theme(Ui* u)
{
    static const char* const THEME[] = { "newtex", "corner", "hfr1", "vfr1", NULL };
    int t = u->cfg.theme >= 1 && u->cfg.theme <= 8 ? u->cfg.theme : 1;
    if (u->theme_loaded == t)
        return 1;
    if (u->theme_loaded)
    {
        dat_sheet_free(&u->win00);
        close_dat(&u->frame);
    }
    u->theme_loaded = 0;
    if (!open_dat(u->host_game, 13 + t, &u->frame, "menu", THEME) || !dat_sheet(&u->frame.file, "win00", &u->win00))
        return 0;
    u->theme_loaded = t;
    return 1;
}

static void add(Ui* u, int kind, int id, const char* label, char* buf, size_t cap)
{
    Widget* w = &u->w[u->nw++];
    memset(w, 0, sizeof *w);
    w->kind = kind, w->id = id, w->label = label, w->text = buf, w->cap = cap;
}

static void options(Ui* u, int id, const char* label, int n, const char* const* opt)
{
    add(u, W_CHOICE, id, label, NULL, 0);
    Widget* w = &u->w[u->nw - 1];
    w->nopt = n;
    for (int i = 0; i < n; ++i)
        w->opt[i] = opt[i];
}

/* The controls of the current screen, in focus order */
static void build(Ui* u)
{
    static const char* const ONOFF[] = { "ON", "OFF" },
                             *const THEMES[] = { "1", "2", "3", "4", "5", "6", "7", "8" },
                             *const SPACE[] = { "Own Space", "In Place" };
    u->nw = 0;
    if (u->screen == SCREEN_SIGNIN)
    {
        add(u, W_TEXT, ID_USER, "Username", u->cfg.user, sizeof u->cfg.user);
        add(u, W_SECRET, ID_PASSWORD, "Password", u->password, sizeof u->password);
        add(u, W_TEXT, ID_OTP, "One-time Code", u->otp, sizeof u->otp);
        add(u, W_BUTTON, ID_SIGNIN, "Sign In", NULL, 0);
        add(u, W_BUTTON, ID_SETTINGS, "Settings", NULL, 0);
    }
    else
    {
        add(u, W_TEXT, ID_SERVER, "Server", u->cfg.server, sizeof u->cfg.server);
        options(u, ID_REMEMBER, "Remember Password", 2, ONOFF);
        options(u, ID_THEME, "Window Theme", 8, THEMES);
        options(u, ID_SPACE, "Full Screen", 2, SPACE);
        add(u, W_BUTTON, ID_BACK, "Back", NULL, 0);
    }
    if (u->focus >= u->nw)
        u->focus = u->nw - 1;
}

/* the option a choice is set to */
static int choice_value(const Ui* u, int id)
{
    if (id == ID_REMEMBER)
        return u->cfg.remember ? 0 : 1;
    if (id == ID_SPACE)
        return u->cfg.space ? 0 : 1;
    return (u->cfg.theme >= 1 && u->cfg.theme <= 8 ? u->cfg.theme : 1) - 1;
}

static int busy(void)
{
    return SDL_GetAtomicInt(&g_job.state) == JOB_RUNNING;
}

static void sound(Ui* u, int which);

static void set_status(Ui* u, const char* s, int error)
{
    snprintf(u->status, sizeof u->status, "%s", s);
    u->status_error = error;
    if (error)
        sound(u, SND_ERROR);
}

static void start_signin(Ui* u)
{
    if (busy())
        return;
    if (!u->cfg.server[0])
    {
        set_status(u, "Set the server in Settings first.", 1);
        return;
    }
    if (!u->cfg.user[0] || !u->password[0])
    {
        set_status(u, "Enter your username and password.", 1);
        u->focus = !u->cfg.user[0] ? 0 : 1;
        return;
    }
    Job* j = &g_job;
    SDL_strlcpy(j->server, u->cfg.server, sizeof j->server);
    SDL_strlcpy(j->user, u->cfg.user, sizeof j->user);
    SDL_strlcpy(j->password, u->password, sizeof j->password);
    SDL_strlcpy(j->otp, u->otp, sizeof j->otp);
    j->auth_port = u->cfg.auth_port, j->data_port = u->cfg.data_port, j->view_port = u->cfg.view_port;
    SDL_strlcpy(j->loader_version, u->cfg.loader_version, sizeof j->loader_version);
    j->error[0] = 0;
    SDL_SetAtomicInt(&j->state, JOB_RUNNING);
    fprintf(stderr, "[signin] signing in as %s on %s\n", j->user, j->server);
    if (!plat_thread_start(job_thread, j))
    {
        snprintf(j->error, sizeof j->error, "Cannot start the sign-in.");
        SDL_SetAtomicInt(&j->state, JOB_FAILED);
    }
    set_status(u, "", 0);
}

/* a choice set to option v */
static void choose(Ui* u, int id, int v)
{
    if (v == choice_value(u, id))
        return;
    if (id == ID_REMEMBER)
        u->cfg.remember = v == 0;
    else if (id == ID_SPACE)
    {
        u->cfg.space = v == 0;
        /* SDL reads it when it starts */
        set_status(u, "Full screen changes the next time the game starts.", 0);
    }
    else if (id == ID_THEME)
    {
        u->cfg.theme = v + 1;
        if (!load_theme(u))
            set_status(u, "That window theme did not load.", 1);
    }
}

/* the cursor onto control i: on a choice, at the option it is set to */
static void focus_on(Ui* u, int i)
{
    u->focus = i;
    if (i >= 0 && i < u->nw && u->w[i].kind == W_CHOICE)
        u->sub = choice_value(u, u->w[i].id);
    u->focus_time = SDL_GetTicks();
}

static void activate(Ui* u, int i)
{
    if (i < 0 || i >= u->nw)
        return;
    Widget* w = &u->w[i];
    if (w->kind == W_CHOICE || w->kind == W_BUTTON)
    {
        u->press_at = SDL_GetTicks(), u->press_w = i, u->press_o = w->kind == W_CHOICE ? u->sub : -1;
        sound(u, SND_CONFIRM);
    }
    if (w->kind == W_CHOICE)
        choose(u, w->id, u->sub);
    else if (w->id == ID_SETTINGS && !busy())
        u->screen = SCREEN_SETTINGS, set_status(u, "", 0), build(u), focus_on(u, 0), u->anim_at = SDL_GetTicks();
    else if (w->id == ID_BACK)
        u->screen = SCREEN_SIGNIN, build(u), focus_on(u, 0), u->anim_at = SDL_GetTicks();
    else if (u->screen == SCREEN_SIGNIN)
        start_signin(u);
}

static void type(Ui* u, const char* s)
{
    if (u->focus < 0 || u->focus >= u->nw || busy())
        return;
    Widget* w = &u->w[u->focus];
    if (w->kind != W_TEXT && w->kind != W_SECRET)
        return;
    size_t len = strlen(w->text);
    for (; *s && len + 1 < w->cap; ++s)
        if (*s >= ' ' && *s <= '~') /* the font's ASCII */
            w->text[len++] = *s;
    w->text[len] = 0;
    u->focus_time = SDL_GetTicks();
}

/* ---- the screen, as the PC lobby's first (the notice with Accept and Decline) ----
 * The lobby lays out in the menus' own 1024x576 (registry 0037/0038's 16:9, a texel of the art a
 * unit), k pixels a unit, in a 16:9 box centred on the screen; its backdrop and bar along the
 * bottom run the screen's whole width. The pieces (FFXiMain 2026-09-03, lobbywin in ROM/119/50.DAT):
 *   - the backdrop, window "ptcbgwin": lobbywin sprite 85 stretched from 640x480 over the screen,
 *     the theme's background in two halves over black, and the key art (part 2) at its top left;
 *   - the bar, window "lobyhelp": 26 units from 69 above the bottom, the screen's width, the theme's
 *     unfocused window background and frame;
 *   - the notice, window "ptc8lice": 512 wide, from 192 down, centred - the theme's focused window
 *     background, lobbywin sprite 95's shading, the theme's frame - and in it the sign-in's rows and
 *     the buttons, as Accept and Decline are (lobbywin 101, 102): a "b1n" pill added in blue, the
 *     words in "keytopHD"'s letters, the one under the cursor tinted 0x80805020 and a unit up and
 *     left. The sheets' parts keep their colours and blends (datui.h's UI_BLEND_*). */

enum
{
    WIN_W = 512, /* the notice's */
    WIN_Y = 192,
    PAD = 24,    /* its words from its left edge */
    RIGHT = 34,  /* its controls, and the buttons (Decline), from its right */
    LINE_H = 16, /* a line of its words */
    TOP_H = 36,  /* the status line, and room under it */
    STATUS_LINES = 3, /* the most the status wraps to (a server's message) */
    ROW_H = 24,    /* a field's row */
    CHOICE_H = 30, /* a row of buttons' (their pills 24 tall) */
    OPTION_W = 44, /* a choice's button at the least */
    GAP = 10,    /* between buttons (Accept and Decline's) */
    BUTTON_W = 118,
};

static void quad(UiQuad* q, float x0, float y0, float x1, float y1, float u, float v, float uw, float uh,
    const uint8_t top[4], const uint8_t bottom[4], const char* image)
{
    memset(q, 0, sizeof *q);
    q->x0 = x0, q->y0 = y0, q->x1 = x1, q->y1 = y1;
    q->u0 = u, q->v0 = v, q->u1 = u + uw, q->v1 = v + uh;
    memcpy(q->color[0], top, 4), memcpy(q->color[1], top, 4);
    memcpy(q->color[2], bottom, 4), memcpy(q->color[3], bottom, 4);
    q->image = image;
}


/* A sheet's part at ox, oy (its own origin), sx, sy pixels a unit, its colours (0x80 = 1.0) times
 * tint (255 = 1.0) and its alpha times a */
static void part_quad(UiQuad* q, const DatPart* p, float ox, float oy, float sx, float sy, const uint8_t tint[4], float a)
{
    memset(q, 0, sizeof *q);
    q->x0 = ox + p->x[0] * sx, q->y0 = oy + p->y[0] * sy, q->x1 = ox + p->x[3] * sx, q->y1 = oy + p->y[3] * sy;
    q->u0 = p->u, q->v0 = p->v, q->u1 = p->u + p->uw, q->v1 = p->v + p->uh;
    for (int v = 0; v < 4; ++v)
        for (int c = 0; c < 4; ++c)
        {
            unsigned t = p->color[v][c] * 2u * tint[c] / 255u;
            t = c == 3 ? (unsigned)(t * a) : t;
            q->color[v][c] = (uint8_t)(t > 255 ? 255 : t);
        }
    q->image = p->name;
    q->blend = (uint8_t)ui_part_blend(p);
}

/* The theme's window at x, y, w by h pixels (k a unit): its background (focused or not) and its
 * frame, in the sheet's colours and blends, their alpha times a. Draws the background, then fill,
 * then the frame over both. */
static void theme_window(Ui* u, float x, float y, float w, float h, float k, int focused, float a,
    void (*fill)(Ui*, float, float, float, float, float, float), float fk)
{
    static const int SPRITE[UI_WINDOW_QUADS] = { 0, 1, 3, 6, 8, 2, 7, 4, 5 }; /* ui_window's order */
    UiQuad q[UI_WINDOW_QUADS];
    unsigned n = ui_window(&u->win00, x, y, w, h, k, focused, q);
    for (unsigned i = 0; i < n; ++i)
    {
        const DatPart* p = u->win00.sprites[i ? SPRITE[i] : focused ? 9 : 0].parts;
        for (int v = 0; v < 4; ++v)
            for (int c = 0; c < 4; ++c)
            {
                unsigned t = p->color[v][c] * 2u;
                t = c == 3 ? (unsigned)(t * a) : t;
                q[i].color[v][c] = (uint8_t)(t > 255 ? 255 : t);
            }
        q[i].blend = (uint8_t)ui_part_blend(p);
    }
    if (n)
        uidraw_quads(&u->frame.tex, q, 1);
    if (fill)
        fill(u, x, y, w, h, fk, a);
    if (n)
        uidraw_quads(&u->frame.tex, q + 1, n - 1);
}

/* the notice's shading (lobbywin sprite 95's newtex parts), its 144 lines to the window's height */
static void notice_fill(Ui* u, float x, float y, float w, float h, float k, float a)
{
    static const uint8_t ONE[4] = { 255, 255, 255, 255 };
    if (!u->have_art || u->lobbywin.nsprites <= 95)
        return;
    const DatSprite* s = &u->lobbywin.sprites[95];
    float sy = h / 144.0f;
    for (uint32_t i = 0; i < s->nparts && i < 2; ++i)
    {
        UiQuad q;
        part_quad(&q, &s->parts[i], x + w * 0.5f, y, w / WIN_W, sy, ONE, a);
        q.v1 = q.v0 + s->parts[i].uh * sy / k; /* its texels a unit, not stretched */
        uidraw_quads(&u->frame.tex, &q, 1);
    }
}

/* "keytopHD"'s proportional letters (ROM/118/111.DAT): u and width in the texture, and their row -
 * rows' tops 359, 385, 410, heights 25, 24, 26, baselines 379, 405, 432. The lobby draws them 0.7
 * units a texel, each advancing its width and a unit, a space 5. */
typedef struct KeyGlyph
{
    char c;
    uint16_t u;
    uint8_t w, row;
} KeyGlyph;

static const KeyGlyph KEYTOP[] = {
    { '1', 5, 7, 0 }, { '2', 21, 13, 0 }, { '3', 37, 13, 0 }, { '4', 53, 15, 0 }, { '5', 72, 12, 0 },
    { '6', 88, 13, 0 }, { '7', 104, 14, 0 }, { '8', 121, 14, 0 }, { '9', 138, 14, 0 }, { '0', 154, 15, 0 },
    { ':', 172, 5, 0 }, { '-', 183, 4, 0 }, { '_', 192, 16, 0 }, { '/', 209, 12, 0 }, { '@', 223, 20, 0 },
    { 'a', 244, 12, 0 }, { 'b', 259, 14, 0 }, { 'c', 274, 13, 0 }, { 'd', 288, 13, 0 }, { 'e', 304, 12, 0 },
    { 'f', 318, 9, 0 }, { 'g', 328, 14, 0 }, { 'h', 344, 12, 0 }, { 'i', 359, 4, 0 }, { 'j', 364, 7, 0 },
    { 'k', 374, 12, 0 }, { 'l', 388, 4, 0 }, { 'm', 394, 20, 0 }, { 'n', 417, 12, 0 }, { 'o', 431, 14, 0 },
    { 'p', 447, 13, 0 }, { 'q', 462, 13, 0 }, { 'r', 477, 9, 0 }, { 's', 487, 11, 0 }, { 't', 499, 11, 0 },
    { 'u', 3, 12, 1 }, { 'v', 17, 14, 1 }, { 'w', 31, 19, 1 }, { 'x', 50, 13, 1 }, { 'y', 66, 14, 1 },
    { 'z', 81, 13, 1 }, { '!', 96, 4, 1 }, { '?', 104, 11, 1 }, { 'A', 117, 18, 1 }, { 'B', 137, 14, 1 },
    { 'C', 152, 15, 1 }, { 'D', 169, 15, 1 }, { 'E', 187, 12, 1 }, { 'F', 201, 12, 1 }, { 'G', 215, 15, 1 },
    { 'H', 233, 14, 1 }, { 'I', 250, 4, 1 }, { 'J', 255, 11, 1 }, { 'K', 269, 15, 1 }, { 'L', 286, 13, 1 },
    { 'M', 301, 17, 1 }, { 'N', 320, 15, 1 }, { 'O', 338, 17, 1 }, { 'P', 357, 13, 1 }, { 'Q', 371, 18, 1 },
    { 'R', 389, 14, 1 }, { 'S', 405, 14, 1 }, { 'T', 419, 17, 1 }, { 'U', 437, 14, 1 }, { 'V', 453, 17, 1 },
    { 'W', 470, 23, 1 }, { 'X', 494, 15, 1 }, { 'Y', 2, 15, 2 }, { 'Z', 18, 13, 2 }, { ',', 32, 6, 2 },
    { '.', 42, 4, 2 },
};

static const float KEY_TOP[3] = { 359, 385, 410 }, KEY_H[3] = { 25, 24, 26 }, KEY_BASE[3] = { 379, 405, 432 };

static const KeyGlyph* key_glyph(char c)
{
    for (size_t i = 0; i < sizeof KEYTOP / sizeof *KEYTOP; ++i)
        if (KEYTOP[i].c == c)
            return &KEYTOP[i];
    return NULL;
}

static float key_width(const char* s)
{
    float w = 0;
    for (; *s; ++s)
    {
        const KeyGlyph* g = key_glyph(*s);
        w += g ? g->w * 0.7f + 1 : 5;
    }
    return w - 1;
}

/* s with its baseline 15 units under y, as Accept's words: each glyph a shadow taken away two units
 * down and right, then its face added, tinted */
static void key_text(Ui* u, const char* s, float x, float y, float k, const uint8_t tint[4], float a)
{
    UiQuad q[2][64];
    unsigned n = 0;
    for (float at = x; *s && n < 64; ++s)
    {
        const KeyGlyph* g = key_glyph(*s);
        if (!g)
        {
            at += 5 * k;
            continue;
        }
        float top = y + (15 - (KEY_BASE[g->row] - KEY_TOP[g->row]) * 0.7f) * k, gw = g->w * 0.7f * k,
              gh = KEY_H[g->row] * 0.7f * k;
        for (int pass = 0; pass < 2; ++pass)
        {
            UiQuad* t = &q[pass][n];
            float d = pass ? 0 : 2 * k;
            uint8_t col[4] = { pass ? tint[0] : 254, pass ? tint[1] : 254, pass ? tint[2] : 254,
                (uint8_t)((pass ? 254 : 64) * a) };
            quad(t, at + d, top + d, at + d + gw, top + d + gh, g->u, KEY_TOP[g->row], g->w, KEY_H[g->row], col, col,
                "keytophd");
            t->blend = pass ? UI_BLEND_ADD : UI_BLEND_SUBTRACT;
        }
        ++n;
        at += gw + k;
    }
    uidraw_quads(&u->keytop.tex, q[0], n);
    uidraw_quads(&u->keytop.tex, q[1], n);
}

/* how far into a press's flash control i (option o) is, in ticks of 1/60 s left of 9; 0 when not */
static float pressing(const Ui* u, int i, int o)
{
    float t = 9 - (float)(SDL_GetTicks() - u->press_at) * 60 / 1000;
    return u->press_w == i && u->press_o == o && t > 0 ? t : 0;
}


/* The tint of the item under the cursor (0x80805020: orange), and of the rest */
static const uint8_t TINT_ON[4] = { 255, 160, 64, 255 }, TINT_OFF[4] = { 255, 255, 255, 255 };

/* A button as Accept and Decline: its origin at x, y (the pill from 4 units above to 20 below), w
 * wide; the "b1n" pill in three (a left end 38 units of its texels 0-96, a body from 32, a right end
 * of 32-128), its shadow two units down and right; the words centred. Under the cursor it stands a
 * unit up and left, orange with gold words; pressed, a unit down and right, flashing. */
static void pill(Ui* u, float x, float y, float w, float k, const uint8_t tint[4], float a, int shadow)
{
    static const uint8_t BLUE[4] = { 96, 96, 127, 112 }, SHADOW[4] = { 0, 0, 0, 112 };
    float e = 38 * k < w * 0.5f ? 38 * k : w * 0.5f, xs[4] = { x, x + e, x + w - e, x + w };
    static const float U[3][2] = { { 0, 96 }, { 32, 42 }, { 32, 96 } };
    uint8_t col[4];
    for (int c = 0; c < 4; ++c)
    {
        unsigned t = (shadow ? SHADOW : BLUE)[c] * 2u * (shadow ? 255u : tint[c]) / 255u;
        t = c == 3 ? (unsigned)(t * a) : t;
        col[c] = (uint8_t)(t > 255 ? 255 : t);
    }
    UiQuad q[3];
    for (int i = 0; i < 3; ++i)
    {
        quad(&q[i], xs[i], y - 4 * k, xs[i + 1], y + 20 * k, U[i][0], 0, U[i][1], 64, col, col, "b1n");
        q[i].blend = shadow ? UI_BLEND_ALPHA : UI_BLEND_ADD;
    }
    uidraw_quads(&u->pc.tex, q, 3);
}

static void button(Ui* u, const char* s, float x, float y, float w, float k, int cursor, int chosen, float press)
{
    pill(u, x + 2 * k, y + 2 * k, w, k, TINT_OFF, 1, 1);
    float d = press > 0 ? 1 : cursor ? -1 : 0, bx = x + d * k, by = y + d * k;
    const uint8_t* tint = cursor || press > 0 ? TINT_ON : TINT_OFF;
    pill(u, bx, by, w, k, tint, 1, 0);
    key_text(u, s, bx + (w - key_width(s) * k) * 0.5f, by, k, tint, 1);
    /* the press's flash, as the menus': two copies about its middle, growing to 1.3 times and
     * fading as they do, the second a little behind */
    for (int i = 0; press > 0 && i < 2; ++i)
    {
        float ki = 20 - press - 3.75f * i;
        if (ki < 0 || ki > 12.5f)
            continue;
        float sc = 1 + 0.3f * ki / 12.5f, a = 1 - ki / 12.5f, mx = bx + w * 0.5f, my = by + 8 * k;
        float cw = w * sc, ck = k * sc, cx = mx - cw * 0.5f, cy = my - 8 * ck;
        pill(u, cx, cy, cw, ck, TINT_ON, a, 0);
        key_text(u, s, cx + (cw - key_width(s) * ck) * 0.5f, cy, ck, TINT_ON, a);
    }
    if (chosen)
    {
        /* the Config pages' red line under the option a choice is set to */
        float x0 = x + 6 * k, x1 = x + w - 3 * k, y0 = y + 17 * k, y1 = y + 20 * k;
        UiQuad l[3];
        quad(&l[0], x0, y0, x0 + 9 * k, y1, 9, 22, 9, 3, WHITE, WHITE, "gauge");
        quad(&l[1], x0 + 9 * k, y0, x1 - 8 * k, y1, 10, 22, 8, 3, WHITE, WHITE, "gauge");
        quad(&l[2], x1 - 8 * k, y0, x1, y1, 11, 22, 8, 3, WHITE, WHITE, "gauge");
        uidraw_quads(&u->menu.tex, l, 3);
    }
}

/* what is typed, in the game's text font, its cores alone: white, on a field */
static void typed(Ui* u, const char* s, float x, float y, float scale, const uint8_t rgba[4])
{
    UiQuad q[160];
    uidraw_quads(&u->fonts.tex, q, ui_text(&u->ink, s, x, y, scale, rgba, q, 160));
}

/* the notice's words: the game's text font, white */
static void text(Ui* u, const char* s, float x, float y, float k, const uint8_t rgba[4])
{
    UiQuad q[160];
    uidraw_quads(&u->fonts.tex, q, ui_text(&u->font, s, x, y, k, rgba, q, 160));
}

/* a row's height */
static int row_h(const Widget* c)
{
    return c->kind == W_CHOICE ? CHOICE_H : ROW_H;
}

/* the least a choice's buttons need across */
static int choice_need(const Widget* c)
{
    int w = OPTION_W;
    for (int o = 0; o < c->nopt; ++o)
        if (key_width(c->opt[o]) + 30 > w)
            w = (int)key_width(c->opt[o]) + 30;
    return c->nopt * w + (c->nopt - 1) * GAP;
}

static int top_h(const Ui* u, int mw);

/* The window's size in units (WIN_W wide, or wider for a row of buttons that needs it), its
 * controls' column and right edge */
static void measure(const Ui* u, int* pw, int* ph, int* pcx, int* pright)
{
    int cx = 0, cw = WIN_W - RIGHT, rows = 0;
    for (int i = 0; i < u->nw; ++i)
        if (u->w[i].kind != W_BUTTON)
        {
            rows += row_h(&u->w[i]);
            /* after the longest caption, room for the cursor before a control */
            float need = PAD + ui_text_width(&u->font, u->w[i].label, 1) + 40;
            cx = need > cx ? (int)need : cx;
        }
    cw -= cx;
    for (int i = 0; i < u->nw; ++i)
        if (u->w[i].kind == W_CHOICE && choice_need(&u->w[i]) > cw)
            cw = choice_need(&u->w[i]);
    *pw = cx + cw + RIGHT, *pcx = cx, *pright = cx + cw;
    /* the status line, the rows, the buttons' 26 at the bottom as Accept's */
    *ph = top_h(u, *pw) + rows + 8 + 26;
}

/* The status line's words, and their colour: what is going on - the sign-in, a wait on the
 * keychain, what went wrong - or else where it signs in */
static const uint8_t* status_text(const Ui* u, char* msg, size_t n)
{
    if (busy())
        SDL_strlcpy(msg, "Connecting...", n);
    else if (u->status[0])
    {
        SDL_strlcpy(msg, u->status, n);
        return u->status_error ? RED : WHITE;
    }
    else if (SDL_GetAtomicInt(&g_keyread.state) == 1 && SDL_GetTicks() - u->opened > 1500)
        SDL_strlcpy(msg, "Waiting keychain...", n);
    else if (u->screen == SCREEN_SIGNIN)
        snprintf(msg, n, "Sign in to %s", u->cfg.server[0] ? u->cfg.server : "(no server)");
    else
        SDL_strlcpy(msg, "Settings", n);
    return WHITE;
}

/* Breaks msg into lines no wider than width, at spaces where it can: their starts and lengths.
 * Past STATUS_LINES the last is cut short. */
static int status_wrap(const Ui* u, const char* msg, float width, const char* start[STATUS_LINES],
    size_t len[STATUS_LINES])
{
    int n = 0;
    while (*msg && n < STATUS_LINES)
    {
        char line[512];
        size_t fit = 0, brk = 0, total = strlen(msg);
        while (fit < total && fit + 1 < sizeof line)
        {
            memcpy(line, msg, fit + 1);
            line[fit + 1] = 0;
            if (ui_text_width(&u->font, line, 1) > width)
                break;
            if (msg[++fit] == ' ' || !msg[fit])
                brk = fit;
        }
        if (fit < total && brk && n + 1 < STATUS_LINES)
            fit = brk; /* at the last space that fits */
        start[n] = msg, len[n] = fit ? fit : 1;
        msg += len[n++];
        while (*msg == ' ')
            ++msg;
    }
    return n ? n : 1;
}

/* the status line's height with the room under it: a line more for each it wraps onto */
static int top_h(const Ui* u, int mw)
{
    char msg[512];
    const char* start[STATUS_LINES];
    size_t len[STATUS_LINES];
    status_text(u, msg, sizeof msg);
    return TOP_H + (status_wrap(u, msg, (float)(mw - 2 * PAD), start, len) - 1) * LINE_H;
}

/* How far open the window is, 0-1: the game's menus take 25 ticks of 1/60 s either way */
enum
{
    OPEN_MS = 417,
};

static float open_progress(const Ui* u)
{
    float t = (float)(SDL_GetTicks() - u->anim_at) / OPEN_MS;
    if (u->closing)
        return t >= 1 ? 0 : 1 - t;
    return t >= 1 ? 1 : t;
}

/* the window closes; then signin_run returns result */
static void begin_close(Ui* u, int result)
{
    if (!u->closing)
        u->closing = 1, u->close_result = result, u->anim_at = SDL_GetTicks();
}

/* The menus' cursor ("anc/anc_s", 1.DAT): a jewelled arrow in six frames that shimmer in turn,
 * 32 by 20, placed from 32 left and 2 above the item at x, y (its top left) */
enum
{
    CURSOR_MS = 67, /* a frame */
};

static void cursor(Ui* u, float x, float y, float k)
{
    static const float V[3] = { 1, 22, 43 };
    int f = (int)(SDL_GetTicks() / CURSOR_MS % 6);
    UiQuad q;
    quad(&q, x - 32 * k, y - 2 * k, x, y + 18 * k, (float)(f / 3 * 32), V[f % 3], 32, 20, WHITE, WHITE, "anc");
    uidraw_quads(&u->menu.tex, &q, 1);
}

/* Draws the screen and records where each control is */
static void draw(Ui* u, int w, int h)
{
    static const uint8_t ONE[4] = { 255, 255, 255, 255 };
    /* the menus' 1024x576 box: as tall as the screen, or as wide on one narrower than 16:9 */
    float k = (float)h / 576.0f < (float)w / 1024.0f ? (float)h / 576.0f : (float)w / 1024.0f;
    float bx0 = floorf((w - 1024 * k) * 0.5f), by0 = floorf((h - 576 * k) * 0.5f);
    if (u->have_art && u->lobbywin.nsprites > 85)
    {
        /* the backdrop over the screen, the key art in the box */
        const DatSprite* s = &u->lobbywin.sprites[85];
        for (uint32_t i = 0; i < s->nparts && i < 3; ++i)
        {
            UiQuad q;
            if (!strcmp(s->parts[i].name, "titlwin"))
                part_quad(&q, &s->parts[i], bx0, by0, 1024 * k / 640, 576 * k / 480, ONE, 1);
            else
                part_quad(&q, &s->parts[i], 0, 0, w / 640.0f, h / 480.0f, ONE, 1);
            uidraw_quads(!strcmp(s->parts[i].name, "titlwin") ? &u->pc.tex : &u->frame.tex, &q, 1);
        }
    }
    /* the bar along the bottom */
    theme_window(u, 0, by0 + (576 - 69) * k, (float)w, 26 * k, k, 0, 1, NULL, k);

    int mw, mh, cx, right;
    measure(u, &mw, &mh, &cx, &right);
    float ww = mw * k, wh = mh * k, wx = floorf(bx0 + (1024 - mw) * 0.5f * k), wy = by0 + WIN_Y * k;
#define X(v) (wx + (v) * k)
#define Y(v) (wy + (v) * k)

    /* Opening or closing, as the game's menus do: the bare window zooms about its centre, and a
     * larger copy of it, fading as it grows, over it */
    float p = open_progress(u);
    if (p < 1)
    {
        float mx = wx + ww * 0.5f, my = wy + wh * 0.5f, sc = p * p, g = 1.6f * p, a = (96 - 80 * g) / 128;
        theme_window(u, mx - ww * sc * 0.5f, my - wh * sc * 0.5f, ww * sc, wh * sc, k * sc, 1, 1, notice_fill, k * sc);
        if (a > 0)
            theme_window(u, mx - ww * g * 0.5f, my - wh * g * 0.5f, ww * g, wh * g, k * g, 1, a, notice_fill, k * g);
        return;
    }
    theme_window(u, wx, wy, ww, wh, k, 1, 1, notice_fill, k);

    /* the status line, wrapped over as many lines as it takes (up to STATUS_LINES) */
    {
        char msg[512], line[512];
        const char* start[STATUS_LINES];
        size_t len[STATUS_LINES];
        const uint8_t* col = status_text(u, msg, sizeof msg);
        int n = status_wrap(u, msg, (float)(mw - 2 * PAD), start, len);
        for (int i = 0; i < n && msg[0]; ++i)
        {
            snprintf(line, sizeof line, "%.*s", (int)len[i], start[i]);
            text(u, line, X(PAD), Y(10 + i * LINE_H), k, col);
        }
    }

    /* a row a field or choice: its caption, then the field or the choice's buttons */
    Uint64 now = SDL_GetTicks();
    int r = top_h(u, mw);
    for (int i = 0; i < u->nw; ++i)
    {
        Widget* c = &u->w[i];
        if (c->kind == W_BUTTON)
            continue;
        /* the caption, a field and a button's words all on the row's line, 4 down (a pill stands 4
         * above its words) */
        int focused = i == u->focus, top = r + 4;
        r += row_h(c);
        text(u, c->label, X(PAD), Y(top), k, WHITE);
        if (c->kind == W_CHOICE)
        {
            float ow = (float)(right - cx - (c->nopt - 1) * GAP) / c->nopt;
            int v = choice_value(u, c->id);
            c->x0 = X(cx), c->y0 = Y(top - 4), c->x1 = X(right), c->y1 = Y(top + 20);
            for (int o = 0; o < c->nopt; ++o)
            {
                float bx = X(cx + o * (ow + GAP));
                button(u, c->opt[o], bx, Y(top), ow * k, k, focused && o == u->sub, o == v, pressing(u, i, o));
                c->ox0[o] = bx, c->ox1[o] = bx + ow * k;
            }
            if (focused)
                cursor(u, X(cx + u->sub * (ow + GAP)), Y(top), k);
        }
        else
        {
            /* a field: a slider's track between its bookends, lighter under the cursor, what is
             * typed on it */
            UiQuad f[3];
            quad(&f[0], X(cx), Y(top), X(cx + 8), Y(top + 16), 0, 8, 4, 8, WHITE, WHITE, "gauge");
            quad(&f[1], X(right - 8), Y(top), X(right), Y(top + 16), 4, 8, 4, 8, WHITE, WHITE, "gauge");
            /* the track's texels less its ends, which fade out to meet a slider's fill */
            quad(&f[2], X(cx + 8), Y(top), X(right - 8), Y(top + 16), 1, 0, 62, 8, focused ? FIELD_ON : FIELD,
                focused ? FIELD_ON : FIELD, "gauge");
            uidraw_quads(&u->menu.tex, f, 3);
            c->x0 = X(cx), c->y0 = Y(top), c->x1 = X(right), c->y1 = Y(top + 16);
            char shown[160];
            size_t len = strlen(c->text);
            if (c->kind == W_SECRET)
            {
                len = len < sizeof shown - 1 ? len : sizeof shown - 1;
                memset(shown, '*', len);
                shown[len] = 0;
            }
            else
                SDL_strlcpy(shown, c->text, sizeof shown);
            /* the end of a long entry, as fields scroll */
            float ts = k * 0.9375f, tx = X(cx + 14), room = (right - cx - 30) * k;
            const char* v = shown;
            while (*v && ui_text_width(&u->ink, v, ts) > room)
                ++v;
            typed(u, v, tx, Y(top + 0.5f), ts, WHITE);
            if (focused && !busy() && (now - u->focus_time) / 500 % 2 == 0)
            {
                float x = tx + ui_text_width(&u->ink, v, ts) + 1 * k;
                uidraw_rect(x, Y(top + 2), x + 1.5f * k, Y(top + 14), 0xFFE0E0E0u);
            }
            if (focused)
                cursor(u, X(cx), Y(top), k);
        }
    }

    /* the buttons along the bottom as Accept and Decline: 26 units from it, the last ending where
     * the controls do */
    float by = Y(mh - 26), bx = X(right);
    for (int i = u->nw - 1; i >= 0; --i)
    {
        Widget* c = &u->w[i];
        if (c->kind != W_BUTTON)
            continue;
        float bw = key_width(c->label) + 40 > BUTTON_W ? key_width(c->label) + 40 : BUTTON_W;
        bx -= bw * k;
        button(u, c->label, bx, by, bw * k, k, i == u->focus, 0, pressing(u, i, -1));
        c->x0 = bx, c->y0 = by - 4 * k, c->x1 = bx + bw * k, c->y1 = by + 20 * k;
        if (i == u->focus)
            cursor(u, bx, by, k);
        bx -= GAP * k;
    }
#undef X
#undef Y
}

/* the control at x, y, and on a choice the option (else -1) */
static int hit(const Ui* u, float x, float y, int* opt)
{
    *opt = -1;
    for (int i = 0; i < u->nw; ++i)
        if (x >= u->w[i].x0 && x < u->w[i].x1 && y >= u->w[i].y0 && y < u->w[i].y1)
        {
            for (int o = 0; o < u->w[i].nopt; ++o)
                if (x >= u->w[i].ox0[o] && x < u->w[i].ox1[o])
                    *opt = o;
            if (u->w[i].kind != W_CHOICE || *opt >= 0)
                return i;
        }
    return -1;
}

static void key_act(Ui* u, const SDL_KeyboardEvent* e);

/* a key; the cursor's moving sounds as the game's does */
static void key(Ui* u, const SDL_KeyboardEvent* e)
{
    int focus = u->focus, sub = u->sub, screen = u->screen;
    key_act(u, e);
    if (u->screen == screen && (u->focus != focus || u->sub != sub))
        sound(u, SND_MOVE);
}

static void key_act(Ui* u, const SDL_KeyboardEvent* e)
{
    SDL_Keycode k = e->key;
    int shift = (e->mod & SDL_KMOD_SHIFT) != 0, cmd = (e->mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
    Widget* f = u->focus >= 0 && u->focus < u->nw ? &u->w[u->focus] : NULL;
    u->focus_time = SDL_GetTicks();
    /* a held key repeats: fine for moving and deleting, not for acting */
    if (e->repeat && (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE))
        return;
    if (k == SDLK_ESCAPE)
    {
        if (u->screen == SCREEN_SETTINGS)
            u->screen = SCREEN_SIGNIN, build(u), focus_on(u, 0), u->anim_at = SDL_GetTicks(), sound(u, SND_CANCEL);
        else if (!busy())
            begin_close(u, 0), sound(u, SND_CANCEL);
    }
    else if (k == SDLK_TAB || k == SDLK_DOWN || k == SDLK_UP)
    {
        int back = k == SDLK_UP || (k == SDLK_TAB && shift);
        focus_on(u, (u->focus + (back ? u->nw - 1 : 1)) % u->nw);
    }
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
    {
        /* in a field of the sign-in screen: sign in; in Settings' server: on to the next */
        if (f && (f->kind == W_TEXT || f->kind == W_SECRET) && u->screen == SCREEN_SIGNIN)
            sound(u, SND_CONFIRM), start_signin(u);
        else if (f && f->kind == W_TEXT)
            focus_on(u, (u->focus + 1) % u->nw);
        else
            activate(u, u->focus);
    }
    else if (f && f->kind == W_CHOICE && (k == SDLK_LEFT || k == SDLK_RIGHT))
    {
        /* the cursor along the row's buttons, as a Config page's */
        int to = u->sub + (k == SDLK_LEFT ? -1 : 1);
        u->sub = to < 0 ? 0 : to >= f->nopt ? f->nopt - 1 : to;
    }
    else if (f && f->kind == W_BUTTON && (k == SDLK_LEFT || k == SDLK_RIGHT))
    {
        /* along the buttons */
        int to = u->focus + (k == SDLK_LEFT ? -1 : 1);
        if (to >= 0 && to < u->nw && u->w[to].kind == W_BUTTON)
            focus_on(u, to);
    }
    else if (f && (f->kind == W_BUTTON || f->kind == W_CHOICE) && k == SDLK_SPACE)
        activate(u, u->focus);
    else if (f && f->text && k == SDLK_BACKSPACE && !busy())
    {
        size_t len = strlen(f->text);
        f->text[cmd ? 0 : len ? len - 1 : 0] = 0;
    }
    else if (f && f->text && k == SDLK_V && cmd)
    {
        char* clip = SDL_GetClipboardText();
        if (clip)
            type(u, clip);
        SDL_free(clip);
    }
}

int signin_run(const SigninSetup* setup, SigninResult* out)
{
    memset(out, 0, sizeof *out);
    Ui* u = calloc(1, sizeof *u);
    if (!u)
        return -1;
    u->host_game = setup->host_game;
    /* where it keeps its files */
    char dir[1024], cfg_path[1100];
    if (setup->data_dir)
        SDL_strlcpy(dir, setup->data_dir, sizeof dir);
    else
    {
        char* pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
        SDL_strlcpy(dir, pref ? pref : ".", sizeof dir);
        SDL_free(pref);
    }
    SDL_strlcpy(out->data_dir, dir, sizeof out->data_dir);
    config_path(dir, "signin.cfg", cfg_path, sizeof cfg_path);
    config_path(dir, "settings.reg", out->settings_reg, sizeof out->settings_reg);
    default_settings(out->settings_reg, setup);

    Config* c = &u->cfg;
    c->remember = 1, c->theme = 1;
    c->space = setup->default_space >= 0 ? setup->default_space != 0 : 1;
    SDL_strlcpy(c->server, setup->default_server ? setup->default_server : "127.0.0.1", sizeof c->server);
    config_load(cfg_path, c);
    if (setup->server)
        SDL_strlcpy(c->server, setup->server, sizeof c->server);
    if (setup->user)
        SDL_strlcpy(c->user, setup->user, sizeof c->user);
    if (setup->auth_port)
        c->auth_port = setup->auth_port;
    if (setup->data_port)
        c->data_port = setup->data_port;
    if (setup->view_port)
        c->view_port = setup->view_port;
    if (setup->loader_version)
        SDL_strlcpy(c->loader_version, setup->loader_version, sizeof c->loader_version);
    /* Full screen in a Space of its own (the Mac's way: it slides in, and Ctrl+arrows or a swipe
     * move between it and the other desktops) or in place over the desktop. SDL reads this when it
     * starts, for the whole run: the game's window (user32) keeps it. */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, c->space ? "1" : "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "[signin] SDL_Init: %s\n", SDL_GetError());
        free(u);
        return -1;
    }
    int read_keychain = 0;
    if (setup->password)
        SDL_strlcpy(u->password, setup->password, sizeof u->password);
    else if (c->remember && c->user[0])
    {
        keychain_key(c, g_keyread.key, sizeof g_keyread.key);
        read_keychain = 1; /* once the window is up */
    }
    if (setup->otp)
        SDL_strlcpy(u->otp, setup->otp, sizeof u->otp);

    /* the window the game will make of it (settings.reg's 0034 window mode, 0001 x 0002): full
     * screen (0) and borderless full screen (3) cover the display in its own mode, borderless (2)
     * has no frame, windowed (1) does - so the game takes it over without a jump */
    uint32_t mode = settings_dword(out->settings_reg, "0034", 1);
    int ww = (int)settings_dword(out->settings_reg, "0001", 1280), wh = (int)settings_dword(out->settings_reg, "0002", 720);
    if (ww < 640 || wh < 480)
        ww = 1280, wh = 720;
    int full = mode == 0 || mode == 3;
    /* a macOS Space is only for a window with a frame (full screen hides it) */
    SDL_Window* win = SDL_CreateWindow("FINAL FANTASY XI", ww, wh, mode >= 2 && !(full && c->space) ? SDL_WINDOW_BORDERLESS : 0);
    if (win && full)
    {
        SDL_SetWindowFullscreenMode(win, NULL);
        SDL_SetWindowFullscreen(win, true);
    }
    if (!win || !uidraw_open(win))
    {
        fprintf(stderr, "[signin] no window to draw in: %s\n", SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        free(u);
        return -1;
    }

    static const char* const FONT[] = { "moji", NULL };
    static const char* const MENU[] = { "gauge", NULL }, *const CURSOR[] = { "anc", NULL };
    DatImage moji;
    int ok = open_dat(u->host_game, 1, &u->fonts, "font", FONT);
    if (ok)
        uidraw_load(&u->menu.tex, &u->fonts.file, "menu", MENU), uidraw_load(&u->menu.tex, &u->fonts.file, "anc", CURSOR);
    ok = ok && load_theme(u);
    ok = ok && dat_image(&u->fonts.file, "font", "moji", &moji);
    /* the PC client's title art, when its lobby DAT is there (the English one; others have it too) */
    static const char* const PC[] = { "titlwin", "b1n", NULL }, *const KEYTOP_IMG[] = { "keytophd", NULL };
    if (ok && open_dat_at(u->host_game, "ROM/119/50.DAT", &u->pc, "menu", PC))
    {
        /* the lobby's sheet as it was: its backdrop (85) with the key art, the notice's shading (95) */
        u->have_art = u->pc.tex.n == 2 && dat_sheet(&u->pc.file, "lobbywin", &u->lobbywin) && u->lobbywin.nsprites > 95 &&
                      u->lobbywin.sprites[85].nparts >= 3 && !strcmp(u->lobbywin.sprites[85].parts[2].name, "titlwin") &&
                      u->lobbywin.sprites[95].nparts >= 2;
        if (!u->have_art)
            fprintf(stderr, "[signin] ROM/119/50.DAT is not the lobby it was: no backdrop or key art\n");
    }
    /* the buttons' letters */
    if (ok && !open_dat_at(u->host_game, "ROM/118/111.DAT", &u->keytop, "menu", KEYTOP_IMG))
        fprintf(stderr, "[signin] no keytopHD: the buttons have no words\n");
    if (ok)
    {
        ok = ui_font(&moji, &u->font);
        /* the glyphs' cores: the font's white (173) without its baked dark outline (under 60), as
         * the alpha of white, so it takes any colour cleanly */
        for (size_t i = 0; i < (size_t)moji.w * moji.h; ++i)
        {
            uint8_t* p = moji.rgba + i * 4;
            int a = (p[0] - 60) * 255 / (173 - 60);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            p[3] = (uint8_t)(a * p[3] / 255);
            p[0] = p[1] = p[2] = 255;
        }
        u->ink = u->font;
        u->ink.image = "mojiink";
        uidraw_load_rgba(&u->fonts.tex, "mojiink", moji.rgba, moji.w, moji.h);
        dat_image_free(&moji);
        hires_font(&u->fonts.tex, dir);
        hires_gauge(&u->menu.tex, dir);
    }
    if (!ok)
    {
        fprintf(stderr, "[signin] the install's UI art did not load (is --game the FINAL FANTASY XI folder?)\n");
        uidraw_close();
        SDL_DestroyWindow(win);
        free(u);
        return -1;
    }

    /* start where there is something to type */
    build(u);
    focus_on(u, !c->user[0] ? 0 : !u->password[0] ? 1 : 3);
    SDL_StartTextInput(win);
    if (read_keychain)
    {
        SDL_SetAtomicInt(&g_keyread.state, 1);
        if (!plat_thread_start(keyread_thread, &g_keyread))
            SDL_SetAtomicInt(&g_keyread.state, 0);
    }
    /* The volume the game has before a character's own (cnf.dat's) is loaded: USER/tig.dat's
     * setting 0xa8 (little-endian: a version, then that, 0..100), 100 without it */
    {
        char tig[1100];
        if (setup->user_dir)
            config_path(setup->user_dir, "tig.dat", tig, sizeof tig);
        else
            snprintf(tig, sizeof tig, "%s/USER/tig.dat", u->host_game);
        size_t size = 0;
        unsigned char* t = plat_read_file(tig, &size);
        int vol = t && size >= 8 ? (int)(t[4] | t[5] << 8 | t[6] << 16 | (unsigned)t[7] << 24) : 100;
        free(t);
        u->se_gain = se_menu_gain(vol);
        fprintf(stderr, "[signin] sound effects at %d of 100 (%s)\n", vol, tig);
    }
    if ((u->se = se_open()))
        for (int i = 0; i < 4; ++i)
            if (!sewave_load(u->host_game, (uint32_t)i + 1, &u->snd[i]))
                fprintf(stderr, "[signin] no sound effect %d in the install\n", i + 1);
    u->opened = u->anim_at = u->focus_time = SDL_GetTicks();
    u->press_w = -1;
    int result = 0;
    for (int done = 0; !done;)
    {
        build(u);
        discord_signin_frame();
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = 1;
            else if (u->closing || open_progress(u) < 1)
                continue; /* nothing to act on while the window opens or closes */
            else if (e.type == SDL_EVENT_TEXT_INPUT)
                type(u, e.text.text);
            else if (e.type == SDL_EVENT_KEY_DOWN)
                key(u, &e.key);
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
            {
                float d = SDL_GetWindowPixelDensity(win);
                int o, i = hit(u, e.button.x * d, e.button.y * d, &o);
                if (i >= 0)
                {
                    if (i != u->focus && u->w[i].kind != W_BUTTON && u->w[i].kind != W_CHOICE)
                        sound(u, SND_MOVE); /* into a field; a button's is its press */
                    focus_on(u, i);
                    if (o >= 0)
                        u->sub = o;
                    if (u->w[i].kind == W_BUTTON || u->w[i].kind == W_CHOICE)
                        activate(u, i);
                }
            }
            build(u);
        }

        /* the saved password, when macOS has let us read it: into an empty field */
        if (SDL_GetAtomicInt(&g_keyread.state) == 2)
        {
            if (g_keyread.found && !u->password[0])
            {
                SDL_strlcpy(u->password, g_keyread.password, sizeof u->password);
                if (u->screen == SCREEN_SIGNIN && u->focus == 1)
                    u->focus = 2; /* on to the code */
            }
            memset(g_keyread.password, 0, sizeof g_keyread.password);
            SDL_SetAtomicInt(&g_keyread.state, 0);
            if (!u->status_error)
                set_status(u, "", 0);
        }
        int state = SDL_GetAtomicInt(&g_job.state);
        if (state == JOB_FAILED)
        {
            SDL_SetAtomicInt(&g_job.state, JOB_IDLE);
            set_status(u, g_job.error[0] ? g_job.error : "The sign-in failed.", 1);
            u->otp[0] = 0; /* one-time codes are single use */
        }
        else if (state == JOB_DONE)
        {
            /* remembered (or forgotten) off this thread: macOS may ask first */
            Keysave* ks = calloc(1, sizeof *ks);
            if (ks)
            {
                keychain_key(c, ks->key, sizeof ks->key);
                SDL_strlcpy(ks->password, u->password, sizeof ks->password);
                ks->remember = c->remember;
                if (!plat_thread_start(keysave_thread, ks))
                    free(ks);
            }
            /* the loader version the server asked for, sent from the start next time */
            if (g_job.version_used[0])
                SDL_strlcpy(c->loader_version, g_job.version_used, sizeof c->loader_version);
            out->server = g_job.ip;
            SDL_SetAtomicInt(&g_job.state, JOB_IDLE);
            begin_close(u, 1);
        }
        if (u->closing && open_progress(u) <= 0)
            result = u->close_result, done = 1;

        int w, h;
        uidraw_begin(0xFF000000u, &w, &h);
        draw(u, w, h);
        uidraw_end();
    }
    config_save(cfg_path, c);

    SDL_StopTextInput(win);
    se_close(u->se);
    for (int i = 0; i < 4; ++i)
        sewave_free(&u->snd[i]);
    memset(u->password, 0, sizeof u->password);
    dat_sheet_free(&u->lobbywin);
    close_dat(&u->pc);
    close_dat(&u->keytop);
    if (u->theme_loaded)
    {
        dat_sheet_free(&u->win00);
        close_dat(&u->frame);
    }
    uidraw_free(&u->menu.tex);
    close_dat(&u->fonts);
    uidraw_close();
    free(u);
    if (!result)
    {
        SDL_DestroyWindow(win);
        return 0;
    }
    out->window = win;
    return 1;
}
