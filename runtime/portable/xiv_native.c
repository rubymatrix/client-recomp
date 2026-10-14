/* Native versions of hot game functions (recomp.py --wraps, tools/build_xiv.py WRAPS): what the
 * game calls millions of times a frame, where running the translation costs more than the work.
 *
 *   - the CRT's memcpy, memmove and memset (statically linked into ffxivgame.exe; MSVC's memcpy
 *     copies overlapping ranges as memmove does);
 *   - the render state cache's shader constant compare: the cached float4 registers from start on
 *     against the ones being set; the first that differs and everything after it are copied in,
 *     and the result says whether the device must be told (0) or nothing changed (1). The cache
 *     keeps two banks, at register 0x128 and 0x14 of its object;
 *   - a 4x4 matrix (rows) times a float4: x * row0 + y * row1 + z * row2 + w * row3, each step
 *     rounded to float as the game's mulps/addps do (no fused multiply-add). */
#include <string.h>

#include "runtime.h"

extern GuestFn rt_wrap_crt_memcpy, rt_wrap_crt_memmove, rt_wrap_crt_memset;
extern GuestFn rt_wrap_const_cache_a, rt_wrap_const_cache_b;
GuestFn rt_wrap_mat4_vec4 __attribute__((weak)); /* table.c's, in builds that wrap it */

#pragma STDC FP_CONTRACT OFF

static void w_memmove(Guest* g)
{
    uint32_t dst = rt_arg(g, 0), src = rt_arg(g, 1), n = rt_arg(g, 2);
    if (n)
        memmove(GUEST_PTR(dst), GUEST_PTR(src), n);
    rt_return(g, dst, 0);
}

static void w_memset(Guest* g)
{
    uint32_t dst = rt_arg(g, 0), c = rt_arg(g, 1), n = rt_arg(g, 2);
    if (n)
        memset(GUEST_PTR(dst), (int)(c & 0xFF), n);
    rt_return(g, dst, 0);
}

/* thiscall (start, const float4* src, count), returns in al; the registers sit at this + reg * 16 */
static void const_cache(Guest* g, uint32_t reg)
{
    uint32_t start = rt_arg(g, 0), src = rt_arg(g, 1), count = rt_arg(g, 2);
    if (start >= 0x100)
    {
        rt_return(g, (start & ~0xFFu) | 1, 12);
        return;
    }
    uint32_t dst = g->ecx + (start + reg) * 16;
    const float* d = (const float*)GUEST_PTR(dst);
    const float* s = (const float*)GUEST_PTR(src);
    for (uint32_t i = 0; i < count; ++i, d += 4, s += 4)
        if (!(d[0] == s[0] && d[1] == s[1] && d[2] == s[2] && d[3] == s[3])) /* NaN differs */
        {
            memmove(GUEST_PTR(dst + i * 16), GUEST_PTR(src + i * 16), (size_t)(count - i) * 16);
            rt_return(g, (dst + i * 16) & ~0xFFu, 12);
            return;
        }
    rt_return(g, (start & ~0xFFu) | 1, 12);
}

static void w_const_cache_a(Guest* g) { const_cache(g, 0x128); }
static void w_const_cache_b(Guest* g) { const_cache(g, 0x14); }

/* thiscall (float4* out, const float4* v), the matrix at this; returns out */
static void w_mat4_vec4(Guest* g)
{
    uint32_t out = rt_arg(g, 0);
    const float* m = (const float*)GUEST_PTR(g->ecx);
    float v[4], r[4];
    memcpy(v, GUEST_PTR(rt_arg(g, 1)), 16);
    for (int c = 0; c < 4; ++c)
    {
        float a = m[c] * v[0];
        float b = m[4 + c] * v[1];
        a = a + b;
        b = m[8 + c] * v[2];
        a = a + b;
        b = m[12 + c] * v[3];
        r[c] = a + b;
    }
    memcpy(GUEST_PTR(out), r, 16);
    rt_return(g, out, 8);
}

void xiv_native_init(void)
{
    rt_wrap_crt_memcpy = w_memmove;
    rt_wrap_crt_memmove = w_memmove;
    rt_wrap_crt_memset = w_memset;
    rt_wrap_const_cache_a = w_const_cache_a;
    rt_wrap_const_cache_b = w_const_cache_b;
    rt_wrap_mat4_vec4 = w_mat4_vec4;
}
