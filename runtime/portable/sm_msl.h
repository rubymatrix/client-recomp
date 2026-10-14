/* Direct3D 9 shader bytecode (vs/ps 2.0, 2.x, 3.0) to Metal Shading Language.
 *
 * One call translates one shader into one MSL source with one entry point (vmain or pmain). The
 * interface between the two stages and the host:
 *
 *   vertex inputs   v# is [[attribute(#)]] in a stage_in struct; SmInfo.in says which usage and
 *                   index each v# was declared with, for matching against a vertex declaration.
 *                   Vertex buffers are Metal buffers 0-15.
 *   stage outputs   matched by D3D semantic: POSITION0 is [[position]], PSIZE [[point_size]], every
 *                   other (usage, index) a float4 [[user(<usage><index>)]] (sm_semantic). A pixel
 *                   shader reads the same names; SM2's t#/v# inputs and oT#/oD# outputs are
 *                   TEXCOORD#/COLOR#. A pixel shader may read a semantic the vertex shader does not
 *                   write: translate the vertex shader again with it in SmOpts.vs_extra (written 0).
 *   constants       buffer SM_BUF_FLOAT: float4[256] (vs) / [224] (ps); SM_BUF_INT: int4[16];
 *                   SM_BUF_BOOL: uint[16]; SM_BUF_AUX: SmAux (the half-pixel offset, alpha ref,
 *                   sampler LOD bias, user clip planes).
 *   textures        sampler s# is [[texture(#)]] and [[sampler(#)]] (vertex texture fetch too).
 *
 * D3D9 behaviours folded in: the half-pixel offset of vertex positions (SmAux.half_pixel, from
 * the viewport), rsq/log/pow on |x| (pow(x, 0) = 1), cmp/cnd/lrp/sincos/nrm semantics, sampler
 * types from dcl, alpha test as an epilogue (SmOpts.alpha_func), depth-compare sampling for
 * samplers bound to depth textures (SmOpts.shadow_mask: the hardware shadow-map lookup D3D9
 * drivers do), sampler LOD bias and user clip planes (SmAux).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SM_BUF_FLOAT 16
#define SM_BUF_INT 17
#define SM_BUF_BOOL 18
#define SM_BUF_AUX 19

/* D3DDECLUSAGE */
enum
{
    SM_POSITION,
    SM_BLENDWEIGHT,
    SM_BLENDINDICES,
    SM_NORMAL,
    SM_PSIZE,
    SM_TEXCOORD,
    SM_TANGENT,
    SM_BINORMAL,
    SM_TESSFACTOR,
    SM_POSITIONT,
    SM_COLOR,
    SM_FOG,
    SM_DEPTH,
    SM_SAMPLE,
};

typedef struct SmDecl
{
    uint8_t usage, index, reg, used;
} SmDecl;

typedef struct SmInfo
{
    int pixel; /* 1 for a pixel shader */
    int major, minor;
    SmDecl in[16];  /* vertex: v# by register; pixel: what it reads, by semantic */
    unsigned nin;
    SmDecl out[16]; /* vertex: what it writes, by semantic (POSITION0 included) */
    unsigned nout;
    uint8_t sampler[16]; /* 0 unused, 2 2D, 3 cube, 4 volume (D3DSAMPLER_TEXTURE_TYPE) */
    uint8_t color_outputs; /* pixel: oC# written, as a bit mask */
    uint8_t writes_depth;
    uint32_t hash;         /* FNV-1a of the tokens */
} SmInfo;

typedef struct SmOpts
{
    uint8_t alpha_func;   /* pixel: D3DCMPFUNC of the alpha test, 0 or 8 (ALWAYS) when off */
    uint16_t shadow_mask; /* pixel: samplers bound to depth textures (sampled with comparison) */
    SmDecl vs_extra[16];  /* vertex: semantics to write as 0 (what a pixel shader reads beyond them) */
    unsigned nvs_extra;
    uint8_t attr_int[16]; /* vertex: v# fed from a non-normalized integer format: 1 signed, 2 unsigned
                           * (Metal passes those as integers; the shader sees them as floats); 3 UDEC3
                           * and 4 DEC3N, fetched as one uint and unpacked to (x, y, z, 1): Metal has no
                           * unnormalized 10:10:10 format, and its normalized one gives w from bits 30-31 */
    uint16_t depth_mask;  /* pixel: samplers bound to depth textures read as values (INTZ, DF24) */
    uint8_t no_depth_out; /* pixel: no depth buffer is bound, so oDepth goes nowhere (D3D9 drops it;
                           * Metal refuses a pipeline whose shader writes depth with no attachment) */
} SmOpts;

/* The auxiliary values both stages read (buffer SM_BUF_AUX). */
typedef struct SmAux
{
    float half_pixel[4]; /* 1/width, -1/height of the viewport: D3D9's pixel centres */
    float alpha_ref[4];  /* x: D3DRS_ALPHAREF / 255 */
    float lod_bias[16];  /* pixel: D3DSAMP_MIPMAPLODBIAS per sampler (Metal samplers have no bias) */
    float clip[6][4];    /* vertex: the user clip planes, in clip space as D3D9 takes them with shaders */
    uint32_t clip_on[4]; /* x: D3DRS_CLIPPLANEENABLE */
    float rscale[4];     /* pixel: the game's pixels per target pixel (a target made smaller than the
                          * game thinks: upscaling), for vPos */
} SmAux;

/* Parses the declarations only (what SmInfo describes); 0 if the tokens are not a shader this
 * translator takes. */
int sm_info(const uint32_t* tokens, size_t ntokens, SmInfo* info);
/* The whole translation into out (NUL-terminated). Returns the length, or 0 with a reason in err. */
size_t sm_translate(const uint32_t* tokens, size_t ntokens, const SmOpts* opts, SmInfo* info, char* out, size_t cap,
    char* err, size_t errcap);
/* "texcoord3": the MSL user() name of a semantic. */
const char* sm_semantic(unsigned usage, unsigned index, char buf[24]);
/* The byte length of a shader from its version token to END, or 0. */
size_t sm_length(const uint32_t* tokens, size_t max_tokens);
