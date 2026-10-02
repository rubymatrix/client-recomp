/* HLSL generation for the Direct3D 12 back end (gfx_hlsl.c, gfx_hlsl_shaders.c): the same keys and
 * token streams as gfx_msl.h, as Shader Model 5.1. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct Sb
{
    char* s;
    size_t len, cap;
} Sb;

void sb_printf(Sb* b, const char* fmt, ...);

/* The source holding vs_main and fs_main for a key pair, or NULL when a shader uses something the
 * translator does not know (malloc'd; the caller frees). */
char* gfx_hlsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* vs.1.0/1.1 and ps.1.0-1.3 token streams: the function bodies (after the signature, before the
 * fragment tail, which reads r0). 0 when the shader cannot be translated. */
int gfx_hlsl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* tokens);
int gfx_hlsl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* tokens);

/* The utility functions every device needs (present, the frame-rate overlay), one source. */
extern const char gfx_hlsl_util[];
