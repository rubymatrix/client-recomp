/* The navmesh overlay's loader (runtime/portable/navdraw.c) without the game or the GPU: which zones
 * of a region it loads for a camera, what it drops when the camera, the region or the Shift+F11
 * toggle changes, and the memory that takes. Reads the workspace's navmeshes/draw and
 * collision/zones.json.
 *
 *   clang -std=c11 -O1 -g -DRT_GUEST_WINDOW -I runtime -I runtime/portable -I third_party/sdl3/include \
 *       tests/navdraw_test.c -o build/navdraw_test && build/navdraw_test
 *
 * (from recomp/; XIV_NAVDRAW_DIR / XIV_NAVDRAW_ZONES point elsewhere). Exit status 0 when every
 * check passes. */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

unsigned char* rt_guest_base;

void rt_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#include "navdraw.c"

static int g_fails;
#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            ++g_fails;                                                                                                \
        }                                                                                                             \
    } while (0)

static int t_zone = -1;
static float t_cam[3];

static int test_zone(char* name, size_t n, int* id)
{
    (void)n;
    name[0] = 0;
    *id = t_zone;
    return t_zone >= 0;
}

/* a perspective camera at t_cam looking down -z: rows 0, 1 and 3 vanish at the eye */
static int test_camera(float m[16])
{
    const float f = 1.5f, c[3] = { t_cam[0], t_cam[1], t_cam[2] };
    float r[16] = { f, 0, 0, -f * c[0], 0, f, 0, -f * c[1], 0, 0, -1.0001f, 0.25f + 1.0001f * c[2], 0, 0, -1, c[2] };
    memcpy(m, r, sizeof r);
    return 1;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* frames until nothing is loading; the slowest navdraw_frame call (the render thread's cost) */
static const NavDrawSet* settle(double* worst_ms)
{
    const NavDrawSet* set = NULL;
    double t0 = now();
    *worst_ms = 0;
    for (;;)
    {
        double a = now();
        set = navdraw_frame();
        double ms = (now() - a) * 1000;
        if (ms > *worst_ms)
            *worst_ms = ms;
        uint64_t b;
        int loaded, loading;
        navdraw_stats(&b, &loaded, &loading);
        if (!loading || now() - t0 > 60)
            break;
        usleep(2000);
    }
    return set;
}

static const NavDrawGeom* find(const NavDrawSet* set, const char* zone)
{
    for (uint32_t i = 0; set && i < set->n; ++i)
        if (!strcmp(set->g[i]->zone, zone))
            return set->g[i];
    return NULL;
}

static void show(const char* what, const NavDrawSet* set, double worst_ms)
{
    uint64_t b;
    int loaded, loading;
    navdraw_stats(&b, &loaded, &loading);
    printf("== %s: %d zones, %.1f MB, slowest frame %.2f ms:", what, loaded, b / 1048576.0, worst_ms);
    for (uint32_t i = 0; set && i < set->n; ++i)
        printf(" %s%s", set->g[i]->zone, set->g[i]->dim ? "" : "*");
    printf("\n");
}

int main(void)
{
    if (!getenv("XIV_NAVDRAW_DIR"))
        setenv("XIV_NAVDRAW_DIR", "../navmeshes/draw", 1);
    navdraw_zone_hook = test_zone;
    navdraw_camera_hook = test_camera;
    double ms;

    /* Eastern La Noscea at the Cassiopeia Hollow mouth: the cave (sea0Dungeon03) comes in */
    t_zone = 130;
    t_cam[0] = 1134, t_cam[1] = 25, t_cam[2] = -737;
    const NavDrawSet* set = settle(&ms);
    show("zone 130 at the Cassiopeia Hollow mouth", set, ms);
    CHECK(set && set->n >= 2, "zones drawn");
    CHECK(set && !set->g[set->n - 1]->dim && !strcmp(set->g[set->n - 1]->zone, "sea0Field03"), "current zone last, not dim");
    const NavDrawGeom* cave = find(set, "sea0Dungeon03");
    CHECK(cave && cave->dim && cave->tris_shown > 0, "sea0Dungeon03 loaded, dim");
    CHECK(!find(set, "sea0Field02"), "sea0Field02 (1.5 km away) not loaded");
    CHECK(find(set, "sea0Field04"), "sea0Field04 (0.6 km away) loaded");
    CHECK(ms < 20, "render-thread frame %.2f ms", ms);
    for (int i = 0; i < g_nslots; ++i)
        for (int k = i + 1; k < g_nslots; ++k)
            CHECK(strcmp(g_slots[i].file, g_slots[k].file), "%s loaded twice", g_slots[i].file);

    /* the renderer takes the cave's vertices (as gfx9_navdraw.h does), frees them itself later */
    void* taken_t = (void*)cave->tris;
    void* taken_l = (void*)cave->lines;
    size_t taken_tb = cave->tris_bytes, taken_lb = cave->lines_bytes;
    uint32_t cave_id = cave->id;
    navdraw_handoff(cave, 1, 1);
    CHECK(!cave->tris && !cave->lines && cave->tris_bytes, "handed off");

    /* the same region, the camera over Western La Noscea: the cave goes, sea0Field02 comes */
    t_cam[0] = -1500, t_cam[2] = -1000;
    set = settle(&ms);
    show("zone 130, camera at -1500, -1000", set, ms);
    CHECK(!find(set, "sea0Dungeon03"), "cave dropped");
    CHECK(find(set, "sea0Field02"), "sea0Field02 loaded");
    CHECK(find(set, "sea0Field03") && !find(set, "sea0Field03")->dim, "current zone kept");
    navdraw_vfree(taken_t, taken_tb);
    navdraw_vfree(taken_l, taken_lb);

    /* back: the cave loads again, under a new id */
    t_cam[0] = 1134, t_cam[2] = -737;
    set = settle(&ms);
    cave = find(set, "sea0Dungeon03");
    CHECK(cave && cave->id != cave_id && cave->tris, "cave reloaded");

    /* Shift+F11: the current zone only */
    CHECK(navdraw_key(SDL_SCANCODE_F11, 1, 1) == 1 && navdraw_key(SDL_SCANCODE_F11, 0, 1) == 1, "Shift+F11 taken");
    CHECK(navdraw_key(SDL_SCANCODE_F11, 1, 0) == 0 && navdraw_key(SDL_SCANCODE_F11, 0, 0) == 0, "F11 left to the game");
    set = settle(&ms);
    show("others hidden", set, ms);
    CHECK(set && set->n == 1 && !set->g[0]->dim, "the current zone only");
    navdraw_key(SDL_SCANCODE_F11, 1, 1);
    navdraw_key(SDL_SCANCODE_F11, 0, 1);

    /* a sameAs zone (205 is sea0Field03's file): nothing loads twice */
    t_zone = 205;
    set = settle(&ms);
    show("zone 205 (sameAs sea0Field03)", set, ms);
    CHECK(find(set, "sea0Field03a") && !find(set, "sea0Field03a")->dim, "current zone by its own name");
    CHECK(!find(set, "sea0Field03"), "its target not loaded again");
    CHECK(find(set, "sea0Dungeon03"), "the cave kept");

    /* another region: La Noscea's zones all go */
    t_zone = 150;
    t_cam[0] = 0, t_cam[1] = 30, t_cam[2] = -500;
    set = settle(&ms);
    show("zone 150 (the Black Shroud)", set, ms);
    for (uint32_t i = 0; set && i < set->n; ++i)
        CHECK(strncmp(set->g[i]->zone, "sea", 3), "%s still loaded", set->g[i]->zone);
    CHECK(find(set, "fst0Field01") && !find(set, "fst0Field01")->dim, "fst0Field01 current");

    printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
    return g_fails != 0;
}
