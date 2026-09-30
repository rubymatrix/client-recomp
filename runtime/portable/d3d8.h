/* Direct3D 8 for 64-bit hosts (d3d8.c): the D3D8 front end the Metal renderer goes under. */
#pragma once
#include <stdint.h>

/* Registers the shims (before the images are mapped). */
void d3d8_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void d3d8_setup(void);
/* Called at every Present, on the game's thread with the guest lock held, before the frame goes
 * out: where the host adjusts per-frame game state (host64: the frame-rate divisor). */
void d3d8_set_present_hook(void (*fn)(void));
/* Called at every Present just after the frame went out, on the game's thread: between frames, where
 * the host changes the display (modern.c). */
void d3d8_set_after_present(void (*fn)(void));
/* The back buffer and its depth at a new size, the game's objects kept (the game's own copies of the
 * size are the caller's); 0 before the device exists. */
int d3d8_resize(uint32_t w, uint32_t h);
/* The device's window (0 before the device exists) */
uint32_t d3d8_window(void);
/* The size the frame is shown at: the device window's client area, else the back buffer's; 0x0
 * before the device exists. */
void d3d8_screen_size(uint32_t* w, uint32_t* h);
/* The back buffer's size (0x0 before the device exists). */
void d3d8_backbuffer_size(uint32_t* w, uint32_t* h);
/* Called at every Present after the present hook, with the back buffer the frame is in: where the
 * addon host draws its overlay (gfx_draw into it; d3d8 binds its own targets again at its next
 * draw). */
typedef struct GfxTex GfxTex;
void d3d8_set_overlay(void (*fn)(GfxTex* backbuffer, uint32_t w, uint32_t h));
/* Adds a texture pack: <dir>/<hash>_<w>x<h>.dds replacements for the game's textures (see d3d8.c,
 * tools/make_texpack.py). Before the device is created. */
void d3d8_texture_pack(const char* dir);
