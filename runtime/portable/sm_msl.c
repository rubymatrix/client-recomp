/* Direct3D 9 shader bytecode to Metal Shading Language. See sm_msl.h.
 *
 * The bytecode is parsed into instructions, then written out as straight-line MSL over register
 * arrays (r[], v[], o[]) that the Metal compiler scalarises: every D3D instruction becomes one
 * block computing a float4 and storing it through the destination's write mask. Flow control maps
 * onto MSL's (if/for/break); subroutines (call/callnz/label) are inlined at each call.
 *
 * Token layout (the D3D9 DDK, "Shader Code Tokens"):
 *   instruction  opcode 0-15, controls 16-23, length 24-27 (SM2+), predicated 28
 *   destination  register 0-10 and type 28-30 | 11-12 (<< 3), relative 13, write mask 16-19,
 *                modifiers 20-23 (saturate 1, partial precision 2, centroid 4), bit 31 set
 *   source       register and type as above, relative 13 (an address token follows),
 *                swizzle 16-23 (2 bits per component), modifier 24-27, bit 31 set
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sm_msl.h"

/* register types (D3DSHADER_PARAM_REGISTER_TYPE) */
enum
{
    R_TEMP = 0,
    R_INPUT = 1,
    R_CONST = 2,
    R_ADDR = 3, /* vertex: a0; pixel: t# (TEXTURE) */
    R_RASTOUT = 4,
    R_ATTROUT = 5,
    R_OUTPUT = 6, /* vertex 3.0: o#; earlier: oT# (TEXCRDOUT) */
    R_CONSTINT = 7,
    R_COLOROUT = 8,
    R_DEPTHOUT = 9,
    R_SAMPLER = 10,
    R_CONST2 = 11,
    R_CONST3 = 12,
    R_CONST4 = 13,
    R_CONSTBOOL = 14,
    R_LOOP = 15,
    R_TEMPFLOAT16 = 16,
    R_MISCTYPE = 17,
    R_LABEL = 18,
    R_PREDICATE = 19,
};

/* opcodes (D3DSHADER_INSTRUCTION_OPCODE_TYPE) */
enum
{
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL,
    OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS,
    OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81,
    OP_CMP = 88, OP_BEM = 89, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94,
    OP_TEXLDL = 95, OP_BREAKP = 96, OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

typedef struct Param
{
    uint32_t reg, type, mask, mod, swz;
    int rel;              /* relative addressing */
    uint32_t rel_type, rel_reg, rel_comp;
} Param;

typedef struct Ins
{
    uint32_t op, ctl, pred;
    Param dst, p, src[4];
    unsigned nsrc;
    int has_dst;
    const uint32_t* raw; /* the first token after the opcode */
    unsigned len;
} Ins;

#define MAX_INS 8192
#define MAX_DEF 256

typedef struct Ctx
{
    SmInfo* info;
    const SmOpts* opts;
    Ins* ins;
    unsigned nins;
    /* def / defi / defb */
    struct { uint32_t reg; float v[4]; } fdef[MAX_DEF];
    unsigned nfdef;
    struct { uint32_t reg; int32_t v[4]; } idef[16];
    unsigned nidef;
    struct { uint32_t reg, v; } bdef[16];
    unsigned nbdef;
    uint8_t out_used[16];       /* vs 3.0: o# declared */
    SmDecl out_decl[16];        /* vs 3.0: o# -> semantic */
    SmDecl in_decl[16];         /* v# -> semantic (both stages) */
    uint8_t in_used[16];
    uint8_t tex_used[8];        /* ps 2: t# read */
    uint8_t rel_const;          /* c[] read with relative addressing */
    uint8_t vpos, vface;
    /* output */
    char* out;
    size_t cap, len;
    int overflow;
    char* err;
    size_t errcap;
    int failed;
    int depth;  /* indentation */
    int loops;  /* nesting, for unique loop variables */
    int inline_depth;
} Ctx;

static void emit(Ctx* c, const char* fmt, ...)
{
    if (c->len >= c->cap)
    {
        c->overflow = 1;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->out + c->len, c->cap - c->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= c->cap - c->len)
    {
        c->overflow = 1;
        c->len = c->cap;
        return;
    }
    c->len += (size_t)n;
}

static void line(Ctx* c, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(c, "%*s%s\n", 4 * (c->depth + 1), "", buf);
}

static void fail(Ctx* c, const char* fmt, ...)
{
    if (c->failed)
        return;
    c->failed = 1;
    if (c->err && c->errcap)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errcap, fmt, ap);
        va_end(ap);
    }
}

const char* sm_semantic(unsigned usage, unsigned index, char buf[24])
{
    static const char* const names[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                         "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                         "sample" };
    snprintf(buf, 24, "%s%u", usage < 14 ? names[usage] : "unknown", index);
    return buf;
}

size_t sm_length(const uint32_t* t, size_t max)
{
    if (!max || ((t[0] >> 16) != 0xFFFE && (t[0] >> 16) != 0xFFFF))
        return 0;
    for (size_t i = 1; i < max;)
    {
        uint32_t tok = t[i];
        if (tok == 0x0000FFFFu)
            return 4 * (i + 1);
        if ((tok & 0xFFFF) == OP_COMMENT)
            i += 1 + ((tok >> 16) & 0x7FFF);
        else
            i += 1 + ((tok >> 24) & 0xF);
    }
    return 0;
}

/* --- parsing ------------------------------------------------------------------------------------ */

static uint32_t reg_type(uint32_t t) { return ((t >> 28) & 7u) | ((t >> 8) & 0x18u); }

static const uint32_t* parse_param(const uint32_t* p, const uint32_t* end, Param* out, int dst, int sm3)
{
    memset(out, 0, sizeof *out);
    if (p >= end)
        return NULL;
    uint32_t t = *p++;
    out->reg = t & 0x7FF;
    out->type = reg_type(t);
    out->rel = (t >> 13) & 1;
    if (dst)
    {
        out->mask = (t >> 16) & 0xF;
        out->mod = (t >> 20) & 0xF;
        out->swz = 0xE4;
    }
    else
    {
        out->swz = (t >> 16) & 0xFF;
        out->mod = (t >> 24) & 0xF;
        out->mask = 0xF;
    }
    if (out->rel)
    {
        if (sm3 || !dst)
        {
            if (p >= end)
                return NULL;
            uint32_t a = *p++;
            out->rel_type = reg_type(a);
            out->rel_reg = a & 0x7FF;
            out->rel_comp = (a >> 16) & 3; /* the replicated component */
        }
        else
            out->rel_type = R_ADDR;
    }
    return p;
}

static int op_has_dst(uint32_t op)
{
    switch (op)
    {
    case OP_NOP: case OP_CALL: case OP_CALLNZ: case OP_LOOP: case OP_RET: case OP_ENDLOOP: case OP_LABEL:
    case OP_REP: case OP_ENDREP: case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_BREAK:
    case OP_BREAKC: case OP_BREAKP: case OP_PHASE:
        return 0;
    default:
        return 1;
    }
}

static int parse(Ctx* c, const uint32_t* tok, size_t n)
{
    SmInfo* info = c->info;
    uint32_t ver = tok[0];
    info->pixel = (ver >> 16) == 0xFFFF;
    info->major = (ver >> 8) & 0xFF;
    info->minor = ver & 0xFF;
    if (info->major < 2 || info->major > 3)
    {
        fail(c, "shader model %u.%u is not supported", info->major, info->minor);
        return 0;
    }
    int sm3 = info->major >= 3;
    const uint32_t* end = tok + n;
    const uint32_t* p = tok + 1;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ tok[i]) * 16777619u;
    info->hash = h;
    while (p < end)
    {
        uint32_t t = *p++;
        uint32_t op = t & 0xFFFF;
        if (t == 0x0000FFFFu)
            return 1;
        if (op == OP_COMMENT)
        {
            p += (t >> 16) & 0x7FFF;
            continue;
        }
        unsigned len = (t >> 24) & 0xF;
        const uint32_t* next = p + len;
        if (next > end)
        {
            fail(c, "truncated instruction");
            return 0;
        }
        if (c->nins >= MAX_INS)
        {
            fail(c, "too many instructions");
            return 0;
        }
        Ins* in = &c->ins[c->nins];
        memset(in, 0, sizeof *in);
        in->op = op;
        in->ctl = (t >> 16) & 0xFF;
        in->pred = (t >> 28) & 1;
        in->raw = p;
        in->len = len;
        if (op == OP_DCL)
        {
            uint32_t d = p[0];
            const uint32_t* q = parse_param(p + 1, next, &in->dst, 1, sm3);
            if (!q)
                return fail(c, "bad dcl"), 0;
            uint32_t usage = d & 0x1F, index = (d >> 16) & 0xF;
            uint32_t r = in->dst.reg;
            if (in->dst.type == R_SAMPLER && r < 16)
                info->sampler[r] = (uint8_t)((d >> 27) & 0xF);
            else if (in->dst.type == R_INPUT && r < 16)
            {
                c->in_decl[r] = (SmDecl){ (uint8_t)usage, (uint8_t)index, (uint8_t)r, 1 };
                if (info->pixel && !sm3) /* ps 2: v# are colors */
                    c->in_decl[r] = (SmDecl){ SM_COLOR, (uint8_t)r, (uint8_t)r, 1 };
                c->in_used[r] = 1;
            }
            else if (in->dst.type == R_ADDR && info->pixel && r < 8) /* ps 2: t# */
                c->tex_used[r] = 1;
            else if (in->dst.type == R_OUTPUT && sm3 && !info->pixel && r < 16)
            {
                c->out_decl[r] = (SmDecl){ (uint8_t)usage, (uint8_t)index, (uint8_t)r, 1 };
                c->out_used[r] = 1;
            }
            else if (in->dst.type == R_MISCTYPE)
            {
                if (r == 0)
                    c->vpos = 1;
                else
                    c->vface = 1;
            }
            p = next;
            continue; /* not kept as an instruction */
        }
        if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB)
        {
            Param d;
            parse_param(p, next, &d, 1, sm3);
            if (op == OP_DEF && c->nfdef < MAX_DEF)
            {
                c->fdef[c->nfdef].reg = d.reg;
                memcpy(c->fdef[c->nfdef].v, p + 1, 16);
                c->nfdef++;
            }
            else if (op == OP_DEFI && c->nidef < 16)
            {
                c->idef[c->nidef].reg = d.reg;
                memcpy(c->idef[c->nidef].v, p + 1, 16);
                c->nidef++;
            }
            else if (op == OP_DEFB && c->nbdef < 16)
            {
                c->bdef[c->nbdef].reg = d.reg;
                c->bdef[c->nbdef].v = p[1];
                c->nbdef++;
            }
            p = next;
            continue;
        }
        const uint32_t* q = p;
        in->has_dst = op_has_dst(op);
        if (in->has_dst)
        {
            q = parse_param(q, next, &in->dst, 1, sm3);
            if (!q)
                return fail(c, "bad destination (op %u)", op), 0;
        }
        if (in->pred)
        {
            q = parse_param(q, next, &in->p, 0, sm3);
            if (!q)
                return fail(c, "bad predicate (op %u)", op), 0;
        }
        while (q && q < next && in->nsrc < 4)
            q = parse_param(q, next, &in->src[in->nsrc++], 0, sm3);
        if (!q)
            return fail(c, "bad source (op %u)", op), 0;
        /* registers read, for the interface */
        for (unsigned s = 0; s < in->nsrc; ++s)
        {
            Param* sp = &in->src[s];
            if (sp->type == R_CONST && sp->rel)
                c->rel_const = 1;
            if (info->pixel && !sm3 && sp->type == R_ADDR && sp->reg < 8)
                c->tex_used[sp->reg] = 1;
            if (info->pixel && !sm3 && sp->type == R_INPUT && sp->reg < 2 && !c->in_used[sp->reg])
            {
                c->in_decl[sp->reg] = (SmDecl){ SM_COLOR, (uint8_t)sp->reg, (uint8_t)sp->reg, 1 };
                c->in_used[sp->reg] = 1;
            }
            if (sp->type == R_MISCTYPE)
                (sp->reg ? (c->vface = 1) : (c->vpos = 1));
        }
        if (in->has_dst && info->pixel && in->dst.type == R_COLOROUT)
            info->color_outputs |= (uint8_t)(1u << (in->dst.reg & 3));
        if (in->has_dst && info->pixel && in->dst.type == R_DEPTHOUT)
            info->writes_depth = 1;
        c->nins++;
        p = next;
    }
    fail(c, "no END token");
    return 0;
}

/* --- expressions -------------------------------------------------------------------------------- */

static const char* comp(unsigned i)
{
    static const char* const c[4] = { "x", "y", "z", "w" };
    return c[i & 3];
}

static void swizzle_str(uint32_t swz, char out[6])
{
    out[0] = '.';
    for (int i = 0; i < 4; ++i)
        out[1 + i] = "xyzw"[(swz >> (2 * i)) & 3];
    out[5] = 0;
}

static int find_fdef(const Ctx* c, uint32_t reg)
{
    for (int i = (int)c->nfdef - 1; i >= 0; --i)
        if (c->fdef[i].reg == reg)
            return i;
    return -1;
}

/* The base register expression (no swizzle, no modifier): a float4, int4 or bool. */
static void reg_expr(Ctx* c, const Param* p, char* out, size_t cap)
{
    char rel[64] = "";
    if (p->rel)
    {
        if (p->rel_type == R_LOOP)
            snprintf(rel, sizeof rel, "aL + ");
        else
            snprintf(rel, sizeof rel, "a0.%s + ", comp(p->rel_comp));
    }
    switch (p->type)
    {
    case R_TEMP:
    case R_TEMPFLOAT16:
        snprintf(out, cap, "r[%u]", p->reg);
        break;
    case R_INPUT:
        if (p->rel)
            snprintf(out, cap, "v[clamp(%s%u, 0, 15)]", rel, p->reg);
        else
            snprintf(out, cap, "v[%u]", p->reg);
        break;
    case R_CONST:
    case R_CONST2:
    case R_CONST3:
    case R_CONST4:
    {
        uint32_t reg = p->reg + (p->type == R_CONST2 ? 2048 : p->type == R_CONST3 ? 4096 : p->type == R_CONST4 ? 6144 : 0);
        if (p->rel)
            snprintf(out, cap, "C(c, %s%u)", rel, reg);
        else
        {
            int d = find_fdef(c, reg);
            if (d >= 0)
                snprintf(out, cap, "float4(%.9g, %.9g, %.9g, %.9g)", c->fdef[d].v[0], c->fdef[d].v[1], c->fdef[d].v[2],
                    c->fdef[d].v[3]);
            else
                snprintf(out, cap, "c[%u]", reg);
        }
        break;
    }
    case R_ADDR:
        if (c->info->pixel)
            snprintf(out, cap, "tc[%u]", p->reg); /* ps 2: texture coordinates */
        else
            snprintf(out, cap, "float4(a0)");
        break;
    case R_CONSTINT:
        snprintf(out, cap, "float4(I(ci, %u))", p->reg);
        break;
    case R_CONSTBOOL:
        snprintf(out, cap, "B(cb, %u)", p->reg);
        break;
    case R_LOOP:
        snprintf(out, cap, "float4(aL)");
        break;
    case R_SAMPLER:
        snprintf(out, cap, "s%u", p->reg);
        break;
    case R_MISCTYPE:
        snprintf(out, cap, p->reg ? "vface" : "vpos");
        break;
    case R_PREDICATE:
        snprintf(out, cap, "float4(p0)");
        break;
    case R_OUTPUT:
        snprintf(out, cap, "o[%u]", p->reg);
        break;
    case R_COLOROUT:
        snprintf(out, cap, "oC[%u]", p->reg & 3);
        break;
    default:
        fail(c, "source register type %u", p->type);
        snprintf(out, cap, "float4(0)");
    }
}

/* A source operand as a float4 expression: register, swizzle, modifier. */
static void src_expr(Ctx* c, const Param* p, char* out, size_t cap)
{
    char base[160], sw[6];
    reg_expr(c, p, base, sizeof base);
    if (p->type == R_CONSTBOOL || p->type == R_SAMPLER)
    {
        snprintf(out, cap, "%s", base);
        return;
    }
    swizzle_str(p->swz, sw);
    char v[200];
    snprintf(v, sizeof v, "(%s)%s", base, p->swz == 0xE4 ? "" : sw);
    switch (p->mod)
    {
    case 0: snprintf(out, cap, "%s", v); break;
    case 1: snprintf(out, cap, "(-%s)", v); break;
    case 2: snprintf(out, cap, "(%s - 0.5)", v); break;
    case 3: snprintf(out, cap, "(-(%s - 0.5))", v); break;
    case 4: snprintf(out, cap, "(2.0 * %s - 1.0)", v); break;
    case 5: snprintf(out, cap, "(-(2.0 * %s - 1.0))", v); break;
    case 6: snprintf(out, cap, "(1.0 - %s)", v); break;
    case 7: snprintf(out, cap, "(2.0 * %s)", v); break;
    case 8: snprintf(out, cap, "(-2.0 * %s)", v); break;
    case 11: snprintf(out, cap, "abs(%s)", v); break;
    case 12: snprintf(out, cap, "(-abs(%s))", v); break;
    default:
        fail(c, "source modifier %u", p->mod);
        snprintf(out, cap, "%s", v);
    }
}

/* the scalar a replicate-swizzle source selects (its first component) */
static void src_scalar(Ctx* c, const Param* p, char* out, size_t cap)
{
    char e[256];
    src_expr(c, p, e, sizeof e);
    snprintf(out, cap, "(%s).x", e);
}

/* The destination register lvalue. */
static int dst_lvalue(Ctx* c, const Param* d, char* out, size_t cap)
{
    SmInfo* info = c->info;
    switch (d->type)
    {
    case R_TEMP:
    case R_TEMPFLOAT16:
        snprintf(out, cap, "r[%u]", d->reg);
        return 1;
    case R_ADDR:
        if (info->pixel)
            snprintf(out, cap, "tc[%u]", d->reg);
        else
            snprintf(out, cap, "a0f"); /* vs: mov a0 (rounded into a0 after the store) */
        return 1;
    case R_RASTOUT:
        snprintf(out, cap, d->reg == 0 ? "oPos" : d->reg == 1 ? "oFog" : "oPts");
        return 1;
    case R_ATTROUT:
        snprintf(out, cap, "oD[%u]", d->reg & 1);
        return 1;
    case R_OUTPUT:
        if (info->major >= 3)
        {
            if (d->rel)
                snprintf(out, cap, "o[clamp(aL + %u, 0, 15)]", d->reg);
            else
                snprintf(out, cap, "o[%u]", d->reg);
        }
        else
            snprintf(out, cap, "oT[%u]", d->reg & 7);
        return 1;
    case R_COLOROUT:
        snprintf(out, cap, "oC[%u]", d->reg & 3);
        return 1;
    case R_DEPTHOUT:
        snprintf(out, cap, "oDepth");
        return 1;
    case R_PREDICATE:
        snprintf(out, cap, "p0");
        return 1;
    default:
        fail(c, "destination register type %u", d->type);
        snprintf(out, cap, "r[0]");
        return 0;
    }
}

/* Stores float4 expression `val` through d's write mask (and saturate, and the predicate). */
static void store(Ctx* c, const Ins* in, const char* val)
{
    char lv[96];
    const Param* d = &in->dst;
    dst_lvalue(c, d, lv, sizeof lv);
    int scalar_out = d->type == R_DEPTHOUT || (d->type == R_RASTOUT && d->reg != 0);
    const char* sat = (d->mod & 1) ? "saturate" : "";
    if (d->type == R_PREDICATE)
    {
        line(c, "{ bool4 t = %s;", val);
        for (int k = 0; k < 4; ++k)
            if (d->mask & (1u << k))
                line(c, "  p0.%s = t.%s;", comp((unsigned)k), comp((unsigned)k));
        line(c, "}");
        return;
    }
    line(c, "{ float4 t = %s(%s);", sat, val);
    if (scalar_out)
        line(c, "  %s = t.x;", lv);
    else
        for (int k = 0; k < 4; ++k)
        {
            if (!(d->mask & (1u << k)))
                continue;
            if (in->pred)
            {
                char pe[160];
                reg_expr(c, &in->p, pe, sizeof pe);
                unsigned pc = (in->p.swz >> (2 * k)) & 3;
                line(c, "  if (%sp0.%s) %s.%s = t.%s;", in->p.mod == 13 ? "!" : "", comp(pc), lv, comp((unsigned)k),
                    comp((unsigned)k));
            }
            else
                line(c, "  %s.%s = t.%s;", lv, comp((unsigned)k), comp((unsigned)k));
        }
    if (d->type == R_ADDR && !c->info->pixel)
        line(c, "  a0 = int4(floor(a0f + 0.5));");
    line(c, "}");
}

static const char* cmp_op(uint32_t ctl)
{
    switch (ctl & 7)
    {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    default: return "!=";
    }
}

/* --- instructions ------------------------------------------------------------------------------- */

static void translate_range(Ctx* c, unsigned from, unsigned to);

static int label_start(Ctx* c, uint32_t label)
{
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_LABEL && c->ins[i].nsrc && c->ins[i].src[0].reg == label)
            return (int)i + 1;
    return -1;
}

static unsigned label_end(Ctx* c, unsigned start)
{
    for (unsigned i = start; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
            return i;
    return c->nins;
}

static void inline_call(Ctx* c, uint32_t label)
{
    int s = label_start(c, label);
    if (s < 0)
    {
        fail(c, "call to a missing label %u", label);
        return;
    }
    if (c->inline_depth >= 8)
    {
        fail(c, "subroutines nest too deep");
        return;
    }
    c->inline_depth++;
    line(c, "{ /* l%u */", label);
    c->depth++;
    translate_range(c, (unsigned)s, label_end(c, (unsigned)s));
    c->depth--;
    line(c, "}");
    c->inline_depth--;
}

static void texture_sample(Ctx* c, const Ins* in, const char* coord_expr, unsigned s, const char* extra_mode,
    const char* ddx, const char* ddy)
{
    unsigned type = c->info->sampler[s & 15];
    int shadow = c->opts && (c->opts->shadow_mask >> s) & 1;
    char v[1024];
    const char* cc = type == 3 || type == 4 ? ".xyz" : ".xy";
    int depth = c->opts && (c->opts->depth_mask >> s) & 1;
    if (depth && !shadow && type != 3 && type != 4)
    {
        /* a depth texture read for its values (INTZ): the depth in every component */
        if (extra_mode)
            snprintf(v, sizeof v, "float4(t%u.sample(s%u, (%s).xy, %s))", s, s, coord_expr, extra_mode);
        else
            snprintf(v, sizeof v, "float4(t%u.sample(s%u, (%s).xy))", s, s, coord_expr);
    }
    else if (shadow && type != 3 && type != 4)
    {
        /* a depth texture: the comparison D3D9 drivers make for shadow maps (ref = z) */
        snprintf(v, sizeof v, "float4(t%u.sample_compare(s%u, (%s).xy, saturate((%s).z)))", s, s, coord_expr, coord_expr);
    }
    else if (ddx)
        snprintf(v, sizeof v, "t%u.sample(s%u, (%s)%s, %s((%s)%s, (%s)%s))", s, s, coord_expr, cc,
            type == 3 ? "gradientcube" : type == 4 ? "gradient3d" : "gradient2d", ddx, cc, ddy, cc);
    else if (extra_mode)
        snprintf(v, sizeof v, "t%u.sample(s%u, (%s)%s, %s)", s, s, coord_expr, cc, extra_mode);
    else
        snprintf(v, sizeof v, "t%u.sample(s%u, (%s)%s)", s, s, coord_expr, cc);
    store(c, in, v);
}

static void one(Ctx* c, unsigned idx)
{
    const Ins* in = &c->ins[idx];
    char s0[512] = "", s1[512] = "", s2[512] = "", v[2048];
    if (in->nsrc > 0 && in->op != OP_LOOP && in->op != OP_REP && in->op != OP_CALL && in->op != OP_CALLNZ &&
        in->op != OP_LABEL && in->op != OP_IF && in->op != OP_TEX && in->op != OP_TEXLDL && in->op != OP_TEXLDD)
        src_expr(c, &in->src[0], s0, sizeof s0);
    if (in->nsrc > 1 && in->op != OP_TEX && in->op != OP_TEXLDL && in->op != OP_TEXLDD && in->op != OP_CALLNZ &&
        in->op != OP_LOOP)
        src_expr(c, &in->src[1], s1, sizeof s1);
    if (in->nsrc > 2 && in->op != OP_TEXLDD)
        src_expr(c, &in->src[2], s2, sizeof s2);

    switch (in->op)
    {
    case OP_NOP:
    case OP_PHASE:
        return;
    case OP_MOV:
        store(c, in, s0);
        return;
    case OP_MOVA:
    {
        char lv[64];
        snprintf(lv, sizeof lv, "%s", s0);
        line(c, "{ float4 t = %s;", lv);
        for (int k = 0; k < 4; ++k)
            if (in->dst.mask & (1u << k))
                line(c, "  a0.%s = int(floor(t.%s + 0.5));", comp((unsigned)k), comp((unsigned)k));
        line(c, "}");
        return;
    }
    case OP_ADD: snprintf(v, sizeof v, "%s + %s", s0, s1); break;
    case OP_SUB: snprintf(v, sizeof v, "%s - %s", s0, s1); break;
    case OP_MUL: snprintf(v, sizeof v, "%s * %s", s0, s1); break;
    case OP_MAD: snprintf(v, sizeof v, "%s * %s + %s", s0, s1, s2); break;
    case OP_MIN: snprintf(v, sizeof v, "min(%s, %s)", s0, s1); break;
    case OP_MAX: snprintf(v, sizeof v, "max(%s, %s)", s0, s1); break;
    case OP_SLT: snprintf(v, sizeof v, "float4(%s < %s)", s0, s1); break;
    case OP_SGE: snprintf(v, sizeof v, "float4(%s >= %s)", s0, s1); break;
    case OP_DP3: snprintf(v, sizeof v, "float4(dot((%s).xyz, (%s).xyz))", s0, s1); break;
    case OP_DP4: snprintf(v, sizeof v, "float4(dot(%s, %s))", s0, s1); break;
    case OP_RCP: snprintf(v, sizeof v, "float4(1.0 / (%s).x)", s0); break;
    case OP_RSQ: snprintf(v, sizeof v, "float4(rsqrt(abs((%s).x)))", s0); break;
    case OP_EXP:
    case OP_EXPP: snprintf(v, sizeof v, "float4(exp2((%s).x))", s0); break;
    case OP_LOG:
    case OP_LOGP: snprintf(v, sizeof v, "float4(log2(abs((%s).x)))", s0); break;
    case OP_POW: snprintf(v, sizeof v, "float4(powr(abs((%s).x), (%s).x))", s0, s1); break;
    case OP_FRC: snprintf(v, sizeof v, "fract(%s)", s0); break;
    case OP_ABS: snprintf(v, sizeof v, "abs(%s)", s0); break;
    case OP_SGN: snprintf(v, sizeof v, "sign(%s)", s0); break;
    case OP_NRM: snprintf(v, sizeof v, "Nrm(%s)", s0); break;
    case OP_CRS: snprintf(v, sizeof v, "float4(cross((%s).xyz, (%s).xyz), 0.0)", s0, s1); break;
    case OP_LRP: snprintf(v, sizeof v, "mix(%s, %s, %s)", s2, s1, s0); break;
    case OP_CMP: snprintf(v, sizeof v, "select(%s, %s, (%s) >= 0.0)", s2, s1, s0); break;
    case OP_CND: snprintf(v, sizeof v, "select(%s, %s, (%s) > 0.5)", s2, s1, s0); break;
    case OP_DP2ADD: snprintf(v, sizeof v, "float4(dot((%s).xy, (%s).xy) + (%s).x)", s0, s1, s2); break;
    case OP_DSX: snprintf(v, sizeof v, "dfdx(%s)", s0); break;
    case OP_DSY: snprintf(v, sizeof v, "dfdy(%s)", s0); break;
    case OP_SINCOS: snprintf(v, sizeof v, "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", s0, s0); break;
    case OP_LIT:
        snprintf(v, sizeof v, "Lit(%s)", s0);
        break;
    case OP_DST:
        snprintf(v, sizeof v, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", s0, s1, s0, s1);
        break;
    case OP_M4x4:
    case OP_M4x3:
    case OP_M3x4:
    case OP_M3x3:
    case OP_M3x2:
    {
        unsigned rows = in->op == OP_M4x4 || in->op == OP_M3x4 ? 4 : in->op == OP_M3x2 ? 2 : 3;
        int three = in->op == OP_M3x4 || in->op == OP_M3x3 || in->op == OP_M3x2;
        char rowe[4][256];
        for (unsigned r = 0; r < rows; ++r)
        {
            Param p = in->src[1];
            p.reg += r;
            src_expr(c, &p, rowe[r], sizeof rowe[r]);
        }
        int n = snprintf(v, sizeof v, "float4(");
        for (unsigned r = 0; r < 4; ++r)
        {
            if (r < rows)
                n += snprintf(v + n, sizeof v - (size_t)n, three ? "dot((%s).xyz, (%s).xyz)" : "dot(%s, %s)", s0, rowe[r]);
            else
                n += snprintf(v + n, sizeof v - (size_t)n, "0.0");
            n += snprintf(v + n, sizeof v - (size_t)n, r < 3 ? ", " : ")");
        }
        break;
    }
    case OP_SETP:
    {
        snprintf(v, sizeof v, "(%s) %s (%s)", s0, cmp_op(in->ctl), s1);
        store(c, in, v);
        return;
    }
    case OP_TEX:
    {
        char coord[512];
        src_expr(c, &in->src[0], coord, sizeof coord);
        unsigned s = in->src[1].reg;
        if (in->ctl == 1) /* texldp */
        {
            char pc[700];
            snprintf(pc, sizeof pc, "(%s) / (%s).w", coord, coord);
            texture_sample(c, in, pc, s, NULL, NULL, NULL);
        }
        else if (in->ctl == 2) /* texldb */
        {
            char b[600];
            snprintf(b, sizeof b, "bias((%s).w)", coord);
            texture_sample(c, in, coord, s, b, NULL, NULL);
        }
        else
            texture_sample(c, in, coord, s, NULL, NULL, NULL);
        return;
    }
    case OP_TEXLDL:
    {
        char coord[512], l[600];
        src_expr(c, &in->src[0], coord, sizeof coord);
        snprintf(l, sizeof l, "level((%s).w)", coord);
        texture_sample(c, in, coord, in->src[1].reg, l, NULL, NULL);
        return;
    }
    case OP_TEXLDD:
    {
        char coord[512], dx[512], dy[512];
        src_expr(c, &in->src[0], coord, sizeof coord);
        src_expr(c, &in->src[2], dx, sizeof dx);
        src_expr(c, &in->src[3], dy, sizeof dy);
        texture_sample(c, in, coord, in->src[1].reg, NULL, dx, dy);
        return;
    }
    case OP_TEXKILL:
    {
        char r[96];
        dst_lvalue(c, &in->dst, r, sizeof r);
        char cond[256] = "";
        int n = 0;
        for (int k = 0; k < 4; ++k)
            if (in->dst.mask & (1u << k))
                n += snprintf(cond + n, sizeof cond - (size_t)n, "%s%s.%s < 0.0", n ? " || " : "", r, comp((unsigned)k));
        line(c, "if (%s) discard_fragment();", n ? cond : "false");
        return;
    }
    case OP_IF:
    {
        char b[256];
        src_expr(c, &in->src[0], b, sizeof b);
        if (in->src[0].type == R_PREDICATE)
            line(c, "if (%s(%s).x != 0.0) {", in->src[0].mod == 13 ? "!" : "", b);
        else
            line(c, "if (%s%s) {", in->src[0].mod == 13 ? "!" : "", b);
        c->depth++;
        return;
    }
    case OP_IFC:
        line(c, "if ((%s).x %s (%s).x) {", s0, cmp_op(in->ctl), s1);
        c->depth++;
        return;
    case OP_ELSE:
        c->depth--;
        line(c, "} else {");
        c->depth++;
        return;
    case OP_ENDIF:
        c->depth--;
        line(c, "}");
        return;
    case OP_ENDREP:
        c->depth--;
        line(c, "}");
        c->loops--;
        return;
    case OP_ENDLOOP:
        c->depth--;
        line(c, "}");
        line(c, "aL = aL_save%d; }", c->loops);
        c->loops--;
        return;
    case OP_LOOP:
    {
        c->loops++;
        unsigned i = in->src[1].reg;
        line(c, "{ int aL_save%d = aL; int4 L%d = I(ci, %u); aL = L%d.y;", c->loops, c->loops, i, c->loops);
        line(c, "for (int k%d = 0; k%d < L%d.x; ++k%d, aL += L%d.z) {", c->loops, c->loops, c->loops, c->loops, c->loops);
        c->depth++;
        return;
    }
    case OP_REP:
    {
        c->loops++;
        unsigned i = in->src[0].reg;
        line(c, "for (int k%d = 0; k%d < I(ci, %u).x; ++k%d) {", c->loops, c->loops, i, c->loops);
        c->depth++;
        return;
    }
    case OP_BREAK:
        line(c, "break;");
        return;
    case OP_BREAKC:
        line(c, "if ((%s).x %s (%s).x) break;", s0, cmp_op(in->ctl), s1);
        return;
    case OP_BREAKP:
        line(c, "if (%s(%s).x != 0.0) break;", in->src[0].mod == 13 ? "!" : "", s0);
        return;
    case OP_CALL:
        inline_call(c, in->src[0].reg);
        return;
    case OP_CALLNZ:
    {
        char b[256];
        src_expr(c, &in->src[1], b, sizeof b);
        if (in->src[1].type == R_PREDICATE)
            line(c, "if (%s(%s).x != 0.0) {", in->src[1].mod == 13 ? "!" : "", b);
        else
            line(c, "if (%s%s) {", in->src[1].mod == 13 ? "!" : "", b);
        c->depth++;
        inline_call(c, in->src[0].reg);
        c->depth--;
        line(c, "}");
        return;
    }
    case OP_RET:
    case OP_LABEL:
        return; /* handled by the range walk */
    default:
        fail(c, "opcode %u is not supported", in->op);
        return;
    }
    store(c, in, v);
}

static void translate_range(Ctx* c, unsigned from, unsigned to)
{
    for (unsigned i = from; i < to && !c->failed; ++i)
        one(c, i);
}

/* --- the program -------------------------------------------------------------------------------- */

static void prelude(Ctx* c)
{
    emit(c, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    emit(c, "struct SmAux { float4 half_pixel; float4 alpha_ref; };\n");
    emit(c, "static inline float4 Nrm(float4 v) { float d = dot(v.xyz, v.xyz); return d > 0.0 ? v * rsqrt(d) : v; }\n");
    emit(c, "static inline float4 Lit(float4 s) { float4 r = float4(1.0, 0.0, 0.0, 1.0); if (s.x > 0.0) { r.y = s.x; "
            "if (s.y > 0.0) r.z = powr(s.y, clamp(s.w, -127.9961, 127.9961)); } return r; }\n");
    /* defi/defb override the buffers */
    emit(c, "static inline int4 I(constant int4* ci, uint n) {\n    switch (n) {\n");
    for (unsigned i = 0; i < c->nidef; ++i)
        emit(c, "    case %u: return int4(%d, %d, %d, %d);\n", c->idef[i].reg, c->idef[i].v[0], c->idef[i].v[1],
            c->idef[i].v[2], c->idef[i].v[3]);
    emit(c, "    default: return ci[n & 15];\n    }\n}\n");
    emit(c, "static inline bool B(constant uint* cb, uint n) {\n    switch (n) {\n");
    for (unsigned i = 0; i < c->nbdef; ++i)
        emit(c, "    case %u: return %s;\n", c->bdef[i].reg, c->bdef[i].v ? "true" : "false");
    emit(c, "    default: return cb[n & 15] != 0;\n    }\n}\n");
    /* relative constant reads: def'd registers win, as on the hardware */
    unsigned nconst = c->info->pixel ? 224 : 256;
    emit(c, "static inline float4 C(constant float4* c, int n) {\n    switch (n) {\n");
    if (c->rel_const)
        for (unsigned i = 0; i < c->nfdef; ++i)
            emit(c, "    case %u: return float4(%.9g, %.9g, %.9g, %.9g);\n", c->fdef[i].reg, c->fdef[i].v[0],
                c->fdef[i].v[1], c->fdef[i].v[2], c->fdef[i].v[3]);
    emit(c, "    default: return (n >= 0 && n < %u) ? c[n] : float4(0.0);\n    }\n}\n\n", nconst);
}

static void resource_params(Ctx* c)
{
    emit(c, ",\n    constant float4* c [[buffer(%d)]], constant int4* ci [[buffer(%d)]], constant uint* cb [[buffer(%d)]], "
            "constant SmAux& aux [[buffer(%d)]]",
        SM_BUF_FLOAT, SM_BUF_INT, SM_BUF_BOOL, SM_BUF_AUX);
    for (unsigned s = 0; s < 16; ++s)
    {
        unsigned type = c->info->sampler[s];
        if (!type)
            continue;
        int shadow = c->opts && (c->opts->shadow_mask >> s) & 1;
        int depth = c->opts && (c->opts->depth_mask >> s) & 1;
        const char* tt = type == 3 ? "texturecube<float>" : type == 4 ? "texture3d<float>"
                         : shadow || depth ? "depth2d<float>" : "texture2d<float>";
        emit(c, ",\n    %s t%u [[texture(%u)]], sampler s%u [[sampler(%u)]]", tt, s, s, s, s);
    }
}

static void add_out(SmInfo* info, uint8_t usage, uint8_t index, uint8_t reg)
{
    for (unsigned i = 0; i < info->nout; ++i)
        if (info->out[i].usage == usage && info->out[i].index == index)
            return;
    if (info->nout < 16)
        info->out[info->nout++] = (SmDecl){ usage, index, reg, 1 };
}

static void vertex_program(Ctx* c)
{
    SmInfo* info = c->info;
    int sm3 = info->major >= 3;
    char name[24];
    /* inputs */
    emit(c, "struct VIn {\n");
    for (unsigned r = 0; r < 16; ++r)
        if (c->in_used[r])
        {
            unsigned k = c->opts ? c->opts->attr_int[r] : 0;
            emit(c, "    %s v%u [[attribute(%u)]];\n", k == 1 ? "int4" : k == 2 ? "uint4" : "float4", r, r);
        }
    emit(c, "};\n\n");
    for (unsigned r = 0; r < 16; ++r)
        if (c->in_used[r])
            info->in[info->nin++] = c->in_decl[r];
    /* outputs, by semantic */
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->out_used[r])
                add_out(info, c->out_decl[r].usage, c->out_decl[r].index, (uint8_t)r);
    }
    else
    {
        add_out(info, SM_POSITION, 0, 0);
        for (unsigned i = 0; i < 2; ++i)
            add_out(info, SM_COLOR, (uint8_t)i, (uint8_t)i);
        for (unsigned i = 0; i < 8; ++i)
            add_out(info, SM_TEXCOORD, (uint8_t)i, (uint8_t)i);
        add_out(info, SM_FOG, 0, 0);
    }
    int has_pos = 0, has_psize = 0;
    emit(c, "struct VOut {\n");
    for (unsigned i = 0; i < info->nout; ++i)
    {
        SmDecl* d = &info->out[i];
        if (d->usage == SM_POSITION && d->index == 0)
            emit(c, "    float4 position0 [[position]];\n"), has_pos = 1;
        else if (d->usage == SM_PSIZE)
            emit(c, "    float psize0 [[point_size]];\n"), has_psize = 1;
        else
            emit(c, "    float4 %s [[user(%s)]];\n", sm_semantic(d->usage, d->index, name), name);
    }
    if (!has_pos)
        emit(c, "    float4 position0 [[position]];\n");
    if (c->opts)
        for (unsigned i = 0; i < c->opts->nvs_extra; ++i)
        {
            const SmDecl* d = &c->opts->vs_extra[i];
            int dup = 0;
            for (unsigned k = 0; k < info->nout; ++k)
                dup |= info->out[k].usage == d->usage && info->out[k].index == d->index;
            if (!dup && !(d->usage == SM_POSITION && d->index == 0))
                emit(c, "    float4 %s [[user(%s)]];\n", sm_semantic(d->usage, d->index, name), name);
        }
    emit(c, "};\n\n");

    emit(c, "vertex VOut vmain(VIn vin [[stage_in]]");
    resource_params(c);
    emit(c, ")\n{\n");
    emit(c, "    float4 r[32] = {}; float4 v[16] = {}; float4 o[16] = {}; int4 a0 = 0; float4 a0f = 0.0; int aL = 0; bool4 p0 = false;\n");
    emit(c, "    float4 oPos = float4(0.0, 0.0, 0.0, 1.0), oD[2] = {}, oT[8] = {}; float oFog = 0.0, oPts = 1.0;\n");
    emit(c, "    (void)a0; (void)a0f; (void)aL; (void)p0; (void)oPts; (void)oFog;\n");
    for (unsigned r = 0; r < 16; ++r)
        if (c->in_used[r])
            emit(c, "    v[%u] = float4(vin.v%u);\n", r, r);
    /* the main program ends at the first ret before any label */
    unsigned end = c->nins;
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
        {
            end = i;
            break;
        }
    translate_range(c, 0, end);
    emit(c, "    VOut out;\n");
    if (sm3)
    {
        int wrote_pos = 0;
        for (unsigned r = 0; r < 16; ++r)
        {
            if (!c->out_used[r])
                continue;
            SmDecl* d = &c->out_decl[r];
            if (d->usage == SM_POSITION && d->index == 0)
                emit(c, "    out.position0 = o[%u];\n", r), wrote_pos = 1;
            else if (d->usage == SM_PSIZE)
                emit(c, "    out.psize0 = o[%u].x;\n", r);
            else
                emit(c, "    out.%s = o[%u];\n", sm_semantic(d->usage, d->index, name), r);
        }
        if (!wrote_pos)
            emit(c, "    out.position0 = float4(0.0, 0.0, 0.0, 1.0);\n");
    }
    else
    {
        emit(c, "    out.position0 = oPos;\n");
        for (unsigned i = 0; i < 2; ++i)
            emit(c, "    out.color%u = oD[%u];\n", i, i);
        for (unsigned i = 0; i < 8; ++i)
            emit(c, "    out.texcoord%u = oT[%u];\n", i, i);
        emit(c, "    out.fog0 = float4(oFog);\n");
    }
    if (c->opts)
        for (unsigned i = 0; i < c->opts->nvs_extra; ++i)
        {
            const SmDecl* d = &c->opts->vs_extra[i];
            int dup = 0;
            for (unsigned k = 0; k < info->nout; ++k)
                dup |= info->out[k].usage == d->usage && info->out[k].index == d->index;
            if (!dup && !(d->usage == SM_POSITION && d->index == 0))
                emit(c, "    out.%s = float4(0.0);\n", sm_semantic(d->usage, d->index, name));
        }
    (void)has_psize;
    /* D3D9's pixel centres: the half-pixel shift Direct3D 10+ and Metal do not have */
    emit(c, "    out.position0.xy += aux.half_pixel.xy * out.position0.w;\n");
    emit(c, "    return out;\n}\n");
}

static void pixel_program(Ctx* c)
{
    SmInfo* info = c->info;
    int sm3 = info->major >= 3;
    char name[24];
    emit(c, "struct PIn {\n    float4 position0 [[position]];\n");
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->in_used[r])
            {
                SmDecl* d = &c->in_decl[r];
                if (d->usage == SM_POSITION && d->index == 0)
                    continue;
                emit(c, "    float4 %s [[user(%s)]];\n", sm_semantic(d->usage, d->index, name), name);
                info->in[info->nin++] = *d;
            }
    }
    else
    {
        for (unsigned r = 0; r < 2; ++r)
            if (c->in_used[r])
            {
                emit(c, "    float4 color%u [[user(color%u)]];\n", r, r);
                info->in[info->nin++] = (SmDecl){ SM_COLOR, (uint8_t)r, (uint8_t)r, 1 };
            }
        for (unsigned r = 0; r < 8; ++r)
            if (c->tex_used[r])
            {
                emit(c, "    float4 texcoord%u [[user(texcoord%u)]];\n", r, r);
                info->in[info->nin++] = (SmDecl){ SM_TEXCOORD, (uint8_t)r, (uint8_t)r, 1 };
            }
    }
    emit(c, "};\n\nstruct POut {\n");
    for (unsigned i = 0; i < 4; ++i)
        if (info->color_outputs & (1u << i))
            emit(c, "    float4 c%u [[color(%u)]];\n", i, i);
    if (!info->color_outputs)
        emit(c, "    float4 c0 [[color(0)]];\n");
    int depth_out = info->writes_depth && !(c->opts && c->opts->no_depth_out);
    if (depth_out)
        emit(c, "    float depth [[depth(any)]];\n");
    emit(c, "};\n\n");

    emit(c, "fragment POut pmain(PIn pin [[stage_in]]%s", c->vface ? ", bool front [[front_facing]]" : "");
    resource_params(c);
    emit(c, ")\n{\n");
    emit(c, "    float4 r[32] = {}; float4 v[16] = {}; float4 tc[8] = {}; float4 oC[4] = {}; float oDepth = pin.position0.z;\n");
    emit(c, "    int aL = 0; bool4 p0 = false; int4 a0 = 0; (void)aL; (void)p0; (void)a0;\n");
    if (c->vpos)
        emit(c, "    float4 vpos = float4(pin.position0.xy - 0.5, 0.0, 0.0);\n");
    if (c->vface)
        emit(c, "    float4 vface = float4(front ? 1.0 : -1.0);\n");
    if (sm3)
    {
        for (unsigned r = 0; r < 16; ++r)
            if (c->in_used[r])
            {
                SmDecl* d = &c->in_decl[r];
                if (d->usage == SM_POSITION && d->index == 0)
                    emit(c, "    v[%u] = pin.position0;\n", r);
                else
                    emit(c, "    v[%u] = pin.%s;\n", r, sm_semantic(d->usage, d->index, name));
            }
    }
    else
    {
        for (unsigned r = 0; r < 2; ++r)
            if (c->in_used[r])
                emit(c, "    v[%u] = pin.color%u;\n", r, r);
        for (unsigned r = 0; r < 8; ++r)
            if (c->tex_used[r])
                emit(c, "    tc[%u] = pin.texcoord%u;\n", r, r);
    }
    unsigned end = c->nins;
    for (unsigned i = 0; i < c->nins; ++i)
        if (c->ins[i].op == OP_RET || c->ins[i].op == OP_LABEL)
        {
            end = i;
            break;
        }
    translate_range(c, 0, end);
    /* alpha test: the fixed-function stage D3D9 still runs after a pixel shader */
    if (c->opts && c->opts->alpha_func && c->opts->alpha_func != 8)
    {
        static const char* const ops[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        emit(c, "    if (!(oC[0].w %s aux.alpha_ref.x)) discard_fragment();\n", ops[c->opts->alpha_func & 7]);
    }
    emit(c, "    POut out;\n");
    for (unsigned i = 0; i < 4; ++i)
        if (info->color_outputs & (1u << i))
            emit(c, "    out.c%u = oC[%u];\n", i, i);
    if (!info->color_outputs)
        emit(c, "    out.c0 = oC[0];\n");
    if (depth_out)
        emit(c, "    out.depth = oDepth;\n");
    emit(c, "    return out;\n}\n");
}

static Ins g_ins_pool[MAX_INS];

int sm_info(const uint32_t* tokens, size_t ntokens, SmInfo* info)
{
    static char scratch[1 << 18];
    return sm_translate(tokens, ntokens, NULL, info, scratch, sizeof scratch, NULL, 0) != 0;
}

size_t sm_translate(const uint32_t* tokens, size_t ntokens, const SmOpts* opts, SmInfo* info, char* out, size_t cap,
    char* err, size_t errcap)
{
    static Ctx ctx; /* large: the instruction array (callers serialise translations) */
    Ctx* c = &ctx;
    memset(c, 0, sizeof *c);
    memset(info, 0, sizeof *info);
    c->info = info;
    c->opts = opts;
    c->ins = g_ins_pool;
    c->out = out;
    c->cap = cap;
    c->err = err;
    c->errcap = errcap;
    if (err && errcap)
        err[0] = 0;
    if (!parse(c, tokens, ntokens))
        return 0;
    if (!cap)
        return 0;
    prelude(c);
    if (info->pixel)
        pixel_program(c);
    else
        vertex_program(c);
    if (c->overflow)
        fail(c, "output buffer too small");
    if (c->loops != 0 && !c->failed)
        fail(c, "unbalanced flow control");
    return c->failed ? 0 : c->len;
}
