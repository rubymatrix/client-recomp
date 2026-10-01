/* Direct3D 8 for 64-bit hosts (R3.1): the interface FFXiMain draws through, implemented by us.
 *
 * The translated game still speaks D3D8 - Direct3DCreate8, then COM calls on IDirect3DDevice8 and
 * its resources - so this file is the D3D8 front end the Metal renderer (R3.2) sits under.
 * It owns what the game can observe:
 *
 *   - the COM objects (guest memory: a vtable pointer and our object index; every method is a
 *     thunk named "d3d8.dll!IDirect3DDevice8::SetRenderState" and so on, so a method the game
 *     reaches that is not written yet traps with its name),
 *   - resource memory the game locks (textures, surfaces, vertex and index buffers live in the
 *     guest window, allocated on first lock),
 *   - device state with D3D8 semantics - defaults, readback (the game calls GetTransform and
 *     GetViewport millions of times), state-block recording, capture and apply, the references
 *     bound resources hold, the stream/index reset after the ...UP draws.
 *
 * What is drawn goes to the graphics back end (gfx.h: Metal on macOS, R3.2): every texture and
 * render target has a GfxTex beside its guest memory, and Clear, the Draw* calls and Present turn
 * the device state into the back end's clears and draw packets (see "drawing" below).
 *
 * Which copy of a resource is current: for textures the game fills (the managed and system pools)
 * the guest memory is, and a level is uploaded when it is next drawn with after a lock; for render
 * targets and depth the GPU is, and a lock reads the level back (and an unlock writes it again).
 * Vertex and index buffers stay in guest memory; each draw copies the range it uses.
 *
 * The method set is the one the game was measured to use (specs/FFXiMain.2026-08-22.d3d8-surface.txt); vtable order is the D3D8 ABI, as the R1 proxy
 * (d3d8proxy.cpp) lists it. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "d3d8.h"
#include "gfx.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu

/* D3DFORMAT */
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_X1R5G5B5 24u
#define FMT_A1R5G5B5 25u
#define FMT_A4R4G4B4 26u
#define FMT_A8 28u
#define FMT_L8 50u
#define FMT_A8L8 51u
#define FMT_V8U8 60u
#define FMT_D16 80u
#define FMT_D24S8 75u
#define FMT_D24X8 77u
#define FMT_INDEX16 101u
#define FMT_INDEX32 102u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

/* D3DRESOURCETYPE */
#define RT_SURFACE 1u
#define RT_TEXTURE 3u
#define RT_VOLUMETEXTURE 4u
#define RT_CUBETEXTURE 5u
#define RT_VERTEXBUFFER 6u
#define RT_INDEXBUFFER 7u

#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u

/* D3DCAPS8 (212 bytes) as Windows 11's d3d8 reports it for a HAL device (NVIDIA RTX A4500,
 * captured 2026-09-24): vertex shader 1.1, pixel shader 1.4, 8 texture stages, 16384 textures,
 * 8 lights, 16 streams. Everything here is within what Metal on an M1 provides. */
static const uint32_t CAPS8[53] = {
    0x00000001, 0x00000000, 0x00020000, 0x200a0000, 0x00000420, 0x8000000f, 0x00000001, 0x001bbef0, 0x00000ef2,
    0x00f37191, 0x000000ff, 0x00001fff, 0x00001fff, 0x000000ff, 0x00084208, 0x0001ecc5, 0x03030700, 0x03030300,
    0x03030300, 0x0000003f, 0x0000003f, 0x0000001f, 0x00004000, 0x00004000, 0x00000800, 0x00002000, 0x00004000,
    0x00000010, 0x501502f9, 0xccbebc20, 0xccbebc20, 0x4cbebc20, 0x4cbebc20, 0x00000000, 0x000000ff, 0x00180008,
    0x03feffff, 0x00000008, 0x00000008, 0x0000003b, 0x00000008, 0x00000008, 0x00000004, 0x00000000, 0x46000000,
    0x00ffffff, 0x00ffffff, 0x00000010, 0x000000ff, 0xfffe0101, 0x00000100, 0xffff0104, 0x477fe000,
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* FFXI_DRAWLOG (see cap_present) */
static const char* g_cap_path;
static void scene_forget(GfxTex* t);
static FILE* g_cap;
static uint32_t g_cap_esp, g_cap_frame, g_cap_n;
static uint32_t g_cap_at[32], g_cap_nat; /* <path>.at: draws after which the target is dumped */
static void cap_present(void);
static void scene_present(void);

/* --- formats ----------------------------------------------------------------------------------------- */
static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block, or 0 */
{
    return f == FMT_DXT1 ? 8u : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5) ? 16u : 0u;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel for the uncompressed formats */
{
    switch (f)
    {
    case FMT_A8:
    case FMT_L8:
    case 41: /* P8 */
    case 52: /* A4L4 */
        return 1;
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_D16:
    case 73: /* D15S1 */
    case FMT_INDEX16:
        return 2;
    default: return 4;
    }
}

static uint32_t fmt_pitch(uint32_t f, uint32_t w)
{
    uint32_t b = fmt_block(f);
    return b ? ((w + 3) / 4 ? (w + 3) / 4 : 1) * b : w * fmt_bytes(f);
}

static uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h)
{
    return fmt_pitch(f, w) * (fmt_block(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h);
}

/* --- objects ----------------------------------------------------------------------------------------- */
enum
{
    O_FREE,
    O_D3D,
    O_DEVICE,
    O_TEXTURE,
    O_CUBE,
    O_VB,
    O_IB,
    O_SURFACE,
};

typedef struct Obj
{
    uint8_t kind, implicit; /* implicit: the swap chain's back buffer, the auto depth buffer */
    int32_t refs;
    uint32_t guest;     /* the COM pointer the game holds */
    uint32_t container; /* surfaces of a texture: its COM pointer; their references are the texture's */
    uint32_t format, usage, pool, width, height, levels, size, fvf;
    uint32_t mem; /* guest memory, on first lock */
    uint32_t* subs; /* textures: level surfaces (cubes: face * levels + level) */
    uint32_t nsubs;
    uint32_t lod, priority;
    GfxTex* gpu;         /* textures, render targets, depth surfaces */
    GfxBuf* gbuf;        /* static vertex and index buffers: the GPU's copy */
    uint8_t gbuf_dirty;  /* ... which a lock that writes makes stale */
    uint8_t face, level; /* surfaces of a texture: where in it */
    uint8_t dirty;       /* surfaces of a texture: guest memory is newer than the GPU's copy */
    uint8_t gpu_locked;  /* a GPU-owned surface is locked: written back at unlock */
    GfxTex* repl;        /* textures: a texture pack's replacement, drawn with instead of gpu */
    uint32_t repl_pad;   /* ... whose glyph quads are drawn this many texels wider each side */
    uint32_t repl_pad_min; /* ... when they are at least this many texels tall */
    const struct PackEntry* repl_entry; /* ... and its glyph table, if it has one */
    uint32_t pw, ph;     /* render-target textures drawn at the screen's resolution (native_size): gpu's
                          * size, where the game sees width x height; 0 when they are the same */
} Obj;

static Obj* g_objs;
static uint32_t g_nobjs, g_capobjs, g_free_hint = 1;
static uint32_t g_vtbl[O_SURFACE + 1];

static uint32_t obj_new(int kind)
{
    uint32_t i = g_free_hint;
    while (i < g_nobjs && g_objs[i].kind != O_FREE)
        i++;
    if (i >= g_nobjs)
    {
        if (g_nobjs + 2 > g_capobjs)
        {
            g_capobjs = g_capobjs ? g_capobjs * 2 : 1024;
            g_objs = (Obj*)realloc(g_objs, g_capobjs * sizeof *g_objs);
        }
        if (!g_nobjs)
            g_objs[g_nobjs++].kind = O_FREE; /* index 0 is never an object */
        i = g_nobjs++;
    }
    g_free_hint = i + 1;
    Obj* o = &g_objs[i];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->guest = gheap_alloc(8, 1);
    wr32(o->guest, g_vtbl[kind]);
    wr32(o->guest + 4, i);
    return o->guest;
}

static Obj* obj(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i && i < g_nobjs && g_objs[i].kind != O_FREE && g_objs[i].guest == p ? &g_objs[i] : NULL;
}

static uint32_t obj_release(uint32_t p);

static void obj_destroy(Obj* o)
{
    for (uint32_t i = 0; i < o->nsubs; ++i)
    {
        Obj* s = obj(o->subs[i]);
        if (s)
        {
            s->container = 0;
            s->refs = 1;
            obj_release(s->guest);
        }
    }
    free(o->subs);
    scene_forget(o->gpu);
    gfx_tex_destroy(o->gpu);
    gfx_buf_destroy(o->gbuf);
    if (o->mem)
        gheap_free(o->mem);
    uint32_t idx = rd32(o->guest + 4);
    wr32(o->guest, 0);
    gheap_free(o->guest);
    o->kind = O_FREE;
    if (idx < g_free_hint)
        g_free_hint = idx;
}

static uint32_t obj_addref(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_addref(o->container);
    return (uint32_t)++o->refs;
}

static uint32_t obj_release(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_release(o->container);
    if (o->refs > 0)
        o->refs--;
    if (!o->refs && !o->implicit && o->kind != O_DEVICE && o->kind != O_D3D)
    {
        obj_destroy(o);
        return 0;
    }
    return (uint32_t)o->refs;
}

/* a pointer the device or a state block holds: references the new one, releases the old */
static void bind(uint32_t* slot, uint32_t p)
{
    if (*slot == p)
        return;
    if (p)
        obj_addref(p);
    uint32_t old = *slot;
    *slot = p;
    if (old)
        obj_release(old);
}

static uint32_t obj_mem(Obj* o)
{
    if (!o->mem)
        o->mem = gheap_alloc(o->size ? o->size : 16, 1);
    return o->mem;
}

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, uint32_t container)
{
    uint32_t p = obj_new(O_SURFACE);
    Obj* s = obj(p);
    s->format = fmt;
    s->width = w;
    s->height = h;
    s->usage = usage;
    s->pool = pool;
    s->size = fmt_size(fmt, w, h);
    s->container = container;
    if (!container && (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)))
        s->gpu = gfx_tex_create(GFX_TEX_2D, fmt, w, h, 1, (usage & USAGE_DEPTHSTENCIL) ? GFX_USE_DEPTH : GFX_USE_RT);
    return p;
}

/* the GPU texture behind a surface, and which face and level of it */
static GfxTex* surface_gpu(const Obj* s, uint32_t* face, uint32_t* level)
{
    if (s->container)
    {
        Obj* t = obj(s->container);
        *face = s->face, *level = s->level;
        return t ? t->gpu : NULL;
    }
    *face = *level = 0;
    return s->gpu;
}

/* render targets and depth: the GPU holds the current pixels, not guest memory */
static int gpu_owned(const Obj* s)
{
    uint32_t f, l;
    return (s->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && surface_gpu(s, &f, &l);
}

static void mark_dirty(Obj* s)
{
    s->dirty = 1;
    Obj* t = s->container ? obj(s->container) : NULL;
    if (t)
        t->dirty = 1;
}

/* --- texture packs -------------------------------------------------------------------------------------
 * d3d8_texture_pack(folder): high-resolution replacements for the game's own textures (the font, say),
 * made by tools/make_texpack.py. Each is <folder>/<hash>_<w>x<h>.dds: the FNV-1a hash of the first
 * level of a w x h texture as the game uploads it, and the replacement, DXT1/3/5 with its mipmaps.
 * The replacement keeps the original's layout at a larger size, so the game's texture coordinates
 * (fractions of the texture) still land on the same art; it is drawn with instead of the original,
 * filtered with its mipmaps, and dropped again if the game writes something else into the texture.
 * Only textures of a size some entry has are hashed. FFXI_TEXLOG=1 hashes and logs every texture
 * instead, to find the one to replace; with FFXI_TEXDUMP=<folder> as well, each level hashed is
 * written there as <hash>.bin, in D3D's layout. */
typedef struct PackEntry
{
    uint64_t hash;
    uint32_t w, h, pad, pad_min; /* _pad<N>[min<M>] in the name: texels each glyph quad drawn with it is
                                  * widened by (only quads at least M texels tall) */
    float* glyphs;               /* <entry>.glyphs beside it: each glyph's ink box (x0 x1 y0 y1, texels) */
    uint32_t nglyphs;
    char* path;
    GfxTex* tex;
    uint8_t tried; /* loaded, or failed to */
} PackEntry;

static PackEntry* g_pack;
static uint32_t g_npack;
static int g_texlog = -1;

void d3d8_texture_pack(const char* dir)
{
    PlatDir* d = plat_dir_open(dir);
    if (!d)
    {
        rt_log("[recomp] textures: cannot read %s\n", dir);
        return;
    }
    uint32_t added = 0;
    for (const char* name; (name = plat_dir_next(d));)
    {
        unsigned long long hash;
        uint32_t w, h;
        char ext[8];
        uint32_t pad = 0, pad_min = 0;
        if (sscanf(name, "%16llx_%ux%u_pad%umin%u.%7s", &hash, &w, &h, &pad, &pad_min, ext) != 6 &&
            (pad_min = 0, sscanf(name, "%16llx_%ux%u_pad%u.%7s", &hash, &w, &h, &pad, ext) != 5) &&
            (pad = 0, sscanf(name, "%16llx_%ux%u.%7s", &hash, &w, &h, ext) != 4))
            continue;
        if (strcmp(ext, "dds") || strlen(name) < 16)
            continue;
        g_pack = (PackEntry*)realloc(g_pack, (g_npack + 1) * sizeof *g_pack);
        size_t n = strlen(dir) + strlen(name) + 2;
        char* path = (char*)malloc(n);
        snprintf(path, n, "%s%c%s", dir, plat_path_sep, name);
        PackEntry e = { hash, w, h, pad, pad_min, NULL, 0, path, NULL, 0 };
        char gp[1100];
        snprintf(gp, sizeof gp, "%.*s.glyphs", (int)(strlen(path) - 4), path);
        FILE* gf = fopen(gp, "r");
        float b[4];
        while (gf && fscanf(gf, "%f %f %f %f", &b[0], &b[1], &b[2], &b[3]) == 4)
        {
            e.glyphs = (float*)realloc(e.glyphs, (e.nglyphs + 1) * 4 * sizeof(float));
            memcpy(e.glyphs + 4 * e.nglyphs++, b, sizeof b);
        }
        if (gf)
            fclose(gf);
        g_pack[g_npack++] = e;
        added++;
    }
    plat_dir_close(d);
    rt_log("[recomp] textures: %u replacement%s from %s\n", added, added == 1 ? "" : "s", dir);
}

/* A DDS file's texture: DXT1/3/5 only, every level in the file. NULL if it is not one. */
static GfxTex* load_dds(const char* path)
{
    size_t size = 0;
    unsigned char* f = plat_read_file(path, &size);
    GfxTex* t = NULL;
    uint32_t fmt = 0, w = 0, h = 0, levels = 0;
    if (f && size >= 128 && !memcmp(f, "DDS ", 4) && (f[80] & 4))
    {
        h = (uint32_t)f[12] | f[13] << 8 | f[14] << 16 | (uint32_t)f[15] << 24;
        w = (uint32_t)f[16] | f[17] << 8 | f[18] << 16 | (uint32_t)f[19] << 24;
        levels = (uint32_t)f[28] | f[29] << 8;
        fmt = FOURCC(f[84], f[85], f[86], f[87]);
    }
    uint32_t blk = fmt_block(fmt);
    if (blk && w && h && w <= 16384 && h <= 16384)
    {
        levels = levels ? levels : 1;
        size_t off = 128, need = 0;
        for (uint32_t l = 0; l < levels; ++l)
            need += (size_t)(((w >> l ? w >> l : 1) + 3) / 4) * (((h >> l ? h >> l : 1) + 3) / 4) * blk;
        if (off + need <= size && (t = gfx_tex_create(GFX_TEX_2D, fmt, w, h, levels, GFX_USE_SAMPLE)))
            for (uint32_t l = 0; l < levels; ++l)
            {
                uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
                gfx_tex_upload(t, 0, l, f + off, (lw + 3) / 4 * blk);
                off += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * blk;
            }
    }
    if (!t)
        rt_log("[recomp] textures: %s is not a DXT1/3/5 DDS with its levels; not used\n", path);
    free(f);
    return t;
}

/* The replacement for a texture whose first level was just uploaded, or NULL; *pad its entry's. */
static GfxTex* texture_replacement(const Obj* t, const Obj* level0, uint32_t* pad, uint32_t* pad_min, const PackEntry** entry)
{
    *pad = *pad_min = 0, *entry = NULL;
    if (g_texlog < 0)
        g_texlog = getenv("FFXI_TEXLOG") && getenv("FFXI_TEXLOG")[0] == '1';
    if (t->kind != O_TEXTURE || (!g_npack && !g_texlog))
        return NULL;
    int sized = g_texlog;
    for (uint32_t i = 0; i < g_npack && !sized; ++i)
        sized = g_pack[i].w == t->width && g_pack[i].h == t->height;
    if (!sized)
        return NULL;
    uint32_t pitch = fmt_pitch(level0->format, level0->width), blk = fmt_block(level0->format);
    size_t n = (size_t)pitch * (blk ? (level0->height + 3) / 4 : level0->height);
    const uint8_t* p = GUEST_PTR(level0->mem);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < g_npack; ++i)
    {
        PackEntry* e = &g_pack[i];
        if (e->hash != hash || e->w != t->width || e->h != t->height)
            continue;
        if (!e->tried)
        {
            e->tried = 1;
            e->tex = load_dds(e->path);
            if (e->tex)
                rt_log("[recomp] textures: %ux%u %016llx replaced by %s\n", t->width, t->height, (unsigned long long)hash, e->path);
        }
        *pad = e->pad, *pad_min = e->pad_min, *entry = e;
        return e->tex;
    }
    if (g_texlog)
        rt_log("[recomp] textures: %ux%u format %08x hash %016llx texture %08x (no replacement)\n", t->width, t->height,
            t->format, (unsigned long long)hash, t->guest);
    if (g_texlog && getenv("FFXI_TEXDUMP")) /* ... and the level itself, to make a replacement from */
    {
        char dp[512];
        snprintf(dp, sizeof dp, "%s/%016llx.bin", getenv("FFXI_TEXDUMP"), (unsigned long long)hash);
        FILE* df = fopen(dp, "wb");
        if (df)
            fwrite(p, 1, n, df), fclose(df);
    }
    return NULL;
}

/* A texture about to be drawn with: levels written since their last upload go up first. */
static GfxTex* texture_for_draw(uint32_t p, int* kind)
{
    Obj* t = obj(p);
    if (!t || !t->gpu || (t->kind != O_TEXTURE && t->kind != O_CUBE))
        return NULL;
    if (t->dirty)
    {
        for (uint32_t i = 0; i < t->nsubs; ++i)
        {
            Obj* s = obj(t->subs[i]);
            if (s && s->dirty && s->mem && !gpu_owned(s))
            {
                gfx_tex_upload(t->gpu, s->face, s->level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
                if (i == 0)
                    t->repl = texture_replacement(t, s, &t->repl_pad, &t->repl_pad_min, &t->repl_entry);
            }
            if (s)
                s->dirty = 0;
        }
        t->dirty = 0;
    }
    *kind = t->kind == O_CUBE ? 2 : 1;
    return t->repl ? t->repl : t->gpu;
}

/* --- device state ------------------------------------------------------------------------------------- */
#define NXF 280 /* transforms: D3DTS 0..23, then WORLDMATRIX(0..255) = 256..511 */
#define MAX_LIGHTS 64
#define NSTREAMS 16
#define NVSC 256
#define NPSC 8

typedef struct Light
{
    uint32_t v[26]; /* D3DLIGHT8 */
    uint32_t enabled;
} Light;

typedef struct State
{
    uint32_t rs[256];
    uint32_t tss[8][32];
    uint32_t tex[8];
    float xf[NXF][16];
    uint32_t vp[6];
    uint32_t mat[17];
    Light light[MAX_LIGHTS];
    uint32_t clip[6][4];
    uint32_t stream[NSTREAMS], stride[NSTREAMS];
    uint32_t ib, base_vertex;
    uint32_t vs, ps;
    float vsc[NVSC][4], psc[NPSC][4];
} State;

/* what a state block holds */
typedef struct Mask
{
    uint8_t rs[256], tss[8][32], tex[8], xf[NXF], vp, mat, light[MAX_LIGHTS], lighten[MAX_LIGHTS], clip[6],
        stream[NSTREAMS], ib, vs, ps, vsc[NVSC], psc[NPSC];
} Mask;

typedef struct Block
{
    State s;
    Mask m;
} Block;

/* where each input register comes from: an FVF's, or a vertex shader declaration's */
typedef struct Layout
{
    GfxElem el[GFX_NREGS];
    int32_t offset[GFX_NREGS];
    uint8_t rhw;
} Layout;

typedef struct Shader
{
    uint32_t* decl;
    uint32_t ndecl;
    uint32_t* func;
    uint32_t nfunc;
    Layout lay;    /* vertex shaders */
    uint32_t hash; /* of func: the back end's key */
} Shader;

typedef struct Dev
{
    uint32_t guest, d3d, hwnd, behavior, device_type;
    uint32_t pp[13]; /* D3DPRESENT_PARAMETERS */
    uint32_t backbuffer, depth, rt, ds;
    State cur;
    Block* rec;
    Block** blocks;
    uint32_t nblocks;
    Shader* vs;
    uint32_t nvs;
    Shader* ps;
    uint32_t nps;
    int cursor;
} Dev;

static Dev g_dev; /* the game makes one device */
/* One past the highest transform, light and vertex shader constant ever set, in any state: past
 * them every state holds the defaults, so a state block's apply or capture stops there (a
 * D3DSBT_ALL block is otherwise 280 matrices, 64 lights and 256 constants a copy). Never lowered. */
static int g_xf_top, g_light_top, g_vsc_top;

static void raise_top(int* top, uint32_t n)
{
    if ((int)n > *top)
        *top = (int)n;
}
/* A 16x16 occlusion probe tests a quad at the sky's depth (z 0x3f7ffffe, 1 - 2^-23: the game's
 * probe for the sun and its lens flare) since the last probe was read; see lock_rect. */
static int g_probe_sky;
static uint32_t g_d3d;

static int xf_index(uint32_t ts)
{
    return ts < 24 ? (int)ts : ts >= 256 && ts < 512 ? (int)(24 + ts - 256) : -1;
}

static void default_light(Light* l)
{
    memset(l, 0, sizeof *l);
    l->v[0] = 3; /* D3DLIGHT_DIRECTIONAL */
    l->v[1] = l->v[2] = l->v[3] = f2u(1.0f);
    l->v[18] = f2u(1.0f); /* direction (0, 0, 1) */
}

static void state_defaults(State* s, uint32_t w, uint32_t h, int zbuffer)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < NXF; ++i)
        s->xf[i][0] = s->xf[i][5] = s->xf[i][10] = s->xf[i][15] = 1.0f;
    uint32_t* rs = s->rs;
    rs[7] = zbuffer ? 1 : 0;           /* ZENABLE */
    rs[8] = 3;                         /* FILLMODE solid */
    rs[9] = 2;                         /* SHADEMODE gouraud */
    rs[14] = 1;                        /* ZWRITEENABLE */
    rs[16] = 1;                        /* LASTPIXEL */
    rs[19] = 2;                        /* SRCBLEND one */
    rs[20] = 1;                        /* DESTBLEND zero */
    rs[22] = 3;                        /* CULLMODE ccw */
    rs[23] = 4;                        /* ZFUNC lessequal */
    rs[25] = 8;                        /* ALPHAFUNC always */
    rs[37] = f2u(1.0f);                /* FOGEND */
    rs[38] = f2u(1.0f);                /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1;      /* STENCILFAIL/ZFAIL/PASS keep */
    rs[56] = 8;                        /* STENCILFUNC always */
    rs[58] = rs[59] = 0xFFFFFFFFu;     /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;              /* TEXTUREFACTOR */
    rs[136] = 1;                       /* CLIPPING */
    rs[137] = 1;                       /* LIGHTING */
    rs[141] = 1;                       /* COLORVERTEX */
    rs[142] = 1;                       /* LOCALVIEWER */
    rs[145] = 1;                       /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;                       /* SPECULARMATERIALSOURCE color2 */
    rs[154] = rs[155] = f2u(1.0f);     /* POINTSIZE, POINTSIZE_MIN */
    rs[158] = f2u(1.0f);               /* POINTSCALE_A */
    rs[162] = 0xFFFFFFFFu;             /* MULTISAMPLEMASK */
    rs[164] = f2u(1.0f);               /* PATCHSEGMENTS */
    rs[166] = f2u(64.0f);              /* POINTSIZE_MAX */
    rs[168] = 0xF;                     /* COLORWRITEENABLE */
    rs[171] = 1;                       /* BLENDOP add */
    rs[172] = 3;                       /* POSITIONORDER cubic */
    rs[173] = 1;                       /* NORMALORDER linear */
    for (uint32_t t = 0; t < 8; ++t)
    {
        uint32_t* v = s->tss[t];
        v[1] = t ? 1 : 4; /* COLOROP: disable / modulate */
        v[2] = 2;         /* COLORARG1 texture */
        v[3] = 1;         /* COLORARG2 current */
        v[4] = t ? 1 : 2; /* ALPHAOP: disable / selectarg1 */
        v[5] = 2;
        v[6] = 1;
        v[11] = t;        /* TEXCOORDINDEX */
        v[13] = v[14] = v[25] = 1; /* ADDRESSU/V/W wrap */
        v[16] = v[17] = 1;         /* MAGFILTER, MINFILTER point */
        v[21] = 1;                 /* MAXANISOTROPY */
        v[26] = v[27] = v[28] = 1; /* COLORARG0, ALPHAARG0, RESULTARG current */
    }
    s->vp[2] = w;
    s->vp[3] = h;
    s->vp[5] = f2u(1.0f);
}

/* The state a setter writes: the block being recorded, or the device. */
static State* target(Mask** m)
{
    if (g_dev.rec)
    {
        *m = &g_dev.rec->m;
        return &g_dev.rec->s;
    }
    *m = NULL;
    return &g_dev.cur;
}

/* to[i] = from[i] where m[i] (0 or 1), eight at a time: a group with none set is skipped, one with
 * all set is one copy. n is a multiple of 8. */
static void copy_masked(uint32_t* to, const uint32_t* from, const uint8_t* m, int n)
{
    for (int i = 0; i < n; i += 8)
    {
        uint64_t group;
        memcpy(&group, m + i, 8);
        if (group == 0x0101010101010101ull)
            memcpy(to + i, from + i, 32);
        else if (group)
            for (int j = i; j < i + 8; ++j)
                if (m[j])
                    to[j] = from[j];
    }
}

/* Copies the masked entries from `from` to `to` (Apply: block -> device; Capture: device -> block). */
static void state_copy(State* to, const State* from, const Mask* m)
{
    copy_masked(to->rs, from->rs, m->rs, 256);
    for (int t = 0; t < 8; ++t)
    {
        copy_masked(to->tss[t], from->tss[t], m->tss[t], 32);
        if (m->tex[t])
            bind(&to->tex[t], from->tex[t]);
    }
    for (int i = 0; i < g_xf_top; ++i)
        if (m->xf[i])
            memcpy(to->xf[i], from->xf[i], 64);
    if (m->vp)
        memcpy(to->vp, from->vp, sizeof to->vp);
    if (m->mat)
        memcpy(to->mat, from->mat, sizeof to->mat);
    for (int i = 0; i < g_light_top; ++i)
    {
        if (m->light[i])
            memcpy(to->light[i].v, from->light[i].v, sizeof to->light[i].v);
        if (m->lighten[i])
            to->light[i].enabled = from->light[i].enabled;
    }
    for (int i = 0; i < 6; ++i)
        if (m->clip[i])
            memcpy(to->clip[i], from->clip[i], 16);
    for (int i = 0; i < NSTREAMS; ++i)
        if (m->stream[i])
        {
            bind(&to->stream[i], from->stream[i]);
            to->stride[i] = from->stride[i];
        }
    if (m->ib)
    {
        bind(&to->ib, from->ib);
        to->base_vertex = from->base_vertex;
    }
    if (m->vs)
        to->vs = from->vs;
    if (m->ps)
        to->ps = from->ps;
    for (int i = 0; i < g_vsc_top; ++i)
        if (m->vsc[i])
            memcpy(to->vsc[i], from->vsc[i], 16);
    for (int i = 0; i < NPSC; ++i)
        if (m->psc[i])
            memcpy(to->psc[i], from->psc[i], 16);
}

static void block_free(Block* b)
{
    for (int t = 0; t < 8; ++t)
        bind(&b->s.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&b->s.stream[i], 0);
    bind(&b->s.ib, 0);
    free(b);
}

static uint32_t block_token(Block* b)
{
    for (uint32_t i = 0; i < g_dev.nblocks; ++i)
        if (!g_dev.blocks[i])
        {
            g_dev.blocks[i] = b;
            return i + 1;
        }
    g_dev.blocks = (Block**)realloc(g_dev.blocks, (g_dev.nblocks + 1) * sizeof *g_dev.blocks);
    g_dev.blocks[g_dev.nblocks++] = b;
    return g_dev.nblocks;
}

static Block* block_of(uint32_t token)
{
    return token && token <= g_dev.nblocks ? g_dev.blocks[token - 1] : NULL;
}

/* --- IUnknown, shared by every interface ----------------------------------------------------------------- */
static void Unknown_QueryInterface(Guest* g)
{
    wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}
static void Unknown_AddRef(Guest* g) { RET(obj_addref(ARG(0)), 1); }
static void Unknown_Release(Guest* g) { RET(obj_release(ARG(0)), 1); }

/* --- IDirect3D8 -------------------------------------------------------------------------------------------- */
static void desktop(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    user32_desktop_mode(w, h, hz);
}

static const uint32_t MODES[][2] = { { 640, 480 },  { 800, 600 },   { 1024, 768 },  { 1152, 864 },  { 1280, 720 },
                                     { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },  { 1440, 900 },  { 1600, 900 },
                                     { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 } };

/* the display modes offered: the list up to the desktop's size, then the desktop, in both formats */
static uint32_t modes(uint32_t (*out)[4], uint32_t max)
{
    uint32_t dw, dh, hz, n = 0;
    desktop(&dw, &dh, &hz);
    const uint32_t fmts[2] = { FMT_X8R8G8B8, FMT_R5G6B5 };
    for (int f = 0; f < 2; ++f)
    {
        int have_desktop = 0;
        for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; ++i)
            if (MODES[i][0] <= dw && MODES[i][1] <= dh && n < max)
            {
                have_desktop |= MODES[i][0] == dw && MODES[i][1] == dh;
                out[n][0] = MODES[i][0], out[n][1] = MODES[i][1], out[n][2] = hz, out[n][3] = fmts[f], n++;
            }
        if (!have_desktop && n < max)
            out[n][0] = dw, out[n][1] = dh, out[n][2] = hz, out[n][3] = fmts[f], n++;
    }
    return n;
}

static void IDirect3D8_GetAdapterCount(Guest* g) { RET(1, 1); }

static void IDirect3D8_GetAdapterIdentifier(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = ARG(3);
    memset(GUEST_PTR(p), 0, 1068);
    strcpy((char*)GUEST_PTR(p), "ffxi-metal");
    strcpy((char*)GUEST_PTR(p + 512), "FFXI Metal");
    wr32(p + 1024, 0x00010000u); /* DriverVersion 1.0.0.0 */
    wr32(p + 1028, 0x00060000u);
    wr32(p + 1032, 0x106B); /* VendorId: Apple */
    wr32(p + 1064, 1);      /* WHQLLevel */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterModeCount(Guest* g)
{
    uint32_t m[64][4];
    RET(ARG(1) ? 0 : modes(m, 64), 2);
}

static void IDirect3D8_EnumAdapterModes(Guest* g)
{
    uint32_t m[64][4], n = modes(m, 64), i = ARG(2);
    if (ARG(1) || i >= n)
        RET(D3DERR_INVALIDCALL, 4);
    for (int k = 0; k < 4; ++k)
        wr32(ARG(3) + 4u * (uint32_t)k, m[i][k]);
    RET(D3D_OK, 4);
}

static void write_desktop_mode(uint32_t p)
{
    uint32_t w, h, hz;
    desktop(&w, &h, &hz);
    wr32(p, w);
    wr32(p + 4, h);
    wr32(p + 8, hz);
    wr32(p + 12, FMT_X8R8G8B8);
}

static void IDirect3D8_GetAdapterDisplayMode(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 3);
    write_desktop_mode(ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3D8_CheckDeviceType(Guest* g) { RET(ARG(1) ? D3DERR_INVALIDCALL : D3D_OK, 6); }

/* What a Metal device can back. Paletted and 3-3-2 formats are out, as on current D3D9 drivers. */
static int format_ok(uint32_t usage, uint32_t rtype, uint32_t f)
{
    if (rtype == RT_VERTEXBUFFER || rtype == RT_INDEXBUFFER)
        return 1;
    if (rtype == RT_VOLUMETEXTURE)
        return 0;
    if (usage & USAGE_DEPTHSTENCIL)
        return f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8;
    if (usage & USAGE_RENDERTARGET)
        return f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_X1R5G5B5 || f == FMT_A1R5G5B5;
    switch (f)
    {
    case FMT_A8R8G8B8:
    case FMT_X8R8G8B8:
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8:
    case FMT_L8:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_DXT1:
    case FMT_DXT2:
    case FMT_DXT3:
    case FMT_DXT4:
    case FMT_DXT5:
        return 1;
    case FMT_D16:
    case FMT_D24S8:
    case FMT_D24X8:
        return rtype == RT_SURFACE || rtype == RT_TEXTURE;
    default: return 0;
    }
}

static void IDirect3D8_CheckDeviceFormat(Guest* g)
{
    RET(!ARG(1) && format_ok(ARG(4), ARG(5), ARG(6)) ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void IDirect3D8_CheckDeviceMultiSampleType(Guest* g) { RET(ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
static void IDirect3D8_CheckDepthStencilMatch(Guest* g) { RET(D3D_OK, 6); }

static void IDirect3D8_GetDeviceCaps(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    for (int i = 0; i < 53; ++i)
        wr32(ARG(3) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(3), ARG(2)); /* DeviceType as asked */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterMonitor(Guest* g) { RET(0x00050001u, 2); }

/* CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice) */
static void IDirect3D8_CreateDevice(Guest* g)
{
    wr32(ARG(6), 0);
    if (ARG(1) || g_dev.guest)
        RET(g_dev.guest ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL, 7);
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(5) + 4u * (uint32_t)i);
    d->hwnd = d->pp[6] ? d->pp[6] : ARG(3);
    d->behavior = ARG(4);
    d->device_type = ARG(2);
    d->d3d = ARG(0);
    if (!d->pp[0] || !d->pp[1])
    {
        uint32_t w = 640, h = 480;
        user32_client_size(d->hwnd, &w, &h);
        if (!d->pp[0])
            d->pp[0] = w;
        if (!d->pp[1])
            d->pp[1] = h;
    }
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    /* windowed devices ignore the presentation interval and wait for the display; so do we unless
     * a full-screen device asks for IMMEDIATE */
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    gfx_init(user32_sdl_window(d->hwnd), d->pp[7] || d->pp[12] != 0x80000000u);
    obj_addref(d->d3d);
    d->guest = obj_new(O_DEVICE);
    d->backbuffer = new_surface(d->pp[2], d->pp[0], d->pp[1], USAGE_RENDERTARGET, 0, 0);
    obj(d->backbuffer)->implicit = 1;
    if (d->pp[8])
    {
        d->depth = new_surface(d->pp[9], d->pp[0], d->pp[1], USAGE_DEPTHSTENCIL, 0, 0);
        obj(d->depth)->implicit = 1;
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: device %ux%u format %u%s, depth %u, behavior %08x, window %08x\n", d->pp[0], d->pp[1],
        d->pp[2], d->pp[7] ? " windowed" : " fullscreen", d->pp[8] ? d->pp[9] : 0, d->behavior, d->hwnd);
    wr32(ARG(6), d->guest);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: device -------------------------------------------------------------------------- */
static void IDirect3DDevice8_TestCooperativeLevel(Guest* g) { RET(D3D_OK, 1); }
static void IDirect3DDevice8_GetAvailableTextureMem(Guest* g) { RET(512u << 20, 1); }
static void IDirect3DDevice8_ResourceManagerDiscardBytes(Guest* g) { RET(D3D_OK, 2); }

static void IDirect3DDevice8_GetDirect3D(Guest* g)
{
    obj_addref(g_dev.d3d);
    wr32(ARG(1), g_dev.d3d);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDeviceCaps(Guest* g)
{
    for (int i = 0; i < 53; ++i)
        wr32(ARG(1) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(1), g_dev.device_type);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDisplayMode(Guest* g)
{
    write_desktop_mode(ARG(1));
    if (!g_dev.pp[7]) /* fullscreen: the mode the device set */
    {
        wr32(ARG(1), g_dev.pp[0]);
        wr32(ARG(1) + 4, g_dev.pp[1]);
        wr32(ARG(1) + 12, g_dev.pp[2]);
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetCreationParameters(Guest* g)
{
    wr32(ARG(1), 0);
    wr32(ARG(1) + 4, g_dev.device_type);
    wr32(ARG(1) + 8, g_dev.hwnd);
    wr32(ARG(1) + 12, g_dev.behavior);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetCursorProperties(Guest* g) { RET(D3D_OK, 4); }
static void IDirect3DDevice8_SetCursorPosition(Guest* g) { RET(0, 4); }

static void IDirect3DDevice8_ShowCursor(Guest* g)
{
    int was = g_dev.cursor;
    g_dev.cursor = ARG(1) != 0;
    RET((uint32_t)was, 2);
}

static void IDirect3DDevice8_Reset(Guest* g)
{
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(1) + 4u * (uint32_t)i);
    if (!d->pp[0] || !d->pp[1])
        user32_client_size(d->hwnd, &d->pp[0], &d->pp[1]);
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    Obj* bb = obj(d->backbuffer);
    bb->format = d->pp[2], bb->width = d->pp[0], bb->height = d->pp[1], bb->size = fmt_size(bb->format, bb->width, bb->height);
    if (bb->mem)
        gheap_free(bb->mem), bb->mem = 0;
    gfx_tex_destroy(bb->gpu);
    bb->gpu = gfx_tex_create(GFX_TEX_2D, bb->format, bb->width, bb->height, 1, GFX_USE_RT);
    Obj* z = obj(d->depth);
    if (z)
    {
        z->width = d->pp[0], z->height = d->pp[1], z->size = fmt_size(z->format, z->width, z->height);
        if (z->mem)
            gheap_free(z->mem), z->mem = 0;
        gfx_tex_destroy(z->gpu);
        z->gpu = gfx_tex_create(GFX_TEX_2D, z->format, z->width, z->height, 1, GFX_USE_DEPTH);
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    for (int t = 0; t < 8; ++t)
        bind(&d->cur.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&d->cur.stream[i], 0);
    bind(&d->cur.ib, 0);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: reset to %ux%u\n", d->pp[0], d->pp[1]);
    RET(D3D_OK, 2);
}

/* Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion): the frame is done */
static void (*g_present_hook)(void);

void d3d8_set_present_hook(void (*fn)(void)) { g_present_hook = fn; }

void d3d8_screen_size(uint32_t* w, uint32_t* h)
{
    *w = g_dev.pp[0], *h = g_dev.pp[1];
    if (g_dev.hwnd)
        user32_client_size(g_dev.hwnd, w, h);
}

static void (*g_overlay)(GfxTex* backbuffer, uint32_t w, uint32_t h); /* d3d8_set_overlay */
static void ui_present(void);
static void apply_targets(void);
static void (*g_after_present)(void);
static int native_size(uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph);

static void IDirect3DDevice8_Present(Guest* g)
{
    cap_present();
    scene_present();
    ui_present();
    if (g_present_hook)
        g_present_hook();
    Obj* bb = obj(g_dev.backbuffer);
    if (g_overlay && bb && bb->gpu)
        g_overlay(bb->gpu, bb->width, bb->height);
    gfx_present(bb ? bb->gpu : NULL);
    if (g_after_present)
        g_after_present();
    RET(D3D_OK, 5);
}

void d3d8_set_after_present(void (*fn)(void)) { g_after_present = fn; }

void d3d8_set_overlay(void (*fn)(GfxTex* backbuffer, uint32_t w, uint32_t h)) { g_overlay = fn; }

void d3d8_backbuffer_size(uint32_t* w, uint32_t* h)
{
    Obj* bb = obj(g_dev.backbuffer);
    *w = bb ? bb->width : 0;
    *h = bb ? bb->height : 0;
}

uint32_t d3d8_window(void) { return g_dev.hwnd; }

int d3d8_get_transform(uint32_t ts, float m[16])
{
    int i = xf_index(ts);
    if (!g_dev.guest || i < 0)
        return 0;
    memcpy(m, g_dev.cur.xf[i], 16 * sizeof(float));
    return 1;
}

int d3d8_get_render_state(uint32_t rs, uint32_t* v)
{
    if (!g_dev.guest || rs >= 256)
        return 0;
    *v = g_dev.cur.rs[rs];
    return 1;
}

/* The back buffer (and its depth) at a new size, the game's objects kept: its next frame draws into
 * them at once. The render targets drawn at the screen's resolution (native_size) follow it. Between
 * frames (d3d8_set_after_present); the game's own copies of the size are the host's to change. */
int d3d8_resize(uint32_t w, uint32_t h)
{
    Dev* d = &g_dev;
    Obj* bb = obj(d->backbuffer);
    if (!d->guest || !bb || w < 64 || h < 64)
        return 0;
    if (w == d->pp[0] && h == d->pp[1])
        return 1;
    d->pp[0] = w, d->pp[1] = h;
    bb->width = w, bb->height = h, bb->size = fmt_size(bb->format, w, h);
    if (bb->mem)
        gheap_free(bb->mem), bb->mem = 0;
    scene_forget(bb->gpu);
    gfx_tex_destroy(bb->gpu);
    bb->gpu = gfx_tex_create(GFX_TEX_2D, bb->format, w, h, 1, GFX_USE_RT);
    Obj* z = obj(d->depth);
    if (z)
    {
        z->width = w, z->height = h, z->size = fmt_size(z->format, w, h);
        if (z->mem)
            gheap_free(z->mem), z->mem = 0;
        scene_forget(z->gpu);
        gfx_tex_destroy(z->gpu);
        z->gpu = gfx_tex_create(GFX_TEX_2D, z->format, w, h, 1, GFX_USE_DEPTH);
    }
    /* the interface's targets: drawn at the new screen size (their contents are drawn again each frame) */
    for (uint32_t i = 1; i < g_nobjs; ++i)
    {
        Obj* t = &g_objs[i];
        if (t->kind != O_TEXTURE || !t->pw)
            continue;
        uint32_t pw = 0, ph = 0;
        if (!native_size(t->width, t->height, &pw, &ph))
            pw = t->width, ph = t->height;
        if (pw == t->pw && ph == t->ph)
            continue;
        scene_forget(t->gpu);
        gfx_tex_destroy(t->gpu);
        t->pw = pw == t->width && ph == t->height ? 0 : pw, t->ph = t->pw ? ph : 0;
        t->gpu = gfx_tex_create(GFX_TEX_2D, t->format, pw, ph, 1, GFX_USE_RT);
    }
    apply_targets();
    rt_log("[recomp] d3d8: back buffer now %ux%u\n", w, h);
    return 1;
}

static void IDirect3DDevice8_GetBackBuffer(Guest* g)
{
    if (ARG(1))
    {
        wr32(ARG(3), 0);
        RET(D3DERR_INVALIDCALL, 4);
    }
    obj_addref(g_dev.backbuffer);
    wr32(ARG(3), g_dev.backbuffer);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetRasterStatus(Guest* g)
{
    wr32(ARG(1), 1); /* InVBlank */
    wr32(ARG(1) + 4, 0);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetGammaRamp(Guest* g) { RET(0, 3); }

/* --- resources ------------------------------------------------------------------------------------------ */
static uint32_t chain_levels(uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t full = 1;
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1)
        full++;
    return !levels || levels > full ? full : levels;
}

/* FFXI draws its interface into a render-target texture of the menu resolution (the registry's
 * 0037 x 0038: half the window, say) and stretches that over the back buffer, so its text is drawn at
 * half the screen's resolution and doubled. A render-target texture smaller than the back buffer and
 * of its shape is instead given a GPU texture of the back buffer's size: the game still sees its own
 * size (locks, descriptions, XYZRHW coordinates), and its draws land on the larger texture through a
 * viewport scaled to match (scale_to_target), so the interface is drawn at the screen's resolution
 * and the final stretch is one to one. FFXI_UI_NATIVE=0 draws it at the menu resolution again. */
/* w x h is smaller than the screen and of the --ui-aspect box's shape (1280x720 for 16:9, 1px either
 * way): narrower than the screen in a wider window, shorter in a taller one */
static int ui_box_shape(uint32_t w, uint32_t h)
{
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if ((s >= 1.0f && sy >= 1.0f) || !h || w >= bw || h >= bh)
        return 0;
    double boxw = bw * (double)s, boxh = bh * (double)sy;
    return fabs((double)w * boxh - boxw * h) <= boxh;
}

static int native_size(uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph)
{
    static int on = -1;
    if (on < 0)
        on = !(getenv("FFXI_UI_NATIVE") && getenv("FFXI_UI_NATIVE")[0] == '0');
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    if (!on || !bw || !bh || w >= bw || h >= bh)
        return 0;
    if ((uint64_t)w * bh == (uint64_t)h * bw)
    {
        *pw = bw, *ph = bh;
        return 1;
    }
    /* under --ui-aspect the menu target is of the interface box's shape; it still covers the screen,
     * its draws squeezed toward the middle (ui_squeeze) */
    if (!ui_box_shape(w, h))
        return 0;
    *pw = bw, *ph = bh;
    return 1;
}

/* a surface of a texture native_size enlarged */
static int surface_scaled(const Obj* s)
{
    Obj* t = s->container ? obj(s->container) : NULL;
    return t && t->pw;
}

/* A viewport (x, y, w, h first) or D3DRECTs from the game's pixels to the current target's. */
static void scale_to_target(uint32_t* vp, int32_t* rects, uint32_t nrects)
{
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    if (!t || !t->pw)
        return;
    double kx = (double)t->pw / t->width, ky = (double)t->ph / t->height;
    if (vp)
        vp[0] = (uint32_t)(vp[0] * kx + 0.5), vp[1] = (uint32_t)(vp[1] * ky + 0.5), vp[2] = (uint32_t)(vp[2] * kx + 0.5),
        vp[3] = (uint32_t)(vp[3] * ky + 0.5);
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t* r = rects + 4 * i;
        r[0] = (int32_t)(r[0] * kx + 0.5), r[1] = (int32_t)(r[1] * ky + 0.5), r[2] = (int32_t)(r[2] * kx + 0.5),
        r[3] = (int32_t)(r[3] * ky + 0.5);
    }
}

static uint32_t new_texture(int kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    uint32_t p = obj_new(kind);
    Obj* t = obj(p);
    t->width = w, t->height = h, t->usage = usage, t->format = fmt, t->pool = pool;
    uint32_t nl = t->levels = chain_levels(w, h, levels);
    uint32_t faces = kind == O_CUBE ? 6 : 1;
    uint32_t* subs = t->subs = (uint32_t*)calloc(faces * nl, 4);
    t->nsubs = faces * nl;
    /* new_surface may grow the object table: t is not used past here */
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < nl; ++l)
        {
            uint32_t sp = new_surface(fmt, w >> l ? w >> l : 1, h >> l ? h >> l : 1, usage, pool, p);
            subs[f * nl + l] = sp;
            obj(sp)->face = (uint8_t)f, obj(sp)->level = (uint8_t)l;
        }
    uint32_t pw = 0, ph = 0;
    if (kind == O_TEXTURE && (usage & USAGE_RENDERTARGET) && nl == 1 && pool != 2 && native_size(w, h, &pw, &ph))
    {
        obj(p)->pw = pw, obj(p)->ph = ph;
        rt_log("[recomp] d3d8: %ux%u render target drawn at %ux%u\n", w, h, pw, ph);
    }
    if (pool != 2) /* system memory textures are only ever copied from */
        obj(p)->gpu = gfx_tex_create(kind == O_CUBE ? GFX_TEX_CUBE : GFX_TEX_2D, fmt, pw ? pw : w, ph ? ph : h, nl,
            (usage & USAGE_RENDERTARGET) ? GFX_USE_RT : GFX_USE_SAMPLE);
    return p;
}

/* CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture) */
static void IDirect3DDevice8_CreateTexture(Guest* g)
{
    wr32(ARG(7), 0);
    if (!ARG(1) || !ARG(2) || !format_ok(ARG(4), RT_TEXTURE, ARG(5)))
        RET(D3DERR_INVALIDCALL, 8);
    wr32(ARG(7), new_texture(O_TEXTURE, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)));
    RET(D3D_OK, 8);
}

/* CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture) */
static void IDirect3DDevice8_CreateCubeTexture(Guest* g)
{
    wr32(ARG(6), 0);
    if (!ARG(1) || !format_ok(ARG(3), RT_CUBETEXTURE, ARG(4)))
        RET(D3DERR_INVALIDCALL, 7);
    wr32(ARG(6), new_texture(O_CUBE, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)));
    RET(D3D_OK, 7);
}

static void IDirect3DDevice8_CreateVolumeTexture(Guest* g)
{
    wr32(ARG(8), 0);
    RET(D3DERR_NOTAVAILABLE, 9);
}

#define USAGE_DYNAMIC 0x200u

/* A buffer the game does not declare dynamic is written once and drawn many times: it gets a GPU
 * copy, refreshed at the next draw after a lock that writes. Dynamic ones are copied per draw. */
static void static_buffer_init(Obj* b)
{
    if (!(b->usage & USAGE_DYNAMIC) && b->size)
    {
        b->gbuf = gfx_buf_create(b->size);
        b->gbuf_dirty = 1;
    }
}

static GfxBuf* static_buffer(Obj* b)
{
    if (!b->gbuf || !b->mem)
        return NULL;
    if (b->gbuf_dirty)
    {
        gfx_buf_upload(b->gbuf, GUEST_PTR(b->mem), b->size);
        b->gbuf_dirty = 0;
    }
    return b->gbuf;
}

/* CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer) */
static void IDirect3DDevice8_CreateVertexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_VB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->fvf = ARG(3), b->pool = ARG(4);
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer) */
static void IDirect3DDevice8_CreateIndexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_IB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->format = ARG(3), b->pool = ARG(4);
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface) */
static void IDirect3DDevice8_CreateRenderTarget(Guest* g)
{
    wr32(ARG(6), new_surface(ARG(3), ARG(1), ARG(2), USAGE_RENDERTARGET, 0, 0));
    RET(D3D_OK, 7);
}

/* CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface) */
static void IDirect3DDevice8_CreateDepthStencilSurface(Guest* g)
{
    wr32(ARG(5), new_surface(ARG(3), ARG(1), ARG(2), USAGE_DEPTHSTENCIL, 0, 0));
    RET(D3D_OK, 6);
}

/* CreateImageSurface(Width, Height, Format, ppSurface): system memory */
static void IDirect3DDevice8_CreateImageSurface(Guest* g)
{
    wr32(ARG(4), new_surface(ARG(3), ARG(1), ARG(2), 0, 2, 0));
    RET(D3D_OK, 5);
}

/* CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray).
 * Between two surfaces the GPU owns (the back buffer into a render-target texture, say) the copy is
 * a GPU blit. Otherwise it happens in guest memory - a GPU-owned source is read back first - and the
 * destination is uploaded: at once if the GPU owns it, before its next draw if not. */
static void IDirect3DDevice8_CopyRects(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(4));
    if (!s || !d || s->format != d->format)
        RET(D3DERR_INVALIDCALL, 6);
    if (surface_scaled(s) || surface_scaled(d))
    {
        static int said;
        if (!said)
            said = 1, rt_log("[recomp] d3d8: CopyRects with a render target drawn larger is skipped\n");
        RET(D3D_OK, 6);
    }
    uint32_t sface, slevel, dface, dlevel;
    GfxTex* sg = surface_gpu(s, &sface, &slevel);
    GfxTex* dg = surface_gpu(d, &dface, &dlevel);
    int s_gpu = gpu_owned(s), d_gpu = gpu_owned(d);
    uint32_t n = ARG(2) ? ARG(3) : 1, blk = fmt_block(s->format), unit = blk ? 4 : 1, bpp = blk ? blk : fmt_bytes(s->format);
    uint32_t sp = fmt_pitch(s->format, s->width), dp = fmt_pitch(d->format, d->width);
    uint8_t *sm = NULL, *dm = NULL;
    if (!(s_gpu && d_gpu))
    {
        sm = GUEST_PTR(obj_mem(s));
        dm = GUEST_PTR(obj_mem(d));
        if (s_gpu && !(s->usage & USAGE_DEPTHSTENCIL))
            gfx_tex_read(sg, sface, slevel, sm, sp);
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        int32_t l = 0, t = 0, r = (int32_t)s->width, b = (int32_t)s->height;
        if (ARG(2))
        {
            uint32_t rc = ARG(2) + 16 * i;
            l = (int32_t)rd32(rc), t = (int32_t)rd32(rc + 4), r = (int32_t)rd32(rc + 8), b = (int32_t)rd32(rc + 12);
        }
        int32_t x = l, y = t;
        if (ARG(5))
            x = (int32_t)rd32(ARG(5) + 8 * i), y = (int32_t)rd32(ARG(5) + 8 * i + 4);
        if (l < 0 || t < 0 || r > (int32_t)s->width || b > (int32_t)s->height || r <= l || b <= t || x < 0 || y < 0 ||
            x + (r - l) > (int32_t)d->width || y + (b - t) > (int32_t)d->height)
            continue;
        if (s_gpu && d_gpu)
        {
            gfx_copy(sg, sface, slevel, (uint32_t)l, (uint32_t)t, (uint32_t)(r - l), (uint32_t)(b - t), dg, dface, dlevel,
                (uint32_t)x, (uint32_t)y);
            continue;
        }
        uint32_t rows = ((uint32_t)(b - t) + unit - 1) / unit, bytes = ((uint32_t)(r - l) + unit - 1) / unit * bpp;
        uint8_t* drow = dm + ((uint32_t)y / unit) * dp + (uint32_t)x / unit * bpp;
        for (uint32_t row = 0; row < rows; ++row)
            memmove(drow + row * dp, sm + ((uint32_t)t / unit + row) * sp + (uint32_t)l / unit * bpp, bytes);
        if (d_gpu && !(d->usage & USAGE_DEPTHSTENCIL))
            gfx_tex_upload_rect(dg, dface, dlevel, (uint32_t)x, (uint32_t)y, (uint32_t)(r - l), (uint32_t)(b - t), drow, dp);
    }
    if (!d_gpu)
        mark_dirty(d);
    RET(D3D_OK, 6);
}

/* SetRenderTarget(pRenderTarget, pNewZStencil): NULL render target keeps the current one */
static void IDirect3DDevice8_SetRenderTarget(Guest* g)
{
    if (ARG(1))
    {
        bind(&g_dev.rt, ARG(1));
        Obj* rt = obj(ARG(1));
        if (rt) /* D3D8 resets the viewport to the new target */
        {
            uint32_t* vp = g_dev.cur.vp;
            vp[0] = vp[1] = 0, vp[2] = rt->width, vp[3] = rt->height, vp[4] = 0, vp[5] = f2u(1.0f);
        }
    }
    bind(&g_dev.ds, ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderTarget(Guest* g)
{
    obj_addref(g_dev.rt);
    wr32(ARG(1), g_dev.rt);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDepthStencilSurface(Guest* g)
{
    wr32(ARG(1), g_dev.ds);
    if (!g_dev.ds)
        RET(0x88760834u, 2); /* D3DERR_NOTFOUND */
    obj_addref(g_dev.ds);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_BeginScene(Guest* g) { RET(D3D_OK, 1); }
static void IDirect3DDevice8_EndScene(Guest* g) { RET(D3D_OK, 1); }
static void apply_targets(void);

/* Clear(Count, pRects, Flags, Color, Z, Stencil): D3DRECTs are x1, y1, x2, y2 */
static void IDirect3DDevice8_Clear(Guest* g)
{
    apply_targets();
    uint32_t n = ARG(2) ? ARG(1) : 0, vp[6];
    memcpy(vp, g_dev.cur.vp, sizeof vp);
    int32_t few[4 * 16], *rects = n <= 16 ? few : (int32_t*)malloc(16u * n);
    if (n)
        memcpy(rects, ARGP(2), 16u * n);
    scale_to_target(vp, rects, n);
    gfx_clear(n, n ? rects : NULL, ARG(3), ARG(4), u2f(ARG(5)), ARG(6), vp);
    if (rects != few)
        free(rects);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: state ----------------------------------------------------------------------------- */
static void IDirect3DDevice8_SetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], ARGP(2), 64);
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.xf[i], 64);
    RET(D3D_OK, 3);
}

/* MultiplyTransform(State, pMatrix): the state's matrix becomes pMatrix * matrix */
static void IDirect3DDevice8_MultiplyTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    float a[16], r[16];
    memcpy(a, ARGP(2), 64);
    const float* b = g_dev.cur.xf[i];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            r[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], r, 64);
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetViewport(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->vp, ARGP(1), 24);
    if (m)
        m->vp = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetViewport(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.vp, 24);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetMaterial(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->mat, ARGP(1), 68);
    if (m)
        m->mat = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetMaterial(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.mat, 68);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetLight(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->light[i].v, ARGP(2), 104);
    raise_top(&g_light_top, i + 1);
    if (m)
        m->light[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLight(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.light[ARG(1)].v, 104);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_LightEnable(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    if (!s->light[i].v[0]) /* never set: D3D enables a default light */
    {
        default_light(&s->light[i]);
        if (m)
            m->light[i] = 1;
    }
    s->light[i].enabled = ARG(2) != 0;
    raise_top(&g_light_top, i + 1);
    if (m)
        m->lighten[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLightEnable(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.light[ARG(1)].enabled);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetClipPlane(Guest* g)
{
    if (ARG(1) >= 6)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->clip[ARG(1)], ARGP(2), 16);
    if (m)
        m->clip[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetRenderState(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    s->rs[i] = ARG(2);
    if (m)
        m->rs[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderState(Guest* g)
{
    if (ARG(1) >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.rs[ARG(1)]);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_BeginStateBlock(Guest* g)
{
    if (g_dev.rec)
        RET(D3DERR_INVALIDCALL, 1);
    g_dev.rec = (Block*)calloc(1, sizeof(Block));
    RET(D3D_OK, 1);
}

static void IDirect3DDevice8_EndStateBlock(Guest* g)
{
    if (!g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(ARG(1), block_token(g_dev.rec));
    g_dev.rec = NULL;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_ApplyStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&g_dev.cur, &b->s, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_CaptureStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&b->s, &g_dev.cur, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    block_free(b);
    g_dev.blocks[ARG(1) - 1] = NULL;
    RET(D3D_OK, 2);
}

/* CreateStateBlock(Type, pToken): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3. The pixel and vertex
 * sets here are the categories, not D3D's exact render-state lists: rendering state that would be
 * captured by one or the other is captured by both. */
static void IDirect3DDevice8_CreateStateBlock(Guest* g)
{
    uint32_t type = ARG(1);
    if (type < 1 || type > 3 || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 3);
    Block* b = (Block*)calloc(1, sizeof(Block));
    /* state_copy stops at the high-water marks (g_xf_top, g_light_top, g_vsc_top): past them the block
     * starts as the device is (identity transforms, lights off, zero constants), so one applied after
     * the game first sets such an entry puts its default back, not zeros */
    memcpy(b->s.xf, g_dev.cur.xf, sizeof b->s.xf);
    memcpy(b->s.light, g_dev.cur.light, sizeof b->s.light);
    memcpy(b->s.vsc, g_dev.cur.vsc, sizeof b->s.vsc);
    Mask* m = &b->m;
    memset(m->rs, 1, sizeof m->rs);
    memset(m->tss, 1, sizeof m->tss);
    if (type == 1 || type == 2)
    {
        m->ps = 1;
        memset(m->psc, 1, sizeof m->psc);
    }
    if (type == 1 || type == 3)
    {
        m->vs = 1;
        memset(m->vsc, 1, sizeof m->vsc);
        memset(m->light, 1, sizeof m->light);
        memset(m->lighten, 1, sizeof m->lighten);
    }
    if (type == 1)
    {
        memset(m->tex, 1, sizeof m->tex);
        memset(m->xf, 1, sizeof m->xf);
        memset(m->clip, 1, sizeof m->clip);
        memset(m->stream, 1, sizeof m->stream);
        m->vp = m->mat = m->ib = 1;
    }
    state_copy(&b->s, &g_dev.cur, m);
    wr32(ARG(2), block_token(b));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t t = g_dev.cur.tex[ARG(1)];
    if (t)
        obj_addref(t);
    wr32(ARG(2), t);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    bind(&s->tex[ARG(1)], ARG(2));
    if (m)
        m->tex[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    wr32(ARG(3), g_dev.cur.tss[ARG(1)][ARG(2)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    s->tss[ARG(1)][ARG(2)] = ARG(3);
    if (m)
        m->tss[ARG(1)][ARG(2)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_ValidateDevice(Guest* g)
{
    wr32(ARG(1), 1);
    RET(D3D_OK, 2);
}

/* --- IDirect3DDevice8: drawing ----------------------------------------------------------------------------- */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride);

/* DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount) */
static void IDirect3DDevice8_DrawPrimitive(Guest* g)
{
    g_cap_esp = g->esp;
    draw(ARG(1), ARG(3), ARG(2), 0, 0, 0, 0);
    RET(D3D_OK, 4);
}

/* DrawIndexedPrimitive(PrimitiveType, minIndex, NumVertices, startIndex, primCount): the vertex range
 * comes from the indices themselves, not minIndex and NumVertices */
static void IDirect3DDevice8_DrawIndexedPrimitive(Guest* g)
{
    g_cap_esp = g->esp;
    Obj* ib = obj(g_dev.cur.ib);
    if (ib && ib->mem)
    {
        uint32_t isize = ib->format == FMT_INDEX32 ? 4 : 2;
        draw(ARG(1), ARG(5), 0, ib->mem + ARG(4) * isize, isize, 0, 0);
    }
    else
        gfx_prof_skip(GFX_SKIP_NO_INDICES);
    RET(D3D_OK, 6);
}

/* DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
 * The ...UP draws leave stream 0 (and the indices) unset, as D3D8 does. */
/* A glyph quad from a texture whose pack entry has a glyph table: FFXI draws each glyph of its name
 * and small menu fonts as a fixed-width cell from where the glyph starts in the sheet, so the cell also
 * takes in the edge of the glyph beside it there (shown between letters) and cuts the glyph's own lean
 * and outline. The quad is drawn over the ink box of the glyph that starts in the cell instead: the
 * same texels where they overlap, so it lands where the game put it. */
static uint32_t fit_quad(uint32_t data, const Obj* t)
{
    const PackEntry* e = (const PackEntry*)t->repl_entry;
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    float x[4], u[4], v[4], ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f, xlo = 1e30f, xhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)) * (float)t->width;
        v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24)) * (float)t->height;
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi, vlo = v[i] < vlo ? v[i] : vlo, vhi = v[i] > vhi ? v[i] : vhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo))
        return data;
    const float* best = NULL;
    for (uint32_t k = 0; k < e->nglyphs; ++k)
    {
        const float* g = e->glyphs + 4 * k;
        float cy = (g[2] + g[3]) * 0.5f;
        if (cy < vlo || cy > vhi || g[0] < ulo - 3.0f || g[0] >= uhi)
            continue;
        if (!best || g[0] < best[0])
            best = g;
    }
    if (!best)
        return data;
    float k = (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        float nu = u[i] - ulo < uhi - u[i] ? best[0] : best[1];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (nu - u[i]) * k));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(nu / (float)t->width));
    }
    return buf;
}

/* A glyph quad (a strip of four XYZRHW + diffuse + UV vertices) from a texture whose pack entry
 * asks for it, drawn repl_pad texels wider on each side, its UVs moved with it: FFXI cuts each
 * glyph of its italic name font at an upright box, so the lean and the outline at the ends of a
 * name were lost (the neighbours hide it inside the name). A copy: the game's vertices stay. */
static uint32_t widen_quad(uint32_t data, const Obj* t)
{
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    float x[4], u[4], ulo = 1e30f, uhi = -1e30f, xlo = 1e30f, xhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        float v = u2f(rd32(data + 28u * (uint32_t)i + 24));
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20));
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
        vlo = v < vlo ? v : vlo, vhi = v > vhi ? v : vhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo) || (vhi - vlo) * (float)t->height < (float)t->repl_pad_min)
        return data;
    float du = (float)t->repl_pad / (float)t->width, dx = du * (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        int lo = u[i] - ulo < uhi - u[i];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (lo ? -dx : dx)));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (lo ? -du : du)));
    }
    return buf;
}

/* An axis-aligned sprite quad drawn into a render target native_size enlarged: at the menu
 * resolution a quad's edges fall between pixels, but enlarged (2.5x) an edge can fall on a pixel's
 * centre, which then samples the quad's texture exactly at its edge - and a hair past it, wrapping to
 * the far side of the sheet. FFXI draws its wider menu buttons as two strips side by side, the right
 * one starting at u 0 of a sheet whose last column is transparent there: the alpha test dropped that
 * pixel, a black line down the button (the selected one, a pixel off the others, lands on a centre);
 * Config > Modern's pills stretch a body from u 0 of the same sheet. The UVs are pulled in by a sliver of a texel, which point sampling does not otherwise show. A copy:
 * the game's vertices stay. */
static int g_cap_inset; /* the drawlog: what inset_quad did with the draw it notes (-1 not a sprite, 1 inset) */

static uint32_t inset_quad(uint32_t data, const Obj* t)
{
    g_cap_inset = -1;
    Obj* rt = obj(g_dev.rt);
    Obj* rtt = rt && rt->container ? obj(rt->container) : NULL;
    if (!rtt || !rtt->pw || g_dev.cur.tss[0][16] != 1 || !t->width || !t->height)
        return data;
    float ex = (1.0f / 64.0f) / (float)t->width, ey = (1.0f / 64.0f) / (float)t->height;
    float x[4], y[4], u[4], v[4];
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), y[i] = u2f(rd32(data + 28u * (uint32_t)i + 4));
        u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)), v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24));
    }
    /* axis aligned: vertices 0 and 1 share a row, 0 and 2 a column (a strip) */
    if (fabsf(y[0] - y[1]) > 0.01f || fabsf(y[2] - y[3]) > 0.01f || fabsf(x[0] - x[2]) > 0.01f ||
        fabsf(x[1] - x[3]) > 0.01f || fabsf(v[0] - v[1]) > 1e-6f || fabsf(u[0] - u[2]) > 1e-6f)
        return data;
    g_cap_inset = 1;
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    float uc = (u[0] + u[1]) * 0.5f, vc = (v[0] + v[2]) * 0.5f;
    for (int i = 0; i < 4; ++i)
    {
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (u[i] < uc ? ex : u[i] > uc ? -ex : 0.0f)));
        wr32(buf + 28u * (uint32_t)i + 24, f2u(v[i] + (v[i] < vc ? ey : v[i] > vc ? -ey : 0.0f)));
    }
    return buf;
}

static void IDirect3DDevice8_DrawPrimitiveUP(Guest* g)
{
    g_cap_esp = g->esp;
    if (!(g_dev.cur.vs & 1) && (g_dev.cur.vs & 0xE) == 4 && ARG(3) && rd32(ARG(3) + 8) == 0x3f7ffffeu)
        g_probe_sky = 1; /* transformed vertices at the sky's depth */
    uint32_t data = ARG(3);
    Obj* t0 = obj(g_dev.cur.tex[0]);
    if (t0 && t0->repl && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
    {
        if (t0->repl_entry && ((const PackEntry*)t0->repl_entry)->nglyphs)
            data = fit_quad(data, t0);
        else if (t0->repl_pad)
            data = widen_quad(data, t0);
    }
    else if (t0 && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
        data = inset_quad(data, t0);
    draw(ARG(1), ARG(2), 0, 0, 0, data, ARG(4));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    RET(D3D_OK, 5);
}

/* DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData,
 * IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) */
static void IDirect3DDevice8_DrawIndexedPrimitiveUP(Guest* g)
{
    g_cap_esp = g->esp;
    draw(ARG(1), ARG(4), 0, ARG(5), ARG(6) == FMT_INDEX32 ? 4 : 2, ARG(7), ARG(8));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    bind(&g_dev.cur.ib, 0);
    RET(D3D_OK, 9);
}

/* --- IDirect3DDevice8: shaders and streams ------------------------------------------------------------------ */
static uint32_t* copy_tokens(uint32_t p, uint32_t end, uint32_t* n)
{
    uint32_t k = 0;
    while (k < 65536 && rd32(p + 4 * k) != end)
        k++;
    k++;
    uint32_t* t = (uint32_t*)malloc(4 * k);
    memcpy(t, GUEST_PTR(p), 4 * k);
    *n = k;
    return t;
}

static uint32_t type_size(uint32_t t)
{
    return t == GFX_FLOAT2 || t == GFX_SHORT4 ? 8 : t == GFX_FLOAT3 ? 12 : t == GFX_FLOAT4 ? 16 : 4;
}

static void add_elem(Layout* l, int reg, uint32_t stream, uint32_t type, uint32_t* off)
{
    if (reg >= GFX_NREGS)
        return;
    l->el[reg] = (GfxElem){ 1, (uint8_t)stream, (uint8_t)type, 0 };
    l->offset[reg] = (int32_t)*off;
    *off += type_size(type);
}

/* an FVF code: everything in stream 0, in D3D's order */
static void fvf_layout(uint32_t fvf, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t off = 0, pos = fvf & 0xE;
    if (pos == 4) /* XYZRHW */
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT4, &off);
        l->rhw = 1;
    }
    else if (pos)
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT3, &off);
        if (pos >= 6) /* XYZB1..5: blend weights, the last one indices with LASTBETA_UBYTE4 */
        {
            int betas = (int)(pos - 4) / 2, weights = betas - ((fvf & 0x1000) ? 1 : 0);
            if (weights > 4)
                weights = 4;
            if (weights > 0)
                add_elem(l, GFX_R_BLENDWEIGHT, 0, GFX_FLOAT1 + (uint32_t)weights - 1, &off);
            if (fvf & 0x1000)
                add_elem(l, GFX_R_BLENDINDICES, 0, GFX_UBYTE4, &off);
        }
    }
    if (fvf & 0x10)
        add_elem(l, GFX_R_NORMAL, 0, GFX_FLOAT3, &off);
    if (fvf & 0x20)
        add_elem(l, GFX_R_PSIZE, 0, GFX_FLOAT1, &off);
    if (fvf & 0x40)
        add_elem(l, GFX_R_DIFFUSE, 0, GFX_D3DCOLOR, &off);
    if (fvf & 0x80)
        add_elem(l, GFX_R_SPECULAR, 0, GFX_D3DCOLOR, &off);
    uint32_t n = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < n && i < 8; ++i)
    {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        add_elem(l, GFX_R_TEXCOORD0 + (int)i, 0, f == 0 ? GFX_FLOAT2 : f == 1 ? GFX_FLOAT3 : f == 2 ? GFX_FLOAT4 : GFX_FLOAT1, &off);
    }
}

/* a vertex shader declaration: D3DVSD_STREAM, then D3DVSD_REG / D3DVSD_SKIP per element */
static void decl_layout(const uint32_t* t, uint32_t n, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t stream = 0, off = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t tok = t[i];
        if (tok == 0xFFFFFFFFu)
            break;
        switch (tok >> 29)
        {
        case 1: stream = tok & 0xF, off = 0; break;
        case 2:
            if ((tok >> 28) & 1)
                off += 4 * ((tok >> 16) & 0xF);
            else
                add_elem(l, (int)(tok & 0x1F), stream, (tok >> 16) & 0xF, &off);
            break;
        case 4: i += 4 * ((tok >> 25) & 0xF); break; /* constants: SetVertexShader loads them */
        case 5: i += (tok >> 24) & 0x1F; break;
        default: break;
        }
    }
}

static uint32_t token_hash(const uint32_t* t, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        for (int b = 0; b < 32; b += 8)
            h = (h ^ ((t[i] >> b) & 255)) * 16777619u;
    return h ? h : 1;
}

/* CreateVertexShader(pDeclaration, pFunction, pHandle, Usage). Handles are odd: D3D8 tells a shader
 * from an FVF code (bit 0 always clear) by that bit. */
static void IDirect3DDevice8_CreateVertexShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nvs && d->vs[i].decl)
        i++;
    if (i == d->nvs)
    {
        d->vs = (Shader*)realloc(d->vs, (d->nvs + 1) * sizeof *d->vs);
        memset(&d->vs[d->nvs++], 0, sizeof(Shader));
    }
    Shader* s = &d->vs[i];
    s->decl = copy_tokens(ARG(1), 0xFFFFFFFFu, &s->ndecl);
    s->func = ARG(2) ? copy_tokens(ARG(2), 0x0000FFFFu, &s->nfunc) : NULL;
    if (!ARG(2))
        s->nfunc = 0;
    decl_layout(s->decl, s->ndecl, &s->lay);
    s->hash = s->func ? token_hash(s->func, s->nfunc) : 0;
    wr32(ARG(3), ((i + 1) << 1) | 1);
    RET(D3D_OK, 5);
}

static Shader* vs_of(uint32_t h)
{
    uint32_t i = (h >> 1) - 1;
    return (h & 1) && i < g_dev.nvs && g_dev.vs[i].decl ? &g_dev.vs[i] : NULL;
}

static void IDirect3DDevice8_SetVertexShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->vs = ARG(1);
    if (m)
        m->vs = 1;
    /* D3DVSD_CONST in the declaration: constants loaded with the shader */
    Shader* sh = vs_of(ARG(1));
    for (uint32_t i = 0; sh && i < sh->ndecl && sh->decl[i] != 0xFFFFFFFFu; ++i)
    {
        uint32_t tok = sh->decl[i];
        if (tok >> 29 == 4)
        {
            uint32_t count = (tok >> 25) & 0xF, reg = tok & 0x7F;
            for (uint32_t k = 0; k < count && reg + k < NVSC && i + 1 + 4 * k + 3 < sh->ndecl; ++k)
            {
                memcpy(s->vsc[reg + k], &sh->decl[i + 1 + 4 * k], 16);
                raise_top(&g_vsc_top, reg + k + 1);
                if (m)
                    m->vsc[reg + k] = 1;
            }
            i += 4 * count;
        }
        else if (tok >> 29 == 5)
            i += (tok >> 24) & 0x1F;
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetVertexShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.vs);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteVertexShader(Guest* g)
{
    Shader* s = vs_of(ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    free(s->decl);
    free(s->func);
    memset(s, 0, sizeof *s);
    if (g_dev.cur.vs == ARG(1))
        g_dev.cur.vs = 0;
    RET(D3D_OK, 2);
}

/* Set/GetVertexShaderConstant(Register, pConstantData, ConstantCount) */
static void IDirect3DDevice8_SetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->vsc[r], ARGP(2), 16 * n);
    raise_top(&g_vsc_top, r + n);
    if (m)
        memset(&m->vsc[r], 1, n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    memcpy(ARGP(2), g_dev.cur.vsc[r], 16 * n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    bind(&s->stream[ARG(1)], ARG(2));
    s->stride[ARG(1)] = ARG(3);
    if (m)
        m->stream[ARG(1)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = g_dev.cur.stream[ARG(1)];
    if (p)
        obj_addref(p);
    wr32(ARG(2), p);
    wr32(ARG(3), g_dev.cur.stride[ARG(1)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetIndices(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    bind(&s->ib, ARG(1));
    s->base_vertex = ARG(2);
    if (m)
        m->ib = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetIndices(Guest* g)
{
    uint32_t p = g_dev.cur.ib;
    if (p)
        obj_addref(p);
    wr32(ARG(1), p);
    wr32(ARG(2), g_dev.cur.base_vertex);
    RET(D3D_OK, 3);
}

/* CreatePixelShader(pFunction, pHandle) */
static void IDirect3DDevice8_CreatePixelShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nps && d->ps[i].func)
        i++;
    if (i == d->nps)
    {
        d->ps = (Shader*)realloc(d->ps, (d->nps + 1) * sizeof *d->ps);
        memset(&d->ps[d->nps++], 0, sizeof(Shader));
    }
    d->ps[i].func = copy_tokens(ARG(1), 0x0000FFFFu, &d->ps[i].nfunc);
    d->ps[i].hash = token_hash(d->ps[i].func, d->ps[i].nfunc);
    wr32(ARG(2), i + 1);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetPixelShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->ps = ARG(1);
    if (m)
        m->ps = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetPixelShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.ps);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeletePixelShader(Guest* g)
{
    uint32_t i = ARG(1) - 1;
    if (!ARG(1) || i >= g_dev.nps || !g_dev.ps[i].func)
        RET(D3DERR_INVALIDCALL, 2);
    free(g_dev.ps[i].func);
    memset(&g_dev.ps[i], 0, sizeof(Shader));
    if (g_dev.cur.ps == ARG(1))
        g_dev.cur.ps = 0;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetPixelShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NPSC || n > NPSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->psc[r], ARGP(2), 16 * n);
    if (m)
        memset(&m->psc[r], 1, n);
    RET(D3D_OK, 4);
}

/* --- drawing: the device state as the back end's draw packet ------------------------------------------------ */
/* D3D's v * M with row-major matrices: r = a * b */
static void mat_mul(float* r, const float* a, const float* b)
{
    float t[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            t[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    memcpy(r, t, sizeof t);
}

/* the normal matrix: the inverse transpose of m's upper 3x3 (D3D lights in camera space) */
static void normal_matrix(float* r, const float* m)
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float s = det != 0.0f ? 1.0f / det : 0.0f;
    memset(r, 0, 64);
    /* inverse transpose = cofactor matrix / det */
    r[0] = A * s, r[1] = B * s, r[2] = C * s;
    r[4] = -(b * i - c * h) * s, r[5] = (a * i - c * g) * s, r[6] = -(a * h - b * g) * s;
    r[8] = (b * f - c * e) * s, r[9] = -(a * f - c * d) * s, r[10] = (a * e - b * d) * s;
    r[15] = 1.0f;
}

static void xform(float* out, const float* v, float w, const float* m) /* (v, w) * m, xyz */
{
    for (int k = 0; k < 3; ++k)
        out[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + w * m[12 + k];
}

static void color4(float* out, uint32_t c)
{
    out[0] = ((c >> 16) & 255) / 255.0f, out[1] = ((c >> 8) & 255) / 255.0f, out[2] = (c & 255) / 255.0f, out[3] = (c >> 24) / 255.0f;
}

/* vertices a D3D primitive count spans */
static uint32_t prim_vertices(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5:
    case 6: return n + 2;
    default: return 0;
    }
}

static void apply_targets(void)
{
    uint32_t cf = 0, cl = 0, df, dl;
    Obj* rt = obj(g_dev.rt);
    Obj* ds = obj(g_dev.ds);
    GfxTex* c = rt ? surface_gpu(rt, &cf, &cl) : NULL;
    GfxTex* z = ds ? surface_gpu(ds, &df, &dl) : NULL;
    gfx_set_targets(c, cf, cl, z);
}

static GfxDraw g_draw; /* one device, one draw at a time */

/* The keys and uniforms for the current state; 0 when the state cannot be drawn. */
static int build_draw(GfxDraw* d)
{
    State* s = &g_dev.cur;
    const uint32_t* rs = s->rs;
    /* the keys and bindings, and the uniforms up to the lights; lights and shader constants are
     * written where they are used (the back end uploads only those) */
    memset(d, 0, offsetof(GfxDraw, u));
    memset(&d->u, 0, offsetof(GfxU, light));
    memset(d->tex, 0, sizeof(GfxDraw) - offsetof(GfxDraw, tex));

    Layout fvf;
    const Layout* lay;
    Shader* vs = NULL;
    if (s->vs & 1)
    {
        vs = vs_of(s->vs);
        if (!vs)
            return gfx_prof_skip(GFX_SKIP_NO_SHADER), 0;
        lay = &vs->lay;
    }
    else
    {
        fvf_layout(s->vs, &fvf);
        lay = &fvf;
    }
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        if (lay->el[r].used && lay->el[r].stream >= GFX_NSTREAMS)
            return gfx_prof_skip(GFX_SKIP_STREAM), 0;
        d->vs.el[r] = lay->el[r];
        d->u.offset[r] = lay->offset[r];
    }
    if (!lay->el[GFX_R_POSITION].used && !(vs && vs->func))
        return gfx_prof_skip(GFX_SKIP_NO_POSITION), 0;
    d->vs.rhw = lay->rhw;
    if (vs && vs->func)
    {
        d->vs.prog = vs->hash;
        d->vs_tokens = vs->func;
        memcpy(d->u.vsc, s->vsc, sizeof d->u.vsc);
    }
    if (s->ps)
    {
        uint32_t i = s->ps - 1;
        if (i < g_dev.nps && g_dev.ps[i].func)
        {
            d->fs.prog = g_dev.ps[i].hash;
            d->ps_tokens = g_dev.ps[i].func;
            memcpy(d->u.psc, s->psc, sizeof d->u.psc);
        }
    }

    /* transforms: WORLD is D3DTS_WORLDMATRIX(0), index 24 here */
    float wv[16];
    mat_mul(wv, s->xf[24], s->xf[2]);
    mat_mul(d->u.wvp, wv, s->xf[3]);
    memcpy(d->u.wv, wv, 64);
    normal_matrix(d->u.wvit, wv);
    for (int i = 0; i < 8; ++i)
        memcpy(d->u.texm[i], s->xf[16 + i], 64);

    /* D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive, Power */
    memcpy(d->u.mat_d, &s->mat[0], 16);
    memcpy(d->u.mat_a, &s->mat[4], 16);
    memcpy(d->u.mat_s, &s->mat[8], 16);
    memcpy(d->u.mat_e, &s->mat[12], 16);
    d->u.params[0] = u2f(s->mat[16]);
    d->u.params[1] = (float)(rs[24] & 255); /* ALPHAREF */
    d->u.params[2] = u2f(rs[36]);           /* FOGSTART */
    d->u.params[3] = u2f(rs[37]);           /* FOGEND */
    d->u.params2[0] = u2f(rs[38]);          /* FOGDENSITY */
    color4(d->u.ambient, rs[139]);
    color4(d->u.tfactor, rs[60]);
    color4(d->u.fogcolor, rs[34]);
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    scale_to_target(d->vp, NULL, 0); /* the XYZRHW mapping (u.vp) stays in the game's pixels */

    int ff_vertex = !d->vs.prog && !lay->rhw;
    if (ff_vertex && rs[137]) /* LIGHTING */
    {
        d->vs.lighting = 1;
        d->vs.normalize = rs[143] != 0;
        d->vs.localviewer = rs[142] != 0;
        d->vs.specular = rs[29] != 0;
        if (rs[141]) /* COLORVERTEX: the material sources */
        {
            d->vs.src_diffuse = (uint8_t)rs[145], d->vs.src_specular = (uint8_t)rs[146];
            d->vs.src_ambient = (uint8_t)rs[147], d->vs.src_emissive = (uint8_t)rs[148];
        }
        const float* view = s->xf[2];
        int n = 0;
        for (int i = 0; i < g_light_top && n < GFX_NLIGHTS; ++i)
        {
            const Light* l = &s->light[i];
            uint32_t type = l->v[0];
            if (!l->enabled || type < 1 || type > 3)
                continue;
            /* D3DLIGHT8: Type, Diffuse, Specular, Ambient, Position, Direction, Range, Falloff,
             * Attenuation0..2, Theta, Phi */
            float v[26];
            memcpy(v, l->v, sizeof v);
            GfxLight* gl = &d->u.light[n];
            memcpy(gl->diffuse, &v[1], 16);
            memcpy(gl->specular, &v[5], 16);
            memcpy(gl->ambient, &v[9], 16);
            xform(gl->pos, &v[13], 1.0f, view);
            gl->pos[3] = v[19];
            float dir[3];
            xform(dir, &v[16], 0.0f, view);
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            float k = len > 0 ? (type == 3 ? -1.0f : 1.0f) / len : 0.0f; /* directional: toward the light */
            gl->dir[0] = dir[0] * k, gl->dir[1] = dir[1] * k, gl->dir[2] = dir[2] * k, gl->dir[3] = v[20];
            gl->att[0] = v[21], gl->att[1] = v[22], gl->att[2] = v[23];
            gl->spot[0] = cosf(v[24] * 0.5f), gl->spot[1] = cosf(v[25] * 0.5f);
            d->vs.light_type[n++] = (uint8_t)type;
        }
        d->vs.nlights = (uint8_t)n;
    }

    if (rs[28]) /* FOGENABLE: table (pixel) fog, else vertex fog, else the vertices' specular alpha */
    {
        if (rs[35])
            d->fs.fog = (uint8_t)rs[35];
        else
        {
            d->fs.fog = 4;
            if (ff_vertex)
                d->vs.fog_vertex = (uint8_t)rs[140];
        }
        d->vs.range_fog = ff_vertex && rs[48];
    }

    /* the texture stages, up to the first disabled one */
    int nst = 0;
    if (!d->fs.prog)
        while (nst < 8 && s->tss[nst][1] != 1)
            nst++;
    d->fs.nstages = (uint8_t)nst;
    int nused = d->fs.prog ? 4 : nst, repl0 = 0;
    d->vs.ntex = (uint8_t)(d->vs.prog || d->fs.prog ? 8 : nst);
    for (int i = 0; i < nused; ++i)
    {
        const uint32_t* t = s->tss[i];
        GfxStage* st = &d->fs.st[i];
        int kind = 0;
        GfxTex* tex = texture_for_draw(s->tex[i], &kind);
        if (tex)
        {
            d->tex[i] = tex;
            st->tex = (uint8_t)kind;
            Obj* to = obj(s->tex[i]);
            uint32_t maxlevel = t[20] > to->lod ? t[20] : to->lod;
            d->samp[i] = (GfxSampler){ (uint8_t)t[13], (uint8_t)t[14], (uint8_t)t[25], (uint8_t)t[16], (uint8_t)t[17], (uint8_t)t[18],
                (uint8_t)t[21], (uint8_t)maxlevel, 0, { 0 }, t[15] };
            if (to->repl && tex == to->repl) /* drawn smaller than it is: linear, between its mipmaps */
            {
                d->samp[i].mag = d->samp[i].min = d->samp[i].mip = 2, d->samp[i].max_level = 0;
                d->samp[i].lod_cap = 2; /* its half-size mip at most: smaller ones blend neighbouring glyphs in */
                if (i == 0)
                    repl0 = 1;
            }
        }
        if (!d->fs.prog)
        {
            st->cop = (uint8_t)t[1], st->ca1 = (uint8_t)t[2], st->ca2 = (uint8_t)t[3], st->ca0 = (uint8_t)t[26];
            st->aop = (uint8_t)t[4], st->aa1 = (uint8_t)t[5], st->aa2 = (uint8_t)t[6], st->aa0 = (uint8_t)t[27];
            st->result = (uint8_t)t[28];
        }
        uint32_t ttf = t[24], count = ttf & 0xFF;
        st->ncoord = (uint8_t)(count ? count : 2);
        st->projected = (ttf & 256) && count > 1;
        if (!d->vs.prog)
        {
            d->vs.tci[i] = (uint8_t)((t[11] & 7) | ((t[11] >> 16) & 0xF) << 4);
            d->vs.ttf[i] = (uint8_t)(count | ((ttf & 256) ? 0x80 : 0));
        }
    }
    d->fs.alpha_func = (uint8_t)(rs[15] ? rs[25] : 0);
    d->fs.specular_add = !d->fs.prog && rs[29];
    d->vs.flat = d->fs.flat = rs[9] == 1; /* SHADEMODE flat */

    d->pipe.blend = rs[27] != 0;
    if (d->pipe.blend)
        d->pipe.src = (uint8_t)rs[19], d->pipe.dst = (uint8_t)rs[20], d->pipe.op = (uint8_t)rs[171];
    /* FFXI draws its text adding (ONE, ONE): its font's outline can only brighten what is behind it.
     * A replacement drawn that way has its colour premultiplied by alpha (make_texpack --additive), so
     * over (ONE, INVSRCALPHA) draws the same fill and lets the outline darken, as the game's other
     * lettering does. */
    if (repl0 && d->pipe.blend && d->pipe.src == 2 && d->pipe.dst == 2 && d->pipe.op == 1)
        d->pipe.dst = 6;
    d->pipe.write_mask = (uint8_t)(rs[168] & 0xF);
    if (g_dev.ds)
    {
        d->depth.zenable = rs[7] != 0, d->depth.zwrite = rs[14] != 0, d->depth.zfunc = (uint8_t)rs[23];
        if (rs[52]) /* STENCILENABLE */
        {
            d->depth.stencil = 1;
            d->depth.sfail = (uint8_t)rs[53], d->depth.szfail = (uint8_t)rs[54], d->depth.spass = (uint8_t)rs[55];
            d->depth.sfunc = (uint8_t)rs[56], d->depth.sread = (uint8_t)rs[58], d->depth.swrite = (uint8_t)rs[59];
            d->stencil_ref = rs[57];
        }
    }
    d->cull = (uint8_t)rs[22];
    d->fill = (uint8_t)rs[8];
    d->zbias = (int32_t)rs[47];
    return 1;
}

/* Points the draw at vertices first .. first + count of the streams its layout reads: the bound
 * vertex buffers, or the ...UP draw's memory as stream 0. */
static int set_streams(GfxDraw* d, uint32_t first, uint32_t count, uint32_t up_data, uint32_t up_stride)
{
    int used[GFX_NSTREAMS] = { 0 };
    for (int r = 0; r < GFX_NREGS; ++r)
        if (d->vs.el[r].used)
            used[d->vs.el[r].stream] = 1;
    for (int st = 0; st < GFX_NSTREAMS; ++st)
    {
        if (!used[st])
            continue;
        uint32_t stride, base, size;
        if (up_data)
        {
            if (st)
                return gfx_prof_skip(GFX_SKIP_STREAM), 0;
            stride = up_stride, base = up_data, size = 0xFFFFFFFFu;
        }
        else
        {
            Obj* b = obj(g_dev.cur.stream[st]);
            if (!b || !b->mem)
                return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
            stride = g_dev.cur.stride[st], base = b->mem, size = b->size;
        }
        uint32_t start = first * stride, bytes = stride ? count * stride : 64;
        if (start >= size)
            return gfx_prof_skip(GFX_SKIP_RANGE), 0;
        if (bytes > size - start)
            bytes = size - start;
        GfxBuf* gb = up_data ? NULL : static_buffer(obj(g_dev.cur.stream[st]));
        if (gb && !(start & 3))
        {
            d->buf[st] = gb, d->buf_off[st] = start;
            d->u.stride[st] = (int32_t)stride;
            continue;
        }
        d->data[st] = GUEST_PTR(base + start);
        d->size[st] = bytes;
        d->u.stride[st] = (int32_t)stride;
    }
    return 1;
}

static void index_range(uint32_t indices, uint32_t size, uint32_t n, uint32_t* lo, uint32_t* hi)
{
    uint32_t a = 0xFFFFFFFFu, b = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t v = size == 4 ? rd32(indices + 4 * i) : rd16(indices + 2 * i);
        a = v < a ? v : a;
        b = v > b ? v : b;
    }
    *lo = a, *hi = b;
}

/* --ui-aspect (user32_set_ui_aspect): FFXI lays its interface out in a fixed space and stretches
 * it to the back buffer, so a window wider than that space stretches the interface. Its menus,
 * text and cursor are pre-transformed (XYZRHW) draws to the back buffer; each is squeezed toward
 * the middle by mapping the viewport's width onto a centered box of the aspect asked for (the
 * XYZRHW mapping reads d->u.vp). Draws that span the whole width - the 3D scene put on the screen,
 * fades, letterboxing - are left full. With a menu resolution other than the window's, FFXI draws
 * the interface into a render-target texture of that size (the menu target) and puts it on the
 * screen as one full-width quad: the draws into it are squeezed the same way, so its full-width ones
 * (the dimming behind a menu, the lobby's copy of the 3D scene) still cover the screen. The mouse is
 * mapped back in user32.c. */
/* Bars across the whole width (the lobby's help bar): a full-width draw no taller than this share of
 * the height is a bar's body, and the frame drawn over it after (edge lines inset by a corner's
 * width, corner pieces at the ends) goes out to the screen's edges with it. Reset at Present. */
#define UI_BAR_MAX_H 0.15f
#define UI_MAX_BARS 8
static struct
{
    uint32_t rt;
    float y0, y1;
} g_ui_bars[UI_MAX_BARS];
static int g_ui_nbars;

/* Where the interface was drawn, on a coarse grid over the screen: this frame's, and the last
 * finished one's, which user32 asks (user32_ui_hit) to tell the interface from the world under the
 * mouse. g_ui_composited: the menu target is on the screen this frame, so what goes over it after
 * is the game's cursor. */
#define UI_GW 128
#define UI_GH 64
static uint8_t g_ui_grid[2][UI_GH][UI_GW];
static int g_ui_cur, g_ui_composited;

static int ui_hit(float fx, float fy)
{
    int x = (int)(fx * UI_GW), y = (int)(fy * UI_GH);
    return x >= 0 && x < UI_GW && y >= 0 && y < UI_GH && g_ui_grid[g_ui_cur ^ 1][y][x];
}

/* a box of the screen (0..1 across and down) the interface covers */
static void ui_mark(float x0, float x1, float y0, float y1)
{
    int a = (int)(x0 * UI_GW), b = (int)(x1 * UI_GW), c = (int)(y0 * UI_GH), e = (int)(y1 * UI_GH);
    a = a < 0 ? 0 : a, c = c < 0 ? 0 : c, b = b >= UI_GW ? UI_GW - 1 : b, e = e >= UI_GH ? UI_GH - 1 : e;
    for (int y = c; y <= e; ++y)
        for (int x = a; x <= b; ++x)
            g_ui_grid[g_ui_cur][y][x] = 1;
}

static void ui_present(void)
{
    g_ui_nbars = 0;
    g_ui_composited = 0;
    g_ui_cur ^= 1;
    memset(g_ui_grid[g_ui_cur], 0, sizeof g_ui_grid[0]);
    user32_ui_hit = ui_hit;
}

/* The draw's x (axis 0) or y (axis 1) mapped to a + b x (the game's pixels, across the whole
 * target): its viewport - the clip rectangle the game gave it, in the target's pixels - moves and
 * narrows to match, while the XYZRHW mapping (u.vp, the viewport in the game's pixels) stays, so the
 * draw keeps its clip */
static void ui_target_size(float* w, float* h);

static void ui_map_axis(GfxDraw* d, int i, float a, float b)
{
    float x0 = d->u.vp[i], w = d->u.vp[i + 2];
    if (!(w > 0))
        return;
    float k = (float)d->vp[i + 2] / w; /* the target's pixels per game pixel */
    float l = (a + b * x0) * k, r = (a + b * (x0 + w)) * k;
    /* kept within the target: the back ends clip a viewport to it, which would shrink the mapping
     * (a bar's edge line, widened past the screen's edge, stopped short of its corner) */
    float tw, th;
    ui_target_size(&tw, &th);
    float edge = i == 0 ? tw : th;
    l = l < 0 ? 0 : l;
    r = r > edge * k ? edge * k : r;
    d->vp[i] = (uint32_t)(l + 0.5f);
    d->vp[i + 2] = r > l ? (uint32_t)(r - l + 0.5f) : 0;
    /* what rounding the clip took off, given back to the mapping so the draw lands where asked */
    d->u.vp[i] = x0 + ((float)d->vp[i] / k - (a + b * x0)) / b;
    d->u.vp[i + 2] = (float)d->vp[i + 2] / k / b;
}

static void ui_map(GfxDraw* d, float a, float b) { ui_map_axis(d, 0, a, b); }

/* The game's size of the target the draw goes to: the back buffer's, or the menu target's */
static void ui_target_size(float* w, float* h)
{
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    *w = t ? (float)t->width : (float)g_dev.pp[0];
    *h = t ? (float)t->height : (float)g_dev.pp[1];
}

static void ui_squeeze(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, int mark)
{
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if (s >= 1.0f && sy >= 1.0f)
        return;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            break;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
        ylo = y < ylo ? y : ylo;
        yhi = y > yhi ? y : yhi;
    }
    if (lo > hi)
        return;
    /* everything is squeezed toward the target's middle, whatever viewport the draw has (the chat
     * log's text and the menus' titles are clipped to their windows); "the whole width" with some
     * slack: the rules screen's backdrop stops 1.5 pixels short */
    float W, h;
    ui_target_size(&W, &h);
    float slack = 1.0f + W / 256.0f;
    if (sy < 1.0f)
    {
        /* a window taller than the interface's shape: squeezed toward the middle up and down. What
         * covers the whole target stays (the 3D scene put on the screen, fades, the dimming behind a
         * menu); a full-width band at the top or bottom (a cutscene's bars, the lobby's help bar)
         * keeps its outer edge on the screen's */
        float yslack = 1.0f + h / 256.0f, a = h * 0.5f * (1.0f - sy), b = sy;
        int wide = lo <= slack && hi >= W - slack, top = ylo <= yslack, bottom = yhi >= h - yslack;
        if (wide && top && bottom)
            return;
        if (wide && (top || bottom) && yhi > ylo)
        {
            float t0 = top ? 0.0f : a + sy * ylo, t1 = bottom ? h : a + sy * yhi;
            b = (t1 - t0) / (yhi - ylo), a = t0 - b * ylo;
        }
        ui_map_axis(d, 1, a, b);
        if (mark)
            ui_mark(lo / W, hi / W, (a + b * ylo) / h, (a + b * yhi) / h);
        return;
    }
    if (lo <= slack && hi >= W - slack)
    {
        if (yhi - ylo <= h * UI_BAR_MAX_H && g_ui_nbars < UI_MAX_BARS)
            g_ui_bars[g_ui_nbars].rt = g_dev.rt, g_ui_bars[g_ui_nbars].y0 = ylo, g_ui_bars[g_ui_nbars].y1 = yhi,
            g_ui_nbars++;
        return;
    }
    /* a piece of a bar's frame: within a bar's band and at an edge (a corner's width, 64 pixels) */
    float edge = 64.0f * W / 1280.0f, a = W * 0.5f * (1.0f - s), b = s;
    for (int i = 0; i < g_ui_nbars; ++i)
    {
        if (g_ui_bars[i].rt != g_dev.rt || ylo < g_ui_bars[i].y0 - 2.0f || yhi > g_ui_bars[i].y1 + 2.0f)
            continue;
        int left = lo <= edge, right = hi >= W - edge;
        if (left && right) /* an edge line: its ends keep their insets, squeezed */
        {
            float l = lo * s, r = W - (W - hi) * s;
            b = (r - l) / (hi - lo), a = l - b * lo;
            break;
        }
        if (left && hi < W * 0.5f) /* the left corner, squeezed toward the left edge */
        {
            a = 0.0f;
            break;
        }
        if (right && lo > W * 0.5f) /* the right corner */
        {
            a = W * (1.0f - s);
            break;
        }
    }
    ui_map(d, a, b);
    if (mark)
        ui_mark((a + b * lo) / W, (a + b * hi) / W, ylo / h, yhi / h);
}

/* FFXI_DRAWLOG=<path>: while <path>.go exists, the next frame's draws are written to <path> (then
 * .go is removed): the game's return addresses on the guest stack, the texture, and the vertices'
 * box. For finding which of the game's code draws what. */
static void cap_present(void)
{
    static int init;
    if (!init)
    {
        init = 1, g_cap_path = getenv("FFXI_DRAWLOG");
        /* without it, ~/Library/Caches/FFXI/draws.txt (still only while draws.txt.go exists) */
        static char def[1024];
        if ((!g_cap_path || !*g_cap_path) && getenv("HOME"))
            snprintf(def, sizeof def, "%s/Library/Caches/FFXI/draws.txt", getenv("HOME")), g_cap_path = def;
    }
    if (!g_cap_path)
        return;
    ++g_cap_frame;
    if (g_cap)
    {
        fclose(g_cap), g_cap = NULL;
        char go[1024];
        snprintf(go, sizeof go, "%s.go", g_cap_path);
        remove(go);
        return;
    }
    if (g_cap_frame % 30)
        return;
    char go[1024];
    snprintf(go, sizeof go, "%s.go", g_cap_path);
    FILE* f = fopen(go, "rb");
    if (!f)
        return;
    fclose(f);
    g_cap = fopen(g_cap_path, "w");
    g_cap_n = 0;
    g_cap_nat = 0;
    char at[1024];
    snprintf(at, sizeof at, "%s.at", g_cap_path);
    FILE* af = fopen(at, "r");
    while (af && g_cap_nat < 32 && fscanf(af, "%u", &g_cap_at[g_cap_nat]) == 1)
        g_cap_nat++;
    if (af)
        fclose(af);
    uint32_t w, h;
    d3d8_screen_size(&w, &h);
    if (g_cap)
        fprintf(g_cap, "frame %u screen %ux%u backbuffer %ux%u image %08x-%08x\n", g_cap_frame, w, h, g_dev.pp[0],
            g_dev.pp[1], rt_image_lo, rt_image_hi);
}

/* the current target as it is after draw n, if <path>.at asks for it: <path>.<n>.bin, a header
 * of width and height, then its pixels as the GPU holds them (BGRA) */
static void cap_dump_target(uint32_t n)
{
    uint32_t k = 0;
    while (k < g_cap_nat && g_cap_at[k] != n)
        k++;
    Obj* rt = obj(g_dev.rt);
    Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    if (k == g_cap_nat || !t || !t->gpu || t->format != 21)
        return;
    uint32_t w = t->pw ? t->pw : t->width, h = t->pw ? t->ph : t->height;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    gfx_tex_read(t->gpu, 0, 0, px, w * 4);
    char path[1100];
    snprintf(path, sizeof path, "%s.%u.bin", g_cap_path, n);
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fwrite(&w, 4, 1, f), fwrite(&h, 4, 1, f), fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
    }
    free(px);
}

static void cap_draw(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t n, uint32_t up_data,
    uint32_t up_stride)
{
    FILE* f = g_cap;
    Obj* crt = obj(g_dev.rt);
    fprintf(f, "#%u prim %u count %u n %u vs %x rhw %d rt %s %ux%u %08x vp %u,%u %ux%u", g_cap_n++, prim, count, n, g_dev.cur.vs,
        d->vs.rhw, g_dev.rt == g_dev.backbuffer ? "bb" : "off", crt ? crt->width : 0, crt ? crt->height : 0, g_dev.rt, d->vp[0],
        d->vp[1], d->vp[2], d->vp[3]);
    const uint32_t* crs = g_dev.cur.rs;
    const uint32_t* ct = g_dev.cur.tss[0];
    fprintf(f, " atest %u func %u ref %u blend %u %u/%u op %u tss0 c%u(%x,%x) a%u(%x,%x) tss1 c%u", crs[15], crs[25], crs[24],
        crs[27], crs[19], crs[20], crs[171], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], g_dev.cur.tss[1][1]);
    Obj* t = obj(g_dev.cur.tex[0]);
    if (t)
        fprintf(f, " tex %08x %ux%u fmt %u filt %u/%u", g_dev.cur.tex[0], t->width, t->height, t->format, ct[16], ct[17]);
    fprintf(f, " up %u inset %d", up_data ? 1u : 0u, g_cap_inset);
    g_cap_inset = 0;
    fprintf(f, " z %u/%u func %u fog %u/%u lit %u caster %u", d->depth.zenable, d->depth.zwrite, d->depth.zfunc, d->fs.fog, d->vs.fog_vertex,
        d->vs.lighting, d->caster);
    if (!d->vs.rhw)
    {
        const float* P = g_dev.cur.xf[3];
        const float* V = g_dev.cur.xf[2];
        const float* W = g_dev.cur.xf[24];
        fprintf(f, "\n  proj %.4f %.4f %.4f %.4f view t %.2f %.2f %.2f world t %.2f %.2f %.2f", P[0], P[5], P[8], P[9],
            V[12], V[13], V[14], W[12], W[13], W[14]);
    }
    fprintf(f, "\n  stack");
    for (uint32_t a = g_cap_esp, k = 0; a < g_cap_esp + 0x600 && k < 12; a += 4)
    {
        uint32_t v = rd32(a);
        if (v < rt_image_lo + 6 || v >= rt_image_hi)
            continue;
        if (rd8(v - 5) == 0xE8 || rd8(v - 6) == 0xFF || rd8(v - 3) == 0xFF || rd8(v - 2) == 0xFF)
            fprintf(f, " %08x", v), ++k;
    }
    fprintf(f, "\n");
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 12)
            break;
        for (int c = 0; c < 3; ++c)
        {
            float x = u2f(rd32(base + at + 4 * c));
            lo[c] = x < lo[c] ? x : lo[c];
            hi[c] = x > hi[c] ? x : hi[c];
        }
        if (i < 4 && g_dev.cur.vs == 0x144 && stride == 28) /* XYZRHW, diffuse, one texture: with its UV */
            fprintf(f, "  v%u %.2f %.2f %.4f uv %.5f %.5f c %08x\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)), u2f(rd32(base + at + 20)), u2f(rd32(base + at + 24)), rd32(base + at + 16));
        else if (i < 4)
            fprintf(f, "  v%u %.2f %.2f %.4f\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)));
    }
    fprintf(f, "  box %.2f..%.2f  %.2f..%.2f  %.4f..%.4f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
}

/* --- the scene's end, for the back end's scene effects (gfx_scene_done) ------------------------------
 * The frame's 3D scene is the target that takes depth-tested draws of untransformed vertices and is
 * at least half the back buffer (not the 16x16 probes or smaller effect targets). It is finished
 * when a draw samples it (FFXI draws its background target onto the back buffer as a quad), when
 * transformed vertices go over it on the back buffer (the interface), or at Present. The camera
 * comes from its fixed-function draws: the zone's own shader draws carry theirs in constants. */
static struct
{
    GfxTex* rt;     /* the scene's target */
    /* the world's view: the target that took the most 3D draws the frame before. FFXI draws the world
     * into one 4096 target and copies it into another, where nameplates and effects go on; other
     * targets' scenes get no effects, cast no shadows and give no camera. */
    GfxTex* world;
    int world_done; /* the effects ran on it this frame: once a frame */
    struct { GfxTex* t; uint32_t n; } tally[4]; /* this frame's 3D draws per target */
    uint32_t draws; /* 3D draws to it since the effects last ran */
    int done, cam;  /* the effects ran this frame; s holds a camera */
    /* where this frame's camera and sun came from: the draw's number, and for the camera 2 when that
     * draw was fogged (the world), 1 when not; 0 before one is found */
    uint32_t cam_draw, sun_draw, cam_rank;
    GfxScene s;
    /* the profile (FFXI_PROFILE): scenes by how they ended and where the camera came from, 3D draws
     * after the effects ran, and screen-space or undepth-tested draws onto the scene before them
     * (what the effects would shade) */
    uint32_t st_frames, st_why[4], st_cam[3], st_late, st_over, st_over_rhw;
    /* the trace (scene_trace): what each of the last frames did with the world's scene */
    uint32_t frame, tr_late, tr_draws, tr_targets;
    char tr_why;
    struct { uint32_t frame, draws, late, targets; char why, ran, changed, cam; } trace[1200];
} g_scene;

/* The sun, by vote. FFXI lights each object with lights of its own, some tens of degrees apart, so
 * the first lit draw's light (the old way) turned the shadows as the camera moved. Each lit draw of
 * the world votes for each of its directional lights, in the world: its first light counts 1, the
 * rest 0.5; lights within about 2 degrees are one light; scores fade by 0.99 a frame. Another light
 * becomes the sun only when it has scored 1.5 times the sun's for four frames running. The winner's
 * direction is its latest, so the sun still moves through the day. */
static struct
{
    struct { float dir[3], color[4], score; } c[8];
    int cur, chal, chal_frames; /* the sun, the light overtaking it and for how many frames; -1 none */
} g_sunv = { .cur = -1, .chal = -1 };

static void sun_vote(const float* w, const float* color, float weight)
{
    int at = -1, low = -1;
    for (int i = 0; i < 8; ++i)
    {
        const float* d = g_sunv.c[i].dir;
        if (g_sunv.c[i].score > 0.0f && d[0] * w[0] + d[1] * w[1] + d[2] * w[2] > 0.9995f)
        {
            at = i;
            break;
        }
        if (i != g_sunv.cur && (low < 0 || g_sunv.c[i].score < g_sunv.c[low].score))
            low = i;
    }
    if (at < 0)
    {
        if (low < 0)
            return;
        at = low, g_sunv.c[at].score = 0.0f;
        if (g_sunv.chal == at)
            g_sunv.chal = -1;
    }
    memcpy(g_sunv.c[at].dir, w, 12);
    memcpy(g_sunv.c[at].color, color, 16);
    g_sunv.c[at].score += weight;
}

/* the frame's sun from the votes so far */
static int sun_pick(void)
{
    int top = -1;
    for (int i = 0; i < 8; ++i)
        if (g_sunv.c[i].score > 0.0f && (top < 0 || g_sunv.c[i].score > g_sunv.c[top].score))
            top = i;
    if (top < 0)
        return g_sunv.cur;
    if (g_sunv.cur < 0 || g_sunv.c[g_sunv.cur].score <= 0.0f)
        g_sunv.cur = top, g_sunv.chal = -1;
    else if (top != g_sunv.cur && g_sunv.c[top].score > 1.5f * g_sunv.c[g_sunv.cur].score)
    {
        g_sunv.chal_frames = g_sunv.chal == top ? g_sunv.chal_frames + 1 : 1;
        g_sunv.chal = top;
        if (g_sunv.chal_frames >= 4)
            g_sunv.cur = top, g_sunv.chal = -1;
    }
    else
        g_sunv.chal = -1;
    return g_sunv.cur;
}

/* a texture going away: the scene no longer points at it */
static void scene_forget(GfxTex* t)
{
    if (!t)
        return;
    if (g_scene.rt == t)
        g_scene.rt = NULL, g_scene.draws = 0, g_scene.done = 0;
    if (g_scene.world == t)
        g_scene.world = NULL;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == t)
            g_scene.tally[i].t = NULL, g_scene.tally[i].n = 0;
}

static void scene_finish(const char* why)
{
    if (g_cap && g_scene.draws && !g_scene.done)
    {
        const float* p = g_scene.s.proj;
        const uint32_t* vp = g_scene.s.vp;
        const float* f = g_scene.s.fogcolor;
        fprintf(g_cap, "scene done (%s) after %u 3D draws, camera %d from draw %u (fogged %d), sun %d from draw %u, "
            "projection %g %g %g %g / %g %g, viewport %u %u %u %u z %g..%g, fog %.2f %.2f %.2f\n",
            why, g_scene.draws, g_scene.cam, g_scene.cam_draw, g_scene.cam_rank == 2, g_scene.s.sun_dir[3] != 0.0f,
            g_scene.sun_draw, p[0], p[5], p[10], p[11], p[14], p[15], vp[0], vp[1], vp[2], vp[3], u2f(vp[4]), u2f(vp[5]),
            f[0], f[1], f[2]);
    }
    if (g_scene.draws && !g_scene.done)
    {
        g_scene.st_why[!strcmp(why, "sampled") || why[0] == 'e' ? 0 : why[0] == 'i' ? 1 : why[0] == 'p' ? 2 : 3]++;
        g_scene.st_cam[g_scene.cam ? g_scene.cam_rank : 0]++;
    }
    if (g_scene.draws && !g_scene.done && g_scene.cam && (!g_scene.world || g_scene.rt == g_scene.world) &&
        !g_scene.world_done)
    {
        /* the sun the votes chose, into this scene's camera space; w = 1 when lit draws voted in this
         * scene (without, the back end holds the last sun) */
        int sun = sun_pick();
        GfxScene* sc = &g_scene.s;
        sc->sun_dir[3] = 0.0f;
        if (sun >= 0 && g_scene.sun_draw)
        {
            const float* w = g_sunv.c[sun].dir;
            for (int j = 0; j < 3; ++j)
                sc->sun_dir[j] = w[0] * sc->view[j] + w[1] * sc->view[4 + j] + w[2] * sc->view[8 + j];
            float l = sqrtf(sc->sun_dir[0] * sc->sun_dir[0] + sc->sun_dir[1] * sc->sun_dir[1] + sc->sun_dir[2] * sc->sun_dir[2]);
            if (l > 0.0f)
            {
                sc->sun_dir[0] /= l, sc->sun_dir[1] /= l, sc->sun_dir[2] /= l, sc->sun_dir[3] = 1.0f;
                memcpy(sc->sun_color, g_sunv.c[sun].color, 16);
            }
        }
        gfx_scene_done(g_scene.rt, &g_scene.s);
        g_scene.world_done = 1;
        g_scene.tr_why = why[0] == 's' && why[1] == 'w' ? 'w' : why[0], g_scene.tr_draws = g_scene.draws;
    }
    g_scene.draws = 0;
    g_scene.done = 1;
}

static void scene_note(GfxDraw* d)
{
    d->caster = 0;
    uint32_t face, level;
    Obj* rt = obj(g_dev.rt);
    GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    if (!c)
        return;
    int large = (uint64_t)rt->width * rt->height * 2 >= (uint64_t)g_dev.pp[0] * g_dev.pp[1];
    if (g_scene.draws && !g_scene.done)
    {
        int sampled = 0;
        for (int i = 0; i < 8; ++i)
            sampled |= d->tex[i] == g_scene.rt;
        /* the scene drawn somewhere as large as the screen (copied into the target the interface goes
         * on, or onto the back buffer): not the sun flare's occlusion probe, which copies it into
         * 16x16 - several times a frame, while the world is still being drawn */
        if (sampled && large)
            scene_finish("sampled");
        /* the interface over the scene - on the back buffer, or into the scene's own target: FFXI draws
         * nameplates and floating text there, blended, before it draws the scene onto the back buffer;
         * the effects go under them, not over. (Not the probe's own screen-space quads there, which
         * are opaque - and textured, some frames.) */
        else if (d->vs.rhw && c == g_scene.rt && (g_dev.rt == g_dev.backbuffer || d->pipe.blend))
            scene_finish("interface");
        /* the effects over the world - flames, glows, spells: blended, writing no depth, and unfogged
         * (the zone's own blended decals among its draws are fogged). The effects go under them: fog
         * read from the depth behind a flame took it for the wall there, and fogged it more or less as
         * it flickered. */
        else if (!d->vs.rhw && c == g_scene.rt && d->depth.zenable && !d->depth.zwrite && d->pipe.blend &&
            !d->fs.fog && !d->vs.fog_vertex)
            scene_finish("effects");
    }
    if (c == g_scene.rt && g_scene.draws && !g_scene.done && (d->vs.rhw || !d->depth.zenable))
        g_scene.st_over++, g_scene.st_over_rhw += d->vs.rhw;
    if (d->vs.rhw || !d->depth.zenable || !obj(g_dev.ds) || face || level ||
        (uint64_t)rt->width * rt->height * 2 < (uint64_t)g_dev.pp[0] * g_dev.pp[1])
        return;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == c || !g_scene.tally[i].t || i == 3)
        {
            if (g_scene.tally[i].t != c)
                g_scene.tally[i].t = c, g_scene.tally[i].n = 0;
            g_scene.tally[i].n++;
            break;
        }
    /* 3D into other targets - the game draws characters into one partway through the world, then
     * goes on with the world - is not the world's scene: it neither ends nor starts one */
    if (g_scene.world && c != g_scene.world)
        return;
    if (c != g_scene.rt) /* a new scene: the one before is done; the new one's camera and sun are its own */
    {
        scene_finish("switch");
        g_scene.rt = c, g_scene.draws = 0, g_scene.done = 0, g_scene.cam_rank = 0, g_scene.sun_draw = 0;
    }
    if (g_scene.done) /* 3D again after the effects ran: drawn as it is */
    {
        g_scene.st_late++, g_scene.tr_late++;
        return;
    }
    g_scene.draws++;
    /* what casts the sun's shadow (the back end's shadow map): the scene's opaque, depth-writing
     * draws - alpha-tested ones too (leaves, fences) - of the world, which is fogged or drawn by the
     * zone's shaders; not the sky, the sun and its flare (unfogged, with views of their own) */
    d->caster = d->depth.zwrite && (!d->pipe.blend || (d->fs.alpha_func && d->fs.alpha_func != 8)) &&
        (d->vs.prog || d->fs.fog || d->vs.fog_vertex) && !g_scene.world_done;
    const State* s = &g_dev.cur;
    GfxScene* sc = &g_scene.s;
    /* the camera and fog from the frame's first depth-writing fixed-function draw, a fogged one
     * (the world) over any other: the sun, its flare and the sky come later with their own views
     * and no fog, and taking theirs made the effects' fog come and go with the camera's angle */
    uint32_t rank = d->fs.fog ? 2u : 1u;
    if (!d->vs.prog && s->xf[3][11] != 0.0f && d->depth.zwrite && rank > g_scene.cam_rank)
    {
        g_scene.cam_rank = rank, g_scene.cam_draw = g_scene.draws;
        memcpy(sc->proj, s->xf[3], 64);
        memcpy(sc->view, s->xf[2], 64);
        memcpy(sc->vp, d->vp, sizeof sc->vp);
        memcpy(sc->ambient, d->u.ambient, 16);
        memcpy(sc->fogcolor, d->u.fogcolor, 16);
        sc->fog[0] = d->u.params[2], sc->fog[1] = d->u.params[3], sc->fog[2] = d->fs.fog ? 1.0f : 0.0f;
        g_scene.cam = 1;
    }
    if (d->vs.nlights && d->depth.zwrite)
    {
        /* each directional light's vote (sun_vote), back into the world through the view's rotation */
        const float* v = s->xf[2];
        for (int k = 0; k < d->vs.nlights; ++k)
        {
            if (d->vs.light_type[k] != 3)
                continue;
            const float* c = d->u.light[k].dir;
            float w[3];
            for (int i = 0; i < 3; ++i)
                w[i] = c[0] * v[i * 4] + c[1] * v[i * 4 + 1] + c[2] * v[i * 4 + 2];
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (l <= 0.0f)
                continue;
            w[0] /= l, w[1] /= l, w[2] /= l;
            sun_vote(w, d->u.light[k].diffuse, k == 0 ? 1.0f : 0.5f);
            if (!g_scene.sun_draw)
                g_scene.sun_draw = g_scene.draws;
        }
    }
}

/* Present: the scene ends here if nothing ended it before; the next frame starts over */
/* The trace: while ~/Library/Caches/FFXI/trace.go exists (checked every 30 frames), the last 1200
 * frames' scenes go to trace.txt beside it and the effects' own to trace_gfx.txt; then .go is
 * removed. For a flicker the eye catches: what the frame before and after it did. */
static void scene_trace(GfxTex* world_before)
{
    uint32_t targets = 0;
    for (int i = 0; i < 4; ++i)
        targets += g_scene.tally[i].t != NULL;
    __typeof__(g_scene.trace[0])* t = &g_scene.trace[g_scene.frame % 1200];
    t->frame = g_scene.frame, t->draws = g_scene.tr_draws, t->late = g_scene.tr_late, t->targets = targets;
    t->why = g_scene.world_done ? g_scene.tr_why : '-', t->ran = (char)g_scene.world_done;
    t->changed = (char)(world_before != g_scene.world), t->cam = (char)g_scene.cam_rank;
    g_scene.tr_late = g_scene.tr_draws = 0, g_scene.tr_why = 0;
    g_scene.frame++;
    static char dir[900];
    if (!dir[0] && getenv("HOME"))
        snprintf(dir, sizeof dir, "%s/Library/Caches/FFXI", getenv("HOME"));
    if (!dir[0] || g_scene.frame % 30)
        return;
    char go[1024], out[1024];
    snprintf(go, sizeof go, "%s/trace.go", dir);
    struct stat st;
    if (stat(go, &st))
        return;
    remove(go);
    snprintf(out, sizeof out, "%s/trace.txt", dir);
    FILE* f = fopen(out, "w");
    if (f)
    {
        fprintf(f, "frame effects ended(s sampled, e effects, i interface, p present, w switch, - none) world-draws late-3D targets "
                   "world-changed camera-rank\n");
        uint32_t n = g_scene.frame < 1200 ? g_scene.frame : 1200;
        for (uint32_t k = g_scene.frame - n; k < g_scene.frame; ++k)
        {
            __typeof__(g_scene.trace[0])* e = &g_scene.trace[k % 1200];
            fprintf(f, "%u %d %c %u %u %u %d %d\n", e->frame, e->ran, e->why ? e->why : '?', e->draws, e->late, e->targets,
                e->changed, e->cam);
        }
        fclose(f);
    }
    snprintf(out, sizeof out, "%s/trace_gfx.txt", dir);
    gfx_trace_dump(out);
}

static void scene_present(void)
{
    scene_finish("present");
    GfxTex* world_before = g_scene.world;
    /* next frame's world: this frame's busiest target */
    uint32_t best = 0;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t && g_scene.tally[i].n > best)
            best = g_scene.tally[i].n, g_scene.world = g_scene.tally[i].t;
    scene_trace(world_before);
    g_scene.world_done = 0;
    for (int i = 0; i < 8; ++i)
        g_sunv.c[i].score *= 0.99f;
    for (int i = 0; i < 4; ++i)
        g_scene.tally[i].t = NULL, g_scene.tally[i].n = 0;
    if (gfx_profiling && ++g_scene.st_frames == 120)
    {
        fprintf(stderr, "[recomp] d3d8: scenes (120 frames, the last on %s): ended sampled %u, interface %u, present %u, switch %u; camera fogged %u, "
            "unfogged %u, none %u; 3D after the effects %u; over the scene before them %u (%u screen-space)\n",
            g_scene.rt == g_scene.world ? "the world's view" : "another target", g_scene.st_why[0], g_scene.st_why[1], g_scene.st_why[2],
            g_scene.st_why[3], g_scene.st_cam[2], g_scene.st_cam[1], g_scene.st_cam[0],
            g_scene.st_late, g_scene.st_over, g_scene.st_over_rhw);
        memset(g_scene.st_why, 0, sizeof g_scene.st_why), memset(g_scene.st_cam, 0, sizeof g_scene.st_cam);
        g_scene.st_frames = g_scene.st_late = g_scene.st_over = g_scene.st_over_rhw = 0;
    }
    g_scene.done = 0;
    g_scene.cam_draw = g_scene.sun_draw = g_scene.cam_rank = 0;
}

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n);

/* A draw: from `start` (non-indexed), through guest indices, or from ...UP memory. */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride)
{
    uint32_t n = prim_vertices(prim, count);
    if (!n)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_packet(prim, count, start, indices, index_size, up_data, up_stride, n);
    if (gfx_profiling)
        gfx_prof_front(gfx_now_ns() - t0);
}

/* The game's cursor: a small screen-space draw (its sprite, 18 pixels in a 1280-wide menu) from the
 * cursor's 64x64 sheet whose box holds the point the game was last given the mouse at. The sheet
 * matters: with the mouse outside the interface box that point is raw, and a menu glyph whose
 * unsqueezed place lies under it would otherwise be taken for the cursor and drawn there. */
static int ui_is_cursor(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width > 64 || t->height > 64)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f, W, H, fx, fy;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            return 0;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        lo = x < lo ? x : lo, hi = x > hi ? x : hi, ylo = y < ylo ? y : ylo, yhi = y > yhi ? y : yhi;
    }
    ui_target_size(&W, &H);
    user32_mouse_given(&fx, &fy);
    float mx = fx * W, my = fy * H, most = 48.0f * W / 1280.0f;
    int hit = hi - lo <= most && yhi - ylo <= most && mx >= lo - 2.0f && mx <= hi + 2.0f && my >= ylo - 2.0f &&
        my <= yhi + 2.0f;
    return hit;
}

/* The target arrow: the game puts it over its target's place on the screen, a sprite of the cursor's
 * 64x64 sheet (cursor frames, the menus' hand, the arrow) drawn turned a quarter - the only sprite of
 * the interface drawn so (the lobby, menus and zones checked): the texture's u runs down the screen.
 * It stays where the game put it, over the world. */
static int ui_is_world_pointer(uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width != 64 || t->height != 64 || n != 4 || g_dev.cur.vs != 0x144)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        Obj* b = obj(g_dev.cur.stream[0]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[0], size = b->size;
    }
    uint32_t a = first * stride, c = a + stride; /* XYZRHW, diffuse, then u v at 20 */
    if (stride < 28 || c + 28 > size)
        return 0;
    float x0 = u2f(rd32(base + a)), y0 = u2f(rd32(base + a + 4)), u0 = u2f(rd32(base + a + 20));
    float x1 = u2f(rd32(base + c)), y1 = u2f(rd32(base + c + 4)), u1 = u2f(rd32(base + c + 20));
    return fabsf(x1 - x0) < 0.5f && fabsf(y1 - y0) >= 1.0f && fabsf(u1 - u0) > 1e-4f;
}

/* A surface of FFXI's menu target: a render-target texture smaller than the screen of the interface
 * box's shape under --ui-aspect (1280x720 for 16:9), which the game stretches over the whole screen */
static int menu_target(const Obj* o)
{
    const Obj* t = o && o->container ? obj(o->container) : NULL;
    return t && t->kind == O_TEXTURE && (t->usage & USAGE_RENDERTARGET) && ui_box_shape(t->width, t->height);
}

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    GfxDraw* d = &g_draw;
    if (!build_draw(d))
        return;
    uint32_t first = start, nverts = n;
    if (indices)
    {
        uint32_t lo, hi;
        index_range(indices, index_size, n, &lo, &hi);
        first = lo + (up_data ? 0 : g_dev.cur.base_vertex);
        nverts = hi - lo + 1;
        d->u.vofs = -(int32_t)lo;
        d->indices = GUEST_PTR(indices);
        d->index_size = index_size;
        Obj* ib = up_data ? NULL : obj(g_dev.cur.ib);
        GfxBuf* gb = ib && indices >= ib->mem && indices < ib->mem + ib->size ? static_buffer(ib) : NULL;
        if (gb && !((indices - ib->mem) & 3) && prim != 6 /* fans are rebuilt from the indices */)
            d->ibuf = gb, d->ibuf_off = indices - ib->mem;
    }
    if (!set_streams(d, first, nverts, up_data, up_stride))
        return;
    scene_note(d);
    if (g_cap)
        cap_draw(d, prim, count, first, nverts, up_data, up_stride);
    if (d->vs.rhw && (g_dev.rt == g_dev.backbuffer || menu_target(obj(g_dev.rt))))
    {
        /* the game's cursor is drawn where the mouse was given to it: squeezed when that was
         * unsqueezed (over the interface), as it is when it was given as it is (over the world) */
        int cursor = ui_is_cursor(d, first, nverts, up_data, up_stride);
        if (!cursor && ui_is_world_pointer(first, nverts, up_data, up_stride))
            ; /* the target arrow: where the game put it */
        else if (!cursor || !user32_mouse_raw())
            ui_squeeze(d, first, nverts, up_data, up_stride, !cursor); /* the cursor never marks the interface */
        Obj* t0 = obj(g_dev.cur.tex[0]);
        if (g_dev.rt == g_dev.backbuffer && t0 && t0->kind == O_TEXTURE && (t0->usage & USAGE_RENDERTARGET) &&
            ui_box_shape(t0->width, t0->height))
            g_ui_composited = 1;
        if (g_cap)
            fprintf(g_cap, "  ui vp %.1f %.1f %.1f %.1f clip %u %u %u %u\n", d->u.vp[0], d->u.vp[1], d->u.vp[2], d->u.vp[3],
                d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    }
    d->prim = prim, d->count = count;
    apply_targets();
    gfx_draw(d);
    if (g_cap)
        cap_dump_target(g_cap_n - 1);
}

/* --- resources: IDirect3DResource8, textures, buffers, surfaces ------------------------------------------ */
static void Resource_GetDevice(Guest* g)
{
    obj_addref(g_dev.guest);
    wr32(ARG(1), g_dev.guest);
    RET(D3D_OK, 2);
}

static void Resource_SetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->priority : 0;
    if (o)
        o->priority = ARG(1);
    RET(old, 2);
}

static void Resource_GetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->priority : 0, 1);
}

static void Resource_PreLoad(Guest* g) { RET(0, 1); }

static void Resource_GetType(Guest* g)
{
    Obj* o = obj(ARG(0));
    static const uint32_t types[] = { 0, 0, 0, RT_TEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER, RT_SURFACE };
    RET(o ? types[o->kind] : 0, 1);
}

static void Texture_SetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->lod : 0;
    if (o && o->pool == 1) /* managed textures only */
        o->lod = ARG(1) < o->levels ? ARG(1) : o->levels - 1;
    RET(old, 2);
}

static void Texture_GetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->lod : 0, 1);
}

static void Texture_GetLevelCount(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->levels : 0, 1);
}

static void write_surface_desc(uint32_t p, const Obj* s)
{
    wr32(p, s->format);
    wr32(p + 4, RT_SURFACE);
    wr32(p + 8, s->usage);
    wr32(p + 12, s->pool);
    wr32(p + 16, s->size);
    wr32(p + 20, 0); /* MultiSampleType */
    wr32(p + 24, s->width);
    wr32(p + 28, s->height);
}

static Obj* sub(uint32_t tex, uint32_t face, uint32_t level)
{
    Obj* t = obj(tex);
    if (!t || level >= t->levels || face >= (t->kind == O_CUBE ? 6u : 1u))
        return NULL;
    return obj(t->subs[face * t->levels + level]);
}

static void Texture_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

static void Texture_GetSurfaceLevel(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    wr32(ARG(2), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    obj_addref(s->guest);
    RET(D3D_OK, 3);
}

#define LOCK_READONLY 0x10u

/* D3DLOCKED_RECT for a surface and an optional RECT. A surface the GPU owns is read back first
 * and written again at unlock; one the game owns is uploaded before its next draw. */
static void lock_rect(Obj* s, uint32_t locked, uint32_t rect, uint32_t flags)
{
    uint32_t pitch = fmt_pitch(s->format, s->width), bits = obj_mem(s);
    if (gpu_owned(s) && surface_scaled(s))
    {
        /* its pixels are not the game's size: not read back (FFXI never reads its menu target) */
        static int said;
        if (!said)
            said = 1, rt_log("[recomp] d3d8: a lock of a %ux%u render target drawn larger reads nothing\n", s->width, s->height);
    }
    else if (gpu_owned(s))
    {
        uint32_t face, level;
        GfxTex* g = surface_gpu(s, &face, &level);
        if (!s->gpu_locked && !(s->usage & USAGE_DEPTHSTENCIL))
        {
            /* The game's per-frame probe decides what is drawn from these pixels: a copy even a frame
             * old makes characters flicker. So the read waits (the back end commits each frame in
             * chunks, so the wait is the frame's tail); FFXI_ASYNC_READBACK=1 trades that for
             * the newest finished copy. */
            static int async = -1, visible = -1;
            if (async < 0)
                async = getenv("FFXI_ASYNC_READBACK") && getenv("FFXI_ASYNC_READBACK")[0] == '1';
            if (visible < 0)
                visible = !(getenv("FFXI_PROBE") && !strcmp(getenv("FFXI_PROBE"), "gpu"));
            /* The game's 16x16 occlusion probe (FFXiMain 0x1006c8c0: CopyRects of a 16x16 target,
             * a read-only lock, bit 7 of the blue byte counted over 8x8 samples: 0 hidden .. 256
             * fully visible) reads fully visible, with no wait for the GPU. Reading it for real
             * stalls the CPU on the whole scene every frame (7-8 ms at a 4096x4096 background),
             * and a late answer makes characters flicker; answering visible draws what the probe
             * would have hidden, which the depth test hides anyway. FFXI_PROBE=gpu reads it.
             * Not so the sky probe: the sun's lens flare draws without a depth test, so it is the
             * probe that hides it behind walls. That one reads a frame late, for free; the flare
             * writes no depth, so the late answer does not feed back into the next. */
            int probe = (flags & LOCK_READONLY) && s->width == 16 && s->height == 16, sky = probe && g_probe_sky;
            if (probe)
                g_probe_sky = 0;
            if (visible && sky)
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
            else if (visible && probe)
                memset(GUEST_PTR(bits), 0xFF, (size_t)pitch * s->height);
            else if (async && (flags & LOCK_READONLY) && s->width * s->height <= 128 * 128)
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
            else
                gfx_tex_read(g, face, level, GUEST_PTR(bits), pitch);
        }
        if (!(flags & LOCK_READONLY) && !(s->usage & USAGE_DEPTHSTENCIL))
            s->gpu_locked = 1;
    }
    else if (!(flags & LOCK_READONLY))
        mark_dirty(s);
    if (rect)
    {
        uint32_t l = rd32(rect), t = rd32(rect + 4), blk = fmt_block(s->format);
        bits += blk ? (t / 4) * pitch + (l / 4) * blk : t * pitch + l * fmt_bytes(s->format);
    }
    wr32(locked, pitch);
    wr32(locked + 4, bits);
}

static void unlock_rect(Obj* s)
{
    if (!s->gpu_locked)
        return;
    uint32_t face, level;
    GfxTex* g = surface_gpu(s, &face, &level);
    gfx_tex_upload(g, face, level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
    s->gpu_locked = 0;
}

/* LockRect(Level, pLockedRect, pRect, Flags) */
static void Texture_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 5);
    lock_rect(s, ARG(2), ARG(3), ARG(4));
    RET(D3D_OK, 5);
}

static void Texture_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    unlock_rect(s);
    RET(D3D_OK, 2);
}
static void Texture_AddDirtyRect(Guest* g) { RET(D3D_OK, 2); }

static void Cube_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

/* GetCubeMapSurface(FaceType, Level, ppCubeMapSurface) */
static void Cube_GetCubeMapSurface(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    wr32(ARG(3), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    obj_addref(s->guest);
    RET(D3D_OK, 4);
}

/* LockRect(FaceType, Level, pLockedRect, pRect, Flags) */
static void Cube_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 6);
    lock_rect(s, ARG(3), ARG(4), ARG(5));
    RET(D3D_OK, 6);
}

static void Cube_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    unlock_rect(s);
    RET(D3D_OK, 3);
}
static void Cube_AddDirtyRect(Guest* g) { RET(D3D_OK, 3); }

/* Lock(OffsetToLock, SizeToLock, ppbData, Flags) */
static void Buffer_Lock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b || ARG(1) > b->size)
        RET(D3DERR_INVALIDCALL, 5);
    if (!(ARG(4) & LOCK_READONLY))
        b->gbuf_dirty = 1;
    wr32(ARG(3), obj_mem(b) + ARG(1));
    RET(D3D_OK, 5);
}

static void Buffer_Unlock(Guest* g) { RET(obj(ARG(0)) ? D3D_OK : D3DERR_INVALIDCALL, 1); }

static void VertexBuffer_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[6] = { 100 /* D3DFMT_VERTEXDATA */, RT_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void IndexBuffer_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[5] = { b->format, RT_INDEXBUFFER, b->usage, b->pool, b->size };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void Surface_GetContainer(Guest* g)
{
    Obj* s = obj(ARG(0));
    uint32_t c = s && s->container ? s->container : g_dev.guest;
    obj_addref(c);
    wr32(ARG(2), c);
    RET(D3D_OK, 3);
}

static void Surface_GetDesc(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    write_surface_desc(ARG(1), s);
    RET(D3D_OK, 2);
}

/* LockRect(pLockedRect, pRect, Flags) */
static void Surface_LockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    lock_rect(s, ARG(1), ARG(2), ARG(3));
    RET(D3D_OK, 4);
}

static void Surface_UnlockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 1);
    unlock_rect(s);
    RET(D3D_OK, 1);
}

/* --- entry point and registration ---------------------------------------------------------------------------- */
/* Direct3DCreate8(SDKVersion) */
static void sh_Direct3DCreate8(Guest* g)
{
    if (!g_d3d)
        g_d3d = obj_new(O_D3D);
    else
        obj_addref(g_d3d);
    RET(g_d3d, 1);
}

/* DebugSetMute(BOOL): D3DX silences the debug runtime; cdecl (the caller pops its argument) */
static void sh_DebugSetMute(Guest* g) { RETC(0); }

/* ValidateVertexShader(pShader, pDeclaration, pCaps, ReturnErrors, ppErrors) and
 * ValidatePixelShader(pShader, pCaps, ReturnErrors, ppErrors), stdcall: what D3DX's shader
 * assembler asks of d3d8.dll after assembling (fetched by GetProcAddress, 0x102ed02c in build
 * 2026-09-03). As Wine: the version token decides (vs.1.0/1.1, ps.1.0-1.4), and there are never
 * error strings. The game's shaders are vs.1.1/ps.1.1, which pass. */
#define E_FAIL 0x80004005u
static void sh_ValidateVertexShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(4))
        wr32(ARG(4), 0);
    RET(v == 0xFFFE0100u || v == 0xFFFE0101u ? D3D_OK : E_FAIL, 5);
}

static void sh_ValidatePixelShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(3))
        wr32(ARG(3), 0);
    RET(v >= 0xFFFF0100u && v <= 0xFFFF0104u ? D3D_OK : E_FAIL, 4);
}

#define D(i, m) { "d3d8.dll", #i "::" #m, i##_##m }
#define U(i, m, f) { "d3d8.dll", #i "::" #m, f }
#define UNKNOWN(i) U(i, QueryInterface, Unknown_QueryInterface), U(i, AddRef, Unknown_AddRef), U(i, Release, Unknown_Release)
#define RESOURCE(i)                                                                                                   \
    UNKNOWN(i), U(i, GetDevice, Resource_GetDevice), U(i, SetPriority, Resource_SetPriority),                         \
        U(i, GetPriority, Resource_GetPriority), U(i, PreLoad, Resource_PreLoad), U(i, GetType, Resource_GetType)

static const ShimDef D3D8[] = {
    { "d3d8.dll", "Direct3DCreate8", sh_Direct3DCreate8 },
    { "d3d8.dll", "DebugSetMute", sh_DebugSetMute },
    { "d3d8.dll", "ValidateVertexShader", sh_ValidateVertexShader },
    { "d3d8.dll", "ValidatePixelShader", sh_ValidatePixelShader },
    UNKNOWN(IDirect3D8),
    D(IDirect3D8, GetAdapterCount),
    D(IDirect3D8, GetAdapterIdentifier),
    D(IDirect3D8, GetAdapterModeCount),
    D(IDirect3D8, EnumAdapterModes),
    D(IDirect3D8, GetAdapterDisplayMode),
    D(IDirect3D8, CheckDeviceType),
    D(IDirect3D8, CheckDeviceFormat),
    D(IDirect3D8, CheckDeviceMultiSampleType),
    D(IDirect3D8, CheckDepthStencilMatch),
    D(IDirect3D8, GetDeviceCaps),
    D(IDirect3D8, GetAdapterMonitor),
    D(IDirect3D8, CreateDevice),

    UNKNOWN(IDirect3DDevice8),
    D(IDirect3DDevice8, TestCooperativeLevel),
    D(IDirect3DDevice8, GetAvailableTextureMem),
    D(IDirect3DDevice8, ResourceManagerDiscardBytes),
    D(IDirect3DDevice8, GetDirect3D),
    D(IDirect3DDevice8, GetDeviceCaps),
    D(IDirect3DDevice8, GetDisplayMode),
    D(IDirect3DDevice8, GetCreationParameters),
    D(IDirect3DDevice8, SetCursorProperties),
    D(IDirect3DDevice8, SetCursorPosition),
    D(IDirect3DDevice8, ShowCursor),
    D(IDirect3DDevice8, Reset),
    D(IDirect3DDevice8, Present),
    D(IDirect3DDevice8, GetBackBuffer),
    D(IDirect3DDevice8, GetRasterStatus),
    D(IDirect3DDevice8, SetGammaRamp),
    D(IDirect3DDevice8, CreateTexture),
    D(IDirect3DDevice8, CreateVolumeTexture),
    D(IDirect3DDevice8, CreateCubeTexture),
    D(IDirect3DDevice8, CreateVertexBuffer),
    D(IDirect3DDevice8, CreateIndexBuffer),
    D(IDirect3DDevice8, CreateRenderTarget),
    D(IDirect3DDevice8, CreateDepthStencilSurface),
    D(IDirect3DDevice8, CreateImageSurface),
    D(IDirect3DDevice8, CopyRects),
    D(IDirect3DDevice8, SetRenderTarget),
    D(IDirect3DDevice8, GetRenderTarget),
    D(IDirect3DDevice8, GetDepthStencilSurface),
    D(IDirect3DDevice8, BeginScene),
    D(IDirect3DDevice8, EndScene),
    D(IDirect3DDevice8, Clear),
    D(IDirect3DDevice8, SetTransform),
    D(IDirect3DDevice8, GetTransform),
    D(IDirect3DDevice8, MultiplyTransform),
    D(IDirect3DDevice8, SetViewport),
    D(IDirect3DDevice8, GetViewport),
    D(IDirect3DDevice8, SetMaterial),
    D(IDirect3DDevice8, GetMaterial),
    D(IDirect3DDevice8, SetLight),
    D(IDirect3DDevice8, GetLight),
    D(IDirect3DDevice8, LightEnable),
    D(IDirect3DDevice8, GetLightEnable),
    D(IDirect3DDevice8, SetClipPlane),
    D(IDirect3DDevice8, SetRenderState),
    D(IDirect3DDevice8, GetRenderState),
    D(IDirect3DDevice8, BeginStateBlock),
    D(IDirect3DDevice8, EndStateBlock),
    D(IDirect3DDevice8, ApplyStateBlock),
    D(IDirect3DDevice8, CaptureStateBlock),
    D(IDirect3DDevice8, DeleteStateBlock),
    D(IDirect3DDevice8, CreateStateBlock),
    D(IDirect3DDevice8, GetTexture),
    D(IDirect3DDevice8, SetTexture),
    D(IDirect3DDevice8, GetTextureStageState),
    D(IDirect3DDevice8, SetTextureStageState),
    D(IDirect3DDevice8, ValidateDevice),
    D(IDirect3DDevice8, DrawPrimitive),
    D(IDirect3DDevice8, DrawIndexedPrimitive),
    D(IDirect3DDevice8, DrawPrimitiveUP),
    D(IDirect3DDevice8, DrawIndexedPrimitiveUP),
    D(IDirect3DDevice8, CreateVertexShader),
    D(IDirect3DDevice8, SetVertexShader),
    D(IDirect3DDevice8, GetVertexShader),
    D(IDirect3DDevice8, DeleteVertexShader),
    D(IDirect3DDevice8, SetVertexShaderConstant),
    D(IDirect3DDevice8, GetVertexShaderConstant),
    D(IDirect3DDevice8, SetStreamSource),
    D(IDirect3DDevice8, GetStreamSource),
    D(IDirect3DDevice8, SetIndices),
    D(IDirect3DDevice8, GetIndices),
    D(IDirect3DDevice8, CreatePixelShader),
    D(IDirect3DDevice8, SetPixelShader),
    D(IDirect3DDevice8, GetPixelShader),
    D(IDirect3DDevice8, DeletePixelShader),
    D(IDirect3DDevice8, SetPixelShaderConstant),

    RESOURCE(IDirect3DTexture8),
    U(IDirect3DTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DTexture8, GetLevelDesc, Texture_GetLevelDesc),
    U(IDirect3DTexture8, GetSurfaceLevel, Texture_GetSurfaceLevel),
    U(IDirect3DTexture8, LockRect, Texture_LockRect),
    U(IDirect3DTexture8, UnlockRect, Texture_UnlockRect),
    U(IDirect3DTexture8, AddDirtyRect, Texture_AddDirtyRect),

    RESOURCE(IDirect3DCubeTexture8),
    U(IDirect3DCubeTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DCubeTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DCubeTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DCubeTexture8, GetLevelDesc, Cube_GetLevelDesc),
    U(IDirect3DCubeTexture8, GetCubeMapSurface, Cube_GetCubeMapSurface),
    U(IDirect3DCubeTexture8, LockRect, Cube_LockRect),
    U(IDirect3DCubeTexture8, UnlockRect, Cube_UnlockRect),
    U(IDirect3DCubeTexture8, AddDirtyRect, Cube_AddDirtyRect),

    RESOURCE(IDirect3DVertexBuffer8),
    U(IDirect3DVertexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DVertexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DVertexBuffer8, GetDesc, VertexBuffer_GetDesc),

    RESOURCE(IDirect3DIndexBuffer8),
    U(IDirect3DIndexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DIndexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DIndexBuffer8, GetDesc, IndexBuffer_GetDesc),

    UNKNOWN(IDirect3DSurface8),
    U(IDirect3DSurface8, GetDevice, Resource_GetDevice),
    U(IDirect3DSurface8, GetContainer, Surface_GetContainer),
    U(IDirect3DSurface8, GetDesc, Surface_GetDesc),
    U(IDirect3DSurface8, LockRect, Surface_LockRect),
    U(IDirect3DSurface8, UnlockRect, Surface_UnlockRect),
    { NULL, NULL, NULL },
};

/* vtable order: the D3D8 ABI (d3d8proxy.cpp's tables) */
#define RESOURCE8_NAMES "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData", \
    "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType"
static const char* const kD3D8[] = { "QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
    "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
    "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "GetDeviceCaps", "GetAdapterMonitor",
    "CreateDevice", NULL };
static const char* const kDevice[] = { "QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
    "GetAvailableTextureMem", "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
    "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight",
    "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "BeginStateBlock", "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock",
    "CreateStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState",
    "SetTextureStageState", "ValidateDevice", "GetInfo", "SetPaletteEntries", "GetPaletteEntries",
    "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
    "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices",
    "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader",
    "SetPixelShaderConstant", "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch",
    "DeletePatch", NULL };
static const char* const kTexture[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kCube[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetCubeMapSurface", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kVB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kIB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kSurface[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("d3d8.dll", full));
    }
    return v;
}

void d3d8_init(void)
{
    thunk_register(D3D8);
}

/* the vtables are guest memory: built once the guest heap is up (from the host, before GameStart) */
void d3d8_setup(void)
{
    g_vtbl[O_D3D] = make_vtbl("IDirect3D8", kD3D8);
    g_vtbl[O_DEVICE] = make_vtbl("IDirect3DDevice8", kDevice);
    g_vtbl[O_TEXTURE] = make_vtbl("IDirect3DTexture8", kTexture);
    g_vtbl[O_CUBE] = make_vtbl("IDirect3DCubeTexture8", kCube);
    g_vtbl[O_VB] = make_vtbl("IDirect3DVertexBuffer8", kVB);
    g_vtbl[O_IB] = make_vtbl("IDirect3DIndexBuffer8", kIB);
    g_vtbl[O_SURFACE] = make_vtbl("IDirect3DSurface8", kSurface);
}
