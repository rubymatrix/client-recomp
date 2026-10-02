/* The game's UI art from its DATs. See datui.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datui.h"

int dat_read(const char* host_path, DatFile* f)
{
    memset(f, 0, sizeof *f);
    FILE* fp = fopen(host_path, "rb");
    if (!fp)
        return 0;
    long n = -1;
    if (!fseek(fp, 0, SEEK_END))
        n = ftell(fp);
    if (n <= 0 || fseek(fp, 0, SEEK_SET))
    {
        fclose(fp);
        return 0;
    }
    f->data = malloc((size_t)n);
    if (f->data && fread(f->data, 1, (size_t)n, fp) == (size_t)n)
        f->size = (size_t)n;
    else
    {
        free(f->data);
        f->data = NULL;
    }
    fclose(fp);
    return f->data != NULL;
}

void dat_free(DatFile* f)
{
    free(f->data);
    memset(f, 0, sizeof *f);
}

static uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The next chunk at *off: its type, payload and payload size. 0 at the end or at a bad header. */
static int next_chunk(const DatFile* f, size_t* off, unsigned* type, const uint8_t** payload, size_t* size)
{
    if (*off + 16 > f->size)
        return 0;
    uint32_t info = rd32(f->data + *off + 4);
    size_t len = (size_t)((info >> 7) & 0x7ffff) * 16;
    if (len < 16 || *off + len > f->size)
        return 0;
    *type = info & 0x7f;
    *payload = f->data + *off + 16;
    *size = len - 16;
    *off += len;
    return 1;
}

/* A padded 8-character DAT name to lowercase C, without the padding. */
static void dat_name(const uint8_t* src, char out[9])
{
    int n = 0;
    for (int i = 0; i < 8; ++i)
    {
        char c = (char)src[i];
        out[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        if (c != ' ' && c)
            n = i + 1;
    }
    out[n] = 0;
}

static int name_is(const uint8_t* src, const char* want)
{
    char s[9];
    dat_name(src, s);
    for (size_t i = 0;; ++i)
    {
        char a = s[i], b = want[i];
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
}

/* ---- images ---- */

static void rgb565(uint16_t c, uint8_t out[4])
{
    out[0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
    out[1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
    out[3] = 255;
}

/* A 4x4 block's colours: DXT1 has a transparent 3-colour mode (c0 <= c1); DXT3/5 always 4 */
static void dxt_colors(const uint8_t* b, int dxt1, uint8_t pal[4][4])
{
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    for (int k = 0; k < 3; ++k)
    {
        if (!dxt1 || c0 > c1)
        {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        else
        {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (uint8_t)(!dxt1 || c0 > c1 ? 255 : 0);
}

static int decode_dxt(const uint8_t* src, size_t n, int kind, uint32_t w, uint32_t h, uint8_t* rgba)
{
    size_t block = kind == 1 ? 8 : 16;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if ((size_t)bw * bh * block > n)
        return 0;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, src += block)
        {
            const uint8_t* cb = kind == 1 ? src : src + 8;
            uint8_t pal[4][4], alpha[16];
            dxt_colors(cb, kind == 1, pal);
            uint32_t bits = rd32(cb + 4);
            if (kind == 3)
                for (int i = 0; i < 16; ++i)
                    alpha[i] = (uint8_t)((src[i / 2] >> (i % 2 * 4) & 15) * 17);
            else if (kind == 5)
            {
                uint8_t a[8] = { src[0], src[1] };
                for (int i = 2; i < 8; ++i)
                    a[i] = a[0] > a[1] ? (uint8_t)(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                         : i < 6       ? (uint8_t)(((6 - i) * a[0] + (i - 1) * a[1]) / 5)
                                       : (uint8_t)(i == 6 ? 0 : 255);
                uint64_t ab = 0;
                for (int i = 0; i < 6; ++i)
                    ab |= (uint64_t)src[2 + i] << (8 * i);
                for (int i = 0; i < 16; ++i)
                    alpha[i] = a[ab >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; ++i)
            {
                uint32_t x = bx * 4 + (uint32_t)(i % 4), y = by * 4 + (uint32_t)(i / 4);
                if (x >= w || y >= h)
                    continue;
                uint8_t* d = rgba + ((size_t)y * w + x) * 4;
                const uint8_t* c = pal[bits >> (2 * i) & 3];
                d[0] = c[0];
                d[1] = c[1];
                d[2] = c[2];
                d[3] = kind == 1 ? c[3] : alpha[i];
            }
        }
    return 1;
}

static int decode_image(const uint8_t* p, size_t n, DatImage* out)
{
    /* flag, category, name, then the BITMAPINFOHEADER */
    if (n < 17 + 40)
        return 0;
    const uint8_t* bi = p + 17;
    int32_t w = (int32_t)rd32(bi + 4), h = (int32_t)rd32(bi + 8);
    unsigned bpp = rd16(bi + 14);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return 0;
    const uint8_t* src = bi + 40;
    size_t left = n - 17 - 40, px = (size_t)w * (size_t)h;
    uint8_t* rgba = malloc(px * 4);
    if (!rgba)
        return 0;
    int ok = 0;
    char fourcc[4] = { 0 };
    if (left >= 4)
        for (int i = 0; i < 4; ++i)
            fourcc[i] = (char)src[3 - i];
    if (left >= 12 && !memcmp(fourcc, "DXT", 3) && (fourcc[3] == '1' || fourcc[3] == '3' || fourcc[3] == '5'))
        ok = decode_dxt(src + 12, left - 12, fourcc[3] - '0', (uint32_t)w, (uint32_t)h, rgba);
    else if (bpp == 8 && left >= 1024 + px)
    {
        const uint8_t* pal = src;
        for (size_t i = 0; i < px; ++i)
        {
            const uint8_t* c = pal + src[1024 + i] * 4;
            rgba[i * 4 + 0] = c[2];
            rgba[i * 4 + 1] = c[1];
            rgba[i * 4 + 2] = c[0];
            rgba[i * 4 + 3] = c[3];
        }
        ok = 1;
    }
    else if (bpp == 32 && left >= px * 4)
    {
        for (size_t i = 0; i < px; ++i)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        }
        ok = 1;
    }
    if (!ok)
    {
        free(rgba);
        return 0;
    }
    /* PS2 alpha: 0x80 is opaque. DXT1's 0/255 stays as it is. */
    for (size_t i = 0; i < px; ++i)
    {
        unsigned a = rgba[i * 4 + 3] * 2u;
        rgba[i * 4 + 3] = (uint8_t)(a > 255 ? 255 : a);
    }
    out->w = (uint32_t)w;
    out->h = (uint32_t)h;
    out->rgba = rgba;
    return 1;
}

int dat_image(const DatFile* f, const char* category, const char* name, DatImage* out)
{
    memset(out, 0, sizeof *out);
    size_t off = 0, n;
    unsigned type;
    const uint8_t* p;
    while (next_chunk(f, &off, &type, &p, &n))
        if (type == 0x20 && n >= 17 && name_is(p + 1, category) && name_is(p + 9, name))
            return decode_image(p, n, out);
    return 0;
}

void dat_image_free(DatImage* img)
{
    free(img->rgba);
    memset(img, 0, sizeof *img);
}

/* ---- sprite sheets ---- */

enum
{
    PART_BYTES = 45,
    PART_NAME = 16,
};

/* Walks a sheet's sprites; with parts NULL only counts them. 0 if it runs past the chunk. */
static int walk_sheet(const uint8_t* p, size_t n, uint32_t* nsprites, uint32_t* nparts, DatSprite* sprites, DatPart* parts)
{
    if (n < 17)
        return 0;
    size_t at = 17 + (size_t)p[16] * 16;
    if (at + 2 > n)
        return 0;
    uint32_t ns = rd16(p + at), np = 0;
    at += 2;
    for (uint32_t s = 0; s < ns; ++s)
    {
        if (at + 1 > n)
            return 0;
        unsigned k = p[at++];
        if (sprites)
        {
            sprites[s].nparts = k;
            sprites[s].parts = parts + np;
        }
        for (unsigned i = 0; i < k; ++i, ++np)
        {
            if (at + PART_BYTES + PART_NAME > n)
                return 0;
            if (parts)
            {
                const uint8_t* r = p + at;
                DatPart* d = &parts[np];
                for (int v = 0; v < 4; ++v)
                {
                    d->x[v] = (int16_t)rd16(r + v * 4);
                    d->y[v] = (int16_t)rd16(r + v * 4 + 2);
                }
                d->uw = (int16_t)rd16(r + 16);
                d->uh = (int16_t)rd16(r + 18);
                d->u = (int16_t)rd16(r + 20);
                d->v = (int16_t)rd16(r + 22);
                memcpy(d->color, r + 25, 16);
                memcpy(d->mode, r + 41, 4);
                dat_name(r + PART_BYTES, d->category);
                dat_name(r + PART_BYTES + 8, d->name);
            }
            at += PART_BYTES + PART_NAME;
        }
    }
    *nsprites = ns;
    *nparts = np;
    return 1;
}

int dat_sheet(const DatFile* f, const char* name, DatSheet* out)
{
    memset(out, 0, sizeof *out);
    size_t off = 0, n;
    unsigned type;
    const uint8_t* p;
    while (next_chunk(f, &off, &type, &p, &n))
    {
        uint32_t ns, np;
        if (type != 0x31 || n < 16 || !name_is(p + 8, name) || !walk_sheet(p, n, &ns, &np, NULL, NULL))
            continue;
        out->sprites = calloc(ns ? ns : 1, sizeof *out->sprites);
        out->all = calloc(np ? np : 1, sizeof *out->all);
        if (!out->sprites || !out->all)
        {
            dat_sheet_free(out);
            return 0;
        }
        walk_sheet(p, n, &ns, &np, out->sprites, out->all);
        out->nsprites = ns;
        dat_name(p + 8, out->name);
        return 1;
    }
    return 0;
}

void dat_sheet_free(DatSheet* s)
{
    free(s->sprites);
    free(s->all);
    memset(s, 0, sizeof *s);
}

/* ---- layout ---- */

static void set_color(UiQuad* q, const uint8_t rgba[4][4])
{
    for (int v = 0; v < 4; ++v)
    {
        unsigned r = rgba[v][0] * 2u, g = rgba[v][1] * 2u, b = rgba[v][2] * 2u, a = rgba[v][3] * 2u;
        q->color[v][0] = (uint8_t)(r > 255 ? 255 : r);
        q->color[v][1] = (uint8_t)(g > 255 ? 255 : g);
        q->color[v][2] = (uint8_t)(b > 255 ? 255 : b);
        q->color[v][3] = (uint8_t)(a > 255 ? 255 : a);
    }
}

/* A part placed at x, y with its own extent (in its x/y coordinates) mapped to w by h pixels and
 * its texels repeated tu, tv times across (1 = as the sheet has them). */
static void place(UiQuad* q, const DatPart* p, float x, float y, float w, float h, float tu, float tv)
{
    q->x0 = x;
    q->y0 = y;
    q->x1 = x + w;
    q->y1 = y + h;
    q->u0 = p->u;
    q->v0 = p->v;
    q->u1 = p->u + p->uw * tu;
    q->v1 = p->v + p->uh * tv;
    memset(q->color, 255, sizeof q->color);
    q->image = p->name;
    q->skew = 0;
    q->shape = 0;
    q->blend = UI_BLEND_ALPHA;
}

unsigned ui_window(const DatSheet* win00, float x, float y, float w, float h, float scale, int focused, UiQuad* out)
{
    if (win00->nsprites < 10)
        return 0;
    for (uint32_t i = 0; i < 10; ++i)
        if (win00->sprites[i].nparts != 1)
            return 0;
    const DatPart* bg = win00->sprites[focused ? 9 : 0].parts;
    const DatPart *tl = win00->sprites[1].parts, *top = win00->sprites[2].parts, *tr = win00->sprites[3].parts,
                  *left = win00->sprites[4].parts, *right = win00->sprites[5].parts, *bl = win00->sprites[6].parts,
                  *bot = win00->sprites[7].parts, *br = win00->sprites[8].parts;
    float c = 24 * scale, span_w = w - 2 * c, span_h = h - 2 * c;
    if (span_w < 0)
        span_w = 0;
    if (span_h < 0)
        span_h = 0;
    /* the background tiles at one texel a pixel-per-scale, with the theme's shading */
    place(&out[0], bg, x, y, w, h, w / (bg->uw * scale), h / (bg->uh * scale));
    set_color(&out[0], bg->color);
    /* The corners cut 24-square cells from one framed square (TL at texel 1,1, BR at 7,7). An edge
     * sits across its cell as far in as its texels are from the corner's, so their lines meet: the
     * sheet's own positions put the bottom edge a pixel low (y 20 for texel 26 against 7). */
    place(&out[1], tl, x, y, c, c, 1, 1);
    place(&out[2], tr, x + w - c, y, c, c, 1, 1);
    place(&out[3], bl, x, y + h - c, c, c, 1, 1);
    place(&out[4], br, x + w - c, y + h - c, c, c, 1, 1);
    place(&out[5], top, x + c, y + (top->v - tl->v) * scale, span_w, top->uh * scale, span_w / (top->uw * scale), 1);
    place(&out[6], bot, x + c, y + h - c + (bot->v - bl->v) * scale, span_w, bot->uh * scale,
        span_w / (bot->uw * scale), 1);
    place(&out[7], left, x + (left->u - tl->u) * scale, y + c, left->uw * scale, span_h, 1, span_h / (left->uh * scale));
    place(&out[8], right, x + w - c + (right->u - tr->u) * scale, y + c, right->uw * scale, span_h, 1,
        span_h / (right->uh * scale));
    return UI_WINDOW_QUADS;
}

unsigned ui_sprite(const DatSprite* s, float origin_x, float origin_y, float scale, UiQuad* out, unsigned max)
{
    unsigned n = 0;
    for (uint32_t i = 0; i < s->nparts && n < max; ++i)
    {
        const DatPart* p = &s->parts[i];
        /* TL TR BL BR of a rectangle: TL/BL share x, TL/TR share y */
        if (p->x[0] != p->x[2] || p->x[1] != p->x[3] || p->y[0] != p->y[1] || p->y[2] != p->y[3])
            continue;
        UiQuad* q = &out[n++];
        q->x0 = origin_x + p->x[0] * scale;
        q->x1 = origin_x + p->x[1] * scale;
        q->y0 = origin_y + p->y[0] * scale;
        q->y1 = origin_y + p->y[2] * scale;
        q->u0 = p->u;
        q->v0 = p->v;
        q->u1 = p->u + p->uw;
        q->v1 = p->v + p->uh;
        /* a mirrored part: keep x0 < x1 and swap the texels instead */
        if (q->x0 > q->x1)
        {
            float t = q->x0;
            q->x0 = q->x1;
            q->x1 = t;
            t = q->u0;
            q->u0 = q->u1;
            q->u1 = t;
        }
        if (q->y0 > q->y1)
        {
            float t = q->y0;
            q->y0 = q->y1;
            q->y1 = t;
            t = q->v0;
            q->v0 = q->v1;
            q->v1 = t;
        }
        set_color(q, p->color);
        q->image = p->name;
        q->skew = 0;
        q->shape = 0;
        q->blend = UI_BLEND_ALPHA;
    }
    return n;
}

/* ---- text ---- */

enum
{
    FONT_CELL = 16,
    FONT_ROW = 64,
};

int ui_font(const DatImage* moji, UiFont* f)
{
    if (moji->w != FONT_CELL * FONT_ROW || moji->h < FONT_CELL * 2)
        return 0;
    f->image = "moji";
    f->cell = FONT_CELL;
    for (int c = 0; c < 95; ++c)
    {
        uint32_t x0 = (uint32_t)(c % FONT_ROW) * FONT_CELL, y0 = (uint32_t)(c / FONT_ROW) * FONT_CELL;
        int right = -1;
        for (uint32_t y = 0; y < FONT_CELL; ++y)
            for (uint32_t x = 0; x < FONT_CELL; ++x)
                if (moji->rgba[((y0 + y) * moji->w + x0 + x) * 4 + 3] > 24 && (int)x > right)
                    right = (int)x;
        /* the space has no pixels: a third of the cell, as the game spaces words */
        f->advance[c] = (uint8_t)(right < 0 ? FONT_CELL / 3 : right + 1);
    }
    return 1;
}

float ui_text_width(const UiFont* f, const char* s, float scale)
{
    float w = 0;
    for (; *s; ++s)
        if (*s >= ' ' && *s <= '~')
            w += f->advance[*s - ' '] * scale;
    return w;
}

unsigned ui_text(const UiFont* f, const char* s, float x, float y, float scale, const uint8_t rgba[4], UiQuad* out,
    unsigned max)
{
    unsigned n = 0;
    for (; *s && n < max; ++s)
    {
        if (*s < ' ' || *s > '~')
            continue;
        int c = *s - ' ';
        float adv = f->advance[c] * scale;
        if (*s != ' ')
        {
            UiQuad* q = &out[n++];
            q->x0 = x;
            q->y0 = y;
            q->x1 = x + adv;
            q->y1 = y + f->cell * scale;
            q->u0 = (float)(c % FONT_ROW * FONT_CELL);
            q->v0 = (float)(c / FONT_ROW * FONT_CELL);
            q->u1 = q->u0 + f->advance[c];
            q->v1 = q->v0 + f->cell;
            for (int v = 0; v < 4; ++v)
                memcpy(q->color[v], rgba, 4);
            q->image = f->image;
            q->skew = 0;
            q->shape = 0;
            q->blend = UI_BLEND_ALPHA;
        }
        x += adv;
    }
    return n;
}

/* ---- the menus' letters ---- */

static const UiGlyph GLYPHS[] = {
    { 'A', 77, 26, 9, 11, 26 }, { 'B', 87, 26, 9, 11, 26 }, { 'C', 97, 26, 9, 12, 26 }, { 'D', 107, 26, 9, 11, 26 },
    { 'E', 0, 39, 9, 11, 39 }, { 'F', 10, 39, 9, 11, 39 }, { 'G', 20, 39, 9, 12, 39 }, { 'H', 31, 39, 9, 11, 39 },
    { 'I', 42, 39, 6, 11, 39 }, { 'J', 48, 39, 9, 12, 39 }, { 'K', 58, 39, 9, 11, 39 }, { 'L', 69, 39, 9, 11, 39 },
    { 'M', 80, 39, 9, 11, 39 }, { 'N', 91, 39, 9, 11, 39 }, { 'O', 102, 39, 9, 12, 39 }, { 'P', 113, 39, 9, 11, 39 },
    { 'Q', 0, 52, 9, 12, 52 }, { 'R', 11, 52, 10, 11, 52 }, { 'S', 22, 52, 9, 12, 52 }, { 'T', 31, 52, 9, 11, 52 },
    { 'U', 41, 52, 9, 12, 52 }, { 'V', 52, 52, 10, 11, 52 }, { 'W', 63, 52, 9, 11, 52 }, { 'X', 76, 52, 9, 11, 52 },
    { 'Y', 88, 52, 9, 11, 52 }, { 'Z', 99, 52, 9, 11, 52 },
    { 'a', 49, 67, 8, 9, 65 }, { 'b', 58, 65, 9, 11, 65 }, { 'c', 68, 67, 9, 9, 65 }, { 'd', 78, 65, 9, 11, 65 },
    { 'e', 89, 67, 9, 9, 65 }, { 'f', 99, 65, 7, 11, 65 }, { 'g', 107, 67, 9, 12, 65 }, { 'h', 0, 78, 9, 11, 78 },
    { 'i', 10, 78, 5, 11, 78 }, { 'j', 16, 78, 8, 14, 78 }, { 'k', 25, 78, 9, 11, 78 }, { 'l', 35, 78, 6, 11, 78 },
    { 'm', 42, 80, 9, 9, 78 }, { 'n', 56, 80, 9, 9, 78 }, { 'o', 66, 80, 9, 9, 78 }, { 'p', 76, 80, 9, 12, 78 },
    { 'q', 87, 80, 9, 13, 78 }, { 'r', 98, 80, 7, 9, 78 }, { 's', 106, 80, 9, 9, 78 }, { 't', 0, 92, 6, 10, 91 },
    { 'u', 7, 93, 9, 9, 91 }, { 'v', 17, 93, 9, 9, 91 }, { 'w', 27, 93, 9, 9, 91 }, { 'x', 39, 93, 9, 9, 91 },
    { 'y', 50, 93, 9, 12, 91 }, { 'z', 60, 93, 8, 9, 91 },
    { '0', 40, 13, 9, 11, 13 }, { '1', 51, 13, 7, 11, 13 }, { '2', 61, 13, 8, 11, 13 }, { '3', 71, 13, 8, 11, 13 },
    { '4', 80, 13, 9, 11, 13 }, { '5', 91, 13, 8, 11, 13 }, { '6', 101, 13, 8, 11, 13 }, { '7', 111, 13, 8, 11, 13 },
    { '8', 0, 26, 9, 11, 26 }, { '9', 10, 26, 8, 11, 26 },
    { '.', 23, 20, 4, 4, 13 }, { '-', 11, 17, 8, 4, 13 }, { ':', 18, 28, 5, 8, 26 }, { '/', 30, 13, 9, 11, 13 },
    { '(', 81, 0, 6, 11, 0 }, { ')', 93, 0, 6, 11, 0 }, { '!', 12, 0, 6, 11, 0 }, { '%', 50, 0, 10, 11, 0 },
    { '+', 111, 0, 8, 11, 0 }, { '\'', 72, 0, 5, 7, 0 }, { '?', 56, 26, 9, 11, 26 },
};

const UiGlyph* ui_menu_glyph(char c)
{
    for (size_t i = 0; i < sizeof GLYPHS / sizeof *GLYPHS; ++i)
        if (GLYPHS[i].c == c)
            return &GLYPHS[i];
    return NULL;
}

int ui_menu_text_width(const char* s)
{
    int w = 0;
    for (; *s; ++s)
    {
        const UiGlyph* g = ui_menu_glyph(*s);
        w += g ? g->w - 1 : 4;
    }
    return w;
}

int ui_part_blend(const DatPart* p)
{
    return p->mode[2] == 2 ? UI_BLEND_ADD : p->mode[1] == 2 ? UI_BLEND_SUBTRACT : UI_BLEND_ALPHA;
}
