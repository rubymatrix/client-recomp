/* Metal Shading Language for a draw's keys (gfx.h): D3D8's fixed-function pipeline - transform,
 * lighting, fog, texture coordinate generation and transforms, the texture stage cascade, alpha
 * test - as one vertex and one fragment function per key pair, and vs.1.x / ps.1.x shaders
 * translated (gfx_msl_shaders.c). Pure C: the Metal side (gfx_metal.m) compiles the text and caches
 * the result by key.
 *
 * Conventions the functions follow:
 *   - matrices are D3D's bytes in a float4x4, so `m * v` is D3D's v * M;
 *   - D3D puts pixel centers on integer coordinates and Metal on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup");
 *   - vertex fetch is by hand from the stream buffers (buffers 0..3, stride and per-register offset
 *     in the uniforms), so one function serves any offsets and strides;
 *   - the uniforms are buffer 4 in both stages. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_msl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the MSL struct U");

void sb_printf(Sb* b, const char* fmt, ...)
{
    va_list ap;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->cap ? vsnprintf(b->s + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room)
        {
            b->len += (size_t)n;
            return;
        }
        b->cap = b->cap ? b->cap * 2 : 8192;
        if (n >= 0 && b->cap < b->len + (size_t)n + 1)
            b->cap = b->len + (size_t)n + 1;
        b->s = (char*)realloc(b->s, b->cap);
    }
}

static const char PRELUDE[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Light { float4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
    "struct U {\n"
    "  float4x4 wvp, wv, wvit;\n"
    "  float4x4 texm[8];\n"
    "  float4 mat_d, mat_a, mat_s, mat_e, params, ambient, tfactor, fogcolor, params2, vp;\n"
    "  int4 vofs, stride;\n"
    "  int4 offset[5];\n"
    "  Light light[8];\n"
    "  float4 vsc[96];\n"
    "  float4 psc[8];\n"
    "};\n"
    "static inline int reg_offset(constant U& u, int r) { return u.offset[r >> 2][r & 3]; }\n"
    "static inline float4 ld_color(device const uchar* p) { uchar4 c = *(device const uchar4*)p; return float4(c.z, c.y, c.x, c.w) / 255.0; }\n";

/* lit per pixel: the vertex function passes on what the lighting starts from (the normal, the
 * position and the material colors), and the fragment function lights - the same equations, but
 * a highlight no longer depends on where the vertices fall. pixel 1: the directional lights (the
 * sun, the moon) per pixel, the point and spot lights per vertex as the game made them - its torches
 * flicker, and a pool of light per pixel pulses with them; pixel 2: all of them per pixel. */
static int pixel_lit(const GfxVsKey* k) { return k->pixel && k->lighting && !k->rhw && !k->flat && !k->prog; }
static int point_per_vertex(const GfxVsKey* k)
{
    if (k->pixel != 1)
        return 0;
    for (int i = 0; i < k->nlights; ++i)
        if (k->light_type[i] != 3)
            return 1;
    return 0;
}

/* the vertex function's output: what the fragment function reads */
static void emit_vout(Sb* b, const GfxVsKey* k)
{
    int ntex = k->ntex;
    const char* fl = k->flat ? " [[flat]]" : "";
    sb_printf(b, "struct VOut {\n  float4 pos [[position]];\n  float4 d [[user(d)]]%s;\n  float4 s [[user(s)]]%s;\n", fl, fl);
    if (pixel_lit(k))
        sb_printf(b, "  float3 n [[user(n)]];\n  float4 pe [[user(pe)]];\n  float4 md [[user(md)]];\n  float4 ma [[user(ma)]];\n"
                     "  float4 ms [[user(ms)]];\n  float4 me [[user(me)]];\n%s",
            point_per_vertex(k) ? "  float4 pa [[user(pa)]];\n  float4 pd [[user(pd)]];\n  float4 ps [[user(ps)]];\n" : "");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  float4 t%d [[user(t%d)]];\n", i, i);
    sb_printf(b, "  float fog [[user(fog)]];\n  float ez [[user(ez)]];\n  float psize [[point_size]];\n};\n");
}

/* v# for an element: a float4 read from its stream */
static void emit_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg);
        return;
    }
    sb_printf(b, "  device const uchar* p%d = s%d + vi * u.stride[%d] + reg_offset(u, %d);\n", reg, e->stream, e->stream, reg);
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], 0, 0, 1);\n", reg, reg); break;
    case GFX_FLOAT2:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], 0, 1);\n", reg, reg, reg);
        break;
    case GFX_FLOAT3:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], ((device const float*)p%d)[2], 1);\n",
            reg, reg, reg, reg);
        break;
    case GFX_FLOAT4:
        sb_printf(b, "  float4 v%d = float4(((device const float*)p%d)[0], ((device const float*)p%d)[1], ((device const float*)p%d)[2], ((device const float*)p%d)[3]);\n",
            reg, reg, reg, reg, reg);
        break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = ld_color(p%d);\n", reg, reg); break;
    case GFX_UBYTE4: sb_printf(b, "  float4 v%d = float4(*(device const uchar4*)p%d);\n", reg, reg); break;
    case GFX_SHORT2: sb_printf(b, "  float4 v%d = float4(float2(*(device const short2*)p%d), 0, 1);\n", reg, reg); break;
    case GFX_SHORT4: sb_printf(b, "  float4 v%d = float4(*(device const short4*)p%d);\n", reg, reg); break;
    default: sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg); break;
    }
}

static int elem_components(uint8_t type)
{
    switch (type)
    {
    case GFX_FLOAT1: return 1;
    case GFX_FLOAT2:
    case GFX_SHORT2: return 2;
    case GFX_FLOAT3: return 3;
    default: return 4;
    }
}

/* drawn from the sun: whatever clip-space position the function makes (the camera's, from the
 * transforms or a vertex shader's constants) goes on through the camera's inverse into the sun's
 * view - one matrix, so any draw of the scene can be drawn again into the shadow map */
void gfx_msl_vs_params(Sb* b, const GfxVsKey* k)
{
    if (k->shadow)
        sb_printf(b, ", constant float4x4& sm [[buffer(5)]]");
}

void gfx_msl_vs_return(Sb* b, const GfxVsKey* k)
{
    if (k->shadow) /* D3D's pixel-centre fixup undone (the map has pixels of its own), then on into the map */
        sb_printf(b, "  o.pos.x -= o.pos.w / u.vp.z;\n  o.pos.y += o.pos.w / u.vp.w;\n  o.pos = sm * o.pos;\n");
    sb_printf(b, "  return o;\n}\n");
}

static void emit_vs_signature(Sb* b, const GfxVsKey* k)
{
    sb_printf(b, "vertex VOut vs_main(uint vid [[vertex_id]], constant U& u [[buffer(4)]]");
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        sb_printf(b, ", device const uchar* s%d [[buffer(%d)]]", s, s);
    gfx_msl_vs_params(b, k);
    sb_printf(b, ") {\n  VOut o;\n  int vi = int(vid) + u.vofs.x;\n  o.psize = 1.0;\n");
}

/* clip-space position fixup: D3D's pixel centers onto Metal's */
static void emit_fixup(Sb* b)
{
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
}

/* a D3DMATERIALCOLORSOURCE: the vertex color when the vertex has it, else the material's */
static const char* mcs(const GfxVsKey* k, int src, const char* mat)
{
    return src == 1 && k->el[GFX_R_DIFFUSE].used ? "v5" : src == 2 && k->el[GFX_R_SPECULAR].used ? "v6" : mat;
}

static void emit_fog_factor(Sb* b, const char* dst, int mode, const char* dist)
{
    switch (mode)
    {
    case 1: sb_printf(b, "  %s = saturate(exp(-u.params2.x * %s));\n", dst, dist); break;
    case 2: sb_printf(b, "  %s = saturate(exp(-(u.params2.x * %s) * (u.params2.x * %s)));\n", dst, dist, dist); break;
    case 3: sb_printf(b, "  %s = saturate((u.params.w - %s) / (u.params.w - u.params.z));\n", dst, dist); break;
    default: sb_printf(b, "  %s = 1.0;\n", dst); break;
    }
}

/* D3D's lighting from N and pe (view space) and the material colors cd, ca, cs, ce in scope, into
 * lit_d and lit_s */
/* which: 0 every light; 1 the directional ones, with the rest's terms from the vertex function
 * (in.pa, pd, ps); 2 the rest alone, leaving amb, dif and spc for it to pass on */
static void emit_lighting(Sb* b, const GfxVsKey* k, int pixel, int which)
{
    if (which == 2)
        sb_printf(b, "  float3 amb = float3(0), dif = float3(0), spc = float3(0);\n");
    else if (which == 1)
        sb_printf(b, "  float3 amb = u.ambient.rgb + in.pa.rgb, dif = in.pd.rgb, spc = in.ps.rgb;\n");
    else
        sb_printf(b, "  float3 amb = u.ambient.rgb, dif = float3(0), spc = float3(0);\n");
    if (k->specular)
        sb_printf(b, "  float3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "float3(0, 0, -1)");
    for (int i = 0; i < k->nlights; ++i)
    {
        int t = k->light_type[i];
        if ((which == 1 && t != 3) || (which == 2 && t == 3))
            continue;
        sb_printf(b, "  {\n    constant Light& L = u.light[%d];\n", i);
        if (t == 3)
            sb_printf(b, "    float3 l = L.dir.xyz;\n    float a = 1.0;\n");
        else
        {
            sb_printf(b,
                "    float3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    float3 l = lv / max(d, 1e-20);\n"
                "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
            /* per pixel, D3D's hard edge at the range is a ring, and its attenuation near the light (far
             * over 1, where no vertex ever was) a white spot - both flash as the game's lights flicker
             * and move: the last quarter of the range fades, and the light is at most its colour */
            if (pixel)
                sb_printf(b, "    a = min(a, 1.0) * saturate((L.pos.w - d) / max(0.25 * L.pos.w, 1e-6));\n");
            if (t == 2)
                sb_printf(b,
                    "    float rho = dot(-l, L.dir.xyz);\n"
                    "    a *= rho > L.spot.x ? 1.0 : rho <= L.spot.y ? 0.0 : pow(saturate((rho - L.spot.y) / (L.spot.x - L.spot.y)), L.dir.w);\n");
        }
        sb_printf(b, "    amb += L.ambient.rgb * a;\n    float ndl = max(dot(N, l), 0.0);\n    dif += L.diffuse.rgb * (ndl * a);\n");
        if (k->specular)
            sb_printf(b, "    if (ndl > 0.0) spc += L.specular.rgb * (pow(max(dot(N, normalize(V + l)), 0.0), u.params.x) * a);\n");
        sb_printf(b, "  }\n");
    }
    if (which != 2)
        sb_printf(b,
            "  float4 lit_d = saturate(float4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  float4 lit_s = saturate(float4(cs.rgb * spc, cs.a));\n");
}

static void emit_ff_vs(Sb* b, const GfxVsKey* k)
{
    emit_vs_signature(b, k);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        emit_fetch(b, k, r);
    if (k->rhw)
    {
        sb_printf(b,
            "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n"
            "  float2 ndc = float2((v0.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (v0.y - u.vp.y) / u.vp.w * 2.0);\n"
            "  o.pos = float4(ndc * w, v0.z * w, w);\n"
            "  float3 pe = float3(0, 0, w);\n"
            "  o.ez = w;\n");
    }
    else
    {
        sb_printf(b,
            "  float4 P = float4(v0.xyz, 1.0);\n"
            "  o.pos = u.wvp * P;\n"
            "  float3 pe = (u.wv * P).xyz;\n"
            "  o.ez = pe.z;\n");
    }
    emit_fixup(b);
    int need_normal = k->lighting || 0;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  float3 N = (u.wvit * float4(v3.xyz, 0.0)).xyz;\n");
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  float3 N = float3(0, 0, 1);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "float4(1.0)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "float4(0.0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  float4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        if (pixel_lit(k)) /* the length of N too: without NORMALIZENORMALS a scaled one lights more */
        {
            sb_printf(b, "  o.n = N;\n  o.pe = float4(pe, length(N));\n  o.md = cd, o.ma = ca, o.ms = cs, o.me = ce;\n"
                         "  o.d = cd;\n  o.s = cs;\n");
            if (point_per_vertex(k))
            {
                sb_printf(b, "  {\n");
                emit_lighting(b, k, 0, 2);
                sb_printf(b, "  o.pa = float4(amb, 0), o.pd = float4(dif, 0), o.ps = float4(spc, 0);\n  }\n");
            }
        }
        else
        {
            emit_lighting(b, k, 0, 0);
            sb_printf(b, "  o.d = lit_d;\n  o.s = lit_s;\n");
        }
    }
    else
        sb_printf(b, "  o.d = %s;\n  o.s = %s;\n", dif_in, spe_in);

    if (k->fog_vertex && !k->rhw)
    {
        sb_printf(b, "  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        emit_fog_factor(b, "o.fog", k->fog_vertex, "fd");
    }
    else
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "float4(1.0)");

    for (int i = 0; i < k->ntex; ++i)
    {
        int idx = k->tci[i] & 7, gen = k->tci[i] >> 4, count = k->ttf[i] & 7;
        if (k->rhw)
            gen = 0;
        switch (gen)
        {
        case 1: sb_printf(b, "  float4 c%d = float4(N, 1.0);\n", i); break;
        case 2: sb_printf(b, "  float4 c%d = float4(pe, 1.0);\n", i); break;
        case 3: sb_printf(b, "  float4 c%d = float4(reflect(%s, N), 1.0);\n", i, k->localviewer ? "normalize(pe)" : "float3(0, 0, 1)"); break;
        default:
        {
            int reg = GFX_R_TEXCOORD0 + idx;
            if (!k->el[reg].used)
                sb_printf(b, "  float4 c%d = float4(0, 0, 0, 1);\n", i);
            else if (count)
            {
                /* D3D fills the component after the last one given with 1, which is how a 2D
                 * texture matrix carries its translation in the third row */
                int n = elem_components(k->el[reg].type);
                char y[16] = "1.0", z[16] = "1.0", w[16] = "1.0";
                if (n > 1)
                    snprintf(y, sizeof y, "v%d.y", reg);
                if (n > 2)
                    snprintf(z, sizeof z, "v%d.z", reg);
                if (n > 3)
                    snprintf(w, sizeof w, "v%d.w", reg);
                sb_printf(b, "  float4 c%d = float4(v%d.x, %s, %s, %s);\n", i, reg, y, z, w);
            }
            else
                sb_printf(b, "  float4 c%d = v%d;\n", i, reg);
            break;
        }
        }
        if (count && !k->rhw)
            sb_printf(b, "  c%d = u.texm[%d] * c%d;\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    gfx_msl_vs_return(b, k);
}

/* --- fragment: the texture stage cascade ------------------------------------------------------------ */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "in.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "in.s"; break;
    case 5: base = "tmp"; break;
    default: base = "cur"; break;
    }
    char v[64];
    if (a & 0x20)
        snprintf(v, sizeof v, "%s.aaaa", base);
    else
        snprintf(v, sizeof v, "%s", base);
    if (a & 0x10)
        snprintf(out, n, "(1.0 - %s)", v);
    else
        snprintf(out, n, "%s", v);
}

/* D3DTEXTUREOP on float4 arguments; the caller keeps .rgb or .a */
static void op_expr(char* out, size_t n, int op, const char* a1, const char* a2, const char* a0)
{
    switch (op)
    {
    case 2: snprintf(out, n, "%s", a1); break;
    case 3: snprintf(out, n, "%s", a2); break;
    case 4: snprintf(out, n, "(%s * %s)", a1, a2); break;
    case 5: snprintf(out, n, "(%s * %s * 2.0)", a1, a2); break;
    case 6: snprintf(out, n, "(%s * %s * 4.0)", a1, a2); break;
    case 7: snprintf(out, n, "(%s + %s)", a1, a2); break;
    case 8: snprintf(out, n, "(%s + %s - 0.5)", a1, a2); break;
    case 9: snprintf(out, n, "((%s + %s - 0.5) * 2.0)", a1, a2); break;
    case 10: snprintf(out, n, "(%s - %s)", a1, a2); break;
    case 11: snprintf(out, n, "(%s + %s - %s * %s)", a1, a2, a1, a2); break;
    case 12: snprintf(out, n, "mix(%s, %s, in.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "mix(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "mix(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "mix(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "float4(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

static void emit_fs_signature(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    int pix = pixel_lit(vk);
    sb_printf(b, "fragment float4 fs_main(VOut %s [[stage_in]], constant U& u [[buffer(4)]]", pix ? "vin" : "in");
    for (int i = 0; i < 8; ++i)
    {
        int t = k->prog || i < k->nstages ? k->st[i].tex : 0;
        if (t == 1)
            sb_printf(b, ", texture2d<float> tx%d [[texture(%d)]], sampler sp%d [[sampler(%d)]]", i, i, i, i);
        else if (t == 2)
            sb_printf(b, ", texturecube<float> tx%d [[texture(%d)]], sampler sp%d [[sampler(%d)]]", i, i, i, i);
    }
    sb_printf(b, ") {\n");
    if (pix) /* the colors the vertex function would have given, lit here */
    {
        sb_printf(b, "  VOut in = vin;\n  {\n  float3 N = in.n * (rsqrt(max(dot(in.n, in.n), 1e-20)) * %s);\n"
                     "  float3 pe = in.pe.xyz;\n  float4 cd = in.md, ca = in.ma, cs = in.ms, ce = in.me;\n",
            vk->normalize ? "1.0" : "in.pe.w");
        emit_lighting(b, vk, 1, point_per_vertex(vk) ? 1 : 0);
        sb_printf(b, "  in.d = lit_d, in.s = lit_s;\n  }\n");
    }
}

static void emit_fs_tail(Sb* b, const GfxFsKey* k, const char* col)
{
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  discard_fragment();\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(rint(saturate(%s.a) * 255.0) %s u.params.y)) discard_fragment();\n", col, cmp[k->alpha_func]);
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(in.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(in.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        sb_printf(b, "  %s.rgb = mix(u.fogcolor.rgb, %s.rgb, f);\n", col, col);
    }
    sb_printf(b, "  return %s;\n}\n", col);
}

static void emit_ff_fs(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    emit_fs_signature(b, k, vk);
    sb_printf(b, "  float4 cur = in.d, tmp = float4(0), tex = float4(1);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        if (s->tex == 1)
        {
            if (s->projected)
            {
                const char* w = s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y";
                sb_printf(b, "  tex = tx%d.sample(sp%d, in.t%d.xy / in.t%d.%s);\n", i, i, i, i, w);
            }
            else
                sb_printf(b, "  tex = tx%d.sample(sp%d, in.t%d.xy);\n", i, i, i);
        }
        else if (s->tex == 2)
            sb_printf(b, "  tex = tx%d.sample(sp%d, in.t%d.xyz);\n", i, i, i);
        else
            sb_printf(b, "  tex = float4(1);\n");
        char a1[64], a2[64], a0[64], ce[512], ae[512];
        arg_expr(a1, sizeof a1, s->ca1);
        arg_expr(a2, sizeof a2, s->ca2);
        arg_expr(a0, sizeof a0, s->ca0);
        op_expr(ce, sizeof ce, s->cop, a1, a2, a0);
        const char* dst = s->result == 5 ? "tmp" : "cur";
        if (s->cop == 24) /* DOTPRODUCT3 goes to every channel, alpha too */
        {
            sb_printf(b, "  %s = saturate(%s);\n", dst, ce);
            continue;
        }
        if (s->aop > 1)
        {
            arg_expr(a1, sizeof a1, s->aa1);
            arg_expr(a2, sizeof a2, s->aa2);
            arg_expr(a0, sizeof a0, s->aa0);
            op_expr(ae, sizeof ae, s->aop, a1, a2, a0);
            sb_printf(b, "  { float3 c = saturate(%s.rgb); float a = saturate(%s.a); %s = float4(c, a); }\n", ce, ae, dst);
        }
        else /* alpha disabled: the alpha carries on unchanged */
            sb_printf(b, "  { float3 c = saturate(%s.rgb); %s = float4(c, %s.a); }\n", ce, dst, i ? "cur" : "in.d");
    }
    if (k->specular_add)
        sb_printf(b, "  cur.rgb = saturate(cur.rgb + in.s.rgb);\n");
    emit_fs_tail(b, k, "cur");
}

char* gfx_msl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    sb_printf(&b, "%s", PRELUDE);
    emit_vout(&b, vk);
    if (vk->prog)
    {
        if (!gfx_msl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    if (fk->prog)
    {
        emit_fs_signature(&b, fk, vk);
        if (!gfx_msl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk, vk);
    return b.s;
fail:
    free(b.s);
    return NULL;
}
