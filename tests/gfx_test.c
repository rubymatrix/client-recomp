/* The graphics back end without the game (R3.2): renders into an offscreen target through gfx.h,
 * reads the pixels back and checks them - clears, D3D's pixel-center rules for XYZRHW vertices,
 * texturing and the texture formats, fixed-function lighting, fog, alpha test, blending, the
 * vs.1.1 / ps.1.1 translation - then builds a sweep of fixed-function keys (every texture op,
 * argument modifier, fog mode, light type, texture coordinate source) so a generator change that
 * emits bad MSL fails here rather than in the game.
 *
 * usage: gfx_test            (exit status 0 when every check passes)
 *        gfx_test --window   the same on a device made for an SDL window, then two seconds of
 *                            frames presented to it (the CAMetalLayer path) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx.h"

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

#define W 32
#define H 32

static int g_fails;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

static GfxTex *g_rt, *g_ds;
static uint32_t g_px[W * H];

static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void readback(void)
{
    gfx_tex_read(g_rt, 0, 0, g_px, W * 4);
}

/* A8R8G8B8 as the D3D value */
static uint32_t px(int x, int y) { return g_px[y * W + x]; }

static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 32; s += 8)
    {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

static const uint32_t VP[6] = { 0, 0, W, H, 0, 0x3F800000u };

/* a draw with the device defaults the front end would produce */
static void defaults(GfxDraw* d)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp), identity(d->u.wv), identity(d->u.wvit);
    for (int i = 0; i < 8; ++i)
        identity(d->u.texm[i]);
    d->u.vp[0] = 0, d->u.vp[1] = 0, d->u.vp[2] = W, d->u.vp[3] = H;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1;
    d->pipe.write_mask = 0xF;
    d->cull = 1;
    d->depth.zfunc = 4;
    memcpy(d->vp, VP, sizeof VP);
    /* stage 0: MODULATE(TEXTURE, CURRENT), SELECTARG1(TEXTURE) alpha - D3D's defaults */
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2 };
}

/* XYZRHW | DIFFUSE | TEX1 (FVF 0x144), the game's UI vertex */
typedef struct
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} UiVert;

static void layout_ui(GfxDraw* d)
{
    d->vs.rhw = 1;
    d->vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT4, 0 };
    d->vs.el[5] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d->vs.ntex = 1;
    d->u.stride[0] = sizeof(UiVert);
    d->u.offset[0] = 0, d->u.offset[5] = 16, d->u.offset[7] = 20;
}

static void quad(UiVert* v, float x0, float y0, float x1, float y1, uint32_t c, float z)
{
    v[0] = (UiVert){ x0, y0, z, 1, c, 0, 0 };
    v[1] = (UiVert){ x1, y0, z, 1, c, 1, 0 };
    v[2] = (UiVert){ x0, y1, z, 1, c, 0, 1 };
    v[3] = (UiVert){ x1, y1, z, 1, c, 1, 1 };
}

static void draw_ui(GfxDraw* d, UiVert* v)
{
    d->data[0] = v, d->size[0] = 4 * sizeof(UiVert);
    d->prim = GFX_TRIANGLESTRIP, d->count = 2;
    gfx_draw(d);
}

static void test_clear(void)
{
    gfx_clear(0, NULL, 3, 0xFFFF0000u, 1.0f, 0, VP);
    readback();
    CHECK(px(0, 0) == 0xFFFF0000u && px(W - 1, H - 1) == 0xFFFF0000u, "full clear: %08x", px(0, 0));
    int32_t r[4] = { 4, 4, 8, 8 };
    gfx_clear(1, r, 1, 0xFF00FF00u, 1.0f, 0, VP);
    readback();
    CHECK(px(4, 4) == 0xFF00FF00u && px(7, 7) == 0xFF00FF00u, "rect clear inside: %08x %08x", px(4, 4), px(7, 7));
    CHECK(px(3, 4) == 0xFFFF0000u && px(8, 8) == 0xFFFF0000u, "rect clear outside: %08x %08x", px(3, 4), px(8, 8));
}

static void test_rhw_edges(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    UiVert v[4];
    quad(v, 8, 8, 24, 24, 0xFF0000FFu, 0.5f);
    draw_ui(&d, v);
    readback();
    /* D3D: pixel centers on integers, so [8, 24) covers pixels 8..23 */
    CHECK(px(8, 8) == 0xFF0000FFu && px(23, 23) == 0xFF0000FFu, "quad inside: %08x %08x", px(8, 8), px(23, 23));
    CHECK(px(7, 8) == 0xFF000000u && px(24, 23) == 0xFF000000u && px(8, 7) == 0xFF000000u && px(8, 24) == 0xFF000000u,
        "quad edges: %08x %08x %08x %08x", px(7, 8), px(24, 23), px(8, 7), px(8, 24));
}

static void test_texture(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* 2x2: red green / blue white, point sampled over a 16x16 quad */
    uint32_t tex[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, tex, 8);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0].tex = 1;
    d.tex[0] = t;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 2) == 0xFFFF0000u && px(12, 2) == 0xFF00FF00u && px(2, 12) == 0xFF0000FFu && px(12, 12) == 0xFFFFFFFFu,
        "texture: %08x %08x %08x %08x", px(2, 2), px(12, 2), px(2, 12), px(12, 12));

    /* modulated by the vertex color */
    quad(v, 16, 0, 32, 16, 0xFF808080u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(28, 12), 0xFF808080u, 1), "modulate: %08x", px(28, 12));
    gfx_tex_destroy(t);

    /* R5G6B5 and DXT1 */
    uint16_t t565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    t = gfx_tex_create(GFX_TEX_2D, 23, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, t565, 4);
    d.tex[0] = t;
    quad(v, 0, 16, 16, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 18) == 0xFFFF0000u && px(12, 18) == 0xFF00FF00u && px(2, 28) == 0xFF0000FFu, "R5G6B5: %08x %08x %08x", px(2, 18),
        px(12, 18), px(2, 28));
    gfx_tex_destroy(t);

    /* one DXT1 block, every texel color0 = pure green (0x07E0) */
    uint8_t dxt[8] = { 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
    t = gfx_tex_create(GFX_TEX_2D, 0x31545844u /* DXT1 */, 4, 4, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, dxt, 8);
    d.tex[0] = t;
    quad(v, 16, 16, 32, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(24, 24) == 0xFF00FF00u, "DXT1: %08x", px(24, 24));
    gfx_tex_destroy(t);
}

static void test_alpha(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* diffuse only */
    /* alpha test GREATER 0x80: a 0x40-alpha quad is discarded */
    d.fs.alpha_func = 5;
    d.u.params[1] = 128;
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0x40FFFFFFu, 0.5f);
    draw_ui(&d, v);
    /* blending SRCALPHA / INVSRCALPHA at 50% over black */
    d.fs.alpha_func = 8;
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = 6, d.pipe.op = 1;
    quad(v, 16, 0, 32, 16, 0x80FFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFF000000u, "alpha test: %08x", px(8, 8));
    CHECK(near(px(24, 8) & 0xFFFFFF, 0x808080u, 2), "blend: %08x", px(24, 8));
}

static void test_depth(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFFFF0000u, 0.25f);
    draw_ui(&d, v);
    quad(v, 0, 0, 16, 16, 0xFF00FF00u, 0.75f); /* behind: hidden */
    draw_ui(&d, v);
    quad(v, 16, 16, 32, 32, 0xFF0000FFu, 0.1f); /* in front */
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFFFF0000u && px(24, 24) == 0xFF0000FFu, "depth: %08x %08x", px(8, 8), px(24, 24));
}

/* XYZ | NORMAL (FVF 0x012) through the transform and one directional light */
static void test_lighting(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    float vert[4][6] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 24, d.u.offset[3] = 12;
    d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 3, d.vs.normalize = 1;
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    float half[4] = { 0.5f, 0.5f, 0.5f, 1 };
    memcpy(d.u.mat_d, half, 16);
    /* light straight at the surface (toward the light: -z), white; ambient 0.25 on a 1.0 material */
    d.u.light[0].diffuse[0] = d.u.light[0].diffuse[1] = d.u.light[0].diffuse[2] = 1;
    d.u.light[0].dir[2] = -1;
    d.u.ambient[0] = d.u.ambient[1] = d.u.ambient[2] = 0.25f;
    d.u.mat_a[0] = d.u.mat_a[1] = d.u.mat_a[2] = 1;
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    /* 0.25 ambient + 0.5 diffuse = 0.75 */
    CHECK(near(px(16, 16), 0xFFBFBFBFu, 2), "lighting: %08x", px(16, 16));
}

/* A point light just before the middle of a large quad: lit per pixel (light 2) its pool shows in
 * the middle; lit per vertex (light 0, and light 1: there the sun is per pixel, the game's torches
 * per vertex), the corners (almost edge-on to it) are all there is, and the middle stays dark. */
static void test_pixel_lighting(void)
{
    static const float modes[3] = { 2, 0, 1 };
    for (int pass = 0; pass < 3; ++pass)
    {
        gfx_fx_set("light", modes[pass]);
        gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
        GfxDraw d;
        defaults(&d);
        float vert[4][6] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
        d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.u.stride[0] = 24, d.u.offset[3] = 12;
        d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 1, d.vs.normalize = 1;
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
        d.u.mat_d[0] = d.u.mat_d[1] = d.u.mat_d[2] = d.u.mat_d[3] = 1;
        d.u.light[0].diffuse[0] = d.u.light[0].diffuse[1] = d.u.light[0].diffuse[2] = 1;
        d.u.light[0].pos[2] = 0.4f, d.u.light[0].pos[3] = 10.0f;
        d.u.light[0].att[0] = 1;
        d.data[0] = vert, d.size[0] = sizeof vert;
        d.prim = GFX_TRIANGLESTRIP, d.count = 2;
        gfx_draw(&d);
        readback();
        uint32_t c = px(W / 2, H / 2) & 255;
        if (pass == 0)
            CHECK(c >= 230, "per-pixel lighting: middle %u (want the light's pool)", c);
        else
            CHECK(c <= 40, "per-vertex point light (light %g): middle %u (want dark, from the corners)", modes[pass], c);
    }
    gfx_fx_set("light", 1.0f);
}

static void test_fog(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    /* linear table fog from 0 to 2 on w = 1 (rhw 1): half fogged toward white */
    d.fs.fog = 3;
    d.u.params[2] = 0, d.u.params[3] = 2;
    d.u.fogcolor[0] = d.u.fogcolor[1] = d.u.fogcolor[2] = 1;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFF000000u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(16, 16), 0xFF808080u, 2), "fog: %08x", px(16, 16));
}

static void test_shaders(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* vs.1.1: m4x4 oPos, v0, c0 / mov oD0, c4 / mov oT0, v7 */
    static const uint32_t vs[] = {
        0xFFFE0101u,
        20, 0x80000000u | (4u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16), 0x80000000u | (2u << 28) | (0xE4u << 16),
        1, 0x80000000u | (5u << 28) | (0xFu << 16), 0x80000000u | (2u << 28) | (0xE4u << 16) | 4,
        1, 0x80000000u | (6u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16) | 7,
        0x0000FFFFu,
    };
    /* ps.1.1: tex t0 / mul r0, t0, v0 */
    static const uint32_t ps[] = {
        0xFFFF0101u,
        66, 0x80000000u | (3u << 28) | (0xFu << 16),
        5, 0x80000000u | (0xFu << 16), 0x80000000u | (3u << 28) | (0xE4u << 16), 0x80000000u | (1u << 28) | (0xE4u << 16),
        0x0000FFFFu,
    };
    GfxDraw d;
    defaults(&d);
    d.vs.prog = 1, d.fs.prog = 1, d.vs_tokens = vs, d.ps_tokens = ps;
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d.vs.ntex = 8;
    d.u.stride[0] = 20, d.u.offset[7] = 12;
    identity(&d.u.vsc[0][0]); /* c0..c3 */
    d.u.vsc[4][0] = 1, d.u.vsc[4][1] = 0.5f, d.u.vsc[4][2] = 0, d.u.vsc[4][3] = 1;
    uint32_t white = 0xFFFFFFFFu;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, &white, 4);
    d.tex[0] = t;
    d.fs.st[0].tex = 1;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    float vert[4][5] = { { -1, 1, 0.5f, 0, 0 }, { 1, 1, 0.5f, 1, 0 }, { -1, -1, 0.5f, 0, 1 }, { 1, -1, 0.5f, 1, 1 } };
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    CHECK(near(px(16, 16), 0xFFFF8000u, 2), "vs/ps 1.1: %08x", px(16, 16));
    gfx_tex_destroy(t);
}

/* every texture op and argument form, fog mode, light type and coordinate source: all must build */
static void test_sweep(void)
{
    uint32_t before = gfx_failures();
    GfxDraw d;
    UiVert v[4];
    quad(v, 0, 0, 1, 1, 0xFFFFFFFFu, 0.5f);
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    GfxTex* cube = gfx_tex_create(GFX_TEX_CUBE, 21, 1, 1, 1, GFX_USE_SAMPLE);
    for (int op = 1; op <= 26; ++op)
        for (int form = 0; form < 3; ++form)
        {
            defaults(&d);
            layout_ui(&d);
            int mod = form == 1 ? 0x10 : form == 2 ? 0x20 : 0;
            d.fs.nstages = 2;
            d.fs.st[0] = (GfxStage){ (uint8_t)op, (uint8_t)(2 | mod), (uint8_t)(0 | mod), 3, (uint8_t)(op > 1 ? op : 2), 2, 0, 4, 1, 1, 0, 2 };
            d.fs.st[1] = (GfxStage){ (uint8_t)op, 1, (uint8_t)(5 | mod), 4, 1, 1, 1, 1, 5, 2, 0, 3 };
            d.tex[0] = t, d.tex[1] = cube;
            d.fs.specular_add = (uint8_t)(op & 1);
            d.fs.fog = (uint8_t)(op % 5);
            d.fs.alpha_func = (uint8_t)(op % 9);
            d.vs.ntex = 2;
            draw_ui(&d, v);
        }
    for (int lt = 1; lt <= 3; ++lt)
        for (int gen = 0; gen < 4; ++gen)
        {
            defaults(&d);
            float vert[3][8] = { { 0 } };
            d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
            d.u.stride[0] = 32, d.u.offset[3] = 12, d.u.offset[7] = 24;
            d.vs.lighting = 1, d.vs.specular = 1, d.vs.localviewer = (uint8_t)(gen & 1);
            d.vs.nlights = 3, d.vs.light_type[0] = (uint8_t)lt, d.vs.light_type[1] = 3, d.vs.light_type[2] = 1;
            d.vs.src_diffuse = (uint8_t)(gen % 3);
            d.vs.fog_vertex = (uint8_t)gen, d.vs.range_fog = (uint8_t)(lt & 1);
            d.vs.ntex = 2, d.vs.tci[0] = (uint8_t)(gen << 4), d.vs.ttf[0] = (uint8_t)(gen ? 3 : 2), d.vs.tci[1] = 0, d.vs.ttf[1] = 0x82;
            d.fs.nstages = 2;
            d.fs.st[1] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 3 };
            d.tex[0] = d.tex[1] = t;
            d.fs.st[0].tex = 1;
            d.fs.fog = (uint8_t)(gen ? 4 : 0);
            d.vs.flat = d.fs.flat = (uint8_t)(lt == 2);
            d.data[0] = vert, d.size[0] = sizeof vert;
            d.prim = GFX_TRIANGLEFAN, d.count = 1;
            gfx_draw(&d);
        }
    gfx_finish();
    gfx_tex_destroy(t);
    gfx_tex_destroy(cube);
    CHECK(gfx_failures() == before, "%u keys failed to build", gfx_failures() - before);
}

/* frames to a window: a back buffer the size of the window, cleared and drawn, presented */
static void run_window(SDL_Window* win)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    GfxTex* bb = gfx_tex_create(GFX_TEX_2D, 22, (uint32_t)ww, (uint32_t)wh, 1, GFX_USE_RT);
    uint32_t vp[6] = { 0, 0, (uint32_t)ww, (uint32_t)wh, 0, 0x3F800000u };
    gfx_set_targets(bb, 0, 0, NULL);
    Uint64 end = SDL_GetTicks() + 2000;
    for (int frame = 0; SDL_GetTicks() < end; ++frame)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            ;
        gfx_clear(0, NULL, 1, 0xFF203040u, 1.0f, 0, vp);
        GfxDraw d;
        defaults(&d);
        layout_ui(&d);
        d.u.vp[2] = (float)ww, d.u.vp[3] = (float)wh;
        memcpy(d.vp, vp, sizeof vp);
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
        float x = (float)(frame % 120) / 120.0f * (float)(ww - 64);
        UiVert v[4];
        quad(v, x, (float)wh / 2 - 32, x + 64, (float)wh / 2 + 32, 0xFFFFA000u, 0.5f);
        draw_ui(&d, v);
        gfx_present(bb);
    }
    gfx_tex_destroy(bb);
}

/* --- the scene effects (gfx_scene_done, fx on) ---------------------------------------------------------
 * A 128x128 scene through a 90-degree perspective camera, left- or right-handed (FFXI's is
 * right-handed: the scene is at negative z), made of flat-colored quads; each test turns on the one
 * effect it checks. */
enum { SS = 128 };
static GfxTex *g_srt, *g_sds;
static float g_sproj[16], g_sz;
static uint32_t g_spx[SS * SS];
static const uint32_t SVP[6] = { 0, 0, SS, SS, 0, 0x3F800000u };

static void fx_only(const char* on, float v)
{
    static const char* const all[] = { "ao", "fog", "bloom", "rays", "grade", "shadow", "sun" };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        gfx_fx_set(all[i], 0.0f);
    if (on)
        gfx_fx_set(on, v);
}

static void scene_begin(int rh, uint32_t clear)
{
    if (!g_srt)
        g_srt = gfx_tex_create(GFX_TEX_2D, 21, SS, SS, 1, GFX_USE_RT), g_sds = gfx_tex_create(GFX_TEX_2D, 75, SS, SS, 1, GFX_USE_DEPTH);
    gfx_set_targets(g_srt, 0, 0, g_sds);
    gfx_clear(0, NULL, 3, clear, 1.0f, 0, SVP);
    /* PerspectiveFovLH / RH: 90 degrees, square, near 0.5, far 100 */
    float zn = 0.5f, zf = 100.0f;
    g_sz = rh ? -1.0f : 1.0f;
    memset(g_sproj, 0, sizeof g_sproj);
    g_sproj[0] = g_sproj[5] = 1.0f, g_sproj[10] = g_sz * zf / (zf - zn), g_sproj[11] = g_sz, g_sproj[14] = -zn * zf / (zf - zn);
}

/* a quad (strip order) in view space, z given as a distance in front of the camera, of one color */
static void scene_quad(float q[4][3], uint32_t color)
{
    GfxDraw d;
    defaults(&d);
    memcpy(d.u.wvp, g_sproj, 64);
    identity(d.u.wv); /* the world matrix the identity, as a character's is */
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(TFACTOR) */
    d.u.tfactor[0] = ((color >> 16) & 255) / 255.0f, d.u.tfactor[1] = ((color >> 8) & 255) / 255.0f;
    d.u.tfactor[2] = (color & 255) / 255.0f, d.u.tfactor[3] = 1.0f;
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    float v[4][3];
    for (int i = 0; i < 4; ++i)
        v[i][0] = q[i][0], v[i][1] = q[i][1], v[i][2] = q[i][2] * g_sz;
    d.data[0] = v, d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

/* the scene done, with a directional light toward sun (view space, left-handed; NULL for none) and
 * a fog color; its pixels read back */
static void scene_end(const float* sun, uint32_t fog)
{
    GfxScene sc;
    memset(&sc, 0, sizeof sc);
    memcpy(sc.proj, g_sproj, 64);
    identity(sc.view);
    memcpy(sc.vp, SVP, sizeof SVP);
    if (sun)
    {
        float l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        sc.sun_dir[0] = sun[0] / l, sc.sun_dir[1] = sun[1] / l, sc.sun_dir[2] = sun[2] / l * g_sz, sc.sun_dir[3] = 1;
        sc.sun_color[0] = sc.sun_color[1] = sc.sun_color[2] = 1;
    }
    sc.fog[2] = 1.0f; /* the game fogged its world */
    sc.fogcolor[0] = ((fog >> 16) & 255) / 255.0f, sc.fogcolor[1] = ((fog >> 8) & 255) / 255.0f, sc.fogcolor[2] = (fog & 255) / 255.0f;
    gfx_scene_done(g_srt, &sc);
    gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
}

static uint32_t spx(int x, int y, int shift) { return (g_spx[y * SS + x] >> shift) & 255; }

/* a wall 6 ahead and a floor 3 below meeting it (at row 96) */
static void wall_and_floor(uint32_t color)
{
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -3, 6 }, { 8, -3, 6 } };
    float floor[4][3] = { { -8, -3, 6 }, { 8, -3, 6 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    scene_quad(wall, color);
    scene_quad(floor, color);
}

/* Occlusion darkens the wall where it meets the floor and leaves the open wall as it was: at each
 * quality (its taps a frame), by 20 levels or more in a single frame. */
static void test_scene_ao(int rh)
{
    for (int q = 0; q < 3; ++q)
    {
        fx_only("ao", 0.8f);
        gfx_fx_set("ao_quality", (float)q);
        scene_begin(rh, 0xFF000000u);
        wall_and_floor(0xFFFFFFFFu);
        scene_end(NULL, 0);
        uint32_t open = spx(64, 30, 0), corner = spx(64, 94, 0);
        CHECK(open >= 245, "occlusion (%s, quality %d): open wall %u (want about 255)", rh ? "RH" : "LH", q, open);
        CHECK(corner + 20 <= open, "occlusion (%s, quality %d): wall at the floor %u (want 20 darker than %u)", rh ? "RH" : "LH",
            q, corner, open);
    }
    gfx_fx_set("ao_quality", 0.0f);
}

/* A block floating 0.7 before a wall (inside the occlusion's reach, but far nearer the camera than
 * the wall) leaves no halo on it: it hides the wall, it does not shade it. */
static void test_scene_ao_halo(void)
{
    fx_only("ao", 1.0f);
    scene_begin(1, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    float block[4][3] = { { -0.5f, 0.5f, 5.3f }, { 0.5f, 0.5f, 5.3f }, { -0.5f, -0.5f, 5.3f }, { 0.5f, -0.5f, 5.3f } };
    scene_quad(wall, 0xFFFFFFFFu);
    scene_quad(block, 0xFFFFFFFFu);
    scene_end(NULL, 0);
    /* the block spans 64 +- 6 pixels; the wall 3 pixels past its edge */
    uint32_t beside = spx(73, 64, 0), below = spx(64, 73, 0);
    CHECK(beside >= 245 && below >= 245, "occlusion: wall around a floating block %u %u (want no halo)", beside, below);
}

/* Height fog: the far wall takes more of the fog's color than the floor just ahead; with height
 * falloff, the floor (below the camera) more than the wall's top at the same distance. */
static void test_scene_fog(void)
{
    fx_only("fog", 0.08f);
    gfx_fx_set("fog_falloff", 0.0f), gfx_fx_set("fog_max", 1.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    scene_end(NULL, 0xFFFF0000u);
    uint32_t far_g = spx(64, 40, 8), near_g = spx(64, 126, 8), far_r = spx(64, 40, 16);
    CHECK(far_r >= 250 && far_g <= 200, "fog: far wall %u/%u (want red)", far_r, far_g);
    CHECK(near_g >= far_g + 25, "fog: near floor green %u, far wall %u (want less fog near)", near_g, far_g);
    /* falloff: more fog below the camera than above it, at the wall */
    gfx_fx_set("fog_falloff", 0.5f), gfx_fx_set("fog_height", 0.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    scene_end(NULL, 0xFFFF0000u);
    uint32_t high = spx(64, 20, 8), low = spx(64, 90, 8);
    CHECK(low + 20 < high, "fog: below the camera %u, above %u (want more fog below)", low, high);
    /* from one frame to the next the fog's color eases: red, then blue is still mostly red */
    gfx_fx_set("fog_falloff", 0.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF000000u);
    scene_end(NULL, 0xFFFF0000u);
    gfx_present(NULL);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF000000u);
    scene_end(NULL, 0xFF0000FFu);
    uint32_t r = spx(64, 40, 16), b = spx(64, 40, 0);
    CHECK(r > 2 * b, "fog: a new color the next frame %u red, %u blue (want it eased: mostly red)", r, b);
    gfx_fx_set("fog_falloff", 0.08f), gfx_fx_set("fog_height", 2.0f), gfx_fx_set("fog_max", 0.5f);
}

/* Bloom: a bright square glows onto the dark wall around it. */
static void test_scene_bloom(void)
{
    fx_only("bloom", 1.0f);
    gfx_fx_set("threshold", 0.5f);
    scene_begin(1, 0xFF000000u);
    float lamp[4][3] = { { -0.5f, 0.5f, 5 }, { 0.5f, 0.5f, 5 }, { -0.5f, -0.5f, 5 }, { 0.5f, -0.5f, 5 } };
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    scene_quad(lamp, 0xFFFFFFFFu);
    scene_quad(wall, 0xFF101010u);
    scene_end(NULL, 0);
    /* the lamp spans 64 +- 6.4 pixels; 4 pixels past its edge */
    uint32_t glow = spx(75, 64, 0), far = spx(120, 120, 0);
    CHECK(glow >= 28, "bloom: next to the lamp %u (want a glow over 16)", glow);
    CHECK(far <= 24, "bloom: far corner %u (want the wall's 16)", far);
    gfx_fx_set("threshold", 0.75f);
}

/* God rays: with the sun behind the top of a wall (the sky above it bright), light spills down over
 * the wall below the skyline; with the sun behind the camera, none. */
static void test_scene_rays(void)
{
    float wall[4][3] = { { -8, 0, 6 }, { 8, 0, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    const float sun[3] = { 0, 0.1f, 1 }, behind[3] = { 0, 0.1f, -1 };
    uint32_t lit[2];
    for (int k = 0; k < 2; ++k)
    {
        fx_only("rays", 1.0f);
        scene_begin(1, 0xFFFFFFFFu); /* white sky */
        scene_quad(wall, 0xFF202020u);
        scene_end(k ? behind : sun, 0);
        lit[k] = spx(64, 72, 0); /* 8 rows below the skyline */
    }
    CHECK(lit[0] >= 60, "god rays: wall below the sun %u (want lit over its 32)", lit[0]);
    CHECK(lit[1] <= 40, "god rays: sun behind the camera %u (want the wall's 32)", lit[1]);
}

/* Sun shadows: a post standing on the floor, the sun beyond it and high: the floor before the post
 * (toward the camera) is in its shadow, the floor beside it is not; with the sun behind the camera,
 * or no sun, no shadow. */
static void test_scene_shadow(void)
{
    const float beyond[3] = { 0, 0.5f, 1 }, behind[3] = { 0, 0.5f, -1 };
    const float* suns[3] = { beyond, behind, NULL };
    for (int k = 0; k < 3; ++k)
    {
        fx_only("shadow", 1.0f);
        gfx_fx_set("shadow_length", 3.0f);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 20 }, { 8, -3, 20 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float post[4][3] = { { -1, 0, 6 }, { 1, 0, 6 }, { -1, -3, 6 }, { 1, -3, 6 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(suns[k], 0);
        /* the floor 5 ahead: row 102; before the post (x 0) and beside it (x 3) */
        uint32_t before = spx(64, 102, 0), beside = spx(102, 102, 0);
        CHECK(beside >= 245, "sun shadows (%d): open floor %u (want lit)", k, beside);
        if (k == 0)
            CHECK(before <= 160, "sun shadows: floor before the post %u (want shaded)", before);
        else
            CHECK(before >= 245, "sun shadows (%d): floor before the post %u (want lit: the sun is %s)", k, before,
                k == 1 ? "behind the camera" : "absent");
    }
    gfx_fx_set("shadow_length", 0.6f);
}

/* The sun's shadow map: a post 10 ahead, 3 high, the sun beyond it and up at about 27 degrees - its
 * shadow reaches 6 along the floor toward the camera, far past what contact shadows see; beside it
 * the floor is lit, and with the sun behind the camera the floor before it is lit too. */
static void test_scene_sun_map(void)
{
    const float beyond[3] = { 0, 0.5f, 1 }, behind[3] = { 0, 0.5f, -1 };
    for (int k = 0; k < 2; ++k)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(k ? behind : beyond, 0);
        /* the floor 7 ahead: row 91; before the post (x 0) and beside it (x 4) */
        uint32_t before = spx(64, 91, 0), beside = spx(100, 91, 0);
        CHECK(beside >= 245, "sun map (%d): open floor %u (want lit)", k, beside);
        if (k == 0)
            CHECK(before <= 160, "sun map: floor in the post's shadow %u (want shaded)", before);
        else
            CHECK(before >= 245, "sun map: sun behind the camera, floor before the post %u (want lit)", before);
    }
}

/* a card from buffer b (x -1..1, y -3..0 at z 0: strip order) placed at t (view space, right-handed)
 * by its own world matrix, as the zone's copies of one mesh are */
static void scene_card(GfxBuf* b, float tx, float ty, float tz)
{
    float w[16];
    identity(w);
    w[12] = tx, w[13] = ty, w[14] = tz;
    GfxDraw d;
    defaults(&d);
    memcpy(d.u.wv, w, 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            d.u.wvp[i * 4 + j] = w[i * 4] * g_sproj[j] + w[i * 4 + 1] * g_sproj[4 + j] + w[i * 4 + 2] * g_sproj[8 + j] + w[i * 4 + 3] * g_sproj[12 + j];
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    d.buf[0] = b, d.size[0] = 48;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

static void scene_floor(void)
{
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    scene_quad(floor, 0xFFFFFFFFu);
}

/* The zone out of view still casts, and copies of one mesh are told apart. Three cards from one
 * buffer, each placed by its own matrix: A a unit behind the camera, B and C 10 ahead. The sun is
 * behind the camera, so A's shadow falls forward onto the floor in view. Frame 1 draws all three,
 * frame 2 only C: A (out of view) must still cast, and B (in plain view, not drawn: it went) must
 * not. With one entry per buffer, C overwrites A and A's shadow is lost. */
static void test_scene_sun_cache(void)
{
    const float behind[3] = { 0, 0.5f, -1 };
    float card[4][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { -1, -3, 0 }, { 1, -3, 0 } };
    GfxBuf* b = gfx_buf_create(sizeof card);
    gfx_buf_upload(b, card, sizeof card);
    gfx_fx_set("sun_casters", 0.0f); /* the zone casts too */
    gfx_fx_set("temporal", 0.0f);
    for (int frame = 0; frame < 2; ++frame)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        scene_floor();
        if (frame == 0)
            scene_card(b, 0, 0, 1), scene_card(b, 0, 0, -10);
        scene_card(b, 4, 0, -10);
        scene_end(behind, 0);
        /* A's shadow: the floor 4 ahead (row 112); B's: 13 ahead (row 79), hidden behind B in frame 1 */
        uint32_t a_sh = spx(64, 112, 0), b_sh = spx(64, 79, 0), open = spx(20, 112, 0);
        CHECK(open >= 245, "sun cache (frame %d): open floor %u (want lit)", frame, open);
        CHECK(a_sh <= 160, "sun cache (frame %d): floor in the shadow of the card behind the camera %u (want shaded%s)", frame,
            a_sh, frame ? ", from the copy kept out of view" : "");
        if (frame)
            CHECK(b_sh >= 245, "sun cache: floor behind the card in view the game stopped drawing %u (want lit: it went)", b_sh);
        gfx_present(NULL);
    }
    /* characters alone cast (sun_casters 1, the default): the zone's cards, drawn again, do not */
    gfx_fx_set("sun_casters", 1.0f);
    fx_only("sun", 1.0f);
    scene_begin(1, 0xFF000000u);
    scene_floor();
    scene_card(b, 0, 0, 1);
    scene_end(behind, 0);
    CHECK(spx(64, 112, 0) >= 245, "sun casters 1: the floor in the zone's card's shadow %u (want lit: the zone does not cast)",
        spx(64, 112, 0));
    gfx_present(NULL);
    gfx_fx_set("temporal", 0.85f);
    gfx_buf_destroy(b);
}

/* Over time: a post (a character) stands for three frames, then steps sideways. Its shadow is at full
 * strength at once where it now falls and gone where it was: the history is held to what this frame
 * sees around each point (without that: faint at the new spot, a trail at the old). */
static void test_scene_temporal(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    uint32_t now = 0, was = 0;
    for (int frame = 0; frame < 4; ++frame)
    {
        fx_only("sun", 1.0f);
        gfx_fx_set("temporal", 0.85f);
        scene_begin(1, 0xFF000000u);
        scene_floor();
        float x = frame < 3 ? 0.0f : -3.0f;
        float post[4][3] = { { x - 1, 0, 10 }, { x + 1, 0, 10 }, { x - 1, -3, 10 }, { x + 1, -3, 10 } };
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(beyond, 0);
        /* the floor 7 ahead (row 91): under x -3 (column 37) and x 0 (column 64) */
        now = spx(37, 91, 0), was = spx(64, 91, 0);
        gfx_present(NULL);
    }
    CHECK(now <= 160, "temporal: the post's shadow where it stepped to %u (want shaded at once)", now);
    CHECK(was >= 245, "temporal: where the post's shadow was %u (want lit: no trail)", was);
}

/* Hard edges: with sun_soft 0 a post's shadow edge has at most 2 partly lit pixels along a row, fewer
 * than with the default softness. */
static void test_scene_sun_hard(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    int partial[2];
    for (int k = 0; k < 2; ++k)
    {
        fx_only("sun", 1.0f);
        gfx_fx_set("temporal", 0.0f);
        gfx_fx_set("sun_soft", k ? 0.0f : 0.03f);
        gfx_fx_set("sun_detail", k ? 1.0f : 0.0f);
        scene_begin(1, 0xFF000000u);
        scene_floor();
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(beyond, 0);
        partial[k] = 0;
        for (int x = 64; x < 100; ++x)
        {
            uint32_t v = spx(x, 91, 0);
            partial[k] += v > 70 && v < 245;
        }
        gfx_present(NULL);
    }
    CHECK(partial[1] <= 2 && partial[1] <= partial[0], "hard shadows: %d partly lit pixels at the edge (soft: %d; want 2 or fewer)",
        partial[1], partial[0]);
    gfx_fx_set("sun_soft", 0.03f), gfx_fx_set("sun_detail", 0.0f), gfx_fx_set("temporal", 0.85f);
}

/* Resolution: a pole 0.2 wide casts a thin shadow, dark in its middle and gone a few pixels beside
 * it (the near cascade's centimetre texels, no blur over it). */
static void test_scene_sun_sharp(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    fx_only("sun", 1.0f);
    gfx_fx_set("temporal", 0.0f);
    scene_begin(1, 0xFF000000u);
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    float pole[4][3] = { { -0.1f, 0, 10 }, { 0.1f, 0, 10 }, { -0.1f, -3, 10 }, { 0.1f, -3, 10 } };
    scene_quad(floor, 0xFFFFFFFFu);
    scene_quad(pole, 0xFFFFFFFFu);
    scene_end(beyond, 0);
    uint32_t mid = spx(64, 91, 0), side = spx(68, 91, 0);
    CHECK(mid <= 140, "sharp shadows: under a 0.2-wide pole %u (want dark)", mid);
    CHECK(side >= 235, "sharp shadows: 4 pixels beside it %u (want lit)", side);
    gfx_fx_set("temporal", 0.85f);
}

/* The scene filter: a 1024x1024 scene of 8-pixel stripes drawn at 32x32 is gray, where one
 * bilinear sample per pixel (every pixel lands on a white row) is white - the shimmer. */
static void test_scene_filter(void)
{
    enum { B = 1024 };
    GfxTex* rt = gfx_tex_create(GFX_TEX_2D, 21, B, B, 1, GFX_USE_RT);
    const uint32_t bvp[6] = { 0, 0, B, B, 0, 0x3F800000u };
    gfx_set_targets(rt, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xFF000000u, 1.0f, 0, bvp);
    static UiVert rows[B / 16][6];
    for (int i = 0; i < B / 16; ++i)
    {
        UiVert q[4];
        quad(q, 0, (float)(16 * i), B, (float)(16 * i + 8), 0xFFFFFFFFu, 0.5f);
        rows[i][0] = q[0], rows[i][1] = q[1], rows[i][2] = q[2], rows[i][3] = q[1], rows[i][4] = q[3], rows[i][5] = q[2];
    }
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.u.vp[2] = d.u.vp[3] = B;
    memcpy(d.vp, bvp, sizeof bvp);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    d.data[0] = rows, d.size[0] = sizeof rows;
    d.prim = GFX_TRIANGLELIST, d.count = 2 * (B / 16);
    gfx_draw(&d);
    GfxScene sc;
    memset(&sc, 0, sizeof sc); /* no camera: the filter alone */
    for (int pass = 0; pass < 2; ++pass)
    {
        gfx_fx_set("filter", (float)!pass);
        gfx_scene_done(rt, &sc);
        gfx_set_targets(g_rt, 0, 0, g_ds);
        defaults(&d);
        layout_ui(&d);
        d.tex[0] = rt;
        d.samp[0] = (GfxSampler){ 3, 3, 3, 2, 2, 0, 0, 0, 0 }; /* CLAMP, LINEAR, no mips: the game's */
        d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 };
        UiVert v[4];
        quad(v, 0, 0, W, H, 0xFFFFFFFFu, 0);
        for (int i = 0; i < 4; ++i) /* each screen pixel's center on a texel's center (row 32y + 16) */
            v[i].u += 0.5f / B, v[i].v += 0.5f / B;
        draw_ui(&d, v);
        readback();
        uint32_t c = px(16, 16) & 255;
        if (pass == 0)
            CHECK(c >= 100 && c <= 156, "scene filter: stripes at 1/32 size %u (want gray)", c);
        else
            CHECK(c >= 250, "scene filter off: %u (want the white row one sample finds)", c);
        gfx_set_targets(rt, 0, 0, NULL);
    }
    gfx_fx_set("filter", 1.0f);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    gfx_tex_destroy(rt);
}

/* A large render target the game samples itself (FFXI's character shadow, projected around the
 * player) with a mip filter reads its one level, not the empty chain the scene filter may add. */
static void test_large_target_mips(void)
{
    enum { B = 1024 };
    GfxTex* rt = gfx_tex_create(GFX_TEX_2D, 21, B, B, 1, GFX_USE_RT);
    const uint32_t bvp[6] = { 0, 0, B, B, 0, 0x3F800000u };
    gfx_set_targets(rt, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xFFFF0000u, 1.0f, 0, bvp);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.tex[0] = rt;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 2, 2, 2, 0, 0, 0 }; /* LINEAR, mips LINEAR */
    d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 };
    UiVert v[4];
    quad(v, 0, 0, W, H, 0xFFFFFFFFu, 0);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(16, 16), 0xFFFF0000u, 2), "large target through a mip filter: %08x (want its red)", px(16, 16));
    gfx_tex_destroy(rt);
}

static void test_scene_effects(void)
{
    test_large_target_mips();
    test_scene_ao(0);
    test_scene_ao(1);
    test_scene_ao_halo();
    test_scene_fog();
    test_scene_bloom();
    test_scene_rays();
    test_scene_shadow();
    test_scene_sun_map();
    test_scene_sun_cache();
    test_scene_temporal();
    test_scene_sun_hard();
    test_scene_sun_sharp();
    CHECK(gfx_failures() == 0, "scene effects: %u failures", gfx_failures());
    gfx_set_targets(g_rt, 0, 0, g_ds);
    test_scene_filter();
    gfx_tex_destroy(g_srt);
    gfx_tex_destroy(g_sds);
}

int main(int argc, char** argv)
{
    SDL_Window* win = NULL;
    if (argc > 1 && !strcmp(argv[1], "--window"))
    {
        if (!SDL_Init(SDL_INIT_VIDEO) || !(win = SDL_CreateWindow("gfx_test", 640, 360, 0)))
        {
            printf("no window: %s\n", SDL_GetError());
            return 1;
        }
    }
    gfx_set_sync_pipelines(1);
    setenv("FFXI_FX", "1", 1); /* the scene effects run only where a test asks (test_scene_effects) */
    setenv("FFXI_FX_FILE", "/nonexistent/fx.txt", 1); /* not the player's settings */
    if (!gfx_init(win, 1))
    {
        printf("no graphics back end\n");
        return 1;
    }
    g_rt = gfx_tex_create(GFX_TEX_2D, 21, W, H, 1, GFX_USE_RT);
    g_ds = gfx_tex_create(GFX_TEX_2D, 75, W, H, 1, GFX_USE_DEPTH);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    test_clear();
    test_rhw_edges();
    test_texture();
    test_alpha();
    test_depth();
    test_lighting();
    test_pixel_lighting();
    test_fog();
    test_shaders();
    test_sweep();
    test_scene_effects();
    gfx_present(NULL);
    if (win)
    {
        run_window(win);
        CHECK(gfx_failures() == 0, "window frames: %u failures", gfx_failures());
        SDL_DestroyWindow(win);
    }
    printf(g_fails ? "gfx_test: %d failed\n" : "gfx_test: ok\n", g_fails);
    return g_fails != 0;
}
