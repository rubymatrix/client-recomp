/* D3D8 shader token streams to HLSL: gfx_msl_shaders.c's translation for the Direct3D 12 back end -
 * vs.1.0/1.1 (the five programmable vertex shaders FFXiMain creates) and ps.1.0-1.3 (its one pixel
 * shader, ps.1.1), instruction by instruction onto float4 registers. What is not here returns 0,
 * and the draw is skipped with a log line naming the opcode.
 *
 * Token layout (D3D8): an instruction token holds the opcode in bits 0-15 and no length, so the
 * parameter count comes from the opcode. A parameter token holds the register number in bits 0-10
 * and the type in bits 28-30; a destination adds the write mask (16-19), the result modifier
 * (20-23: saturate) and the shift (24-27); a source adds the swizzle (16-23), the source modifier
 * (24-27) and, in a vertex shader, relative addressing (bit 13) through a0.x. */
#include <stdio.h>
#include <string.h>

#include "gfx_hlsl.h"

void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg);
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord);

enum
{
    R_TEMP,
    R_INPUT,
    R_CONST,
    R_ADDR, /* vs: a0; ps: t# */
    R_RASTOUT,
    R_ATTROUT,
    R_TEXCRDOUT,
};

static int nparams(uint32_t op)
{
    switch (op)
    {
    case 0: return 0;                                       /* nop */
    case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: return 2;
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17: return 3;
    case 20: case 21: case 22: case 23: case 24: return 3; /* m4x4 .. m3x2 */
    case 4: case 18: case 80: case 88: return 4;            /* mad, lrp, cnd, cmp */
    case 81: return 5;                                      /* def */
    case 64: case 65: case 66: return 1;                    /* texcoord, texkill, tex */
    case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 74: case 75: case 77: return 2;
    case 76: return 3;                                      /* texm3x3spec */
    default: return -1;
    }
}

static const char* const SWZ = "xyzw";

/* a source operand as a float4 expression */
static void src_expr(char* out, size_t n, uint32_t t, int ps)
{
    uint32_t type = (t >> 28) & 7, num = t & 0x7FF, mod = (t >> 24) & 0xF, sw = (t >> 16) & 0xFF;
    char reg[64];
    switch (type)
    {
    case R_TEMP: snprintf(reg, sizeof reg, "r%u", num); break;
    case R_INPUT: snprintf(reg, sizeof reg, "v%u", num); break;
    case R_CONST:
        if (!ps && (t & 0x2000))
            snprintf(reg, sizeof reg, "u.vsc[clamp(int(a0.x) + %u, 0, 95)]", num);
        else
            snprintf(reg, sizeof reg, ps ? "c%u" : "u.vsc[%u]", num);
        break;
    case R_ADDR: snprintf(reg, sizeof reg, ps ? "t%u" : "a0", num); break;
    default: snprintf(reg, sizeof reg, "float4(0, 0, 0, 0)"); break;
    }
    char v[128];
    if (sw != 0xE4)
        snprintf(v, sizeof v, "%s.%c%c%c%c", reg, SWZ[sw & 3], SWZ[(sw >> 2) & 3], SWZ[(sw >> 4) & 3], SWZ[(sw >> 6) & 3]);
    else
        snprintf(v, sizeof v, "%s", reg);
    switch (mod)
    {
    case 1: snprintf(out, n, "(-%s)", v); break;
    case 2: snprintf(out, n, "(%s - 0.5)", v); break;
    case 3: snprintf(out, n, "(-(%s - 0.5))", v); break;
    case 4: snprintf(out, n, "((%s - 0.5) * 2.0)", v); break;
    case 5: snprintf(out, n, "(-((%s - 0.5) * 2.0))", v); break;
    case 6: snprintf(out, n, "(1.0 - %s)", v); break;
    case 7: snprintf(out, n, "(%s * 2.0)", v); break;
    case 8: snprintf(out, n, "(-(%s * 2.0))", v); break;
    case 9: snprintf(out, n, "(%s / %s.z)", v, v); break;
    case 10: snprintf(out, n, "(%s / %s.w)", v, v); break;
    default: snprintf(out, n, "%s", v); break;
    }
}

/* a destination register name, 0 when it is not one we can write */
static int dst_name(char* out, size_t n, uint32_t t, int ps)
{
    uint32_t type = (t >> 28) & 7, num = t & 0x7FF;
    switch (type)
    {
    case R_TEMP: snprintf(out, n, "r%u", num); return 1;
    case R_ADDR: snprintf(out, n, ps ? "t%u" : "a0", num); return 1;
    case R_RASTOUT:
        if (ps)
            return 0;
        snprintf(out, n, num == 0 ? "oPos" : num == 1 ? "oFog" : "oPts");
        return 1;
    case R_ATTROUT: snprintf(out, n, "oD%u", num & 1); return !ps;
    case R_TEXCRDOUT: snprintf(out, n, "oT%u", num & 7); return !ps && num < 8;
    default: return 0;
    }
}

/* dst.mask = (value) with the result modifiers */
static void write_dst(Sb* b, uint32_t d, const char* value, int ps)
{
    char name[32];
    if (!dst_name(name, sizeof name, d, ps))
        return;
    uint32_t mask = (d >> 16) & 0xF, sat = (d >> 20) & 1, shift = (d >> 24) & 0xF;
    char v[1024];
    const char* scale = shift == 1 ? " * 2.0" : shift == 2 ? " * 4.0" : shift == 3 ? " * 8.0" : shift == 15 ? " * 0.5"
        : shift == 14 ? " * 0.25" : shift == 13 ? " * 0.125" : "";
    snprintf(v, sizeof v, "(%s)%s", value, scale);
    if (sat)
    {
        char w[1100];
        snprintf(w, sizeof w, "saturate(%s)", v);
        snprintf(v, sizeof v, "%s", w);
    }
    else if (ps) /* ps.1.x registers hold [-1, 1] (at least; D3D8 hardware: MaxPixelShaderValue 1) */
    {
        char w[1100];
        snprintf(w, sizeof w, "clamp(%s, -1.0, 1.0)", v);
        snprintf(v, sizeof v, "%s", w);
    }
    if (mask == 0xF || mask == 0)
        sb_printf(b, "  %s = float4(%s);\n", name, v);
    else
    {
        char m[5] = { 0 };
        int k = 0;
        for (int i = 0; i < 4; ++i)
            if (mask & (1u << i))
                m[k++] = SWZ[i];
        sb_printf(b, "  %s.%s = float4(%s).%s;\n", name, m, v, m);
    }
}

/* the arithmetic both shader types share; 0 if op is not one of them */
static int arith(Sb* b, uint32_t op, const uint32_t* p, int ps)
{
    char s0[160], s1[160], s2[160], e[800];
    if (nparams(op) >= 2)
        src_expr(s0, sizeof s0, p[1], ps);
    if (nparams(op) >= 3)
        src_expr(s1, sizeof s1, p[2], ps);
    if (nparams(op) >= 4)
        src_expr(s2, sizeof s2, p[3], ps);
    switch (op)
    {
    case 1: snprintf(e, sizeof e, "%s", s0); break;
    case 2: snprintf(e, sizeof e, "%s + %s", s0, s1); break;
    case 3: snprintf(e, sizeof e, "%s - %s", s0, s1); break;
    case 4: snprintf(e, sizeof e, "%s * %s + %s", s0, s1, s2); break;
    case 5: snprintf(e, sizeof e, "%s * %s", s0, s1); break;
    case 6: snprintf(e, sizeof e, "(float4)(%s.w == 0.0 ? INFINITY : 1.0 / %s.w)", s0, s0); break;
    case 7: snprintf(e, sizeof e, "(float4)(%s.w == 0.0 ? INFINITY : rsqrt(abs(%s.w)))", s0, s0); break;
    case 8: snprintf(e, sizeof e, "(float4)(dot(%s.xyz, %s.xyz))", s0, s1); break;
    case 9: snprintf(e, sizeof e, "(float4)(dot(%s, %s))", s0, s1); break;
    case 10: snprintf(e, sizeof e, "min(%s, %s)", s0, s1); break;
    case 11: snprintf(e, sizeof e, "max(%s, %s)", s0, s1); break;
    case 12: snprintf(e, sizeof e, "(float4)(%s < %s)", s0, s1); break;
    case 13: snprintf(e, sizeof e, "(float4)(%s >= %s)", s0, s1); break;
    case 14: case 78: snprintf(e, sizeof e, "(float4)(exp2(%s.w))", s0); break;
    case 15: case 79: snprintf(e, sizeof e, "(float4)(%s.w == 0.0 ? -INFINITY : log2(abs(%s.w)))", s0, s0); break;
    case 16:
        snprintf(e, sizeof e,
            "float4(1.0, max(%s.x, 0.0), (%s.x > 0.0 && %s.y > 0.0) ? pow(%s.y, clamp(%s.w, -127.9961, 127.9961)) : 0.0, 1.0)", s0, s0,
            s0, s0, s0);
        break;
    case 17: snprintf(e, sizeof e, "float4(1.0, %s.y * %s.y, %s.z, %s.w)", s0, s1, s0, s1); break;
    case 18: snprintf(e, sizeof e, "lerp(%s, %s, %s)", s2, s1, s0); break;
    case 19: snprintf(e, sizeof e, "frac(%s)", s0); break;
    case 80: snprintf(e, sizeof e, "(%s.w > 0.5 ? %s : %s)", s0, s1, s2); break; /* cnd: r0.a */
    case 88: snprintf(e, sizeof e, "(%s >= 0.0 ? %s : %s)", s0, s1, s2); break;
    default: return 0;
    }
    write_dst(b, p[0], e, ps);
    return 1;
}

/* m4x4 / m4x3 / m3x4 / m3x3 / m3x2: dot products against consecutive constant rows */
static int matrix_op(Sb* b, uint32_t op, const uint32_t* p)
{
    int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2;
    int three = op >= 22;
    char s0[160], name[32], row[160];
    src_expr(s0, sizeof s0, p[1], 0);
    if (!dst_name(name, sizeof name, p[0], 0))
        return 1;
    for (int i = 0; i < rows; ++i)
    {
        src_expr(row, sizeof row, p[2] + (uint32_t)i, 0);
        if (three)
            sb_printf(b, "  %s.%c = dot(%s.xyz, %s.xyz);\n", name, SWZ[i], s0, row);
        else
            sb_printf(b, "  %s.%c = dot(%s, %s);\n", name, SWZ[i], s0, row);
    }
    return 1;
}

int gfx_hlsl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* t)
{
    if (!t || (t[0] & 0xFFFF0000u) != 0xFFFE0000u)
        return 0;
    sb_printf(b, "VOut vs_main(uint vid : SV_VertexID) {\n  VOut o = (VOut)0;\n  int vi = int(vid) + u.vofs.x;\n");
    /* the inputs the declaration maps: v# is the declaration's register */
    for (int r = 0; r < GFX_NREGS; ++r)
        gfx_hlsl_fetch(b, k, r);
    sb_printf(b, "  float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, a0 = 0;\n"
                 "  float4 oPos = float4(0, 0, 0, 1), oFog = 1, oPts = 1, oD0 = 0, oD1 = 0;\n"
                 "  float4 oT0 = 0, oT1 = 0, oT2 = 0, oT3 = 0, oT4 = 0, oT5 = 0, oT6 = 0, oT7 = 0;\n");
    for (uint32_t i = 1; i < 65536;)
    {
        uint32_t tok = t[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu)
            break;
        if (op == 0xFFFE) /* comment */
        {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        int np = nparams(op);
        if (np < 0)
        {
            fprintf(stderr, "[recomp] gfx: vs opcode %u not translated\n", op);
            return 0;
        }
        const uint32_t* p = &t[i + 1];
        if (op >= 20 && op <= 24)
            matrix_op(b, op, p);
        else if (op == 81 || op == 0)
            ; /* def (vs.1.1 has none), nop */
        else if (!arith(b, op, p, 0))
        {
            fprintf(stderr, "[recomp] gfx: vs opcode %u not translated\n", op);
            return 0;
        }
        i += 1 + (uint32_t)np;
    }
    sb_printf(b, "  o.pos = oPos;\n");
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
    sb_printf(b, "  o.d = saturate(oD0);\n  o.s = saturate(oD1);\n  o.fog = oFog.x;\n  o.ez = oPos.w;\n");
    for (int i = 0; i < k->ntex; ++i)
        sb_printf(b, "  o.t%d = oT%d;\n", i, i);
    sb_printf(b, "  return o;\n}\n");
    return 1;
}

int gfx_hlsl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* t)
{
    if (!t || (t[0] & 0xFFFF0000u) != 0xFFFF0000u)
        return 0;
    uint32_t minor = t[0] & 0xFF;
    if (((t[0] >> 8) & 0xFF) != 1 || minor > 3)
    {
        fprintf(stderr, "[recomp] gfx: pixel shader version %08x not translated\n", t[0]);
        return 0;
    }
    sb_printf(b, "  float4 r0 = 0, r1 = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n  float4 v0 = vin.d, v1 = vin.s;\n");
    for (int c = 0; c < GFX_NPSC; ++c) /* def overrides these */
        sb_printf(b, "  float4 c%d = u.psc[%d];\n", c, c);
    for (uint32_t i = 1; i < 65536;)
    {
        uint32_t tok = t[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu)
            break;
        if (op == 0xFFFE)
        {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        int np = nparams(op);
        if (np < 0)
        {
            fprintf(stderr, "[recomp] gfx: ps opcode %u not translated\n", op);
            return 0;
        }
        const uint32_t* p = &t[i + 1];
        uint32_t n = p[0] & 0x7FF;
        char dst[8], coord[32];
        switch (op)
        {
        case 0: break;
        case 81: /* def c#, x, y, z, w */
            sb_printf(b, "  c%u = float4(asfloat(%uu), asfloat(%uu), asfloat(%uu), asfloat(%uu));\n", n, p[1], p[2], p[3], p[4]);
            break;
        case 66: /* tex t# */
            snprintf(dst, sizeof dst, "t%u", n);
            if (n < 8 && k->st[n].tex == 2)
                snprintf(coord, sizeof coord, "vin.t%u.xyz", n), gfx_hlsl_sample(b, dst, (int)n, 1, coord);
            else if (n < 8 && k->st[n].tex == 1)
                snprintf(coord, sizeof coord, "vin.t%u.xy", n), gfx_hlsl_sample(b, dst, (int)n, 0, coord);
            else
                sb_printf(b, "  t%u = float4(0, 0, 0, 1);\n", n);
            break;
        case 64: sb_printf(b, "  t%u = float4(saturate(vin.t%u.xyz), 1.0);\n", n, n); break; /* texcoord */
        case 65: sb_printf(b, "  if (any(vin.t%u.xyz < 0.0)) discard;\n", n); break;        /* texkill */
        default:
            if (!arith(b, op, p, 1))
            {
                fprintf(stderr, "[recomp] gfx: ps opcode %u not translated\n", op);
                return 0;
            }
        }
        i += 1 + (uint32_t)np;
    }
    return 1;
}
