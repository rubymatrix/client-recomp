/* The addon host's overlay: Dear ImGui (the exact build Ashita v4 wraps: 1.92.3 WIP docking,
 * a28cb615), text objects (Ashita's font objects, Windower's texts), primitives (Ashita's
 * primitives, Windower's images) and textures, drawn over the game's frame through the graphics
 * back end (gfx.h) - so Metal and D3D12 both get it with no back-end code.
 *
 * One ImGui context for every addon. Coordinates are the back buffer's pixels (the game's).
 * Frame: addons_frame (Present hook) runs xi_gui_begin, the addons' drawing events, xi_gui_end;
 * then d3d8's overlay hook (just before the frame goes out) draws text objects and primitives in
 * creation order, then ImGui's draw data.
 *
 * Texture ids: ImTextureID and the ids Lua sees are small 32-bit numbers (Ashita addons pass a
 * texture as `tonumber(ffi.cast('uint32_t', tex))`, so an id must survive being cut to 32 bits). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
#include "d3d8.h"
#include "gfx.h"
#include "host.h"
#include "plat.h"
}

#include <SDL3/SDL.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* --- assertions ------------------------------------------------------------------------------- */

extern "C" void xi_imgui_assert(const char* expr, const char* file, int line)
{
    char key[512];
    snprintf(key, sizeof key, "imgui %s:%d", file, line);
    Addon* a = xi_current();
    xi_log_once(key, "ImGui assertion failed%s%s: %s (%s:%d)", a ? " in " : "", a ? a->name : "", expr, file, line);
}

/* --- textures --------------------------------------------------------------------------------- */

struct XiTex
{
    uint32_t id;
    GfxTex* gpu;
    int w, h;
    Addon* owner; /* NULL: the host's (ImGui's atlas) */
};

static std::unordered_map<uint32_t, XiTex*> g_tex;
static uint32_t g_next_tex = 0x100;
static GfxTex* g_white;

static XiTex* tex_new(int w, int h, Addon* owner)
{
    XiTex* t = new XiTex();
    t->id = g_next_tex++;
    t->w = w, t->h = h;
    t->owner = owner;
    t->gpu = xi_headless ? NULL : gfx_tex_create(GFX_TEX_2D, 21 /* A8R8G8B8 */, (uint32_t)w, (uint32_t)h, 1, GFX_USE_SAMPLE);
    g_tex[t->id] = t;
    return t;
}

static void tex_free(XiTex* t)
{
    g_tex.erase(t->id);
    if (t->gpu)
        gfx_tex_destroy(t->gpu);
    delete t;
}

static XiTex* tex_of(uint64_t id)
{
    auto it = g_tex.find((uint32_t)id);
    return it == g_tex.end() ? NULL : it->second;
}

/* RGBA bytes -> the back end's A8R8G8B8 (B, G, R, A in memory) */
static void upload_rgba(XiTex* t, const uint8_t* rgba, int x, int y, int w, int h, int pitch)
{
    if (!t->gpu)
        return;
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (int j = 0; j < h; ++j)
    {
        const uint8_t* s = rgba + (size_t)j * pitch;
        uint8_t* d = &bgra[(size_t)j * w * 4];
        for (int i = 0; i < w; ++i, s += 4, d += 4)
            d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
    }
    if (x == 0 && y == 0 && w == t->w && h == t->h)
        gfx_tex_upload(t->gpu, 0, 0, bgra.data(), (uint32_t)w * 4);
    else
        gfx_tex_upload_rect(t->gpu, 0, 0, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, bgra.data(), (uint32_t)w * 4);
}

/* An image file (PNG, BMP, TGA, JPEG...) as a texture: its id, or 0. */
extern "C" uint32_t xi_gui_texture_file(const char* path, int* w, int* h)
{
    char host[1200];
    xi_host_path(path, host, sizeof host);
    int iw, ih, n;
    uint8_t* px = stbi_load(host, &iw, &ih, &n, 4);
    if (!px)
        return 0;
    XiTex* t = tex_new(iw, ih, xi_current());
    upload_rgba(t, px, 0, 0, iw, ih, iw * 4);
    stbi_image_free(px);
    if (w)
        *w = iw;
    if (h)
        *h = ih;
    return t->id;
}

/* An image in memory (a file's bytes), or raw pixels when w and h are given: RGBA (or BGRA when bgra). */
extern "C" uint32_t xi_gui_texture_memory(const uint8_t* data, size_t n, int w, int h, int bgra, int* ow, int* oh)
{
    if (w > 0 && h > 0)
    {
        if (n < (size_t)w * h * 4)
            return 0;
        std::vector<uint8_t> rgba(data, data + (size_t)w * h * 4);
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4)
                std::swap(rgba[i], rgba[i + 2]);
        XiTex* t = tex_new(w, h, xi_current());
        upload_rgba(t, rgba.data(), 0, 0, w, h, w * 4);
        if (ow)
            *ow = w;
        if (oh)
            *oh = h;
        return t->id;
    }
    int iw, ih, c;
    uint8_t* px = stbi_load_from_memory(data, (int)n, &iw, &ih, &c, 4);
    if (!px)
        return 0;
    XiTex* t = tex_new(iw, ih, xi_current());
    upload_rgba(t, px, 0, 0, iw, ih, iw * 4);
    stbi_image_free(px);
    if (ow)
        *ow = iw;
    if (oh)
        *oh = ih;
    return t->id;
}

extern "C" void xi_gui_texture_free(uint32_t id)
{
    XiTex* t = tex_of(id);
    if (t && t->owner)
        tex_free(t);
}

extern "C" int xi_gui_texture_size(uint32_t id, int* w, int* h)
{
    XiTex* t = tex_of(id);
    if (!t)
        return 0;
    *w = t->w, *h = t->h;
    return 1;
}

/* --- text objects and primitives ------------------------------------------------------------ */

struct XiText
{
    uint32_t id;
    Addon* owner;
    std::string text;
    std::string family = "Arial";
    float size = 12.0f; /* pixels */
    int bold = 0, italic = 0, visible = 1;
    float x = 0, y = 0;
    uint32_t color = 0xFFFFFFFF;        /* ARGB */
    uint32_t stroke_color = 0xFF000000; /* ARGB */
    float stroke = 0;                   /* outline width, pixels */
    int bg_visible = 0;
    uint32_t bg_color = 0x80000000;
    float pad = 0;
    int right = 0, bottom = 0; /* x is the right edge / y the bottom edge */
    float w_fixed = 0, h_fixed = 0; /* > 0: the box is this size */
    uint32_t parent = 0;
    int anchor = 0; /* Ashita's FrameAnchor: 0 top left, 1 top right, 2 bottom left, 3 bottom right */
    int locked = 0, can_focus = 1;
    float ext_w = 0, ext_h = 0; /* as drawn last */
    uint32_t order;
};

struct XiPrim
{
    uint32_t id;
    Addon* owner;
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0xFFFFFFFF; /* ARGB */
    uint32_t tex = 0;
    int visible = 1;
    int fit = 1, repeat = 0;
    float tex_x = 0, tex_y = 0, scale_x = 1, scale_y = 1;
    int locked = 0, can_focus = 1;
    uint32_t order;
};

static std::vector<XiText*> g_texts;
static std::vector<XiPrim*> g_prims;
static uint32_t g_next_obj = 1, g_order;

extern "C" uint32_t xi_text_new(void)
{
    XiText* t = new XiText();
    t->id = g_next_obj++;
    t->owner = xi_current();
    t->order = g_order++;
    g_texts.push_back(t);
    return t->id;
}

static XiText* text_of(uint32_t id)
{
    for (XiText* t : g_texts)
        if (t->id == id)
            return t;
    return NULL;
}

extern "C" void xi_text_delete(uint32_t id)
{
    for (size_t i = 0; i < g_texts.size(); ++i)
        if (g_texts[i]->id == id)
        {
            delete g_texts[i];
            g_texts.erase(g_texts.begin() + (long)i);
            return;
        }
}

extern "C" uint32_t xi_prim_new(void)
{
    XiPrim* p = new XiPrim();
    p->id = g_next_obj++;
    p->owner = xi_current();
    p->order = g_order++;
    g_prims.push_back(p);
    return p->id;
}

static XiPrim* prim_of(uint32_t id)
{
    for (XiPrim* p : g_prims)
        if (p->id == id)
            return p;
    return NULL;
}

extern "C" void xi_prim_delete(uint32_t id)
{
    for (size_t i = 0; i < g_prims.size(); ++i)
        if (g_prims[i]->id == id)
        {
            delete g_prims[i];
            g_prims.erase(g_prims.begin() + (long)i);
            return;
        }
}

/* Fields by name, as numbers or strings: the Lua layers read and write objects through these. */
extern "C" int xi_text_set(uint32_t id, const char* f, double n, const char* s)
{
    XiText* t = text_of(id);
    if (!t)
        return 0;
#define NUM(name, field) else if (!strcmp(f, name)) t->field = (decltype(t->field))n
    if (!strcmp(f, "text"))
        t->text = s ? s : "";
    else if (!strcmp(f, "font"))
        t->family = s ? s : "Arial";
    NUM("size", size); NUM("bold", bold); NUM("italic", italic); NUM("visible", visible); NUM("x", x); NUM("y", y);
    NUM("color", color); NUM("stroke_color", stroke_color); NUM("stroke", stroke); NUM("bg_visible", bg_visible);
    NUM("bg_color", bg_color); NUM("pad", pad); NUM("right", right); NUM("bottom", bottom); NUM("width", w_fixed);
    NUM("height", h_fixed); NUM("parent", parent); NUM("anchor", anchor); NUM("locked", locked);
    NUM("can_focus", can_focus);
    else return 0;
#undef NUM
    return 1;
}

static void text_extents(XiText* t, float* w, float* h);

extern "C" int xi_text_get(uint32_t id, const char* f, double* n, const char** s)
{
    XiText* t = text_of(id);
    if (!t)
        return 0;
    *s = NULL;
#define NUM(name, field) else if (!strcmp(f, name)) *n = (double)t->field
    if (!strcmp(f, "text"))
        *s = t->text.c_str();
    else if (!strcmp(f, "font"))
        *s = t->family.c_str();
    else if (!strcmp(f, "extent_w") || !strcmp(f, "extent_h"))
    {
        float w, h;
        text_extents(t, &w, &h);
        *n = f[7] == 'w' ? w : h;
    }
    NUM("size", size); NUM("bold", bold); NUM("italic", italic); NUM("visible", visible); NUM("x", x); NUM("y", y);
    NUM("color", color); NUM("stroke_color", stroke_color); NUM("stroke", stroke); NUM("bg_visible", bg_visible);
    NUM("bg_color", bg_color); NUM("pad", pad); NUM("right", right); NUM("bottom", bottom); NUM("width", w_fixed);
    NUM("height", h_fixed); NUM("parent", parent); NUM("anchor", anchor); NUM("locked", locked);
    NUM("can_focus", can_focus);
    else return 0;
#undef NUM
    return 1;
}

extern "C" int xi_prim_set(uint32_t id, const char* f, double n)
{
    XiPrim* p = prim_of(id);
    if (!p)
        return 0;
#define NUM(name, field) else if (!strcmp(f, name)) p->field = (decltype(p->field))n
    if (0)
        ;
    NUM("x", x); NUM("y", y); NUM("width", w); NUM("height", h); NUM("color", color); NUM("texture", tex);
    NUM("visible", visible); NUM("fit", fit); NUM("repeat", repeat); NUM("tex_x", tex_x); NUM("tex_y", tex_y);
    NUM("scale_x", scale_x); NUM("scale_y", scale_y); NUM("locked", locked); NUM("can_focus", can_focus);
    else return 0;
#undef NUM
    return 1;
}

extern "C" int xi_prim_get(uint32_t id, const char* f, double* n)
{
    XiPrim* p = prim_of(id);
    if (!p)
        return 0;
#define NUM(name, field) else if (!strcmp(f, name)) *n = (double)p->field
    if (0)
        ;
    NUM("x", x); NUM("y", y); NUM("width", w); NUM("height", h); NUM("color", color); NUM("texture", tex);
    NUM("visible", visible); NUM("fit", fit); NUM("repeat", repeat); NUM("tex_x", tex_x); NUM("tex_y", tex_y);
    NUM("scale_x", scale_x); NUM("scale_y", scale_y); NUM("locked", locked); NUM("can_focus", can_focus);
    else return 0;
#undef NUM
    return 1;
}

/* --- fonts ------------------------------------------------------------------------------------ */

/* A family (and style) to a font file: the system's font folders, then <data dir>/fonts/. */
static std::unordered_map<std::string, ImFont*> g_fonts;
static ImFont* g_default_font;

static int file_exists(const char* p)
{
    PlatStat st;
    return plat_stat(p, &st);
}

static std::string font_file(const std::string& family, int bold, int italic)
{
    static const char* const DIRS[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/", "/System/Library/Fonts/", "/Library/Fonts/",
#elif defined(_WIN32)
        "C:\\Windows\\Fonts\\",
#else
        "/usr/share/fonts/truetype/", "/usr/share/fonts/",
#endif
    };
    /* Windows families the addons ask for, and what stands in for them elsewhere */
    static const struct
    {
        const char* family;
        const char* alt;
    } ALT[] = { { "consolas", "Menlo" }, { "segoe ui", "Helvetica" }, { "tahoma", "Verdana" }, { "ms gothic", "Osaka" },
                { "meiryo", "Hiragino Sans GB" }, { "lucida console", "Menlo" }, { "calibri", "Helvetica" } };
    std::vector<std::string> names;
    std::string style = bold && italic ? " Bold Italic" : bold ? " Bold" : italic ? " Italic" : "";
    names.push_back(family + style);
    names.push_back(family);
    std::string low = family;
    for (char& c : low)
        c = (char)tolower((unsigned char)c);
    for (auto& a : ALT)
        if (low == a.family)
            names.push_back(std::string(a.alt) + style), names.push_back(a.alt);
    std::string data = std::string(xi_data_dir()) + "fonts/";
    for (const std::string& n : names)
    {
        for (const char* ext : { ".ttf", ".otf", ".ttc", ".TTF" })
        {
            std::string p = data + n + ext;
            if (file_exists(p.c_str()))
                return p;
            for (const char* d : DIRS)
            {
                p = std::string(d) + n + ext;
                if (file_exists(p.c_str()))
                    return p;
            }
        }
    }
    return std::string();
}

static ImFont* font_for(const std::string& family, int bold, int italic)
{
    std::string key = family + (bold ? "|b" : "|") + (italic ? "i" : "");
    auto it = g_fonts.find(key);
    if (it != g_fonts.end())
        return it->second;
    ImFont* f = NULL;
    std::string path = font_file(family, bold, italic);
    if (!path.empty())
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &cfg);
    }
    if (!f)
    {
        xi_log_once(key.c_str(), "font %s%s%s: not found, the default font stands in", family.c_str(), bold ? " bold" : "",
            italic ? " italic" : "");
        f = g_default_font;
    }
    g_fonts[key] = f;
    return f;
}

/* --- colour codes in text --------------------------------------------------------------------- */

/* Windower's \cs(r,g,b) ... \cr and Ashita's |cAARRGGBB| ... |r: runs of one colour. */
struct Run
{
    const char *s, *e;
    uint32_t color; /* ARGB */
};

static void runs_of(const std::string& text, uint32_t base, std::vector<Run>& out)
{
    const char* p = text.c_str();
    const char* start = p;
    uint32_t cur = base;
    while (*p)
    {
        if (p[0] == '\\' && p[1] == 'c' && p[2] == 's' && p[3] == '(')
        {
            int r, g, b, n = 0;
            if (sscanf(p + 4, "%d ,%d ,%d )%n", &r, &g, &b, &n) == 3 && n)
            {
                if (p > start)
                    out.push_back({ start, p, cur });
                cur = (base & 0xFF000000u) | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
                p += 4 + n;
                start = p;
                continue;
            }
        }
        if (p[0] == '\\' && p[1] == 'c' && p[2] == 'r')
        {
            if (p > start)
                out.push_back({ start, p, cur });
            cur = base;
            p += 3;
            start = p;
            continue;
        }
        if (p[0] == '|' && p[1] == 'c' && strlen(p) >= 11 && p[10] == '|')
        {
            if (p > start)
                out.push_back({ start, p, cur });
            cur = (uint32_t)strtoul(std::string(p + 2, 8).c_str(), NULL, 16);
            p += 11;
            start = p;
            continue;
        }
        if (p[0] == '|' && p[1] == 'r')
        {
            if (p > start)
                out.push_back({ start, p, cur });
            cur = base;
            p += 2;
            start = p;
            continue;
        }
        ++p;
    }
    if (p > start)
        out.push_back({ start, p, cur });
}

static ImU32 im_color(uint32_t argb)
{
    return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24);
}

/* The text's size without its box, line by line (colour codes take no room). */
static ImVec2 measure(ImFont* f, float size, const std::string& text)
{
    std::vector<Run> runs;
    runs_of(text, 0, runs);
    std::string plain;
    for (const Run& r : runs)
        plain.append(r.s, r.e);
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, plain.c_str());
}

static void text_extents(XiText* t, float* w, float* h)
{
    if (!ImGui::GetCurrentContext())
    {
        *w = *h = 0;
        return;
    }
    ImFont* f = font_for(t->family, t->bold, t->italic);
    ImVec2 sz = measure(f, t->size, t->text);
    *w = t->w_fixed > 0 ? t->w_fixed : sz.x + t->pad * 2;
    *h = t->h_fixed > 0 ? t->h_fixed : sz.y + t->pad * 2;
}

/* The top left of a text object's box, following its parent and anchor. */
static void text_origin(XiText* t, float w, float h, float* ox, float* oy, int depth)
{
    float x = t->x, y = t->y;
    if (t->parent && depth < 8)
    {
        XiText* p = text_of(t->parent);
        if (p)
        {
            float pw, ph, px, py;
            text_extents(p, &pw, &ph);
            text_origin(p, pw, ph, &px, &py, depth + 1);
            x += (t->anchor & 1) ? px + pw : px;
            y += (t->anchor & 2) ? py + ph : py;
        }
    }
    if (t->right)
        x -= w;
    if (t->bottom)
        y -= h;
    *ox = x, *oy = y;
}

static void draw_text(ImDrawList* dl, XiText* t)
{
    ImFont* f = font_for(t->family, t->bold, t->italic);
    float w, h, x, y;
    text_extents(t, &w, &h);
    text_origin(t, w, h, &x, &y, 0);
    t->ext_w = w, t->ext_h = h;
    if (t->bg_visible)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), im_color(t->bg_color));
    std::vector<Run> runs;
    runs_of(t->text, t->color, runs);
    float cx = x + t->pad, cy = y + t->pad, line_h = f->CalcTextSizeA(t->size, FLT_MAX, 0, "A").y;
    for (const Run& r : runs)
    {
        const char* s = r.s;
        while (s < r.e)
        {
            const char* nl = (const char*)memchr(s, '\n', (size_t)(r.e - s));
            const char* e = nl ? nl : r.e;
            if (e > s)
            {
                if (t->stroke > 0)
                {
                    ImU32 sc = im_color(t->stroke_color);
                    float k = t->stroke;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (dx || dy)
                                dl->AddText(f, t->size, ImVec2(cx + dx * k, cy + dy * k), sc, s, e);
                }
                dl->AddText(f, t->size, ImVec2(cx, cy), im_color(r.color), s, e);
                if (t->bold && f == g_default_font)
                    dl->AddText(f, t->size, ImVec2(cx + 1, cy), im_color(r.color), s, e);
                cx += f->CalcTextSizeA(t->size, FLT_MAX, 0, s, e).x;
            }
            if (nl)
            {
                cx = x + t->pad;
                cy += line_h;
                s = nl + 1;
            }
            else
                s = e;
        }
    }
}

static void draw_prim(ImDrawList* dl, XiPrim* p)
{
    ImVec2 a(p->x, p->y), b(p->x + p->w * p->scale_x, p->y + p->h * p->scale_y);
    ImU32 c = im_color(p->color);
    XiTex* t = p->tex ? tex_of(p->tex) : NULL;
    if (!t)
    {
        dl->AddRectFilled(a, b, c);
        return;
    }
    ImVec2 uv0(p->tex_x / t->w, p->tex_y / t->h), uv1(1, 1);
    if (p->repeat)
        uv1 = ImVec2(uv0.x + (b.x - a.x) / t->w, uv0.y + (b.y - a.y) / t->h);
    else if (!p->fit)
        uv1 = ImVec2(uv0.x + p->w / t->w, uv0.y + p->h / t->h);
    dl->AddImage(ImTextureRef((ImTextureID)t->id), a, b, uv0, uv1, c);
}

/* --- ImGui's textures (1.92: the renderer creates and updates them) ---------------------------- */

static void update_texture(ImTextureData* td)
{
    if (td->Status == ImTextureStatus_WantCreate)
    {
        XiTex* t = tex_new(td->Width, td->Height, NULL);
        const uint8_t* px = (const uint8_t*)td->GetPixels();
        if (td->Format == ImTextureFormat_RGBA32)
            upload_rgba(t, px, 0, 0, td->Width, td->Height, td->GetPitch());
        else
        {
            std::vector<uint8_t> rgba((size_t)td->Width * td->Height * 4);
            for (int j = 0; j < td->Height; ++j)
                for (int i = 0; i < td->Width; ++i)
                {
                    uint8_t a = px[(size_t)j * td->GetPitch() + i];
                    uint8_t* d = &rgba[((size_t)j * td->Width + i) * 4];
                    d[0] = d[1] = d[2] = 255, d[3] = a;
                }
            upload_rgba(t, rgba.data(), 0, 0, td->Width, td->Height, td->Width * 4);
        }
        td->SetTexID((ImTextureID)t->id);
        td->SetStatus(ImTextureStatus_OK);
    }
    else if (td->Status == ImTextureStatus_WantUpdates)
    {
        XiTex* t = tex_of((uint64_t)td->GetTexID());
        if (t)
        {
            ImTextureRect r = td->UpdateRect;
            const uint8_t* base = (const uint8_t*)td->GetPixelsAt(r.x, r.y);
            if (td->Format == ImTextureFormat_RGBA32)
                upload_rgba(t, base, r.x, r.y, r.w, r.h, td->GetPitch());
            else
            {
                std::vector<uint8_t> rgba((size_t)r.w * r.h * 4);
                for (int j = 0; j < r.h; ++j)
                    for (int i = 0; i < r.w; ++i)
                    {
                        uint8_t a = base[(size_t)j * td->GetPitch() + i];
                        uint8_t* d = &rgba[((size_t)j * r.w + i) * 4];
                        d[0] = d[1] = d[2] = 255, d[3] = a;
                    }
                upload_rgba(t, rgba.data(), r.x, r.y, r.w, r.h, r.w * 4);
            }
        }
        td->SetStatus(ImTextureStatus_OK);
    }
    else if (td->Status == ImTextureStatus_WantDestroy && td->UnusedFrames > 0)
    {
        XiTex* t = tex_of((uint64_t)td->GetTexID());
        if (t)
            tex_free(t);
        td->SetTexID(ImTextureID_Invalid);
        td->SetStatus(ImTextureStatus_Destroyed);
    }
}

/* --- drawing through gfx.h -------------------------------------------------------------------- */

struct Vtx
{
    float x, y, z, rhw;
    uint32_t color; /* D3DCOLOR: ARGB */
    float u, v;
};

static GfxDraw g_gd;
static std::vector<Vtx> g_vtx;

static void draw_setup(uint32_t w, uint32_t h)
{
    GfxDraw* d = &g_gd;
    memset(d, 0, sizeof *d);
    d->vs.rhw = 1;
    d->vs.el[GFX_R_POSITION] = GfxElem{ 1, 0, GFX_FLOAT4, 0 };
    d->vs.el[GFX_R_DIFFUSE] = GfxElem{ 1, 0, GFX_D3DCOLOR, 0 };
    d->vs.el[GFX_R_TEXCOORD0] = GfxElem{ 1, 0, GFX_FLOAT2, 0 };
    d->u.offset[GFX_R_POSITION] = 0;
    d->u.offset[GFX_R_DIFFUSE] = 16;
    d->u.offset[GFX_R_TEXCOORD0] = 20;
    d->u.stride[0] = sizeof(Vtx);
    d->vs.ntex = 1;
    d->fs.nstages = 1;
    GfxStage* st = &d->fs.st[0];
    st->cop = 4, st->ca1 = 2, st->ca2 = 0, st->ca0 = 1; /* MODULATE texture, diffuse */
    st->aop = 4, st->aa1 = 2, st->aa2 = 0, st->aa0 = 1;
    st->result = 1;
    st->tex = 1;
    st->ncoord = 2;
    d->samp[0] = GfxSampler{ 3, 3, 3, 2, 2, 0, 1, 0, 0, { 0 }, 0 }; /* clamp, linear */
    d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1; /* SRCALPHA, INVSRCALPHA */
    d->pipe.write_mask = 0xF;
    d->cull = 1, d->fill = 3;
    d->u.vp[0] = 0, d->u.vp[1] = 0, d->u.vp[2] = (float)w, d->u.vp[3] = (float)h;
    float zmin = 0, zmax = 1;
    d->vp[0] = 0, d->vp[1] = 0, d->vp[2] = w, d->vp[3] = h;
    memcpy(&d->vp[4], &zmin, 4);
    memcpy(&d->vp[5], &zmax, 4);
    d->prim = GFX_TRIANGLELIST;
    d->index_size = 2;
}

static void render_draw_data(ImDrawData* dd, uint32_t w, uint32_t h)
{
    if (!dd || !dd->CmdListsCount)
        return;
    GfxDraw* d = &g_gd;
    ImVec2 off = dd->DisplayPos, scale = dd->FramebufferScale;
    for (int n = 0; n < dd->CmdListsCount; ++n)
    {
        const ImDrawList* cl = dd->CmdLists[n];
        g_vtx.resize((size_t)cl->VtxBuffer.Size);
        for (int i = 0; i < cl->VtxBuffer.Size; ++i)
        {
            const ImDrawVert& v = cl->VtxBuffer[i];
            uint32_t c = v.col; /* ABGR */
            g_vtx[(size_t)i] = Vtx{ (v.pos.x - off.x) * scale.x, (v.pos.y - off.y) * scale.y, 0.0f, 1.0f,
                (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu), v.uv.x, v.uv.y };
        }
        for (int c = 0; c < cl->CmdBuffer.Size; ++c)
        {
            const ImDrawCmd& cmd = cl->CmdBuffer[c];
            if (cmd.UserCallback)
            {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd.UserCallback(cl, &cmd);
                continue;
            }
            float x0 = (cmd.ClipRect.x - off.x) * scale.x, y0 = (cmd.ClipRect.y - off.y) * scale.y;
            float x1 = (cmd.ClipRect.z - off.x) * scale.x, y1 = (cmd.ClipRect.w - off.y) * scale.y;
            if (x1 <= x0 || y1 <= y0 || !cmd.ElemCount)
                continue;
            XiTex* t = tex_of((uint64_t)cmd.GetTexID());
            d->tex[0] = t && t->gpu ? t->gpu : g_white;
            d->scissor[0] = (int32_t)floorf(x0 < 0 ? 0 : x0), d->scissor[1] = (int32_t)floorf(y0 < 0 ? 0 : y0);
            d->scissor[2] = (int32_t)ceilf(x1) - d->scissor[0], d->scissor[3] = (int32_t)ceilf(y1) - d->scissor[1];
            uint32_t first = cmd.VtxOffset;
            d->data[0] = &g_vtx[first];
            d->size[0] = (uint32_t)((g_vtx.size() - first) * sizeof(Vtx));
            d->indices = cl->IdxBuffer.Data + cmd.IdxOffset;
            d->index_size = sizeof(ImDrawIdx);
            d->count = cmd.ElemCount / 3;
            d->u.vofs = 0;
            d->vertex_start = 0;
            gfx_draw(d);
        }
    }
    (void)w, (void)h;
}

/* --- the frame -------------------------------------------------------------------------------- */

static int g_in_frame, g_up;
static ImGuiErrorRecoveryState g_recover;
static ImDrawList* g_objects; /* text objects and primitives, drawn under ImGui */

extern "C" int xi_gui_in_frame(void) { return g_in_frame; }


static void overlay(GfxTex* bb, uint32_t w, uint32_t h)
{
    if (!g_up)
        return;
    gfx_set_targets(bb, 0, 0, NULL);
    draw_setup(w, h);
    /* text objects and primitives, in creation order */
    ImDrawData objects;
    ImDrawList* lists[1] = { g_objects };
    if (g_objects && g_objects->CmdBuffer.Size)
    {
        objects.Valid = true;
        objects.CmdListsCount = 1;
        objects.CmdLists.Data = lists;
        objects.CmdLists.Size = objects.CmdLists.Capacity = 1;
        objects.DisplayPos = ImVec2(0, 0);
        objects.DisplaySize = ImVec2((float)w, (float)h);
        objects.FramebufferScale = ImVec2(1, 1);
        render_draw_data(&objects, w, h);
        objects.CmdLists.Data = NULL; /* not ImGui's to free */
        objects.CmdLists.Size = objects.CmdLists.Capacity = 0;
    }
    render_draw_data(ImGui::GetDrawData(), w, h);
}

/* Ashita's fonts: Agave at 18 (the default), 24 and 32 pixels, each with Font Awesome 6's regular,
 * brands and solid icons merged in (addons' ICON_FA_* strings), from the fonts built in. */
static ImFont* add_ashita_fonts(ImFontAtlas* atlas)
{
    size_t n;
    const char* agave = xi_embedded("font/Agave-Regular", &n);
    if (!agave)
        return atlas->AddFontDefault();
    ImFont* first = NULL;
    static const float SIZES[] = { 18.0f, 24.0f, 32.0f };
    for (float size : SIZES)
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        ImFont* f = atlas->AddFontFromMemoryTTF((void*)agave, (int)n, size, &cfg);
        if (!first)
            first = f;
        static const struct
        {
            const char* name;
            ImWchar lo, hi;
        } ICONS[] = { { "font/fa-regular-400", 0xe005, 0xf8ff }, { "font/fa-brands-400", 0xe007, 0xf8e8 },
                      { "font/fa-solid-900", 0xe005, 0xf8ff } };
        for (auto& ic : ICONS)
        {
            size_t in;
            const char* data = xi_embedded(ic.name, &in);
            if (!data)
                continue;
            static ImWchar ranges[3][3];
            ImWchar* r = ranges[&ic - ICONS];
            r[0] = ic.lo, r[1] = ic.hi, r[2] = 0;
            ImFontConfig mc;
            mc.MergeMode = true;
            mc.FontDataOwnedByAtlas = false;
            mc.GlyphMinAdvanceX = size;
            mc.GlyphRanges = r;
            atlas->AddFontFromMemoryTTF((void*)data, (int)in, size, &mc);
        }
    }
    return first;
}

extern "C" void xi_gui_init(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "ffxirecompile-gfx";
    io.BackendPlatformName = "ffxirecompile-sdl3";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    static std::string ini;
    ini = std::string(xi_data_dir()) + "imgui.ini";
    io.IniFilename = ini.c_str();
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false; /* an addon's misuse is logged, never fatal */
    io.ConfigErrorRecoveryEnableDebugLog = false; /* logged once per addon instead (xi_gui_addon_end) */
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::StyleColorsDark();
    g_default_font = add_ashita_fonts(io.Fonts);
    d3d8_set_overlay(overlay);
}

static void ensure_up(void)
{
    if (g_up)
        return;
    if (!xi_headless)
    {
        g_white = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
        uint32_t px = 0xFFFFFFFFu;
        gfx_tex_upload(g_white, 0, 0, &px, 4);
    }
    g_objects = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    g_up = 1;
}

extern "C" void xi_gui_begin(uint32_t w, uint32_t h)
{
    if (!w || !h)
        return;
    ensure_up();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static uint64_t last;
    uint64_t now = rt_monotonic_ns();
    io.DeltaTime = last ? (float)((now - last) / 1e9) : 1.0f / 60.0f;
    if (io.DeltaTime <= 0)
        io.DeltaTime = 1.0f / 60.0f;
    last = now;
    ImGui::NewFrame();
    ImGui::ErrorRecoveryStoreState(&g_recover);
    g_in_frame = 1;
}

/* Around each addon's drawing event: what it leaves open (a Begin without its End, a style pushed and
 * never popped) is closed before the next addon draws, and logged once against it. */
static ImGuiErrorRecoveryState g_addon_state;

extern "C" void xi_gui_addon_begin(void)
{
    if (g_in_frame)
        ImGui::ErrorRecoveryStoreState(&g_addon_state);
}

extern "C" void xi_gui_addon_end(Addon* a)
{
    if (!g_in_frame)
        return;
    ImGuiContext& g = *GImGui;
    int windows = g.CurrentWindowStack.Size - g_addon_state.SizeOfWindowStack;
    int ids = g.CurrentWindow ? g.CurrentWindow->IDStack.Size - g_addon_state.SizeOfIDStack : 0;
    int styles = g.StyleVarStack.Size - g_addon_state.SizeOfStyleVarStack;
    int colors = g.ColorStack.Size - g_addon_state.SizeOfColorStack;
    if (windows || ids || styles || colors)
    {
        char key[128];
        snprintf(key, sizeof key, "imgui-unbalanced %s", a ? a->name : "?");
        xi_log_once(key, "%s left ImGui unbalanced (windows %d, ids %d, style vars %d, colours %d): closed", a ? a->name : "?",
            windows, ids, styles, colors);
        ImGui::ErrorRecoveryTryToRecoverState(&g_addon_state);
    }
}

/* This frame's text objects and primitives, in creation order (built before ImGui renders: a new
 * glyph size they ask for is baked into the atlas this frame). */
static void build_objects(void)
{
    g_objects->_ResetForNewFrame();
    g_objects->PushClipRect(ImVec2(0, 0), ImGui::GetIO().DisplaySize);
    g_objects->PushTexture(ImGui::GetIO().Fonts->TexRef);
    struct Item
    {
        uint32_t order;
        XiText* t;
        XiPrim* p;
    };
    std::vector<Item> items;
    for (XiText* t : g_texts)
        if (t->visible && !t->text.empty())
            items.push_back({ t->order, t, NULL });
    for (XiPrim* p : g_prims)
        if (p->visible)
            items.push_back({ p->order, NULL, p });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
    for (const Item& it : items)
        if (it.t)
            draw_text(g_objects, it.t);
        else
            draw_prim(g_objects, it.p);
}

extern "C" void xi_gui_end(void)
{
    if (!g_in_frame)
        return;
    /* whatever an addon left open (a Begin without its End, a push never popped): closed, and the
     * error logged, so the frame ends well */
    ImGui::ErrorRecoveryTryToRecoverState(&g_recover);
    build_objects();
    ImGui::Render();
    g_in_frame = 0;
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->Textures)
        for (ImTextureData* td : *dd->Textures)
            if (td->Status != ImTextureStatus_OK)
                update_texture(td);
}

/* --- input ------------------------------------------------------------------------------------ */

static ImGuiKey imgui_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return (ImGuiKey)(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return (ImGuiKey)(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc)
    {
    case SDL_SCANCODE_0: return ImGuiKey_0;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    case SDL_SCANCODE_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

/* Shift + left drag moves a text object or primitive that can take focus and isn't locked (as
 * Ashita's font objects move). */
static XiText* g_drag_text;
static XiPrim* g_drag_prim;
static float g_drag_dx, g_drag_dy;

static int drag(int msg, int x, int y)
{
    if (msg == 0x201 && (SDL_GetModState() & SDL_KMOD_SHIFT))
    {
        for (size_t i = g_texts.size(); i-- > 0;)
        {
            XiText* t = g_texts[i];
            if (!t->visible || t->locked || !t->can_focus || t->parent)
                continue;
            float w = t->ext_w, h = t->ext_h, ox, oy;
            text_origin(t, w, h, &ox, &oy, 0);
            if (x >= ox && y >= oy && x < ox + w && y < oy + h)
            {
                g_drag_text = t, g_drag_dx = x - t->x, g_drag_dy = y - t->y;
                return 1;
            }
        }
        for (size_t i = g_prims.size(); i-- > 0;)
        {
            XiPrim* p = g_prims[i];
            if (!p->visible || p->locked || !p->can_focus)
                continue;
            if (x >= p->x && y >= p->y && x < p->x + p->w * p->scale_x && y < p->y + p->h * p->scale_y)
            {
                g_drag_prim = p, g_drag_dx = x - p->x, g_drag_dy = y - p->y;
                return 1;
            }
        }
    }
    if (msg == 0x200 && (g_drag_text || g_drag_prim))
    {
        if (g_drag_text)
            g_drag_text->x = x - g_drag_dx, g_drag_text->y = y - g_drag_dy;
        if (g_drag_prim)
            g_drag_prim->x = x - g_drag_dx, g_drag_prim->y = y - g_drag_dy;
        return 1;
    }
    if (msg == 0x202 && (g_drag_text || g_drag_prim))
    {
        g_drag_text = NULL, g_drag_prim = NULL;
        return 1;
    }
    return 0;
}

extern "C" int xi_gui_mouse(int msg, int x, int y, int delta)
{
    if (!ImGui::GetCurrentContext())
        return 0;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((float)x, (float)y);
    int taken = 0;
    switch (msg)
    {
    case 0x201: case 0x202: io.AddMouseButtonEvent(0, msg == 0x201); taken = io.WantCaptureMouse; break;
    case 0x204: case 0x205: io.AddMouseButtonEvent(1, msg == 0x204); taken = io.WantCaptureMouse; break;
    case 0x207: case 0x208: io.AddMouseButtonEvent(2, msg == 0x207); taken = io.WantCaptureMouse; break;
    case 0x20A: io.AddMouseWheelEvent(0, delta / 120.0f); taken = io.WantCaptureMouse; break;
    default: break;
    }
    if (drag(msg, x, y))
        taken = 1;
    return taken;
}

extern "C" int xi_gui_key(uint32_t scancode, int down, uint32_t dik)
{
    (void)dik;
    if (!ImGui::GetCurrentContext())
        return 0;
    ImGuiIO& io = ImGui::GetIO();
    SDL_Keymod m = SDL_GetModState();
    io.AddKeyEvent(ImGuiMod_Ctrl, (m & SDL_KMOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (m & SDL_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (m & SDL_KMOD_ALT) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (m & SDL_KMOD_GUI) != 0);
    ImGuiKey k = imgui_key((SDL_Scancode)scancode);
    if (k != ImGuiKey_None)
        io.AddKeyEvent(k, down != 0);
    return io.WantCaptureKeyboard || io.WantTextInput;
}

extern "C" int xi_gui_text(const char* utf8)
{
    if (!ImGui::GetCurrentContext())
        return 0;
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput)
        return 0;
    io.AddInputCharactersUTF8(utf8);
    return 1;
}

extern "C" int xi_gui_wants_keyboard(void) { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard; }
extern "C" int xi_gui_wants_mouse(void) { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }

extern "C" void xi_gui_free_owned(Addon* a)
{
    for (size_t i = 0; i < g_texts.size();)
        if (g_texts[i]->owner == a)
        {
            delete g_texts[i];
            g_texts.erase(g_texts.begin() + (long)i);
        }
        else
            ++i;
    for (size_t i = 0; i < g_prims.size();)
        if (g_prims[i]->owner == a)
        {
            delete g_prims[i];
            g_prims.erase(g_prims.begin() + (long)i);
        }
        else
            ++i;
    std::vector<XiTex*> owned;
    for (auto& kv : g_tex)
        if (kv.second->owner == a)
            owned.push_back(kv.second);
    for (XiTex* t : owned)
        tex_free(t);
    if (g_drag_text && !text_of(g_drag_text->id))
        g_drag_text = NULL;
}
