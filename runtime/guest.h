/* Guest state and primitives shared by the runtime and every generated function.
 *
 * Generated code keeps registers and flags in C locals (see REGS_DECL/REGS_LOAD/REGS_STORE) and
 * writes them back to Guest only around calls and returns, so the C compiler can keep them in
 * host registers and drop flag computations nobody reads.
 *
 * Guest memory is a 4 GB window at GUEST_BASE. On the R2 target (32-bit Windows) GUEST_BASE is 0:
 * guest addresses are host addresses, and the game's own pointers work unchanged.
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(RT_GUEST_WINDOW)
/* 64-bit hosts (R3: the Windows x64 target and arm64 macOS): guest memory is a reserved 4 GB
 * window somewhere in the host address space; runtime/portable/gwin.c sets its base. */
extern unsigned char* rt_guest_base;
#define GUEST_PTR(a) (rt_guest_base + (uint32_t)(a))
#else
#ifndef GUEST_BASE
#define GUEST_BASE 0u
#endif
#define GUEST_PTR(a) ((unsigned char*)(uintptr_t)(GUEST_BASE + (uint32_t)(a)))
#endif

/* The helpers below are a few instructions each and run in the hottest translated code (an x87
 * push per float load). MSVC inlines to a budget per function, and a large translated function
 * spends it early, leaving these as calls; clang inlines them without being told. */
#if defined(_MSC_VER)
#define RT_INLINE static __forceinline
#else
#define RT_INLINE static inline
#endif

RT_INLINE uint8_t rd8(uint32_t a) { return *GUEST_PTR(a); }
RT_INLINE uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, GUEST_PTR(a), 2); return v; }
RT_INLINE uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, GUEST_PTR(a), 4); return v; }
RT_INLINE uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, GUEST_PTR(a), 8); return v; }
RT_INLINE void wr8(uint32_t a, uint8_t v) { *GUEST_PTR(a) = v; }
RT_INLINE void wr16(uint32_t a, uint16_t v) { memcpy(GUEST_PTR(a), &v, 2); }
RT_INLINE void wr32(uint32_t a, uint32_t v) { memcpy(GUEST_PTR(a), &v, 4); }
RT_INLINE void wr64(uint32_t a, uint64_t v) { memcpy(GUEST_PTR(a), &v, 8); }
RT_INLINE float rdf32(uint32_t a) { float v; memcpy(&v, GUEST_PTR(a), 4); return v; }
RT_INLINE double rdf64(uint32_t a) { double v; memcpy(&v, GUEST_PTR(a), 8); return v; }
RT_INLINE void wrf32(uint32_t a, float v) { memcpy(GUEST_PTR(a), &v, 4); }
RT_INLINE void wrf64(uint32_t a, double v) { memcpy(GUEST_PTR(a), &v, 8); }

#include "sse.h"

typedef struct Guest
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, pf, af, zf, sf, of, df;
    uint32_t fs_base; /* guest TEB: fs:[0] is the guest's SEH chain, never the host's */

    /* x87: values held as double. Precision control is honoured by rounding results to float
     * when PC selects 24 bits (Direct3D 8 sets that on its thread unless FPU_PRESERVE). */
    double st[8];
    uint32_t top;
    uint16_t fcw;
    uint8_t c0, c1, c2, c3;

    /* SSE/SSE2 (runtime/sse.h): MXCSR is kept as written (0x1F80 at thread start). */
    X128 xmm[8];
    uint32_t mxcsr;
    X128 mm[8]; /* MMX, upper halves zero (recomp/x86c.py xmm()) */
} Guest;

typedef void (*GuestFn)(Guest*);

/* Where the game image really is. The translation is made at its preferred base (0x10000000),
 * but inside the retail launcher the viewer's DLLs already occupy that address and Windows relocates
 * FFXiMain. Every address constant the image's relocation table covers is emitted as
 * `constant + RD`; jump-table entries and indirect-call targets are turned back into static
 * addresses before lookup. RD is 0 when the image sits at its preferred base. */
extern uint32_t rt_reloc_delta;
extern uint32_t rt_image_lo, rt_image_hi; /* runtime range of the image */
#define RD rt_reloc_delta

#define REGS_DECL                                                                                \
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;                                             \
    unsigned cf, pf, af, zf, sf, of, df;                                                         \
    (void)af
#define REGS_LOAD                                                                                \
    eax = g->eax; ecx = g->ecx; edx = g->edx; ebx = g->ebx;                                      \
    esp = g->esp; ebp = g->ebp; esi = g->esi; edi = g->edi;                                      \
    cf = g->cf; pf = g->pf; af = g->af; zf = g->zf; sf = g->sf; of = g->of; df = g->df
#define REGS_STORE                                                                               \
    g->eax = eax; g->ecx = ecx; g->edx = edx; g->ebx = ebx;                                      \
    g->esp = esp; g->ebp = ebp; g->esi = esi; g->edi = edi;                                      \
    g->cf = (uint8_t)cf; g->pf = (uint8_t)pf; g->af = (uint8_t)af; g->zf = (uint8_t)zf;          \
    g->sf = (uint8_t)sf; g->of = (uint8_t)of; g->df = (uint8_t)df

/* PF: set when the low byte has an even number of 1 bits. 0x6996 is the parity of each nibble. */
#define PARITY(v) ((unsigned)(!((0x6996u >> ((((uint32_t)(v) & 0xFFu) ^ (((uint32_t)(v) & 0xFFu) >> 4)) & 0xFu)) & 1u)))

/* --- runtime entry points (runtime.c) -------------------------------------------------------- */

void rt_call_indirect(Guest* g, uint32_t target);
#if defined(_MSC_VER)
__declspec(noreturn)
#elif defined(__GNUC__)
__attribute__((noreturn))
#endif
void rt_fatal(Guest* g, uint32_t addr, const char* what);
void rt_cpuid(Guest* g);
void rt_rdtsc(Guest* g);

/* The guest-wide lock: one guest thread runs
 * translated code at a time. The platform bridge releases it around every native call and takes
 * it on every host->guest entry; a loop that never calls out would starve the other guest threads,
 * so every backward jump is a safepoint that yields when another thread is waiting. The same
 * hand-off is what gives arm64 x86's memory ordering for the guest. */
extern volatile uint32_t rt_lock_contended;
void rt_safepoint(void);
#define RT_SAFEPOINT do { if (rt_lock_contended) rt_safepoint(); } while (0)

#define RT_UNIMPL(addr, text) do { REGS_STORE; rt_fatal(g, (addr), "unimplemented: " text); } while (0)
#define RT_BADJUMP(addr) do { REGS_STORE; rt_fatal(g, (addr), "jump to an address with no translation"); } while (0)
#define RT_DIVIDE(addr) do { REGS_STORE; rt_fatal(g, (addr), "divide error (#DE)"); } while (0)
#define RT_TRAP(addr) do { REGS_STORE; rt_fatal(g, (addr), "int3 reached"); } while (0)

/* --- x87 ------------------------------------------------------------------------------------- */

#define ST(i) (g->st[(g->top + (i)) & 7u])

RT_INLINE void fpush(Guest* g, double v)
{
    g->top = (g->top - 1) & 7u;
    g->st[g->top] = v;
}

RT_INLINE double fpop(Guest* g)
{
    double v = g->st[g->top];
    g->top = (g->top + 1) & 7u;
    return v;
}

/* Precision control: PC=00 (24-bit) rounds every result to single precision. */
RT_INLINE double fr_cw(unsigned cw, double x)
{
    return ((cw >> 8) & 3u) == 0 ? (double)(float)x : x;
}
RT_INLINE double fr(Guest* g, double x) { return fr_cw(g->fcw, x); }

/* FCOM/FUCOM/FTST/FICOM: C3 C2 C0 = 000 greater, 001 less, 100 equal, 111 unordered. */
RT_INLINE void fcom(Guest* g, double a, double b)
{
    if (a != a || b != b) { g->c3 = 1; g->c2 = 1; g->c0 = 1; }
    else if (a > b) { g->c3 = 0; g->c2 = 0; g->c0 = 0; }
    else if (a < b) { g->c3 = 0; g->c2 = 0; g->c0 = 1; }
    else { g->c3 = 1; g->c2 = 0; g->c0 = 0; }
    g->c1 = 0;
}

RT_INLINE uint16_t fstsw_top(Guest* g, unsigned top)
{
    return (uint16_t)((g->c3 << 14) | ((top & 7u) << 11) | (g->c2 << 10) | (g->c1 << 9) | (g->c0 << 8));
}
RT_INLINE uint16_t fstsw(Guest* g) { return fstsw_top(g, g->top); }

/* Round per the control word's RC field (00 nearest-even, 01 down, 10 up, 11 truncate). */
RT_INLINE double frnd_cw(unsigned cw, double x)
{
    switch ((cw >> 10) & 3u)
    {
    case 0: return nearbyint(x); /* host default rounding mode is nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
RT_INLINE double frnd(Guest* g, double x) { return frnd_cw(g->fcw, x); }

/* FIST/FISTP: out of range or NaN stores the "integer indefinite" value. */
RT_INLINE int16_t fist16_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -32768.0 || r > 32767.0) ? (int16_t)0x8000 : (int16_t)r;
}
RT_INLINE int32_t fist32_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -2147483648.0 || r > 2147483647.0) ? (int32_t)0x80000000u : (int32_t)r;
}
RT_INLINE int64_t fist64_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -9223372036854775808.0 || r >= 9223372036854775808.0) ? (int64_t)0x8000000000000000ull : (int64_t)r;
}
RT_INLINE int16_t fist16(Guest* g, double x) { return fist16_cw(g->fcw, x); }
RT_INLINE int32_t fist32(Guest* g, double x) { return fist32_cw(g->fcw, x); }
RT_INLINE int64_t fist64(Guest* g, double x) { return fist64_cw(g->fcw, x); }

/* --- x87 in locals: what the recompiler emits for functions with x87 code (recomp/x86c.py) ------
 * g->st[(g->top + i) & 7] as the stack cannot stay in registers: every guest store goes through a
 * byte pointer that may alias g, so each ST(i) is a reload and an index computation. In these
 * functions ST(i) is the local x87_s<i> instead, always ST(0) first. A push or pop renames them
 * (the compiler's copy propagation makes the rotation free), and a pop moves the old ST(0) to
 * ST(7), as the physical register stack does, so the state written back is exact. g has it at
 * every REGS_STORE and REGS_LOAD, and around the few instructions that take all of it. */
#define X87_DECL                                                                                 \
    double x87_s0, x87_s1, x87_s2, x87_s3, x87_s4, x87_s5, x87_s6, x87_s7, x87_t;                \
    unsigned x87_top, x87_cw;                                                                    \
    (void)x87_t
#define X87_LOAD                                                                                 \
    x87_top = g->top & 7u; x87_cw = g->fcw;                                                      \
    x87_s0 = g->st[x87_top]; x87_s1 = g->st[(x87_top + 1) & 7u];                                 \
    x87_s2 = g->st[(x87_top + 2) & 7u]; x87_s3 = g->st[(x87_top + 3) & 7u];                      \
    x87_s4 = g->st[(x87_top + 4) & 7u]; x87_s5 = g->st[(x87_top + 5) & 7u];                      \
    x87_s6 = g->st[(x87_top + 6) & 7u]; x87_s7 = g->st[(x87_top + 7) & 7u]
#define X87_STORE                                                                                \
    g->top = x87_top; g->fcw = (uint16_t)x87_cw;                                                 \
    g->st[x87_top] = x87_s0; g->st[(x87_top + 1) & 7u] = x87_s1;                                 \
    g->st[(x87_top + 2) & 7u] = x87_s2; g->st[(x87_top + 3) & 7u] = x87_s3;                      \
    g->st[(x87_top + 4) & 7u] = x87_s4; g->st[(x87_top + 5) & 7u] = x87_s5;                      \
    g->st[(x87_top + 6) & 7u] = x87_s6; g->st[(x87_top + 7) & 7u] = x87_s7
#define X87_PUSH(v)                                                                              \
    do                                                                                           \
    {                                                                                            \
        double x87_v_ = (v);                                                                     \
        x87_s7 = x87_s6; x87_s6 = x87_s5; x87_s5 = x87_s4; x87_s4 = x87_s3;                      \
        x87_s3 = x87_s2; x87_s2 = x87_s1; x87_s1 = x87_s0; x87_s0 = x87_v_;                      \
        x87_top = (x87_top - 1) & 7u;                                                            \
    } while (0)
#define X87_POP()                                                                                \
    (x87_t = x87_s0, x87_s0 = x87_s1, x87_s1 = x87_s2, x87_s2 = x87_s3, x87_s3 = x87_s4,         \
        x87_s4 = x87_s5, x87_s5 = x87_s6, x87_s6 = x87_s7, x87_s7 = x87_t,                       \
        x87_top = (x87_top + 1) & 7u, x87_t)

/* FXAM: C3 C2 C0 = 001 NaN, 010 normal, 011 infinity, 100 zero, 110 denormal; C1 = sign. */
RT_INLINE void fxam(Guest* g)
{
    double x = ST(0);
    g->c1 = signbit(x) ? 1 : 0;
    switch (fpclassify(x))
    {
    case FP_NAN: g->c3 = 0; g->c2 = 0; g->c0 = 1; break;
    case FP_INFINITE: g->c3 = 0; g->c2 = 1; g->c0 = 1; break;
    case FP_ZERO: g->c3 = 1; g->c2 = 0; g->c0 = 0; break;
    case FP_SUBNORMAL: g->c3 = 1; g->c2 = 1; g->c0 = 0; break;
    default: g->c3 = 0; g->c2 = 1; g->c0 = 0; break;
    }
}

/* FNSTENV/FLDENV (28-byte protected-mode environment) and FNSAVE/FRSTOR (environment + the eight
 * registers as 80-bit, ST(0) first). Tags are not modelled; a save/restore pair round-trips. */
RT_INLINE void wrf80(uint32_t a, double v);
RT_INLINE double rdf80(uint32_t a);
RT_INLINE void fenv_store(Guest* g, uint32_t a)
{
    wr32(a + 0, 0xFFFF0000u | g->fcw);
    wr32(a + 4, 0xFFFF0000u | fstsw(g));
    wr32(a + 8, 0xFFFFFFFFu);
    for (uint32_t k = 12; k < 28; k += 4)
        wr32(a + k, 0);
}
RT_INLINE void fenv_load(Guest* g, uint32_t a)
{
    uint16_t sw = rd16(a + 4);
    g->fcw = rd16(a + 0);
    g->top = (sw >> 11) & 7u;
    g->c0 = (sw >> 8) & 1u; g->c1 = (sw >> 9) & 1u; g->c2 = (sw >> 10) & 1u; g->c3 = (sw >> 14) & 1u;
}
RT_INLINE void fsave(Guest* g, uint32_t a)
{
    fenv_store(g, a);
    for (uint32_t i = 0; i < 8; ++i)
        wrf80(a + 28 + 10 * i, ST(i));
    g->fcw = 0x037F; g->top = 0; g->c0 = g->c1 = g->c2 = g->c3 = 0; /* FNSAVE ends with FNINIT */
}
RT_INLINE void frstor(Guest* g, uint32_t a)
{
    fenv_load(g, a);
    for (uint32_t i = 0; i < 8; ++i)
        ST(i) = rdf80(a + 28 + 10 * i);
}

/* 80-bit extended <-> double. */
RT_INLINE double rdf80(uint32_t a)
{
    uint64_t m;
    uint16_t se;
    memcpy(&m, GUEST_PTR(a), 8);
    memcpy(&se, GUEST_PTR(a) + 8, 2);
    int s = se >> 15, e = se & 0x7FFF;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return s ? -v : v;
}

RT_INLINE void wrf80(uint32_t a, double v)
{
    uint64_t m = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (v != v) { se |= 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; m = 0x8000000000000000ull; }
    else if (v != 0.0)
    {
        int e;
        double f = frexp(fabs(v), &e); /* v = f * 2^e, f in [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(GUEST_PTR(a), &m, 8);
    memcpy(GUEST_PTR(a) + 8, &se, 2);
}
