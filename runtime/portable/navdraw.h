/* Navmesh debug overlay: draws the server's navmesh (navgen's .navdraw files, navgen
 * docs/FORMATS.md §6) over the 3D scene. Off by default and free while off.
 *
 *   F10              toggle the overlay (XIV_NAVDRAW=1 starts it on, XIV_NAVDRAW=0 disables it and F10)
 *   Shift+F10        show / hide pruned polys (hidden by default)
 *   Shift+F11        show / hide the region's other zones (XIV_NAVDRAW_NEIGHBOURS=0 starts them hidden)
 *   Shift+F9         zone geometry (terrain, objects) in wireframe, characters solid
 *                    (XIV_WIREFRAME=1 starts it on; works with XIV_NAVDRAW=0 too)
 *   XIV_NAVDRAW_DIR  folder with index.tsv and the .navdraw files (default: the workspace's navmeshes/draw)
 *   XIV_NAVDRAW_ZONE zone name (fst0Field01) or zone id (150): overrides the client's own zone (its
 *                    map's place id); before the client has a map, the Meteor admin API's
 *                    (127.0.0.1:1990 /sessions) is used, XIV_NAVDRAW_ADMIN=0 turns that off
 *   XIV_NAVDRAW_LIFT metres the overlay is raised over the mesh (default 0.08)
 *   XIV_NAVDRAW_ZONES zones.json (collision/zones.json in the workspace): each zone's region
 *   XIV_NAVDRAW_RADIUS metres from the camera within which the region's other zones load (default 1000)
 *   XIV_NAVDRAW_BUDGET_MB cap on the loaded vertices, all zones together (default 320)
 *   XIV_NAVDRAW_SYNC=1 loads on the render thread instead of the loader thread
 *
 * The client keeps a whole region's layouts loaded (fields and the dungeons inside them), so the
 * overlay draws every zone of the current zone's region (zones.json regionId), the others faded.
 * Files load on a thread, lazily as the camera comes within the radius of a zone's bounds, and are
 * dropped when it moves away or the region changes.
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
/* 1 while zone geometry draws in wireframe (Shift+F9; the D3D9 back end picks the draws). */
extern volatile int navdraw_wire_bg;
/* A key from the window (user32.c, before the game sees it). Returns 1 when it was the overlay's. */
int navdraw_key(int sdl_scancode, int down, int shift);

/* Geometry for the renderer: one per loaded zone. Vertices are expanded per primitive: tris (x y z
 * rgba8, 16 bytes, 3 per triangle), lines (same, 2 per segment). The first `tris_shown` /
 * `lines_shown` vertices are the visible ones (pruned last). `id` is unique to one load, so GPU
 * copies key on it. The vertex arrays are page aligned, `tris_bytes` / `lines_bytes` long (page
 * multiples), so the renderer can wrap them without a copy: it then takes the ones it wrapped with
 * navdraw_handoff (those pointers become NULL) and frees them with navdraw_vfree when the GPU is done. */
typedef struct NavDrawVert
{
    float x, y, z;
    uint32_t rgba;
} NavDrawVert;

typedef struct NavDrawGeom
{
    uint32_t id;
    char zone[32];
    int dim; /* another zone of the region: drawn faded */
    const NavDrawVert* tris;
    uint32_t ntris, tris_shown;   /* vertices */
    const NavDrawVert* lines;
    uint32_t nlines, lines_shown; /* vertices */
    size_t tris_bytes, lines_bytes;
} NavDrawGeom;

#define NAVDRAW_MAX 32

/* The zones to draw this frame, the faded ones first. */
typedef struct NavDrawSet
{
    uint32_t n;
    const NavDrawGeom* g[NAVDRAW_MAX];
} NavDrawSet;

/* Once a frame while navdraw_on: what to draw, or NULL when there is nothing (no zone, no file). */
const NavDrawSet* navdraw_frame(void);
void navdraw_handoff(const NavDrawGeom* g, int tris, int lines);
void navdraw_vfree(void* p, size_t bytes);
float navdraw_lift(void);

/* For tests and logs: bytes of vertices loaded (CPU or handed to the GPU), zones loaded, zones loading. */
void navdraw_stats(uint64_t* bytes, int* loaded, int* loading);
