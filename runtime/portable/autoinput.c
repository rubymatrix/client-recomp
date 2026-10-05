/* Scripted input for unattended runs: XIV_INPUT_SCRIPT=<file>, a timeline of key, gamepad and mouse
 * actions played into the game as if a player made them. See docs/scripted-input.md.
 *
 * Keys and the mouse are synthesized SDL events, pushed onto SDL's queue: the window pump turns
 * them into what the game reads (window messages, GetKeyboardState, DirectInput's keyboard state
 * and buffer) exactly as it does real ones, so a key held here is held everywhere. The gamepad is
 * input.c's scripted pad 0 (XInput and DirectInput both), present from the start while a script is
 * set. Run from the presenting thread once a frame (d3d9.c present).
 *
 * Each line: <time> <command> [args]. Time is milliseconds since the first frame, or +ms after the
 * previous line (lines appended to the file while the game runs count from when they are read, so
 * `echo "+0 stick l 0 1 3000" >> script` walks forward for 3 s now). '#' starts a comment. */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input.h"
#include "runtime.h"
#include "user32.h"

enum
{
    A_KEY,    /* a = scancode, b = down */
    A_PAD,    /* a = button bits, b = down */
    A_STICK,  /* a = 0 left / 1 right, x y = -1..1 */
    A_TRIG,   /* a = 0 left / 1 right, x = 0..1 */
    A_MOVE,   /* x y = 0..1 of the window */
    A_BUTTON, /* a = SDL button, b = down */
    A_SHOT,   /* text = path */
    A_LOG,    /* text */
    A_QUIT,
};

typedef struct Act
{
    uint64_t t; /* ms since the first frame */
    uint32_t order;
    int kind, a, b;
    float x, y;
    char text[256];
} Act;

static Act* g_acts;
static size_t g_n, g_cap;
static uint32_t g_order;
static uint64_t g_t0, g_last_t; /* g_last_t: the previous line's time, for +ms */
static FILE* g_file;
static char g_partial[1024];
static size_t g_plen;
static char g_shotdir[512];
static char g_shot_pending[600];
static XPad g_pad;
static float g_mx = 0.5f, g_my = 0.5f;
static int g_line;

static uint64_t now_ms(void) { return (rt_monotonic_ns() - g_t0) / 1000000u; }

static Act* add(uint64_t t, int kind)
{
    if (g_n == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_acts = (Act*)realloc(g_acts, g_cap * sizeof *g_acts);
    }
    Act* a = &g_acts[g_n++];
    memset(a, 0, sizeof *a);
    a->t = t, a->kind = kind, a->order = g_order++;
    return a;
}

static const struct
{
    const char* name;
    uint16_t bit;
} PAD_BUTTONS[] = {
    { "up", 0x0001 },   { "down", 0x0002 },  { "left", 0x0004 }, { "right", 0x0008 }, { "start", 0x0010 },
    { "back", 0x0020 }, { "select", 0x0020 }, { "ls", 0x0040 },   { "rs", 0x0080 },    { "lb", 0x0100 },
    { "rb", 0x0200 },   { "a", 0x1000 },     { "b", 0x2000 },    { "x", 0x4000 },     { "y", 0x8000 },
};

static int pad_bit(const char* s)
{
    for (size_t i = 0; i < sizeof PAD_BUTTONS / sizeof PAD_BUTTONS[0]; ++i)
        if (!strcasecmp(s, PAD_BUTTONS[i].name))
            return PAD_BUTTONS[i].bit;
    return 0;
}

/* A key by virtual-key code (0x0D, 13) or SDL key name ("Return", "W", "Up", "Escape") */
static int scancode_of(const char* s)
{
    char* end;
    unsigned long vk = strtoul(s, &end, 0);
    if (*s && !*end && isdigit((unsigned char)*s) && (strlen(s) > 1 || vk > 9))
        return user32_scancode_of_vk((uint32_t)vk);
    SDL_Scancode sc = SDL_GetScancodeFromName(s);
    if (sc == SDL_SCANCODE_UNKNOWN && !strcasecmp(s, "enter"))
        sc = SDL_SCANCODE_RETURN;
    if (sc == SDL_SCANCODE_UNKNOWN && !strcasecmp(s, "esc"))
        sc = SDL_SCANCODE_ESCAPE;
    return (int)sc;
}

static void parse_line(char* line, int appended)
{
    g_line++;
    char* hash = strchr(line, '#');
    if (hash)
        *hash = 0;
    char* tok[8];
    int n = 0;
    for (char* p = strtok(line, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
        tok[n++] = p;
    if (n < 2)
        return;
    uint64_t t;
    if (tok[0][0] == '+')
    {
        uint64_t base = g_last_t;
        if (appended && now_ms() > base)
            base = now_ms();
        t = base + strtoull(tok[0] + 1, NULL, 0);
    }
    else
        t = strtoull(tok[0], NULL, 0);
    g_last_t = t;
    const char* cmd = tok[1];
    /* key K [hold] | down K | up K */
    if (!strcasecmp(cmd, "key") || !strcasecmp(cmd, "down") || !strcasecmp(cmd, "up"))
    {
        int sc = n > 2 ? scancode_of(tok[2]) : 0;
        if (!sc)
        {
            rt_log("[input] line %d: unknown key '%s'\n", g_line, n > 2 ? tok[2] : "");
            return;
        }
        int up = !strcasecmp(cmd, "up");
        Act* a = add(t, A_KEY);
        a->a = sc, a->b = !up;
        if (!strcasecmp(cmd, "key"))
        {
            a = add(t + (n > 3 ? strtoull(tok[3], NULL, 0) : 120), A_KEY);
            a->a = sc, a->b = 0;
        }
    }
    /* pad BUTTON [hold] | paddown BUTTON | padup BUTTON */
    else if (!strcasecmp(cmd, "pad") || !strcasecmp(cmd, "paddown") || !strcasecmp(cmd, "padup"))
    {
        int bit = n > 2 ? pad_bit(tok[2]) : 0;
        if (!bit)
        {
            rt_log("[input] line %d: unknown pad button '%s'\n", g_line, n > 2 ? tok[2] : "");
            return;
        }
        Act* a = add(t, A_PAD);
        a->a = bit, a->b = strcasecmp(cmd, "padup") != 0;
        if (!strcasecmp(cmd, "pad"))
        {
            a = add(t + (n > 3 ? strtoull(tok[3], NULL, 0) : 150), A_PAD);
            a->a = bit, a->b = 0;
        }
    }
    /* stick l|r X Y [hold]: -1..1, Y up; without hold it stays until the next stick command */
    else if (!strcasecmp(cmd, "stick") && n >= 5)
    {
        Act* a = add(t, A_STICK);
        a->a = tolower((unsigned char)tok[2][0]) == 'r';
        a->x = strtof(tok[3], NULL), a->y = strtof(tok[4], NULL);
        if (n > 5)
        {
            int side = a->a;
            a = add(t + strtoull(tok[5], NULL, 0), A_STICK);
            a->a = side;
        }
    }
    /* trigger l|r V [hold]: 0..1 */
    else if (!strcasecmp(cmd, "trigger") && n >= 4)
    {
        Act* a = add(t, A_TRIG);
        a->a = tolower((unsigned char)tok[2][0]) == 'r';
        a->x = strtof(tok[3], NULL);
        if (n > 4)
        {
            int side = a->a;
            a = add(t + strtoull(tok[4], NULL, 0), A_TRIG);
            a->a = side;
        }
    }
    /* move X Y | click X Y [left|right|middle] [hold]: X Y are 0..1 of the window across and down */
    else if ((!strcasecmp(cmd, "move") || !strcasecmp(cmd, "click")) && n >= 4)
    {
        Act* a = add(t, A_MOVE);
        a->x = strtof(tok[2], NULL), a->y = strtof(tok[3], NULL);
        if (!strcasecmp(cmd, "click"))
        {
            int b = n > 4 && !strcasecmp(tok[4], "right") ? SDL_BUTTON_RIGHT
                : n > 4 && !strcasecmp(tok[4], "middle")  ? SDL_BUTTON_MIDDLE
                                                          : SDL_BUTTON_LEFT;
            uint64_t hold = n > 5 ? strtoull(tok[5], NULL, 0) : 100;
            a = add(t + 50, A_BUTTON);
            a->a = b, a->b = 1;
            a = add(t + 50 + hold, A_BUTTON);
            a->a = b, a->b = 0;
        }
    }
    else if (!strcasecmp(cmd, "shot") && n >= 3)
    {
        Act* a = add(t, A_SHOT);
        snprintf(a->text, sizeof a->text, "%s/%s.ppm", g_shotdir, tok[2]);
    }
    else if (!strcasecmp(cmd, "log"))
    {
        Act* a = add(t, A_LOG);
        for (int i = 2; i < n; ++i)
            snprintf(a->text + strlen(a->text), sizeof a->text - strlen(a->text), "%s%s", i > 2 ? " " : "", tok[i]);
    }
    else if (!strcasecmp(cmd, "quit"))
        add(t, A_QUIT);
    else
        rt_log("[input] line %d: unknown command '%s'\n", g_line, cmd);
}

/* New complete lines of the script (the whole file the first time, what was appended since after) */
static void read_script(int appended)
{
    if (!g_file)
        return;
    clearerr(g_file);
    int c;
    while ((c = fgetc(g_file)) != EOF)
    {
        if (c == '\n')
        {
            g_partial[g_plen] = 0;
            parse_line(g_partial, appended);
            g_plen = 0;
        }
        else if (g_plen + 1 < sizeof g_partial)
            g_partial[g_plen++] = (char)c;
    }
    if (!appended && g_plen) /* the last line of the file as written, without its newline */
    {
        g_partial[g_plen] = 0;
        parse_line(g_partial, appended);
        g_plen = 0;
    }
}

static void push_key(uint32_t win, int sc, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = win;
    e.key.scancode = (SDL_Scancode)sc;
    e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_KMOD_NONE, false);
    e.key.down = down != 0;
    SDL_PushEvent(&e);
}

static void push_mouse(uint32_t win, int ww, int wh, int button, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    float x = g_mx * (float)ww, y = g_my * (float)wh;
    if (!button)
    {
        static float px = -1, py = -1;
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.timestamp = SDL_GetTicksNS();
        e.motion.windowID = win;
        e.motion.x = x, e.motion.y = y;
        e.motion.xrel = px < 0 ? 0 : x - px, e.motion.yrel = py < 0 ? 0 : y - py;
        px = x, py = y;
    }
    else
    {
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.timestamp = SDL_GetTicksNS();
        e.button.windowID = win;
        e.button.button = (Uint8)button;
        e.button.down = down != 0;
        e.button.clicks = 1;
        e.button.x = x, e.button.y = y;
    }
    user32_force_cursor(1, x, y);
    SDL_PushEvent(&e);
}

static int16_t axis(float v)
{
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return (int16_t)(v * 32767.0f);
}

static int by_time(const void* p, const void* q)
{
    const Act *a = (const Act*)p, *b = (const Act*)q;
    return a->t != b->t ? (a->t < b->t ? -1 : 1) : (a->order < b->order ? -1 : a->order > b->order);
}

const char* autoinput_frame(void)
{
    static int init;
    if (!init)
    {
        init = 1;
        const char* path = getenv("XIV_INPUT_SCRIPT");
        if (!path || !*path)
            return NULL;
        g_t0 = rt_monotonic_ns();
        g_file = fopen(path, "r");
        if (!g_file)
        {
            rt_log("[input] cannot open XIV_INPUT_SCRIPT %s\n", path);
            return NULL;
        }
        const char* dir = getenv("XIV_INPUT_SHOTS");
        if (dir && *dir)
            snprintf(g_shotdir, sizeof g_shotdir, "%s", dir);
        else
        {
            snprintf(g_shotdir, sizeof g_shotdir, "%s", path);
            char* slash = strrchr(g_shotdir, '/');
            if (slash)
                *slash = 0;
            else
                snprintf(g_shotdir, sizeof g_shotdir, ".");
        }
        read_script(0);
        rt_log("[input] script %s: %zu actions\n", path, g_n);
    }
    if (!g_file)
        return NULL;
    static uint64_t last_read;
    uint64_t now = now_ms();
    if (now - last_read >= 100) /* lines appended since */
    {
        last_read = now;
        read_script(1);
    }
    if (!g_n)
        return NULL;
    qsort(g_acts, g_n, sizeof *g_acts, by_time);
    int ww = 0, wh = 0;
    uint32_t win = user32_game_window(&ww, &wh);
    if (!win)
        return NULL;
    size_t done = 0;
    int pad_changed = 0;
    const char* shot = NULL;
    for (; done < g_n && g_acts[done].t <= now; ++done)
    {
        Act* a = &g_acts[done];
        switch (a->kind)
        {
        case A_KEY: push_key(win, a->a, a->b); break;
        case A_PAD:
            g_pad.buttons = a->b ? (uint16_t)(g_pad.buttons | a->a) : (uint16_t)(g_pad.buttons & ~a->a);
            pad_changed = 1;
            break;
        case A_STICK:
            if (a->a)
                g_pad.rx = axis(a->x), g_pad.ry = axis(a->y);
            else
                g_pad.lx = axis(a->x), g_pad.ly = axis(a->y);
            pad_changed = 1;
            break;
        case A_TRIG:
        {
            float v = a->x < 0 ? 0 : a->x > 1 ? 1 : a->x;
            if (a->a)
                g_pad.rt = (uint8_t)(v * 255.0f);
            else
                g_pad.lt = (uint8_t)(v * 255.0f);
            pad_changed = 1;
            break;
        }
        case A_MOVE:
            g_mx = a->x, g_my = a->y;
            push_mouse(win, ww, wh, 0, 0);
            break;
        case A_BUTTON: push_mouse(win, ww, wh, a->a, a->b); break;
        case A_SHOT:
            snprintf(g_shot_pending, sizeof g_shot_pending, "%s", a->text);
            shot = g_shot_pending;
            break;
        case A_LOG: break;
        case A_QUIT:
            rt_log("[input] %llu ms: quit\n", (unsigned long long)now);
            fflush(NULL);
            _Exit(0);
        }
        if (a->kind == A_LOG)
            rt_log("[input] %llu ms: %s\n", (unsigned long long)now, a->text);
        else if (a->kind != A_MOVE && a->kind != A_SHOT)
            rt_log("[input] %llu ms: %s %d %d %.2f %.2f\n", (unsigned long long)now,
                (const char*[]){ "key", "pad", "stick", "trigger", "move", "button", "shot", "log", "quit" }[a->kind], a->a,
                a->b, a->x, a->y);
    }
    if (pad_changed)
        input_vpad_set(&g_pad);
    if (done)
    {
        memmove(g_acts, g_acts + done, (g_n - done) * sizeof *g_acts);
        g_n -= done;
    }
    return shot;
}
