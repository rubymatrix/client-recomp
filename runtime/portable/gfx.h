/* The graphics back end under the D3D8 front end (d3d8.c): Metal on macOS (gfx_metal.m), nothing
 * elsewhere yet (gfx_null.c).
 *
 * d3d8.c keeps every D3D8 semantic - objects, state, state blocks, what a lock returns - and hands
 * this layer resolved work: textures to create and fill, the render targets, clears, and draw
 * packets. A draw packet is the fixed-function (or shader) state reduced to a key that selects
 * the generated shaders and pipeline (GfxVsKey, GfxFsKey, GfxPipeKey), the values those shaders
 * read (GfxU), and the vertex and index bytes the draw touches.
 *
 * Formats are D3DFORMAT values; the back end maps them. Matrices are D3D's: row vectors, row-major
 * storage, v * M. */
#pragma once

#include <stdint.h>

typedef struct GfxTex GfxTex;
typedef struct GfxBuf GfxBuf;

enum
{
    GFX_TEX_2D,
    GFX_TEX_CUBE,
};

enum
{
    GFX_USE_SAMPLE,
    GFX_USE_RT,    /* a render target that can also be sampled */
    GFX_USE_DEPTH, /* a depth-stencil surface */
};

/* D3DPRIMITIVETYPE */
enum
{
    GFX_POINTLIST = 1,
    GFX_LINELIST,
    GFX_LINESTRIP,
    GFX_TRIANGLELIST,
    GFX_TRIANGLESTRIP,
    GFX_TRIANGLEFAN,
};

/* vertex element types: D3DVSDT */
enum
{
    GFX_FLOAT1,
    GFX_FLOAT2,
    GFX_FLOAT3,
    GFX_FLOAT4,
    GFX_D3DCOLOR,
    GFX_UBYTE4,
    GFX_SHORT2,
    GFX_SHORT4,
};

/* the fixed-function input registers (D3DVSDE), also the v# of a vertex shader */
enum
{
    GFX_R_POSITION,
    GFX_R_BLENDWEIGHT,
    GFX_R_BLENDINDICES,
    GFX_R_NORMAL,
    GFX_R_PSIZE,
    GFX_R_DIFFUSE,
    GFX_R_SPECULAR,
    GFX_R_TEXCOORD0, /* .. 14 */
    GFX_R_POSITION2 = 15,
    GFX_R_NORMAL2,
    GFX_NREGS,
};

#define GFX_NSTREAMS 4
#define GFX_NLIGHTS 8
#define GFX_NVSC 96
#define GFX_NPSC 8

typedef struct GfxElem
{
    uint8_t used, stream, type, pad;
} GfxElem;

/* What selects a vertex function. Zeroed, then filled: the bytes are hashed and compared. */
typedef struct GfxVsKey
{
    uint32_t prog;     /* 0: fixed function; else a vertex shader (GfxDraw.vs_tokens), this is its hash */
    GfxElem el[GFX_NREGS];
    uint8_t rhw;       /* XYZRHW positions: already transformed */
    uint8_t lighting, normalize, localviewer, specular;
    uint8_t src_diffuse, src_specular, src_ambient, src_emissive; /* D3DMCS: 0 material, 1 color1, 2 color2 */
    uint8_t nlights, light_type[GFX_NLIGHTS];                    /* D3DLIGHTTYPE of each enabled light */
    uint8_t fog_vertex; /* D3DFOGMODE computed per vertex (FOGVERTEXMODE with no table fog), 0 none */
    uint8_t range_fog;
    uint8_t ntex;       /* texture coordinate outputs */
    uint8_t tci[8];     /* D3DTSS_TEXCOORDINDEX: index | generation mode << 4 */
    uint8_t ttf[8];     /* D3DTSS_TEXTURETRANSFORMFLAGS: count | 0x80 projected */
    uint8_t flat;
    uint8_t pixel;      /* the lighting per pixel rather than per vertex (the back end sets it: the
                         * scene effects' light setting); the vertex function passes the normal on */
    uint8_t shadow;     /* drawn again from the sun (the back end's shadow map): the position the
                         * function makes goes through the matrix in buffer 5 */
    uint8_t pad[1];
} GfxVsKey;

typedef struct GfxStage
{
    uint8_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, result;
    uint8_t tex;       /* 0 none bound, 1 2D, 2 cube */
    uint8_t projected; /* divide the coordinates by their last component */
    uint8_t ncoord;    /* components the vertex function writes for this stage: 2, 3 or 4 */
} GfxStage;

typedef struct GfxFsKey
{
    uint32_t prog; /* 0: the texture stages; else a pixel shader (GfxDraw.ps_tokens), this is its hash */
    GfxStage st[8];
    uint8_t nstages;
    uint8_t alpha_func; /* D3DCMPFUNC, 0 or 8 when the test is off */
    uint8_t fog;        /* D3DFOGMODE: pixel (table) fog; 4 = the vertex function's fog factor */
    uint8_t specular_add;
    uint8_t flat;
    uint8_t pad[3];
} GfxFsKey;

/* The render pipeline beyond the functions: blending and the color write mask. */
typedef struct GfxPipeKey
{
    uint8_t blend, src, dst, op; /* D3DBLEND, D3DBLENDOP */
    uint8_t write_mask;          /* D3DCOLORWRITEENABLE */
    uint8_t pad[3];
} GfxPipeKey;

typedef struct GfxDepthKey
{
    uint8_t zenable, zwrite, zfunc, stencil;
    uint8_t sfail, szfail, spass, sfunc;
    uint8_t sread, swrite, pad[2];
} GfxDepthKey;

typedef struct GfxLight
{
    float diffuse[4], specular[4], ambient[4];
    float pos[4];  /* camera space; w = range */
    float dir[4];  /* camera space, toward the light for directional lights; w = falloff */
    float att[4];  /* attenuation 0, 1, 2 */
    float spot[4]; /* cos(theta / 2), cos(phi / 2) */
} GfxLight;

/* The values the generated functions read: one buffer for both stages. Layout is mirrored in
 * gfx_msl.c's MSL struct; keep them together. */
typedef struct GfxU
{
    float wvp[16], wv[16], wvit[16];
    float texm[8][16];
    float mat_d[4], mat_a[4], mat_s[4], mat_e[4];
    float params[4];  /* material power, alpha ref (0..255), fog start, fog end */
    float ambient[4]; /* D3DRS_AMBIENT */
    float tfactor[4];
    float fogcolor[4];
    float params2[4]; /* fog density */
    float vp[4];      /* viewport x, y, width, height: the XYZRHW mapping */
    int32_t vofs, pad0[3];
    int32_t stride[GFX_NSTREAMS];
    int32_t offset[20]; /* per input register */
    GfxLight light[GFX_NLIGHTS];
    float vsc[GFX_NVSC][4];
    float psc[GFX_NPSC][4];
} GfxU;

typedef struct GfxSampler
{
    uint8_t addr_u, addr_v, addr_w; /* D3DTEXTUREADDRESS */
    uint8_t mag, min, mip;          /* D3DTEXTUREFILTERTYPE */
    uint8_t max_aniso, max_level;
    uint8_t lod_cap, pad[3]; /* 0, or the coarsest mip sampled + 1 (texture packs: glyph sheets whose
                              * small mips blend neighbouring glyphs into each one's edges) */
    uint32_t border;
} GfxSampler;

typedef struct GfxDraw
{
    GfxVsKey vs;
    GfxFsKey fs;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    const uint32_t* vs_tokens; /* the shader behind vs.prog / fs.prog */
    const uint32_t* ps_tokens;
    GfxU u;
    GfxTex* tex[8];
    GfxSampler samp[8];
    uint8_t cull;   /* D3DCULL */
    uint8_t caster; /* an opaque draw of the frame's 3D scene: it casts the sun's shadow */
    uint8_t fill;   /* D3DFILLMODE */
    int32_t zbias;  /* D3DRS_ZBIAS */
    uint32_t stencil_ref;
    uint32_t vp[6]; /* D3DVIEWPORT8 (MinZ, MaxZ as float bits) */
    int32_t scissor[4]; /* x, y, width, height in the target's pixels; width 0: none (the addon
                         * overlay's clip rectangles; D3D8 itself has no scissor) */
    /* vertices: stream s holds vertex (first + i) at data[s] + i * stride - copied per draw - or,
     * for a static buffer, at buf_off[s] + i * stride in buf[s] */
    const void* data[GFX_NSTREAMS];
    GfxBuf* buf[GFX_NSTREAMS];
    uint32_t buf_off[GFX_NSTREAMS];
    uint32_t size[GFX_NSTREAMS];
    /* the primitive: indices (16 or 32 bit) or vertices from vertex_start */
    uint32_t prim, count; /* D3D primitive type and primitive count */
    const void* indices;
    uint32_t index_size;
    GfxBuf* ibuf;      /* the indices are also in this static buffer, at ibuf_off (4-byte aligned) */
    uint32_t ibuf_off;
    uint32_t vertex_start;
} GfxDraw;

/* Brings the device up on an SDL window. 0 on failure (nothing is drawn, nothing fails). */
int gfx_init(void* sdl_window, int vsync);
/* A window of this many pixels: the back buffer's new size after Reset. */
void gfx_resize(uint32_t w, uint32_t h);

/* A vertex or index buffer the game fills once and draws many times: kept on the GPU, uploaded
 * whole when it changes (into a fresh buffer when the GPU may still read the old contents). */
GfxBuf* gfx_buf_create(uint32_t size);
void gfx_buf_destroy(GfxBuf* b);
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size);

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use);
void gfx_tex_destroy(GfxTex* t);
/* A whole level from D3D's layout (pitch bytes per row, or per block row for DXT). */
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch);
/* A rectangle of a level (4x4 aligned for DXT); src points at its first texel. */
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch);
/* A whole level back into D3D's layout (waits for the GPU). */
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* The same without waiting: the newest copy of the level the GPU has finished (1-3 frames old),
 * and a new copy queued behind this frame's work. Only the first read of a level waits. For
 * read-only locks the game polls every frame (FFXI's occlusion probe). */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* A rectangle between textures of one format. */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
    GfxTex* dst, uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy);

/* The render target (a level of a texture, or a cube face) and depth-stencil surface draws go to. */
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth);
/* Clear(Count, pRects, Flags, Color, Z, Stencil) within the viewport (D3DVIEWPORT8). */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t vp[6]);
void gfx_draw(const GfxDraw* d);
/* The camera and light of a frame's 3D scene, as the scene effects need them. */
typedef struct GfxScene
{
    float proj[16];      /* D3DTS_PROJECTION: a perspective one */
    float view[16];      /* D3DTS_VIEW */
    float sun_dir[4];    /* camera space, toward the light; w = 1 when the scene had a directional light */
    float sun_color[4];  /* its diffuse color */
    float ambient[4];    /* D3DRS_AMBIENT */
    float fogcolor[4];
    float fog[4];        /* fog start, end; z = 1 when fog was on */
    uint32_t vp[6];      /* the viewport the 3D draws used (D3DVIEWPORT8) */
} GfxScene;

/* The frame's 3D scene is finished in color (a render target's first level, drawn with depth
 * testing), before anything samples it or draws the interface over it. The back end's scene
 * effects - ambient occlusion from the depth it was drawn with, color grading - run on it in place
 * (FFXI_FX=1); back ends without them do nothing. */
void gfx_scene_done(GfxTex* color, const GfxScene* s);
/* One scene-effect setting by its key in the settings file (fx, ao, fog, bloom, rays, ...). */
void gfx_fx_set(const char* key, float v);
/* Its value now (the settings file reloads while the game runs); 0 for a key it does not know. */
float gfx_fx_get(const char* key);
/* The scene effects' last frames (what the shadows were drawn from), one line each, to a file. */
void gfx_trace_dump(const char* path);

/* The frame is done: the back buffer goes to the window. */
void gfx_present(GfxTex* backbuffer);
/* Waits for the GPU (tests). */
void gfx_finish(void);
/* Tests: build each pipeline in place, before its first draw, and keep no pipeline cache file. */
void gfx_set_sync_pipelines(int on);
/* Shaders or pipelines that failed to build so far (tests). */
uint32_t gfx_failures(void);

/* Frame profile (FFXI_PROFILE=1): the front end adds the time it spends turning a D3D call into
 * back-end work; the back end logs the breakdown every two seconds from Present. */
extern int gfx_profiling;
uint64_t gfx_now_ns(void);
void gfx_prof_front(uint64_t ns);
/* A draw dropped, and why (the profile line counts each reason). */
enum
{
    GFX_SKIP_NO_SHADER,
    GFX_SKIP_STREAM,
    GFX_SKIP_NO_POSITION,
    GFX_SKIP_NO_BUFFER,
    GFX_SKIP_RANGE,
    GFX_SKIP_NO_INDICES,
    GFX_SKIP_PIPELINE,
    GFX_SKIP_NO_TARGET,
    GFX_NSKIPS,
};
void gfx_prof_skip(int reason);
/* Time in a shim call (thunk_timer): counted for the thread that presents, the game's. */
void gfx_prof_shim(uint64_t ns);
