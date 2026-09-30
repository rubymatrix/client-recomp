/* Win32 for addons' ffi on POSIX hosts: the functions Ashita addons call as ffi.C.<name>, with
 * the Win32 signatures as LuaJIT lays them out (LONG, DWORD, BOOL, and here DWORD_PTR and SIZE_T
 * too, are 32 bits: `long` is 32-bit in our ffi; pointers and HANDLEs are 64).
 *
 * Window handles: the game's window is the guest HWND user32.c gave it (d3d8_window()); addons get
 * it from AshitaCore (a number) and ffi.cast it to HWND, which may add the guest base, so a handle
 * is compared by its low 32 bits. Returned handles are the guest HWND as a pointer.
 * Coordinates: a client point is a back buffer pixel (what the overlay draws in); a screen point
 * is the window's position (SDL points) plus that.
 * Keys: SDL's keyboard state by virtual key code; mouse buttons from SDL.
 *
 * On Windows the real functions serve (LuaJIT's ffi.C looks in kernel32 and user32 after the
 * exe): this file is empty there. */
#if !defined(_WIN32)

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "d3d8.h"
#include "d3d_ffi.h"
#include "host.h"
#include "res.h"
#include "user32.h"

typedef struct
{
    int32_t x, y;
} WPOINT;
typedef struct
{
    int32_t left, top, right, bottom;
} WRECT;

/* --- windows -------------------------------------------------------------------------------- */

static uint32_t game_hwnd(void) { return d3d8_window(); }

static SDL_Window* window_of(const void* hwnd)
{
    uint32_t h = (uint32_t)(uintptr_t)hwnd, g = game_hwnd();
    return g && h == g ? (SDL_Window*)user32_sdl_window(g) : NULL;
}

static void* handle(void) { return (void*)(uintptr_t)game_hwnd(); }

static int focused(void)
{
    SDL_Window* w = (SDL_Window*)user32_sdl_window(game_hwnd());
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
}

/* Back buffer pixels per window point. */
static void scale(SDL_Window* w, float* sx, float* sy)
{
    uint32_t bw = 0, bh = 0;
    int ww = 0, wh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    if (w)
        SDL_GetWindowSize(w, &ww, &wh);
    *sx = ww > 0 && bw ? (float)bw / (float)ww : 1.0f;
    *sy = wh > 0 && bh ? (float)bh / (float)wh : 1.0f;
}

static void client_size(SDL_Window* w, int32_t* cw, int32_t* ch)
{
    uint32_t bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    if (bw && bh)
    {
        *cw = (int32_t)bw, *ch = (int32_t)bh;
        return;
    }
    int ww = 0, wh = 0;
    if (w)
        SDL_GetWindowSizeInPixels(w, &ww, &wh);
    *cw = ww, *ch = wh;
}

static void origin(SDL_Window* w, int32_t* x, int32_t* y)
{
    int wx = 0, wy = 0;
    if (w)
        SDL_GetWindowPosition(w, &wx, &wy);
    *x = wx, *y = wy;
}

XI_FFI void* GetForegroundWindow(void) { return focused() ? handle() : NULL; }
XI_FFI void* GetFocus(void) { return focused() ? handle() : NULL; }
XI_FFI void* GetActiveWindow(void) { return focused() ? handle() : NULL; }
XI_FFI void* GetDesktopWindow(void) { return (void*)(uintptr_t)0x10010; }

XI_FFI int32_t IsWindow(void* hwnd) { return window_of(hwnd) != NULL; }

XI_FFI int32_t IsWindowVisible(void* hwnd)
{
    SDL_Window* w = window_of(hwnd);
    return w && !(SDL_GetWindowFlags(w) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}

XI_FFI int32_t IsIconic(void* hwnd)
{
    SDL_Window* w = window_of(hwnd);
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED);
}

XI_FFI int32_t IsZoomed(void* hwnd)
{
    SDL_Window* w = window_of(hwnd);
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED);
}

XI_FFI int32_t SetForegroundWindow(void* hwnd)
{
    SDL_Window* w = window_of(hwnd);
    return w ? SDL_RaiseWindow(w) : 0;
}

XI_FFI void* SetFocus(void* hwnd)
{
    void* prev = GetFocus();
    SetForegroundWindow(hwnd);
    return prev;
}

XI_FFI void* SetActiveWindow(void* hwnd) { return SetFocus(hwnd); }

XI_FFI int32_t AttachThreadInput(uint32_t a, uint32_t b, int32_t attach)
{
    (void)a, (void)b, (void)attach;
    return 1;
}

XI_FFI int32_t GetClientRect(void* hwnd, WRECT* r)
{
    SDL_Window* w = window_of(hwnd);
    if (!w || !r)
        return 0;
    r->left = r->top = 0;
    client_size(w, &r->right, &r->bottom);
    return 1;
}

/* The client area on the screen, plus a title bar and borders when the window has them. */
XI_FFI int32_t GetWindowRect(void* hwnd, WRECT* r)
{
    SDL_Window* w = window_of(hwnd);
    if (!w || !r)
        return 0;
    int32_t x, y, cw, ch;
    origin(w, &x, &y);
    client_size(w, &cw, &ch);
    int top = 0, left = 0, bottom = 0, right = 0;
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS))
        SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right);
    r->left = x - left, r->top = y - top, r->right = x + cw + right, r->bottom = y + ch + bottom;
    return 1;
}

XI_FFI int32_t ClientToScreen(void* hwnd, WPOINT* p)
{
    SDL_Window* w = window_of(hwnd);
    if (!w || !p)
        return 0;
    int32_t x, y;
    origin(w, &x, &y);
    p->x += x, p->y += y;
    return 1;
}

XI_FFI int32_t ScreenToClient(void* hwnd, WPOINT* p)
{
    SDL_Window* w = window_of(hwnd);
    if (!w || !p)
        return 0;
    int32_t x, y;
    origin(w, &x, &y);
    p->x -= x, p->y -= y;
    return 1;
}

enum
{
    SWP_NOSIZE = 0x1,
    SWP_NOMOVE = 0x2,
    SWP_NOZORDER = 0x4,
    SWP_SHOWWINDOW = 0x40,
    SWP_HIDEWINDOW = 0x80,
};

/* Moves (x, y: the frame's top left on the screen) and sizes (cx, cy: the frame's size) the
 * game's window; HWND_TOPMOST (-1) / HWND_NOTOPMOST (-2) set always-on-top. */
XI_FFI int32_t SetWindowPos(void* hwnd, void* after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    SDL_Window* w = window_of(hwnd);
    if (!w)
        return 0;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS))
        SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right);
    if (!(flags & SWP_NOZORDER))
    {
        intptr_t a = (intptr_t)after;
        if (a == -1 || (int32_t)(uint32_t)(uintptr_t)after == -1)
            SDL_SetWindowAlwaysOnTop(w, true);
        else if (a == -2 || (int32_t)(uint32_t)(uintptr_t)after == -2)
            SDL_SetWindowAlwaysOnTop(w, false);
        else
            SDL_RaiseWindow(w);
    }
    if (!(flags & SWP_NOMOVE))
        SDL_SetWindowPosition(w, x + left, y + top);
    if (!(flags & SWP_NOSIZE) && cx > 0 && cy > 0)
    {
        /* the client area in back buffer pixels -> window points */
        float sx, sy;
        scale(w, &sx, &sy);
        int ccx = cx - left - right, ccy = cy - top - bottom;
        if (ccx > 0 && ccy > 0)
            SDL_SetWindowSize(w, (int)((float)ccx / sx + 0.5f), (int)((float)ccy / sy + 0.5f));
    }
    if (flags & SWP_SHOWWINDOW)
        SDL_ShowWindow(w);
    if (flags & SWP_HIDEWINDOW)
        SDL_MinimizeWindow(w); /* hiding the game's only window would lose it */
    return 1;
}

XI_FFI int32_t MoveWindow(void* hwnd, int32_t x, int32_t y, int32_t cx, int32_t cy, int32_t repaint)
{
    (void)repaint;
    return SetWindowPos(hwnd, NULL, x, y, cx, cy, SWP_NOZORDER);
}

XI_FFI int32_t ShowWindow(void* hwnd, int32_t cmd)
{
    SDL_Window* w = window_of(hwnd);
    if (!w)
        return 0;
    int was = IsWindowVisible(hwnd);
    switch (cmd)
    {
    case 2: case 6: case 7: SDL_MinimizeWindow(w); break;  /* SHOWMINIMIZED, MINIMIZE, SHOWMINNOACTIVE */
    case 3: SDL_MaximizeWindow(w); break;                   /* MAXIMIZE */
    case 1: case 9: SDL_RestoreWindow(w); break;            /* SHOWNORMAL, RESTORE */
    case 0: break;                                           /* HIDE: not the game's only window */
    default: SDL_ShowWindow(w); break;
    }
    return was;
}

enum
{
    GWL_STYLE = -16,
    GWL_EXSTYLE = -20,
    WS_POPUP = (int32_t)0x80000000u,
    WS_VISIBLE = 0x10000000,
    WS_OVERLAPPEDWINDOW = 0x00CF0000,
    WS_EX_TOPMOST = 0x8,
};

XI_FFI int32_t GetWindowLongA(void* hwnd, int32_t index)
{
    SDL_Window* w = window_of(hwnd);
    if (!w)
        return 0;
    SDL_WindowFlags f = SDL_GetWindowFlags(w);
    if (index == GWL_STYLE)
        return (f & SDL_WINDOW_BORDERLESS ? WS_POPUP : WS_OVERLAPPEDWINDOW) | (f & SDL_WINDOW_HIDDEN ? 0 : WS_VISIBLE);
    if (index == GWL_EXSTYLE)
        return f & SDL_WINDOW_ALWAYS_ON_TOP ? WS_EX_TOPMOST : 0;
    return 0;
}

XI_FFI int32_t SetWindowLongA(void* hwnd, int32_t index, int32_t v)
{
    SDL_Window* w = window_of(hwnd);
    if (!w)
        return 0;
    int32_t prev = GetWindowLongA(hwnd, index);
    if (index == GWL_STYLE)
        SDL_SetWindowBordered(w, (v & 0x00C00000) != 0); /* WS_CAPTION */
    else if (index == GWL_EXSTYLE)
        SDL_SetWindowAlwaysOnTop(w, (v & WS_EX_TOPMOST) != 0);
    return prev;
}

XI_FFI int32_t GetWindowLongW(void* hwnd, int32_t index) { return GetWindowLongA(hwnd, index); }
XI_FFI int32_t SetWindowLongW(void* hwnd, int32_t index, int32_t v) { return SetWindowLongA(hwnd, index, v); }

static uint32_t g_affinity; /* WDA_*: kept, not acted on (no capture exclusion here) */

XI_FFI int32_t GetWindowDisplayAffinity(void* hwnd, uint32_t* aff)
{
    if (!window_of(hwnd) || !aff)
        return 0;
    *aff = g_affinity;
    return 1;
}

XI_FFI int32_t SetWindowDisplayAffinity(void* hwnd, uint32_t aff)
{
    if (!window_of(hwnd))
        return 0;
    g_affinity = aff;
    return 1;
}

XI_FFI int32_t GetSystemMetrics(int32_t i)
{
    uint32_t w = 0, h = 0, hz = 0;
    user32_desktop_mode(&w, &h, &hz);
    switch (i)
    {
    case 0: case 16: case 78: return (int32_t)w; /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: return (int32_t)h;
    case 4: return 23;                           /* SM_CYCAPTION */
    case 5: case 6: return 1;                    /* SM_CXBORDER */
    case 32: case 33: return 4;                  /* SM_CXFRAME */
    case 43: return 3;                           /* SM_CMOUSEBUTTONS */
    case 80: return 1;                           /* SM_CMONITORS */
    default: return 0;
    }
}

/* --- keys and the mouse --------------------------------------------------------------------- */

static int vk_scancode(int vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return SDL_SCANCODE_A + (vk - 'A');
    if (vk >= '1' && vk <= '9')
        return SDL_SCANCODE_1 + (vk - '1');
    if (vk >= 0x70 && vk <= 0x7B)
        return SDL_SCANCODE_F1 + (vk - 0x70);
    if (vk >= 0x61 && vk <= 0x69)
        return SDL_SCANCODE_KP_1 + (vk - 0x61);
    switch (vk)
    {
    case '0': return SDL_SCANCODE_0;
    case 0x60: return SDL_SCANCODE_KP_0;
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x13: return SDL_SCANCODE_PAUSE;
    case 0x14: return SDL_SCANCODE_CAPSLOCK;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x21: return SDL_SCANCODE_PAGEUP;
    case 0x22: return SDL_SCANCODE_PAGEDOWN;
    case 0x23: return SDL_SCANCODE_END;
    case 0x24: return SDL_SCANCODE_HOME;
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x2C: return SDL_SCANCODE_PRINTSCREEN;
    case 0x2D: return SDL_SCANCODE_INSERT;
    case 0x2E: return SDL_SCANCODE_DELETE;
    case 0x5B: return SDL_SCANCODE_LGUI;
    case 0x5C: return SDL_SCANCODE_RGUI;
    case 0x5D: return SDL_SCANCODE_APPLICATION;
    case 0x6A: return SDL_SCANCODE_KP_MULTIPLY;
    case 0x6B: return SDL_SCANCODE_KP_PLUS;
    case 0x6D: return SDL_SCANCODE_KP_MINUS;
    case 0x6E: return SDL_SCANCODE_KP_PERIOD;
    case 0x6F: return SDL_SCANCODE_KP_DIVIDE;
    case 0x90: return SDL_SCANCODE_NUMLOCKCLEAR;
    case 0x91: return SDL_SCANCODE_SCROLLLOCK;
    case 0xA0: return SDL_SCANCODE_LSHIFT;
    case 0xA1: return SDL_SCANCODE_RSHIFT;
    case 0xA2: return SDL_SCANCODE_LCTRL;
    case 0xA3: return SDL_SCANCODE_RCTRL;
    case 0xA4: return SDL_SCANCODE_LALT;
    case 0xA5: return SDL_SCANCODE_RALT;
    case 0xBA: return SDL_SCANCODE_SEMICOLON;
    case 0xBB: return SDL_SCANCODE_EQUALS;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBD: return SDL_SCANCODE_MINUS;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    case 0xBF: return SDL_SCANCODE_SLASH;
    case 0xC0: return SDL_SCANCODE_GRAVE;
    case 0xDB: return SDL_SCANCODE_LEFTBRACKET;
    case 0xDC: return SDL_SCANCODE_BACKSLASH;
    case 0xDD: return SDL_SCANCODE_RIGHTBRACKET;
    case 0xDE: return SDL_SCANCODE_APOSTROPHE;
    default: return -1;
    }
}

static int vk_down(int vk)
{
    int n = 0;
    const bool* ks = SDL_GetKeyboardState(&n);
    switch (vk)
    {
    case 0x01: case 0x02: case 0x04: case 0x05: case 0x06:
    {
        SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
        int sdl = vk == 1 ? SDL_BUTTON_LEFT : vk == 2 ? SDL_BUTTON_RIGHT : vk == 4 ? SDL_BUTTON_MIDDLE : vk == 5 ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
        return (b & SDL_BUTTON_MASK(sdl)) != 0;
    }
    case 0x10: return vk_down(0xA0) || vk_down(0xA1);
    case 0x11: return vk_down(0xA2) || vk_down(0xA3);
    case 0x12: return vk_down(0xA4) || vk_down(0xA5);
    }
    int sc = vk_scancode(vk);
    return ks && sc >= 0 && sc < n && ks[sc];
}

/* High bit: down; low bit: toggled (Caps Lock, Num Lock, Scroll Lock). */
XI_FFI int16_t GetKeyState(int32_t vk)
{
    int16_t r = vk_down(vk & 0xFF) ? (int16_t)0x8000 : 0;
    SDL_Keymod m = SDL_GetModState();
    if ((vk == 0x14 && (m & SDL_KMOD_CAPS)) || (vk == 0x90 && (m & SDL_KMOD_NUM)) || (vk == 0x91 && (m & SDL_KMOD_SCROLL)))
        r |= 1;
    return r;
}

XI_FFI int16_t GetAsyncKeyState(int32_t vk) { return vk_down(vk & 0xFF) ? (int16_t)0x8000 : 0; }

XI_FFI int32_t GetKeyboardState(uint8_t* out)
{
    if (!out)
        return 0;
    for (int vk = 0; vk < 256; ++vk)
    {
        int16_t s = GetKeyState(vk);
        out[vk] = (uint8_t)((s & 0x8000 ? 0x80 : 0) | (s & 1));
    }
    return 1;
}

/* The cursor on the "screen": the window's position plus the cursor's back buffer pixel. */
XI_FFI int32_t GetCursorPos(WPOINT* p)
{
    if (!p)
        return 0;
    SDL_Window* w = (SDL_Window*)user32_sdl_window(game_hwnd());
    float mx = 0, my = 0;
    if (w)
    {
        float gx, gy;
        int wx, wy;
        SDL_GetGlobalMouseState(&gx, &gy);
        SDL_GetWindowPosition(w, &wx, &wy);
        float sx, sy;
        scale(w, &sx, &sy);
        mx = (gx - (float)wx) * sx, my = (gy - (float)wy) * sy;
        p->x = wx + (int32_t)mx, p->y = wy + (int32_t)my;
        return 1;
    }
    SDL_GetMouseState(&mx, &my);
    p->x = (int32_t)mx, p->y = (int32_t)my;
    return 1;
}

XI_FFI int32_t GetPhysicalCursorPos(WPOINT* p) { return GetCursorPos(p); }

/* --- process, threads, time ----------------------------------------------------------------- */

XI_FFI void* GetCurrentProcess(void) { return (void*)(intptr_t)-1; }
XI_FFI void* GetCurrentThread(void) { return (void*)(intptr_t)-2; }
XI_FFI uint32_t GetCurrentProcessId(void) { return (uint32_t)getpid(); }

XI_FFI uint32_t GetCurrentThreadId(void)
{
    static atomic_uint next = 0x1000;
    static _Thread_local uint32_t id;
    if (!id)
        id = atomic_fetch_add(&next, 4);
    return id;
}

XI_FFI uint32_t GetWindowThreadProcessId(void* hwnd, uint32_t* pid)
{
    (void)hwnd;
    if (pid)
        *pid = GetCurrentProcessId();
    return GetCurrentThreadId();
}

XI_FFI int32_t GetProcessAffinityMask(void* proc, uint32_t* mask, uint32_t* system)
{
    (void)proc;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t all = n >= 32 ? 0xFFFFFFFFu : (1u << (n > 0 ? n : 1)) - 1;
    if (mask)
        *mask = all;
    if (system)
        *system = all;
    return 1;
}

XI_FFI int32_t SetProcessAffinityMask(void* proc, uint32_t mask)
{
    (void)proc;
    xi_log_once("win32 affinity", "SetProcessAffinityMask(0x%x): ignored (the host schedules its threads)", mask);
    return mask != 0;
}

XI_FFI int32_t SetProcessWorkingSetSize(void* proc, uint32_t lo, uint32_t hi)
{
    (void)proc, (void)lo, (void)hi;
    return 1;
}

XI_FFI uint32_t GetLastError(void) { return 0; }
XI_FFI void SetLastError(uint32_t e) { (void)e; }

XI_FFI uint32_t GetTickCount(void) { return (uint32_t)(rt_monotonic_ns() / 1000000u); }
XI_FFI uint64_t GetTickCount64(void) { return rt_monotonic_ns() / 1000000u; }
XI_FFI uint32_t timeGetTime(void) { return GetTickCount(); }

XI_FFI int32_t QueryPerformanceCounter(int64_t* c)
{
    if (!c)
        return 0;
    *c = (int64_t)rt_monotonic_ns();
    return 1;
}

XI_FFI int32_t QueryPerformanceFrequency(int64_t* f)
{
    if (!f)
        return 0;
    *f = 1000000000;
    return 1;
}

/* On the game's thread: capped, so an addon can't hang the game. */
XI_FFI void Sleep(uint32_t ms)
{
    if (ms > 100)
    {
        xi_log_once("win32 sleep", "Sleep(%u) from an addon: capped at 100 ms", ms);
        ms = 100;
    }
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

XI_FFI void OutputDebugStringA(const char* s)
{
    if (s)
        xi_log("OutputDebugString: %s", s);
}

/* No modal box on the game's thread: the text goes to the log and the chat log; IDOK. */
XI_FFI int32_t MessageBoxA(void* hwnd, const char* text, const char* caption, uint32_t type)
{
    (void)hwnd, (void)type;
    xi_log("MessageBox: %s: %s", caption ? caption : "", text ? text : "");
    char line[512];
    snprintf(line, sizeof line, "%s: %s", caption ? caption : "", text ? text : "");
    xi_chat_write(4, line);
    return 1;
}

/* Only web links open (in the browser); anything else would run programs. */
XI_FFI void* ShellExecuteA(void* hwnd, const char* op, const char* file, const char* params, const char* dir, int32_t show)
{
    (void)hwnd, (void)op, (void)params, (void)dir, (void)show;
    if (file && (!strncmp(file, "http://", 7) || !strncmp(file, "https://", 8)) && SDL_OpenURL(file))
        return (void*)(uintptr_t)42;
    xi_log("ShellExecute %s: refused (only http(s) links open)", file ? file : "(null)");
    return (void*)(uintptr_t)5; /* SE_ERR_ACCESSDENIED */
}

/* NULL or the game's module: FFXiMain's base as the address Lua reads it at (host address). */
XI_FFI void* GetModuleHandleA(const char* name)
{
    uint32_t base = 0, size = 0, text = 0, tsize = 0;
    xi_image(&base, &size, &text, &tsize);
    if (!name || !strcasecmp(name, "FFXiMain.dll") || !strcasecmp(name, "FFXiMain"))
        return base ? (void*)(uintptr_t)xi_host_addr(base) : NULL;
    return NULL;
}

/* --- kernel32: code pages ---------------------------------------------------------------------- */

/* MultiByteToWideChar / WideCharToMultiByte for UTF-8 (65001), Shift-JIS (932, through the resource
 * reader's CP932 table) and the ANSI code page (0, 1252: taken as Latin-1). wchar_t is the ffi's
 * (UTF-32 here). */
typedef struct
{
    uint32_t cp;
    uint16_t sjis;
} SjisRev;
static SjisRev* g_sjis_rev;
static size_t g_sjis_nrev;

static int sjis_rev_cmp(const void* a, const void* b)
{
    uint32_t x = ((const SjisRev*)a)->cp, y = ((const SjisRev*)b)->cp;
    return x < y ? -1 : x > y;
}

static void sjis_rev_build(void)
{
    if (g_sjis_rev)
        return;
    g_sjis_rev = (SjisRev*)malloc(sizeof(SjisRev) * 64 * 190);
    if (!g_sjis_rev)
        return;
    for (unsigned lead = 0x81; lead <= 0xFC; ++lead)
    {
        if (lead > 0x9F && lead < 0xE0)
            continue;
        for (unsigned trail = 0x40; trail <= 0xFC; ++trail)
        {
            uint32_t cp = res_sjis_char((uint8_t)lead, (uint8_t)trail);
            if (cp && cp != 0xFFFD && g_sjis_nrev < 64 * 190)
                g_sjis_rev[g_sjis_nrev++] = (SjisRev){ cp, (uint16_t)(lead << 8 | trail) };
        }
    }
    qsort(g_sjis_rev, g_sjis_nrev, sizeof *g_sjis_rev, sjis_rev_cmp);
}

/* one character from a code page's bytes: its code point, and how many bytes it took */
static uint32_t mb_next(uint32_t page, const uint8_t* s, size_t n, size_t* used)
{
    uint32_t c = s[0];
    *used = 1;
    if (page == 65001)
    {
        int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c < 0x80)
            return c;
        if (!k)
            return 0xFFFD;
        c &= 0x3Fu >> k;
        size_t i = 1;
        for (; k && i < n && (s[i] & 0xC0) == 0x80; --k, ++i)
            c = (c << 6) | (s[i] & 0x3F);
        *used = i;
        return k ? 0xFFFD : c;
    }
    if (page == 932)
    {
        if (c < 0x80)
            return c;
        if (c >= 0xA1 && c <= 0xDF)
            return 0xFF61 + (c - 0xA1);
        if (n >= 2)
        {
            uint32_t u = res_sjis_char((uint8_t)c, s[1]);
            if (u)
            {
                *used = 2;
                return u;
            }
        }
        return 0x30FB;
    }
    return c; /* Latin-1 */
}

static size_t mb_put(uint32_t page, uint32_t c, uint8_t* o)
{
    if (page == 65001)
    {
        if (c < 0x80)
            return o[0] = (uint8_t)c, 1;
        if (c < 0x800)
            return o[0] = (uint8_t)(0xC0 | (c >> 6)), o[1] = (uint8_t)(0x80 | (c & 63)), 2;
        if (c < 0x10000)
            return o[0] = (uint8_t)(0xE0 | (c >> 12)), o[1] = (uint8_t)(0x80 | ((c >> 6) & 63)), o[2] = (uint8_t)(0x80 | (c & 63)), 3;
        return o[0] = (uint8_t)(0xF0 | (c >> 18)), o[1] = (uint8_t)(0x80 | ((c >> 12) & 63)), o[2] = (uint8_t)(0x80 | ((c >> 6) & 63)),
               o[3] = (uint8_t)(0x80 | (c & 63)), 4;
    }
    if (page == 932)
    {
        if (c < 0x80)
            return o[0] = (uint8_t)c, 1;
        if (c >= 0xFF61 && c <= 0xFF9F)
            return o[0] = (uint8_t)(c - 0xFF61 + 0xA1), 1;
        sjis_rev_build();
        SjisRev key = { c, 0 };
        const SjisRev* r = g_sjis_rev ? (const SjisRev*)bsearch(&key, g_sjis_rev, g_sjis_nrev, sizeof key, sjis_rev_cmp) : NULL;
        if (r)
            return o[0] = (uint8_t)(r->sjis >> 8), o[1] = (uint8_t)r->sjis, 2;
        return o[0] = '?', 1;
    }
    return o[0] = (uint8_t)(c < 256 ? c : '?'), 1;
}

static int page_ok(uint32_t page) { return page == 65001 || page == 932 || page == 0 || page == 1 || page == 3 || page == 1252 || page == 28591; }

XI_FFI int32_t MultiByteToWideChar(uint32_t page, uint32_t flags, const char* src, int32_t n, wchar_t* dst, int32_t cap)
{
    (void)flags;
    if (!src || !page_ok(page))
        return 0;
    size_t len = n < 0 ? strlen(src) + 1 : (size_t)n;
    const uint8_t* s = (const uint8_t*)src;
    int32_t out = 0;
    for (size_t i = 0; i < len;)
    {
        size_t used;
        uint32_t c = mb_next(page, s + i, len - i, &used);
        i += used;
        int units = sizeof(wchar_t) == 2 && c >= 0x10000 ? 2 : 1;
        if (cap > 0)
        {
            if (out + units > cap)
                return 0; /* ERROR_INSUFFICIENT_BUFFER */
            if (units == 2)
            {
                dst[out] = (wchar_t)(0xD800 + ((c - 0x10000) >> 10));
                dst[out + 1] = (wchar_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
            }
            else
                dst[out] = (wchar_t)c;
        }
        out += units;
    }
    return out;
}

XI_FFI int32_t WideCharToMultiByte(uint32_t page, uint32_t flags, const wchar_t* src, int32_t n, char* dst, int32_t cap,
    const char* def, int32_t* used_def)
{
    (void)flags, (void)def;
    if (!src || !page_ok(page))
        return 0;
    size_t len = 0;
    if (n < 0)
    {
        while (src[len])
            ++len;
        ++len;
    }
    else
        len = (size_t)n;
    if (used_def)
        *used_def = 0;
    int32_t out = 0;
    for (size_t i = 0; i < len; ++i)
    {
        uint32_t c = (uint32_t)src[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < len)
            c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)src[++i] - 0xDC00);
        uint8_t b[4];
        size_t k = mb_put(page, c, b);
        if (cap > 0)
        {
            if (out + (int32_t)k > cap)
                return 0;
            memcpy(dst + out, b, k);
        }
        out += (int32_t)k;
    }
    return out;
}

/* --- winmm: sounds ---------------------------------------------------------------------------- */

/* PlaySound: WAV files (or a WAV in memory) on the default output through SDL, one at a time as
 * Windows plays them; always asynchronous (never blocks the game's thread). */
static SDL_AudioStream* g_snd;
static SDL_AudioSpec g_snd_spec;

static void sound_stop(void)
{
    if (g_snd)
        SDL_ClearAudioStream(g_snd);
}

static int sound_play(const void* wav, size_t n, int from_file, const char* path)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return 0;
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    bool ok = from_file ? SDL_LoadWAV(path, &spec, &buf, &len) : SDL_LoadWAV_IO(SDL_IOFromConstMem(wav, n), true, &spec, &buf, &len);
    if (!ok)
    {
        xi_log("PlaySound %s: %s", from_file ? path : "(memory)", SDL_GetError());
        return 0;
    }
    if (g_snd && (spec.format != g_snd_spec.format || spec.channels != g_snd_spec.channels || spec.freq != g_snd_spec.freq))
    {
        SDL_DestroyAudioStream(g_snd);
        g_snd = NULL;
    }
    if (!g_snd)
    {
        g_snd = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        g_snd_spec = spec;
    }
    int r = 0;
    if (g_snd)
    {
        SDL_ClearAudioStream(g_snd);
        r = SDL_PutAudioStreamData(g_snd, buf, (int)len) && SDL_ResumeAudioStreamDevice(g_snd);
    }
    SDL_free(buf);
    return r;
}

enum
{
    SND_MEMORY = 0x4,
    SND_PURGE = 0x40,
    SND_ALIAS = 0x10000,
    SND_FILENAME = 0x20000,
    SND_RESOURCE = 0x40004,
};

XI_FFI int32_t PlaySoundA(const char* sound, void* module, uint32_t flags)
{
    (void)module;
    if (!sound || (flags & SND_PURGE))
    {
        sound_stop();
        return 1;
    }
    if ((flags & SND_RESOURCE) == SND_RESOURCE || (flags & SND_ALIAS))
    {
        xi_log_once("winmm alias", "PlaySound of a resource or system alias: not supported");
        return 0;
    }
    if (flags & SND_MEMORY)
    {
        const uint8_t* p = (const uint8_t*)sound;
        uint32_t size = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        return memcmp(p, "RIFF", 4) ? 0 : sound_play(p, (size_t)size + 8, 0, NULL);
    }
    char host[1200];
    xi_host_path(sound, host, sizeof host);
    return sound_play(NULL, 0, 1, host);
}

XI_FFI int32_t PlaySoundW(const wchar_t* sound, void* module, uint32_t flags)
{
    if (!sound || (flags & SND_MEMORY))
        return PlaySoundA((const char*)sound, module, flags);
    char u[1200];
    size_t k = 0;
    for (size_t i = 0; sound[i] && k + 4 < sizeof u; ++i)
    {
        uint32_t c = (uint32_t)sound[i];
        if (c < 0x80)
            u[k++] = (char)c;
        else if (c < 0x800)
            u[k++] = (char)(0xC0 | (c >> 6)), u[k++] = (char)(0x80 | (c & 63));
        else
            u[k++] = (char)(0xE0 | ((c >> 12) & 15)), u[k++] = (char)(0x80 | ((c >> 6) & 63)), u[k++] = (char)(0x80 | (c & 63));
    }
    u[k] = 0;
    return PlaySoundA(u, module, flags);
}

XI_FFI int32_t sndPlaySoundA(const char* sound, uint32_t flags)
{
    /* SND_MEMORY is the same bit; a name is a file */
    return PlaySoundA(sound, NULL, (flags & SND_MEMORY) ? flags : flags | SND_FILENAME);
}

/* MCI command strings: not supported; the error Windows gives for a device it doesn't have. */
XI_FFI uint32_t mciSendStringA(const char* cmd, char* ret, uint32_t n, void* cb)
{
    (void)cb;
    xi_log_once("winmm mci", "mciSendString(\"%s\"): not supported", cmd ? cmd : "");
    if (ret && n)
        ret[0] = 0;
    return 263; /* MCIERR_INVALID_DEVICE_NAME */
}

XI_FFI uint32_t waveOutGetNumDevs(void) { return 1; }

#endif /* !_WIN32 */
