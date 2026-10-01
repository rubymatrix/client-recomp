/* SDL calls that macOS only allows on the main thread, made safe from any thread.
 *
 * FINAL FANTASY XIV makes its window, pumps its messages and creates its Direct3D device on a
 * render thread of its own; AppKit traps when a window is touched off the main thread. The host
 * keeps the real main thread for SDL (it initialises video there and pumps events in a loop, which
 * runs SDL_RunOnMainThread callbacks), and every window call below goes there and waits. Event
 * reads off the main thread take what the main thread pumped (SDL_PeepEvents) instead of pumping.
 *
 * Include after SDL3/SDL.h; the SDL names below then mean these wrappers in the including file.
 */
#pragma once

#include <SDL3/SDL.h>

static inline void sdl_main_run(SDL_MainThreadCallback fn, void* arg)
{
    if (SDL_IsMainThread())
        fn(arg);
    else
        SDL_RunOnMainThread(fn, arg, true);
}

typedef struct { const char* t; int w, h; SDL_WindowFlags f; SDL_Window* r; } sdlm_create_args;
static void sdlm_create(void* p) { sdlm_create_args* a = (sdlm_create_args*)p; a->r = SDL_CreateWindow(a->t, a->w, a->h, a->f); }
static inline SDL_Window* sdlm_CreateWindow(const char* t, int w, int h, SDL_WindowFlags f)
{
    sdlm_create_args a = { t, w, h, f, NULL };
    sdl_main_run(sdlm_create, &a);
    return a.r;
}

typedef struct { SDL_Window* w; } sdlm_win_args;
static void sdlm_destroy(void* p) { SDL_DestroyWindow(((sdlm_win_args*)p)->w); }
static inline void sdlm_DestroyWindow(SDL_Window* w) { sdlm_win_args a = { w }; sdl_main_run(sdlm_destroy, &a); }

typedef struct { SDL_Window* w; int x, y; bool r; } sdlm_wii_args;
static void sdlm_size(void* p) { sdlm_wii_args* a = (sdlm_wii_args*)p; a->r = SDL_SetWindowSize(a->w, a->x, a->y); }
static void sdlm_pos(void* p) { sdlm_wii_args* a = (sdlm_wii_args*)p; a->r = SDL_SetWindowPosition(a->w, a->x, a->y); }
static inline bool sdlm_SetWindowSize(SDL_Window* w, int x, int y) { sdlm_wii_args a = { w, x, y, 0 }; sdl_main_run(sdlm_size, &a); return a.r; }
static inline bool sdlm_SetWindowPosition(SDL_Window* w, int x, int y) { sdlm_wii_args a = { w, x, y, 0 }; sdl_main_run(sdlm_pos, &a); return a.r; }

typedef struct { SDL_Window* w; bool b, r; } sdlm_wb_args;
static void sdlm_full(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_SetWindowFullscreen(a->w, a->b); }
static void sdlm_border(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_SetWindowBordered(a->w, a->b); }
static void sdlm_sync(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_SyncWindow(a->w); }
static void sdlm_show(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_ShowWindow(a->w); }
static void sdlm_hide(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_HideWindow(a->w); }
static void sdlm_raise(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_RaiseWindow(a->w); }
static void sdlm_text(void* p) { sdlm_wb_args* a = (sdlm_wb_args*)p; a->r = SDL_StartTextInput(a->w); }
static inline bool sdlm_SetWindowFullscreen(SDL_Window* w, bool b) { sdlm_wb_args a = { w, b, 0 }; sdl_main_run(sdlm_full, &a); return a.r; }
static inline bool sdlm_SetWindowBordered(SDL_Window* w, bool b) { sdlm_wb_args a = { w, b, 0 }; sdl_main_run(sdlm_border, &a); return a.r; }
static inline bool sdlm_SyncWindow(SDL_Window* w) { sdlm_wb_args a = { w, 0, 0 }; sdl_main_run(sdlm_sync, &a); return a.r; }
static inline bool sdlm_ShowWindow(SDL_Window* w) { sdlm_wb_args a = { w, 0, 0 }; sdl_main_run(sdlm_show, &a); return a.r; }
static inline bool sdlm_HideWindow(SDL_Window* w) { sdlm_wb_args a = { w, 0, 0 }; sdl_main_run(sdlm_hide, &a); return a.r; }
static inline bool sdlm_RaiseWindow(SDL_Window* w) { sdlm_wb_args a = { w, 0, 0 }; sdl_main_run(sdlm_raise, &a); return a.r; }
static inline bool sdlm_StartTextInput(SDL_Window* w) { sdlm_wb_args a = { w, 0, 0 }; sdl_main_run(sdlm_text, &a); return a.r; }

typedef struct { SDL_Window* w; const SDL_DisplayMode* m; bool r; } sdlm_mode_args;
static void sdlm_mode(void* p) { sdlm_mode_args* a = (sdlm_mode_args*)p; a->r = SDL_SetWindowFullscreenMode(a->w, a->m); }
static inline bool sdlm_SetWindowFullscreenMode(SDL_Window* w, const SDL_DisplayMode* m) { sdlm_mode_args a = { w, m, 0 }; sdl_main_run(sdlm_mode, &a); return a.r; }

typedef struct { SDL_Window* w; const char* t; bool r; } sdlm_title_args;
static void sdlm_title(void* p) { sdlm_title_args* a = (sdlm_title_args*)p; a->r = SDL_SetWindowTitle(a->w, a->t); }
static inline bool sdlm_SetWindowTitle(SDL_Window* w, const char* t) { sdlm_title_args a = { w, t, 0 }; sdl_main_run(sdlm_title, &a); return a.r; }

typedef struct { float x, y; bool r; } sdlm_warp_args;
static void sdlm_warp(void* p) { sdlm_warp_args* a = (sdlm_warp_args*)p; a->r = SDL_WarpMouseGlobal(a->x, a->y); }
static inline bool sdlm_WarpMouseGlobal(float x, float y) { sdlm_warp_args a = { x, y, 0 }; sdl_main_run(sdlm_warp, &a); return a.r; }

typedef struct { bool r; } sdlm_void_args;
static void sdlm_showc(void* p) { ((sdlm_void_args*)p)->r = SDL_ShowCursor(); }
static void sdlm_hidec(void* p) { ((sdlm_void_args*)p)->r = SDL_HideCursor(); }
static inline bool sdlm_ShowCursor(void) { sdlm_void_args a = { 0 }; sdl_main_run(sdlm_showc, &a); return a.r; }
static inline bool sdlm_HideCursor(void) { sdlm_void_args a = { 0 }; sdl_main_run(sdlm_hidec, &a); return a.r; }

typedef struct { const char* t; bool r; } sdlm_clip_args;
static void sdlm_clip(void* p) { sdlm_clip_args* a = (sdlm_clip_args*)p; a->r = SDL_SetClipboardText(a->t); }
static inline bool sdlm_SetClipboardText(const char* t) { sdlm_clip_args a = { t, 0 }; sdl_main_run(sdlm_clip, &a); return a.r; }

typedef struct { SDL_InitFlags f; bool r; } sdlm_init_args;
static void sdlm_init(void* p) { sdlm_init_args* a = (sdlm_init_args*)p; a->r = SDL_Init(a->f); }
static inline bool sdlm_Init(SDL_InitFlags f) { sdlm_init_args a = { f, 0 }; sdl_main_run(sdlm_init, &a); return a.r; }

typedef struct { float *x, *y; SDL_MouseButtonFlags r; } sdlm_gm_args;
static void sdlm_gmouse(void* p) { sdlm_gm_args* a = (sdlm_gm_args*)p; a->r = SDL_GetGlobalMouseState(a->x, a->y); }
static inline SDL_MouseButtonFlags sdlm_GetGlobalMouseState(float* x, float* y) { sdlm_gm_args a = { x, y, 0 }; sdl_main_run(sdlm_gmouse, &a); return a.r; }

typedef struct { SDL_Window* w; SDL_MetalView r; } sdlm_metal_args;
static void sdlm_metal(void* p) { sdlm_metal_args* a = (sdlm_metal_args*)p; a->r = SDL_Metal_CreateView(a->w); }
static inline SDL_MetalView sdlm_Metal_CreateView(SDL_Window* w) { sdlm_metal_args a = { w, NULL }; sdl_main_run(sdlm_metal, &a); return a.r; }

/* off the main thread an event read takes what the main thread pumped */
static inline bool sdlm_PollEvent(SDL_Event* e)
{
    if (SDL_IsMainThread())
        return SDL_PollEvent(e);
    return SDL_PeepEvents(e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1;
}

#define SDL_CreateWindow sdlm_CreateWindow
#define SDL_DestroyWindow sdlm_DestroyWindow
#define SDL_SetWindowSize sdlm_SetWindowSize
#define SDL_SetWindowPosition sdlm_SetWindowPosition
#define SDL_SetWindowFullscreen sdlm_SetWindowFullscreen
#define SDL_SetWindowBordered sdlm_SetWindowBordered
#define SDL_SyncWindow sdlm_SyncWindow
#define SDL_ShowWindow sdlm_ShowWindow
#define SDL_HideWindow sdlm_HideWindow
#define SDL_RaiseWindow sdlm_RaiseWindow
#define SDL_StartTextInput sdlm_StartTextInput
#define SDL_SetWindowFullscreenMode sdlm_SetWindowFullscreenMode
#define SDL_SetWindowTitle sdlm_SetWindowTitle
#define SDL_WarpMouseGlobal sdlm_WarpMouseGlobal
#define SDL_ShowCursor sdlm_ShowCursor
#define SDL_HideCursor sdlm_HideCursor
#define SDL_SetClipboardText sdlm_SetClipboardText
#define SDL_Init sdlm_Init
#define SDL_GetGlobalMouseState sdlm_GetGlobalMouseState
#define SDL_Metal_CreateView sdlm_Metal_CreateView
#define SDL_PollEvent sdlm_PollEvent
