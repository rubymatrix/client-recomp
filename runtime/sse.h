/* SSE / SSE2 guest state and lane operations, for generated code (recomp/x86c.py "SSE").
 *
 * The eight XMM registers live in Guest (g->xmm[i], an X128 union of lanes) with MXCSR beside
 * them. Generated code copies the source operand into a temporary first (`X128 S = ...`), so a
 * helper never sees its destination and source alias, and calls one helper per instruction.
 * Each helper is a few lane operations that clang lowers to NEON.
 *
 * Semantics follow the Intel SDM where a game can tell the difference:
 *   - MINxx/MAXxx return the second (source) operand when either is NaN or both are zero;
 *   - CMPxx predicates 0-7 produce all-ones / all-zero lane masks, unordered as the SDM says;
 *   - float -> integer conversions that overflow, or see NaN, give the "integer indefinite"
 *     value (0x80000000 / 0x8000000000000000);
 *   - rounding of non-truncating conversions follows MXCSR.RC.
 * MXCSR.RC and FTZ/DAZ reach the host instead (sse_mxcsr_apply, run by LDMXCSR): the host's
 * floating-point control register is per thread like MXCSR, so every float operation of the
 * translation then rounds as the guest asked. arm64 FPCR.FZ flushes inputs and outputs, so FTZ
 * and DAZ map onto it together. Not modelled: exception flags and masks (MXCSR reads back as
 * written), and the reduced precision of RCPxx/RSQRTxx (results are exact, within their spec).
 * The x87 translation shares the host rounding mode, so x87 arithmetic run while the guest has
 * MXCSR.RC away from nearest rounds the SSE way; x87 conversions keep their own (frnd_cw).
 */
#pragma once

#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union X128
{
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f[4];
    double d[2];
} X128;

#define SSE_INLINE static inline

SSE_INLINE void sse_mxcsr_apply(uint32_t mxcsr)
{
    static const int rc[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    fesetround(rc[(mxcsr >> 13) & 3u]);
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~(1ull << 24)) | ((mxcsr & 0x8040u) ? (1ull << 24) : 0); /* FZ <- FTZ | DAZ */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

/* --- loads and stores -------------------------------------------------------------------------- */

#define X(i) (g->xmm[(i)])
#define MM(i) (g->mm[(i)])

SSE_INLINE X128 ld_x128(uint32_t a) { X128 v; memcpy(&v, GUEST_PTR(a), 16); return v; }
SSE_INLINE X128 ld_x64(uint32_t a) { X128 v = {{0}}; memcpy(&v, GUEST_PTR(a), 8); return v; }
SSE_INLINE X128 ld_x32(uint32_t a) { X128 v = {{0}}; memcpy(&v, GUEST_PTR(a), 4); return v; }
SSE_INLINE X128 x_from32(uint32_t lo) { X128 v = {{0}}; v.u32[0] = lo; return v; }
SSE_INLINE void st_x128(uint32_t a, const X128* v) { memcpy(GUEST_PTR(a), v, 16); }
SSE_INLINE void st_x64(uint32_t a, const X128* v) { memcpy(GUEST_PTR(a), v, 8); }
SSE_INLINE void st_x64hi(uint32_t a, const X128* v) { memcpy(GUEST_PTR(a), &v->u64[1], 8); }
SSE_INLINE void st_x32(uint32_t a, const X128* v) { memcpy(GUEST_PTR(a), v, 4); }

/* --- moves ------------------------------------------------------------------------------------- */

SSE_INLINE void x_mov(X128* d, const X128* s) { *d = *s; }
SSE_INLINE void x_movlo32(X128* d, const X128* s) { d->u32[0] = s->u32[0]; }   /* MOVSS xmm, xmm */
SSE_INLINE void x_movlo64(X128* d, const X128* s) { d->u64[0] = s->u64[0]; }   /* MOVSD xmm, xmm; MOVLPS/MOVLPD load */
SSE_INLINE void x_movhi64(X128* d, const X128* s) { d->u64[1] = s->u64[0]; }   /* MOVHPS/MOVHPD load, MOVLHPS */
SSE_INLINE void x_movhl(X128* d, const X128* s) { d->u64[0] = s->u64[1]; }     /* MOVHLPS */
SSE_INLINE void x_movq(X128* d, const X128* s) { d->u64[0] = s->u64[0]; d->u64[1] = 0; } /* MOVQ xmm, xmm/m64 */

/* --- packed / scalar float arithmetic ---------------------------------------------------------- */

SSE_INLINE float fmin_x86(float a, float b) { return a < b ? a : b; }
SSE_INLINE float fmax_x86(float a, float b) { return a > b ? a : b; }
SSE_INLINE double dmin_x86(double a, double b) { return a < b ? a : b; }
SSE_INLINE double dmax_x86(double a, double b) { return a > b ? a : b; }

#define SSE_PS(name, expr)                                                                         \
    SSE_INLINE void name##ps(X128* d, const X128* s)                                               \
    { for (int k = 0; k < 4; k++) { float a = d->f[k], b = s->f[k]; d->f[k] = (expr); } }         \
    SSE_INLINE void name##ss(X128* d, const X128* s)                                               \
    { float a = d->f[0], b = s->f[0]; d->f[0] = (expr); }                                          \
    SSE_INLINE void name##pd(X128* d, const X128* s)                                               \
    { for (int k = 0; k < 2; k++) { double a = d->d[k], b = s->d[k]; d->d[k] = (expr); } }         \
    SSE_INLINE void name##sd(X128* d, const X128* s)                                               \
    { double a = d->d[0], b = s->d[0]; d->d[0] = (expr); }

SSE_PS(x_add, a + b)
SSE_PS(x_sub, a - b)
SSE_PS(x_mul, a * b)
SSE_PS(x_div, a / b)
SSE_PS(x_min, a < b ? a : b)
SSE_PS(x_max, a > b ? a : b)

SSE_INLINE void x_sqrtps(X128* d, const X128* s) { for (int k = 0; k < 4; k++) d->f[k] = sqrtf(s->f[k]); }
SSE_INLINE void x_sqrtss(X128* d, const X128* s) { d->f[0] = sqrtf(s->f[0]); }
SSE_INLINE void x_sqrtpd(X128* d, const X128* s) { for (int k = 0; k < 2; k++) d->d[k] = sqrt(s->d[k]); }
SSE_INLINE void x_sqrtsd(X128* d, const X128* s) { d->d[0] = sqrt(s->d[0]); }
/* RCP/RSQRT: the hardware approximations treat a denormal input as a zero of its sign and flush a
 * denormal result to zero (the results themselves are exact here, within the 1.5 * 2^-12 spec). */
SSE_INLINE float sse_daz(float x) { return fabsf(x) < 1.17549435e-38f ? copysignf(0.0f, x) : x; }
SSE_INLINE float sse_rcp(float x) { return sse_daz(1.0f / sse_daz(x)); }
SSE_INLINE float sse_rsqrt(float x) { x = sse_daz(x); return x == 0.0f ? copysignf(INFINITY, x) : sse_daz(1.0f / sqrtf(x)); }
SSE_INLINE void x_rcpps(X128* d, const X128* s) { for (int k = 0; k < 4; k++) d->f[k] = sse_rcp(s->f[k]); }
SSE_INLINE void x_rcpss(X128* d, const X128* s) { d->f[0] = sse_rcp(s->f[0]); }
SSE_INLINE void x_rsqrtps(X128* d, const X128* s) { for (int k = 0; k < 4; k++) d->f[k] = sse_rsqrt(s->f[k]); }
SSE_INLINE void x_rsqrtss(X128* d, const X128* s) { d->f[0] = sse_rsqrt(s->f[0]); }

/* --- bitwise ----------------------------------------------------------------------------------- */

SSE_INLINE void x_and(X128* d, const X128* s) { d->u64[0] &= s->u64[0]; d->u64[1] &= s->u64[1]; }
SSE_INLINE void x_andn(X128* d, const X128* s) { d->u64[0] = ~d->u64[0] & s->u64[0]; d->u64[1] = ~d->u64[1] & s->u64[1]; }
SSE_INLINE void x_or(X128* d, const X128* s) { d->u64[0] |= s->u64[0]; d->u64[1] |= s->u64[1]; }
SSE_INLINE void x_xor(X128* d, const X128* s) { d->u64[0] ^= s->u64[0]; d->u64[1] ^= s->u64[1]; }

/* --- compares ---------------------------------------------------------------------------------- */

SSE_INLINE int fcmp_pred(double a, double b, unsigned p)
{
    int un = a != a || b != b;
    switch (p & 7u)
    {
    case 0: return !un && a == b;   /* EQ */
    case 1: return !un && a < b;    /* LT */
    case 2: return !un && a <= b;   /* LE */
    case 3: return un;              /* UNORD */
    case 4: return un || a != b;    /* NEQ */
    case 5: return un || !(a < b);  /* NLT */
    case 6: return un || !(a <= b); /* NLE */
    default: return !un;            /* ORD */
    }
}
SSE_INLINE void x_cmpps(X128* d, const X128* s, unsigned p)
{ for (int k = 0; k < 4; k++) d->u32[k] = fcmp_pred(d->f[k], s->f[k], p) ? 0xFFFFFFFFu : 0u; }
SSE_INLINE void x_cmpss(X128* d, const X128* s, unsigned p)
{ d->u32[0] = fcmp_pred(d->f[0], s->f[0], p) ? 0xFFFFFFFFu : 0u; }
SSE_INLINE void x_cmppd(X128* d, const X128* s, unsigned p)
{ for (int k = 0; k < 2; k++) d->u64[k] = fcmp_pred(d->d[k], s->d[k], p) ? ~0ull : 0ull; }
SSE_INLINE void x_cmpsd(X128* d, const X128* s, unsigned p)
{ d->u64[0] = fcmp_pred(d->d[0], s->d[0], p) ? ~0ull : 0ull; }

/* COMISS/UCOMISS/COMISD/UCOMISD write ZF PF CF (unordered: all three) and clear OF SF AF; the
 * generated code takes the result as `SSE_COMI(a, b)` into its flag locals. */
#define SSE_COMI(a, b)                                                                             \
    do {                                                                                           \
        double ca_ = (a), cb_ = (b);                                                               \
        if (ca_ != ca_ || cb_ != cb_) { zf = pf = cf = 1; }                                        \
        else { zf = ca_ == cb_; pf = 0; cf = ca_ < cb_; }                                          \
        of = sf = af = 0;                                                                          \
    } while (0)

/* --- shuffles and unpacks ---------------------------------------------------------------------- */

SSE_INLINE void x_shufps(X128* d, const X128* s, unsigned imm)
{
    X128 a = *d;
    d->u32[0] = a.u32[imm & 3u];
    d->u32[1] = a.u32[(imm >> 2) & 3u];
    d->u32[2] = s->u32[(imm >> 4) & 3u];
    d->u32[3] = s->u32[(imm >> 6) & 3u];
}
SSE_INLINE void x_shufpd(X128* d, const X128* s, unsigned imm)
{
    X128 a = *d;
    d->u64[0] = a.u64[imm & 1u];
    d->u64[1] = s->u64[(imm >> 1) & 1u];
}
SSE_INLINE void x_unpcklps(X128* d, const X128* s)
{ X128 a = *d; d->u32[0] = a.u32[0]; d->u32[1] = s->u32[0]; d->u32[2] = a.u32[1]; d->u32[3] = s->u32[1]; }
SSE_INLINE void x_unpckhps(X128* d, const X128* s)
{ X128 a = *d; d->u32[0] = a.u32[2]; d->u32[1] = s->u32[2]; d->u32[2] = a.u32[3]; d->u32[3] = s->u32[3]; }
SSE_INLINE void x_unpcklpd(X128* d, const X128* s) { d->u64[1] = s->u64[0]; }
SSE_INLINE void x_unpckhpd(X128* d, const X128* s) { d->u64[0] = d->u64[1]; d->u64[1] = s->u64[1]; }
SSE_INLINE void x_pshufd(X128* d, const X128* s, unsigned imm)
{ X128 a = *s; for (int k = 0; k < 4; k++) d->u32[k] = a.u32[(imm >> (2 * k)) & 3u]; }
SSE_INLINE void x_pshuflw(X128* d, const X128* s, unsigned imm)
{ X128 a = *s; for (int k = 0; k < 4; k++) d->u16[k] = a.u16[(imm >> (2 * k)) & 3u]; d->u64[1] = a.u64[1]; }
SSE_INLINE void x_pshufhw(X128* d, const X128* s, unsigned imm)
{ X128 a = *s; for (int k = 0; k < 4; k++) d->u16[4 + k] = a.u16[4 + ((imm >> (2 * k)) & 3u)]; d->u64[0] = a.u64[0]; }

/* --- conversions ------------------------------------------------------------------------------- */

/* MXCSR.RC: 0 nearest-even, 1 down, 2 up, 3 toward zero. */
SSE_INLINE double sse_round(uint32_t mxcsr, double x)
{
    switch ((mxcsr >> 13) & 3u)
    {
    case 0: return rint(x); /* the host runs round-to-nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
SSE_INLINE int32_t sse_i32(double x)
{ return (x != x || x >= 2147483648.0 || x < -2147483648.0) ? INT32_MIN : (int32_t)x; }

SSE_INLINE int32_t cvt_f2i(uint32_t mxcsr, double x) { return sse_i32(sse_round(mxcsr, x)); }
SSE_INLINE int32_t cvtt_f2i(double x) { return sse_i32(trunc(x)); }

SSE_INLINE void x_cvtps2pd(X128* d, const X128* s) { float a = s->f[0], b = s->f[1]; d->d[0] = a; d->d[1] = b; }
SSE_INLINE void x_cvtpd2ps(X128* d, const X128* s) { double a = s->d[0], b = s->d[1]; d->f[0] = (float)a; d->f[1] = (float)b; d->u64[1] = 0; }
SSE_INLINE void x_cvtss2sd(X128* d, const X128* s) { d->d[0] = s->f[0]; }
SSE_INLINE void x_cvtsd2ss(X128* d, const X128* s) { d->f[0] = (float)s->d[0]; }
SSE_INLINE void x_cvtdq2ps(X128* d, const X128* s) { for (int k = 0; k < 4; k++) d->f[k] = (float)s->s32[k]; }
SSE_INLINE void x_cvtdq2pd(X128* d, const X128* s) { int32_t a = s->s32[0], b = s->s32[1]; d->d[0] = a; d->d[1] = b; }
SSE_INLINE void x_cvttps2dq(X128* d, const X128* s) { for (int k = 0; k < 4; k++) d->s32[k] = cvtt_f2i(s->f[k]); }
SSE_INLINE void x_cvttpd2dq(X128* d, const X128* s) { double a = s->d[0], b = s->d[1]; d->s32[0] = cvtt_f2i(a); d->s32[1] = cvtt_f2i(b); d->u64[1] = 0; }
SSE_INLINE void x_cvtps2dq(X128* d, const X128* s, uint32_t mxcsr)
{ for (int k = 0; k < 4; k++) d->s32[k] = cvt_f2i(mxcsr, s->f[k]); }
SSE_INLINE void x_cvtpd2dq(X128* d, const X128* s, uint32_t mxcsr)
{ double a = s->d[0], b = s->d[1]; d->s32[0] = cvt_f2i(mxcsr, a); d->s32[1] = cvt_f2i(mxcsr, b); d->u64[1] = 0; }

/* --- SSE2 integer ------------------------------------------------------------------------------ */

#define SSE_LANES(name, field, T, n, expr)                                                         \
    SSE_INLINE void name(X128* d, const X128* s)                                                   \
    { for (int k = 0; k < (n); k++) { T a = d->field[k], b = s->field[k]; d->field[k] = (T)(expr); } }

SSE_LANES(x_paddb, u8, uint8_t, 16, a + b)
SSE_LANES(x_paddw, u16, uint16_t, 8, a + b)
SSE_LANES(x_paddd, u32, uint32_t, 4, a + b)
SSE_LANES(x_paddq, u64, uint64_t, 2, a + b)
SSE_LANES(x_psubb, u8, uint8_t, 16, a - b)
SSE_LANES(x_psubw, u16, uint16_t, 8, a - b)
SSE_LANES(x_psubd, u32, uint32_t, 4, a - b)
SSE_LANES(x_psubq, u64, uint64_t, 2, a - b)
SSE_LANES(x_pmullw, s16, int16_t, 8, (int32_t)a * (int32_t)b)
SSE_LANES(x_pmulhw, s16, int16_t, 8, ((int32_t)a * (int32_t)b) >> 16)
SSE_LANES(x_pmulhuw, u16, uint16_t, 8, ((uint32_t)a * (uint32_t)b) >> 16)
SSE_LANES(x_pavgb, u8, uint8_t, 16, ((unsigned)a + b + 1u) >> 1)
SSE_LANES(x_pavgw, u16, uint16_t, 8, ((unsigned)a + b + 1u) >> 1)
SSE_LANES(x_pminub, u8, uint8_t, 16, a < b ? a : b)
SSE_LANES(x_pmaxub, u8, uint8_t, 16, a > b ? a : b)
SSE_LANES(x_pminsw, s16, int16_t, 8, a < b ? a : b)
SSE_LANES(x_pmaxsw, s16, int16_t, 8, a > b ? a : b)
SSE_LANES(x_pcmpeqb, u8, uint8_t, 16, a == b ? 0xFFu : 0u)
SSE_LANES(x_pcmpeqw, u16, uint16_t, 8, a == b ? 0xFFFFu : 0u)
SSE_LANES(x_pcmpeqd, u32, uint32_t, 4, a == b ? 0xFFFFFFFFu : 0u)
SSE_LANES(x_pcmpgtb, s8, int8_t, 16, a > b ? -1 : 0)
SSE_LANES(x_pcmpgtw, s16, int16_t, 8, a > b ? -1 : 0)
SSE_LANES(x_pcmpgtd, s32, int32_t, 4, a > b ? -1 : 0)

SSE_INLINE int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
SSE_INLINE int8_t sat8(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
SSE_INLINE uint8_t satu8(int32_t v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
SSE_INLINE uint16_t satu16(int32_t v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }

SSE_LANES(x_paddsb, s8, int8_t, 16, sat8((int32_t)a + b))
SSE_LANES(x_paddsw, s16, int16_t, 8, sat16((int32_t)a + b))
SSE_LANES(x_paddusb, u8, uint8_t, 16, satu8((int32_t)a + b))
SSE_LANES(x_paddusw, u16, uint16_t, 8, satu16((int32_t)a + b))
SSE_LANES(x_psubsb, s8, int8_t, 16, sat8((int32_t)a - b))
SSE_LANES(x_psubsw, s16, int16_t, 8, sat16((int32_t)a - b))
SSE_LANES(x_psubusb, u8, uint8_t, 16, satu8((int32_t)a - b))
SSE_LANES(x_psubusw, u16, uint16_t, 8, satu16((int32_t)a - b))

SSE_INLINE void x_pmuludq(X128* d, const X128* s)
{ for (int k = 0; k < 2; k++) d->u64[k] = (uint64_t)d->u32[2 * k] * s->u32[2 * k]; }
SSE_INLINE void x_pmaddwd(X128* d, const X128* s)
{
    X128 a = *d;
    for (int k = 0; k < 4; k++)
        d->s32[k] = (int32_t)((uint32_t)((int32_t)a.s16[2 * k] * s->s16[2 * k]) + (uint32_t)((int32_t)a.s16[2 * k + 1] * s->s16[2 * k + 1]));
}
SSE_INLINE void x_psadbw(X128* d, const X128* s)
{
    for (int h = 0; h < 2; h++)
    {
        unsigned sum = 0;
        for (int k = 0; k < 8; k++)
        { int v = (int)d->u8[8 * h + k] - (int)s->u8[8 * h + k]; sum += (unsigned)(v < 0 ? -v : v); }
        d->u64[h] = sum;
    }
}

SSE_INLINE void x_packsswb(X128* d, const X128* s)
{ X128 a = *d; for (int k = 0; k < 8; k++) { d->s8[k] = sat8(a.s16[k]); d->s8[8 + k] = sat8(s->s16[k]); } }
SSE_INLINE void x_packuswb(X128* d, const X128* s)
{ X128 a = *d; for (int k = 0; k < 8; k++) { d->u8[k] = satu8(a.s16[k]); d->u8[8 + k] = satu8(s->s16[k]); } }
SSE_INLINE void x_packssdw(X128* d, const X128* s)
{ X128 a = *d; for (int k = 0; k < 4; k++) { d->s16[k] = sat16(a.s32[k]); d->s16[4 + k] = sat16(s->s32[k]); } }

SSE_INLINE void x_punpcklbw(X128* d, const X128* s) { X128 a = *d; for (int k = 0; k < 8; k++) { d->u8[2 * k] = a.u8[k]; d->u8[2 * k + 1] = s->u8[k]; } }
SSE_INLINE void x_punpckhbw(X128* d, const X128* s) { X128 a = *d; for (int k = 0; k < 8; k++) { d->u8[2 * k] = a.u8[8 + k]; d->u8[2 * k + 1] = s->u8[8 + k]; } }
SSE_INLINE void x_punpcklwd(X128* d, const X128* s) { X128 a = *d; for (int k = 0; k < 4; k++) { d->u16[2 * k] = a.u16[k]; d->u16[2 * k + 1] = s->u16[k]; } }
SSE_INLINE void x_punpckhwd(X128* d, const X128* s) { X128 a = *d; for (int k = 0; k < 4; k++) { d->u16[2 * k] = a.u16[4 + k]; d->u16[2 * k + 1] = s->u16[4 + k]; } }
SSE_INLINE void x_punpckldq(X128* d, const X128* s) { x_unpcklps(d, s); }
SSE_INLINE void x_punpckhdq(X128* d, const X128* s) { x_unpckhps(d, s); }
SSE_INLINE void x_punpcklqdq(X128* d, const X128* s) { x_unpcklpd(d, s); }
SSE_INLINE void x_punpckhqdq(X128* d, const X128* s) { x_unpckhpd(d, s); }

/* Shifts by an XMM/m128 count (low 64 bits) or an immediate; counts past the width clear (logical)
 * or fill with the sign (arithmetic). */
SSE_INLINE void x_psllw(X128* d, uint64_t c) { for (int k = 0; k < 8; k++) d->u16[k] = c > 15 ? 0 : (uint16_t)(d->u16[k] << c); }
SSE_INLINE void x_pslld(X128* d, uint64_t c) { for (int k = 0; k < 4; k++) d->u32[k] = c > 31 ? 0 : d->u32[k] << c; }
SSE_INLINE void x_psllq(X128* d, uint64_t c) { for (int k = 0; k < 2; k++) d->u64[k] = c > 63 ? 0 : d->u64[k] << c; }
SSE_INLINE void x_psrlw(X128* d, uint64_t c) { for (int k = 0; k < 8; k++) d->u16[k] = c > 15 ? 0 : (uint16_t)(d->u16[k] >> c); }
SSE_INLINE void x_psrld(X128* d, uint64_t c) { for (int k = 0; k < 4; k++) d->u32[k] = c > 31 ? 0 : d->u32[k] >> c; }
SSE_INLINE void x_psrlq(X128* d, uint64_t c) { for (int k = 0; k < 2; k++) d->u64[k] = c > 63 ? 0 : d->u64[k] >> c; }
SSE_INLINE void x_psraw(X128* d, uint64_t c) { for (int k = 0; k < 8; k++) d->s16[k] = (int16_t)(d->s16[k] >> (c > 15 ? 15 : c)); }
SSE_INLINE void x_psrad(X128* d, uint64_t c) { for (int k = 0; k < 4; k++) d->s32[k] = d->s32[k] >> (c > 31 ? 31 : c); }
SSE_INLINE void x_pslldq(X128* d, unsigned n)
{ X128 a = *d; if (n > 16) n = 16; for (int k = 0; k < 16; k++) d->u8[k] = k >= (int)n ? a.u8[k - n] : 0; }
SSE_INLINE void x_psrldq(X128* d, unsigned n)
{ X128 a = *d; if (n > 16) n = 16; for (int k = 0; k < 16; k++) d->u8[k] = k + n < 16 ? a.u8[k + n] : 0; }

/* --- masks ------------------------------------------------------------------------------------- */

SSE_INLINE uint32_t x_movmskps(const X128* s) { uint32_t r = 0; for (int k = 0; k < 4; k++) r |= (s->u32[k] >> 31) << k; return r; }
SSE_INLINE uint32_t x_movmskpd(const X128* s) { return (uint32_t)((s->u64[0] >> 63) | ((s->u64[1] >> 63) << 1)); }
SSE_INLINE uint32_t x_pmovmskb(const X128* s) { uint32_t r = 0; for (int k = 0; k < 16; k++) r |= (uint32_t)(s->u8[k] >> 7) << k; return r; }
