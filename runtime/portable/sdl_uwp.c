/* The SDL3 calls the runtime makes (user32.c, input.c, dsound.c), for the UWP build, where SDL3 has
 * no back end. Compiled against SDL3's headers for its types, linked instead of SDL3.lib. There is one
 * window, the app's view; its input arrives through uwp_bridge.h and leaves here as SDL events. Audio
 * is a thread that pulls the mix every 10 ms, as SDL's device would - dsound.c moves the DirectSound
 * cursors, and signals the game's notification events, from that callback - and hands it to the app.
 *
 * Only Win32 calls UWP apps may make: SRW locks, threads, Sleep. */
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "uwp_bridge.h"

/* --- mutexes ---------------------------------------------------------------------------------------- */
struct SDL_Mutex
{
    CRITICAL_SECTION cs; /* recursive, as SDL's */
};

SDL_Mutex* SDL_CreateMutex(void)
{
    SDL_Mutex* m = (SDL_Mutex*)calloc(1, sizeof *m);
    if (m)
        InitializeCriticalSectionEx(&m->cs, 0, 0);
    return m;
}
void SDL_LockMutex(SDL_Mutex* m)
{
    if (m)
        EnterCriticalSection(&m->cs);
}
void SDL_UnlockMutex(SDL_Mutex* m)
{
    if (m)
        LeaveCriticalSection(&m->cs);
}

/* --- odds and ends ------------------------------------------------------------------------------- */
const char* SDL_GetError(void) { return "not available in the UWP build"; }
void SDL_free(void* p) { free(p); }
int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}
bool SDL_SetHint(const char* name, const char* value) { (void)name, (void)value; return true; }
/* no hints are set here (the ones asked for are macOS's) */
const char* SDL_GetHint(const char* name) { (void)name; return NULL; }
bool SDL_GetHintBoolean(const char* name, bool default_value) { (void)name; return default_value; }
bool SDL_Init(SDL_InitFlags flags) { (void)flags; return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return true; }
bool SDL_SetClipboardText(const char* text) { (void)text; return false; }

/* --- the event queue ------------------------------------------------------------------------------ */
#define QUEUE 1024
static SRWLOCK g_qlock = SRWLOCK_INIT;
static SDL_Event g_queue[QUEUE];
static unsigned g_qhead, g_qtail;
static char g_text[QUEUE][32]; /* SDL_EVENT_TEXT_INPUT points at these */

static int push(const SDL_Event* e)
{
    AcquireSRWLockExclusive(&g_qlock);
    unsigned next = (g_qtail + 1) % QUEUE;
    int ok = next != g_qhead;
    if (ok)
    {
        g_queue[g_qtail] = *e;
        if (e->type == SDL_EVENT_TEXT_INPUT)
        {
            snprintf(g_text[g_qtail], sizeof g_text[0], "%s", e->text.text);
            g_queue[g_qtail].text.text = g_text[g_qtail];
        }
        g_qtail = next;
    }
    ReleaseSRWLockExclusive(&g_qlock);
    return ok;
}

bool SDL_PollEvent(SDL_Event* e)
{
    AcquireSRWLockExclusive(&g_qlock);
    int any = g_qhead != g_qtail;
    if (any)
    {
        *e = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE;
    }
    ReleaseSRWLockExclusive(&g_qlock);
    return any;
}

/* --- the window ------------------------------------------------------------------------------------ */
#define WINDOW_ID 1
struct SDL_Window
{
    int w, h;
};
static SDL_Window g_window;
static volatile LONG g_view_w = 1920, g_view_h = 1080;
static int g_have_window;

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags)
{
    (void)title, (void)flags;
    g_window.w = w, g_window.h = h;
    g_have_window = 1;
    return &g_window;
}
void SDL_DestroyWindow(SDL_Window* w) { (void)w; g_have_window = 0; }
SDL_WindowID SDL_GetWindowID(SDL_Window* w) { return w ? WINDOW_ID : 0; }
bool SDL_SetWindowPosition(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; return true; }
bool SDL_SetWindowSize(SDL_Window* w, int width, int height)
{
    if (w)
        w->w = width, w->h = height;
    return true;
}
bool SDL_GetWindowSizeInPixels(SDL_Window* w, int* width, int* height)
{
    (void)w;
    if (width)
        *width = (int)g_view_w;
    if (height)
        *height = (int)g_view_h;
    return true;
}
bool SDL_GetWindowSize(SDL_Window* w, int* width, int* height) { return SDL_GetWindowSizeInPixels(w, width, height); }
SDL_Window* SDL_GetWindowFromEvent(const SDL_Event* e) { (void)e; return g_have_window ? &g_window : NULL; }
bool SDL_ShowWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_SetWindowTitle(SDL_Window* w, const char* title) { (void)w, (void)title; return true; } /* the app's */
bool SDL_HideWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_RaiseWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_StartTextInput(SDL_Window* w) { (void)w; return true; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* w) { (void)w; return 0; }
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* default_value)
{
    (void)props, (void)name;
    return default_value;
}

SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID id)
{
    static SDL_DisplayMode m;
    (void)id;
    m.displayID = 1;
    m.format = SDL_PIXELFORMAT_XRGB8888;
    m.w = (int)g_view_w, m.h = (int)g_view_h;
    m.pixel_density = 1.0f;
    m.refresh_rate = 60.0f;
    return &m;
}

void uwp_view_size(int w, int h)
{
    if (w > 0 && h > 0)
        InterlockedExchange(&g_view_w, w), InterlockedExchange(&g_view_h, h);
}

/* --- keyboard and mouse ---------------------------------------------------------------------------- */
static SRWLOCK g_mlock = SRWLOCK_INIT;
static float g_mx, g_my;
static SDL_MouseButtonFlags g_mbuttons;

static bool g_keystate[SDL_SCANCODE_COUNT];

const bool* SDL_GetKeyboardState(int* numkeys)
{
    if (numkeys)
        *numkeys = SDL_SCANCODE_COUNT;
    return g_keystate;
}

bool SDL_OpenURL(const char* url) { (void)url; return false; } /* no browser from the game on the console */

void uwp_key(int scancode, int down, int repeat)
{
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
        g_keystate[scancode] = down != 0;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = WINDOW_ID;
    e.key.scancode = (SDL_Scancode)scancode;
    e.key.down = down != 0;
    e.key.repeat = repeat != 0;
    push(&e);
}

void uwp_text(const char* utf8)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = WINDOW_ID;
    e.text.text = utf8; /* copied by push */
    push(&e);
}

void uwp_mouse_move(float x, float y)
{
    AcquireSRWLockExclusive(&g_mlock);
    g_mx = x, g_my = y;
    SDL_MouseButtonFlags b = g_mbuttons;
    ReleaseSRWLockExclusive(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = WINDOW_ID;
    e.motion.state = b;
    e.motion.x = x, e.motion.y = y;
    push(&e);
}

void uwp_mouse_button(int button, int down)
{
    AcquireSRWLockExclusive(&g_mlock);
    if (down)
        g_mbuttons |= SDL_BUTTON_MASK(button);
    else
        g_mbuttons &= ~SDL_BUTTON_MASK(button);
    float x = g_mx, y = g_my;
    ReleaseSRWLockExclusive(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = WINDOW_ID;
    e.button.button = (Uint8)button;
    e.button.down = down != 0;
    e.button.clicks = 1;
    e.button.x = x, e.button.y = y;
    push(&e);
}

void uwp_mouse_wheel(float dy)
{
    AcquireSRWLockShared(&g_mlock);
    float x = g_mx, y = g_my;
    ReleaseSRWLockShared(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = WINDOW_ID;
    e.wheel.y = dy;
    e.wheel.integer_y = (Sint32)dy;
    e.wheel.mouse_x = x, e.wheel.mouse_y = y;
    push(&e);
}

void uwp_focus(int on)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = on ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = WINDOW_ID;
    push(&e);
}

void uwp_close(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = WINDOW_ID;
    push(&e);
}

SDL_Keymod SDL_GetModState(void)
{
    /* the addon host's binds and ImGui: from the keys the app has passed in */
    SDL_Keymod m = 0;
    const bool* k = SDL_GetKeyboardState(NULL);
    if (k)
    {
        if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL])
            m |= SDL_KMOD_CTRL;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT])
            m |= SDL_KMOD_SHIFT;
        if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT])
            m |= SDL_KMOD_ALT;
    }
    return m;
}

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y)
{
    AcquireSRWLockShared(&g_mlock);
    if (x)
        *x = g_mx;
    if (y)
        *y = g_my;
    SDL_MouseButtonFlags b = g_mbuttons;
    ReleaseSRWLockShared(&g_mlock);
    return b;
}
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) { return SDL_GetMouseState(x, y); }
void SDL_WarpMouseGlobal(float x, float y) { (void)x, (void)y; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

/* --- the controller ------------------------------------------------------------------------------------ */
#define PAD_ID 1
struct SDL_Gamepad
{
    int open;
};
static SDL_Gamepad g_pad;
static SRWLOCK g_plock = SRWLOCK_INIT;
static UwpPad g_padstate;
static UwpRumble g_rumble;

void uwp_gamepad(const UwpPad* p)
{
    AcquireSRWLockExclusive(&g_plock);
    int was = g_padstate.connected;
    g_padstate = *p;
    ReleaseSRWLockExclusive(&g_plock);
    if (!was != !p->connected)
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = p->connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
        e.gdevice.which = PAD_ID;
        push(&e);
    }
}
void uwp_set_rumble(UwpRumble rumble) { g_rumble = rumble; }

static int pad_connected(void)
{
    AcquireSRWLockShared(&g_plock);
    int c = g_padstate.connected;
    ReleaseSRWLockShared(&g_plock);
    return c;
}

SDL_JoystickID* SDL_GetGamepads(int* count)
{
    int n = pad_connected() ? 1 : 0;
    SDL_JoystickID* ids = (SDL_JoystickID*)calloc(2, sizeof *ids);
    if (ids && n)
        ids[0] = PAD_ID;
    if (count)
        *count = n;
    return ids;
}
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id)
{
    if (id != PAD_ID)
        return NULL;
    g_pad.open = 1;
    return &g_pad;
}
void SDL_CloseGamepad(SDL_Gamepad* p)
{
    if (p)
        p->open = 0;
}
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID id) { return id == PAD_ID && pad_connected() ? &g_pad : NULL; }
const char* SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return "Xbox controller"; }
const char* SDL_GetGamepadName(SDL_Gamepad* p) { (void)p; return "Xbox controller"; }
bool SDL_GetGamepadButton(SDL_Gamepad* p, SDL_GamepadButton b)
{
    if (!p || b < 0 || b >= 32)
        return false;
    AcquireSRWLockShared(&g_plock);
    bool down = (g_padstate.buttons >> b) & 1u;
    ReleaseSRWLockShared(&g_plock);
    return down;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* p, SDL_GamepadAxis a)
{
    if (!p || a < 0 || a >= 6)
        return 0;
    AcquireSRWLockShared(&g_plock);
    Sint16 v = g_padstate.axes[a];
    ReleaseSRWLockShared(&g_plock);
    return v;
}
bool SDL_RumbleGamepad(SDL_Gamepad* p, Uint16 low, Uint16 high, Uint32 ms)
{
    (void)p, (void)ms;
    UwpRumble r = g_rumble;
    if (r)
        r(low, high);
    return true;
}

/* --- audio ----------------------------------------------------------------------------------------- */
#define FRAMES 480 /* 10 ms at 48 kHz: dsound.c's cursors move in steps this size */
struct SDL_AudioStream
{
    SDL_AudioStreamCallback callback;
    void* user;
    volatile LONG running;
};
static SDL_AudioStream g_stream;
static UwpAudioSink g_sink;

void uwp_set_audio_sink(UwpAudioSink sink) { g_sink = sink; }

static DWORD WINAPI audio_thread(LPVOID arg)
{
    SDL_AudioStream* s = (SDL_AudioStream*)arg;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    uint64_t done = 0; /* frames pulled so far: paced to the clock, not to Sleep's rounding */
    while (s->running)
    {
        QueryPerformanceCounter(&now);
        uint64_t due = (uint64_t)((now.QuadPart - start.QuadPart) * 48000 / freq.QuadPart);
        while (done + FRAMES <= due)
        {
            s->callback(s->user, s, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
            done += FRAMES;
        }
        Sleep(5);
    }
    return 0;
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
    void* user)
{
    (void)dev, (void)spec; /* dsound.c asks for 48 kHz stereo float */
    g_stream.callback = callback;
    g_stream.user = user;
    return &g_stream;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s)
{
    if (!s || InterlockedExchange(&s->running, 1))
        return true;
    HANDLE t = CreateThread(NULL, 0, audio_thread, s, 0, NULL);
    if (!t)
    {
        s->running = 0;
        return false;
    }
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(t);
    return true;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len)
{
    (void)s;
    UwpAudioSink sink = g_sink;
    if (sink)
        sink((const float*)buf, len / (int)(2 * sizeof(float)));
    return true;
}
