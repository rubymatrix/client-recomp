/* runtime/sse.h against real SSE hardware.
 *
 * One source, built twice:
 *   x86-64 (run under Rosetta 2): runs every operation with the real instruction (SSE/SSE2
 *     intrinsics) on a fixed stream of inputs and writes the results to a golden file;
 *   arm64: runs the same operations through the runtime/sse.h helpers the translation calls,
 *     and compares with the golden file.
 *
 *   clang -arch x86_64 -O1 -msse2 -Iruntime tests/sse_test.c -o build/sse_test_x86
 *   clang -arch arm64  -O1        -Iruntime tests/sse_test.c -o build/sse_test_arm
 *   arch -x86_64 build/sse_test_x86 build/sse_golden.bin && build/sse_test_arm build/sse_golden.bin
 *
 * NaN results compare equal to any NaN (payloads differ between x86 and arm64 and the game
 * never inspects them); RCP/RSQRT compare within their documented 1.5 * 2^-12 relative error.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char guest_mem[64];
#define GUEST_PTR(a) (guest_mem + (uint32_t)(a))
#include "sse.h"

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

/* Inputs: random bit patterns, plus floats and doubles chosen to hit the edges (NaN, infinities,
 * signed zeros, denormals, values past int32 range, exact halves for rounding). */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static const float FEDGE[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, 3.75f, 1e-40f, -1e-40f,
                               3.0e38f, -3.0e38f, INFINITY, -INFINITY, NAN, 2147483520.0f, 2147483648.0f,
                               -2147483648.0f, -2147483904.0f, 123456.789f, -0.333333f, 65535.5f };
static const double DEDGE[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 1e-310, 1e308, INFINITY, -INFINITY,
                                NAN, 2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0, -2147483648.5,
                                -2147483649.0, 3.14159265358979, -1e-5 };

static X128 random_x(int kind)
{
    X128 v;
    for (int k = 0; k < 4; k++) v.u32[k] = rnd32();
    if (kind == 1)
        for (int k = 0; k < 4; k++)
            v.f[k] = (rnd32() & 1) ? FEDGE[rnd32() % (sizeof FEDGE / sizeof *FEDGE)] : (float)((int32_t)rnd32()) / 65536.0f;
    else if (kind == 2)
        for (int k = 0; k < 2; k++)
            v.d[k] = (rnd32() & 1) ? DEDGE[rnd32() % (sizeof DEDGE / sizeof *DEDGE)] : (double)((int32_t)rnd32()) / 3.0;
    return v;
}

enum { F_BITS, F_PS, F_PD, F_APPROX };

typedef struct Case
{
    const char* name;
    int input;  /* 0 bits, 1 floats, 2 doubles */
    int compare;
} Case;

#define OPS(X)                                                                                     \
    X(addps, 1, F_PS) X(subps, 1, F_PS) X(mulps, 1, F_PS) X(divps, 1, F_PS) X(minps, 1, F_PS)      \
    X(maxps, 1, F_PS) X(addss, 1, F_PS) X(subss, 1, F_PS) X(mulss, 1, F_PS) X(divss, 1, F_PS)      \
    X(minss, 1, F_PS) X(maxss, 1, F_PS) X(sqrtps, 1, F_PS) X(sqrtss, 1, F_PS)                      \
    X(rcpps, 1, F_APPROX) X(rsqrtps, 1, F_APPROX)                                                  \
    X(addpd, 2, F_PD) X(subpd, 2, F_PD) X(mulpd, 2, F_PD) X(divpd, 2, F_PD) X(minpd, 2, F_PD)      \
    X(maxpd, 2, F_PD) X(addsd, 2, F_PD) X(mulsd, 2, F_PD) X(minsd, 2, F_PD) X(sqrtpd, 2, F_PD)     \
    X(andps, 0, F_BITS) X(andnps, 0, F_BITS) X(orps, 0, F_BITS) X(xorps, 0, F_BITS)                \
    X(unpcklps, 0, F_BITS) X(unpckhps, 0, F_BITS) X(unpcklpd, 0, F_BITS) X(unpckhpd, 0, F_BITS)    \
    X(shufps_1b, 0, F_BITS) X(shufps_e4, 0, F_BITS) X(shufpd_1, 0, F_BITS) X(pshufd_93, 0, F_BITS) \
    X(pshuflw_1b, 0, F_BITS) X(pshufhw_1b, 0, F_BITS)                                              \
    X(cmpeqps, 1, F_BITS) X(cmpltps, 1, F_BITS) X(cmpleps, 1, F_BITS) X(cmpunordps, 1, F_BITS)     \
    X(cmpneqps, 1, F_BITS) X(cmpnltps, 1, F_BITS) X(cmpnleps, 1, F_BITS) X(cmpordps, 1, F_BITS)    \
    X(cmpltsd, 2, F_BITS) X(cmpnlesd, 2, F_BITS)                                                   \
    X(cvtps2pd, 1, F_PD) X(cvtpd2ps, 2, F_PS) X(cvtss2sd, 1, F_BITS) X(cvtsd2ss, 2, F_BITS)        \
    X(cvtdq2ps, 0, F_PS) X(cvttps2dq, 1, F_BITS) X(cvtps2dq, 1, F_BITS) X(cvttpd2dq, 2, F_BITS)    \
    X(cvtpd2dq, 2, F_BITS) X(cvtdq2pd, 0, F_PD)                                                    \
    X(cvttss2si, 1, F_BITS) X(cvtss2si, 1, F_BITS) X(cvttsd2si, 2, F_BITS) X(cvtsd2si, 2, F_BITS)  \
    X(comiss, 1, F_BITS) X(comisd, 2, F_BITS) X(movmskps, 1, F_BITS) X(movmskpd, 2, F_BITS)        \
    X(pmovmskb, 0, F_BITS)                                                                         \
    X(paddb, 0, F_BITS) X(paddw, 0, F_BITS) X(paddd, 0, F_BITS) X(paddq, 0, F_BITS)                \
    X(psubb, 0, F_BITS) X(psubw, 0, F_BITS) X(psubd, 0, F_BITS) X(psubq, 0, F_BITS)                \
    X(pmullw, 0, F_BITS) X(pmulhw, 0, F_BITS) X(pmulhuw, 0, F_BITS) X(pmuludq, 0, F_BITS)          \
    X(pmaddwd, 0, F_BITS) X(psadbw, 0, F_BITS) X(pavgb, 0, F_BITS) X(pavgw, 0, F_BITS)             \
    X(pminub, 0, F_BITS) X(pmaxub, 0, F_BITS) X(pminsw, 0, F_BITS) X(pmaxsw, 0, F_BITS)            \
    X(pcmpeqb, 0, F_BITS) X(pcmpeqw, 0, F_BITS) X(pcmpeqd, 0, F_BITS) X(pcmpgtb, 0, F_BITS)        \
    X(pcmpgtw, 0, F_BITS) X(pcmpgtd, 0, F_BITS) X(paddsb, 0, F_BITS) X(paddsw, 0, F_BITS)          \
    X(paddusb, 0, F_BITS) X(paddusw, 0, F_BITS) X(psubsb, 0, F_BITS) X(psubsw, 0, F_BITS)          \
    X(psubusb, 0, F_BITS) X(psubusw, 0, F_BITS) X(packsswb, 0, F_BITS) X(packuswb, 0, F_BITS)      \
    X(packssdw, 0, F_BITS) X(punpcklbw, 0, F_BITS) X(punpckhbw, 0, F_BITS) X(punpcklwd, 0, F_BITS) \
    X(punpckhwd, 0, F_BITS) X(punpckldq, 0, F_BITS) X(punpckhdq, 0, F_BITS)                        \
    X(punpcklqdq, 0, F_BITS) X(punpckhqdq, 0, F_BITS)                                              \
    X(psllw_x, 0, F_BITS) X(pslld_5, 0, F_BITS) X(psllq_33, 0, F_BITS) X(psrlw_3, 0, F_BITS)       \
    X(psrld_x, 0, F_BITS) X(psrlq_7, 0, F_BITS) X(psraw_x, 0, F_BITS) X(psrad_9, 0, F_BITS)        \
    X(pslldq_3, 0, F_BITS) X(psrldq_5, 0, F_BITS)

#define CASE(n, in, cmp) { #n, in, cmp },
static const Case CASES[] = { OPS(CASE) };
#define NCASES (int)(sizeof CASES / sizeof *CASES)
#define ITERS 4000

#if defined(__x86_64__)
/* --- golden: the real instructions ---------------------------------------------------------- */
static void run_x86(int op, X128 a, X128 b, X128* out, uint32_t mxcsr)
{
    __m128 fa, fb; __m128d da, db; __m128i ia, ib;
    memcpy(&fa, &a, 16); memcpy(&fb, &b, 16);
    memcpy(&da, &a, 16); memcpy(&db, &b, 16);
    memcpy(&ia, &a, 16); memcpy(&ib, &b, 16);
    __m128i r = ia;
    unsigned saved = _mm_getcsr();
    _mm_setcsr(mxcsr);
    const char* n = CASES[op].name;
#define F(name, expr) else if (!strcmp(n, name)) { __m128 t_ = (expr); memcpy(&r, &t_, 16); }
#define D(name, expr) else if (!strcmp(n, name)) { __m128d t_ = (expr); memcpy(&r, &t_, 16); }
#define I(name, expr) else if (!strcmp(n, name)) { r = (expr); }
#define S(name, expr) else if (!strcmp(n, name)) { r = _mm_cvtsi32_si128((int)(expr)); }
    if (0) {}
    F("addps", _mm_add_ps(fa, fb)) F("subps", _mm_sub_ps(fa, fb)) F("mulps", _mm_mul_ps(fa, fb))
    F("divps", _mm_div_ps(fa, fb)) F("minps", _mm_min_ps(fa, fb)) F("maxps", _mm_max_ps(fa, fb))
    F("addss", _mm_add_ss(fa, fb)) F("subss", _mm_sub_ss(fa, fb)) F("mulss", _mm_mul_ss(fa, fb))
    F("divss", _mm_div_ss(fa, fb)) F("minss", _mm_min_ss(fa, fb)) F("maxss", _mm_max_ss(fa, fb))
    F("sqrtps", _mm_sqrt_ps(fb)) F("sqrtss", _mm_move_ss(fa, _mm_sqrt_ss(fb)))
    F("rcpps", _mm_rcp_ps(fb)) F("rsqrtps", _mm_rsqrt_ps(fb))
    D("addpd", _mm_add_pd(da, db)) D("subpd", _mm_sub_pd(da, db)) D("mulpd", _mm_mul_pd(da, db))
    D("divpd", _mm_div_pd(da, db)) D("minpd", _mm_min_pd(da, db)) D("maxpd", _mm_max_pd(da, db))
    D("addsd", _mm_add_sd(da, db)) D("mulsd", _mm_mul_sd(da, db)) D("minsd", _mm_min_sd(da, db))
    D("sqrtpd", _mm_sqrt_pd(db))
    F("andps", _mm_and_ps(fa, fb)) F("andnps", _mm_andnot_ps(fa, fb)) F("orps", _mm_or_ps(fa, fb))
    F("xorps", _mm_xor_ps(fa, fb)) F("unpcklps", _mm_unpacklo_ps(fa, fb)) F("unpckhps", _mm_unpackhi_ps(fa, fb))
    D("unpcklpd", _mm_unpacklo_pd(da, db)) D("unpckhpd", _mm_unpackhi_pd(da, db))
    F("shufps_1b", _mm_shuffle_ps(fa, fb, 0x1B)) F("shufps_e4", _mm_shuffle_ps(fa, fb, 0xE4))
    D("shufpd_1", _mm_shuffle_pd(da, db, 1)) I("pshufd_93", _mm_shuffle_epi32(ib, 0x93))
    I("pshuflw_1b", _mm_shufflelo_epi16(ib, 0x1B)) I("pshufhw_1b", _mm_shufflehi_epi16(ib, 0x1B))
    F("cmpeqps", _mm_cmpeq_ps(fa, fb)) F("cmpltps", _mm_cmplt_ps(fa, fb)) F("cmpleps", _mm_cmple_ps(fa, fb))
    F("cmpunordps", _mm_cmpunord_ps(fa, fb)) F("cmpneqps", _mm_cmpneq_ps(fa, fb))
    F("cmpnltps", _mm_cmpnlt_ps(fa, fb)) F("cmpnleps", _mm_cmpnle_ps(fa, fb)) F("cmpordps", _mm_cmpord_ps(fa, fb))
    D("cmpltsd", _mm_cmplt_sd(da, db)) D("cmpnlesd", _mm_cmpnle_sd(da, db))
    D("cvtps2pd", _mm_cvtps_pd(fb)) F("cvtpd2ps", _mm_cvtpd_ps(db))
    D("cvtss2sd", _mm_cvtss_sd(da, fb)) F("cvtsd2ss", _mm_cvtsd_ss(fa, db))
    F("cvtdq2ps", _mm_cvtepi32_ps(ib)) I("cvttps2dq", _mm_cvttps_epi32(fb)) I("cvtps2dq", _mm_cvtps_epi32(fb))
    I("cvttpd2dq", _mm_cvttpd_epi32(db)) I("cvtpd2dq", _mm_cvtpd_epi32(db)) D("cvtdq2pd", _mm_cvtepi32_pd(ib))
    S("cvttss2si", _mm_cvttss_si32(fb)) S("cvtss2si", _mm_cvtss_si32(fb))
    S("cvttsd2si", _mm_cvttsd_si32(db)) S("cvtsd2si", _mm_cvtsd_si32(db))
    S("comiss", (_mm_comieq_ss(fa, fb)) | (_mm_comilt_ss(fa, fb) << 1) | (_mm_comile_ss(fa, fb) << 2) | (_mm_ucomineq_ss(fa, fb) << 3))
    S("comisd", (_mm_comieq_sd(da, db)) | (_mm_comilt_sd(da, db) << 1) | (_mm_comile_sd(da, db) << 2) | (_mm_ucomineq_sd(da, db) << 3))
    S("movmskps", _mm_movemask_ps(fb)) S("movmskpd", _mm_movemask_pd(db)) S("pmovmskb", _mm_movemask_epi8(ib))
    I("paddb", _mm_add_epi8(ia, ib)) I("paddw", _mm_add_epi16(ia, ib)) I("paddd", _mm_add_epi32(ia, ib))
    I("paddq", _mm_add_epi64(ia, ib)) I("psubb", _mm_sub_epi8(ia, ib)) I("psubw", _mm_sub_epi16(ia, ib))
    I("psubd", _mm_sub_epi32(ia, ib)) I("psubq", _mm_sub_epi64(ia, ib)) I("pmullw", _mm_mullo_epi16(ia, ib))
    I("pmulhw", _mm_mulhi_epi16(ia, ib)) I("pmulhuw", _mm_mulhi_epu16(ia, ib)) I("pmuludq", _mm_mul_epu32(ia, ib))
    I("pmaddwd", _mm_madd_epi16(ia, ib)) I("psadbw", _mm_sad_epu8(ia, ib)) I("pavgb", _mm_avg_epu8(ia, ib))
    I("pavgw", _mm_avg_epu16(ia, ib)) I("pminub", _mm_min_epu8(ia, ib)) I("pmaxub", _mm_max_epu8(ia, ib))
    I("pminsw", _mm_min_epi16(ia, ib)) I("pmaxsw", _mm_max_epi16(ia, ib)) I("pcmpeqb", _mm_cmpeq_epi8(ia, ib))
    I("pcmpeqw", _mm_cmpeq_epi16(ia, ib)) I("pcmpeqd", _mm_cmpeq_epi32(ia, ib)) I("pcmpgtb", _mm_cmpgt_epi8(ia, ib))
    I("pcmpgtw", _mm_cmpgt_epi16(ia, ib)) I("pcmpgtd", _mm_cmpgt_epi32(ia, ib)) I("paddsb", _mm_adds_epi8(ia, ib))
    I("paddsw", _mm_adds_epi16(ia, ib)) I("paddusb", _mm_adds_epu8(ia, ib)) I("paddusw", _mm_adds_epu16(ia, ib))
    I("psubsb", _mm_subs_epi8(ia, ib)) I("psubsw", _mm_subs_epi16(ia, ib)) I("psubusb", _mm_subs_epu8(ia, ib))
    I("psubusw", _mm_subs_epu16(ia, ib)) I("packsswb", _mm_packs_epi16(ia, ib)) I("packuswb", _mm_packus_epi16(ia, ib))
    I("packssdw", _mm_packs_epi32(ia, ib)) I("punpcklbw", _mm_unpacklo_epi8(ia, ib)) I("punpckhbw", _mm_unpackhi_epi8(ia, ib))
    I("punpcklwd", _mm_unpacklo_epi16(ia, ib)) I("punpckhwd", _mm_unpackhi_epi16(ia, ib))
    I("punpckldq", _mm_unpacklo_epi32(ia, ib)) I("punpckhdq", _mm_unpackhi_epi32(ia, ib))
    I("punpcklqdq", _mm_unpacklo_epi64(ia, ib)) I("punpckhqdq", _mm_unpackhi_epi64(ia, ib))
    I("psllw_x", _mm_sll_epi16(ia, _mm_cvtsi32_si128(b.u8[0] & 31))) I("pslld_5", _mm_slli_epi32(ia, 5))
    I("psllq_33", _mm_slli_epi64(ia, 33)) I("psrlw_3", _mm_srli_epi16(ia, 3))
    I("psrld_x", _mm_srl_epi32(ia, _mm_cvtsi32_si128(b.u8[0] & 63))) I("psrlq_7", _mm_srli_epi64(ia, 7))
    I("psraw_x", _mm_sra_epi16(ia, _mm_cvtsi32_si128(b.u8[0] & 31))) I("psrad_9", _mm_srai_epi32(ia, 9))
    I("pslldq_3", _mm_slli_si128(ia, 3)) I("psrldq_5", _mm_srli_si128(ia, 5))
    else { fprintf(stderr, "no golden for %s\n", n); exit(2); }
    _mm_setcsr(saved);
    memcpy(out, &r, 16);
}
#else
/* --- under test: runtime/sse.h --------------------------------------------------------------- */
typedef struct { uint32_t mxcsr; } MiniGuest;

static void run_sse(int op, X128 a, X128 b, X128* out, uint32_t mxcsr)
{
    MiniGuest gg = { mxcsr }, *g = &gg;
    X128 d = a, s = b, r;
    const char* n = CASES[op].name;
#define H(name, call) else if (!strcmp(n, name)) { call; r = d; }
#define V(name, expr) else if (!strcmp(n, name)) { r = x_from32((uint32_t)(expr)); }
    if (0) {}
    H("addps", x_addps(&d, &s)) H("subps", x_subps(&d, &s)) H("mulps", x_mulps(&d, &s)) H("divps", x_divps(&d, &s))
    H("minps", x_minps(&d, &s)) H("maxps", x_maxps(&d, &s)) H("addss", x_addss(&d, &s)) H("subss", x_subss(&d, &s))
    H("mulss", x_mulss(&d, &s)) H("divss", x_divss(&d, &s)) H("minss", x_minss(&d, &s)) H("maxss", x_maxss(&d, &s))
    H("sqrtps", x_sqrtps(&d, &s)) H("sqrtss", x_sqrtss(&d, &s)) H("rcpps", x_rcpps(&d, &s)) H("rsqrtps", x_rsqrtps(&d, &s))
    H("addpd", x_addpd(&d, &s)) H("subpd", x_subpd(&d, &s)) H("mulpd", x_mulpd(&d, &s)) H("divpd", x_divpd(&d, &s))
    H("minpd", x_minpd(&d, &s)) H("maxpd", x_maxpd(&d, &s)) H("addsd", x_addsd(&d, &s)) H("mulsd", x_mulsd(&d, &s))
    H("minsd", x_minsd(&d, &s)) H("sqrtpd", x_sqrtpd(&d, &s))
    H("andps", x_and(&d, &s)) H("andnps", x_andn(&d, &s)) H("orps", x_or(&d, &s)) H("xorps", x_xor(&d, &s))
    H("unpcklps", x_unpcklps(&d, &s)) H("unpckhps", x_unpckhps(&d, &s)) H("unpcklpd", x_unpcklpd(&d, &s))
    H("unpckhpd", x_unpckhpd(&d, &s)) H("shufps_1b", x_shufps(&d, &s, 0x1B)) H("shufps_e4", x_shufps(&d, &s, 0xE4))
    H("shufpd_1", x_shufpd(&d, &s, 1)) H("pshufd_93", x_pshufd(&d, &s, 0x93)) H("pshuflw_1b", x_pshuflw(&d, &s, 0x1B))
    H("pshufhw_1b", x_pshufhw(&d, &s, 0x1B))
    H("cmpeqps", x_cmpps(&d, &s, 0)) H("cmpltps", x_cmpps(&d, &s, 1)) H("cmpleps", x_cmpps(&d, &s, 2))
    H("cmpunordps", x_cmpps(&d, &s, 3)) H("cmpneqps", x_cmpps(&d, &s, 4)) H("cmpnltps", x_cmpps(&d, &s, 5))
    H("cmpnleps", x_cmpps(&d, &s, 6)) H("cmpordps", x_cmpps(&d, &s, 7)) H("cmpltsd", x_cmpsd(&d, &s, 1))
    H("cmpnlesd", x_cmpsd(&d, &s, 6))
    H("cvtps2pd", x_cvtps2pd(&d, &s)) H("cvtpd2ps", x_cvtpd2ps(&d, &s)) H("cvtss2sd", x_cvtss2sd(&d, &s))
    H("cvtsd2ss", x_cvtsd2ss(&d, &s)) H("cvtdq2ps", x_cvtdq2ps(&d, &s)) H("cvttps2dq", x_cvttps2dq(&d, &s))
    H("cvtps2dq", x_cvtps2dq(&d, &s, g->mxcsr)) H("cvttpd2dq", x_cvttpd2dq(&d, &s)) H("cvtpd2dq", x_cvtpd2dq(&d, &s, g->mxcsr))
    H("cvtdq2pd", x_cvtdq2pd(&d, &s))
    V("cvttss2si", cvtt_f2i(s.f[0])) V("cvtss2si", cvt_f2i(g->mxcsr, s.f[0]))
    V("cvttsd2si", cvtt_f2i(s.d[0])) V("cvtsd2si", cvt_f2i(g->mxcsr, s.d[0]))
    else if (!strcmp(n, "comiss") || !strcmp(n, "comisd"))
    {
        unsigned zf, pf, cf, of, sf, af;
        if (n[5] == 's') SSE_COMI(d.f[0], s.f[0]); else SSE_COMI(d.d[0], s.d[0]);
        (void)of; (void)sf; (void)af;
        /* the intrinsics' view of the flags: eq = ZF&!PF, lt = CF&!PF, le = (ZF|CF)&!PF, neq = !ZF|PF */
        unsigned eq = zf && !pf, lt = cf && !pf, le = (zf || cf) && !pf, ne = !zf || pf;
        r = x_from32(eq | (lt << 1) | (le << 2) | (ne << 3));
    }
    V("movmskps", x_movmskps(&s)) V("movmskpd", x_movmskpd(&s)) V("pmovmskb", x_pmovmskb(&s))
    H("paddb", x_paddb(&d, &s)) H("paddw", x_paddw(&d, &s)) H("paddd", x_paddd(&d, &s)) H("paddq", x_paddq(&d, &s))
    H("psubb", x_psubb(&d, &s)) H("psubw", x_psubw(&d, &s)) H("psubd", x_psubd(&d, &s)) H("psubq", x_psubq(&d, &s))
    H("pmullw", x_pmullw(&d, &s)) H("pmulhw", x_pmulhw(&d, &s)) H("pmulhuw", x_pmulhuw(&d, &s))
    H("pmuludq", x_pmuludq(&d, &s)) H("pmaddwd", x_pmaddwd(&d, &s)) H("psadbw", x_psadbw(&d, &s))
    H("pavgb", x_pavgb(&d, &s)) H("pavgw", x_pavgw(&d, &s)) H("pminub", x_pminub(&d, &s)) H("pmaxub", x_pmaxub(&d, &s))
    H("pminsw", x_pminsw(&d, &s)) H("pmaxsw", x_pmaxsw(&d, &s)) H("pcmpeqb", x_pcmpeqb(&d, &s))
    H("pcmpeqw", x_pcmpeqw(&d, &s)) H("pcmpeqd", x_pcmpeqd(&d, &s)) H("pcmpgtb", x_pcmpgtb(&d, &s))
    H("pcmpgtw", x_pcmpgtw(&d, &s)) H("pcmpgtd", x_pcmpgtd(&d, &s)) H("paddsb", x_paddsb(&d, &s))
    H("paddsw", x_paddsw(&d, &s)) H("paddusb", x_paddusb(&d, &s)) H("paddusw", x_paddusw(&d, &s))
    H("psubsb", x_psubsb(&d, &s)) H("psubsw", x_psubsw(&d, &s)) H("psubusb", x_psubusb(&d, &s))
    H("psubusw", x_psubusw(&d, &s)) H("packsswb", x_packsswb(&d, &s)) H("packuswb", x_packuswb(&d, &s))
    H("packssdw", x_packssdw(&d, &s)) H("punpcklbw", x_punpcklbw(&d, &s)) H("punpckhbw", x_punpckhbw(&d, &s))
    H("punpcklwd", x_punpcklwd(&d, &s)) H("punpckhwd", x_punpckhwd(&d, &s)) H("punpckldq", x_punpckldq(&d, &s))
    H("punpckhdq", x_punpckhdq(&d, &s)) H("punpcklqdq", x_punpcklqdq(&d, &s)) H("punpckhqdq", x_punpckhqdq(&d, &s))
    H("psllw_x", x_psllw(&d, s.u8[0] & 31)) H("pslld_5", x_pslld(&d, 5)) H("psllq_33", x_psllq(&d, 33))
    H("psrlw_3", x_psrlw(&d, 3)) H("psrld_x", x_psrld(&d, s.u8[0] & 63)) H("psrlq_7", x_psrlq(&d, 7))
    H("psraw_x", x_psraw(&d, s.u8[0] & 31)) H("psrad_9", x_psrad(&d, 9)) H("pslldq_3", x_pslldq(&d, 3))
    H("psrldq_5", x_psrldq(&d, 5))
    else { fprintf(stderr, "no helper for %s\n", n); exit(2); }
    *out = r;
}

static int same_f(float a, float b, int approx)
{
    if (a != a || b != b) return (a != a) && (b != b);
    if (approx && isfinite(a) && a != 0.0f) return fabsf(a - b) <= fabsf(a) * 3.67e-4f;
    uint32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return x == y;
}
static int same_d(double a, double b)
{
    if (a != a || b != b) return (a != a) && (b != b);
    uint64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return x == y;
}
static int same(const Case* c, const X128* want, const X128* got)
{
    switch (c->compare)
    {
    case F_PS: case F_APPROX:
        for (int k = 0; k < 4; k++) if (!same_f(want->f[k], got->f[k], c->compare == F_APPROX)) return 0;
        return 1;
    case F_PD:
        for (int k = 0; k < 2; k++) if (!same_d(want->d[k], got->d[k])) return 0;
        return 1;
    default:
        return !memcmp(want, got, 16);
    }
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <golden.bin>\n", argv[0]); return 2; }
    static const uint32_t MXCSR[] = { 0x1F80, 0x3F80, 0x5F80, 0x7F80 }; /* RC = nearest, down, up, zero */
#if defined(__x86_64__)
    FILE* f = fopen(argv[1], "wb");
#else
    FILE* f = fopen(argv[1], "rb");
    int failures = 0, reported = 0;
#endif
    if (!f) { perror(argv[1]); return 2; }
    for (int op = 0; op < NCASES; op++)
    {
        int bad = 0;
        for (int i = 0; i < ITERS; i++)
        {
            X128 a = random_x(CASES[op].input), b = random_x(CASES[op].input);
            uint32_t mx = MXCSR[i & 3];
            X128 r;
#if defined(__x86_64__)
            run_x86(op, a, b, &r, mx);
            fwrite(&r, 16, 1, f);
#else
            X128 want;
            if (fread(&want, 16, 1, f) != 1) { fprintf(stderr, "golden file too short\n"); return 2; }
            sse_mxcsr_apply(mx);
            run_sse(op, a, b, &r, mx);
            sse_mxcsr_apply(0x1F80);
            if (!same(&CASES[op], &want, &r))
            {
                if (bad++ < 2 && reported++ < 40)
                    printf("FAIL %-10s mxcsr=%04x a=%08x %08x %08x %08x b=%08x %08x %08x %08x\n"
                           "     want %08x %08x %08x %08x  got %08x %08x %08x %08x\n",
                           CASES[op].name, mx, a.u32[0], a.u32[1], a.u32[2], a.u32[3], b.u32[0], b.u32[1],
                           b.u32[2], b.u32[3], want.u32[0], want.u32[1], want.u32[2], want.u32[3], r.u32[0],
                           r.u32[1], r.u32[2], r.u32[3]);
            }
#endif
        }
#if !defined(__x86_64__)
        if (bad) { failures++; printf("  %-10s %d/%d mismatches\n", CASES[op].name, bad, ITERS); }
#endif
    }
    fclose(f);
#if defined(__x86_64__)
    printf("golden: %d ops x %d inputs\n", NCASES, ITERS);
    return 0;
#else
    printf("%d of %d ops match SSE hardware on all %d inputs\n", NCASES - failures, NCASES, ITERS);
    return failures ? 1 : 0;
#endif
}
