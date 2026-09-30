/* Direct3D 8, D3DX and Win32 for addons' ffi (d3d_ffi.c, win32_ffi.c).
 *
 * Ashita's libs/d3d8 declares IDirect3DDevice8, IDirect3DTexture8, ID3DXSprite... with ffi.cdef
 * and calls them through `obj.lpVtbl`, and calls D3DX and Win32 as `ffi.C.<name>`. There's no
 * Direct3D on the host (the game's lives in guest memory, d3d8.c), so these are host objects: a
 * struct whose first member points at a table of host functions with the COM signatures, what
 * the ffi declarations expect (x64/arm64: __stdcall means nothing). What they draw goes into the
 * overlay (gui.cpp) in call order, after the text objects and primitives, before ImGui.
 *
 * ffi.C.<name> is dlsym(RTLD_DEFAULT) on POSIX: the functions are exported from host64 (XI_FFI).
 * On Windows LuaJIT looks in the exe's exports first, then kernel32/user32/gdi32/the CRT: D3DX
 * must be exported from host64.exe (XI_FFI is __declspec(dllexport) there); the Win32 functions
 * are the real ones (win32_ffi.c compiles to nothing on Windows).
 *
 * Types: LuaJIT's ffi takes `long` as 32 bits (third_party/luajit PATCHES.md, as on Windows), so
 * every LONG/DWORD/D3DCOLOR here is int32_t/uint32_t. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define XI_FFI __declspec(dllexport)
#else
#define XI_FFI __attribute__((visibility("default"), used))
#endif

typedef struct lua_State lua_State;

/* Adds xi.d3d8_device() (the device, a host address as a number), xi.d3d8_texture_id(tex) and
 * xi.d3d8_stats() (tests) to the xi table on top of the stack. */
void xi_d3d_ffi_open(lua_State* L);
/* The one IDirect3DDevice8 addons see. */
void* xi_d3d8_device(void);

/* --- the overlay's side (gui.cpp) ------------------------------------------------------------- */

typedef struct XiD3DVert
{
    float x, y; /* back buffer pixels */
    float u, v;
    uint32_t argb; /* D3DCOLOR */
} XiD3DVert;

/* A texture with a chosen id (0: the id is taken). The owner is the running addon. */
int xi_gui_d3d_texture_new(uint32_t id, int w, int h);
/* A rectangle of B, G, R, A bytes (A8R8G8B8 in memory). */
void xi_gui_d3d_texture_upload(uint32_t id, const uint8_t* bgra, int x, int y, int w, int h, int pitch);
void xi_gui_d3d_texture_free(uint32_t id);
int xi_gui_d3d_texture_exists(uint32_t id);
/* Counts the overlay's frames (a new one at each xi_gui_begin). */
uint32_t xi_gui_d3d_frame(void);
/* A triangle list (n vertices), textured with a gui texture (0: none), blended with D3DBLEND
 * factors when blend. */
void xi_gui_d3d_triangles(uint32_t tex, const XiD3DVert* v, uint32_t n, int blend, int src, int dst);
/* Text in a font family at size pixels: measures it (w, h) and, when draw, draws it at x, y. */
int xi_gui_d3d_text(const char* family, float size, int bold, int italic, const char* utf8, size_t len, float x,
    float y, uint32_t argb, float* w, float* h, int draw);
/* A text object or primitive drawn now, into the same list (Ashita's ManualRender fonts). */
void xi_gui_d3d_text_object(uint32_t id);
void xi_gui_d3d_prim_object(uint32_t id);
/* This frame's list: commands and vertices (tests). */
void xi_gui_d3d_stats(uint32_t* cmds, uint32_t* verts);

/* --- images and formats (d3d_image.c) --------------------------------------------------------- */

#define XI_FMT_DXT1 0x31545844u /* MAKEFOURCC('D','X','T','1') */
#define XI_FMT_DXT2 0x32545844u
#define XI_FMT_DXT3 0x33545844u
#define XI_FMT_DXT4 0x34545844u
#define XI_FMT_DXT5 0x35545844u

/* D3DXIMAGE_INFO's fields */
typedef struct XiImageInfo
{
    uint32_t width, height, depth, mips, format, type, file_format;
} XiImageInfo;

/* An image file's pixels (BGRA, top-down, width*height*4, malloc'd), or NULL. */
uint8_t* xi_image_decode(const uint8_t* p, size_t n, XiImageInfo* info);
/* Bits per pixel of a D3DFORMAT we can lock (0: not one). */
int xi_fmt_bpp(uint32_t fmt);
/* A row's bytes (a row of 4x4 blocks for DXT), and the rows of a level h pixels high. */
uint32_t xi_fmt_pitch(uint32_t fmt, uint32_t w);
uint32_t xi_fmt_rows(uint32_t fmt, uint32_t h);
void xi_fmt_to_bgra(uint32_t fmt, const uint8_t* src, int pitch, int w, int h, uint8_t* dst, int dpitch);
/* 0 for formats that can't be written (DXT). */
int xi_fmt_from_bgra(uint32_t fmt, const uint8_t* bgra, int spitch, int w, int h, uint8_t* dst, int pitch);
void xi_bgra_resize(const uint8_t* s, int sw, int sh, int spitch, uint8_t* d, int dw, int dh, int dpitch);

#ifdef __cplusplus
}
#endif
