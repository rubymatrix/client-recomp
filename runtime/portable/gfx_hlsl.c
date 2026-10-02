/* HLSL (Shader Model 5.1) for a draw's keys (gfx.h): the Direct3D 12 back end's counterpart of
 * gfx_msl.c, with the same fixed-function pipeline - transform, lighting, fog, texture coordinate
 * generation and transforms, the texture stage cascade, alpha test - as one vertex and one pixel
 * function per key pair, and vs.1.x / ps.1.x shaders translated (gfx_hlsl_shaders.c). Pure C: the
 * D3D12 side (gfx_d3d12.c) compiles the text and caches the result by key.
 *
 * Conventions the functions follow (the root signature is gfx_d3d12.c's):
 *   - matrices are D3D's bytes in a column_major float4x4, so `mul(m, v)` is D3D's v * M;
 *   - D3D8 puts pixel centers on integer coordinates and D3D12 on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup"), as on Metal;
 *   - vertex fetch is by hand from the stream buffers (raw buffers t0..t3), stride and per-register
 *     offset in the uniforms, so one function serves any offsets and strides;
 *   - the uniforms are b0 in both stages; textures and samplers are indices into the descriptor
 *     heaps (b1), Texture2D in space1 and TextureCube in space2. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_hlsl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the HLSL struct U");

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
    "#define INFINITY asfloat(0x7F800000u)\n"
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
    "cbuffer CU : register(b0) { U u; };\n"
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "ByteAddressBuffer s0 : register(t0);\n"
    "ByteAddressBuffer s1 : register(t1);\n"
    "ByteAddressBuffer s2 : register(t2);\n"
    "ByteAddressBuffer s3 : register(t3);\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "TextureCube txc[] : register(t0, space2);\n"
    "SamplerState smp[] : register(s0);\n"
    "float4 ld_color(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }\n"
    "float4 ld_ubyte4(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24); }\n"
    "float4 ld_short2(ByteAddressBuffer b, int a) { int c = asint(b.Load(a)); return float4((c << 16) >> 16, c >> 16, 0, 1); }\n"
    "float4 ld_short4(ByteAddressBuffer b, int a) { int2 c = asint(b.Load2(a)); return float4((c.x << 16) >> 16, c.x >> 16, (c.y << 16) >> 16, c.y >> 16); }\n";

/* the vertex function's output: what the pixel function reads */
static void emit_vout(Sb* b, int ntex, int flat)
{
    const char* fl = flat ? "nointerpolation " : "";
    sb_printf(b, "struct VOut {\n  float4 pos : SV_Position;\n  %sfloat4 d : COLOR0;\n  %sfloat4 s : COLOR1;\n", fl, fl);
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  float4 t%d : TEXCOORD%d;\n", i, i);
    sb_printf(b, "  float fog : FOG;\n  float ez : EZ;\n};\n");
}

/* v# for an element: a float4 read from its stream (shared with gfx_hlsl_shaders.c's vs.1.x) */
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg);
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg);
        return;
    }
    sb_printf(b, "  int ad%d = vi * u.stride[%d] + u.offset[%d][%d];\n", reg, e->stream, reg >> 2, reg & 3);
    int s = e->stream;
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load(ad%d)), 0, 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT2: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load2(ad%d)), 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT3: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load3(ad%d)), 1);\n", reg, s, reg); break;
    case GFX_FLOAT4: sb_printf(b, "  float4 v%d = asfloat(s%d.Load4(ad%d));\n", reg, s, reg); break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = ld_color(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_UBYTE4: sb_printf(b, "  float4 v%d = ld_ubyte4(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT2: sb_printf(b, "  float4 v%d = ld_short2(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT4: sb_printf(b, "  float4 v%d = ld_short4(s%d, ad%d);\n", reg, s, reg); break;
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

static void emit_vs_signature(Sb* b)
{
    sb_printf(b, "VOut vs_main(uint vid : SV_VertexID) {\n  VOut o = (VOut)0;\n  int vi = int(vid) + u.vofs.x;\n");
}

/* clip-space position fixup: D3D8's pixel centers onto D3D12's */
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

static void emit_ff_vs(Sb* b, const GfxVsKey* k)
{
    emit_vs_signature(b);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        gfx_hlsl_fetch(b, k, r);
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
            "  o.pos = mul(u.wvp, P);\n"
            "  float3 pe = mul(u.wv, P).xyz;\n"
            "  o.ez = pe.z;\n");
    }
    emit_fixup(b);
    int need_normal = k->lighting || 0;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  float3 N = mul(u.wvit, float4(v3.xyz, 0.0)).xyz;\n");
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  float3 N = float3(0, 0, 1);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "float4(1, 1, 1, 1)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "float4(0, 0, 0, 0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  float4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        sb_printf(b, "  float3 amb = u.ambient.rgb, dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
        if (k->specular)
            sb_printf(b, "  float3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "float3(0, 0, -1)");
        for (int i = 0; i < k->nlights; ++i)
        {
            int t = k->light_type[i];
            sb_printf(b, "  {\n    Light L = u.light[%d];\n", i);
            if (t == 3)
                sb_printf(b, "    float3 l = L.dir.xyz;\n    float a = 1.0;\n");
            else
            {
                sb_printf(b,
                    "    float3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    float3 l = lv / max(d, 1e-20);\n"
                    "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
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
        sb_printf(b,
            "  o.d = saturate(float4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  o.s = saturate(float4(cs.rgb * spc, cs.a));\n");
    }
    else
        sb_printf(b, "  o.d = %s;\n  o.s = %s;\n", dif_in, spe_in);

    if (k->fog_vertex && !k->rhw)
    {
        sb_printf(b, "  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        emit_fog_factor(b, "o.fog", k->fog_vertex, "fd");
    }
    else
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "float4(1, 1, 1, 1)");

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
            sb_printf(b, "  c%d = mul(u.texm[%d], c%d);\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    sb_printf(b, "  return o;\n}\n");
}

/* --- pixel: the texture stage cascade --------------------------------------------------------------- */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "vin.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "vin.s"; break;
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
    case 12: snprintf(out, n, "lerp(%s, %s, vin.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "lerp(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "lerp(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "lerp(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "(float4)(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

/* texture slot i sampled at coord: the slot's heap indices are root constants */
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord);
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord)
{
    char c = "xyzw"[i & 3];
    sb_printf(b, "  %s = %s[ti[%d].%c].Sample(smp[si[%d].%c], %s);\n", dst, cube ? "txc" : "tx2", i >> 2, c, i >> 2, c, coord);
}

static void emit_fs_signature(Sb* b)
{
    sb_printf(b, "float4 fs_main(VOut vin) : SV_Target {\n");
}

static void emit_fs_tail(Sb* b, const GfxFsKey* k, const char* col)
{
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  clip(-1.0);\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(round(saturate(%s.a) * 255.0) %s u.params.y)) discard;\n", col, cmp[k->alpha_func]);
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(vin.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(vin.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        sb_printf(b, "  %s.rgb = lerp(u.fogcolor.rgb, %s.rgb, f);\n", col, col);
    }
    sb_printf(b, "  return %s;\n}\n", col);
}

static void emit_ff_fs(Sb* b, const GfxFsKey* k)
{
    emit_fs_signature(b);
    sb_printf(b, "  float4 cur = vin.d, tmp = float4(0, 0, 0, 0), tex = float4(1, 1, 1, 1);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        char coord[64];
        if (s->tex == 1)
        {
            if (s->projected)
            {
                const char* w = s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y";
                snprintf(coord, sizeof coord, "vin.t%d.xy / vin.t%d.%s", i, i, w);
            }
            else
                snprintf(coord, sizeof coord, "vin.t%d.xy", i);
            gfx_hlsl_sample(b, "tex", i, 0, coord);
        }
        else if (s->tex == 2)
        {
            snprintf(coord, sizeof coord, "vin.t%d.xyz", i);
            gfx_hlsl_sample(b, "tex", i, 1, coord);
        }
        else
            sb_printf(b, "  tex = float4(1, 1, 1, 1);\n");
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
            sb_printf(b, "  { float3 c = saturate(%s.rgb); %s = float4(c, %s.a); }\n", ce, dst, i ? "cur" : "vin.d");
    }
    if (k->specular_add)
        sb_printf(b, "  cur.rgb = saturate(cur.rgb + vin.s.rgb);\n");
    emit_fs_tail(b, k, "cur");
}

char* gfx_hlsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    sb_printf(&b, "%s", PRELUDE);
    emit_vout(&b, vk->ntex, vk->flat);
    if (vk->prog)
    {
        if (!gfx_hlsl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    if (fk->prog)
    {
        emit_fs_signature(&b);
        if (!gfx_hlsl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk);
    return b.s;
fail:
    free(b.s);
    return NULL;
}

/* --- the device's own functions: the back buffer to the window, and the frame-rate overlay -------------- */
const char gfx_hlsl_util[] =
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "SamplerState smp[] : register(s0);\n"
    "struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "PO present_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "float4 present_fs(PO i) : SV_Target {\n"
    "  return float4(tx2[ti[0].x].Sample(smp[si[0].x], i.uv).rgb, 1.0);\n"
    "}\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "cbuffer OU : register(b0) { float4 rect; float scale; uint n; float2 size; uint4 text[8]; };\n"
    "static const uint FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "float4 overlay_vs(uint vid : SV_VertexID) : SV_Position {\n"
    "  float2 c = rect.xy + float2((vid & 1) ? rect.z : 0.0, (vid & 2) ? rect.w : 0.0);\n"
    "  return float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1);\n"
    "}\n"
    "float4 overlay_fs(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = (pos.xy - rect.xy) / scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n";
