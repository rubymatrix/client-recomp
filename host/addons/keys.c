/* Input for the addon host: every SDL event reaches user32_event_hook first (user32.c).
 *
 * FFXI reads the keyboard and mouse through DirectInput, not window messages, so whatever the
 * overlay or an addon takes must be kept from both: returning 1 here does that for presses, wheel
 * turns and typed text. Releases and motion always go through (a key held before the overlay took
 * the keyboard doesn't stick; the game's cursor follows the mouse).
 *
 * Order: ImGui (when it wants the mouse or keyboard), binds, then the addons' key and mouse
 * events (Ashita's key/mouse, Windower's keyboard/mouse). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "addons.h"
#include "d3d8.h"
#include "dinput.h"
#include "host.h"
#include "input.h"
#include "user32.h"

#define CHAT_MODE 207

/* --- key names (DIK codes) -------------------------------------------------------------------- */

static const struct
{
    const char* name;
    uint8_t dik;
} KEYS[] = {
    { "escape", 0x01 }, { "esc", 0x01 }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 },
    { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B }, { "-", 0x0C }, { "minus", 0x0C },
    { "=", 0x0D }, { "equals", 0x0D }, { "backspace", 0x0E }, { "back", 0x0E }, { "tab", 0x0F }, { "q", 0x10 },
    { "w", 0x11 }, { "e", 0x12 }, { "r", 0x13 }, { "t", 0x14 }, { "y", 0x15 }, { "u", 0x16 }, { "i", 0x17 },
    { "o", 0x18 }, { "p", 0x19 }, { "[", 0x1A }, { "lbracket", 0x1A }, { "]", 0x1B }, { "rbracket", 0x1B },
    { "enter", 0x1C }, { "return", 0x1C }, { "lctrl", 0x1D }, { "lcontrol", 0x1D }, { "a", 0x1E }, { "s", 0x1F },
    { "d", 0x20 }, { "f", 0x21 }, { "g", 0x22 }, { "h", 0x23 }, { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 },
    { ";", 0x27 }, { "semicolon", 0x27 }, { "'", 0x28 }, { "apostrophe", 0x28 }, { "`", 0x29 }, { "grave", 0x29 },
    { "lshift", 0x2A }, { "\\", 0x2B }, { "backslash", 0x2B }, { "z", 0x2C }, { "x", 0x2D }, { "c", 0x2E },
    { "v", 0x2F }, { "b", 0x30 }, { "n", 0x31 }, { "m", 0x32 }, { ",", 0x33 }, { "comma", 0x33 }, { ".", 0x34 },
    { "period", 0x34 }, { "/", 0x35 }, { "slash", 0x35 }, { "rshift", 0x36 }, { "numpad*", 0x37 },
    { "multiply", 0x37 }, { "lalt", 0x38 }, { "lmenu", 0x38 }, { "space", 0x39 }, { "capslock", 0x3A },
    { "capital", 0x3A }, { "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E }, { "f5", 0x3F },
    { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 }, { "numlock", 0x45 },
    { "scrolllock", 0x46 }, { "scroll", 0x46 }, { "numpad7", 0x47 }, { "numpad8", 0x48 }, { "numpad9", 0x49 },
    { "numpad-", 0x4A }, { "subtract", 0x4A }, { "numpad4", 0x4B }, { "numpad5", 0x4C }, { "numpad6", 0x4D },
    { "numpad+", 0x4E }, { "add", 0x4E }, { "numpad1", 0x4F }, { "numpad2", 0x50 }, { "numpad3", 0x51 },
    { "numpad0", 0x52 }, { "numpad.", 0x53 }, { "decimal", 0x53 }, { "f11", 0x57 }, { "f12", 0x58 },
    { "numpadenter", 0x9C }, { "rctrl", 0x9D }, { "rcontrol", 0x9D }, { "numpad/", 0xB5 }, { "divide", 0xB5 },
    { "sysrq", 0xB7 }, { "printscreen", 0xB7 }, { "ralt", 0xB8 }, { "rmenu", 0xB8 }, { "pause", 0xC5 },
    { "home", 0xC7 }, { "up", 0xC8 }, { "pageup", 0xC9 }, { "prior", 0xC9 }, { "left", 0xCB }, { "right", 0xCD },
    { "end", 0xCF }, { "down", 0xD0 }, { "pagedown", 0xD1 }, { "next", 0xD1 }, { "insert", 0xD2 },
    { "delete", 0xD3 }, { "lwin", 0xDB }, { "rwin", 0xDC }, { "apps", 0xDD },
};

static int dik_of_name(const char* s)
{
    for (unsigned i = 0; i < sizeof KEYS / sizeof *KEYS; ++i)
        if (!strcasecmp(KEYS[i].name, s))
            return KEYS[i].dik;
    return -1;
}

static const char* name_of_dik(uint8_t d)
{
    for (unsigned i = 0; i < sizeof KEYS / sizeof *KEYS; ++i)
        if (KEYS[i].dik == d)
            return KEYS[i].name;
    return "?";
}

/* --- binds ------------------------------------------------------------------------------------ */

enum
{
    MOD_CTRL = 1,
    MOD_ALT = 2,
    MOD_WIN = 4,
    MOD_APPS = 8,
    MOD_SHIFT = 16,
};

typedef struct Bind
{
    uint8_t dik, mods;
    int chat; /* 0 any, 1 only with the chat line closed (%), 2 only open ($) */
    int up;   /* runs on release */
    char* command;
    char text[64];
    struct Bind* next;
} Bind;
static Bind* g_binds;
static uint8_t g_down[256];

static int parse_key(const char* s, uint8_t* dik, uint8_t* mods, int* chat)
{
    *mods = 0, *chat = 0;
    for (;; ++s)
    {
        if (*s == '^')
            *mods |= MOD_CTRL;
        else if (*s == '!')
            *mods |= MOD_ALT;
        else if (*s == '@')
            *mods |= MOD_WIN;
        else if (*s == '#')
            *mods |= MOD_APPS;
        else if (*s == '~' || (*s == '+' && s[1]))
            *mods |= MOD_SHIFT;
        else if (*s == '%')
            *chat = 1;
        else if (*s == '$')
            *chat = 2;
        else
            break;
    }
    int d = dik_of_name(s);
    if (d < 0)
        return 0;
    *dik = (uint8_t)d;
    return 1;
}

int xi_bind(const char* key, const char* command, int how)
{
    uint8_t dik, mods;
    int chat;
    if (!parse_key(key, &dik, &mods, &chat))
        return 0;
    int up = how == 2;
    Bind** pp = &g_binds;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->dik == dik && (*pp)->mods == mods && (*pp)->up == up)
            break;
    if (!*pp)
        *pp = (Bind*)calloc(1, sizeof **pp);
    Bind* b = *pp;
    b->dik = dik, b->mods = mods, b->chat = chat, b->up = up;
    free(b->command);
    b->command = strdup(command);
    snprintf(b->text, sizeof b->text, "%s", key);
    return 1;
}

int xi_unbind(const char* key)
{
    if (!key)
    {
        while (g_binds)
        {
            Bind* b = g_binds;
            g_binds = b->next;
            free(b->command);
            free(b);
        }
        return 1;
    }
    uint8_t dik, mods;
    int chat, found = 0;
    if (!parse_key(key, &dik, &mods, &chat))
        return 0;
    for (Bind** pp = &g_binds; *pp;)
    {
        Bind* b = *pp;
        if (b->dik == dik && b->mods == mods)
        {
            *pp = b->next;
            free(b->command);
            free(b);
            found = 1;
            continue;
        }
        pp = &b->next;
    }
    return found;
}

void xi_bind_list(void)
{
    char line[512];
    int n = 0;
    for (Bind* b = g_binds; b; b = b->next, ++n)
    {
        snprintf(line, sizeof line, "  %s%s -> %s", b->text, b->up ? " (up)" : "", b->command);
        xi_chat_write(CHAT_MODE, line);
    }
    if (!n)
        xi_chat_write(CHAT_MODE, "no binds");
}

int xi_key_down(uint32_t dik) { return dik < 256 && g_down[dik]; }

static uint8_t mods_now(void)
{
    SDL_Keymod m = SDL_GetModState();
    uint8_t r = 0;
    if (m & SDL_KMOD_CTRL)
        r |= MOD_CTRL;
    if (m & SDL_KMOD_ALT)
        r |= MOD_ALT;
    if (m & SDL_KMOD_GUI)
        r |= MOD_WIN;
    if (m & SDL_KMOD_SHIFT)
        r |= MOD_SHIFT;
    return r;
}

static int is_modifier(uint8_t dik)
{
    return dik == 0x1D || dik == 0x9D || dik == 0x38 || dik == 0xB8 || dik == 0x2A || dik == 0x36 || dik == 0xDB ||
        dik == 0xDC;
}

/* A bound key: its command runs, and the game doesn't see the key. */
static int run_binds(uint8_t dik, int down)
{
    if (is_modifier(dik))
        return 0;
    uint8_t mods = mods_now();
    int open = xi_chat_input_open(), hit = 0;
    for (Bind* b = g_binds; b; b = b->next)
    {
        if (b->dik != dik || b->mods != mods)
            continue;
        if ((b->chat == 1 && open) || (b->chat == 2 && !open))
            continue;
        if (b->up == !down)
            xi_chat_queue(1, b->command);
        hit = 1;
    }
    return hit;
}

/* --- the hook --------------------------------------------------------------------------------- */

static float g_sx = 1, g_sy = 1; /* window points -> overlay (back buffer) pixels */

static void scale_for(int ww, int wh)
{
    uint32_t bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    g_sx = ww > 0 && bw ? (float)bw / (float)ww : 1.0f;
    g_sy = wh > 0 && bh ? (float)bh / (float)wh : 1.0f;
}

/* The mouse through ImGui and the addons: 1 to keep it from the game. */
static int mouse(int msg, float px, float py, int delta)
{
    int x = (int)(px * g_sx), y = (int)(py * g_sy);
    int taken = xi_gui_mouse(msg, x, y, delta);
    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = "mouse";
    e.msg = msg, e.x = x, e.y = y, e.delta = delta;
    e.blocked = taken;
    xi_raise(&e);
    return taken || e.blocked || e.handled;
}

/* A press the overlay took is the overlay's until it is released. */
static uint8_t g_mouse_taken;

static int hook(const void* ev, int ww, int wh)
{
    const SDL_Event* e = (const SDL_Event*)ev;
    scale_for(ww, wh);
    switch (e->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        mouse(0x200, e->motion.x, e->motion.y, 0);
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        int down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0)
            return 0;
        static const int MSG[3][2] = { { 0x202, 0x201 }, { 0x205, 0x204 }, { 0x208, 0x207 } };
        int taken = mouse(MSG[b][down], e->button.x, e->button.y, 0);
        if (down && taken)
            g_mouse_taken |= (uint8_t)(1u << b);
        if (!down)
            g_mouse_taken &= (uint8_t)~(1u << b);
        return down && taken;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        return mouse(0x20A, mx, my, (int)(e->wheel.y * 120));
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        int down = e->type == SDL_EVENT_KEY_DOWN;
        uint8_t dik = input_dik(e->key.scancode);
        int taken = xi_gui_key((uint32_t)e->key.scancode, down, dik);
        if (dik)
        {
            if (!e->key.repeat)
                g_down[dik] = (uint8_t)down;
            if (!taken && !e->key.repeat && run_binds(dik, down))
                taken = 1;
            XiEvent k;
            memset(&k, 0, sizeof k);
            k.name = "key";
            k.key = dik, k.down = down, k.blocked = taken;
            k.vk = (uint32_t)e->key.key;
            /* the message's lParam layout: repeat 1, scan code 16-23, extended 24, previous 30, up 31 */
            k.flags = 1u | ((uint32_t)(dik & 0x7F) << 16) | ((dik & 0x80) ? 1u << 24 : 0) |
                (down ? (e->key.repeat ? 1u << 30 : 0) : 0xC0000000u);
            xi_raise(&k);
            if (k.blocked || k.handled)
                taken = 1;
        }
        return down && taken;
    }
    case SDL_EVENT_TEXT_INPUT:
        return xi_gui_text(e->text.text);
    default:
        return 0;
    }
}

/* XInput, as the game reads it (dinput_xpad_hook): each button change is an xinput_button event
 * (key = the bit in wButtons, down); a press an addon blocks is kept from the game until released. */
static void xpad(uint32_t user, XPad* pad)
{
    static uint16_t last[4], held[4];
    if (user > 3)
        return;
    uint16_t now = pad->buttons, changed = (uint16_t)(now ^ last[user]);
    for (unsigned b = 0; changed && b < 16; ++b)
    {
        uint16_t m = (uint16_t)(1u << b);
        if (!(changed & m))
            continue;
        int down = (now & m) != 0;
        XiEvent e;
        memset(&e, 0, sizeof e);
        e.name = "xinput_button";
        e.key = b, e.down = down, e.id = user;
        xi_raise(&e);
        if (down && (e.blocked || e.handled))
            held[user] |= m;
        if (!down)
            held[user] &= (uint16_t)~m;
    }
    last[user] = now;
    pad->buttons &= (uint16_t)~held[user];
}

static void quit(void) { addons_shutdown(); }

void xi_input_init(void)
{
    user32_event_hook = hook;
    user32_quit_hook = quit;
    dinput_xpad_hook = xpad;
    (void)name_of_dik;
}
