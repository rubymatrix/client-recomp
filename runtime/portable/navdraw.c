/* Navmesh debug overlay: the data side (navdraw.h). Finds and loads the .navdraw files (navgen
 * docs/FORMATS.md §6) of the current zone and the other zones of its region near the camera, on a
 * loader thread, and expands each into coloured triangles and line segments for the renderer
 * (gfx9_metal.m draws them, depth-tested against the scene). */
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
#include <sys/mman.h>
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
    NavDrawGeom pub; /* first: navdraw_handoff casts back */
    NavDrawVert* tris;
    NavDrawVert* lines;
    uint32_t tris_visible, lines_visible; /* without pruned */
} Geom;

/* page-aligned vertex memory, so the renderer can wrap it in a GPU buffer without a copy */
static void* vmem_alloc(size_t bytes, size_t* rounded)
{
    size_t pg = (size_t)getpagesize();
    size_t n = (bytes + pg - 1) / pg * pg;
    if (!n)
        n = pg;
    void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    *rounded = n;
    return p;
}

void navdraw_vfree(void* p, size_t bytes)
{
    if (p && bytes)
        munmap(p, bytes);
}

static void geom_free(Geom* g)
{
    navdraw_vfree(g->tris, g->pub.tris_bytes);
    navdraw_vfree(g->lines, g->pub.lines_bytes);
    memset(g, 0, sizeof *g);
}

static uint64_t geom_bytes(const Geom* g)
{
    return (uint64_t)g->pub.tris_bytes + g->pub.lines_bytes;
}

static void push_line(NavDrawVert** v, uint32_t* n, uint32_t* cap, const float a[3], const float b[3], uint32_t c)
{
    if (*n + 2 > *cap) /* sized for the worst case: not reached */
        return;
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
    /* lines: 2 vertices an edge; a link is at most 22 segments (16 arc, 2 barbs, 2 crosses of 2) */
    uint32_t cap = (uint32_t)(h.edges * 2ull + h.links * 44ull);
    g.tris = (NavDrawVert*)vmem_alloc((size_t)h.tris * 3 * sizeof *g.tris, &g.pub.tris_bytes);
    g.lines = (NavDrawVert*)vmem_alloc((size_t)cap * sizeof *g.lines, &g.pub.lines_bytes);
    if (!g.tris || !g.lines)
    {
        rt_log("[navdraw] %s: out of memory\n", path);
        geom_free(&g);
        free(b);
        return 0;
    }
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

    uint32_t nl = 0;
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
    /* give back the unused tail of the lines' worst case */
    size_t pg = (size_t)getpagesize(), used = ((size_t)nl * sizeof *g.lines + pg - 1) / pg * pg;
    if (!used)
        used = pg;
    if (used < g.pub.lines_bytes)
    {
        munmap((uint8_t*)g.lines + used, g.pub.lines_bytes - used);
        g.pub.lines_bytes = used;
    }
    g.pub.tris = g.tris;
    g.pub.lines = g.lines;
    rt_log("[navdraw] %s: %u triangles (%u pruned), %u edges, %u links, %u gates, %.1f MB from %s\n", zone, h.tris,
        h.tris - g.tris_visible / 3, h.edges, h.links, h.gates, geom_bytes(&g) / 1048576.0, path);
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

/* --- the region (zones.json) ------------------------------------------------------------------------- */

typedef struct ZoneInfo
{
    char name[64];
    int id, region, same_as;
} ZoneInfo;

static const char* zones_path(void)
{
    static char path[1100];
    const char* e = getenv("XIV_NAVDRAW_ZONES");
    if (e && *e)
        return e;
    /* draw_dir is <workspace>/navmeshes/draw */
    snprintf(path, sizeof path, "%s/../../collision/zones.json", draw_dir());
    return path;
}

/* zones.json: {"zones": {"<name>": {"zoneId": n, "regionId": n, "sameAs": "<name>", ...}, ...}}. A
 * small scanner for that shape, not a JSON parser. Returns the count, -1 when it cannot be read. */
static int zones_read(ZoneInfo* out, int max)
{
    FILE* f = fopen(zones_path(), "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = size > 0 ? (char*)malloc((size_t)size + 1) : NULL;
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(b);
        return -1;
    }
    fclose(f);
    b[size] = 0;
    int n = 0, depth = 0, zones_depth = -1;
    ZoneInfo* cur = NULL;
    for (const char* p = b; *p;)
    {
        char c = *p;
        if (c == '"')
        {
            char s[64];
            size_t k = 0;
            for (++p; *p && *p != '"'; ++p)
            {
                if (*p == '\\' && p[1])
                    ++p;
                if (k + 1 < sizeof s)
                    s[k++] = *p;
            }
            s[k] = 0;
            if (*p)
                ++p;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                ++p;
            if (*p != ':')
                continue; /* a value */
            ++p;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                ++p;
            if (depth == 1 && !strcmp(s, "zones") && *p == '{')
                zones_depth = 2;
            else if (depth == zones_depth && *p == '{' && n < max)
            {
                cur = &out[n++];
                memset(cur, 0, sizeof *cur);
                snprintf(cur->name, sizeof cur->name, "%s", s);
                cur->id = cur->region = -1;
            }
            else if (cur && depth == zones_depth + 1)
            {
                if (!strcmp(s, "zoneId"))
                    cur->id = (int)strtol(p, NULL, 10);
                else if (!strcmp(s, "regionId"))
                    cur->region = (int)strtol(p, NULL, 10);
                else if (!strcmp(s, "sameAs"))
                    cur->same_as = 1;
            }
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
        {
            --depth;
            if (depth == zones_depth)
                cur = NULL;
            else if (depth < zones_depth)
                zones_depth = -1;
        }
        ++p;
    }
    free(b);
    return n;
}

/* --- the zones loaded ---------------------------------------------------------------------------------- */

enum { S_NONE, S_LOADING, S_READY, S_FAILED };

typedef struct Slot
{
    char zone[64];
    char file[1100];
    int current;
    int have_bounds;
    float bmin[3], bmax[3];
    uint64_t estimate; /* bytes of vertices, from the header's counts */
    int state;
    uint32_t token;
    Geom geom;
} Slot;

static Slot g_slots[NAVDRAW_MAX];
static int g_nslots;
static int g_show_neighbours = 1;
static uint32_t g_ids;
static NavDrawSet g_set;

/* the loader thread: requests in, geometry out */
typedef struct Req
{
    uint32_t token;
    int prio;
    char file[1100], zone[64];
} Req;

typedef struct Res
{
    uint32_t token;
    int ok;
    Geom geom;
} Res;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Req g_reqs[NAVDRAW_MAX];
static int g_nreqs;
static Res g_res[NAVDRAW_MAX * 2];
static int g_nres;
static int g_loader; /* 0 not started, 1 running, -1 none (loads on the caller) */

static void* loader_thread(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;)
    {
        while (!g_nreqs || g_nres >= (int)(sizeof g_res / sizeof g_res[0]))
            pthread_cond_wait(&g_cv, &g_mu);
        int best = 0;
        for (int i = 1; i < g_nreqs; ++i)
            if (g_reqs[i].prio > g_reqs[best].prio)
                best = i;
        Req r = g_reqs[best];
        memmove(&g_reqs[best], &g_reqs[best + 1], (size_t)(g_nreqs - best - 1) * sizeof *g_reqs);
        --g_nreqs;
        pthread_mutex_unlock(&g_mu);
        Res res;
        memset(&res, 0, sizeof res);
        res.token = r.token;
        res.ok = geom_load(&res.geom, r.file, r.zone);
        pthread_mutex_lock(&g_mu);
        g_res[g_nres++] = res;
    }
    return NULL;
}

static void request(Slot* s)
{
    s->token = ++g_ids;
    s->state = S_LOADING;
    if (!g_loader)
    {
        const char* e = getenv("XIV_NAVDRAW_SYNC");
        g_loader = -1;
        pthread_t t;
        if (!(e && strcmp(e, "0")) && pthread_create(&t, NULL, loader_thread, NULL) == 0)
            pthread_detach(t), g_loader = 1;
    }
    if (g_loader < 0)
    {
        Res res;
        memset(&res, 0, sizeof res);
        res.token = s->token;
        res.ok = geom_load(&res.geom, s->file, s->zone);
        g_res[g_nres++] = res; /* drained before the next request */
        return;
    }
    pthread_mutex_lock(&g_mu);
    Req* r = &g_reqs[g_nreqs++];
    r->token = s->token;
    r->prio = s->current;
    snprintf(r->file, sizeof r->file, "%s", s->file);
    snprintf(r->zone, sizeof r->zone, "%s", s->zone);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* drops a slot's geometry, or its load (a load already running finishes and is thrown away) */
static void slot_drop(Slot* s)
{
    if (s->state == S_LOADING)
    {
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_nreqs; ++i)
            if (g_reqs[i].token == s->token)
            {
                memmove(&g_reqs[i], &g_reqs[i + 1], (size_t)(g_nreqs - i - 1) * sizeof *g_reqs);
                --g_nreqs;
                break;
            }
        pthread_mutex_unlock(&g_mu);
    }
    if (s->state == S_READY)
        rt_log("[navdraw] %s unloaded (%.1f MB)\n", s->zone, geom_bytes(&s->geom) / 1048576.0);
    geom_free(&s->geom);
    s->state = S_NONE;
    s->token = 0;
}

static void drain(void)
{
    if (g_loader > 0)
        pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nres; ++i)
    {
        Res* r = &g_res[i];
        Slot* s = NULL;
        for (int k = 0; k < g_nslots; ++k)
            if (g_slots[k].state == S_LOADING && g_slots[k].token == r->token)
                s = &g_slots[k];
        if (!s)
        {
            geom_free(&r->geom); /* dropped while it loaded */
            continue;
        }
        if (r->ok)
        {
            s->geom = r->geom;
            s->geom.pub.id = s->token;
            s->state = S_READY;
        }
        else
            s->state = S_FAILED;
    }
    g_nres = 0;
    if (g_loader > 0)
    {
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

static int read_bounds(Slot* s)
{
    FILE* f = fopen(s->file, "rb");
    if (!f)
        return 0;
    NdHeader h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == XNVD_MAGIC && h.version == 1;
    fclose(f);
    if (!ok)
        return 0;
    memcpy(s->bmin, h.bmin, sizeof s->bmin);
    memcpy(s->bmax, h.bmax, sizeof s->bmax);
    s->estimate = ((uint64_t)h.tris * 3 + h.edges * 2ull + h.links * 44ull) * sizeof(NavDrawVert);
    s->have_bounds = 1;
    return 1;
}

/* The zones to keep for the current zone: the current one's file, then the file of every other
 * zone of its region (once each: a sameAs zone names its target's file). */
static void region_build(const char* cur_zone, const char* cur_file, int cur_id)
{
    static char zones[NAVDRAW_MAX][64], files[NAVDRAW_MAX][1100]; /* static: big for a stack */
    int n = 0, region = -1;
    snprintf(zones[0], sizeof zones[0], "%s", cur_zone);
    snprintf(files[0], sizeof files[0], "%s", cur_file);
    n = 1;
    static ZoneInfo zi[512];
    int nz = zones_read(zi, (int)(sizeof zi / sizeof zi[0]));
    for (int i = 0; i < nz; ++i)
        if ((cur_id >= 0 && zi[i].id == cur_id) || !strcmp(zi[i].name, cur_zone))
        {
            region = zi[i].region;
            break;
        }
    if (nz < 0)
        rt_log("[navdraw] cannot read %s: the current zone only\n", zones_path());
    for (int i = 0; region >= 0 && i < nz && n < NAVDRAW_MAX; ++i)
    {
        if (zi[i].region != region || zi[i].same_as)
            continue;
        char file[1100], zone[128];
        if (!index_lookup(zi[i].name, -1, file, sizeof file, zone, sizeof zone))
            continue;
        int dup = 0;
        for (int k = 0; k < n; ++k)
            dup |= !strcmp(files[k], file);
        if (dup)
            continue;
        snprintf(zones[n], sizeof zones[n], "%s", zone);
        snprintf(files[n], sizeof files[n], "%s", file);
        ++n;
    }
    /* keep what is loaded and still wanted, drop the rest */
    static Slot old[NAVDRAW_MAX];
    int nold = g_nslots;
    memcpy(old, g_slots, sizeof old);
    memset(g_slots, 0, sizeof g_slots);
    g_nslots = 0;
    for (int i = 0; i < n; ++i)
    {
        Slot* s = &g_slots[g_nslots++];
        int kept = 0;
        for (int k = 0; k < nold && !kept; ++k)
            if (old[k].file[0] && !strcmp(old[k].file, files[i]))
            {
                *s = old[k];
                old[k].file[0] = 0;
                kept = 1;
            }
        if (!kept)
        {
            snprintf(s->file, sizeof s->file, "%s", files[i]);
            read_bounds(s);
        }
        snprintf(s->zone, sizeof s->zone, "%s", zones[i]);
        s->current = i == 0;
        snprintf(s->geom.pub.zone, sizeof s->geom.pub.zone, "%s", zones[i]);
    }
    for (int k = 0; k < nold; ++k)
        if (old[k].file[0])
            slot_drop(&old[k]);
    if (region >= 0)
    {
        char list[1024] = "";
        for (int i = 1; i < g_nslots; ++i)
            snprintf(list + strlen(list), sizeof list - strlen(list), " %s", g_slots[i].zone);
        rt_log("[navdraw] %s: region %d, other zones:%s\n", cur_zone, region, *list ? list : " none");
    }
}

/* where the camera is: the point the view-projection maps to x = y = w = 0 (rows 0, 1, 3) */
static int camera_pos(const float m[16], float p[3])
{
    const float *a = m, *b = m + 4, *c = m + 12;
    float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    if (fabsf(det) < 1e-12f)
        return 0;
    float r[3] = { -a[3], -b[3], -c[3] };
    p[0] = (r[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r[1] * c[2] - b[2] * r[2]) + a[2] * (r[1] * c[1] - b[1] * r[2])) / det;
    p[1] = (a[0] * (r[1] * c[2] - b[2] * r[2]) - r[0] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * r[2] - r[1] * c[0])) / det;
    p[2] = (a[0] * (b[1] * r[2] - r[1] * c[1]) - a[1] * (b[0] * r[2] - r[1] * c[0]) + r[0] * (b[0] * c[1] - b[1] * c[0])) / det;
    return 1;
}

static float box_dist(const Slot* s, const float p[3])
{
    float d2 = 0;
    for (int i = 0; i < 3; ++i)
    {
        float d = p[i] < s->bmin[i] ? s->bmin[i] - p[i] : p[i] > s->bmax[i] ? p[i] - s->bmax[i] : 0;
        d2 += d * d;
    }
    return sqrtf(d2);
}

static float env_f(const char* name, float def)
{
    const char* e = getenv(name);
    return e && *e ? strtof(e, NULL) : def;
}

/* --- per frame ---------------------------------------------------------------------------------------- */

const NavDrawSet* navdraw_frame(void)
{
    static char cur_name[64];
    static int cur_id = -2;
    static int warned;
    static float radius = -1, budget;
    if (radius < 0)
    {
        radius = env_f("XIV_NAVDRAW_RADIUS", 1000.0f);
        budget = env_f("XIV_NAVDRAW_BUDGET_MB", 320.0f) * 1048576.0f;
    }
    char name[64] = "";
    int id = -1;
    if (!navdraw_get_zone(name, sizeof name, &id))
    {
        if (!warned)
            rt_log("[navdraw] no zone yet (set XIV_NAVDRAW_ZONE, or run the Meteor admin API)\n"), warned = 1;
    }
    else if (strcmp(name, cur_name) || id != cur_id)
    {
        snprintf(cur_name, sizeof cur_name, "%s", name);
        cur_id = id;
        char file[1400], zone[128];
        if (index_lookup(name, id, file, sizeof file, zone, sizeof zone))
            region_build(zone, file, *name ? -1 : id);
        else
        {
            rt_log("[navdraw] zone %s%s%d not in %s/index.tsv\n", name, *name ? "" : "id ", *name ? 0 : id, draw_dir());
            for (int i = 0; i < g_nslots; ++i)
                slot_drop(&g_slots[i]);
            g_nslots = 0;
        }
    }
    drain();

    float m[16], cam[3];
    int have_cam = navdraw_get_camera(m) && camera_pos(m, cam);
    /* what to load: the current zone, then the others nearest first while within the radius and
     * the budget; what to drop: the others once well outside it (hysteresis), or hidden */
    uint64_t used = 0;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].state == S_READY)
            used += geom_bytes(&g_slots[i].geom);
        else if (g_slots[i].state == S_LOADING)
            used += g_slots[i].estimate;
    for (int i = 0; i < g_nslots; ++i)
    {
        Slot* s = &g_slots[i];
        if (s->current || s->state == S_NONE || s->state == S_FAILED)
            continue;
        if (!g_show_neighbours || (have_cam && s->have_bounds && box_dist(s, cam) > radius * 1.25f + 100.0f))
        {
            used -= s->state == S_READY ? geom_bytes(&s->geom) : s->estimate;
            slot_drop(s);
        }
    }
    if (g_nslots && g_slots[0].state == S_NONE)
    {
        request(&g_slots[0]);
        used += g_slots[0].estimate;
    }
    for (;;)
    {
        Slot* best = NULL;
        float best_d = 0;
        for (int i = 1; g_show_neighbours && have_cam && i < g_nslots; ++i)
        {
            Slot* s = &g_slots[i];
            if (s->state != S_NONE || !s->have_bounds)
                continue;
            float d = box_dist(s, cam);
            if (d <= radius && used + s->estimate <= (uint64_t)budget && (!best || d < best_d))
                best = s, best_d = d;
        }
        if (!best)
            break;
        request(best);
        used += best->estimate;
    }
    drain(); /* loads done on this thread (XIV_NAVDRAW_SYNC) */

    g_set.n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < g_nslots; ++i)
        {
            Slot* s = &g_slots[i];
            if (s->state != S_READY || s->current != pass)
                continue;
            Geom* g = &s->geom;
            g->pub.dim = !s->current;
            g->pub.tris_shown = g_show_pruned ? g->pub.ntris : g->tris_visible;
            g->pub.lines_shown = g_show_pruned ? g->pub.nlines : g->lines_visible;
            g_set.g[g_set.n++] = &g->pub;
        }
    return g_set.n ? &g_set : NULL;
}

void navdraw_handoff(const NavDrawGeom* pub, int tris, int lines)
{
    Geom* g = (Geom*)pub;
    if (tris)
        g->tris = NULL, g->pub.tris = NULL;
    if (lines)
        g->lines = NULL, g->pub.lines = NULL;
    /* the sizes stay (geom_free skips the NULL pointers): the budget counts the GPU's copy until
     * the zone is dropped */
}

void navdraw_stats(uint64_t* bytes, int* loaded, int* loading)
{
    *bytes = 0, *loaded = 0, *loading = 0;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].state == S_READY)
            *bytes += geom_bytes(&g_slots[i].geom), ++*loaded;
        else if (g_slots[i].state == S_LOADING)
            ++*loading;
}

int navdraw_key(int sc, int down, int shift)
{
    static int f11_taken;
    if (g_disabled)
        return 0;
    if (sc == SDL_SCANCODE_F11)
    {
        /* Shift+F11 only: plain F11 stays the game's */
        if (down && shift)
        {
            g_show_neighbours = !g_show_neighbours;
            rt_log("[navdraw] other zones of the region %s\n", g_show_neighbours ? "shown" : "hidden");
            f11_taken = 1;
            return 1;
        }
        if (!down && f11_taken)
        {
            f11_taken = 0;
            return 1;
        }
        return 0;
    }
    if (sc != SDL_SCANCODE_F10)
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
    const char* nb = getenv("XIV_NAVDRAW_NEIGHBOURS");
    g_show_neighbours = !(nb && !strcmp(nb, "0"));
}
