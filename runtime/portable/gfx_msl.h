/* MSL generation for the Metal back end (gfx_msl.c, gfx_msl_shaders.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct Sb
{
    char* s;
    size_t len, cap;
} Sb;

void sb_printf(Sb* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/* The source of one library holding vs_main and fs_main for a key pair, or NULL when a shader
 * uses something the translator does not know (malloc'd; the caller frees). */
char* gfx_msl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* vs.1.0/1.1 and ps.1.0-1.4 token streams: the function bodies (after the signature, before the
 * fragment tail, which reads r0). 0 when the shader cannot be translated. */
int gfx_msl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* tokens);
/* The vertex function's parameters after the streams, and its end (the shadow pass's matrix). */
void gfx_msl_vs_params(Sb* b, const GfxVsKey* k);
void gfx_msl_vs_return(Sb* b, const GfxVsKey* k);
int gfx_msl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* tokens);
