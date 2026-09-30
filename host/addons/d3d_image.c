/* Images and pixel formats for the addons' Direct3D (d3d_ffi.c): what D3DX reads (BMP files and
 * bare DIBs such as FFXI's item icons, DDS with DXT1-5, and PNG, JPEG, TGA, GIF... through
 * stb_image), and the D3D8 formats a texture can be locked in.
 *
 * Pixels move around as B, G, R, A bytes (D3DFMT_A8R8G8B8 in memory, what the overlay's textures
 * are), rows top-down. */
#include <stdlib.h>
#include <string.h>

#include "d3d_ffi.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* --- formats ---------------------------------------------------------------------------------- */

static int is_dxt(uint32_t f) { return f == XI_FMT_DXT1 || f == XI_FMT_DXT2 || f == XI_FMT_DXT3 || f == XI_FMT_DXT4 || f == XI_FMT_DXT5; }

int xi_fmt_bpp(uint32_t f)
{
    switch (f)
    {
    case 21: case 22: return 32;             /* A8R8G8B8, X8R8G8B8 */
    case 20: return 24;                      /* R8G8B8 */
    case 23: case 24: case 25: case 26: case 51: return 16; /* R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8L8 */
    case 28: case 50: return 8;              /* A8, L8 */
    case XI_FMT_DXT1: return 4;
    case XI_FMT_DXT2: case XI_FMT_DXT3: case XI_FMT_DXT4: case XI_FMT_DXT5: return 8;
    default: return 0;
    }
}

uint32_t xi_fmt_pitch(uint32_t f, uint32_t w)
{
    if (is_dxt(f))
        return ((w + 3) / 4) * (f == XI_FMT_DXT1 ? 8u : 16u);
    return w * (uint32_t)xi_fmt_bpp(f) / 8;
}

uint32_t xi_fmt_rows(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }

static uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t x6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t x4(uint32_t v) { return (uint8_t)(v * 17); }

static void rgb565(uint32_t c, uint8_t* out)
{
    out[0] = x5(c & 31), out[1] = x6((c >> 5) & 63), out[2] = x5((c >> 11) & 31);
}

/* One 4x4 DXT block into 16 BGRA pixels. */
static void dxt_block(uint32_t f, const uint8_t* b, uint8_t px[16][4])
{
    const uint8_t* col = f == XI_FMT_DXT1 ? b : b + 8;
    uint32_t c0 = rd16(col), c1 = rd16(col + 2), bits = rd32(col + 4);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]), rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (f != XI_FMT_DXT1 || c0 > c1)
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    else
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    pal[2][3] = 255;
    pal[3][3] = f == XI_FMT_DXT1 && c0 <= c1 ? 0 : 255;
    for (int i = 0; i < 16; ++i)
        memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
    if (f == XI_FMT_DXT2 || f == XI_FMT_DXT3)
        for (int i = 0; i < 16; ++i)
            px[i][3] = x4((b[i / 2] >> ((i & 1) * 4)) & 15);
    else if (f == XI_FMT_DXT4 || f == XI_FMT_DXT5)
    {
        uint32_t a0 = b[0], a1 = b[1], a[8];
        a[0] = a0, a[1] = a1;
        if (a0 > a1)
            for (int k = 2; k < 8; ++k)
                a[k] = ((8 - k) * a0 + (k - 1) * a1) / 7;
        else
        {
            for (int k = 2; k < 6; ++k)
                a[k] = ((6 - k) * a0 + (k - 1) * a1) / 5;
            a[6] = 0, a[7] = 255;
        }
        uint64_t ab = 0;
        for (int k = 0; k < 6; ++k)
            ab |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; ++i)
            px[i][3] = (uint8_t)a[(ab >> (3 * i)) & 7];
    }
}

void xi_fmt_to_bgra(uint32_t f, const uint8_t* src, int pitch, int w, int h, uint8_t* dst, int dpitch)
{
    if (is_dxt(f))
    {
        uint32_t bs = f == XI_FMT_DXT1 ? 8 : 16;
        for (int by = 0; by < (h + 3) / 4; ++by)
            for (int bx = 0; bx < (w + 3) / 4; ++bx)
            {
                uint8_t px[16][4];
                dxt_block(f, src + (size_t)by * pitch + (size_t)bx * bs, px);
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                    {
                        int x = bx * 4 + i, y = by * 4 + j;
                        if (x < w && y < h)
                            memcpy(dst + (size_t)y * dpitch + (size_t)x * 4, px[j * 4 + i], 4);
                    }
            }
        return;
    }
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = src + (size_t)y * pitch;
        uint8_t* d = dst + (size_t)y * dpitch;
        for (int x = 0; x < w; ++x, d += 4)
        {
            uint32_t v;
            switch (f)
            {
            case 21: memcpy(d, s + x * 4, 4); break;
            case 22: memcpy(d, s + x * 4, 3), d[3] = 255; break;
            case 20: memcpy(d, s + x * 3, 3), d[3] = 255; break;
            case 23: rgb565(rd16(s + x * 2), d), d[3] = 255; break;
            case 24: case 25:
                v = rd16(s + x * 2);
                d[0] = x5(v & 31), d[1] = x5((v >> 5) & 31), d[2] = x5((v >> 10) & 31);
                d[3] = f == 24 || (v & 0x8000) ? 255 : 0;
                break;
            case 26:
                v = rd16(s + x * 2);
                d[0] = x4(v & 15), d[1] = x4((v >> 4) & 15), d[2] = x4((v >> 8) & 15), d[3] = x4(v >> 12);
                break;
            case 28: d[0] = d[1] = d[2] = 255, d[3] = s[x]; break;
            case 50: d[0] = d[1] = d[2] = s[x], d[3] = 255; break;
            case 51: d[0] = d[1] = d[2] = s[x * 2], d[3] = s[x * 2 + 1]; break;
            default: d[0] = d[1] = d[2] = 0, d[3] = 0; break;
            }
        }
    }
}

int xi_fmt_from_bgra(uint32_t f, const uint8_t* bgra, int spitch, int w, int h, uint8_t* dst, int pitch)
{
    if (is_dxt(f) || !xi_fmt_bpp(f))
        return 0;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = bgra + (size_t)y * spitch;
        uint8_t* d = dst + (size_t)y * pitch;
        for (int x = 0; x < w; ++x, s += 4)
        {
            uint32_t v;
            switch (f)
            {
            case 21: memcpy(d + x * 4, s, 4); break;
            case 22: memcpy(d + x * 4, s, 3), d[x * 4 + 3] = 255; break;
            case 20: memcpy(d + x * 3, s, 3); break;
            case 23: v = (uint32_t)(s[0] >> 3) | (uint32_t)(s[1] >> 2) << 5 | (uint32_t)(s[2] >> 3) << 11; goto w16;
            case 24: case 25:
                v = (uint32_t)(s[0] >> 3) | (uint32_t)(s[1] >> 3) << 5 | (uint32_t)(s[2] >> 3) << 10 | (s[3] >= 128 ? 0x8000u : 0);
                goto w16;
            case 26: v = (uint32_t)(s[0] >> 4) | (uint32_t)(s[1] >> 4) << 4 | (uint32_t)(s[2] >> 4) << 8 | (uint32_t)(s[3] >> 4) << 12; goto w16;
            case 51: d[x * 2] = (uint8_t)((s[0] + s[1] * 2 + s[2]) / 4), d[x * 2 + 1] = s[3]; break;
            case 28: d[x] = s[3]; break;
            case 50: d[x] = (uint8_t)((s[0] + s[1] * 2 + s[2]) / 4); break;
            w16:
                d[x * 2] = (uint8_t)v, d[x * 2 + 1] = (uint8_t)(v >> 8);
                break;
            }
        }
    }
    return 1;
}

/* Bilinear, BGRA to BGRA. */
void xi_bgra_resize(const uint8_t* s, int sw, int sh, int spitch, uint8_t* d, int dw, int dh, int dpitch)
{
    for (int y = 0; y < dh; ++y)
    {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        float ty = fy < 0 ? 0 : fy - (float)y0;
        for (int x = 0; x < dw; ++x)
        {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            float tx = fx < 0 ? 0 : fx - (float)x0;
            const uint8_t *a = s + (size_t)y0 * spitch + (size_t)x0 * 4, *b = s + (size_t)y0 * spitch + (size_t)x1 * 4;
            const uint8_t *c = s + (size_t)y1 * spitch + (size_t)x0 * 4, *e = s + (size_t)y1 * spitch + (size_t)x1 * 4;
            uint8_t* o = d + (size_t)y * dpitch + (size_t)x * 4;
            for (int k = 0; k < 4; ++k)
            {
                float top = (float)a[k] + ((float)b[k] - (float)a[k]) * tx;
                float bot = (float)c[k] + ((float)e[k] - (float)c[k]) * tx;
                o[k] = (uint8_t)(top + (bot - top) * ty + 0.5f);
            }
        }
    }
}

/* --- DIB (BMP files, and bare DIBs: FFXI's icons) --------------------------------------------- */

static int mask_shift(uint32_t m)
{
    int s = 0;
    if (!m)
        return 0;
    while (!(m & 1))
        m >>= 1, ++s;
    return s;
}

static uint8_t mask_get(uint32_t v, uint32_t m)
{
    if (!m)
        return 0;
    int s = mask_shift(m);
    uint32_t bits = m >> s, x = (v & m) >> s;
    return (uint8_t)(bits == 255 ? x : x * 255 / bits);
}

/* A DIB from its BITMAPINFOHEADER; pixels at `bits` (NULL: right after the palette). */
static uint8_t* dib_decode(const uint8_t* p, size_t n, const uint8_t* bits, int* ow, int* oh)
{
    if (n < 40)
        return NULL;
    uint32_t hs = rd32(p);
    if (hs < 40 || hs > n)
        return NULL;
    int32_t w = (int32_t)rd32(p + 4), h = (int32_t)rd32(p + 8);
    uint32_t bpp = rd16(p + 14), comp = rd32(p + 16), used = rd32(p + 32);
    int topdown = h < 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (comp != 0 && comp != 3))
        return NULL;
    uint32_t masks[4] = { 0, 0, 0, 0 };
    const uint8_t* pal = p + hs;
    if (comp == 3)
    {
        if (hs >= 52)
            masks[0] = rd32(p + 40), masks[1] = rd32(p + 44), masks[2] = rd32(p + 48), masks[3] = hs >= 56 ? rd32(p + 52) : 0;
        else if (hs + 12 <= n)
            masks[0] = rd32(p + hs), masks[1] = rd32(p + hs + 4), masks[2] = rd32(p + hs + 8), pal += 12;
    }
    else if (bpp == 16)
        masks[0] = 0x7C00, masks[1] = 0x3E0, masks[2] = 0x1F;
    uint32_t ncolors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    if (!bits)
        bits = pal + ncolors * 4;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (bits < p || (size_t)(bits - p) + stride * (size_t)h > n || (size_t)(pal - p) + ncolors * 4 > n)
        return NULL;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return NULL;
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 4);
    if (!out)
        return NULL;
    /* FFXI keeps alpha (0..0x80) in the palette's spare byte and a 32 bpp DIB's fourth byte: 0x80
     * is opaque. All zero means no alpha at all. */
    int has_alpha = 0, half = 1;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = bits + stride * (size_t)(topdown ? y : h - 1 - y);
        uint8_t* d = out + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, d += 4)
        {
            uint32_t v;
            switch (bpp)
            {
            case 1: case 4: case 8:
            {
                uint32_t i = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (row[x / 8] >> (7 - (x & 7))) & 1;
                if (i >= ncolors)
                    i = 0;
                memcpy(d, pal + i * 4, 4);
                break;
            }
            case 16:
                v = rd16(row + x * 2);
                d[0] = mask_get(v, masks[2]), d[1] = mask_get(v, masks[1]), d[2] = mask_get(v, masks[0]);
                d[3] = masks[3] ? mask_get(v, masks[3]) : 0;
                break;
            case 24: memcpy(d, row + x * 3, 3), d[3] = 0; break;
            default:
                v = rd32(row + x * 4);
                if (comp == 3)
                    d[0] = mask_get(v, masks[2]), d[1] = mask_get(v, masks[1]), d[2] = mask_get(v, masks[0]),
                    d[3] = mask_get(v, masks[3]);
                else
                    memcpy(d, row + x * 4, 4);
                break;
            }
            if (d[3])
                has_alpha = 1;
            if (d[3] > 0x80)
                half = 0;
        }
    }
    size_t np = (size_t)w * h;
    for (size_t i = 0; i < np; ++i)
    {
        uint8_t* d = out + i * 4;
        d[3] = !has_alpha ? 255 : half ? (uint8_t)(d[3] >= 0x80 ? 255 : d[3] * 2) : d[3];
    }
    *ow = w, *oh = h;
    return out;
}

/* --- DDS ------------------------------------------------------------------------------------ */

static uint8_t* dds_decode(const uint8_t* p, size_t n, int* ow, int* oh, uint32_t* fmt, uint32_t* mips)
{
    if (n < 128 || memcmp(p, "DDS ", 4) || rd32(p + 4) != 124)
        return NULL;
    uint32_t h = rd32(p + 12), w = rd32(p + 16), mip = rd32(p + 28);
    uint32_t pf = rd32(p + 80), four = rd32(p + 84), bits = rd32(p + 88);
    uint32_t rm = rd32(p + 92), gm = rd32(p + 96), bm = rd32(p + 100), am = rd32(p + 104);
    if (!w || !h || w > 16384 || h > 16384)
        return NULL;
    const uint8_t* data = p + 128;
    size_t avail = n - 128;
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 4);
    if (!out)
        return NULL;
    if (pf & 4) /* DDPF_FOURCC */
    {
        if (!is_dxt(four) || (size_t)xi_fmt_pitch(four, w) * xi_fmt_rows(four, h) > avail)
            return free(out), (uint8_t*)NULL;
        xi_fmt_to_bgra(four, data, (int)xi_fmt_pitch(four, w), (int)w, (int)h, out, (int)w * 4);
        *fmt = four;
    }
    else if (bits == 32 || bits == 24 || bits == 16)
    {
        size_t stride = (size_t)w * bits / 8;
        if (stride * h > avail)
            return free(out), (uint8_t*)NULL;
        int alpha = (pf & 1) && am; /* DDPF_ALPHAPIXELS */
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* s = data + y * stride + x * bits / 8;
                uint32_t v = bits == 32 ? rd32(s) : bits == 24 ? (rd16(s) | (uint32_t)s[2] << 16) : rd16(s);
                uint8_t* d = out + ((size_t)y * w + x) * 4;
                d[0] = mask_get(v, bm), d[1] = mask_get(v, gm), d[2] = mask_get(v, rm);
                d[3] = alpha ? mask_get(v, am) : 255;
            }
        *fmt = alpha ? 21 : 22;
    }
    else
        return free(out), (uint8_t*)NULL;
    *ow = (int)w, *oh = (int)h;
    *mips = mip ? mip : 1;
    return out;
}

/* --- any image ------------------------------------------------------------------------------ */

uint8_t* xi_image_decode(const uint8_t* p, size_t n, XiImageInfo* info)
{
    int w = 0, h = 0;
    uint8_t* px = NULL;
    memset(info, 0, sizeof *info);
    info->depth = 1, info->mips = 1, info->format = 21, info->type = 3; /* D3DRTYPE_TEXTURE */
    if (!p || n < 4)
        return NULL;
    if (n >= 54 && p[0] == 'B' && p[1] == 'M')
    {
        uint32_t off = rd32(p + 10);
        if (off < n)
            px = dib_decode(p + 14, n - 14, p + off, &w, &h);
        info->file_format = 0; /* D3DXIFF_BMP */
    }
    else if (n >= 40 && (rd32(p) == 40 || rd32(p) == 52 || rd32(p) == 56 || rd32(p) == 108 || rd32(p) == 124))
    {
        px = dib_decode(p, n, NULL, &w, &h);
        info->file_format = 6; /* D3DXIFF_DIB */
    }
    else if (!memcmp(p, "DDS ", 4))
    {
        px = dds_decode(p, n, &w, &h, &info->format, &info->mips);
        info->file_format = 4;
    }
    if (!px && n <= 0x7FFFFFFF)
    {
        int c;
        uint8_t* rgba = stbi_load_from_memory(p, (int)n, &w, &h, &c, 4);
        if (rgba)
        {
            for (size_t i = 0; i < (size_t)w * h; ++i)
            {
                uint8_t t = rgba[i * 4];
                rgba[i * 4] = rgba[i * 4 + 2], rgba[i * 4 + 2] = t;
            }
            px = (uint8_t*)malloc((size_t)w * h * 4);
            if (px)
                memcpy(px, rgba, (size_t)w * h * 4);
            stbi_image_free(rgba);
            info->format = c == 4 || c == 2 ? 21 : 22;
            info->file_format = p[0] == 0x89 && p[1] == 'P' ? 3 : p[0] == 0xFF && p[1] == 0xD8 ? 1 : 2; /* PNG, JPG, else TGA */
        }
    }
    if (!px)
        return NULL;
    info->width = (uint32_t)w, info->height = (uint32_t)h;
    return px;
}
