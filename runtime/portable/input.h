/* Input state for the DirectInput layer (input.c): what SDL3 reports, kept the way DirectInput
 * reports it. Fed from the message pump (user32.c) on the window's thread; read from any guest
 * thread. */
#pragma once

#include <stdint.h>

typedef struct InputEvent
{
    uint32_t ofs, data, time, seq; /* DIDEVICEOBJECTDATA's first four fields */
} InputEvent;

enum
{
    INPUT_KEYBOARD,
    INPUT_MOUSE,
    INPUT_KINDS
};

/* The DIK code (set-1 scan code, 0x80 for E0 keys) of an SDL_Scancode; 0 if none. */
uint8_t input_dik(int sdl_scancode);
/* Called by the pump for every SDL event (an SDL_Event*). */
void input_sdl_event(const void* sdl_event);
/* The window lost focus: every key and button comes up. */
void input_release_all(void);

/* Keyboard: 256 bytes indexed by DIK code (set-1 scan code, 0x80 for the E0 keys), 0x80 = down. */
void input_keyboard(uint8_t state[256]);
/* Mouse: motion since the last call with reset set, and button states (0x80 = down). */
void input_mouse(int32_t* dx, int32_t* dy, int32_t* dz, uint8_t buttons[8], int reset);

/* Buffered events of a kind, from *cursor on (0 = only what arrives after now); returns how many
 * were copied and moves the cursor past them. *lost is set if the ring overran the cursor. */
uint32_t input_cursor_now(int kind);
unsigned input_events(int kind, uint32_t* cursor, InputEvent* out, unsigned max, int* lost);
/* Called whenever input arrives, e.g. to set a DirectInput notification event. */
void input_set_notify(int kind, void (*fn)(int kind));

/* Gamepads, in the layout an Xbox controller has under DirectInput. */
typedef struct PadState
{
    int32_t x, y, z, rx, ry; /* sticks -32768..32767; z: right trigger minus left trigger */
    uint32_t pov;            /* hundredths of a degree clockwise from up, 0xFFFFFFFF centred */
    uint8_t buttons[12];     /* A B X Y LB RB Back Start LS RS Guide - */
} PadState;

/* The same gamepad as XInput reports it (XINPUT_GAMEPAD): buttons, triggers 0..255, sticks with
 * Y up. 0 if there is no such pad. */
typedef struct XPad
{
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} XPad;

int input_xpad(int index, XPad* out);
/* Rumble: motor speeds 0..65535 (XINPUT_VIBRATION), until the next call. */
void input_rumble(int index, uint16_t low, uint16_t high);

int input_pad_count(void);
int input_pad_state(int index, PadState* out);
const char* input_pad_name(int index);
