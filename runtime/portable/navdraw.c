/* Navmesh debug overlay: the data side (navdraw.h). Finds and loads the current zone's .navdraw
 * file (navgen docs/FORMATS.md §6) and expands it into coloured triangles and line segments for the
 * renderer (gfx9_metal.m draws them, depth-tested against the scene). */
#include "navdraw.h"
#include "guest.h"

#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <mach-o/dyld.h>

void rt_log(const char* fmt, ...);

int (*navdraw_camera_hook)(float m[16]);
int (*navdraw_zone_hook)(char* name, size_t n, int* zone_id);
volatile int navdraw_on;

static int g_disabled; /* XIV_NAVDRAW=0 */
static int g_show_pruned;

/* --- the file (FORMATS §6) ----------------------------------------------------------------------- */

#define XNVD_MAGIC 0x44564E58u

typedef struct NdHeader
{
    uint32_t magic, version, header_size, reserved0;
    char zone[32];
    float bmin[3], bmax[3];
    uint32_t verts, tris, edges, links, gates, reserved1;
} NdHeader;

typedef struct NdTri
{
    uint32_t v[3];
    uint8_t cls, area;
    uint16_t flags;
    int16_t gate;
    uint8_t bits, pad;
} NdTri;

typedef struct NdEdge
{
    uint32_t a, b;
    uint8_t kind, bits;
    uint16_t pad;
} NdEdge;

typedef struct NdLink
{
    float a[3], b[3];
    uint8_t kind, one_way;
    int16_t gate;
    float radius;
} NdLink;

typedef struct NdGate
{
    char id[56];
    uint8_t def, role, color, pad;
    uint32_t polys;
} NdGate;

_Static_assert(sizeof(NdHeader) == 96, "navdraw header");
_Static_assert(sizeof(NdTri) == 20, "navdraw tri");
_Static_assert(sizeof(NdEdge) == 12, "navdraw edge");
_Static_assert(sizeof(NdLink) == 32, "navdraw link");
_Static_assert(sizeof(NdGate) == 64, "navdraw gate");

static uint32_t rgba(float r, float g, float b, float a)
{
#define B(x) ((uint32_t)lroundf(fminf(fmaxf(x, 0.0f), 1.0f) * 255.0f))
    return B(r) | B(g) << 8 | B(b) << 16 | B(a) << 24;
#undef B
}

/* colour class (FORMATS §6): ground, water, player-only, gate, other */
static uint32_t face_colour(const NdTri* t, const NdGate* gates, uint32_t ngates)
{
    if (t->bits & 1)
        return rgba(0.45f, 0.45f, 0.5f, 0.22f);
    switch (t->cls)
    {
    case 0: return rgba(0.25f, 0.85f, 0.4f, 0.32f);
    case 1: return rgba(0.25f, 0.55f, 1.0f, 0.38f);
    case 2: return rgba(1.0f, 0.8f, 0.2f, 0.38f);
    case 3:
        /* closed by default: red; open or unknown: violet */
        if (t->gate >= 0 && (uint32_t)t->gate < ngates && gates[t->gate].def != 0)
            return rgba(0.75f, 0.4f, 1.0f, 0.45f);
        return rgba(1.0f, 0.25f, 0.25f, 0.45f);
    default: return rgba(0.7f, 0.7f, 0.7f, 0.3f);
    }
}

static uint32_t edge_colour(const NdEdge* e)
{
    if (e->bits & 1)
        return rgba(0.6f, 0.6f, 0.65f, 0.35f);
    switch (e->kind)
    {
    case 0: return rgba(0.95f, 1.0f, 0.95f, 0.9f);  /* boundary */
    case 2: return rgba(1.0f, 0.95f, 0.35f, 0.75f); /* area / gate / pruned change */
    default: return rgba(0.05f, 0.2f, 0.1f, 0.22f); /* internal */
    }
}

static uint32_t link_colour(const NdLink* l)
{
    if (l->gate >= 0)
        return rgba(1.0f, 0.35f, 0.35f, 0.95f);
    switch (l->kind)
    {
    case 1: return rgba(1.0f, 0.3f, 1.0f, 0.95f);  /* LIFT */
    case 2: return rgba(1.0f, 0.6f, 0.15f, 0.95f); /* DROP */
    default: return rgba(0.3f, 0.95f, 1.0f, 0.95f);
    }
}

typedef struct Geom
{
    NavDrawGeom pub;
    NavDrawVert* tris;
    NavDrawVert* lines;
    uint32_t tris_visible, lines_visible; /* without pruned */
} Geom;

static Geom g_geom;
static int g_have_geom;

static void geom_free(Geom* g)
{
    free(g->tris);
    free(g->lines);
    memset(g, 0, sizeof *g);
}

static void push_line(NavDrawVert** v, uint32_t* n, uint32_t* cap, const float a[3], const float b[3], uint32_t c)
{
    if (*n + 2 > *cap)
    {
        *cap = *cap ? *cap * 2 : 4096;
        *v = (NavDrawVert*)realloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = (NavDrawVert){ a[0], a[1], a[2], c };
    (*v)[(*n)++] = (NavDrawVert){ b[0], b[1], b[2], c };
}

/* an off-mesh link as an arc from a to b, with an arrow head at b when it is one-way */
static void push_link(NavDrawVert** v, uint32_t* n, uint32_t* cap, const NdLink* l)
{
    uint32_t c = link_colour(l);
    float dx = l->b[0] - l->a[0], dy = l->b[1] - l->a[1], dz = l->b[2] - l->a[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    float h = 0.25f * sqrtf(dx * dx + dz * dz) + 0.4f;
    enum { SEG = 16 };
    float prev[3] = { l->a[0], l->a[1], l->a[2] }, last[3] = { 0 };
    for (int i = 1; i <= SEG; ++i)
    {
        float t = (float)i / SEG;
        float p[3] = { l->a[0] + dx * t, l->a[1] + dy * t + h * 4 * t * (1 - t), l->a[2] + dz * t };
        push_line(v, n, cap, prev, p, c);
        if (i == SEG - 1)
            memcpy(last, p, sizeof last);
        memcpy(prev, p, sizeof prev);
    }
    if (l->one_way && len > 0.01f)
    {
        /* two barbs back along the arc's last segment, sideways in XZ */
        float tx = l->b[0] - last[0], ty = l->b[1] - last[1], tz = l->b[2] - last[2];
        float tl = sqrtf(tx * tx + ty * ty + tz * tz);
        if (tl > 1e-4f)
            tx /= tl, ty /= tl, tz /= tl;
        float sx = -tz, sz = tx, sl = sqrtf(sx * sx + sz * sz);
        if (sl < 1e-4f)
            sx = 1, sz = 0, sl = 1;
        sx /= sl, sz /= sl;
        float s = fminf(0.6f, 0.3f * len + 0.15f);
        for (int k = -1; k <= 1; k += 2)
        {
            float p[3] = { l->b[0] - tx * s + sx * s * 0.5f * k, l->b[1] - ty * s, l->b[2] - tz * s + sz * s * 0.5f * k };
            push_line(v, n, cap, l->b, p, c);
        }
    }
    /* the endpoints: small crosses on the ground */
    for (int e = 0; e < 2; ++e)
    {
        const float* p = e ? l->b : l->a;
        float r = 0.25f;
        float a0[3] = { p[0] - r, p[1], p[2] }, a1[3] = { p[0] + r, p[1], p[2] };
        float b0[3] = { p[0], p[1], p[2] - r }, b1[3] = { p[0], p[1], p[2] + r };
        push_line(v, n, cap, a0, a1, c);
        push_line(v, n, cap, b0, b1, c);
    }
}

static int geom_load(Geom* out, const char* path, const char* zone)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        rt_log("[navdraw] cannot open %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = size > 0 ? (uint8_t*)malloc((size_t)size) : NULL;
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(b);
        rt_log("[navdraw] cannot read %s\n", path);
        return 0;
    }
    fclose(f);
    NdHeader h;
    memcpy(&h, b, size >= (long)sizeof h ? sizeof h : 0);
    uint64_t want = (uint64_t)h.header_size + h.verts * 12ull + h.tris * sizeof(NdTri) + h.edges * sizeof(NdEdge) +
        h.links * sizeof(NdLink) + h.gates * sizeof(NdGate);
    if (size < (long)sizeof h || h.magic != XNVD_MAGIC || h.version != 1 || h.header_size < sizeof h ||
        want != (uint64_t)size)
    {
        rt_log("[navdraw] %s: not a version 1 .navdraw file\n", path);
        free(b);
        return 0;
    }
    const float* verts = (const float*)(b + h.header_size);
    const NdTri* tris = (const NdTri*)(verts + h.verts * 3);
    const NdEdge* edges = (const NdEdge*)(tris + h.tris);
    const NdLink* links = (const NdLink*)(edges + h.edges);
    const NdGate* gates = (const NdGate*)(links + h.links);

    Geom g;
    memset(&g, 0, sizeof g);
    snprintf(g.pub.zone, sizeof g.pub.zone, "%s", zone);
    g.tris = (NavDrawVert*)malloc((size_t)h.tris * 3 * sizeof *g.tris + 1);
    /* visible triangles first, pruned after */
    uint32_t n = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (uint32_t i = 0; i < h.tris; ++i)
        {
            const NdTri* t = &tris[i];
            if ((t->bits & 1) != pass)
                continue;
            uint32_t c = face_colour(t, gates, h.gates);
            for (int k = 0; k < 3; ++k)
            {
                uint32_t vi = t->v[k] < h.verts ? t->v[k] : 0;
                g.tris[n++] = (NavDrawVert){ verts[vi * 3], verts[vi * 3 + 1], verts[vi * 3 + 2], c };
            }
        }
        if (!pass)
            g.tris_visible = n;
    }
    g.pub.ntris = n;

    uint32_t nl = 0, cap = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        /* internal edges are many and faint: drawn under the boundaries */
        for (int kind = 0; kind < 2; ++kind)
            for (uint32_t i = 0; i < h.edges; ++i)
            {
                const NdEdge* e = &edges[i];
                if ((e->bits & 1) != pass || (e->kind == 1) != (kind == 0))
                    continue;
                if (e->a >= h.verts || e->b >= h.verts)
                    continue;
                push_line(&g.lines, &nl, &cap, &verts[e->a * 3], &verts[e->b * 3], edge_colour(e));
            }
        if (!pass)
        {
            for (uint32_t i = 0; i < h.links; ++i)
                push_link(&g.lines, &nl, &cap, &links[i]);
            g.lines_visible = nl;
        }
    }
    g.pub.nlines = nl;
    g.pub.tris = g.tris;
    g.pub.lines = g.lines;
    rt_log("[navdraw] %s: %u triangles (%u pruned), %u edges, %u links, %u gates from %s\n", zone, h.tris,
        h.tris - g.tris_visible / 3, h.edges, h.links, h.gates, path);
    free(b);
    *out = g;
    return 1;
}

/* --- where the files are -------------------------------------------------------------------------- */

static char g_dir[1024];

static const char* draw_dir(void)
{
    if (g_dir[0])
        return g_dir;
    const char* e = getenv("XIV_NAVDRAW_DIR");
    if (e && *e)
    {
        snprintf(g_dir, sizeof g_dir, "%s", e);
        return g_dir;
    }
    /* the first folder up from the executable (build/xivboot, or inside build/<app>.app) that has
     * navmeshes/draw/index.tsv: the workspace */
    char exe[1024], real[1024];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) == 0 && realpath(exe, real))
        for (char* s = strrchr(real, '/'); s; s = strrchr(real, '/'))
        {
            *s = 0;
            char probe[1100];
            snprintf(probe, sizeof probe, "%s/navmeshes/draw/index.tsv", real);
            if (!access(probe, R_OK))
            {
                snprintf(g_dir, sizeof g_dir, "%s/navmeshes/draw", real);
                return g_dir;
            }
        }
    snprintf(g_dir, sizeof g_dir, "../navmeshes/draw");
    return g_dir;
}

/* index.tsv: zone, zone id, file. Finds the file for a zone name, or for a zone id when name is empty;
 * writes the zone's name to `zone`. */
static int index_lookup(const char* name, int id, char* file, size_t nfile, char* zone, size_t nzone)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/index.tsv", draw_dir());
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f))
    {
        char z[128], fl[256];
        int zid;
        if (line[0] == '#' || sscanf(line, "%127s %d %255s", z, &zid, fl) != 3)
            continue;
        if (name && *name ? !strcmp(z, name) : zid == id)
        {
            snprintf(file, nfile, "%s/%s", draw_dir(), fl);
            snprintf(zone, nzone, "%s", z);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* --- the current zone ------------------------------------------------------------------------------- */

/* the Meteor admin API (meteor-server Map Server/Admin/AdminServer.cs): /sessions lists each
 * player with "zoneId". Polled on a thread every 2 s while the overlay is on. */
static volatile int g_admin_zone = -1;
static int g_admin_started;

static int http_get(const char* path, char* out, size_t n)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(1990);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0)
    {
        close(s);
        return -1;
    }
    char req[256];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    if (send(s, req, (size_t)rl, 0) != rl)
    {
        close(s);
        return -1;
    }
    size_t got = 0;
    for (;;)
    {
        ssize_t r = recv(s, out + got, n - 1 - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
        if (got >= n - 1)
            break;
    }
    out[got] = 0;
    close(s);
    return (int)got;
}

static void* admin_thread(void* arg)
{
    (void)arg;
    static char buf[1 << 16];
    for (;;)
    {
        if (navdraw_on && http_get("/sessions", buf, sizeof buf) > 0)
        {
            const char* z = strstr(buf, "\"zoneId\"");
            int id = -1;
            if (z && sscanf(strchr(z, ':') + 1, " %d", &id) == 1 && id != g_admin_zone)
            {
                g_admin_zone = id;
            }
        }
        usleep(2000000);
    }
    return NULL;
}

int navdraw_get_zone(char* name, size_t n, int* zone_id)
{
    if (navdraw_zone_hook && navdraw_zone_hook(name, n, zone_id))
        return 1;
    const char* e = getenv("XIV_NAVDRAW_ZONE");
    if (e && *e)
    {
        char* end;
        long id = strtol(e, &end, 10);
        if (!*end)
            name[0] = 0, *zone_id = (int)id;
        else
            snprintf(name, n, "%s", e), *zone_id = -1;
        return 1;
    }
    /* the client's map: MapLayoutElement* at 0x0133d10c, its placeId (the zone id, kept current
     * across seamless crossings) at +0x98 */
    {
        uint32_t map, id;
        memcpy(&map, GUEST_PTR(0x0133d10c), 4);
        if (map && map < 0xFFFF0000u)
        {
            memcpy(&id, GUEST_PTR(map + 0x98), 4);
            if (id && id < 0x10000)
            {
                name[0] = 0;
                *zone_id = (int)id;
                return 1;
            }
        }
    }
    const char* adm = getenv("XIV_NAVDRAW_ADMIN");
    if (adm && !strcmp(adm, "0"))
        return 0;
    if (!g_admin_started)
    {
        g_admin_started = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, admin_thread, NULL) == 0)
            pthread_detach(t);
    }
    if (g_admin_zone < 0)
        return 0;
    name[0] = 0;
    *zone_id = g_admin_zone;
    return 1;
}

int navdraw_get_camera(float m[16])
{
    /* TODO(camera): the client's world view-projection for the 3D scene. A provider sets
     * navdraw_camera_hook (see gfx9_metal.m for the one from the shader constants). */
    return navdraw_camera_hook ? navdraw_camera_hook(m) : 0;
}

float navdraw_lift(void)
{
    static float lift = -1;
    if (lift < 0)
        lift = getenv("XIV_NAVDRAW_LIFT") ? strtof(getenv("XIV_NAVDRAW_LIFT"), NULL) : 0.08f;
    return lift;
}

/* --- per frame ---------------------------------------------------------------------------------------- */

const NavDrawGeom* navdraw_frame(void)
{
    static char cur_name[64];
    static int cur_id = -2;
    static uint32_t gen;
    static int warned;
    char name[64] = "";
    int id = -1;
    if (!navdraw_get_zone(name, sizeof name, &id))
    {
        if (!warned)
            rt_log("[navdraw] no zone yet (set XIV_NAVDRAW_ZONE, or run the Meteor admin API)\n"), warned = 1;
        return g_have_geom ? &g_geom.pub : NULL;
    }
    if (strcmp(name, cur_name) || id != cur_id)
    {
        snprintf(cur_name, sizeof cur_name, "%s", name);
        cur_id = id;
        char file[1400], zone[128];
        geom_free(&g_geom);
        g_have_geom = 0;
        if (index_lookup(name, id, file, sizeof file, zone, sizeof zone))
            g_have_geom = geom_load(&g_geom, file, zone);
        else
            rt_log("[navdraw] zone %s%s%d not in %s/index.tsv\n", name, *name ? "" : "id ", *name ? 0 : id, draw_dir());
        g_geom.pub.gen = ++gen;
    }
    if (!g_have_geom)
        return NULL;
    g_geom.pub.tris_shown = g_show_pruned ? g_geom.pub.ntris : g_geom.tris_visible;
    g_geom.pub.lines_shown = g_show_pruned ? g_geom.pub.nlines : g_geom.lines_visible;
    return &g_geom.pub;
}

int navdraw_key(int sc, int down, int shift)
{
    if (g_disabled || sc != SDL_SCANCODE_F10)
        return 0;
    if (down)
    {
        if (shift)
        {
            g_show_pruned = !g_show_pruned;
            rt_log("[navdraw] pruned polys %s\n", g_show_pruned ? "shown" : "hidden");
        }
        else
        {
            navdraw_on = !navdraw_on;
            rt_log("[navdraw] overlay %s\n", navdraw_on ? "on" : "off");
        }
    }
    return 1;
}

__attribute__((constructor)) static void navdraw_init(void)
{
    const char* e = getenv("XIV_NAVDRAW");
    g_disabled = e && !strcmp(e, "0");
    navdraw_on = e && strcmp(e, "0") != 0;
    const char* p = getenv("XIV_NAVDRAW_PRUNED");
    g_show_pruned = p && strcmp(p, "0");
}
