/* Config > Modern. See modern.h.
 *
 * FFXiMain's menus (2026-09-03) are layouts from the menu DAT (English: ROM/119/51.DAT, chunk type
 * 0x30) that its menu manager (0x10621838) opens by name, each run by a handler object named in a
 * static table of menus (0x103722d8: name, a pointer to the global holding the handler, flags).
 * What a menu shows is sprites (chunk type 0x31 sheets: quads of the game's textures): a Config
 * page is one panel sprite - its background, title strip, bullets and captions, the words made of
 * the menu font's glyphs - and a sprite per button; the page's handler draws the rest each frame
 * (the red underline under the current choice, a slider's fill) with sprites of its own. The game
 * draws and animates the window, moves the pointing hand, plays the sounds and closes it on cancel.
 *
 * Here, with no change to the game's code, all of that is made the same way:
 *   - A sprite sheet of our own, "menu    modernps", from the game's textures: the page's panel,
 *     captions and buttons, the Config list's label, underlines and a slider's fill. Registered
 *     with the game's sheets.
 *   - The Config list's layout, rebuilt from the DAT with our item after its last and registered
 *     in place of the original, which is renamed out of the way.
 *   - The page's layout: buttons and sliders over its panel.
 *   - The menu table has no free entry, but has one the game never finds - the second of two
 *     "conf1win" entries - and room for one more in its end marker. Each becomes a page's,
 *     pointing at a global of our own.
 *   - The page's handler is a Config page (the game's base class, ctor 0x10196980) whose vtable
 *     points at shims here; the Config list's handler gets a copy of its vtable whose select slot
 *     comes here first. (The pages are a table: another is a row in PAGES and a spare entry.)
 * Config > Menus hides items of the game's own menus the same way: the combat menu's lists of
 * actions without Trust, and Magic's, Status's and Abilities' layouts, rebuilt with the items
 * hidden empty.
 * What is there is checked before anything is written: another build leaves the menus as they are. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "build.h"
#include "d3d8.h"
#include "datui.h"
#include "gfx.h"
#include "gthread.h"
#include "gwin.h"
#include "modern.h"
#include "thunk.h"
#include "user32.h"
#include "vfs.h"

/* ---- the game ----
 * Its addresses are the build's (meta/builds.json "modern", generated/build.h; 2026-09-03's in the
 * comments here). A build without them all has FFXI_MODERN 0: then nothing here calls the game. */
enum
{
    MGR = FFXI_MODERN_MGR,                 /* 0x10621838 the menu manager */
    LAYOUT_FIND = FFXI_MODERN_LAYOUT_FIND, /* 0x1015e030 thiscall mgr (const char name[16]): the layout, or 0 */
    LAYOUT_ADD = FFXI_MODERN_LAYOUT_ADD,   /* 0x1015df80 thiscall mgr (const void** payload): parses and registers it */
    SHEETS = FFXI_MODERN_SHEETS,           /* 0x104e1bf8 the sprite sheets */
    SHEET_ADD = FFXI_MODERN_SHEET_ADD,     /* 0x10120290 thiscall SHEETS (const void** payload) */
    SHEET_FIND = FFXI_MODERN_SHEET_FIND,   /* 0x10120450 thiscall SHEETS (const char name[16]): the sheet; [sheet] its sprites */
    MENU_OPEN = FFXI_MODERN_MENU_OPEN,     /* 0x1015e350 thiscall mgr (const char* name, bool, bool) */
    PAGE_CTOR = FFXI_MODERN_PAGE_CTOR,     /* 0x10196980 thiscall: a Config page */
    PAGE_VTBL = FFXI_MODERN_PAGE_VTBL,     /* 0x10336a38 */
    PAGE_MARK = FFXI_MODERN_PAGE_MARK,     /* 0x10196a20 thiscall page (v, a, b): sprite +0x14 at item (v ? b : a), +0x25, +4 */
    PAGE_FILL = FFXI_MODERN_PAGE_FILL,     /* 0x10196b10 thiscall page (item, float, colour): sprite +0x1c across item, scaled */
    CONFIG_INST = FFXI_MODERN_CONFIG_INST, /* 0x10662718 the global holding the Config list's handler */
    CONFIG_VTBL = FFXI_MODERN_CONFIG_VTBL, /* 0x10337798 */
    WINDOW_PART = FFXI_MODERN_WINDOW_PART, /* 0x101181d0 thiscall window (short item): its part */
    LOBBY_INST = FFXI_MODERN_LOBBY_INST,   /* 0x10662784 the global holding the lobby's Config window's handler */
    LOBBY_VTBL = FFXI_MODERN_LOBBY_VTBL,   /* 0x10337140 */
    LOBBY_INPUT = FFXI_MODERN_LOBBY_INPUT, /* 0x1019ddd0 its slot 6 */
    LOBBY_HELP = FFXI_MODERN_LOBBY_HELP,   /* 0x10669078 the global holding the lobby's help line ("lobyhelp"): +0x2c its text's index */
    /* the display as the game keeps it, read from the registry once at its start */
    DISP_WIN_W = FFXI_MODERN_DISP_WIN_W,   /* 0x10456a58 0001: the window's (back buffer's) width */
    DISP_WIN_H = FFXI_MODERN_DISP_WIN_H,   /* 0x10456c08 0002 */
    DISP_MODE = FFXI_MODERN_DISP_MODE,     /* 0x10456a7c 0034: the window mode */
    DISP_GFX = FFXI_MODERN_DISP_GFX,       /* 0x1045666c the global holding its graphics object: +0x15c/+0x160 the back
                                            * buffer's size, +0x98 a stack of viewports (24 bytes each, depth at
                                            * +0x158), the screen's first; +0x1e0 the menu target (0: none) */
    DISP_VIEWPORT = FFXI_MODERN_DISP_VIEWPORT, /* 0x10009d20 thiscall gfx (): the top viewport the whole back buffer, set */
    DISP_SCENE = FFXI_MODERN_DISP_SCENE,   /* 0x104568fc the global holding its scene: +0x10/+0x12 (16 bits) the back
                                            * buffer's size, +0x14/+0x16 the menus' */
    DISP_DEVICE = FFXI_MODERN_DISP_DEVICE, /* 0x103d3cbc the global holding its device: +0xc/+0x10 the present
                                            * parameters' size, +0x28 windowed */
    DISP_MENU_W = FFXI_MODERN_DISP_MENU_W, /* 0x10456ac8 0037: the menus' width */
    DISP_MENU_H = FFXI_MODERN_DISP_MENU_H, /* 0x10456a90 0038 */
    DISP_RT = FFXI_MODERN_DISP_RT,         /* 0x1000a6e0 thiscall gfx (w, h, void** out, usage, pool): a render-target
                                            * texture, as the menu target is made at its start */
    DISP_DEPTH = FFXI_MODERN_DISP_DEPTH,   /* 0x1000ad70 thiscall gfx (w, h): a depth surface */
    WIN_SETPOS = FFXI_MODERN_WIN_SETPOS,   /* 0x10119110 thiscall window (x, y): moved there, its parts with it,
                                            * its handler told (OnMove) */
    LOG_INST = FFXI_MODERN_LOG_INST,       /* 0x1062193c the global holding the chat log's handler ("logwindo") */
    LOG2_INST = FFXI_MODERN_LOG2_INST,     /* 0x10621940 ... the second log's ("logwin2") */
    LOG_FIT = FFXI_MODERN_LOG_FIT,         /* 0x10162d90 thiscall log handler (): its window to its width */
    CONFIG_GET = FFXI_MODERN_CONFIG_GET,   /* 0x10193850 cdecl (id): a setting of the game's Config */
    HELP_INST = FFXI_MODERN_HELP_INST,     /* 0x105781f8 the global holding the help line's handler ("helpwind") */
    HELP_SETRECT = FFXI_MODERN_WIN_SETRECT, /* 0x1011a5a0 thiscall window (x, y, w, h, 1, 0, 0): its frame there, at once */
    VTBL_SLOTS = 17,
    CONFIG_ITEMS = 13,          /* the Config list's own */
    EV_DOWN = 1, EV_UP = 2, EV_RIGHT = 3, EV_LEFT = 4, EV_SELECT = 5, /* a menu's input events (OnInput); the game's own sliders go up on 3 */
};

static const char CONFIG_NAME[] = "menu    configwi", SHEET_NAME[] = "menu    modernps",
                  RENAMED[] = "menu    configw_";

/* ---- what the pages set ---- */

enum
{
    TOGGLE, /* ON / OFF */
    CHOICE, /* a button an option */
    SLIDER, /* Min to Max */
    LIST,   /* one of a list (List): a slider along it, its value's words after Max */
};

typedef struct Row
{
    const char* label;
    const char* key; /* the settings file's; "@..." host64's */
    int kind;
    int nopt;
    const char* opt[4];
    float val[4];
    float lo, hi; /* a slider's range */
    const char* help;
} Row;

/* the game's items the Menus page can hide: a bit each in g_hide */
enum
{
    HIDE_TRUST,          /* the combat menu's and Magic's */
    HIDE_MAGIC_GEOMANCY,
    HIDE_OLD_MAGIC_TRUST, /* was Magic's alone: read as HIDE_TRUST */
    HIDE_MASTER_LEVELS,
    HIDE_UNITY,
    HIDE_JOB_POINTS,
    HIDE_ALTER_EGO,
    HIDE_MOUNTS, /* Abilities' Mount, the combat menu's and the main menu's */
    NHIDE,
};

static ModernSetup g_setup;
static char g_data_dir[1024], g_game[1024];
static float g_ui_aspect;
static unsigned g_hide;
static void hide_apply(void);
static int g_fx_touched, g_host_touched;
static char g_fx_keys[32][24]; /* the scene settings changed, to write */
static int g_nfx_keys;

/* ---- the display: the game's own settings (settings.reg), read when it starts ----
 * The window's size (0001 x 0002), the background's (0003 x 0004, square) and the menus' (0037 x
 * 0038). The menus' size is the window's over the UI scale, in the interface's shape when it has
 * one. Written to the settings file as they change; the game takes them when it next starts. */
enum
{
    MAX_LIST = 48,
};

typedef struct List
{
    const char* key;
    int n;
    char text[MAX_LIST][16];
    int w[MAX_LIST], h[MAX_LIST]; /* @res: a size */
    float v[MAX_LIST];            /* @menu: a scale */
    int spr[MAX_LIST];            /* its value's sprite (list_sprites) */
} List;

static List g_res = { "@res" }, g_scale = { "@menu" };
static char g_settings_reg[1100];
static int g_win_w, g_win_h, g_bg, g_menu_w, g_menu_h, g_mode; /* as the settings file has them; g_mode 0034 */
static int g_display_live; /* the window's mode or size changed: display_apply, between frames */

static List* list_of(const char* key) { return !strcmp(key, "@res") ? &g_res : !strcmp(key, "@menu") ? &g_scale : NULL; }

/* the menus' size for a window and a scale: w x h over it, as wide as the interface's shape */
static void menu_size(int ww, int wh, float scale, int* mw, int* mh)
{
    int h = (int)lroundf((float)wh / scale), w = (int)lroundf((float)ww / scale);
    if (g_ui_aspect > 0.0f && (int)lroundf((float)h * g_ui_aspect) < w)
        w = (int)lroundf((float)h * g_ui_aspect);
    *mw = (w + 1) & ~1, *mh = (h + 1) & ~1;
}

/* the list's entry nearest what the settings file has */
static int list_at(const List* l)
{
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < l->n; ++i)
    {
        float d = l == &g_res ? (float)(abs(l->w[i] - g_win_w) + abs(l->h[i] - g_win_h))
                              : fabsf(l->v[i] - (g_menu_h > 0 ? (float)g_win_h / (float)g_menu_h : 1.0f));
        if (d < bd)
            bd = d, best = i;
    }
    return best;
}

static void list_set(List* l, int i)
{
    if (i < 0 || i >= l->n)
        return;
    /* the scale as it is (a step of its list), before the window changes it */
    float scale = g_scale.n ? g_scale.v[list_at(&g_scale)] : 1.0f;
    if (l == &g_res)
        g_win_w = l->w[i], g_win_h = l->h[i];
    else
        scale = l->v[i];
    menu_size(g_win_w, g_win_h, scale, &g_menu_w, &g_menu_h);
}

static float get(const Row* r)
{
    const List* l = list_of(r->key);
    if (l)
        return (float)list_at(l);
    if (!strcmp(r->key, "@bg"))
        return (float)g_bg;
    if (!strcmp(r->key, "@mode"))
        return (float)g_mode;
    if (!strcmp(r->key, "@fps"))
        return g_setup.fps_divisor ? (float)*g_setup.fps_divisor : 1.0f;
    if (!strcmp(r->key, "@ui"))
        return g_ui_aspect;
    if (!strncmp(r->key, "@hide", 5))
        return g_hide >> atoi(r->key + 5) & 1 ? 0.0f : 1.0f;
    return gfx_fx_get(r->key);
}

static void save_display(void);

static void set(const Row* r, float v)
{
    List* l = list_of(r->key);
    if (l || !strcmp(r->key, "@bg") || !strcmp(r->key, "@mode"))
    {
        if (l)
            list_set(l, (int)v);
        else if (!strcmp(r->key, "@bg"))
            g_bg = (int)v;
        else
            g_mode = (int)v;
        g_display_live |= l || !strcmp(r->key, "@mode"); /* at once (display_apply) */
        /* written at once: a window closed other than by its close (the lobby's) still keeps it */
        save_display();
    }
    else if (!strcmp(r->key, "@fps"))
    {
        if (g_setup.fps_divisor)
            *g_setup.fps_divisor = (uint32_t)v;
        g_host_touched = 1;
    }
    else if (!strcmp(r->key, "@ui"))
    {
        g_ui_aspect = v;
        user32_set_ui_aspect(v);
        g_host_touched = 1;
    }
    else if (!strncmp(r->key, "@hide", 5))
    {
        unsigned bit = 1u << atoi(r->key + 5);
        g_hide = v != 0.0f ? g_hide & ~bit : g_hide | bit;
        g_host_touched = 1;
        hide_apply();
    }
    else
    {
        gfx_fx_set(r->key, v);
        int known = 0;
        for (int i = 0; i < g_nfx_keys; ++i)
            known |= !strcmp(g_fx_keys[i], r->key);
        if (!known && g_nfx_keys < 32)
            SDL_strlcpy(g_fx_keys[g_nfx_keys++], r->key, sizeof g_fx_keys[0]);
        g_fx_touched = 1;
    }
}

/* the option the setting is at; -1 when it is none of them */
static int option(const Row* r)
{
    float v = get(r);
    for (int i = 0; i < r->nopt; ++i)
        if (fabsf(v - r->val[i]) <= 1e-3f * (fabsf(r->val[i]) > 1 ? fabsf(r->val[i]) : 1))
            return i;
    return -1;
}

enum
{
    SLIDER_STEPS = 20,
};

/* a slider's place, 0..1 (below its range - draw distance's 0, "as the command line says" - is 0) */
static float place(const Row* r)
{
    float t = (get(r) - r->lo) / (r->hi - r->lo);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

static void slide(const Row* r, int dir)
{
    float t = roundf(place(r) * SLIDER_STEPS) + (float)dir;
    t = t < 0 ? 0 : t > SLIDER_STEPS ? SLIDER_STEPS : t;
    set(r, r->lo + (r->hi - r->lo) * t / SLIDER_STEPS);
}

static const Row MODERN_ROWS[] = {
    { "Modern Effects", "fx", TOGGLE, 2, { "ON", "OFF" }, { 1, 0 }, 0, 0,
        "Ambient occlusion, fog, light, shadows and the rest below." },
    { "Ambient Occlusion", "ao", SLIDER, 0, { 0 }, { 0 }, 0, 1.5f, "Soft shade where surfaces meet." },
    { "Fog", "fog", SLIDER, 0, { 0 }, { 0 }, 0, 0.02f, "Haze over distance, lit by the sun." },
    { "God Rays", "rays", SLIDER, 0, { 0 }, { 0 }, 0, 1.5f, "Shafts of light from the sun." },
    { "Bloom", "bloom", SLIDER, 0, { 0 }, { 0 }, 0, 2.0f, "A glow around bright lights." },
    { "Color Grading", "grade", SLIDER, 0, { 0 }, { 0 }, 0, 1.0f, "Richer color and contrast." },
    { "Sun Shadows", "sun", SLIDER, 0, { 0 }, { 0 }, 0, 1.0f, "Characters cast shadows from the sun." },
    { "Per-Pixel Lighting", "light", CHOICE, 3, { "Off", "Sun", "All" }, { 0, 1, 2 }, 0, 0,
        "Smooth light across surfaces: the sun's, or every light's." },
    { "Sharpening", "sharpen", SLIDER, 0, { 0 }, { 0 }, 0, 1.0f, "Crisper detail over the whole screen." },
    { "Anti-Shimmer", "filter", TOGGLE, 2, { "ON", "OFF" }, { 1, 0 }, 0, 0, "Steadies fine detail in motion." },
    { "Draw Distance", "draw", SLIDER, 0, { 0 }, { 0 }, 1, 6, "How far out the world is drawn." },
    { "Character Distance", "draw_entities", SLIDER, 0, { 0 }, { 0 }, 1, 4, "How far out characters are drawn." },
    { "Frame Rate", "@fps", CHOICE, 2, { "30 fps", "60 fps" }, { 2, 1 }, 0, 0, "The game's frame rate." },
    { "Interface Shape", "@ui", CHOICE, 3, { "Full", "16:9", "4:3" }, { 0, 16.0f / 9.0f, 4.0f / 3.0f }, 0, 0,
        "Keeps the menus in a box of this shape on a wide screen." },
};

static const Row DISPLAY_ROWS[] = {
    /* 0034: 1 a window, 3 borderless over the whole screen, 0 full screen (2, a borderless window, reads as none) */
    { "Window Mode", "@mode", CHOICE, 3, { "Windowed", "Borderless", "Full" }, { 1, 3, 0 }, 0, 0,
        "A window, borderless over the whole screen, or full screen." },
    { "Resolution", "@res", LIST, 0, { 0 }, { 0 }, 0, 0,
        "The window's size." },
    { "UI Scale", "@menu", LIST, 0, { 0 }, { 0 }, 0, 0,
        "How large the menus and text are drawn." },
    { "Background Resolution", "@bg", CHOICE, 3, { "4096", "6144", "8192" }, { 4096, 6144, 8192 }, 0, 0,
        "The size the world is drawn at before it fits the window. From the next start." },
    { "Anti-Aliasing", "aa", CHOICE, 2, { "Off", "FXAA" }, { 0, 1 }, 0, 0, "Smooths the world's jagged edges." },
    { "Texture Filtering", "aniso", CHOICE, 4, { "Off", "4x", "8x", "16x" }, { 1, 4, 8, 16 }, 0, 0,
        "Sharper ground and walls at an angle." },
    { "FPS Counter", "fps", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "The frame rate, in the screen's top left corner." },
};

static const Row MENUS_ROWS[] = {
    { "Trust", "@hide0", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Trust in the combat menu and in the main menu's Magic." },
    { "Mounts", "@hide7", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Mount in Abilities, the combat menu's and the main menu's." },
    { "Geomancy (Magic)", "@hide1", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Geomancy in the main menu's Magic." },
    { "Master Levels", "@hide3", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Master Levels in the main menu's Status." },
    { "Unity", "@hide4", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0, "Unity in the main menu's Status." },
    { "Job Points", "@hide5", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Job Points in the main menu's Status." },
    { "Alter Ego Points", "@hide6", TOGGLE, 2, { "Show", "Hide" }, { 1, 0 }, 0, 0,
        "Alter Ego Points in the main menu's Status." },
};

/* ---- remembering ---- */

static void join(const char* dir, const char* name, char* out, size_t n)
{
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/", name);
}

/* the scene effects' settings file, as gfx_metal.m finds it */
static int fx_file(char* out, size_t n)
{
    const char* file = getenv("FFXI_FX_FILE");
    if (file && *file)
        return snprintf(out, n, "%s", file), 1;
    if (getenv("HOME"))
        return snprintf(out, n, "%s/Library/Caches/FFXI/fx.txt", getenv("HOME")), 1;
    return 0;
}

/* The scene settings changed into the settings file: a line already there for a key is rewritten
 * in place, the rest of the file (other keys, notes) kept; keys it lacked go at the end. */
static void save_fx(void)
{
    char path[1100], tmp[1200];
    if (!fx_file(path, sizeof path))
        return;
    char* dir = SDL_strdup(path);
    char* slash = strrchr(dir, '/');
    if (slash)
        *slash = 0, SDL_CreateDirectory(dir);
    SDL_free(dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE* in = fopen(path, "r");
    FILE* out = fopen(tmp, "w");
    if (!out)
    {
        if (in)
            fclose(in);
        fprintf(stderr, "[modern] cannot write %s\n", tmp);
        return;
    }
    int written[32] = { 0 };
    char line[512], key[64];
    float v;
    while (in && fgets(line, sizeof line, in))
    {
        int k = -1;
        if (sscanf(line, " %63[a-z_] = %f", key, &v) == 2)
            for (int i = 0; i < g_nfx_keys; ++i)
                if (!strcmp(g_fx_keys[i], key))
                    k = i;
        if (k < 0)
            fputs(line, out);
        else if (!written[k])
        {
            fprintf(out, "%s=%g\n", key, (double)gfx_fx_get(key));
            written[k] = 1;
        }
    }
    if (in)
        fclose(in);
    for (int i = 0; i < g_nfx_keys; ++i)
        if (!written[i])
            fprintf(out, "%s=%g\n", g_fx_keys[i], (double)gfx_fx_get(g_fx_keys[i]));
    if (fclose(out) || rename(tmp, path))
        fprintf(stderr, "[modern] cannot replace %s\n", path);
}

/* The display's values into the settings file: its lines for them rewritten in place, the rest
 * kept, any it lacked added */
static void save_display(void)
{
    static const char* const NAMES[7] = { "0001", "0002", "0003", "0004", "0037", "0038", "0034" };
    const int v[7] = { g_win_w, g_win_h, g_bg, g_bg, g_menu_w, g_menu_h, g_mode };
    char tmp[1200], line[512];
    if (!g_settings_reg[0])
        return;
    snprintf(tmp, sizeof tmp, "%s.new", g_settings_reg);
    FILE* in = fopen(g_settings_reg, "r");
    FILE* out = fopen(tmp, "w");
    if (!out)
    {
        if (in)
            fclose(in);
        fprintf(stderr, "[modern] cannot write %s\n", tmp);
        return;
    }
    int written[7] = { 0 }, crlf = 1, any = 0;
    while (in && fgets(line, sizeof line, in))
    {
        size_t n = strlen(line);
        if (!any)
            crlf = n >= 2 && line[n - 2] == '\r', any = 1;
        int k = -1;
        for (int i = 0; i < 7; ++i)
            if (line[0] == '"' && !strncmp(line + 1, NAMES[i], 4) && line[5] == '"')
                k = i;
        if (k < 0)
            fputs(line, out);
        else if (!written[k])
        {
            fprintf(out, "\"%s\"=dword:%08x%s", NAMES[k], (unsigned)v[k], crlf ? "\r\n" : "\n");
            written[k] = 1;
        }
    }
    if (in)
        fclose(in);
    else
        fprintf(out, "REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
    for (int i = 0; i < 7; ++i)
        if (!written[i])
            fprintf(out, "\"%s\"=dword:%08x%s", NAMES[i], (unsigned)v[i], crlf ? "\r\n" : "\n");
    if (fclose(out) || rename(tmp, g_settings_reg))
        fprintf(stderr, "[modern] cannot replace %s\n", g_settings_reg);
    else
        fprintf(stderr, "[modern] display from the next start: mode %d, %dx%d, menus %dx%d, background %d\n", g_mode,
            g_win_w, g_win_h, g_menu_w, g_menu_h, g_bg);
}

static void save(void)
{
    if (g_fx_touched)
        save_fx();
    g_fx_touched = 0, g_nfx_keys = 0;
    if (g_host_touched && g_data_dir[0])
    {
        char path[1100];
        join(g_data_dir, "modern.cfg", path, sizeof path);
        FILE* f = fopen(path, "w");
        if (f)
        {
            fprintf(f, "fps_divisor=%u\nui_aspect=%g\nhide=%u\n", g_setup.fps_divisor ? *g_setup.fps_divisor : 1u,
                (double)g_ui_aspect, g_hide);
            fclose(f);
        }
        else
            fprintf(stderr, "[modern] cannot write %s\n", path);
    }
    g_host_touched = 0;
}

/* a key=value file's value for key, or NULL */
static const char* cfg_value(const char* path, const char* key, char* buf, size_t n)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return NULL;
    const char* found = NULL;
    char line[512];
    size_t klen = strlen(key);
    while (!found && fgets(line, sizeof line, f))
        if (!strncmp(line, key, klen) && line[klen] == '=')
        {
            line[strcspn(line, "\r\n")] = 0;
            SDL_strlcpy(buf, line + klen + 1, n);
            found = buf;
        }
    fclose(f);
    return found;
}

/* a DWORD of the settings file ("name"=dword:hex), or dflt */
static int settings_dword(const char* name, int dflt)
{
    FILE* f = g_settings_reg[0] ? fopen(g_settings_reg, "r") : NULL;
    if (!f)
        return dflt;
    char line[256], want[16];
    snprintf(want, sizeof want, "\"%s\"=dword:", name);
    int v = dflt;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, strlen(want)))
            v = (int)strtoul(line + strlen(want), NULL, 16);
    fclose(f);
    return v;
}

static void res_add(int w, int h)
{
    if (w < 640 || h < 480)
        return;
    for (int i = 0; i < g_res.n; ++i)
        if (g_res.w[i] == w && g_res.h[i] == h)
            return;
    if (g_res.n == MAX_LIST)
        return;
    int at = g_res.n++;
    /* in order: narrowest first, then shortest */
    while (at > 0 && (g_res.w[at - 1] > w || (g_res.w[at - 1] == w && g_res.h[at - 1] > h)))
    {
        g_res.w[at] = g_res.w[at - 1], g_res.h[at] = g_res.h[at - 1];
        memcpy(g_res.text[at], g_res.text[at - 1], sizeof g_res.text[0]);
        --at;
    }
    g_res.w[at] = w, g_res.h[at] = h;
    snprintf(g_res.text[at], sizeof g_res.text[0], "%dx%d", w, h);
}

/* The sizes the window can be: the display's modes (the one the game's window is on), and the
 * size the settings file has; a few common ones when SDL knows of no display yet */
static void lists_fill(void)
{
    if (!g_scale.n)
        for (int i = 0; i <= 10; ++i)
        {
            g_scale.v[g_scale.n] = 0.5f + 0.25f * (float)i;
            snprintf(g_scale.text[g_scale.n++], sizeof g_scale.text[0], "%gx", (double)(0.5f + 0.25f * (float)i));
        }
    if (g_res.n)
        return;
    int nwin = 0, nmodes = 0;
    SDL_DisplayID display = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO))
    {
        SDL_Window** wins = SDL_GetWindows(&nwin);
        display = wins && nwin > 0 ? SDL_GetDisplayForWindow(wins[0]) : 0;
        SDL_free(wins);
        if (!display)
            display = SDL_GetPrimaryDisplay();
    }
    SDL_DisplayMode** modes = display ? SDL_GetFullscreenDisplayModes(display, &nmodes) : NULL;
    for (int i = 0; modes && i < nmodes; ++i)
        res_add(modes[i]->w, modes[i]->h);
    SDL_free(modes);
    const SDL_DisplayMode* desk = display ? SDL_GetDesktopDisplayMode(display) : NULL;
    if (desk)
        res_add(desk->w, desk->h);
    if (g_res.n < 2)
    {
        static const int COMMON[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 },
            { 3840, 2160 } };
        for (size_t i = 0; i < sizeof COMMON / sizeof COMMON[0]; ++i)
            res_add(COMMON[i][0], COMMON[i][1]);
    }
    res_add(g_win_w, g_win_h);
    fprintf(stderr, "[modern] display: %dx%d, menus %dx%d, background %d (%s); %d sizes, at %s; scale at %s\n", g_win_w, g_win_h,
        g_menu_w, g_menu_h, g_bg, g_settings_reg, g_res.n, g_res.text[list_at(&g_res)], g_scale.text[list_at(&g_scale)]);
}

/* ---- guest memory ---- */

static uint32_t gbytes(const void* p, uint32_t n)
{
    uint32_t a = gheap_alloc(n, 1);
    if (a)
        memcpy(GUEST_PTR(a), p, n);
    return a;
}

static uint32_t gstr(const char* s) { return gbytes(s, (uint32_t)strlen(s) + 1); }

static int guest_is(uint32_t addr, const void* bytes, size_t n) { return !memcmp(GUEST_PTR(addr), bytes, n); }

/* ---- the layouts ---- */

/* The payload of the chunk of a type (0x30 a layout, 0x31 a sprite sheet) called name in a DAT of
 * the install (as "ROM\\119\\51.DAT"): a copy, or NULL. */
static uint8_t* dat_chunk(const char* file, int type, const char* name, size_t* size)
{
    /* the game's path for it, then the file it opens: a DAT overlay's, else the install's */
    char guest[1200], path[1200];
    snprintf(guest, sizeof guest, "%s\\%s", g_game, file);
    if (!vfs_overlay_path(guest, path, sizeof path) && !vfs_host_path(guest, path, sizeof path))
        return NULL;
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n)
        free(d), d = NULL;
    fclose(f);
    uint8_t* out = NULL;
    for (long o = 0; d && o + 16 <= n;)
    {
        uint32_t info = (uint32_t)d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
        long len = (long)((info >> 7) & 0x7FFFF) * 16;
        if (len < 16 || o + len > n)
            break;
        if ((int)(info & 0x7F) == type && len >= 48 && !memcmp(d + o + 16, name, 16))
        {
            *size = (size_t)len - 16;
            out = malloc(*size);
            if (out)
                memcpy(out, d + o + 16, *size);
            break;
        }
        o += len;
    }
    free(d);
    return out;
}

/* the layout called name in the menu DAT (the lobby's: 50.DAT) */
static uint8_t* dat_layout_in(const char* file, const char* name, size_t* size) { return dat_chunk(file, 0x30, name, size); }

static uint8_t* dat_layout(const char* name, size_t* size) { return dat_layout_in("ROM\\119\\51.DAT", name, size); }

static void w16(uint8_t* p, int v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

static int r16(const uint8_t* p) { return (int16_t)(p[0] | p[1] << 8); }

typedef struct Ref
{
    int slot, index;
    const char* name; /* 16 characters */
} Ref;

/* A layout block - the window (id < 0) or an item - of a multiple of 16 bytes:
 *   +0 size, +2 x, +4 y, +0xa w, +0xc h, +0xe/+0x10 an offset (the title tab's);
 *   the window: +0x13 a flag, +0x14 refs, +0x15/+0x16 its strings' lengths;
 *   an item: +0x12 id, +0x15 previous, next, up, down, right, left (ids; 0xff none), +0x1b refs,
 *     +0x1d/+0x1e its strings' lengths;
 *   at +0x20 the refs (u16 slot, i16 sprite, sheet name[16]), then two strings (the help line's
 *   and title's text ids; "-1" for none). */
static size_t block(uint8_t* p, int x, int y, int w, int h, int id, const uint8_t links[6], const Ref* refs,
    int nrefs, const char* s1, const char* s2, int flag)
{
    size_t l1 = strlen(s1), l2 = strlen(s2), size = (0x20 + 20 * (size_t)nrefs + l1 + 1 + l2 + 1 + 15) & ~(size_t)15;
    memset(p, 0, size);
    w16(p, (int)size), w16(p + 2, x), w16(p + 4, y), w16(p + 0xa, w), w16(p + 0xc, h);
    if (id < 0)
        p[0x13] = (uint8_t)flag, p[0x14] = (uint8_t)nrefs, p[0x15] = (uint8_t)l1, p[0x16] = (uint8_t)l2;
    else
    {
        w16(p + 0x12, id);
        memcpy(p + 0x15, links, 6);
        p[0x1b] = (uint8_t)nrefs, p[0x1d] = (uint8_t)l1, p[0x1e] = (uint8_t)l2;
    }
    uint8_t* q = p + 0x20;
    for (int i = 0; i < nrefs; ++i, q += 20)
    {
        w16(q, refs[i].slot), w16(q + 2, refs[i].index);
        memcpy(q + 4, refs[i].name, 16);
    }
    memcpy(q, s1, l1 + 1);
    memcpy(q + l1 + 1, s2, l2 + 1);
    return size;
}


/* The menu font's small glyphs in "font    font" (datui.h), the ones the game's captions and
 * buttons are made of */
typedef UiGlyph Glyph;

static const Glyph* glyph(char c) { return ui_menu_glyph(c); }

static int text_width(const char* s) { return ui_menu_text_width(s); }

static const char NEWTEX[] = "menu    newtex  ", GAUGE[] = "menu    gauge   ", BUTTONTO[] = "menu    buttonto",
                  FONT[] = "font    font    ";

/* Colours are RGBA a vertex (TL, TR, BL, BR), 0x7f the texel as it is; modes as the game's sheets
 * have them (their bytes in order). */
enum
{
    WHITE = 0x7f7f7f7fu,
    SHADOW = 0x7f7f7f20u,  /* a button's words' drop shadow */
    PILL_COL = 0x60607f70u, /* a button */
    PILL_SHADOW = 0x00000070u,
    M_PANEL = 0x01000101u, /* a panel's own parts */
    M_ITEM = 0x01000201u,  /* a button's */
    M_SHADOW = 0x01020001u,
    M_GAUGE = 0x00000201u,
};

static struct
{
    uint8_t p[256 * 1024];
    size_t n, count_at;
    int sprites, overflow;
} S;

static void put(const void* b, size_t n)
{
    if (S.n + n > sizeof S.p)
    {
        S.overflow = 1;
        return;
    }
    memcpy(S.p + S.n, b, n);
    S.n += n;
}

static int sprite_begin(void)
{
    S.count_at = S.n;
    put("", 1);
    return S.sprites++;
}

static void part4(int x0, int y0, int x1, int y1, int uw, int uh, int u, int v, const uint32_t col[4], uint32_t mode,
    const char* image)
{
    if (S.overflow || S.p[S.count_at] == 255)
        return;
    uint8_t b[61] = { 0 };
    int16_t xy[12] = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y0, (int16_t)x0, (int16_t)y1, (int16_t)x1,
        (int16_t)y1, (int16_t)uw, (int16_t)uh, (int16_t)u, (int16_t)v };
    for (int i = 0; i < 12; ++i)
        b[2 * i] = (uint8_t)xy[i], b[2 * i + 1] = (uint8_t)((uint16_t)xy[i] >> 8);
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 4; ++i)
            b[25 + 4 * k + i] = (uint8_t)(col[k] >> (24 - 8 * i));
    for (int i = 0; i < 4; ++i)
        b[41 + i] = (uint8_t)(mode >> (24 - 8 * i));
    memcpy(b + 45, image, 16);
    put(b, sizeof b);
    S.p[S.count_at]++;
}

static void part(int x0, int y0, int x1, int y1, int uw, int uh, int u, int v, uint32_t col, uint32_t mode,
    const char* image)
{
    const uint32_t c[4] = { col, col, col, col };
    part4(x0, y0, x1, y1, uw, uh, u, v, c, mode, image);
}

/* s with its line's top at x, y: a button's words (a drop shadow under each glyph) or a panel's */
static const char* g_font = FONT; /* the glyphs' image: the lobby's panel names it "FONT    font    " */

static void words(int x, int y, const char* s, int button)
{
    for (int pass = button ? 0 : 1; pass < 2; ++pass)
    {
        int at = x, d = pass ? 0 : 2;
        for (const char* c = s; *c; ++c)
        {
            const Glyph* g = glyph(*c);
            if (!g)
            {
                at += 4;
                continue;
            }
            int top = y + g->v - g->top + d;
            part(at + d, top, at + d + g->w, top + g->h, g->w, g->h, g->u, g->v, pass ? WHITE : SHADOW,
                pass ? (button ? M_ITEM : M_PANEL) : M_SHADOW, g_font);
            at += g->w - 1;
        }
    }
}

/* The menu pill, w wide, 16 tall. "menu/buttonto" holds it in two strips - its left end and body
 * (row 0 from u 16), its body and right end (row 16) - which the game's own 88-wide buttons put
 * side by side; at other widths their bodies meet in a seam. Here: the left end, one body stretched
 * between, the right end. */
static void pill(int x, int y, int w, uint32_t col, uint32_t mode)
{
    part(x, y, x + 8, y + 16, 8, 16, 16, 0, col, mode, BUTTONTO);
    part(x + 8, y, x + w - 8, y + 16, 32, 16, 0, 16, col, mode, BUTTONTO);
    part(x + w - 8, y, x + w, y + 16, 8, 16, 32, 16, col, mode, BUTTONTO);
}

/* a button: its pill and words, at its left as the game's are */
static int button_sprite(const char* s, int w)
{
    int i = sprite_begin();
    pill(0, 0, w, PILL_COL, M_ITEM);
    words(5, 1, s, 1);
    return i;
}

/* the red line under a button the game marks the current choice with: drawn at the button's
 * x + 0x25, y + 4 (PAGE_MARK), from 6 to w - 3 across its foot */
static int underline_sprite(int w)
{
    int i = sprite_begin(), x0 = 6 - 0x25, x1 = w - 3 - 0x25;
    part(x0, 13, x0 + 9, 16, 9, 3, 9, 22, WHITE, M_ITEM, GAUGE);
    part(x0 + 9, 13, x1 - 8, 16, 8, 3, 10, 22, WHITE, M_ITEM, GAUGE);
    part(x1 - 8, 13, x1, 16, 8, 3, 11, 22, WHITE, M_ITEM, GAUGE);
    return i;
}


/* ---- the pages ---- */

enum
{
    PAGE_X = 16, /* where the Config pages open, in the menus' coordinates */
    PAGE_Y = 48,
    MIN_W = 424,
    TITLE_H = 30, /* the title strip, and room under it */
    ROW_H = 22,
    TRACK_W = 150, /* a slider's, from 32 right of its row's controls' column */
    LIST_VALUE_X = 32 + TRACK_W + 12 + 36, /* a list's value's words, from its row's controls' column: after Max */
    MAX_ROWS = 24,
    MAX_ITEMS = 64,
};

typedef struct Item
{
    int row, opt; /* opt -1: a slider */
    int x, w, sprite;
} Item;

typedef struct Page
{
    const char* name;  /* its layout's and its menu table entry's */
    const char* title; /* on its title strip, and its item in the Config list */
    const char* config_help;
    uint32_t spare; /* the menu table entry it takes, */
    const char* spare_was; /* named this (and, for the duplicate conf1win, after one of the same) */
    int spare_how;         /* SPARE_* */
    uint32_t modes;        /* its entry's +0x28: the game modes it opens in (0x08 the lobby's, 0x01 the field's) and more */
    const Row* rows;
    int nrows;
    /* made at setup */
    Item items[MAX_ITEMS];
    int nitems, row_item[MAX_ROWS + 1], w, h, cx; /* cx: the controls' column */
    int spr_panel, spr_caption, spr_config, spr_line[MAX_ROWS];
    uint32_t layout, layout_size, handler, str_name, str_config_help, str_help[MAX_ROWS];
} Page;

/* How a page's menu table entry is free: the second of two of a name (the game only ever finds the
 * first), or the table's end - the empty entry its loops stop at, unread otherwise, whose place the
 * next thing in memory (first byte 0) then takes */
enum
{
    SPARE_UNUSED = 0, /* an entry whose menu is in no DAT and that nothing opens */
    SPARE_DUP = 1,
    SPARE_END = 2,
    TABLE_END = FFXI_MODERN_MENUS_END, /* 0x10376270 */
};

static const char NO_NAME[16] = { 0 };

static Page PAGES[] = {
    { "menu    displayw", "Display", "Window mode, resolution, UI scale, background, anti-aliasing and the frame rate.",
        FFXI_MODERN_MENUS_DISPLAY, "tkdebug dbdelsel", SPARE_UNUSED, 0x1c0209u, DISPLAY_ROWS,
        sizeof DISPLAY_ROWS / sizeof DISPLAY_ROWS[0] },
    { "menu    menushid", "Menus", "Show or hide items of the game's own menus.", TABLE_END, NO_NAME, SPARE_END, 0x1c0201u,
        MENUS_ROWS, sizeof MENUS_ROWS / sizeof MENUS_ROWS[0] },
    { "menu    modernwi", "Modern", "Graphics beyond the original's: effects, lighting and draw distance.",
        FFXI_MODERN_MENUS_CONF1WIN, "menu    conf1win", SPARE_DUP, 0x1c0201u, MODERN_ROWS,
        sizeof MODERN_ROWS / sizeof MODERN_ROWS[0] },
};
enum
{
    PAGE_DISPLAY = 0, /* the one the lobby's Config window opens */
};
enum
{
    NPAGES = sizeof PAGES / sizeof PAGES[0],
};

static int g_spr_track, g_spr_fill; /* a slider's hit area (nothing to see) and its fill */
static int g_spr_blank;             /* nothing to see: what a hidden item of the game's draws */

static int row_top(int r) { return TITLE_H + r * ROW_H; }

/* the buttons of a row: as wide as its longest word needs, at least the game's small or middling */
static void layout_items(Page* p)
{
    p->nitems = 0, p->w = MIN_W, p->cx = 150;
    for (int r = 0; r < p->nrows; ++r)
        if (32 + text_width(p->rows[r].label) + 24 > p->cx)
            p->cx = 32 + text_width(p->rows[r].label) + 24;
    for (int r = 0; r < p->nrows; ++r)
    {
        const Row* row = &p->rows[r];
        p->row_item[r] = p->nitems;
        if (row->kind == SLIDER)
            p->items[p->nitems++] = (Item){ r, -1, p->cx + 32, TRACK_W, 0 };
        else if (row->kind == LIST)
        {
            const List* l = list_of(row->key);
            int widest = 0;
            for (int i = 0; l && i < l->n; ++i)
                widest = text_width(l->text[i]) > widest ? text_width(l->text[i]) : widest;
            p->items[p->nitems++] = (Item){ r, -1, p->cx + 32, TRACK_W, 0 };
            if (p->cx + LIST_VALUE_X + widest + 16 > p->w)
                p->w = p->cx + LIST_VALUE_X + widest + 16;
        }
        else
        {
            int w = row->nopt > 3 ? 52 : 64;
            for (int o = 0; o < row->nopt; ++o)
                if (text_width(row->opt[o]) + 20 > w)
                    w = (text_width(row->opt[o]) + 21) & ~1;
            for (int o = 0; o < row->nopt; ++o)
                p->items[p->nitems++] = (Item){ r, o, p->cx + o * (w + 12), w, 0 };
            if (p->cx + row->nopt * (w + 12) + 12 > p->w)
                p->w = p->cx + row->nopt * (w + 12) + 12;
        }
    }
    p->row_item[p->nrows] = p->nitems;
    p->h = TITLE_H + p->nrows * ROW_H + 8;
}

/* A page's sprites: its panel (as the Config pages': the theme's background darkening downward, the
 * title strip fading to red, a bullet a row, the buttons' shadows, the sliders' tracks), a row's
 * words apart from it (a sprite holds at most 255 parts), a row's underline, a list's values, then a
 * button an item. */
static void page_sprites(Page* p)
{
    p->spr_panel = sprite_begin();
    const uint32_t bg[4] = { WHITE, WHITE, 0x40404040u, 0x40404040u };
    part4(0, 0, p->w, p->h, p->w, p->h, 0, 0, bg, M_PANEL, NEWTEX);
    const uint32_t strip[4] = { 0x7f7f7f20u, 0x7f404010u, 0x7f7f7f20u, 0x7f404010u };
    part4(0, 4, p->w, 20, 64, 8, 0, 0, strip, M_ITEM, GAUGE);
    words(8, 6, p->title, 0);
    for (int r = 0; r < p->nrows; ++r)
    {
        int y = row_top(r);
        part(20, y + 4, 28, y + 12, 8, 8, 0, 8, WHITE, M_GAUGE, GAUGE);
        int tx = p->cx + 32;
        if (p->rows[r].kind == SLIDER || p->rows[r].kind == LIST)
        {
            part(tx - 8, y, tx, y + 16, 4, 8, 0, 8, WHITE, M_GAUGE, GAUGE);
            part(tx + TRACK_W, y, tx + TRACK_W + 8, y + 16, 4, 8, 4, 8, WHITE, M_GAUGE, GAUGE);
            part(tx, y, tx + TRACK_W, y + 16, 64, 8, 0, 0, SHADOW, M_ITEM, GAUGE);
        }
    }
    for (int i = 0; i < p->nitems; ++i)
        if (p->items[i].opt >= 0)
            pill(p->items[i].x + 2, row_top(p->items[i].row) + 2, p->items[i].w, PILL_SHADOW, M_PANEL);

    p->spr_caption = S.sprites;
    for (int r = 0; r < p->nrows; ++r)
    {
        sprite_begin();
        words(32, 2, p->rows[r].label, 0);
        if (p->rows[r].kind == SLIDER || p->rows[r].kind == LIST)
            words(p->cx, 2, "Min", 0), words(p->cx + 32 + TRACK_W + 12, 2, "Max", 0);
    }
    for (int r = 0; r < p->nrows; ++r)
    {
        p->spr_line[r] = -1;
        if (p->rows[r].kind != SLIDER && p->rows[r].kind != LIST)
            p->spr_line[r] = underline_sprite(p->items[p->row_item[r]].w);
    }
    for (int i = 0; i < p->nitems; ++i)
        if (p->items[i].opt >= 0)
            p->items[i].sprite = button_sprite(p->rows[p->items[i].row].opt[p->items[i].opt], p->items[i].w);
        else
            p->items[i].sprite = g_spr_track;
    p->spr_config = button_sprite(p->title, 88);
}

/* A list's values: each its words after the slider's Max, drawn by PAGE_MARK (at the item - the
 * track, 32 right of the controls' column - x + 0x25, y + 4) */
static void list_sprites(List* l)
{
    for (int i = 0; i < l->n; ++i)
    {
        l->spr[i] = sprite_begin();
        words(LIST_VALUE_X - 32 - 0x25, 2 - 4, l->text[i], 0);
    }
}

static int build_sheet(void)
{
    S.n = 0, S.sprites = 0, S.overflow = 0;
    put(SHEET_NAME, 16);
    put("\x04", 1);
    put(NEWTEX, 16), put(GAUGE, 16), put(BUTTONTO, 16), put(FONT, 16);
    size_t count = S.n;
    put("\0\0", 2);
    g_spr_track = sprite_begin();
    part(0, 0, TRACK_W, 16, 64, 8, 0, 0, 0x7f7f7f00u, M_ITEM, GAUGE);
    g_spr_fill = sprite_begin();
    part(0, 2, TRACK_W, 14, 64, 6, 0, 1, 0x60607f7fu, M_GAUGE, GAUGE);
    g_spr_blank = sprite_begin();
    part(0, 0, 1, 1, 1, 1, 0, 0, 0x7f7f7f00u, M_ITEM, GAUGE);
    for (int k = 0; k < NPAGES; ++k)
    {
        layout_items(&PAGES[k]);
        page_sprites(&PAGES[k]);
    }
    list_sprites(&g_res);
    list_sprites(&g_scale);
    S.p[count] = (uint8_t)S.sprites, S.p[count + 1] = (uint8_t)(S.sprites >> 8);
    return !S.overflow;
}

/* the item of row r nearest x across */
static int nearest(const Page* p, int r, int x)
{
    int best = p->row_item[r];
    for (int i = p->row_item[r]; i < p->row_item[r + 1]; ++i)
        if (abs(p->items[i].x - x) < abs(p->items[best].x - x))
            best = i;
    return best;
}

/* A page's layout: the panel with the cursor, a button an option and a slider a range (up and down
 * to the nearest in the next row, left and right along its own), then each row's words as a part
 * that takes no cursor (as the Config list's title tab). */
static uint8_t* page_layout(const Page* p, size_t* out_size)
{
    const Ref window[2] = { { 0, p->spr_panel, SHEET_NAME }, { 6, 0, "anc     anc_s   " } };
    static const uint8_t NONE[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int n = p->nitems + p->nrows, rows = p->nrows;
    uint8_t* dst = calloc(1, 0x20 + 96 + (size_t)n * 64);
    if (!dst)
        return NULL;
    memcpy(dst, p->name, 16);
    dst[0x10] = 1, dst[0x11] = (uint8_t)n;
    size_t to = 0x20;
    to += block(dst + to, PAGE_X, PAGE_Y, p->w, p->h, -1, NULL, window, 2, "-1", "-1", 0);
    for (int i = 0; i < p->nitems; ++i)
    {
        const Item* it = &p->items[i];
        int r = it->row, first = p->row_item[r], across = p->row_item[r + 1] - first, k = i - first;
        uint8_t links[6] = { (uint8_t)((i + p->nitems - 1) % p->nitems + 1), (uint8_t)((i + 1) % p->nitems + 1),
            (uint8_t)(nearest(p, (r + rows - 1) % rows, it->x) + 1), (uint8_t)(nearest(p, (r + 1) % rows, it->x) + 1),
            (uint8_t)(first + (k + 1) % across + 1), (uint8_t)(first + (k + across - 1) % across + 1) };
        Ref ref = { 0, it->sprite, SHEET_NAME };
        to += block(dst + to, it->x, row_top(r), it->w, 16, i + 1, links, &ref, 1, "-1", "-1", 0);
    }
    for (int r = 0; r < rows; ++r)
    {
        Ref ref = { 0, p->spr_caption + r, SHEET_NAME };
        to += block(dst + to, 0, row_top(r), 16, 16, p->nitems + 1 + r, NONE, &ref, 1, "-1", "-1", 0);
    }
    *out_size = to;
    return dst;
}

/* The Config list with our pages after its last item: every block as it is, the window taller, ours
 * linked in between the last item and the first, the title tab renumbered after them. */
static uint8_t* config_layout(size_t* out_size)
{
    size_t size;
    uint8_t* src = dat_layout(CONFIG_NAME, &size);
    if (!src)
        return NULL;
    int count = src[0x11];
    uint8_t* dst = calloc(1, size + 128 * NPAGES);
    size_t at = 0x20, to = 0x20;
    memcpy(dst, src, 0x20);
    int last = 0, lastx = 0, lasty = -1, lastw = 0, lasth = 0, ok = 1;
    for (int b = -1; b < count; ++b)
    {
        size_t n = at + 0x20 <= size ? (size_t)r16(src + at) : 0;
        if (n < 0x20 || at + n > size)
        {
            ok = 0;
            break;
        }
        if (b >= 0 && src[at + 0x15] == 0xFF)
        {
            /* the title tab: ours go in before it */
            for (int k = 0; k < NPAGES; ++k)
            {
                int id = last + 1 + k, up = id - 1, down = k + 1 < NPAGES ? id + 1 : 1;
                uint8_t links[6] = { (uint8_t)up, (uint8_t)down, (uint8_t)up, (uint8_t)down, (uint8_t)id, (uint8_t)id };
                Ref ref = { 0, PAGES[k].spr_config, SHEET_NAME };
                to += block(dst + to, lastx, lasty + lasth * (k + 1), lastw, lasth, id, links, &ref, 1, "-1", "-1", 0);
            }
            memcpy(dst + to, src + at, n);
            w16(dst + to + 0x12, last + NPAGES + 1);
        }
        else
        {
            memcpy(dst + to, src + at, n);
            if (b < 0)
                w16(dst + to + 0xc, r16(dst + to + 0xc) + 16 * NPAGES); /* the window: taller */
            else if (r16(src + at + 4) > lasty)
                last = r16(src + at + 0x12), lastx = r16(src + at + 2), lasty = r16(src + at + 4),
                lastw = r16(src + at + 0xa), lasth = r16(src + at + 0xc);
        }
        to += n;
        at += n;
    }
    free(src);
    if (!ok || last != CONFIG_ITEMS)
    {
        free(dst);
        return NULL;
    }
    dst[0x11] = (uint8_t)(count + NPAGES);
    /* the last item leads down to ours, the first up to them */
    at = 0x20 + (size_t)r16(dst + 0x20);
    for (int b = 0; b < count + NPAGES; ++b, at += (size_t)r16(dst + at))
    {
        int id = r16(dst + at + 0x12);
        if (id == last)
            dst[at + 0x16] = dst[at + 0x18] = (uint8_t)(last + 1);
        if (id == 1)
            dst[at + 0x15] = dst[at + 0x17] = (uint8_t)(last + NPAGES);
    }
    *out_size = to;
    return dst;
}

/* ---- the handlers ---- */

static uint32_t g_config_slots[VTBL_SLOTS]; /* the Config list's own */
static void lobby_input(Guest* g);
static void lobby_setup(void);
static uint32_t g_scratch;                  /* guest room: a name, a cursor */
static uint32_t g_sheet_guest, g_config_guest, g_config_size; /* the game keeps pointers into these */

static uint32_t window_of(uint32_t handler) { return rd32(handler + 8); }

static uint32_t find_sheet(void)
{
    memcpy(GUEST_PTR(g_scratch + 16), SHEET_NAME, 16);
    return guest_thiscall(SHEET_FIND, SHEETS, 1, (uint32_t[]){ g_scratch + 16 });
}

static int g_nsprites; /* our sheet's */

/* one of our sprites, as the game holds it */
static uint32_t sprite(int i)
{
    uint32_t sheet = find_sheet(), list = sheet ? rd32(sheet) : 0;
    return list && i >= 0 && i < g_nsprites ? rd32(list + 4u * (uint32_t)i) : 0;
}

/* item's help, in the game's help line while the cursor is on it */
static void help(uint32_t window, int item, uint32_t str)
{
    uint32_t part = window ? guest_thiscall(WINDOW_PART, window, 1, (uint32_t[]){ (uint32_t)item }) : 0;
    if (part && rd32(part + 0x4c) != str)
    {
        wr8(part + 0x4a, 1);
        wr32(part + 0x4c, str);
    }
}

static Page* page_of(uint32_t handler)
{
    for (int k = 0; k < NPAGES; ++k)
        if (PAGES[k].handler == handler)
            return &PAGES[k];
    return NULL;
}

/* the Config list's select: ours open their pages, the rest as the game does */
static void config_input(Guest* g)
{
    uint32_t self = g->ecx, ev = ARG(0), item = ARG(1);
    int k = (int16_t)item - CONFIG_ITEMS - 1;
    if ((int16_t)ev == EV_SELECT && k >= 0 && k < NPAGES)
        guest_thiscall(MENU_OPEN, MGR, 3, (uint32_t[]){ PAGES[k].str_name, 1, 0 });
    else
        guest_thiscall(g_config_slots[6], self, 2, (uint32_t[]){ ev, item });
    RET(0, 2);
}

/* ... and its drawing: our items' help */
static void config_draw(Guest* g)
{
    uint32_t self = g->ecx;
    guest_thiscall(g_config_slots[4], self, 0, NULL);
    for (int k = 0; k < NPAGES; ++k)
        help(window_of(self), CONFIG_ITEMS + 1 + k, PAGES[k].str_config_help);
    RET(0, 0);
}

static void page_open(Guest* g) { RET(0, 0); }

static void page_close(Guest* g)
{
    save();
    RET(0, 0);
}

/* each frame the page is up: the red line under each row's current choice, each slider's fill and
 * a list's value - as a Config page draws them, with our sprites in the page's slots for them */
static void page_draw(Guest* g)
{
    uint32_t self = g->ecx, window = window_of(self), fill = sprite(g_spr_fill);
    Page* p = page_of(self);
    for (int r = 0; p && r < p->nrows; ++r)
    {
        const Row* row = &p->rows[r];
        uint32_t id = (uint32_t)p->row_item[r] + 1;
        const List* l = list_of(row->key);
        if (row->kind == LIST && l && l->n)
        {
            int at = list_at(l);
            float t = l->n > 1 ? (float)at / (float)(l->n - 1) : 1.0f;
            uint32_t bits, value = sprite(l->spr[at]);
            memcpy(&bits, &t, 4);
            if (fill)
            {
                wr32(self + 0x1c, fill);
                guest_thiscall(PAGE_FILL, self, 3, (uint32_t[]){ id, bits, 0x80808080u });
            }
            if (value)
            {
                wr32(self + 0x14, value);
                guest_thiscall(PAGE_MARK, self, 3, (uint32_t[]){ 0, id, id });
            }
        }
        else if (row->kind == SLIDER)
        {
            float t = place(row);
            uint32_t bits;
            memcpy(&bits, &t, 4);
            if (fill)
            {
                wr32(self + 0x1c, fill);
                guest_thiscall(PAGE_FILL, self, 3, (uint32_t[]){ id, bits, 0x80808080u });
            }
        }
        else
        {
            int o = option(row);
            uint32_t line = sprite(p->spr_line[r]);
            if (o >= 0 && line)
            {
                wr32(self + 0x14, line);
                guest_thiscall(PAGE_MARK, self, 3, (uint32_t[]){ 0, id + (uint32_t)o, id + (uint32_t)o });
            }
        }
        for (int i = p->row_item[r]; i < p->row_item[r + 1]; ++i)
            help(window, i + 1, p->str_help[r]);
    }
    RET(0, 0);
}

/* input on a page: select sets a button's option; a slider's own left and right (it leads to
 * itself either way) step it */
static void page_input(Guest* g)
{
    int ev = (int16_t)ARG(0), id = (int16_t)ARG(1);
    Page* p = page_of(g->ecx);
    if (p && id >= 1 && id <= p->nitems)
    {
        const Item* it = &p->items[id - 1];
        const Row* row = &p->rows[it->row];
        const List* l = list_of(row->key);
        if (row->kind == LIST && l && l->n)
        {
            int at = list_at(l);
            if (ev == EV_LEFT || ev == EV_RIGHT)
                set(row, (float)(ev == EV_RIGHT ? (at + 1 < l->n ? at + 1 : at) : at > 0 ? at - 1 : 0));
        }
        else if (it->opt >= 0 && ev == EV_SELECT)
            set(row, row->val[it->opt]);
        else if (it->opt < 0 && (ev == EV_LEFT || ev == EV_RIGHT))
            slide(row, ev == EV_RIGHT ? 1 : -1);
    }
    RET(0, 2);
}

static const ShimDef SHIMS[] = {
    { "modern", "config_input", config_input },
    { "modern", "config_draw", config_draw },
    { "modern", "page_open", page_open },
    { "modern", "page_close", page_close },
    { "modern", "page_draw", page_draw },
    { "modern", "page_input", page_input },
    { "modern", "lobby_input", lobby_input },
    { NULL, NULL, NULL },
};

/* ---- putting it in ---- */

static uint32_t find_layout(const char* name16)
{
    memcpy(GUEST_PTR(g_scratch + 16), name16, 16);
    return guest_thiscall(LAYOUT_FIND, MGR, 1, (uint32_t[]){ g_scratch + 16 });
}

/* the game's parse of a chunk payload into one of its lists */
static void add(uint32_t fn, uint32_t list, uint32_t payload)
{
    wr32(g_scratch + 40, payload);
    guest_thiscall(fn, list, 1, (uint32_t[]){ g_scratch + 40 });
}

/* ---- hiding the game's own items ---- */

/* The combat menu is no layout: as it opens, the game fills it from a list of action ids for the
 * kind of menu it is (13 each, 0x1c ending one) and picks the window ("actionm<n>") for its length.
 * Trust is action 25: a list without it is a menu without it. */
enum
{
    ACTION_LISTS = FFXI_MODERN_ACTION_LISTS, /* 0x1036f390 */
    ACTION_KINDS = 8,
    ACTION_LEN = 13,
    ACTION_TRUST = 25,
};

static const uint8_t ACTIONS_KNOWN[ACTION_KINDS][ACTION_LEN] = {
    { 4, 8, 7, 25, 9, 12, 13, 22, 24, 28 },
    { 4, 14, 12, 15, 28 },
    { 4, 8, 7, 25, 9, 19, 16, 12, 22, 24, 27, 10, 28 },
    { 4, 14, 16, 12, 15, 27, 10, 28 },
    { 1, 8, 7, 25, 9, 12, 22, 24, 11, 28 },
    { 21, 14, 12, 11, 28 },
    { 18, 8, 7, 25, 9, 3, 12, 24, 11, 28 },
    { 6, 8, 7, 25, 9, 20, 17, 12, 26, 28 },
};

static int g_hide_ready, g_actions_ok;

static void actions_install(void)
{
    if (!g_actions_ok)
        return;
    uint8_t* d = GUEST_PTR(ACTION_LISTS);
    for (int k = 0; k < ACTION_KINDS; ++k)
    {
        int n = 0;
        for (int i = 0; i < ACTION_LEN; ++i)
            if (!(ACTIONS_KNOWN[k][i] == ACTION_TRUST && g_hide >> HIDE_TRUST & 1))
                d[k * ACTION_LEN + n++] = ACTIONS_KNOWN[k][i];
        while (n < ACTION_LEN)
            d[k * ACTION_LEN + n++] = 0;
    }
}

/* Magic, Status and Abilities are layouts, their handlers going by an item's id. The game keeps its cursor
 * within the count of items, so a hidden item stays - its id, and the count, as they were - but
 * empty: no size, a sprite of nothing, the links that led to it leading past it. The rows under it
 * move up and the window is shorter. */
typedef struct HiddenItem
{
    int id, bit, sprite; /* sprite: its first ref's, as known */
} HiddenItem;

typedef struct Menu
{
    const char *name, *renamed;
    int blocks; /* its items, the title tab one of them */
    HiddenItem items[4];
    int nitems;
    int from_bottom;      /* 1: it stands on the combat menu, its foot where it was */
    unsigned installed;   /* the hide bits of ours in the game's list */
    uint32_t guest, size; /* ours, which the game keeps pointers into */
} Menu;

static Menu MENUS[] = {
    { "menu    mgcmenu ", "menu    mgcmenu_", 9,
        { { 7, HIDE_MAGIC_GEOMANCY, 496 }, { 8, HIDE_TRUST, 505 } }, 2 },
    { "menu    abimenu ", "menu    abimenu_", 7, { { 6, HIDE_MOUNTS, 551 } }, 1 },
    { "menu    abiselec", "menu    abisele_", 6, { { 5, HIDE_MOUNTS, 551 } }, 1, 1 },
    { "menu    statcom2", "menu    statcom_", 14,
        { { 12, HIDE_MASTER_LEVELS, 564 }, { 11, HIDE_UNITY, 529 }, { 8, HIDE_JOB_POINTS, 669 },
            { 13, HIDE_ALTER_EGO, 843 } },
        4 },
};
enum
{
    NMENUS = sizeof MENUS / sizeof MENUS[0],
    MENU_MAX_BLOCKS = 32,
};

/* the menu's layout from the DAT with the items in hide empty; NULL when it is not the one known */
static uint8_t* menu_layout(const Menu* m, unsigned hide, size_t* out_size)
{
    size_t size;
    uint8_t* p = dat_layout(m->name, &size);
    if (!p)
        return NULL;
    size_t at[MENU_MAX_BLOCKS], o = 0x20, win = 0x20;
    int y[MENU_MAX_BLOCKS], hidden[MENU_MAX_BLOCKS] = { 0 }, n = p[0x11], ok = n == m->blocks && n < MENU_MAX_BLOCKS;
    uint8_t links[MENU_MAX_BLOCKS][6];
    for (int b = -1; ok && b < n; ++b)
    {
        size_t len = o + 0x20 <= size ? (size_t)r16(p + o) : 0;
        if (len < 0x20 || o + len > size)
            ok = 0;
        else if (b >= 0)
        {
            at[b] = o, y[b] = r16(p + o + 4);
            memcpy(links[b], p + o + 0x15, 6);
            if (r16(p + o + 0x12) != b + 1) /* the known ones number their items in order */
                ok = 0;
        }
        o += len;
    }
    int nhidden = 0;
    for (int i = 0; ok && i < m->nitems; ++i)
    {
        int b = m->items[i].id - 1;
        if (b < 0 || b >= n || p[at[b] + 0x1b] < 1 || r16(p + at[b] + 0x22) != m->items[i].sprite)
            ok = 0;
        else if (hide >> m->items[i].bit & 1)
            hidden[b] = 1, ++nhidden;
    }
    if (!ok)
    {
        free(p);
        return NULL;
    }
    for (int b = 0; b < n; ++b)
    {
        uint8_t* q = p + at[b];
        if (hidden[b])
        {
            w16(q + 2, 0), w16(q + 4, 0), w16(q + 0xa, 0), w16(q + 0xc, 0);
            for (int r = 0; r < q[0x1b]; ++r)
            {
                w16(q + 0x20 + 20 * r + 2, g_spr_blank);
                memcpy(q + 0x20 + 20 * r + 4, SHEET_NAME, 16);
            }
            continue;
        }
        if (links[b][0] == 0xFF)
            continue; /* the title tab */
        int above = 0;
        for (int c = 0; c < n; ++c)
            above += hidden[c] && y[c] < y[b];
        w16(q + 4, y[b] - 16 * above);
        for (int j = 0; j < 6; ++j)
        {
            int t = links[b][j];
            for (int guard = 0; guard < n && t >= 1 && t <= n && hidden[t - 1]; ++guard)
                t = links[t - 1][j];
            q[0x15 + j] = (uint8_t)t;
        }
    }
    /* the window shorter, and its frame: "comwin" sprite k is a panel with k + 1 buttons' wells */
    w16(p + win + 0xc, r16(p + win + 0xc) - 16 * nhidden);
    if (m->from_bottom)
        w16(p + win + 4, r16(p + win + 4) + 16 * nhidden);
    if (p[win + 0x14] >= 1 && !memcmp(p + win + 0x24, "menu    comwin  ", 16) && r16(p + win + 0x22) >= nhidden)
        w16(p + win + 0x22, r16(p + win + 0x22) - nhidden);
    *out_size = size;
    return p;
}

/* ours in the game's list in place of what is there, when what is there hides other than it should */
static void menu_install(Menu* m)
{
    unsigned want = 0;
    for (int i = 0; i < m->nitems; ++i)
        want |= g_hide & 1u << m->items[i].bit;
    uint32_t cur = find_layout(m->name);
    if (!cur)
        return;
    uint32_t parsed = rd32(cur + 8), block = parsed ? rd32(parsed) : 0;
    int ours = m->guest && block >= m->guest && block < m->guest + m->size;
    if (ours ? want == m->installed : !want)
        return;
    size_t n;
    uint8_t* l = menu_layout(m, want, &n);
    if (!l)
    {
        fprintf(stderr, "[modern] %.16s in the menu DAT is not the one known: its items stay\n", m->name);
        m->nitems = 0;
        return;
    }
    m->guest = gbytes(l, (uint32_t)n), m->size = (uint32_t)n;
    free(l);
    memcpy(GUEST_PTR(cur + 0x46), m->renamed, 16);
    if (!find_layout(m->name))
        add(LAYOUT_ADD, MGR, m->guest);
    m->installed = want;
}

/* the settings into the game's menus: each shows as it next opens */
static void hide_apply(void)
{
    if (!g_hide_ready)
        return;
    actions_install();
    for (int k = 0; k < NMENUS; ++k)
        menu_install(&MENUS[k]);
}

/* ---- the lobby's Config window: a Display Settings row ----
 * The title screen's Config ("menu    lobycwin", in the lobby's menu DAT, 50.DAT) is a Config page
 * of its own: music, volume, where logging out goes, the background's shape and a Gamepad button,
 * its panel sprite 184 of "menu    lobbywin". Here it gets a row more, a Display button under
 * Gamepad that opens Config > Display: the panel copied from the DAT, 38 taller with the row's
 * bullet, words and button shadow; the layout with the button after Gamepad; the handler's input
 * first here. Its help line is the lobby's own (a text by index): none on ours. */
enum
{
    LOBBY_ITEMS = 13, /* the window's own; Gamepad the last */
    LOBBY_GAMEPAD = 13,
    LOBBY_OURS = 14,
    LOBBY_ROW = 38,   /* between its rows */
    LOBBY_PANEL = 184,
};

static const char LOBBY_NAME[] = "menu    lobycwin", LOBBY_RENAMED[] = "menu    lobycwi_",
                  LOBBY_SHEET[] = "menu    modernlb", LOBBY_WIN_SHEET[] = "menu    lobbywin",
                  FONT_LOBBY[] = "FONT    font    ";
static uint32_t g_lobby_sheet, g_lobby_layout, g_lobby_size, g_lobby_slots[VTBL_SLOTS], g_lobby_vt;
static int g_lobby_ok;

/* A sprite's parts in a sheet payload (name, images, sprites of parts of 61 bytes): where they
 * start, their count; NULL when it has no such sprite */
static const uint8_t* sheet_sprite(const uint8_t* p, size_t n, int index, int* nparts)
{
    size_t at = 17 + 16 * (size_t)(n > 16 ? p[16] : 0);
    if (at + 2 > n)
        return NULL;
    int count = p[at] | p[at + 1] << 8;
    at += 2;
    for (int i = 0; i < count && at < n; ++i)
    {
        int k = p[at++];
        if (at + 61 * (size_t)k > n)
            return NULL;
        if (i == index)
            return *nparts = k, p + at;
        at += 61 * (size_t)k;
    }
    return NULL;
}

/* The lobby's sheet: the Config window's panel a row taller, and the Display button */
static int lobby_sheet(void)
{
    size_t n;
    uint8_t* win = dat_chunk("ROM\\119\\50.DAT", 0x31, LOBBY_WIN_SHEET, &n);
    int nparts = 0;
    const uint8_t* parts = win ? sheet_sprite(win, n, LOBBY_PANEL, &nparts) : NULL;
    /* the panel as known: its background (newtex) first, the window's size */
    if (!parts || nparts < 2 || nparts > 200 || memcmp(parts + 45, NEWTEX, 16) || r16(parts + 10) != 248 ||
        r16(parts + 18) != 248)
    {
        free(win);
        return 0;
    }
    S.n = 0, S.sprites = 0, S.overflow = 0;
    put(LOBBY_SHEET, 16);
    put("\x05", 1);
    put(NEWTEX, 16), put(GAUGE, 16), put(BUTTONTO, 16), put(FONT_LOBBY, 16), put(FONT, 16);
    size_t count = S.n;
    put("\0\0", 2);
    sprite_begin();
    for (int i = 0; i < nparts; ++i)
    {
        uint8_t b[61];
        memcpy(b, parts + 61 * i, 61);
        if (i == 0)
        {
            /* the background: to the new foot, its texture as tall */
            int h = 248 + LOBBY_ROW;
            w16(b + 10, h), w16(b + 14, h), w16(b + 18, h);
        }
        put(b, 61);
        S.p[S.count_at]++;
    }
    free(win);
    int y = 177 + LOBBY_ROW; /* the Gamepad row's words' top, a row down */
    g_font = FONT_LOBBY;
    part(20, y + 2, 28, y + 10, 8, 8, 0, 8, WHITE, M_GAUGE, GAUGE);
    words(32, y, "Display Settings", 0);
    pill(64, 191 + LOBBY_ROW + 2, 88, PILL_SHADOW, M_PANEL);
    button_sprite("Display", 88);
    g_font = FONT;
    S.p[count] = (uint8_t)S.sprites, S.p[count + 1] = (uint8_t)(S.sprites >> 8);
    return !S.overflow;
}

/* The window's layout from the DAT with ours after Gamepad: the window a row taller on our panel,
 * Gamepad leading down and on to ours, the music row (the top) up to ours */
static uint8_t* lobby_layout(size_t* out_size)
{
    size_t size;
    uint8_t* src = dat_layout_in("ROM\\119\\50.DAT", LOBBY_NAME, &size);
    if (!src || src[0x11] != LOBBY_ITEMS)
    {
        free(src);
        return NULL;
    }
    uint8_t* dst = calloc(1, size + 128);
    memcpy(dst, src, size);
    free(src);
    size_t at = 0x20, win = 0x20;
    int ok = 0, gy = 0, gx = 0;
    for (int b = -1; b < LOBBY_ITEMS; ++b)
    {
        size_t n = at + 0x20 <= size ? (size_t)r16(dst + at) : 0;
        if (n < 0x20 || at + n > size)
        {
            ok = 0;
            break;
        }
        if (b >= 0)
        {
            uint8_t* q = dst + at;
            int id = r16(q + 0x12);
            if (id == LOBBY_GAMEPAD)
            {
                q[0x16] = q[0x18] = LOBBY_OURS; /* next, down */
                gx = r16(q + 2), gy = r16(q + 4), ok = 1;
            }
            if (q[0x17] == LOBBY_GAMEPAD && id != LOBBY_GAMEPAD && r16(q + 4) < 60)
                q[0x17] = LOBBY_OURS; /* the top row's up */
            if (q[0x15] == LOBBY_GAMEPAD && id != LOBBY_GAMEPAD && r16(q + 4) < 60)
                q[0x15] = LOBBY_OURS; /* the first's previous */
        }
        at += n;
    }
    uint8_t* w = dst + win;
    if (!ok || gy != 191 || r16(w + 0xc) != 248 || w[0x14] < 1 || r16(w + 0x22) != LOBBY_PANEL ||
        memcmp(w + 0x24, LOBBY_WIN_SHEET, 16))
    {
        free(dst);
        return NULL;
    }
    w16(w + 0xc, 248 + LOBBY_ROW);
    w16(w + 0x22, 0);
    memcpy(w + 0x24, LOBBY_SHEET, 16);
    static const uint8_t links[6] = { LOBBY_GAMEPAD, 5, LOBBY_GAMEPAD, 5, LOBBY_OURS, LOBBY_OURS };
    Ref ref = { 0, 1, LOBBY_SHEET };
    at += block(dst + at, gx, gy + LOBBY_ROW, 88, 16, LOBBY_OURS, links, &ref, 1, "-1", "-1", 0);
    dst[0x11] = LOBBY_ITEMS + 1;
    *out_size = at;
    return dst;
}

/* the lobby Config window's input: ours opens Config > Display, the rest as the game does */
static void lobby_input(Guest* g)
{
    uint32_t self = g->ecx, ev = ARG(0), item = ARG(1);
    if ((int16_t)ev == EV_SELECT && (int16_t)item == LOBBY_OURS && !rd8(self + 0x24) && !rd8(self + 0x2c))
    {
        guest_thiscall(MENU_OPEN, MGR, 3, (uint32_t[]){ PAGES[PAGE_DISPLAY].str_name, 1, 0 });
    }
    else
        guest_thiscall(g_lobby_slots[6], self, 2, (uint32_t[]){ ev, item });
    /* on ours, no help (the game's help for an id past its own is the volume's) */
    uint32_t window = window_of(self), help = rd32(LOBBY_HELP);
    if (window && help && (int16_t)rd16(window + 0x4c) == LOBBY_OURS)
        wr32(help + 0x2c, 0);
    RET(0, 2);
}

static void lobby_setup(void)
{
    size_t n;
    uint8_t* l = lobby_sheet() ? lobby_layout(&n) : NULL;
    if (!l)
    {
        fprintf(stderr, "[modern] the lobby's Config window is not the one known: no Display Settings there\n");
        return;
    }
    g_lobby_sheet = gbytes(S.p, (uint32_t)S.n);
    g_lobby_layout = gbytes(l, (uint32_t)n), g_lobby_size = (uint32_t)n;
    free(l);
    g_lobby_vt = gheap_alloc(4 * (VTBL_SLOTS + 1), 1);
    for (int i = 0; i < VTBL_SLOTS; ++i)
        g_lobby_slots[i] = rd32(LOBBY_VTBL + 4u * i), wr32(g_lobby_vt + 4u * i, g_lobby_slots[i]);
    wr32(g_lobby_vt + 4 * 6, thunk_for("modern", "lobby_input"));
    g_lobby_ok = g_lobby_slots[6] == LOBBY_INPUT;
}

/* ours into the lobby: its sheet, its layout in place of the game's, its handler's input */
static void lobby_install(void)
{
    if (!g_lobby_ok)
        return;
    memcpy(GUEST_PTR(g_scratch + 16), LOBBY_SHEET, 16);
    if (!guest_thiscall(SHEET_FIND, SHEETS, 1, (uint32_t[]){ g_scratch + 16 }))
        add(SHEET_ADD, SHEETS, g_lobby_sheet);
    uint32_t h = rd32(LOBBY_INST);
    if (h && rd32(h) == LOBBY_VTBL)
        wr32(h, g_lobby_vt);
    uint32_t cur = find_layout(LOBBY_NAME);
    uint32_t parsed = cur ? rd32(cur + 8) : 0, block = parsed ? rd32(parsed) : 0;
    if (cur && !(block >= g_lobby_layout && block < g_lobby_layout + g_lobby_size))
    {
        memcpy(GUEST_PTR(cur + 0x46), LOBBY_RENAMED, 16);
        if (!find_layout(LOBBY_NAME))
            add(LAYOUT_ADD, MGR, g_lobby_layout);
        fprintf(stderr, "[modern] the lobby's Config: Display Settings added\n");
    }
}

/* Ours in the game's lists: the sheet, the Config list in place of the game's (renamed), the pages.
 * The game could load its menu data again, so this is looked at now and then. */
static void install(void)
{
    if (!find_sheet())
        add(SHEET_ADD, SHEETS, g_sheet_guest);
    uint32_t cur = find_layout(CONFIG_NAME);
    /* the game's parsed window keeps a pointer to its block in the payload */
    uint32_t parsed = cur ? rd32(cur + 8) : 0, block = parsed ? rd32(parsed) : 0;
    if (cur && !(block >= g_config_guest && block < g_config_guest + g_config_size))
    {
        memcpy(GUEST_PTR(cur + 0x46), RENAMED, 16);
        if (!find_layout(CONFIG_NAME))
            add(LAYOUT_ADD, MGR, g_config_guest);
        fprintf(stderr, "[modern] Config list: Modern added\n");
    }
    for (int k = 0; k < NPAGES; ++k)
        if (!find_layout(PAGES[k].name))
            add(LAYOUT_ADD, MGR, PAGES[k].layout);
    hide_apply();
    lobby_install();
}

/* 0 when this is not the build the addresses are for */
static int setup(void)
{
    static const uint32_t CONFIG_KNOWN[7] = { FFXI_MODERN_CONFIG_SLOT0, FFXI_MODERN_CONFIG_SLOT1, 0, 0,
        FFXI_MODERN_CONFIG_SLOT4, 0, FFXI_MODERN_CONFIG_SLOT6 };
    uint32_t cfg = rd32(CONFIG_INST);
    if (!cfg || rd32(cfg) != CONFIG_VTBL || rd32(PAGE_VTBL + 16) != FFXI_MODERN_CONFIG_SLOT4)
        return 0;
    for (int i = 0; i < 7; ++i)
        if (CONFIG_KNOWN[i] && rd32(CONFIG_VTBL + 4u * i) != CONFIG_KNOWN[i])
            return 0;
    for (int k = 0; k < NPAGES; ++k)
        if (PAGES[k].spare_how == SPARE_END
                ? !guest_is(PAGES[k].spare, NO_NAME, 16) || rd32(PAGES[k].spare + 0x20) ||
                      *GUEST_PTR(PAGES[k].spare + 0x2c)
                : !guest_is(PAGES[k].spare, PAGES[k].spare_was, 16) ||
                      (PAGES[k].spare_how == SPARE_DUP && !guest_is(PAGES[k].spare - 0x2c, PAGES[k].spare_was, 16)) ||
                      find_layout(PAGES[k].spare_was))
            return 0;
    g_actions_ok = guest_is(ACTION_LISTS, ACTIONS_KNOWN, sizeof ACTIONS_KNOWN);
    lists_fill();
    size_t csize;
    uint8_t* c = build_sheet() ? config_layout(&csize) : NULL;
    if (!c)
    {
        fprintf(stderr, "[modern] the Config list in %s\\ROM\\119\\51.DAT is not the one known\n", g_game);
        return 0;
    }
    g_sheet_guest = gbytes(S.p, (uint32_t)S.n), g_nsprites = S.sprites;
    g_config_guest = gbytes(c, (uint32_t)csize), g_config_size = (uint32_t)csize;
    free(c);

    thunk_register(SHIMS);
    /* a page's handler: a Config page with our slots */
    uint32_t vt = gheap_alloc(4 * (VTBL_SLOTS + 1), 1);
    for (int i = 0; i < VTBL_SLOTS; ++i)
        wr32(vt + 4u * i, rd32(PAGE_VTBL + 4u * i));
    wr32(vt + 4 * 1, thunk_for("modern", "page_open"));
    wr32(vt + 4 * 3, thunk_for("modern", "page_close"));
    wr32(vt + 4 * 4, thunk_for("modern", "page_draw"));
    wr32(vt + 4 * 6, thunk_for("modern", "page_input"));
    for (int k = 0; k < NPAGES; ++k)
    {
        Page* p = &PAGES[k];
        size_t n;
        uint8_t* l = page_layout(p, &n);
        if (!l)
            return 0;
        p->layout = gbytes(l, (uint32_t)n), p->layout_size = (uint32_t)n;
        free(l);
        p->str_name = gstr(p->name), p->str_config_help = gstr(p->config_help);
        for (int r = 0; r < p->nrows; ++r)
            p->str_help[r] = gstr(p->rows[r].help);
        uint32_t global = gheap_alloc(4, 1);
        p->handler = gheap_alloc(0x60, 1);
        guest_thiscall(PAGE_CTOR, p->handler, 0, NULL);
        wr32(p->handler, vt);
        wr32(global, p->handler);
        /* its menu table entry: flagged as the Config pages are but for cancel, which the game
         * handles (closing it) */
        uint8_t* e = GUEST_PTR(p->spare);
        memcpy(e, p->name, 16);
        memset(e + 16, 0, 16);
        wr32(p->spare + 0x20, global);
        wr32(p->spare + 0x24, 0x20000u);
        wr32(p->spare + 0x28, p->modes);
    }

    /* the Config list's handler: a copy of its vtable, select and draw ours */
    uint32_t cvt = gheap_alloc(4 * (VTBL_SLOTS + 1), 1);
    for (int i = 0; i < VTBL_SLOTS; ++i)
        g_config_slots[i] = rd32(CONFIG_VTBL + 4u * i), wr32(cvt + 4u * i, g_config_slots[i]);
    wr32(cvt + 4 * 4, thunk_for("modern", "config_draw"));
    wr32(cvt + 4 * 6, thunk_for("modern", "config_input"));
    wr32(cfg, cvt);
    g_hide_ready = 1;
    if (!g_actions_ok)
        fprintf(stderr, "[modern] the combat menu's lists are not the ones known: its items stay\n");
    fprintf(stderr, "[modern] Config > Modern ready (%d sprites, %zu bytes)\n", g_nsprites, S.n);
    lobby_setup();
    return 1;
}

/* a COM object of the game's let go (IUnknown::Release, stdcall) */
static void com_release(uint32_t o)
{
    if (o)
        guest_thiscall(rd32(rd32(o) + 8), 0, 1, (uint32_t[]){ o });
}

/* The menus at mw x mh: the menu target the game draws its interface into made again at that size, as
 * at its start (none when it is the back buffer's: the interface drawn on it directly), and its copies
 * of the size - the registry's, its scene's and the menu manager's area, which its windows are laid
 * out and kept within. */
/* The open windows kept where they were within an area now w0 x h0 larger by dw x dh: each as its
 * layout anchors it when it opens (its window block's +0x13: 1 to the right edge, 2 the bottom, 3
 * both; 0 the top left) - its place as opened (+0x42..+0x48) and its place now moved alike. The
 * manager's list of them: +0 the first node; a node +0 the next, +0x10 its window, +0x14 set to skip. */
static void windows_follow(int w0, int h0, int w1, int h1)
{
    int dw = w1 - w0, dh = h1 - h0, n = 0;
    for (uint32_t node = rd32(MGR); node && n < 512; node = rd32(node), ++n)
    {
        uint32_t win = rd32(node + 0x10);
        if (!win || rd8(node + 0x14))
            continue;
        uint32_t layout = rd32(win + 4), parsed = layout ? rd32(layout + 8) : 0, blk = parsed ? rd32(parsed) : 0;
        if (!blk)
            continue;
        int a = rd8(blk + 0x13);
        int x = (int16_t)rd16(win + 0x52), y = (int16_t)rd16(win + 0x54), ax, ay;
        fprintf(stderr, "[modern] window %.16s: anchor %d at %d,%d, opened at %d,%d, block %d,%d, scaled %d\n",
            (const char*)GUEST_PTR(layout + 0x46), a, x, y, (int16_t)rd16(win + 0x42), (int16_t)rd16(win + 0x44),
            (int16_t)rd16(blk + 2), (int16_t)rd16(blk + 4), rd8(win + 0x99)); /* TODO: drop */
        if (rd8(win + 0x99))
        {
            /* laid out in proportion to the area (the lobby's backgrounds) */
            ax = w0 ? x * w1 / w0 - x : 0, ay = h0 ? y * h1 / h0 - y : 0;
        }
        else if (!a && x == (int16_t)rd16(win + 0x42) + (w0 - 512) / 2 && y == (int16_t)rd16(win + 0x44) + (h0 - 448) / 2)
        {
            /* centred as it opened (0x1011acc0: its place as opened plus half the area's room past 512 x 448) */
            ax = (w1 - 512) / 2 - (w0 - 512) / 2, ay = (h1 - 448) / 2 - (h0 - 448) / 2;
            if (ax || ay)
                guest_thiscall(WIN_SETPOS, win, 2, (uint32_t[]){ (uint32_t)(x + ax), (uint32_t)(y + ay) });
            continue;
        }
        else
            ax = a == 1 || a == 3 ? dw : 0, ay = a == 2 || a == 3 ? dh : 0;
        if (!ax && !ay)
            continue;
        for (int k = 0; k < 4; ++k)
            wr16(win + 0x42 + 2 * k, (uint16_t)((int16_t)rd16(win + 0x42 + 2 * k) + (k & 1 ? ay : ax)));
        guest_thiscall(WIN_SETPOS, win, 2, (uint32_t[]){ (uint32_t)(x + ax), (uint32_t)(y + ay) });
    }
    /* what lays itself out from the area once: the logs' width (the Config's chat width settings) and
     * the help line's stretch */
    static const struct
    {
        uint32_t inst;
        int setting;
    } LOGS[] = { { LOG_INST, 0x9c }, { LOG2_INST, 0xc5 } };
    for (int i = 0; i < 2; ++i)
    {
        uint32_t h = rd32(LOGS[i].inst);
        if (!h || !rd32(h + 8))
            continue;
        uint32_t pct = guest_call(CONFIG_GET, 1, (uint32_t[]){ (uint32_t)LOGS[i].setting });
        guest_thiscall(rd32(rd32(h) + 0x5c), h, 1, (uint32_t[]){ pct });
        guest_thiscall(LOG_FIT, h, 0, NULL);
    }
    /* The help line: its open (0x1013a250) makes it its layout's width plus a share of the area's past
     * 512 - measured from its width then, so not to be run again; the share is found from its width now
     * and given of the new area. */
    uint32_t help = rd32(HELP_INST), hw = help ? rd32(help + 8) : 0;
    uint32_t hl = hw ? rd32(hw + 4) : 0, hp = hl ? rd32(hl + 8) : 0, hb = hp ? rd32(hp) : 0;
    if (hb && w0 > 0)
    {
        int x1 = (int16_t)rd16(hw + 0x3a), y1 = (int16_t)rd16(hw + 0x3c), x2 = (int16_t)rd16(hw + 0x3e),
            y2 = (int16_t)rd16(hw + 0x40), lw = (int16_t)rd16(hb + 0xa);
        int width = (int)lroundf((float)(x2 - x1 - lw + 512) * (float)w1 / (float)w0) + lw - 512;
        if (width > 16)
            guest_thiscall(HELP_SETRECT, hw, 7, (uint32_t[]){ (uint32_t)x1, (uint32_t)y1, (uint32_t)width, (uint32_t)(y2 - y1), 1, 0, 0 });
    }
}

static void menu_apply(int mw, int mh)
{
    uint32_t gfx = rd32(DISP_GFX), scene = rd32(DISP_SCENE);
    if (!gfx || !scene || mw < 64 || mh < 64)
        return;
    int bw = rd16(scene + 0x10), bh = rd16(scene + 0x12);
    int w0 = (int16_t)rd16(MGR + 0x80) - (int16_t)rd16(MGR + 0x7c), h0 = (int16_t)rd16(MGR + 0x82) - (int16_t)rd16(MGR + 0x7e);
    if (mw == rd16(scene + 0x14) && mh == rd16(scene + 0x16) && (rd32(gfx + 0x1e0) != 0) == (mw != bw || mh != bh))
        return;
    com_release(rd32(gfx + 0x1e0)), com_release(rd32(gfx + 0x1e8));
    wr32(gfx + 0x1e0, 0), wr32(gfx + 0x1e8, 0);
    if (mw != bw || mh != bh)
    {
        uint32_t tex = guest_thiscall(DISP_RT, gfx, 5, (uint32_t[]){ (uint32_t)mw, (uint32_t)mh, g_scratch + 48, 1, 0 });
        uint32_t depth = tex ? guest_thiscall(DISP_DEPTH, gfx, 2, (uint32_t[]){ (uint32_t)mw, (uint32_t)mh }) : 0;
        if (tex && depth)
            wr32(gfx + 0x1e0, tex), wr32(gfx + 0x1e8, depth);
        else
        {
            /* as the game does when it cannot: the interface on the back buffer, at its size */
            com_release(tex), com_release(depth);
            mw = bw, mh = bh;
        }
    }
    wr32(DISP_MENU_W, (uint32_t)mw), wr32(DISP_MENU_H, (uint32_t)mh);
    wr16(scene + 0x14, (uint16_t)mw), wr16(scene + 0x16, (uint16_t)mh);
    wr16(MGR + 0x80, (uint16_t)mw), wr16(MGR + 0x82, (uint16_t)mh);
    windows_follow(w0, h0, mw, mh);
}

/* The window's mode and size now, between frames (after Present): the window (user32), the back
 * buffer (d3d8), then the game's own copies of them - the registry's values it read at its start,
 * its graphics object's and its first viewport (made once, at its start), its scene's, its device's
 * present parameters. Without a menu target (the menus the back buffer's size) its menus' size
 * follows. What its size decides at its start - the menu target's - stays until it starts again. */
static void display_apply(void)
{
    if (!g_display_live)
        return;
    g_display_live = 0;
    uint32_t hwnd = d3d8_window();
    if (!hwnd || !FFXI_MODERN)
        return;
    int w = g_win_w, h = g_win_h;
    user32_set_window(hwnd, g_mode, w, h);
    if (!d3d8_resize((uint32_t)w, (uint32_t)h))
        return;
    wr32(DISP_WIN_W, (uint32_t)w), wr32(DISP_WIN_H, (uint32_t)h), wr32(DISP_MODE, (uint32_t)g_mode);
    uint32_t gfx = rd32(DISP_GFX), scene = rd32(DISP_SCENE), dev = rd32(DISP_DEVICE);
    if (gfx)
    {
        wr32(gfx + 0x15c, (uint32_t)w), wr32(gfx + 0x160, (uint32_t)h);
        if (!rd8(gfx + 0x158))
            guest_thiscall(DISP_VIEWPORT, gfx, 0, NULL);
        else
            wr32(gfx + 0xa0, (uint32_t)w), wr32(gfx + 0xa4, (uint32_t)h); /* the screen's, under the others */
    }
    if (scene)
        wr16(scene + 0x10, (uint16_t)w), wr16(scene + 0x12, (uint16_t)h);
    if (dev)
        wr32(dev + 0xc, (uint32_t)w), wr32(dev + 0x10, (uint32_t)h), wr32(dev + 0x28, g_mode != 0);
    menu_apply(g_menu_w, g_menu_h);
    fprintf(stderr, "[modern] display now: mode %d, %dx%d, menus %dx%d\n", g_mode, w, h, g_menu_w, g_menu_h);
}

void modern_frame(void)
{
    static int state; /* 0 waiting for the menus, 1 in, -1 not this build */
    static unsigned frames;
    if (state < 0 || ++frames % 30)
        return;
    if (!g_scratch)
        g_scratch = gheap_alloc(64, 1);
    if (state == 0)
    {
        /* Without this build's addresses there is nothing to call: a wrong one need not even be a
         * function (FATAL: no translation). */
        if (!FFXI_MODERN)
        {
            fprintf(stderr, "[modern] build %s has no menu addresses (meta/builds.json): no Config > Modern\n",
                FFXI_BUILD);
            state = -1;
            return;
        }
        /* the menus are there once the Config list's layout is, or the lobby's Config window's */
        if (!find_layout(CONFIG_NAME) && !find_layout(LOBBY_NAME))
            return;
        if (!setup())
        {
            fprintf(stderr, "[modern] this build's menus are not the ones known: no Config > Modern\n");
            state = -1;
            return;
        }
        state = 1;
    }
    if (frames % 120 == 0 || state == 1)
    {
        install();
        state = 2;
    }
}

void modern_init(const ModernSetup* setup)
{
    g_setup = *setup;
    d3d8_set_after_present(display_apply);
    SDL_strlcpy(g_game, setup->game ? setup->game : "", sizeof g_game);
    if (setup->data_dir)
        SDL_strlcpy(g_data_dir, setup->data_dir, sizeof g_data_dir);
    else
    {
        /* the sign-in screen's default */
        char* pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
        SDL_strlcpy(g_data_dir, pref ? pref : "", sizeof g_data_dir);
        SDL_free(pref);
    }
    g_ui_aspect = user32_ui_aspect();
    if (setup->settings_reg)
        SDL_strlcpy(g_settings_reg, setup->settings_reg, sizeof g_settings_reg);
    g_win_w = settings_dword("0001", 1280), g_win_h = settings_dword("0002", 720);
    g_bg = settings_dword("0003", 4096);
    g_mode = settings_dword("0034", 1);
    g_menu_w = settings_dword("0037", g_win_w), g_menu_h = settings_dword("0038", g_win_h);
    char path[1100], buf[64];
    join(g_data_dir, "modern.cfg", path, sizeof path);
    if (!setup->fps_given && setup->fps_divisor && cfg_value(path, "fps_divisor", buf, sizeof buf))
    {
        long d = strtol(buf, NULL, 10);
        if (d >= 1 && d <= 4)
            *setup->fps_divisor = (uint32_t)d;
    }
    if (!setup->ui_aspect_given && cfg_value(path, "ui_aspect", buf, sizeof buf))
    {
        float v = (float)atof(buf);
        if (v == 0.0f || (v >= 0.5f && v <= 8.0f))
            g_ui_aspect = v, user32_set_ui_aspect(v);
    }
    if (cfg_value(path, "hide", buf, sizeof buf))
    {
        g_hide = (unsigned)strtoul(buf, NULL, 10) & ((1u << NHIDE) - 1);
        if (g_hide >> HIDE_OLD_MAGIC_TRUST & 1)
            g_hide = (g_hide & ~(1u << HIDE_OLD_MAGIC_TRUST)) | 1u << HIDE_TRUST;
    }
}
