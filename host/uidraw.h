/* Screens host64 draws itself before the game runs (sign-in), through the graphics back end the
 * game will use (gfx.h), on the SDL window the game then takes over (user32_adopt_window): one
 * window, no second device. Draws datui.h's quads: the game's own UI art.
 *
 * Main thread only, and only before the game creates its device. */
#pragma once

#include <stdint.h>

#include "datui.h"
#include "gfx.h"

typedef struct UiTex
{
    char name[24];
    uint32_t w, h;
    GfxTex* gpu;
    int smooth; /* filtered and clamped (a picture), not point-sampled and wrapping (the game's art) */
} UiTex;

/* The images of one DAT, by the names its quads carry. */
typedef struct UiTexSet
{
    unsigned n;
    UiTex tex[32];
} UiTexSet;

/* Brings the back end up on sdl_window. 0 if it cannot (no Metal: nothing to draw with). */
int uidraw_open(void* sdl_window);
/* Drops the screens' back buffer; the device and the window stay up for the game. */
void uidraw_close(void);

/* Adds f's images called category/names (a NULL-terminated list) to the set: the number added. */
unsigned uidraw_load(UiTexSet* set, const DatFile* f, const char* category, const char* const* names);
/* Adds a picture from memory (RGBA, top row first) to the set as name, smoothly filtered: 1, or 0
 * if it cannot. */
int uidraw_load_rgba(UiTexSet* set, const char* name, const uint8_t* rgba, uint32_t w, uint32_t h);
/* The same for a larger drawing of an image the quads address in lw x lh texels (a texture pack's
 * 4x font): it replaces the set's image of that name, if there is one. */
int uidraw_load_rgba_scaled(UiTexSet* set, const char* name, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t lw,
    uint32_t lh);
void uidraw_free(UiTexSet* set);

/* A frame: the window's size in pixels (the back buffer follows it), cleared to argb. */
void uidraw_begin(uint32_t argb, int* w, int* h);
/* Quads whose images are in set, in order, blended over what is there. Quads of images not in the
 * set are skipped. */
void uidraw_quads(const UiTexSet* set, const UiQuad* q, unsigned n);
/* A flat rectangle, argb (for fields' carets and the like). */
void uidraw_rect(float x0, float y0, float x1, float y1, uint32_t argb);
void uidraw_end(void);
