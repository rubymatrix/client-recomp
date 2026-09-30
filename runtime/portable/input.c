/* Input state for the DirectInput layer. See input.h.
 *
 * Keys are kept by DIK code - the PC set-1 scan code, with 0x80 for the E0-prefixed keys - which
 * is what DirectInput's keyboard reports whatever the layout. The mouse is relative motion, as
 * DirectInput's mouse is. Gamepads are SDL gamepads, presented in the layout an Xbox controller
 * has under DirectInput (the one FFXI's gamepad configuration is written around). */
#include <SDL3/SDL.h>
#include <string.h>

#include "input.h"
#include "runtime.h"

#define RING 1024

static SDL_Mutex* g_lock;
static uint8_t g_keys[256];
static int32_t g_dx, g_dy, g_dz;
static uint8_t g_buttons[8];

static struct
{
    InputEvent ev[RING];
    uint32_t seq; /* the next sequence number; events are ev[seq % RING] */
    void (*notify)(int kind);
} g_ring[INPUT_KINDS];

static uint32_t g_seq = 1; /* DirectInput's sequence numbers are shared by every device */

static void lock(void)
{
    if (!g_lock)
        g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
}

static void unlock(void) { SDL_UnlockMutex(g_lock); }

static uint8_t dik_of(SDL_Scancode sc);
uint8_t input_dik(int sdl_scancode) { return dik_of((SDL_Scancode)sdl_scancode); }

static uint8_t dik_of(SDL_Scancode sc)
{
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        char c = (char)('A' + (sc - SDL_SCANCODE_A));
        for (int i = 0; row1[i]; ++i)
            if (row1[i] == c)
                return (uint8_t)(0x10 + i);
        for (int i = 0; row2[i]; ++i)
            if (row2[i] == c)
                return (uint8_t)(0x1E + i);
        for (int i = 0; row3[i]; ++i)
            if (row3[i] == c)
                return (uint8_t)(0x2C + i);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) /* 1..9, 0 */
        return (uint8_t)(0x02 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10)
        return (uint8_t)(0x3B + (sc - SDL_SCANCODE_F1));
    switch (sc)
    {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C;
    case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_PRINTSCREEN: return 0xB7;
    case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_PAUSE: return 0xC5;
    case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD;
    case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3;
    case SDL_SCANCODE_LGUI: return 0xDB;
    case SDL_SCANCODE_RGUI: return 0xDC;
    case SDL_SCANCODE_APPLICATION: return 0xDD;
    default: return 0;
    }
}

/* under the lock */
static void push(int kind, uint32_t ofs, uint32_t data)
{
    InputEvent* e = &g_ring[kind].ev[g_ring[kind].seq % RING];
    e->ofs = ofs;
    e->data = data;
    e->time = (uint32_t)(rt_monotonic_ns() / 1000000u);
    e->seq = g_seq++;
    g_ring[kind].seq++;
}

void input_sdl_event(const void* ev)
{
    const SDL_Event* e = (const SDL_Event*)ev;
    int kind = -1;
    lock();
    switch (e->type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        uint8_t k = dik_of(e->key.scancode);
        uint8_t v = e->type == SDL_EVENT_KEY_DOWN ? 0x80 : 0;
        if (k && (g_keys[k] != v)) /* auto-repeat is not a DirectInput event */
        {
            g_keys[k] = v;
            push(INPUT_KEYBOARD, k, v);
            kind = INPUT_KEYBOARD;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
    {
        int32_t dx = (int32_t)e->motion.xrel, dy = (int32_t)e->motion.yrel;
        g_dx += dx, g_dy += dy;
        if (dx)
            push(INPUT_MOUSE, 0, (uint32_t)dx);
        if (dy)
            push(INPUT_MOUSE, 4, (uint32_t)dy);
        kind = INPUT_MOUSE;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        int32_t dz = (int32_t)(e->wheel.y * 120); /* WHEEL_DELTA per notch */
        g_dz += dz;
        push(INPUT_MOUSE, 8, (uint32_t)dz);
        kind = INPUT_MOUSE;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        /* SDL: 1 left, 2 middle, 3 right; DirectInput: 0 left, 1 right, 2 middle */
        static const int map[] = { -1, 0, 2, 1, 3, 4 };
        int b = e->button.button < 6 ? map[e->button.button] : -1;
        if (b >= 0)
        {
            g_buttons[b] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0x80 : 0;
            push(INPUT_MOUSE, 12u + (uint32_t)b, g_buttons[b]);
            kind = INPUT_MOUSE;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!SDL_OpenGamepad(e->gdevice.which))
            rt_log("[recomp] input: cannot open gamepad: %s\n", SDL_GetError());
        else
            rt_log("[recomp] input: gamepad %s\n", SDL_GetGamepadNameForID(e->gdevice.which));
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
    {
        SDL_Gamepad* p = SDL_GetGamepadFromID(e->gdevice.which);
        if (p)
            SDL_CloseGamepad(p);
        break;
    }
    default: break;
    }
    void (*notify)(int) = kind >= 0 ? g_ring[kind].notify : NULL;
    unlock();
    if (notify)
        notify(kind);
}

void input_release_all(void)
{
    lock();
    for (int k = 0; k < 256; ++k)
        if (g_keys[k])
        {
            g_keys[k] = 0;
            push(INPUT_KEYBOARD, (uint32_t)k, 0);
        }
    for (int b = 0; b < 8; ++b)
        if (g_buttons[b])
        {
            g_buttons[b] = 0;
            push(INPUT_MOUSE, 12u + (uint32_t)b, 0);
        }
    unlock();
}

void input_keyboard(uint8_t state[256])
{
    lock();
    memcpy(state, g_keys, 256);
    unlock();
}

void input_mouse(int32_t* dx, int32_t* dy, int32_t* dz, uint8_t buttons[8], int reset)
{
    lock();
    *dx = g_dx, *dy = g_dy, *dz = g_dz;
    memcpy(buttons, g_buttons, 8);
    if (reset)
        g_dx = g_dy = g_dz = 0;
    unlock();
}

uint32_t input_cursor_now(int kind)
{
    lock();
    uint32_t c = g_ring[kind].seq;
    unlock();
    return c;
}

unsigned input_events(int kind, uint32_t* cursor, InputEvent* out, unsigned max, int* lost)
{
    lock();
    uint32_t head = g_ring[kind].seq;
    *lost = head - *cursor > RING;
    if (*lost)
        *cursor = head - RING;
    unsigned n = 0;
    while (*cursor != head && n < max)
        out[n++] = g_ring[kind].ev[(*cursor)++ % RING];
    unlock();
    return n;
}

void input_set_notify(int kind, void (*fn)(int kind))
{
    lock();
    g_ring[kind].notify = fn;
    unlock();
}

/* --- gamepads -------------------------------------------------------------------------------------- */
static SDL_Gamepad* pad(int index)
{
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_Gamepad* p = ids && index >= 0 && index < n ? SDL_GetGamepadFromID(ids[index]) : NULL;
    SDL_free(ids);
    return p;
}

int input_pad_count(void)
{
    int n = 0;
    SDL_free(SDL_GetGamepads(&n));
    return n;
}

const char* input_pad_name(int index)
{
    SDL_Gamepad* p = pad(index);
    return p ? SDL_GetGamepadName(p) : NULL;
}

static int16_t flip(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }

int input_xpad(int index, XPad* x)
{
    SDL_Gamepad* p = pad(index);
    memset(x, 0, sizeof *x);
    if (!p)
        return 0;
    static const struct
    {
        SDL_GamepadButton b;
        uint16_t bit;
    } MAP[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001 },      { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002 },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004 },    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008 },
        { SDL_GAMEPAD_BUTTON_START, 0x0010 },        { SDL_GAMEPAD_BUTTON_BACK, 0x0020 },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040 },   { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080 },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200 },
        { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 },        { SDL_GAMEPAD_BUTTON_EAST, 0x2000 },
        { SDL_GAMEPAD_BUTTON_WEST, 0x4000 },         { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 },
    };
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; ++i)
        if (SDL_GetGamepadButton(p, MAP[i].b))
            x->buttons |= MAP[i].bit;
    x->lt = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    x->rt = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    x->lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
    x->ly = flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY)); /* SDL: down is positive */
    x->rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX);
    x->ry = flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
    return 1;
}

void input_rumble(int index, uint16_t low, uint16_t high)
{
    SDL_Gamepad* p = pad(index);
    if (p)
        SDL_RumbleGamepad(p, low, high, (low || high) ? 0xFFFFFFFFu : 0);
}

int input_pad_state(int index, PadState* s)
{
    SDL_Gamepad* p = pad(index);
    memset(s, 0, sizeof *s);
    s->pov = 0xFFFFFFFFu;
    if (!p)
        return 0;
    s->x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
    s->y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    s->rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX);
    s->ry = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    /* triggers 0..32767 each: one axis, as XInput devices show in DirectInput */
    s->z = (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 2;
    static const SDL_GamepadButton order[11] = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_GUIDE };
    for (int i = 0; i < 11; ++i)
        s->buttons[i] = SDL_GetGamepadButton(p, order[i]) ? 0x80 : 0;
    int up = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP), down = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    int left = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT), right = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    int dx = right - left, dy = down - up;
    if (dx || dy)
    {
        static const uint32_t angle[3][3] = { { 31500, 0, 4500 }, { 27000, 0xFFFFFFFFu, 9000 }, { 22500, 18000, 13500 } };
        s->pov = angle[dy + 1][dx + 1];
    }
    return 1;
}
