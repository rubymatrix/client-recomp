/* Navmesh debug overlay: draws the server's navmesh (navgen's .navdraw files, navgen
 * docs/FORMATS.md §6) over the 3D scene. Off by default and free while off.
 *
 *   F10              toggle the overlay (XIV_NAVDRAW=1 starts it on, XIV_NAVDRAW=0 disables it and F10)
 *   Shift+F10        show / hide pruned polys (hidden by default)
 *   XIV_NAVDRAW_DIR  folder with index.tsv and the .navdraw files (default: the workspace's navmeshes/draw)
 *   XIV_NAVDRAW_ZONE zone name (fst0Field01) or zone id (150): overrides the client's own zone (its
 *                    map's place id); before the client has a map, the Meteor admin API's
 *                    (127.0.0.1:1990 /sessions) is used, XIV_NAVDRAW_ADMIN=0 turns that off
 *   XIV_NAVDRAW_LIFT metres the overlay is raised over the mesh (default 0.08)
 *
 * The inputs it needs from the client each frame (navdraw_get_camera, navdraw_get_zone) are hooks a
 * provider sets; the scene depth comes from the D3D9 back end (gfx9_metal.m). */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The world to clip transform of the 3D scene: clip[i] = dot(m[4i..4i+3], (x, y, z, 1)) for world
 * coordinates (metres, Y up, as the server and navgen use them), D3D clip space (z 0..w).
 * Returns 0 while there is none (no scene, or the camera is not known). */
extern int (*navdraw_camera_hook)(float m[16]);
/* The zone the client is in: a server zone name, or a zone id (name empty). Returns 0 if unknown. */
extern int (*navdraw_zone_hook)(char* name, size_t n, int* zone_id);

int navdraw_get_camera(float m[16]);
int navdraw_get_zone(char* name, size_t n, int* zone_id);

/* 1 while the overlay is on (cheap: a load). */
extern volatile int navdraw_on;
/* A key from the window (user32.c, before the game sees it). Returns 1 when it was the overlay's. */
int navdraw_key(int sdl_scancode, int down, int shift);

/* Geometry for the renderer, the current zone's, loaded on zone change. Vertices are expanded per
 * primitive: tris (x y z rgba8, 16 bytes, 3 per triangle), lines (same, 2 per segment). The first
 * `tris_shown` / `lines_shown` vertices are the visible ones (pruned last). `gen` changes whenever the
 * data does, so GPU copies know to refresh. */
typedef struct NavDrawVert
{
    float x, y, z;
    uint32_t rgba;
} NavDrawVert;

typedef struct NavDrawGeom
{
    uint32_t gen;
    char zone[32];
    const NavDrawVert* tris;
    uint32_t ntris, tris_shown;   /* vertices */
    const NavDrawVert* lines;
    uint32_t nlines, lines_shown; /* vertices */
} NavDrawGeom;

/* Once a frame while navdraw_on: the current zone's geometry, or NULL when there is nothing to draw
 * (no zone, no file). */
const NavDrawGeom* navdraw_frame(void);
float navdraw_lift(void);
