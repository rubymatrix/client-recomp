/* The graphics back end under the Direct3D 9 front end (d3d9.c): Metal on macOS (gfx9_metal.m).
 *
 * d3d9.c keeps every D3D9 semantic - COM objects, device state, what a lock returns - and hands
 * this layer resolved work: textures to create and fill, the render targets, clears, and draws
 * with everything a draw reads (shaders, vertex layout, buffers, constants, textures, samplers and
 * the render states that shape the pipeline). Shaders are D3D9 bytecode; the back end translates
 * them (sm_msl.c) per pipeline variant and caches the result.
 *
 * Formats are D3DFORMAT values, vertex element types D3DDECLTYPE, states D3D9's own enums. */
#pragma once

#include <stdint.h>

typedef struct G9Tex G9Tex;
typedef struct G9Buf G9Buf;
typedef struct G9Shader G9Shader;

enum { G9_TEX_2D, G9_TEX_CUBE, G9_TEX_3D };
enum { G9_USE_SAMPLE, G9_USE_RT, G9_USE_DEPTH };

#define G9_MAX_RT 4
#define G9_MAX_STREAMS 16
#define G9_MAX_ELEMS 16
#define G9_PS_SAMPLERS 16
#define G9_VS_SAMPLERS 4
#define G9_SAMPLERS (G9_PS_SAMPLERS + G9_VS_SAMPLERS) /* pixel 0-15, then D3DVERTEXTEXTURESAMPLER0-3 */
#define G9_VS_FLOATS 256
#define G9_PS_FLOATS 224

/* --- device ------------------------------------------------------------------------------------ */

/* Brings Metal up on an SDL window. 0 on failure. */
int g9_init(void* sdl_window, int vsync);
/* The window's drawable size changed (Reset). */
void g9_resize(uint32_t w, uint32_t h);
/* Whether a D3DFORMAT can be a texture (use G9_USE_SAMPLE), render target or depth buffer. */
int g9_format_ok(uint32_t d3dfmt, int use);

/* --- resources --------------------------------------------------------------------------------- */

G9Tex* g9_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, int use);
void g9_tex_destroy(G9Tex* t);
/* A box of a level, D3D's layout: pitch bytes per row (per block row for DXT), slice bytes per
 * depth slice. face: cube face 0-5. */
void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
    uint32_t d, const void* src, uint32_t pitch, uint32_t slice);
/* A whole level (2D or one face) back into D3D's layout; waits for the GPU. */
void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
void g9_tex_gen_mips(G9Tex* t);
/* StretchRect: a rectangle of one surface into a rectangle of another, scaled with a point or
 * linear filter (rects: left, top, right, bottom). Depth to depth copies too (same size). */
void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t srect[4], G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t drect[4], int linear);
/* ColorFill: a rectangle of a surface (NULL: all of it) to a D3DCOLOR. */
void g9_fill(G9Tex* t, uint32_t face, uint32_t level, const int32_t rect[4], uint32_t color);

/* A buffer the game fills once and draws often (vertex or index data): kept on the GPU. */
G9Buf* g9_buf_create(uint32_t size);
void g9_buf_destroy(G9Buf* b);
void g9_buf_upload(G9Buf* b, uint32_t offset, const void* data, uint32_t size);

G9Shader* g9_shader_create(const uint32_t* tokens, uint32_t ntokens);
void g9_shader_destroy(G9Shader* s);

/* --- drawing ----------------------------------------------------------------------------------- */

typedef struct G9Viewport
{
    uint32_t x, y, w, h;
    float minz, maxz;
} G9Viewport;

void g9_set_targets(G9Tex* const color[G9_MAX_RT], const uint8_t face[G9_MAX_RT], const uint8_t level[G9_MAX_RT],
    G9Tex* depth);
/* Clear(Count, pRects (left, top, right, bottom), Flags, Color, Z, Stencil) inside the viewport
 * and, with scissor test on, the scissor rectangle. */
void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const G9Viewport* vp, const int32_t* scissor);

typedef struct G9Elem
{
    uint8_t stream, type, usage, index; /* D3DVERTEXELEMENT9 (method is always DEFAULT here) */
    uint16_t offset, pad;
} G9Elem;

typedef struct G9Sampler
{
    uint8_t addr_u, addr_v, addr_w; /* D3DTEXTUREADDRESS */
    uint8_t mag, min, mip;          /* D3DTEXTUREFILTERTYPE */
    uint8_t max_aniso, max_level;
    uint8_t srgb, pad[3];
    uint32_t border;
    float lod_bias;
} G9Sampler;

typedef struct G9Stream
{
    G9Buf* buf;        /* the GPU copy, or NULL: data holds the bytes this draw uses */
    const void* data;  /* ... from byte 0 of the stream's vertex 0 */
    uint32_t offset;   /* buf: the byte offset of vertex 0 */
    uint32_t size;     /* data: bytes */
    uint32_t stride;
    uint32_t freq;     /* D3DSTREAMSOURCE_INSTANCEDATA | n per instance, or 0 per vertex */
} G9Stream;

/* The render states that shape a draw (D3DRENDERSTATETYPE values). */
typedef struct G9States
{
    uint8_t zenable, zwrite, zfunc, stencil, two_sided;
    uint8_t sfail, szfail, spass, sfunc, ccw_sfail, ccw_szfail, ccw_spass, ccw_sfunc;
    uint8_t sread, swrite;
    uint8_t blend, src, dst, op, sep_alpha, src_a, dst_a, op_a;
    uint8_t write_mask[G9_MAX_RT];
    uint8_t alpha_func, cull, fill, scissor, srgb_write, pad[3];
    uint32_t stencil_ref, blend_factor, alpha_ref;
    float depth_bias, slope_bias;
} G9States;

typedef struct G9Draw
{
    G9Shader* vs;
    G9Shader* ps;
    G9Elem el[G9_MAX_ELEMS];
    uint32_t nel;
    G9Stream stream[G9_MAX_STREAMS];
    /* indices: from ibuf at ibuf_offset, or idata; index_size 0 for a non-indexed draw */
    G9Buf* ibuf;
    const void* idata;
    uint32_t ibuf_offset, index_size;
    uint32_t prim;        /* D3DPRIMITIVETYPE */
    uint32_t count;       /* primitives */
    uint32_t first;       /* the first index (indexed) or vertex */
    int32_t base_vertex;  /* added to every index */
    uint32_t instances;   /* 1 unless instancing */
    const float* vsc;     /* G9_VS_FLOATS float4s */
    const int32_t* vsi;   /* 16 int4s */
    const uint32_t* vsb;  /* 16 */
    const float* psc;     /* G9_PS_FLOATS float4s */
    const int32_t* psi;
    const uint32_t* psb;
    G9Tex* tex[G9_SAMPLERS];
    G9Sampler samp[G9_SAMPLERS];
    G9States rs;
    G9Viewport vp;
    int32_t scissor[4];   /* left, top, right, bottom */
} G9Draw;

void g9_draw(const G9Draw* d);
/* The frame is done: the back buffer goes to the window. */
void g9_present(G9Tex* backbuffer);
/* Waits for the GPU. */
void g9_finish(void);
