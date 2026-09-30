/* ThornyFFXI's gdifonts renderer (gdifonttexture.dll, GDI+ on Windows) for addons' ffi: text and
 * rounded rectangles drawn into IDirect3DTexture8 objects (d3d_ffi.c), which the gdifonts Lua
 * library then draws with an ID3DXSprite. tHotBar, tCrossBar and other addons carry the library;
 * its `ffi.load('...gdifonttexture.dll')` is the ffi.C namespace here (xi.lua), so these are
 * exported from host64 under the DLL's names and with its structures (gdifonts' include.lua).
 *
 * Text: stb_truetype, fonts found as the overlay's text objects find them (xi_gui_font_file);
 * FontHeight is the em size in pixels, as GDI+'s UnitPixel. Bold or italic without a font file
 * for the style is made (a pixel wider, a slant). The outline is a pen OutlineWidth wide around
 * the glyphs' edges (half outside), under the fill; gradients run the fill colour to
 * GradientColor in the eight GDI+ directions. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d_ffi.h"
#include "host.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef struct
{
    int32_t BoxHeight, BoxWidth;
    float FontHeight, OutlineWidth;
    uint32_t FontFlags, FontColor, OutlineColor, GradientStyle, GradientColor;
    char FontFamily[256];
    char FontText[4096];
} GdiFontData;

typedef struct
{
    int32_t Width, Height, Diameter;
    uint32_t OutlineColor, OutlineWidth, FillColor, GradientStyle, GradientColor;
} GdiRectData;

typedef struct
{
    int32_t Width, Height;
    void* Texture;
} GdiFontReturn;

enum
{
    F_BOLD = 1,
    F_ITALIC = 2,
    F_UNDERLINE = 4,
    F_STRIKEOUT = 8,
};

typedef struct Manager
{
    uint32_t magic;
    char dump[1024];
    unsigned dumped;
} Manager;

#define MANAGER_MAGIC 0x47444946u /* "GDIF" */

/* --- fonts ---------------------------------------------------------------------------------- */

typedef struct Face
{
    char key[300];
    uint8_t* data;
    stbtt_fontinfo info;
    int ok, fake_bold, fake_italic;
    struct Face* next;
} Face;

static Face* g_faces;

static int contains_ci(const char* s, const char* w)
{
    size_t n = strlen(w);
    for (; *s; ++s)
    {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)w[i]))
            ++i;
        if (i == n)
            return 1;
    }
    return 0;
}

static Face* face_for(const char* family, int bold, int italic)
{
    char key[300];
    snprintf(key, sizeof key, "%s|%d%d", family, bold, italic);
    for (Face* f = g_faces; f; f = f->next)
        if (!strcmp(f->key, key))
            return f->ok ? f : NULL;
    Face* f = (Face*)calloc(1, sizeof *f);
    if (!f)
        return NULL;
    snprintf(f->key, sizeof f->key, "%s", key);
    f->next = g_faces, g_faces = f;
    char path[1200];
    if (!xi_gui_font_file(family, bold, italic, path, sizeof path) && !xi_gui_font_file("Arial", bold, italic, path, sizeof path) &&
        !xi_gui_font_file("Helvetica", 0, 0, path, sizeof path))
    {
        xi_log_once(key, "gdifonts: no font for %s", family);
        return NULL;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n)
    {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off))
    {
        xi_log_once(key, "gdifonts: can't read %s", path);
        return NULL;
    }
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    f->fake_bold = bold && !contains_ci(base, "bold");
    f->fake_italic = italic && !contains_ci(base, "italic") && !contains_ci(base, "oblique");
    f->ok = 1;
    return f;
}

static uint32_t utf8_next(const unsigned char** p)
{
    const unsigned char* s = *p;
    uint32_t c = *s++;
    int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (k)
        c &= 0x3Fu >> k;
    for (; k && (*s & 0xC0) == 0x80; --k)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

/* --- pixels --------------------------------------------------------------------------------- */

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static float gradient_t(uint32_t style, float x, float y, float w, float h)
{
    float u = w > 1 ? x / (w - 1) : 0, v = h > 1 ? y / (h - 1) : 0;
    switch (style)
    {
    case 1: return u;
    case 2: return (u + v) * 0.5f;
    case 3: return v;
    case 4: return ((1 - u) + v) * 0.5f;
    case 5: return 1 - u;
    case 6: return 1 - (u + v) * 0.5f;
    case 7: return 1 - v;
    case 8: return (u + 1 - v) * 0.5f;
    default: return 0;
    }
}

/* ARGB colour at (x, y) of a fill with a gradient */
static void fill_color(uint32_t c0, uint32_t c1, uint32_t style, float x, float y, float w, float h, float out[4])
{
    float t = style ? clampf(gradient_t(style, x, y, w, h), 0, 1) : 0;
    for (int k = 0; k < 4; ++k)
    {
        float a = (float)((c0 >> (8 * k)) & 0xFF), b = (float)((c1 >> (8 * k)) & 0xFF);
        out[k] = a + (b - a) * t; /* b g r a */
    }
}

/* fill (coverage cf, colour f) over outline (coverage co, colour o) into BGRA */
static void compose(uint8_t* d, float cf, const float f[4], float co, const float o[4])
{
    float af = cf * f[3] / 255.0f, ao = co * o[3] / 255.0f;
    float a = af + ao * (1 - af);
    if (a <= 0)
    {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    for (int k = 0; k < 3; ++k)
        d[k] = (uint8_t)clampf((f[k] * af + o[k] * ao * (1 - af)) / a + 0.5f, 0, 255);
    d[3] = (uint8_t)clampf(a * 255.0f + 0.5f, 0, 255);
}

/* Grows a coverage mask by radius r (pixels), soft at the edge: what a pen r wide outside draws. */
static float* dilate(const float* m, int w, int h, float r)
{
    float* o = (float*)calloc((size_t)w * h, sizeof *o);
    if (!o)
        return NULL;
    int R = (int)ceilf(r);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float best = 0;
            for (int dy = -R; dy <= R && best < 1; ++dy)
            {
                int yy = y + dy;
                if (yy < 0 || yy >= h)
                    continue;
                for (int dx = -R; dx <= R; ++dx)
                {
                    int xx = x + dx;
                    if (xx < 0 || xx >= w)
                        continue;
                    float v = m[(size_t)yy * w + xx];
                    if (v <= best)
                        continue;
                    float edge = clampf(r + 0.5f - sqrtf((float)(dx * dx + dy * dy)), 0, 1);
                    if (v * edge > best)
                        best = v * edge;
                }
            }
            o[(size_t)y * w + x] = best;
        }
    return o;
}

static void dump(Manager* m, const char* what, const uint8_t* bgra, int w, int h)
{
    if (!m || !m->dump[0])
        return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s_%04u.bmp", m->dump, what, m->dumped++);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t size = 54 + (uint32_t)w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t v = 54;
    memcpy(hdr + 10, &v, 4);
    v = 40;
    memcpy(hdr + 14, &v, 4);
    int32_t iw = w, ih = -h;
    memcpy(hdr + 18, &iw, 4);
    memcpy(hdr + 22, &ih, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(bgra, 4, (size_t)w * h, f);
    fclose(f);
}

/* --- the exports ---------------------------------------------------------------------------- */

XI_FFI uint32_t* CreateFontManager(void* device)
{
    (void)device;
    Manager* m = (Manager*)calloc(1, sizeof *m);
    if (m)
        m->magic = MANAGER_MAGIC;
    return (uint32_t*)m;
}

static Manager* manager(uint32_t* p) { return p && ((Manager*)p)->magic == MANAGER_MAGIC ? (Manager*)p : NULL; }

XI_FFI void DestroyFontManager(uint32_t* p)
{
    Manager* m = manager(p);
    if (m)
    {
        m->magic = 0;
        free(m);
    }
}

XI_FFI void EnableTextureDump(uint32_t* p, const char* folder)
{
    Manager* m = manager(p);
    if (m && folder)
        xi_host_path(folder, m->dump, sizeof m->dump);
}

XI_FFI void DisableTextureDump(uint32_t* p)
{
    Manager* m = manager(p);
    if (m)
        m->dump[0] = 0;
}

XI_FFI int GetFontAvailable(const char* family)
{
    char path[1200];
    return family && xi_gui_font_file(family, 0, 0, path, sizeof path);
}

#define MAX_LINES 128

XI_FFI GdiFontReturn CreateTexture(uint32_t* mgr, const GdiFontData* d)
{
    GdiFontReturn r = { 0, 0, NULL };
    if (!d || !d->FontText[0] || d->FontHeight <= 0 || d->FontHeight > 512)
        return r;
    int bold = (d->FontFlags & F_BOLD) != 0, italic = (d->FontFlags & F_ITALIC) != 0;
    char family[256];
    memcpy(family, d->FontFamily, sizeof family);
    family[255] = 0;
    Face* f = face_for(family[0] ? family : "Arial", bold, italic);
    if (!f)
        return r;
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, d->FontHeight);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    float line_h = (float)(asc - desc + gap) * scale, ascent = (float)asc * scale;

    /* lines and their widths */
    char text[4097];
    memcpy(text, d->FontText, 4096);
    text[4096] = 0;
    const char* lines[MAX_LINES];
    float widths[MAX_LINES];
    int nl = 0;
    for (char *s = text, *e; s && nl < MAX_LINES; s = e ? e + 1 : NULL)
    {
        e = strchr(s, '\n');
        if (e)
            *e = 0;
        size_t len = strlen(s);
        if (len && s[len - 1] == '\r')
            s[len - 1] = 0;
        lines[nl] = s;
        float w = 0;
        int prev = 0;
        for (const unsigned char* p = (const unsigned char*)s; *p;)
        {
            int c = (int)utf8_next(&p);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);
            if (prev)
                w += (float)stbtt_GetCodepointKernAdvance(&f->info, prev, c) * scale;
            w += (float)adv * scale;
            prev = c;
        }
        widths[nl++] = w;
    }
    float maxw = 0;
    for (int i = 0; i < nl; ++i)
        if (widths[i] > maxw)
            maxw = widths[i];
    float ow = d->OutlineWidth > 0 ? d->OutlineWidth : 0;
    float slant = f->fake_italic ? 0.2f : 0;
    int pad = (int)ceilf(ow * 0.5f) + 1;
    int W = d->BoxWidth > 0 ? d->BoxWidth : (int)ceilf(maxw + slant * line_h + (f->fake_bold ? 1 : 0)) + 2 * pad;
    int H = d->BoxHeight > 0 ? d->BoxHeight : (int)ceilf(line_h * (float)nl) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192)
        return r;

    float* mask = (float*)calloc((size_t)W * H, sizeof *mask);
    if (!mask)
        return r;
    for (int li = 0; li < nl; ++li)
    {
        float x = (float)pad, base = (float)pad + (float)li * line_h + ascent;
        int prev = 0;
        for (const unsigned char* p = (const unsigned char*)lines[li]; *p;)
        {
            int c = (int)utf8_next(&p);
            if (prev)
                x += (float)stbtt_GetCodepointKernAdvance(&f->info, prev, c) * scale;
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);
            int gx0, gy0, gx1, gy1;
            float sx = x - floorf(x);
            stbtt_GetCodepointBitmapBoxSubpixel(&f->info, c, scale, scale, sx, 0, &gx0, &gy0, &gx1, &gy1);
            int gw = gx1 - gx0, gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw < 1024 && gh < 1024)
            {
                unsigned char* g = (unsigned char*)malloc((size_t)gw * gh);
                if (g)
                {
                    stbtt_MakeCodepointBitmapSubpixel(&f->info, g, gw, gh, gw, scale, scale, sx, 0, c);
                    int ox = (int)floorf(x) + gx0, oy = (int)floorf(base) + gy0;
                    for (int y = 0; y < gh; ++y)
                    {
                        int yy = oy + y;
                        if (yy < 0 || yy >= H)
                            continue;
                        /* a slant made for italic: rows above the baseline move right */
                        float shift = slant * (base - (float)yy);
                        int ish = (int)floorf(shift);
                        float fr = shift - (float)ish;
                        for (int xg = 0; xg < gw; ++xg)
                        {
                            float v = g[(size_t)y * gw + xg] / 255.0f;
                            if (v <= 0)
                                continue;
                            for (int k = 0; k < 2; ++k)
                            {
                                int xx = ox + xg + ish + k;
                                float part = v * (k ? fr : 1 - fr);
                                if (xx >= 0 && xx < W && part > 0)
                                {
                                    float* m = &mask[(size_t)yy * W + xx];
                                    *m = clampf(*m + part, 0, 1);
                                }
                            }
                        }
                    }
                    free(g);
                }
            }
            x += (float)adv * scale;
            prev = c;
        }
        /* underline and strikeout */
        float th = d->FontHeight / 14.0f < 1 ? 1 : d->FontHeight / 14.0f;
        for (int which = 0; which < 2; ++which)
        {
            if (!(d->FontFlags & (which ? F_STRIKEOUT : F_UNDERLINE)))
                continue;
            float y0 = which ? base - d->FontHeight * 0.3f : base + d->FontHeight * 0.1f;
            for (int yy = (int)floorf(y0); yy < (int)ceilf(y0 + th) && yy < H; ++yy)
                for (int xx = pad; xx < pad + (int)ceilf(widths[li]) && xx < W; ++xx)
                    if (yy >= 0)
                        mask[(size_t)yy * W + xx] = 1;
        }
    }
    if (f->fake_bold) /* a pixel wider */
        for (int y = 0; y < H; ++y)
            for (int x = W - 1; x > 0; --x)
            {
                float* m = &mask[(size_t)y * W + x];
                float l = m[-1];
                if (l > *m)
                    *m = l;
            }
    float* outline = ow > 0 && (d->OutlineColor >> 24) ? dilate(mask, W, H, ow * 0.5f) : NULL;
    uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
    if (!px)
    {
        free(mask);
        free(outline);
        return r;
    }
    float oc[4];
    fill_color(d->OutlineColor, d->OutlineColor, 0, 0, 0, 1, 1, oc);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float fc[4];
            fill_color(d->FontColor, d->GradientColor, d->GradientStyle, (float)x, (float)y, (float)W, (float)H, fc);
            size_t i = (size_t)y * W + x;
            compose(px + i * 4, mask[i], fc, outline ? outline[i] : 0, oc);
        }
    free(mask);
    free(outline);
    dump(manager(mgr), "font", px, W, H);
    r.Texture = xi_d3d_texture_bgra(px, W, H);
    free(px);
    if (r.Texture)
        r.Width = W, r.Height = H;
    return r;
}

/* signed distance from p to a rounded rectangle [x0,x1]x[y0,y1] with corner radius rad */
static float rr_dist(float px, float py, float x0, float y0, float x1, float y1, float rad)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hx = (x1 - x0) * 0.5f - rad, hy = (y1 - y0) * 0.5f - rad;
    float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad;
}

XI_FFI GdiFontReturn CreateRectTexture(uint32_t* mgr, const GdiRectData* d)
{
    GdiFontReturn r = { 0, 0, NULL };
    if (!d || d->Width <= 0 || d->Height <= 0 || d->Width > 8192 || d->Height > 8192)
        return r;
    int W = d->Width, H = d->Height;
    float ow = (float)d->OutlineWidth;
    float rad = d->Diameter > 0 ? (float)d->Diameter * 0.5f : 0;
    float lim = (float)(W < H ? W : H) * 0.5f;
    if (rad > lim)
        rad = lim;
    uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
    if (!px)
        return r;
    float oc[4];
    fill_color(d->OutlineColor, d->OutlineColor, 0, 0, 0, 1, 1, oc);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            float dout = rr_dist(cx, cy, 0, 0, (float)W, (float)H, rad);
            float rin = rad - ow > 0 ? rad - ow : 0;
            float din = ow > 0 ? rr_dist(cx, cy, ow, ow, (float)W - ow, (float)H - ow, rin) : dout;
            float cov_out = clampf(0.5f - dout, 0, 1), cov_in = clampf(0.5f - din, 0, 1);
            float fc[4];
            fill_color(d->FillColor, d->GradientColor, d->GradientStyle, (float)x, (float)y, (float)W, (float)H, fc);
            compose(px + ((size_t)y * W + x) * 4, cov_in, fc, ow > 0 ? cov_out - cov_in * cov_out : 0, oc);
        }
    dump(manager(mgr), "rect", px, W, H);
    r.Texture = xi_d3d_texture_bgra(px, W, H);
    free(px);
    if (r.Texture)
        r.Width = W, r.Height = H;
    return r;
}
