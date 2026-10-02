/* The screens' drawing on the graphics back end. See uidraw.h. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "uidraw.h"

enum
{
    FMT_A8R8G8B8 = 21,
    FMT_X8R8G8B8 = 22,
    MAX_BATCH = 256, /* quads a draw */
};

/* XYZRHW | DIFFUSE | TEX1, the game's own UI vertex */
typedef struct Vert
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} Vert;

static SDL_Window* g_window;
static GfxTex* g_bb;
static int g_bw, g_bh;
static Vert g_verts[MAX_BATCH * 6];

int uidraw_open(void* sdl_window)
{
    g_window = sdl_window;
    return gfx_init(sdl_window, 1);
}

void uidraw_close(void)
{
    if (g_bb)
    {
        gfx_finish();
        gfx_tex_destroy(g_bb);
    }
    g_bb = NULL;
    g_bw = g_bh = 0;
    g_window = NULL;
}

unsigned uidraw_load(UiTexSet* set, const DatFile* f, const char* category, const char* const* names)
{
    unsigned added = 0;
    for (; *names && set->n < sizeof set->tex / sizeof *set->tex; ++names)
    {
        DatImage img;
        if (!dat_image(f, category, *names, &img))
            continue;
        /* RGBA to A8R8G8B8's bytes: B G R A */
        for (size_t i = 0; i < (size_t)img.w * img.h; ++i)
        {
            uint8_t t = img.rgba[i * 4];
            img.rgba[i * 4] = img.rgba[i * 4 + 2];
            img.rgba[i * 4 + 2] = t;
        }
        UiTex* t = &set->tex[set->n];
        t->gpu = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, img.w, img.h, 1, GFX_USE_SAMPLE);
        t->smooth = 0;
        if (t->gpu)
        {
            gfx_tex_upload(t->gpu, 0, 0, img.rgba, img.w * 4);
            SDL_strlcpy(t->name, *names, sizeof t->name);
            t->w = img.w;
            t->h = img.h;
            set->n++;
            added++;
        }
        dat_image_free(&img);
    }
    return added;
}

int uidraw_load_rgba(UiTexSet* set, const char* name, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    return uidraw_load_rgba_scaled(set, name, rgba, w, h, w, h);
}

int uidraw_load_rgba_scaled(UiTexSet* set, const char* name, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t lw,
    uint32_t lh)
{
    for (unsigned i = 0; i < set->n; ++i)
        if (!strcmp(set->tex[i].name, name))
        {
            gfx_tex_destroy(set->tex[i].gpu);
            set->tex[i] = set->tex[--set->n];
            break;
        }
    if (set->n == sizeof set->tex / sizeof *set->tex)
        return 0;
    uint8_t* bgra = malloc((size_t)w * h * 4);
    if (!bgra)
        return 0;
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    UiTex* t = &set->tex[set->n];
    t->gpu = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, w, h, 1, GFX_USE_SAMPLE);
    if (t->gpu)
    {
        gfx_tex_upload(t->gpu, 0, 0, bgra, w * 4);
        SDL_strlcpy(t->name, name, sizeof t->name);
        t->w = lw, t->h = lh;
        t->smooth = 1;
        set->n++;
    }
    free(bgra);
    return t->gpu != NULL;
}

void uidraw_free(UiTexSet* set)
{
    for (unsigned i = 0; i < set->n; ++i)
        gfx_tex_destroy(set->tex[i].gpu);
    memset(set, 0, sizeof *set);
}

static const UiTex* find(const UiTexSet* set, const char* name)
{
    for (unsigned i = 0; i < set->n; ++i)
        if (!strcmp(set->tex[i].name, name))
            return &set->tex[i];
    return NULL;
}

static void identity(float* m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1;
}

/* The fixed-function state of a UI draw: pre-transformed vertices, the texel times the vertex
 * colour (or the colour alone), source-over blending, no depth. */
static void setup(GfxDraw* d, const UiTex* t, int shape, int blend)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp), identity(d->u.wv), identity(d->u.wvit);
    for (int i = 0; i < 8; ++i)
        identity(d->u.texm[i]);
    d->u.vp[2] = (float)g_bw, d->u.vp[3] = (float)g_bh;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1;
    uint32_t vp[6] = { 0, 0, (uint32_t)g_bw, (uint32_t)g_bh, 0, 0x3F800000u };
    memcpy(d->vp, vp, sizeof vp);
    d->vs.rhw = 1;
    d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT4, 0 };
    d->vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->u.stride[0] = sizeof(Vert);
    d->vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d->vs.ntex = 1;
    d->u.offset[GFX_R_POSITION] = 0, d->u.offset[GFX_R_DIFFUSE] = 16, d->u.offset[GFX_R_TEXCOORD0] = 20;
    d->fs.nstages = 1;
    if (t)
    {
        /* D3DTOP_MODULATE (4) of TEXTURE (2) and DIFFUSE (0), colour and alpha; a shape only
         * takes the texture's alpha, its colour SELECTARG1 (2) of DIFFUSE */
        d->fs.st[0] = shape ? (GfxStage){ 2, 0, 1, 1, 4, 2, 0, 1, 1, 1, 0, 2 } : (GfxStage){ 4, 2, 0, 1, 4, 2, 0, 1, 1, 1, 0, 2 };
        d->tex[0] = t->gpu;
        /* the game's art wraps (the frame's edges and backgrounds repeat their texels), point
         * sampled; a picture is filtered, clamped at its edges */
        d->samp[0] = t->smooth ? (GfxSampler){ 3, 3, 3, 2, 2, 0, 1, 0, 0 } : (GfxSampler){ 1, 1, 1, 1, 1, 0, 1, 0, 0 };
    }
    else
        d->fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(DIFFUSE) */
    /* SRCALPHA, then INVSRCALPHA ADD (over), ONE ADD (added) or ONE REVSUBTRACT (taken away) */
    d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = blend == UI_BLEND_ALPHA ? 6 : 2;
    d->pipe.op = blend == UI_BLEND_SUBTRACT ? 3 : 1;
    d->pipe.write_mask = 0xF;
    d->cull = 1; /* D3DCULL_NONE */
    d->depth.zfunc = 8; /* ALWAYS */
    d->prim = GFX_TRIANGLELIST;
}

static void flush(GfxDraw* d, unsigned quads)
{
    if (!quads)
        return;
    d->data[0] = g_verts;
    d->size[0] = quads * 6 * sizeof(Vert);
    d->count = quads * 2;
    gfx_draw(d);
}

static uint32_t argb(const uint8_t c[4])
{
    return (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
}

/* A quad as two triangles: TL TR BL, TR BR BL, its top edge skew pixels to the right. D3D puts
 * pixel centres on whole coordinates, so the corners move half a pixel back for texels to land one
 * to a pixel. */
static void emit(Vert* v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
    const uint32_t c[4], float skew)
{
    x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f;
    Vert tl = { x0 + skew, y0, 0.5f, 1, c[0], u0, v0 }, tr = { x1 + skew, y0, 0.5f, 1, c[1], u1, v0 },
         bl = { x0, y1, 0.5f, 1, c[2], u0, v1 }, br = { x1, y1, 0.5f, 1, c[3], u1, v1 };
    v[0] = tl, v[1] = tr, v[2] = bl, v[3] = tr, v[4] = br, v[5] = bl;
}

void uidraw_begin(uint32_t clear, int* w, int* h)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw < 1)
        pw = 1;
    if (ph < 1)
        ph = 1;
    if (!g_bb || pw != g_bw || ph != g_bh)
    {
        if (g_bb)
        {
            gfx_finish();
            gfx_tex_destroy(g_bb);
        }
        g_bb = gfx_tex_create(GFX_TEX_2D, FMT_X8R8G8B8, (uint32_t)pw, (uint32_t)ph, 1, GFX_USE_RT);
        g_bw = pw, g_bh = ph;
    }
    gfx_set_targets(g_bb, 0, 0, NULL);
    uint32_t vp[6] = { 0, 0, (uint32_t)pw, (uint32_t)ph, 0, 0x3F800000u };
    gfx_clear(0, NULL, 1, clear, 1.0f, 0, vp);
    *w = pw, *h = ph;
}

void uidraw_quads(const UiTexSet* set, const UiQuad* q, unsigned n)
{
    GfxDraw d;
    const UiTex* cur = NULL;
    int cur_shape = 0, cur_blend = 0;
    unsigned batch = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        const UiTex* t = find(set, q[i].image);
        if (!t)
            continue;
        if (t != cur || q[i].shape != cur_shape || q[i].blend != cur_blend || batch == MAX_BATCH)
        {
            if (cur)
                flush(&d, batch);
            setup(&d, t, q[i].shape, q[i].blend);
            cur = t, cur_shape = q[i].shape, cur_blend = q[i].blend, batch = 0;
        }
        uint32_t c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = argb(q[i].color[k]);
        emit(&g_verts[batch * 6], q[i].x0, q[i].y0, q[i].x1, q[i].y1, q[i].u0 / (float)t->w, q[i].v0 / (float)t->h,
            q[i].u1 / (float)t->w, q[i].v1 / (float)t->h, c, q[i].skew);
        batch++;
    }
    if (cur)
        flush(&d, batch);
}

void uidraw_rect(float x0, float y0, float x1, float y1, uint32_t color)
{
    GfxDraw d;
    setup(&d, NULL, 0, UI_BLEND_ALPHA);
    uint32_t c[4] = { color, color, color, color };
    emit(g_verts, x0, y0, x1, y1, 0, 0, 0, 0, c, 0);
    flush(&d, 1);
}

void uidraw_end(void)
{
    gfx_present(g_bb);
}
