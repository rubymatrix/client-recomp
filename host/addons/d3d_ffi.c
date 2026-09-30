/* Direct3D 8 and D3DX for addons' ffi: host COM objects (see d3d_ffi.h).
 *
 * Ashita's libs/d3d8 files declare the interfaces as `struct { Vtbl* lpVtbl; }` and call
 * `self.lpVtbl.Method(self, ...)`, so every object here is a struct whose first member points at
 * a table of host functions in the interface's method order. Methods nobody implements are stubs
 * that log "IDirect3DDevice8::Name is not supported" once and return E_NOTIMPL.
 *
 * - IDirect3DDevice8: one, static, for every addon (AshitaCore:GetDirect3DDevice(), xi.d3d8_device).
 *   It is not the game's device (that one lives in guest memory, d3d8.c): Get* of transforms and
 *   render states read the game's current values unless an addon set its own this frame; Set*
 *   only change what the addons' own draws use, and last one frame (the game sets its state again
 *   every frame). The viewport is the whole back buffer. Draws (DrawPrimitive[UP],
 *   DrawIndexedPrimitive[UP]) are transformed on the CPU (XYZ through world, view and
 *   projection; XYZRHW as they are), clipped at the near plane, and queued to the overlay.
 * - IDirect3DTexture8, IDirect3DSurface8: pixels in host memory in the format asked for (the
 *   formats d3d_image.c knows; others become A8R8G8B8), level 0 uploaded to a gui texture at
 *   UnlockRect. The gui texture's id is the low 32 bits of the object's address, so the numbers
 *   addons hand ImGui (`tonumber(ffi.cast('uint32_t', tex))`) are the texture.
 * - IDirect3DVertexBuffer8, IDirect3DIndexBuffer8: host memory.
 * - ID3DXSprite (Begin, Draw, DrawTransform, End...), ID3DXFont (DrawTextA/W with the overlay's
 *   fonts), IDirect3D8 (adapter queries).
 * - D3DX functions: textures from files and memory (BMP, DIB, DDS, PNG, JPEG, TGA...), image
 *   info, D3DXCreateTexture, surfaces from memory, files and surfaces, sprites, fonts.
 *
 * Not here: render targets other than the back buffer, cube and volume textures, shaders
 * (vertex shader handles other than FVF codes, pixel shaders), lighting (vertices keep their
 * diffuse colour), texture stage operations other than modulate. Each logs once when asked for. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "d3d8.h"
#include "d3d_ffi.h"
#include "host.h"

#include "lauxlib.h"
#include "lua.h"

typedef int32_t HR;
#define S_OK_ 0
#define S_FALSE_ 1
#define E_NOTIMPL_ ((HR)0x80004001u)
#define E_NOINTERFACE_ ((HR)0x80004002u)
#define E_FAIL_ ((HR)0x80004005u)
#define E_OUTOFMEMORY_ ((HR)0x8007000Eu)
#define D3DERR_INVALIDCALL ((HR)0x8876086Cu)
#define D3DERR_NOTAVAILABLE ((HR)0x8876086Au)
#define D3DXERR_INVALIDDATA ((HR)0x88760B59u)

#define D3DX_DEFAULT 0xFFFFFFFFu
#define D3DX_DEFAULT_NONPOW2 0xFFFFFFFEu

enum
{
    FMT_UNKNOWN = 0,
    FMT_A8R8G8B8 = 21,
    FMT_X8R8G8B8 = 22,
    FMT_INDEX16 = 101,
    FMT_INDEX32 = 102,
    FMT_VERTEXDATA = 100,
    FMT_D24S8 = 75,
};

enum
{
    RT_SURFACE = 1,
    RT_TEXTURE = 3,
    RT_VERTEXBUFFER = 6,
    RT_INDEXBUFFER = 7,
};

/* --- the structures the ffi declarations lay out (LONG and DWORD are 32 bits there) ------------ */

typedef struct
{
    int32_t left, top, right, bottom;
} XRECT;
typedef struct
{
    int32_t pitch;
    void* bits;
} XLOCKED_RECT;
typedef struct
{
    uint32_t format, type, usage, pool, size, multisample, width, height;
} XSURFACE_DESC;
typedef struct
{
    uint32_t x, y, w, h;
    float minz, maxz;
} XVIEWPORT;
typedef struct
{
    uint32_t width, height, refresh, format;
} XDISPLAYMODE;
typedef struct
{
    uint32_t adapter, device_type;
    void* hwnd;
    uint32_t behavior;
} XCREATION_PARAMETERS;
typedef struct
{
    uint32_t format, type, usage, pool, size, fvf;
} XVB_DESC;
typedef struct
{
    uint32_t format, type, usage, pool, size;
} XIB_DESC;
typedef struct
{
    float x, y;
} XVEC2;
typedef struct
{
    int32_t height, width, escapement, orientation, weight;
    uint8_t italic, underline, strikeout, charset, out_precision, clip_precision, quality, pitch_family;
    char face[32];
} XLOGFONTA;
typedef struct
{
    uint32_t DeviceType, AdapterOrdinal, Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps,
        PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps,
        TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps,
        VolumeTextureAddressCaps, LineCaps, MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat,
        MaxTextureAspectRatio, MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    uint32_t StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures,
        VertexProcessingCaps, MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    uint32_t MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion,
        MaxVertexShaderConst, PixelShaderVersion;
    float MaxPixelShaderValue;
} XCAPS8;
typedef struct
{
    char driver[512], description[512];
    int64_t driver_version;
    uint32_t vendor, device, subsys, revision;
    uint8_t guid[16];
    uint32_t whql;
} XADAPTER_IDENTIFIER8;

/* --- objects -------------------------------------------------------------------------------- */

enum
{
    K_DEVICE,
    K_D3D8,
    K_TEXTURE,
    K_SURFACE,
    K_VERTEXBUFFER,
    K_INDEXBUFFER,
    K_SPRITE,
    K_FONT,
    K_KINDS
};

#define MAGIC 0x58443344u /* "D3DX" */

typedef struct Com
{
    void** vtbl;
    uint32_t magic;
    int kind;
    int refs;
    Addon* owner;
} Com;

typedef struct Level
{
    uint32_t w, h, pitch;
    uint8_t* data;
} Level;

#define MAX_LEVELS 14

typedef struct Texture
{
    Com h;
    uint32_t id; /* the gui texture: low 32 bits of this object's address (0: none) */
    uint32_t format, usage, pool, nlevels;
    Level lv[MAX_LEVELS];
    int locked[MAX_LEVELS];
    int32_t lock_top[MAX_LEVELS], lock_bottom[MAX_LEVELS];
} Texture;

typedef struct Surface
{
    Com h;
    Texture* parent; /* a texture's level, or NULL */
    uint32_t level;
    Level own; /* image surfaces */
    uint32_t format;
    int backbuffer; /* 1 back buffer, 2 depth: descriptions only */
} Surface;

typedef struct Buffer
{
    Com h;
    uint8_t* data;
    uint32_t size, usage, fvf, format, pool;
} Buffer;

typedef struct Sprite
{
    Com h;
    int begun;
} Sprite;

typedef struct Font
{
    Com h;
    XLOGFONTA lf;
    char family[64];
    float size;
    int bold, italic;
} Font;

static const char* const KIND_NAME[K_KINDS] = { "IDirect3DDevice8", "IDirect3D8", "IDirect3DTexture8", "IDirect3DSurface8",
    "IDirect3DVertexBuffer8", "IDirect3DIndexBuffer8", "ID3DXSprite", "ID3DXFont" };

/* each interface's methods, in vtable order (Ashita's libs/d3d8/interfaces) */
static const char* const M_DEVICE[] = { "QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
    "GetAvailableTextureMem", "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
    "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear",
    "SetTransform", "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial",
    "SetLight", "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState",
    "GetRenderState", "BeginStateBlock", "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock",
    "DeleteStateBlock", "CreateStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture",
    "GetTextureStageState", "SetTextureStageState", "ValidateDevice", "GetInfo", "SetPaletteEntries",
    "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive",
    "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader",
    "SetVertexShader", "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant",
    "GetVertexShaderConstant", "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource",
    "GetStreamSource", "SetIndices", "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader",
    "DeletePixelShader", "SetPixelShaderConstant", "GetPixelShaderConstant", "GetPixelShaderFunction",
    "DrawRectPatch", "DrawTriPatch", "DeletePatch", NULL };
static const char* const M_D3D8[] = { "QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
    "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
    "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "GetDeviceCaps",
    "GetAdapterMonitor", "CreateDevice", NULL };
static const char* const M_TEXTURE[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "SetLOD", "GetLOD",
    "GetLevelCount", "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const M_SURFACE[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect", NULL };
static const char* const M_BUFFER[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "Lock", "Unlock",
    "GetDesc", NULL };
static const char* const M_SPRITE[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "Begin", "Draw",
    "DrawTransform", "End", "OnLostDevice", "OnResetDevice", NULL };
static const char* const M_FONT[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetLogFont", "Begin",
    "DrawTextA", "DrawTextW", "End", "OnLostDevice", "OnResetDevice", NULL };
static const char* const* const METHODS[K_KINDS] = { M_DEVICE, M_D3D8, M_TEXTURE, M_SURFACE, M_BUFFER, M_BUFFER,
    M_SPRITE, M_FONT };

static HR unimpl(Com* self, int i)
{
    const char* iface = self && self->magic == MAGIC && self->kind >= 0 && self->kind < K_KINDS ? KIND_NAME[self->kind] : "?";
    const char* name = "?";
    if (self && self->magic == MAGIC && self->kind >= 0 && self->kind < K_KINDS)
    {
        const char* const* m = METHODS[self->kind];
        for (int k = 0; m[k] && k <= i; ++k)
            name = m[k];
    }
    char key[128];
    snprintf(key, sizeof key, "d3d8 %s::%s", iface, name);
    Addon* a = xi_current();
    xi_log_once(key, "%s::%s is not supported (addon %s); E_NOTIMPL", iface, name, a ? a->name : "?");
    return E_NOTIMPL_;
}

static void unsupported(const char* what)
{
    char key[160];
    snprintf(key, sizeof key, "d3d8 %s", what);
    Addon* a = xi_current();
    xi_log_once(key, "%s is not supported (addon %s)", what, a ? a->name : "?");
}

#define S(i) \
    static HR stub_##i(Com* self) { return unimpl(self, i); }
S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15) S(16) S(17) S(18) S(19) S(20)
S(21) S(22) S(23) S(24) S(25) S(26) S(27) S(28) S(29) S(30) S(31) S(32) S(33) S(34) S(35) S(36) S(37) S(38) S(39)
S(40) S(41) S(42) S(43) S(44) S(45) S(46) S(47) S(48) S(49) S(50) S(51) S(52) S(53) S(54) S(55) S(56) S(57) S(58)
S(59) S(60) S(61) S(62) S(63) S(64) S(65) S(66) S(67) S(68) S(69) S(70) S(71) S(72) S(73) S(74) S(75) S(76) S(77)
S(78) S(79) S(80) S(81) S(82) S(83) S(84) S(85) S(86) S(87) S(88) S(89) S(90) S(91) S(92) S(93) S(94) S(95) S(96)
#undef S
static void* const STUBS[] = { stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7, stub_8, stub_9, stub_10,
    stub_11, stub_12, stub_13, stub_14, stub_15, stub_16, stub_17, stub_18, stub_19, stub_20, stub_21, stub_22, stub_23,
    stub_24, stub_25, stub_26, stub_27, stub_28, stub_29, stub_30, stub_31, stub_32, stub_33, stub_34, stub_35, stub_36,
    stub_37, stub_38, stub_39, stub_40, stub_41, stub_42, stub_43, stub_44, stub_45, stub_46, stub_47, stub_48, stub_49,
    stub_50, stub_51, stub_52, stub_53, stub_54, stub_55, stub_56, stub_57, stub_58, stub_59, stub_60, stub_61, stub_62,
    stub_63, stub_64, stub_65, stub_66, stub_67, stub_68, stub_69, stub_70, stub_71, stub_72, stub_73, stub_74, stub_75,
    stub_76, stub_77, stub_78, stub_79, stub_80, stub_81, stub_82, stub_83, stub_84, stub_85, stub_86, stub_87, stub_88,
    stub_89, stub_90, stub_91, stub_92, stub_93, stub_94, stub_95, stub_96 };

static void* g_vt[K_KINDS][100];

static void vt_set(int kind, const char* name, void* fn)
{
    const char* const* m = METHODS[kind];
    for (int k = 0; m[k]; ++k)
        if (!strcmp(m[k], name))
        {
            g_vt[kind][k] = fn;
            return;
        }
    xi_log("d3d_ffi: %s has no method %s", KIND_NAME[kind], name);
}

static int valid(const void* p, int kind)
{
    const Com* c = (const Com*)p;
    return c && c->magic == MAGIC && c->kind == kind;
}

/* Texture objects live in one slab mapped where every address, cut to 32 bits, is at least
 * 0x10000000 (never one of ImGui's small ids) and unique among live textures: the id Lua gets from
 * tonumber(ffi.cast('uint32_t', tex)) is the gui texture's. (Taking blocks from malloc and retrying
 * failed whenever the allocator's region sat low in its 4 GB.) */
#define TEX_SLOTS 16384u
static uint8_t* g_slab;
static size_t g_slot_size, g_slab_bytes, g_slab_next;
static void* g_slab_free;

static void* map_bytes(void* hint, size_t n)
{
#if defined(_WIN32)
    return VirtualAlloc(hint, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(hint, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void unmap_bytes(void* p, size_t n)
{
#if defined(_WIN32)
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

static int slab_fits(uintptr_t a, size_t n)
{
    uint64_t lo = (uint32_t)a;
    return lo >= 0x10000000u && lo + n <= 0xFFFFFFFFull;
}

static void* slab_alloc(size_t n)
{
    if (!g_slab)
    {
        g_slot_size = (n + 63) & ~(size_t)63;
        g_slab_bytes = g_slot_size * TEX_SLOTS;
        uint8_t* p = (uint8_t*)map_bytes(NULL, g_slab_bytes);
        for (int tries = 0; p && !slab_fits((uintptr_t)p, g_slab_bytes) && tries < 32; ++tries)
        {
            /* the same 4 GB, higher up (0x20000000 + a step each try) */
            uintptr_t hint = ((uintptr_t)p & ~(uintptr_t)0xFFFFFFFFu) + 0x20000000u + (uintptr_t)tries * 0x04000000u;
            unmap_bytes(p, g_slab_bytes);
            p = (uint8_t*)map_bytes((void*)hint, g_slab_bytes);
        }
        if (!p || !slab_fits((uintptr_t)p, g_slab_bytes))
        {
            xi_log("d3d8 ffi: no memory for texture objects where their ids fit");
            if (p)
                unmap_bytes(p, g_slab_bytes);
            return NULL;
        }
        g_slab = p;
    }
    if (n > g_slot_size)
        return NULL;
    void* p;
    if (g_slab_free)
    {
        p = g_slab_free;
        g_slab_free = *(void**)p;
    }
    else if (g_slab_next < TEX_SLOTS)
        p = g_slab + g_slot_size * g_slab_next++;
    else
    {
        xi_log_once("d3d8 texture slots", "d3d8 ffi: %u textures alive at once: no more", TEX_SLOTS);
        return NULL;
    }
    memset(p, 0, g_slot_size);
    return p;
}

static int in_slab(const void* p)
{
    return g_slab && (const uint8_t*)p >= g_slab && (const uint8_t*)p < g_slab + g_slab_bytes;
}

static void* obj_alloc(size_t n, int kind, int* id_out)
{
    void* p = kind == K_TEXTURE ? slab_alloc(n) : calloc(1, n);
    if (p)
    {
        Com* c = (Com*)p;
        c->vtbl = g_vt[kind];
        c->magic = MAGIC;
        c->kind = kind;
        c->refs = 1;
        c->owner = xi_current();
        if (id_out)
            *id_out = (int)(uint32_t)(uintptr_t)p;
    }
    return p;
}

static void obj_free(void* p)
{
    ((Com*)p)->magic = 0;
    if (in_slab(p))
    {
        *(void**)p = g_slab_free;
        g_slab_free = p;
    }
    else
        free(p);
}

static HR com_qi(Com* self, const void* iid, void** out)
{
    (void)iid;
    if (!out)
        return E_NOINTERFACE_;
    *out = self;
    ++self->refs;
    return S_OK_;
}

static uint32_t com_addref(Com* self) { return (uint32_t)++self->refs; }

/* --- the device's state --------------------------------------------------------------------- */

#define NXF 28 /* D3DTS 0..23, WORLDMATRIX(0..3) = 256..259 */

typedef struct DState
{
    uint32_t rs[256];
    uint8_t rs_set[256];
    uint32_t tss[8][32];
    uint8_t tss_set[8][32];
    float xf[NXF][16];
    uint8_t xf_set[NXF];
    XVIEWPORT vp;
    int vp_set;
    Com* tex[8];
    uint8_t tex_set[8], fvf_set;
    uint32_t fvf;
    Buffer* stream;
    uint32_t stride;
    Buffer* indices;
    uint32_t base_vertex;
    float material[17];
} DState;

typedef struct Device
{
    Com h;
    DState s;
    uint32_t frame;
    DState* blocks[256];
    uint8_t block_full[256]; /* captured whole (CreateStateBlock, CaptureStateBlock), not recorded */
    int recording; /* a state block being recorded: its index + 1 */
} Device;

static Device g_device;
static Com g_d3d8;

static int xf_index(uint32_t ts) { return ts < 24 ? (int)ts : ts >= 256 && ts < 260 ? (int)(24 + ts - 256) : -1; }

static const float IDENTITY[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* The per-frame part: render and texture stage states, transforms and the viewport go back to
 * the game's; bindings (textures, FVF, streams) are the addons' alone and stay. */
static void state_defaults(DState* s)
{
    DState keep = *s;
    memset(s, 0, sizeof *s);
    memcpy(s->tex, keep.tex, sizeof s->tex);
    s->fvf = keep.fvf, s->stream = keep.stream, s->stride = keep.stride, s->indices = keep.indices;
    s->base_vertex = keep.base_vertex;
    memcpy(s->material, keep.material, sizeof s->material);
    for (int i = 0; i < NXF; ++i)
        memcpy(s->xf[i], IDENTITY, sizeof IDENTITY);
    s->rs[27] = 1;               /* ALPHABLENDENABLE (ours: the overlay blends by default) */
    s->rs[19] = 5, s->rs[20] = 6; /* SRCBLEND SRCALPHA, DESTBLEND INVSRCALPHA */
    for (int t = 0; t < 8; ++t)
    {
        s->tss[t][1] = t ? 1 : 4; /* COLOROP: stage 0 MODULATE, others DISABLE */
        s->tss[t][2] = 2, s->tss[t][3] = 0; /* COLORARG1 TEXTURE, COLORARG2 CURRENT */
        s->tss[t][4] = t ? 1 : 2;           /* ALPHAOP SELECTARG1 / DISABLE */
        s->tss[t][5] = 2, s->tss[t][6] = 0;
    }
}

static void bb_size(uint32_t* w, uint32_t* h)
{
    d3d8_backbuffer_size(w, h);
    if (!*w || !*h)
        *w = 1920, *h = 1080; /* the harness, or before the game's device exists */
}

/* The addons' state lasts a frame: the game sets its own again every frame. */
static Device* dev_sync(void)
{
    uint32_t f = xi_gui_d3d_frame();
    if (f != g_device.frame)
    {
        g_device.frame = f;
        state_defaults(&g_device.s);
    }
    return &g_device;
}

/* --- textures ------------------------------------------------------------------------------- */

static int fmt_ok(uint32_t f) { return xi_fmt_bpp(f) != 0; }

static uint32_t level_bytes(const Level* l, uint32_t fmt) { return l->pitch * xi_fmt_rows(fmt, l->h); }

static void tex_upload(Texture* t)
{
    if (!t->id)
        return;
    Level* l = &t->lv[0];
    uint8_t* bgra = (uint8_t*)malloc((size_t)l->w * l->h * 4);
    if (!bgra)
        return;
    xi_fmt_to_bgra(t->format, l->data, (int)l->pitch, (int)l->w, (int)l->h, bgra, (int)l->w * 4);
    xi_gui_d3d_texture_upload(t->id, bgra, 0, 0, (int)l->w, (int)l->h, (int)l->w * 4);
    free(bgra);
}

static Texture* tex_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    if (!w || !h || w > 16384 || h > 16384)
        return NULL;
    if (!fmt_ok(fmt))
    {
        char what[96];
        snprintf(what, sizeof what, "texture format %u (made A8R8G8B8)", fmt);
        unsupported(what);
        fmt = FMT_A8R8G8B8;
    }
    uint32_t full = 1;
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1)
        ++full;
    if (!levels || levels > full)
        levels = full;
    if (levels > MAX_LEVELS)
        levels = MAX_LEVELS;
    int id = 0;
    Texture* t = (Texture*)obj_alloc(sizeof(Texture), K_TEXTURE, &id);
    if (!t)
        return NULL;
    t->format = fmt, t->usage = usage, t->pool = pool, t->nlevels = levels;
    for (uint32_t i = 0; i < levels; ++i)
    {
        Level* l = &t->lv[i];
        l->w = w >> i ? w >> i : 1;
        l->h = h >> i ? h >> i : 1;
        l->pitch = xi_fmt_pitch(fmt, l->w);
        l->data = (uint8_t*)calloc(1, level_bytes(l, fmt));
        if (!l->data)
        {
            for (uint32_t k = 0; k < i; ++k)
                free(t->lv[k].data);
            obj_free(t);
            return NULL;
        }
    }
    if (xi_gui_d3d_texture_new((uint32_t)id, (int)w, (int)h))
    {
        t->id = (uint32_t)id;
        tex_upload(t);
    }
    return t;
}

static void unbind(Com* c)
{
    for (int i = 0; i < 8; ++i)
        if (g_device.s.tex[i] == c)
            g_device.s.tex[i] = NULL;
    if ((Com*)g_device.s.stream == c)
        g_device.s.stream = NULL;
    if ((Com*)g_device.s.indices == c)
        g_device.s.indices = NULL;
    for (int b = 0; b < 256; ++b)
        if (g_device.blocks[b])
            for (int i = 0; i < 8; ++i)
                if (g_device.blocks[b]->tex[i] == c)
                    g_device.blocks[b]->tex[i] = NULL;
}

static uint32_t tex_release(Texture* t)
{
    if (--t->h.refs > 0)
        return (uint32_t)t->h.refs;
    unbind(&t->h);
    if (t->id)
        xi_gui_d3d_texture_free(t->id);
    for (uint32_t i = 0; i < t->nlevels; ++i)
        free(t->lv[i].data);
    obj_free(t);
    return 0;
}

static HR res_getdevice(Com* self, void** out)
{
    (void)self;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = &g_device;
    return S_OK_;
}
static HR res_privdata(Com* self) { (void)self; return D3DERR_NOTAVAILABLE; } /* Set/Get/FreePrivateData */
static uint32_t res_priority(Com* self) { (void)self; return 0; }
static void res_preload(Com* self) { (void)self; }
static uint32_t tex_type(Com* self) { (void)self; return RT_TEXTURE; }
static uint32_t tex_lod(Com* self) { (void)self; return 0; }
static uint32_t tex_levelcount(Texture* t) { return t->nlevels; }

static HR tex_leveldesc(Texture* t, uint32_t level, XSURFACE_DESC* d)
{
    if (level >= t->nlevels || !d)
        return D3DERR_INVALIDCALL;
    Level* l = &t->lv[level];
    d->format = t->format, d->type = RT_SURFACE, d->usage = t->usage, d->pool = t->pool;
    d->size = level_bytes(l, t->format), d->multisample = 0, d->width = l->w, d->height = l->h;
    return S_OK_;
}

static HR tex_getsurface(Texture* t, uint32_t level, Surface** out)
{
    if (level >= t->nlevels || !out)
        return D3DERR_INVALIDCALL;
    Surface* s = (Surface*)obj_alloc(sizeof(Surface), K_SURFACE, NULL);
    if (!s)
        return E_OUTOFMEMORY_;
    s->parent = t, s->level = level, s->format = t->format;
    ++t->h.refs;
    *out = s;
    return S_OK_;
}

static HR level_lock(Level* l, uint32_t fmt, XLOCKED_RECT* lr, const XRECT* r, int32_t* top, int32_t* bottom)
{
    if (!lr)
        return D3DERR_INVALIDCALL;
    int32_t x = 0, y = 0;
    *top = 0, *bottom = (int32_t)l->h;
    if (r)
    {
        if (r->left < 0 || r->top < 0 || r->right > (int32_t)l->w || r->bottom > (int32_t)l->h || r->left >= r->right ||
            r->top >= r->bottom)
            return D3DERR_INVALIDCALL;
        x = r->left, y = r->top;
        *top = r->top, *bottom = r->bottom;
    }
    uint32_t bpp = (uint32_t)xi_fmt_bpp(fmt);
    int dxt = xi_fmt_rows(fmt, 4) == 1;
    lr->pitch = (int32_t)l->pitch;
    lr->bits = dxt ? l->data + (size_t)(y / 4) * l->pitch + (size_t)(x / 4) * (bpp * 2)
                   : l->data + (size_t)y * l->pitch + (size_t)x * bpp / 8;
    return S_OK_;
}

static HR tex_lock(Texture* t, uint32_t level, XLOCKED_RECT* lr, const XRECT* r, uint32_t flags)
{
    (void)flags;
    if (level >= t->nlevels)
        return D3DERR_INVALIDCALL;
    HR hr = level_lock(&t->lv[level], t->format, lr, r, &t->lock_top[level], &t->lock_bottom[level]);
    if (hr == S_OK_)
        t->locked[level] = 1;
    return hr;
}

static HR tex_unlock(Texture* t, uint32_t level)
{
    if (level >= t->nlevels || !t->locked[level])
        return D3DERR_INVALIDCALL;
    t->locked[level] = 0;
    if (level == 0)
        tex_upload(t);
    return S_OK_;
}

static HR tex_dirty(Texture* t, const XRECT* r)
{
    (void)r;
    tex_upload(t);
    return S_OK_;
}

/* --- surfaces ------------------------------------------------------------------------------- */

static Level* surf_level(Surface* s) { return s->parent ? &s->parent->lv[s->level] : s->own.data ? &s->own : NULL; }

static uint32_t surf_release(Surface* s)
{
    if (--s->h.refs > 0)
        return (uint32_t)s->h.refs;
    if (s->parent)
        tex_release(s->parent);
    free(s->own.data);
    obj_free(s);
    return 0;
}

static HR surf_container(Surface* s, const void* iid, void** out)
{
    (void)iid;
    if (!out || !s->parent)
        return E_NOINTERFACE_;
    ++s->parent->h.refs;
    *out = s->parent;
    return S_OK_;
}

static HR surf_desc(Surface* s, XSURFACE_DESC* d)
{
    if (!d)
        return D3DERR_INVALIDCALL;
    memset(d, 0, sizeof *d);
    d->type = RT_SURFACE;
    d->format = s->format;
    if (s->backbuffer)
    {
        bb_size(&d->width, &d->height);
        d->usage = s->backbuffer == 1 ? 1u /* RENDERTARGET */ : 2u /* DEPTHSTENCIL */;
        return S_OK_;
    }
    Level* l = surf_level(s);
    if (!l)
        return D3DERR_INVALIDCALL;
    d->width = l->w, d->height = l->h, d->size = level_bytes(l, s->format);
    d->pool = s->parent ? s->parent->pool : 2 /* SYSTEMMEM */;
    d->usage = s->parent ? s->parent->usage : 0;
    return S_OK_;
}

static HR surf_lock(Surface* s, XLOCKED_RECT* lr, const XRECT* r, uint32_t flags)
{
    if (s->parent)
        return tex_lock(s->parent, s->level, lr, r, flags);
    Level* l = surf_level(s);
    if (!l)
    {
        unsupported("locking the back buffer");
        return D3DERR_INVALIDCALL;
    }
    int32_t a, b;
    return level_lock(l, s->format, lr, r, &a, &b);
}

static HR surf_unlock(Surface* s)
{
    if (s->parent)
        return tex_unlock(s->parent, s->level);
    return surf_level(s) ? S_OK_ : D3DERR_INVALIDCALL;
}

static Surface* surf_image(uint32_t w, uint32_t h, uint32_t fmt)
{
    if (!w || !h || w > 16384 || h > 16384)
        return NULL;
    if (!fmt_ok(fmt))
        fmt = FMT_A8R8G8B8;
    Surface* s = (Surface*)obj_alloc(sizeof(Surface), K_SURFACE, NULL);
    if (!s)
        return NULL;
    s->format = fmt;
    s->own.w = w, s->own.h = h, s->own.pitch = xi_fmt_pitch(fmt, w);
    s->own.data = (uint8_t*)calloc(1, level_bytes(&s->own, fmt));
    if (!s->own.data)
    {
        obj_free(s);
        return NULL;
    }
    return s;
}

static Surface* surf_backbuffer(int which)
{
    Surface* s = (Surface*)obj_alloc(sizeof(Surface), K_SURFACE, NULL);
    if (s)
        s->backbuffer = which, s->format = which == 1 ? FMT_X8R8G8B8 : FMT_D24S8;
    return s;
}

/* A surface's pixels as BGRA (rect r, or all): malloc'd. */
static uint8_t* surf_read(Surface* s, const XRECT* r, int* w, int* h)
{
    Level* l = surf_level(s);
    if (!l)
        return NULL;
    uint8_t* all = (uint8_t*)malloc((size_t)l->w * l->h * 4);
    if (!all)
        return NULL;
    xi_fmt_to_bgra(s->format, l->data, (int)l->pitch, (int)l->w, (int)l->h, all, (int)l->w * 4);
    XRECT full = { 0, 0, (int32_t)l->w, (int32_t)l->h };
    if (!r)
        r = &full;
    if (r->left < 0 || r->top < 0 || r->right > (int32_t)l->w || r->bottom > (int32_t)l->h || r->left >= r->right || r->top >= r->bottom)
    {
        free(all);
        return NULL;
    }
    *w = r->right - r->left, *h = r->bottom - r->top;
    uint8_t* out = (uint8_t*)malloc((size_t)*w * *h * 4);
    if (out)
        for (int y = 0; y < *h; ++y)
            memcpy(out + (size_t)y * *w * 4, all + ((size_t)(r->top + y) * l->w + (size_t)r->left) * 4, (size_t)*w * 4);
    free(all);
    return out;
}

/* BGRA pixels (sw x sh) into a surface's rect (r, or all), scaled to fit; colour key applied. */
static HR surf_write(Surface* s, const XRECT* r, const uint8_t* bgra, int sw, int sh, uint32_t key)
{
    Level* l = surf_level(s);
    if (!l)
        return D3DERR_INVALIDCALL;
    XRECT full = { 0, 0, (int32_t)l->w, (int32_t)l->h };
    if (!r)
        r = &full;
    if (r->left < 0 || r->top < 0 || r->right > (int32_t)l->w || r->bottom > (int32_t)l->h || r->left >= r->right || r->top >= r->bottom)
        return D3DERR_INVALIDCALL;
    int dw = r->right - r->left, dh = r->bottom - r->top;
    uint8_t* px = (uint8_t*)malloc((size_t)dw * dh * 4);
    if (!px)
        return E_OUTOFMEMORY_;
    if (dw == sw && dh == sh)
        memcpy(px, bgra, (size_t)dw * dh * 4);
    else
        xi_bgra_resize(bgra, sw, sh, sw * 4, px, dw, dh, dw * 4);
    if (key)
        for (size_t i = 0; i < (size_t)dw * dh; ++i)
        {
            uint32_t v = (uint32_t)px[i * 4] | (uint32_t)px[i * 4 + 1] << 8 | (uint32_t)px[i * 4 + 2] << 16 | (uint32_t)px[i * 4 + 3] << 24;
            if (v == key)
                memset(px + i * 4, 0, 4);
        }
    int ok;
    if (xi_fmt_rows(s->format, 4) == 1) /* DXT: not written */
        ok = 0;
    else
        ok = xi_fmt_from_bgra(s->format, px, dw * 4, dw, dh, l->data + (size_t)r->top * l->pitch + (size_t)r->left * (uint32_t)xi_fmt_bpp(s->format) / 8,
            (int)l->pitch);
    free(px);
    if (!ok)
    {
        unsupported("writing a DXT surface");
        return D3DERR_INVALIDCALL;
    }
    if (s->parent && s->level == 0)
        tex_upload(s->parent);
    return S_OK_;
}

/* --- vertex and index buffers ------------------------------------------------------------- */

static Buffer* buf_create(int kind, uint32_t size, uint32_t usage, uint32_t fvf_or_format, uint32_t pool)
{
    if (!size || size > (256u << 20))
        return NULL;
    Buffer* b = (Buffer*)obj_alloc(sizeof(Buffer), kind, NULL);
    if (!b)
        return NULL;
    b->data = (uint8_t*)calloc(1, size);
    if (!b->data)
    {
        obj_free(b);
        return NULL;
    }
    b->size = size, b->usage = usage, b->pool = pool;
    if (kind == K_VERTEXBUFFER)
        b->fvf = fvf_or_format, b->format = FMT_VERTEXDATA;
    else
        b->format = fvf_or_format == FMT_INDEX32 ? FMT_INDEX32 : FMT_INDEX16;
    return b;
}

static uint32_t buf_release(Buffer* b)
{
    if (--b->h.refs > 0)
        return (uint32_t)b->h.refs;
    unbind(&b->h);
    free(b->data);
    obj_free(b);
    return 0;
}

static uint32_t buf_type(Buffer* b) { return b->h.kind == K_VERTEXBUFFER ? RT_VERTEXBUFFER : RT_INDEXBUFFER; }

static HR buf_lock(Buffer* b, uint32_t offset, uint32_t size, uint8_t** out, uint32_t flags)
{
    (void)size, (void)flags;
    if (!out || offset > b->size)
        return D3DERR_INVALIDCALL;
    *out = b->data + offset;
    return S_OK_;
}

static HR buf_unlock(Buffer* b) { (void)b; return S_OK_; }

static HR buf_desc(Buffer* b, void* out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    if (b->h.kind == K_VERTEXBUFFER)
    {
        XVB_DESC* d = (XVB_DESC*)out;
        d->format = FMT_VERTEXDATA, d->type = RT_VERTEXBUFFER, d->usage = b->usage, d->pool = b->pool, d->size = b->size, d->fvf = b->fvf;
    }
    else
    {
        XIB_DESC* d = (XIB_DESC*)out;
        d->format = b->format, d->type = RT_INDEXBUFFER, d->usage = b->usage, d->pool = b->pool, d->size = b->size;
    }
    return S_OK_;
}

/* --- drawing -------------------------------------------------------------------------------- */

typedef struct Fvf
{
    int ok, rhw, pos, diffuse, tex0, texdim, stride;
} Fvf;

static Fvf fvf_parse(uint32_t fvf)
{
    Fvf f;
    memset(&f, 0, sizeof f);
    f.diffuse = f.tex0 = -1;
    uint32_t p = fvf & 0x00E;
    int off = 0;
    if (p == 0x002)
        off = 12;
    else if (p == 0x004)
        off = 16, f.rhw = 1;
    else if (p >= 0x006 && p <= 0x00E)
        off = 12 + 4 * (int)((p - 0x004) / 2);
    else
        return f; /* no position */
    if (fvf & 0x010)
        off += 12; /* NORMAL */
    if (fvf & 0x020)
        off += 4; /* PSIZE */
    if (fvf & 0x040)
        f.diffuse = off, off += 4;
    if (fvf & 0x080)
        off += 4; /* SPECULAR */
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < ntex && i < 8; ++i)
    {
        uint32_t code = (fvf >> (16 + 2 * i)) & 3;
        int dim = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        if (i == 0)
            f.tex0 = off, f.texdim = dim;
        off += 4 * dim;
    }
    f.stride = off;
    f.ok = 1;
    return f;
}

typedef struct CV /* a vertex in clip space (or screen space when rhw) */
{
    float x, y, z, w;
    float u, v;
    float c[4]; /* b g r a, 0..255 */
} CV;

static void mat_mul(float* out, const float* a, const float* b)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}

/* The transform an addon draws with: its own this frame, else the game's. */
static void get_xf(Device* d, uint32_t ts, float m[16])
{
    int i = xf_index(ts);
    if (i >= 0 && d->s.xf_set[i])
        memcpy(m, d->s.xf[i], sizeof(float) * 16);
    else if (!d3d8_get_transform(ts, m))
        memcpy(m, IDENTITY, sizeof IDENTITY);
}

static uint32_t get_rs(Device* d, uint32_t rs)
{
    uint32_t v;
    if (rs < 256 && d->s.rs_set[rs])
        return d->s.rs[rs];
    if (d3d8_get_render_state(rs, &v))
        return v;
    return rs < 256 ? d->s.rs[rs] : 0;
}

static XVIEWPORT get_vp(Device* d)
{
    if (d->s.vp_set)
        return d->s.vp;
    XVIEWPORT v = { 0, 0, 0, 0, 0.0f, 1.0f };
    bb_size(&v.w, &v.h);
    return v;
}

typedef struct Out
{
    XiD3DVert* v;
    uint32_t n, cap;
} Out;

static void out_push(Out* o, float x, float y, float u, float v, const float* c)
{
    if (o->n == o->cap)
    {
        uint32_t cap = o->cap ? o->cap * 2 : 1024;
        XiD3DVert* nv = (XiD3DVert*)realloc(o->v, cap * sizeof *nv);
        if (!nv)
            return;
        o->v = nv, o->cap = cap;
    }
    XiD3DVert* d = &o->v[o->n++];
    d->x = x, d->y = y, d->u = u, d->v = v;
    uint32_t b = (uint32_t)(c[0] + 0.5f), g = (uint32_t)(c[1] + 0.5f), r = (uint32_t)(c[2] + 0.5f), a = (uint32_t)(c[3] + 0.5f);
    d->argb = (a > 255 ? 255 : a) << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

typedef struct Ctx
{
    int rhw;
    XVIEWPORT vp;
    Out out;
} Ctx;

static void to_screen(const Ctx* c, const CV* v, float* x, float* y)
{
    if (c->rhw)
    {
        *x = v->x, *y = v->y;
        return;
    }
    float iw = 1.0f / v->w;
    *x = (float)c->vp.x + (v->x * iw + 1.0f) * 0.5f * (float)c->vp.w;
    *y = (float)c->vp.y + (1.0f - v->y * iw) * 0.5f * (float)c->vp.h;
}

static CV lerp(const CV* a, const CV* b, float t)
{
    CV r;
    r.x = a->x + (b->x - a->x) * t, r.y = a->y + (b->y - a->y) * t, r.z = a->z + (b->z - a->z) * t, r.w = a->w + (b->w - a->w) * t;
    r.u = a->u + (b->u - a->u) * t, r.v = a->v + (b->v - a->v) * t;
    for (int k = 0; k < 4; ++k)
        r.c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    return r;
}

/* distance to the near plane (D3D: z >= 0) */
static float near_d(const CV* v) { return v->z; }

static void emit_tri(Ctx* c, const CV* a, const CV* b, const CV* d)
{
    const CV* in[3] = { a, b, d };
    CV poly[4];
    int n = 0;
    if (c->rhw)
    {
        for (int i = 0; i < 3; ++i)
            poly[n++] = *in[i];
    }
    else
        for (int i = 0; i < 3; ++i)
        {
            const CV *p = in[i], *q = in[(i + 1) % 3];
            float dp = near_d(p), dq = near_d(q);
            int pin = dp >= 0 && p->w > 0, qin = dq >= 0 && q->w > 0;
            if (pin)
                poly[n++] = *p;
            if (pin != qin && n < 4)
            {
                float t = dp / (dp - dq);
                poly[n++] = lerp(p, q, t);
            }
        }
    for (int i = 1; i + 1 < n; ++i)
    {
        const CV* t[3] = { &poly[0], &poly[i], &poly[i + 1] };
        if (!c->rhw && (t[0]->w <= 0 || t[1]->w <= 0 || t[2]->w <= 0))
            continue;
        for (int k = 0; k < 3; ++k)
        {
            float x, y;
            to_screen(c, t[k], &x, &y);
            out_push(&c->out, x, y, t[k]->u, t[k]->v, t[k]->c);
        }
    }
}

/* A line as a quad a pixel wide (points as a pixel square). */
static void emit_line(Ctx* c, const CV* a, const CV* b)
{
    CV p = *a, q = *b;
    if (!c->rhw)
    {
        float dp = p.z, dq = q.z;
        if ((dp < 0 || p.w <= 0) && (dq < 0 || q.w <= 0))
            return;
        if (dp < 0 || p.w <= 0)
            p = lerp(&p, &q, dp / (dp - dq));
        else if (dq < 0 || q.w <= 0)
            q = lerp(&q, &p, dq / (dq - dp));
    }
    float x0, y0, x1, y1;
    to_screen(c, &p, &x0, &y0);
    to_screen(c, &q, &x1, &y1);
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    float nx = 0.5f, ny = 0;
    if (len < 1e-4f) /* a point: a pixel square */
        x0 -= 0.5f, x1 += 0.5f;
    else
        nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
    out_push(&c->out, x0 + nx, y0 + ny, p.u, p.v, p.c);
    out_push(&c->out, x1 + nx, y1 + ny, q.u, q.v, q.c);
    out_push(&c->out, x1 - nx, y1 - ny, q.u, q.v, q.c);
    out_push(&c->out, x0 + nx, y0 + ny, p.u, p.v, p.c);
    out_push(&c->out, x1 - nx, y1 - ny, q.u, q.v, q.c);
    out_push(&c->out, x0 - nx, y0 - ny, p.u, p.v, p.c);
}

static uint32_t prim_verts(uint32_t type, uint32_t count)
{
    switch (type)
    {
    case 1: return count;
    case 2: return count * 2;
    case 3: return count + 1;
    case 4: return count * 3;
    case 5: case 6: return count + 2;
    default: return 0;
    }
}

/* One draw: nverts vertices at data (stride), through indices (NULL: in order). */
static HR draw(Device* d, uint32_t type, uint32_t count, const uint8_t* data, size_t data_size, uint32_t stride,
    const void* indices, int index32, uint32_t base_vertex)
{
    uint32_t n = prim_verts(type, count);
    if (!n || !data || count > 1000000)
        return D3DERR_INVALIDCALL;
    uint32_t fvfcode = d->s.fvf;
    if (fvfcode & 1)
    {
        unsupported("drawing with a vertex shader");
        return D3DERR_INVALIDCALL;
    }
    Fvf f = fvf_parse(fvfcode);
    if (!f.ok)
        return D3DERR_INVALIDCALL;
    if (!stride)
        stride = (uint32_t)f.stride;
    Com* tex = d->s.tex[0];
    uint32_t colorop = d->s.tss_set[0][1] ? d->s.tss[0][1] : 4;
    uint32_t tid = tex && valid(tex, K_TEXTURE) && colorop != 1 && f.tex0 >= 0 ? ((Texture*)tex)->id : 0;
    float mvp[16];
    if (!f.rhw)
    {
        float w[16], v[16], p[16];
        get_xf(d, 256, w);
        get_xf(d, 2, v);
        get_xf(d, 3, p);
        mat_mul(mvp, w, v);
        mat_mul(mvp, mvp, p);
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    c.rhw = f.rhw;
    c.vp = get_vp(d);
    CV* cv = (CV*)malloc(sizeof(CV) * n);
    if (!cv)
        return E_OUTOFMEMORY_;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t k = indices ? (index32 ? ((const uint32_t*)indices)[i] : ((const uint16_t*)indices)[i]) : i;
        k += base_vertex;
        size_t at = (size_t)k * stride;
        if (at + (size_t)f.stride > data_size)
        {
            free(cv);
            free(c.out.v);
            return D3DERR_INVALIDCALL;
        }
        const uint8_t* s = data + at;
        float pos[4];
        memcpy(pos, s, f.rhw ? 16 : 12);
        CV* o = &cv[i];
        if (f.rhw)
            o->x = pos[0], o->y = pos[1], o->z = pos[2], o->w = pos[3];
        else
        {
            o->x = pos[0] * mvp[0] + pos[1] * mvp[4] + pos[2] * mvp[8] + mvp[12];
            o->y = pos[0] * mvp[1] + pos[1] * mvp[5] + pos[2] * mvp[9] + mvp[13];
            o->z = pos[0] * mvp[2] + pos[1] * mvp[6] + pos[2] * mvp[10] + mvp[14];
            o->w = pos[0] * mvp[3] + pos[1] * mvp[7] + pos[2] * mvp[11] + mvp[15];
        }
        uint32_t col = 0xFFFFFFFFu;
        if (f.diffuse >= 0)
            memcpy(&col, s + f.diffuse, 4);
        o->c[0] = (float)(col & 0xFF), o->c[1] = (float)((col >> 8) & 0xFF), o->c[2] = (float)((col >> 16) & 0xFF), o->c[3] = (float)(col >> 24);
        o->u = o->v = 0;
        if (f.tex0 >= 0)
        {
            memcpy(&o->u, s + f.tex0, 4);
            if (f.texdim > 1)
                memcpy(&o->v, s + f.tex0 + 4, 4);
        }
    }
    switch (type)
    {
    case 1:
        for (uint32_t i = 0; i < n; ++i)
            emit_line(&c, &cv[i], &cv[i]);
        break;
    case 2:
        for (uint32_t i = 0; i + 1 < n; i += 2)
            emit_line(&c, &cv[i], &cv[i + 1]);
        break;
    case 3:
        for (uint32_t i = 0; i + 1 < n; ++i)
            emit_line(&c, &cv[i], &cv[i + 1]);
        break;
    case 4:
        for (uint32_t i = 0; i + 2 < n; i += 3)
            emit_tri(&c, &cv[i], &cv[i + 1], &cv[i + 2]);
        break;
    case 5:
        for (uint32_t i = 0; i + 2 < n; ++i)
            if (i & 1)
                emit_tri(&c, &cv[i + 1], &cv[i], &cv[i + 2]);
            else
                emit_tri(&c, &cv[i], &cv[i + 1], &cv[i + 2]);
        break;
    case 6:
        for (uint32_t i = 1; i + 1 < n; ++i)
            emit_tri(&c, &cv[0], &cv[i], &cv[i + 1]);
        break;
    }
    free(cv);
    if (c.out.n)
        xi_gui_d3d_triangles(tid, c.out.v, c.out.n, (int)get_rs(d, 27), (int)get_rs(d, 19), (int)get_rs(d, 20));
    free(c.out.v);
    return S_OK_;
}

/* --- IDirect3DDevice8 --------------------------------------------------------------------- */

static uint32_t dev_addref(Device* d) { (void)d; return 1; }
static uint32_t dev_release(Device* d) { (void)d; return 1; }
static HR dev_ok(Device* d) { (void)d; return S_OK_; }
static uint32_t dev_texmem(Device* d) { (void)d; return 512u << 20; }
static HR dev_discard(Device* d, uint32_t n) { (void)d, (void)n; return S_OK_; }

static HR dev_getd3d(Device* d, void** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = &g_d3d8;
    return S_OK_;
}

static void fill_caps(XCAPS8* c)
{
    memset(c, 0, sizeof *c);
    c->DeviceType = 1; /* HAL */
    c->Caps2 = 0x20000000u | 0x80000000u; /* CANRENDERWINDOWED, FULLSCREENGAMMA */
    c->PresentationIntervals = 0x80000001u;
    c->DevCaps = 0x10 | 0x40 | 0x80 | 0x200 | 0x10000 | 0x80000; /* EXECUTESYSTEMMEMORY..HWTRANSFORMANDLIGHT */
    c->PrimitiveMiscCaps = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100;
    c->RasterCaps = 0x1 | 0x10 | 0x80 | 0x100;
    c->ZCmpCaps = c->AlphaCmpCaps = 0xFF;
    c->SrcBlendCaps = c->DestBlendCaps = 0x1FFF;
    c->ShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;
    c->TextureCaps = 0x1 | 0x4 | 0x40 | 0x800; /* PERSPECTIVE, ALPHA, CUBEMAP?, MIPMAP */
    c->TextureFilterCaps = c->CubeTextureFilterCaps = c->VolumeTextureFilterCaps = 0x0303 | 0x030000;
    c->TextureAddressCaps = c->VolumeTextureAddressCaps = 0x3F;
    c->LineCaps = 0x1F;
    c->MaxTextureWidth = c->MaxTextureHeight = 16384;
    c->MaxVolumeExtent = 2048;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 16384;
    c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = c->GuardBandTop = -32768.0f, c->GuardBandRight = c->GuardBandBottom = 32768.0f;
    c->StencilCaps = 0xFF;
    c->FVFCaps = 8;
    c->TextureOpCaps = 0x3FFFFFF;
    c->MaxTextureBlendStages = 8, c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = 0x3F;
    c->MaxActiveLights = 8, c->MaxUserClipPlanes = 6, c->MaxVertexBlendMatrices = 4, c->MaxVertexBlendMatrixIndex = 0;
    c->MaxPointSize = 64.0f;
    c->MaxPrimitiveCount = 0xFFFFF, c->MaxVertexIndex = 0xFFFFFF, c->MaxStreams = 16, c->MaxStreamStride = 256;
    c->VertexShaderVersion = 0xFFFE0101u, c->MaxVertexShaderConst = 96;
    c->PixelShaderVersion = 0xFFFF0104u;
    c->MaxPixelShaderValue = 8.0f;
}

static HR dev_caps(Device* d, XCAPS8* c)
{
    (void)d;
    if (!c)
        return D3DERR_INVALIDCALL;
    fill_caps(c);
    return S_OK_;
}

static HR dev_displaymode(Device* d, XDISPLAYMODE* m)
{
    (void)d;
    if (!m)
        return D3DERR_INVALIDCALL;
    bb_size(&m->width, &m->height);
    m->refresh = 60, m->format = FMT_X8R8G8B8;
    return S_OK_;
}

static HR dev_creation(Device* d, XCREATION_PARAMETERS* p)
{
    (void)d;
    if (!p)
        return D3DERR_INVALIDCALL;
    p->adapter = 0, p->device_type = 1, p->behavior = 0x40;
    p->hwnd = (void*)(uintptr_t)d3d8_window();
    return S_OK_;
}

static void dev_cursorpos(Device* d, int x, int y, uint32_t f) { (void)d, (void)x, (void)y, (void)f; }
static int dev_showcursor(Device* d, int show) { (void)d, (void)show; return 1; }

static HR dev_reset(Device* d, void* pp)
{
    (void)d, (void)pp;
    unsupported("IDirect3DDevice8::Reset");
    return D3DERR_INVALIDCALL;
}

static HR dev_present(Device* d) { (void)d; return S_OK_; }

static HR dev_backbuffer(Device* d, uint32_t n, uint32_t type, Surface** out)
{
    (void)d, (void)n, (void)type;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = surf_backbuffer(1);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

static HR dev_rasterstatus(Device* d, uint32_t* rs)
{
    (void)d;
    if (rs)
        rs[0] = 0, rs[1] = 0;
    return S_OK_;
}

static void dev_setgamma(Device* d, uint32_t f, const void* r) { (void)d, (void)f, (void)r; }

static void dev_getgamma(Device* d, uint16_t* r)
{
    (void)d;
    if (r)
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
                r[c * 256 + i] = (uint16_t)(i * 257);
}

static HR dev_createtexture(Device* d, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
    Texture** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    if (usage & 1)
        unsupported("render target textures (drawn into: never; sampled: as locked)");
    *out = tex_create(w, h, levels, usage, fmt, pool);
    return *out ? S_OK_ : D3DERR_INVALIDCALL;
}

static HR dev_createvb(Device* d, uint32_t len, uint32_t usage, uint32_t fvf, uint32_t pool, Buffer** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = buf_create(K_VERTEXBUFFER, len, usage, fvf, pool);
    return *out ? S_OK_ : D3DERR_INVALIDCALL;
}

static HR dev_createib(Device* d, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, Buffer** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = buf_create(K_INDEXBUFFER, len, usage, fmt, pool);
    return *out ? S_OK_ : D3DERR_INVALIDCALL;
}

static HR dev_createimage(Device* d, uint32_t w, uint32_t h, uint32_t fmt, Surface** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = surf_image(w, h, fmt);
    return *out ? S_OK_ : D3DERR_INVALIDCALL;
}

static HR dev_copyrects(Device* d, Surface* src, const XRECT* rects, uint32_t n, Surface* dst, const int32_t* points)
{
    (void)d;
    if (!valid(src, K_SURFACE) || !valid(dst, K_SURFACE))
        return D3DERR_INVALIDCALL;
    XRECT all;
    Level* sl = surf_level(src);
    if (!sl || !surf_level(dst))
        return D3DERR_INVALIDCALL;
    if (!rects || !n)
    {
        all.left = all.top = 0, all.right = (int32_t)sl->w, all.bottom = (int32_t)sl->h;
        rects = &all, n = 1;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        int w, h;
        uint8_t* px = surf_read(src, &rects[i], &w, &h);
        if (!px)
            return D3DERR_INVALIDCALL;
        XRECT to = { points ? points[i * 2] : rects[i].left, points ? points[i * 2 + 1] : rects[i].top, 0, 0 };
        to.right = to.left + w, to.bottom = to.top + h;
        HR hr = surf_write(dst, &to, px, w, h, 0);
        free(px);
        if (hr != S_OK_)
            return hr;
    }
    return S_OK_;
}

static HR dev_updatetexture(Device* d, Texture* src, Texture* dst)
{
    (void)d;
    if (!valid(src, K_TEXTURE) || !valid(dst, K_TEXTURE) || src->format != dst->format)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < src->nlevels && i < dst->nlevels; ++i)
        if (src->lv[i].w == dst->lv[i].w && src->lv[i].h == dst->lv[i].h)
            memcpy(dst->lv[i].data, src->lv[i].data, level_bytes(&src->lv[i], src->format));
    tex_upload(dst);
    return S_OK_;
}

static HR dev_setrendertarget(Device* d, Surface* rt, Surface* ds)
{
    (void)d, (void)ds;
    if (!rt || (valid(rt, K_SURFACE) && rt->backbuffer == 1))
        return S_OK_;
    unsupported("SetRenderTarget to anything but the back buffer");
    return D3DERR_INVALIDCALL;
}

static HR dev_getrendertarget(Device* d, Surface** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = surf_backbuffer(1);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

static HR dev_getdepth(Device* d, Surface** out)
{
    (void)d;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = surf_backbuffer(2);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

static HR dev_clear(Device* d, uint32_t n, const void* rects, uint32_t flags, uint32_t color, float z, uint32_t st)
{
    (void)d, (void)n, (void)rects, (void)flags, (void)color, (void)z, (void)st;
    unsupported("IDirect3DDevice8::Clear (ignored: it would clear the game's frame)");
    return S_OK_;
}

static DState* rec(Device* d) { return d->recording ? d->blocks[d->recording - 1] : NULL; }

static HR dev_settransform(Device* d, uint32_t ts, const float* m)
{
    dev_sync();
    int i = xf_index(ts);
    if (i < 0 || !m)
        return D3DERR_INVALIDCALL;
    DState* s = rec(d) ? rec(d) : &d->s;
    memcpy(s->xf[i], m, sizeof(float) * 16);
    s->xf_set[i] = 1;
    return S_OK_;
}

static HR dev_gettransform(Device* d, uint32_t ts, float* m)
{
    dev_sync();
    if (xf_index(ts) < 0 || !m)
        return D3DERR_INVALIDCALL;
    get_xf(d, ts, m);
    return S_OK_;
}

static HR dev_multransform(Device* d, uint32_t ts, const float* m)
{
    float cur[16];
    if (dev_gettransform(d, ts, cur) != S_OK_ || !m)
        return D3DERR_INVALIDCALL;
    mat_mul(cur, m, cur);
    return dev_settransform(d, ts, cur);
}

static HR dev_setviewport(Device* d, const XVIEWPORT* v)
{
    dev_sync();
    if (!v)
        return D3DERR_INVALIDCALL;
    DState* s = rec(d) ? rec(d) : &d->s;
    s->vp = *v, s->vp_set = 1;
    return S_OK_;
}

static HR dev_getviewport(Device* d, XVIEWPORT* v)
{
    dev_sync();
    if (!v)
        return D3DERR_INVALIDCALL;
    *v = get_vp(d);
    return S_OK_;
}

static HR dev_setmaterial(Device* d, const float* m)
{
    if (m)
        memcpy(d->s.material, m, sizeof d->s.material);
    return S_OK_;
}

static HR dev_getmaterial(Device* d, float* m)
{
    if (!m)
        return D3DERR_INVALIDCALL;
    memcpy(m, d->s.material, sizeof d->s.material);
    return S_OK_;
}

static HR dev_setlight(Device* d, uint32_t i, const void* l) { (void)d, (void)i, (void)l; return S_OK_; }

static HR dev_getlight(Device* d, uint32_t i, void* l)
{
    (void)d, (void)i;
    if (l)
        memset(l, 0, 104);
    return S_OK_;
}

static HR dev_lightenable(Device* d, uint32_t i, int on) { (void)d, (void)i, (void)on; return S_OK_; }

static HR dev_getlightenable(Device* d, uint32_t i, int* on)
{
    (void)d, (void)i;
    if (on)
        *on = 0;
    return S_OK_;
}

static HR dev_setclipplane(Device* d, uint32_t i, const float* p) { (void)d, (void)i, (void)p; return S_OK_; }

static HR dev_getclipplane(Device* d, uint32_t i, float* p)
{
    (void)d, (void)i;
    if (p)
        memset(p, 0, 16);
    return S_OK_;
}

static HR dev_setrs(Device* d, uint32_t rs, uint32_t v)
{
    dev_sync();
    if (rs >= 256)
        return D3DERR_INVALIDCALL;
    DState* s = rec(d) ? rec(d) : &d->s;
    s->rs[rs] = v, s->rs_set[rs] = 1;
    return S_OK_;
}

static HR dev_getrs(Device* d, uint32_t rs, uint32_t* v)
{
    dev_sync();
    if (rs >= 256 || !v)
        return D3DERR_INVALIDCALL;
    *v = get_rs(d, rs);
    return S_OK_;
}

/* State blocks: what an addon set (on top of the game's state) - enough to save and restore. */
static int block_new(Device* d)
{
    for (int i = 0; i < 256; ++i)
        if (!d->blocks[i])
        {
            d->blocks[i] = (DState*)calloc(1, sizeof(DState));
            d->block_full[i] = 0;
            return d->blocks[i] ? i + 1 : 0;
        }
    return 0;
}

static HR dev_beginblock(Device* d)
{
    if (d->recording)
        return D3DERR_INVALIDCALL;
    int t = block_new(d);
    if (!t)
        return E_OUTOFMEMORY_;
    d->recording = t;
    return S_OK_;
}

static HR dev_endblock(Device* d, uint32_t* token)
{
    if (!d->recording || !token)
        return D3DERR_INVALIDCALL;
    *token = (uint32_t)d->recording;
    d->recording = 0;
    return S_OK_;
}

static HR dev_createblock(Device* d, uint32_t type, uint32_t* token)
{
    dev_sync();
    (void)type;
    if (!token)
        return D3DERR_INVALIDCALL;
    int t = block_new(d);
    if (!t)
        return E_OUTOFMEMORY_;
    *d->blocks[t - 1] = d->s;
    d->block_full[t - 1] = 1;
    *token = (uint32_t)t;
    return S_OK_;
}

static DState* block_of(Device* d, uint32_t token) { return token >= 1 && token <= 256 ? d->blocks[token - 1] : NULL; }

static HR dev_applyblock(Device* d, uint32_t token)
{
    dev_sync();
    DState* b = block_of(d, token);
    if (!b)
        return D3DERR_INVALIDCALL;
    if (d->block_full[token - 1])
    {
        d->s = *b;
        return S_OK_;
    }
    /* recorded: only what was set while recording */
    DState* to = &d->s;
    for (int i = 0; i < 256; ++i)
        if (b->rs_set[i])
            to->rs[i] = b->rs[i], to->rs_set[i] = 1;
    for (int t = 0; t < 8; ++t)
    {
        for (int i = 0; i < 32; ++i)
            if (b->tss_set[t][i])
                to->tss[t][i] = b->tss[t][i], to->tss_set[t][i] = 1;
        if (b->tex_set[t])
            to->tex[t] = b->tex[t];
    }
    for (int i = 0; i < NXF; ++i)
        if (b->xf_set[i])
            memcpy(to->xf[i], b->xf[i], sizeof to->xf[i]), to->xf_set[i] = 1;
    if (b->vp_set)
        to->vp = b->vp, to->vp_set = 1;
    if (b->fvf_set)
        to->fvf = b->fvf;
    return S_OK_;
}

static HR dev_captureblock(Device* d, uint32_t token)
{
    dev_sync();
    DState* b = block_of(d, token);
    if (!b)
        return D3DERR_INVALIDCALL;
    if (d->block_full[token - 1])
        *b = d->s;
    else
    {
        /* a recorded block captures the current values of what it holds */
        DState* from = &d->s;
        for (int i = 0; i < 256; ++i)
            if (b->rs_set[i])
                b->rs[i] = get_rs(d, (uint32_t)i);
        for (int t = 0; t < 8; ++t)
        {
            for (int i = 0; i < 32; ++i)
                if (b->tss_set[t][i])
                    b->tss[t][i] = from->tss[t][i];
            if (b->tex_set[t])
                b->tex[t] = from->tex[t];
        }
        for (int i = 0; i < NXF; ++i)
            if (b->xf_set[i])
                get_xf(d, i < 24 ? (uint32_t)i : (uint32_t)(256 + i - 24), b->xf[i]);
        if (b->vp_set)
            b->vp = get_vp(d);
        if (b->fvf_set)
            b->fvf = from->fvf;
    }
    return S_OK_;
}

static HR dev_deleteblock(Device* d, uint32_t token)
{
    DState* b = block_of(d, token);
    if (!b)
        return D3DERR_INVALIDCALL;
    free(b);
    d->blocks[token - 1] = NULL;
    d->block_full[token - 1] = 0;
    return S_OK_;
}

static HR dev_setclipstatus(Device* d, const void* s) { (void)d, (void)s; return S_OK_; }

static HR dev_getclipstatus(Device* d, uint32_t* s)
{
    (void)d;
    if (s)
        s[0] = s[1] = 0;
    return S_OK_;
}

static HR dev_gettexture(Device* d, uint32_t stage, Com** out)
{
    dev_sync();
    if (stage >= 8 || !out)
        return D3DERR_INVALIDCALL;
    *out = d->s.tex[stage];
    if (*out)
        ++(*out)->refs;
    return S_OK_;
}

static HR dev_settexture(Device* d, uint32_t stage, Com* t)
{
    dev_sync();
    if (stage >= 8)
        return D3DERR_INVALIDCALL;
    if (t && !valid(t, K_TEXTURE))
    {
        unsupported("SetTexture with a texture the host didn't make");
        t = NULL;
    }
    DState* s = rec(d) ? rec(d) : &d->s;
    s->tex[stage] = t, s->tex_set[stage] = 1;
    return S_OK_;
}

static HR dev_gettss(Device* d, uint32_t stage, uint32_t type, uint32_t* v)
{
    dev_sync();
    if (stage >= 8 || type >= 32 || !v)
        return D3DERR_INVALIDCALL;
    *v = d->s.tss[stage][type];
    return S_OK_;
}

static HR dev_settss(Device* d, uint32_t stage, uint32_t type, uint32_t v)
{
    dev_sync();
    if (stage >= 8 || type >= 32)
        return D3DERR_INVALIDCALL;
    DState* s = rec(d) ? rec(d) : &d->s;
    s->tss[stage][type] = v, s->tss_set[stage][type] = 1;
    return S_OK_;
}

static HR dev_validate(Device* d, uint32_t* passes)
{
    (void)d;
    if (passes)
        *passes = 1;
    return S_OK_;
}

static HR dev_getinfo(Device* d, uint32_t id, void* p, uint32_t n) { (void)d, (void)id, (void)p, (void)n; return S_FALSE_; }
static HR dev_setpalette(Device* d, uint32_t n, const void* p) { (void)d, (void)n, (void)p; return S_OK_; }

static HR dev_getpalette(Device* d, uint32_t n, uint8_t* p)
{
    (void)d, (void)n;
    if (p)
        memset(p, 0xFF, 1024);
    return S_OK_;
}

static HR dev_setcurpalette(Device* d, uint32_t n) { (void)d, (void)n; return S_OK_; }

static HR dev_getcurpalette(Device* d, uint32_t* n)
{
    (void)d;
    if (n)
        *n = 0;
    return S_OK_;
}

static HR dev_drawprim(Device* d, uint32_t type, uint32_t start, uint32_t count)
{
    dev_sync();
    Buffer* vb = d->s.stream;
    if (!vb || !d->s.stride)
        return D3DERR_INVALIDCALL;
    return draw(d, type, count, vb->data, vb->size, d->s.stride, NULL, 0, start);
}

static HR dev_drawindexed(Device* d, uint32_t type, uint32_t minindex, uint32_t nverts, uint32_t start, uint32_t count)
{
    dev_sync();
    (void)minindex, (void)nverts;
    Buffer *vb = d->s.stream, *ib = d->s.indices;
    if (!vb || !ib || !d->s.stride)
        return D3DERR_INVALIDCALL;
    int i32 = ib->format == FMT_INDEX32;
    size_t isz = i32 ? 4 : 2;
    uint32_t n = prim_verts(type, count);
    if ((size_t)(start + n) * isz > ib->size)
        return D3DERR_INVALIDCALL;
    return draw(d, type, count, vb->data, vb->size, d->s.stride, ib->data + (size_t)start * isz, i32, d->s.base_vertex);
}

static HR dev_drawup(Device* d, uint32_t type, uint32_t count, const void* data, uint32_t stride)
{
    dev_sync();
    uint32_t n = prim_verts(type, count);
    return draw(d, type, count, (const uint8_t*)data, (size_t)n * stride, stride, NULL, 0, 0);
}

static HR dev_drawindexedup(Device* d, uint32_t type, uint32_t minindex, uint32_t nverts, uint32_t count, const void* indices,
    uint32_t ifmt, const void* data, uint32_t stride)
{
    dev_sync();
    if (!indices)
        return D3DERR_INVALIDCALL;
    return draw(d, type, count, (const uint8_t*)data, (size_t)(minindex + nverts) * stride, stride, indices, ifmt == FMT_INDEX32, 0);
}

static HR dev_createvs(Device* d, const uint32_t* decl, const uint32_t* fn, uint32_t* handle, uint32_t usage)
{
    (void)d, (void)decl, (void)fn, (void)usage;
    unsupported("vertex shaders");
    if (handle)
        *handle = 0;
    return D3DERR_INVALIDCALL;
}

static HR dev_setvs(Device* d, uint32_t handle)
{
    dev_sync();
    DState* s = rec(d) ? rec(d) : &d->s;
    s->fvf = handle, s->fvf_set = 1;
    return S_OK_;
}

static HR dev_getvs(Device* d, uint32_t* handle)
{
    dev_sync();
    if (!handle)
        return D3DERR_INVALIDCALL;
    *handle = d->s.fvf;
    return S_OK_;
}

static HR dev_deletevs(Device* d, uint32_t h) { (void)d, (void)h; return S_OK_; }
static HR dev_setconst(Device* d, uint32_t r, const void* p, uint32_t n) { (void)d, (void)r, (void)p, (void)n; return S_OK_; }

static HR dev_getconst(Device* d, uint32_t r, void* p, uint32_t n)
{
    (void)d, (void)r;
    if (p)
        memset(p, 0, (size_t)n * 16);
    return S_OK_;
}

static HR dev_setstream(Device* d, uint32_t n, Buffer* vb, uint32_t stride)
{
    dev_sync();
    if (n >= 16)
        return D3DERR_INVALIDCALL;
    if (n)
        return S_OK_; /* only stream 0 is drawn from */
    if (vb && !valid(vb, K_VERTEXBUFFER))
        return D3DERR_INVALIDCALL;
    d->s.stream = vb, d->s.stride = stride;
    return S_OK_;
}

static HR dev_getstream(Device* d, uint32_t n, Buffer** vb, uint32_t* stride)
{
    dev_sync();
    if (!vb || !stride)
        return D3DERR_INVALIDCALL;
    *vb = n ? NULL : d->s.stream;
    *stride = n ? 0 : d->s.stride;
    if (*vb)
        ++(*vb)->h.refs;
    return S_OK_;
}

static HR dev_setindices(Device* d, Buffer* ib, uint32_t base)
{
    dev_sync();
    if (ib && !valid(ib, K_INDEXBUFFER))
        return D3DERR_INVALIDCALL;
    d->s.indices = ib, d->s.base_vertex = base;
    return S_OK_;
}

static HR dev_getindices(Device* d, Buffer** ib, uint32_t* base)
{
    dev_sync();
    if (!ib || !base)
        return D3DERR_INVALIDCALL;
    *ib = d->s.indices, *base = d->s.base_vertex;
    if (*ib)
        ++(*ib)->h.refs;
    return S_OK_;
}

static HR dev_createps(Device* d, const uint32_t* fn, uint32_t* handle)
{
    (void)d, (void)fn;
    unsupported("pixel shaders");
    if (handle)
        *handle = 0;
    return D3DERR_INVALIDCALL;
}

static HR dev_setps(Device* d, uint32_t h)
{
    (void)d;
    if (h)
        unsupported("pixel shaders");
    return h ? D3DERR_INVALIDCALL : S_OK_;
}

static HR dev_getps(Device* d, uint32_t* h)
{
    (void)d;
    if (h)
        *h = 0;
    return S_OK_;
}

/* --- IDirect3D8 ----------------------------------------------------------------------------- */

static uint32_t d8_count(Com* s) { (void)s; return 1; }

static HR d8_identifier(Com* s, uint32_t adapter, uint32_t flags, XADAPTER_IDENTIFIER8* id)
{
    (void)s, (void)flags;
    if (adapter || !id)
        return D3DERR_INVALIDCALL;
    memset(id, 0, sizeof *id);
    snprintf(id->driver, sizeof id->driver, "ffxirecompile");
    snprintf(id->description, sizeof id->description, "FFXIRecompile overlay");
    return S_OK_;
}

static uint32_t d8_modecount(Com* s, uint32_t adapter) { (void)s; return adapter ? 0 : 1; }

static HR d8_mode(Com* s, uint32_t adapter, XDISPLAYMODE* m)
{
    (void)s;
    if (adapter)
        return D3DERR_INVALIDCALL;
    return dev_displaymode(&g_device, m);
}

static HR d8_enummode(Com* s, uint32_t adapter, uint32_t mode, XDISPLAYMODE* m)
{
    if (mode)
        return D3DERR_INVALIDCALL;
    return d8_mode(s, adapter, m);
}

static HR d8_ok(Com* s) { (void)s; return S_OK_; }

static HR d8_multisample(Com* s, uint32_t a, uint32_t t, uint32_t f, int w, uint32_t ms)
{
    (void)s, (void)a, (void)t, (void)f, (void)w;
    return ms ? D3DERR_NOTAVAILABLE : S_OK_;
}

static HR d8_caps(Com* s, uint32_t adapter, uint32_t type, XCAPS8* c)
{
    (void)s, (void)type;
    if (adapter)
        return D3DERR_INVALIDCALL;
    return dev_caps(&g_device, c);
}

static void* d8_monitor(Com* s, uint32_t a) { (void)s, (void)a; return NULL; }

static HR d8_createdevice(Com* s)
{
    (void)s;
    unsupported("IDirect3D8::CreateDevice");
    return D3DERR_INVALIDCALL;
}

static uint32_t d8_addref(Com* s) { (void)s; return 1; }

/* --- ID3DXSprite ---------------------------------------------------------------------------- */

static uint32_t sprite_release(Sprite* s)
{
    if (--s->h.refs > 0)
        return (uint32_t)s->h.refs;
    obj_free(s);
    return 0;
}

static HR sprite_begin(Sprite* s)
{
    s->begun = 1;
    return S_OK_;
}

static HR sprite_end(Sprite* s)
{
    s->begun = 0;
    return S_OK_;
}

static HR sprite_ok(Sprite* s) { (void)s; return S_OK_; }

/* The quad (the source rect's size at the origin) through a 2D transform: x' = x*_11 + y*_21 + _41. */
static HR sprite_drawxf(Sprite* s, Texture* t, const XRECT* src, const float* m, uint32_t color)
{
    (void)s;
    if (!valid(t, K_TEXTURE) || !m)
        return D3DERR_INVALIDCALL;
    float tw = (float)t->lv[0].w, th = (float)t->lv[0].h;
    XRECT r = { 0, 0, (int32_t)tw, (int32_t)th };
    if (src)
        r = *src;
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float u0 = (float)r.left / tw, v0 = (float)r.top / th, u1 = (float)r.right / tw, v1 = (float)r.bottom / th;
    float cx[4] = { 0, w, w, 0 }, cy[4] = { 0, 0, h, h }, cu[4] = { u0, u1, u1, u0 }, cv[4] = { v0, v0, v1, v1 };
    XiD3DVert q[4];
    for (int i = 0; i < 4; ++i)
    {
        float wd = cx[i] * m[3] + cy[i] * m[7] + m[15];
        if (wd == 0)
            wd = 1;
        q[i].x = (cx[i] * m[0] + cy[i] * m[4] + m[12]) / wd;
        q[i].y = (cx[i] * m[1] + cy[i] * m[5] + m[13]) / wd;
        q[i].u = cu[i], q[i].v = cv[i];
        q[i].argb = color;
    }
    XiD3DVert tri[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
    xi_gui_d3d_triangles(t->id, tri, 6, 1, 5, 6);
    return S_OK_;
}

/* D3DXMatrixTransformation2D(NULL, 0, scaling, rotation center, rotation, translation). */
static HR sprite_draw(Sprite* s, Texture* t, const XRECT* src, const XVEC2* scale, const XVEC2* center, float rot,
    const XVEC2* trans, uint32_t color)
{
    float sx = scale ? scale->x : 1, sy = scale ? scale->y : 1;
    float cx = center ? center->x : 0, cy = center ? center->y : 0;
    float tx = trans ? trans->x : 0, ty = trans ? trans->y : 0;
    float c = cosf(rot), sn = sinf(rot);
    /* scale, then rotate about the centre, then translate (row vectors) */
    float m[16] = { sx * c, sx * sn, 0, 0, -sy * sn, sy * c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    m[12] = cx - cx * c + cy * sn + tx;
    m[13] = cy - cx * sn - cy * c + ty;
    return sprite_drawxf(s, t, src, m, color);
}

/* --- ID3DXFont ------------------------------------------------------------------------------ */

static uint32_t font_release(Font* f)
{
    if (--f->h.refs > 0)
        return (uint32_t)f->h.refs;
    obj_free(f);
    return 0;
}

static HR font_getlogfont(Font* f, XLOGFONTA* lf)
{
    if (!lf)
        return D3DERR_INVALIDCALL;
    *lf = f->lf;
    return S_OK_;
}

static HR font_ok(Font* f) { (void)f; return S_OK_; }

static int utf8_valid(const char* s, size_t n)
{
    for (size_t i = 0; i < n;)
    {
        uint8_t c = (uint8_t)s[i];
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > n)
            return 0;
        for (size_t j = 1; j < k; ++j)
            if (((uint8_t)s[i + j] & 0xC0) != 0x80)
                return 0;
        i += k;
    }
    return 1;
}

static size_t put_utf8(char* o, uint32_t c)
{
    if (c < 0x80)
        return o[0] = (char)c, 1;
    if (c < 0x800)
        return o[0] = (char)(0xC0 | (c >> 6)), o[1] = (char)(0x80 | (c & 63)), 2;
    if (c < 0x10000)
        return o[0] = (char)(0xE0 | (c >> 12)), o[1] = (char)(0x80 | ((c >> 6) & 63)), o[2] = (char)(0x80 | (c & 63)), 3;
    return o[0] = (char)(0xF0 | (c >> 18)), o[1] = (char)(0x80 | ((c >> 12) & 63)), o[2] = (char)(0x80 | ((c >> 6) & 63)),
           o[3] = (char)(0x80 | (c & 63)), 4;
}

/* DrawText: lines (split at \n unless DT_SINGLELINE) placed in the rect; returns the height. */
static int font_draw_utf8(Font* f, const char* s, size_t n, XRECT* r, uint32_t fmt, uint32_t color)
{
    enum { DT_CENTER = 1, DT_RIGHT = 2, DT_VCENTER = 4, DT_BOTTOM = 8, DT_SINGLELINE = 0x20, DT_CALCRECT = 0x400 };
    XRECT zero = { 0, 0, 0, 0 };
    if (!r)
        r = &zero;
    const char* lines[256];
    size_t lens[256];
    int nl = 0;
    size_t st = 0;
    for (size_t i = 0; i <= n && nl < 256; ++i)
        if (i == n || (s[i] == '\n' && !(fmt & DT_SINGLELINE)))
        {
            size_t e = i;
            if (e > st && s[e - 1] == '\r')
                --e;
            lines[nl] = s + st, lens[nl] = e - st, ++nl;
            st = i + 1;
        }
    float lw[256], lh = f->size, total = 0, maxw = 0;
    for (int i = 0; i < nl; ++i)
    {
        float w = 0, h = 0;
        xi_gui_d3d_text(f->family, f->size, f->bold, f->italic, lines[i], lens[i], 0, 0, color, &w, &h, 0);
        lw[i] = w;
        if (h > lh)
            lh = h;
        if (w > maxw)
            maxw = w;
    }
    total = lh * (float)nl;
    if (fmt & DT_CALCRECT)
    {
        r->right = r->left + (int32_t)ceilf(maxw);
        r->bottom = r->top + (int32_t)ceilf(total);
        return (int)ceilf(total);
    }
    float y = (float)r->top;
    if (fmt & DT_BOTTOM)
        y = (float)r->bottom - total;
    else if (fmt & DT_VCENTER)
        y = ((float)r->top + (float)r->bottom - total) * 0.5f;
    for (int i = 0; i < nl; ++i, y += lh)
    {
        float x = (float)r->left;
        if (fmt & DT_RIGHT)
            x = (float)r->right - lw[i];
        else if (fmt & DT_CENTER)
            x = ((float)r->left + (float)r->right - lw[i]) * 0.5f;
        xi_gui_d3d_text(f->family, f->size, f->bold, f->italic, lines[i], lens[i], floorf(x), floorf(y), color, NULL, NULL, 1);
    }
    return (int)ceilf(total);
}

static int font_drawa(Font* f, const char* s, int count, XRECT* r, uint32_t fmt, uint32_t color)
{
    if (!s)
        return 0;
    size_t n = count < 0 ? strlen(s) : (size_t)count;
    if (utf8_valid(s, n))
        return font_draw_utf8(f, s, n, r, fmt, color);
    char* u = (char*)malloc(n * 2 + 1); /* Latin-1 */
    if (!u)
        return 0;
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
        k += put_utf8(u + k, (uint8_t)s[i]);
    int h = font_draw_utf8(f, u, k, r, fmt, color);
    free(u);
    return h;
}

static size_t wlen(const wchar_t* s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

/* wchar_t text (UTF-16 on Windows, UTF-32 elsewhere, as the ffi's wchar_t) to UTF-8: malloc'd. */
static char* wide_utf8(const wchar_t* s, int count, size_t* out_n)
{
    size_t n = count < 0 ? wlen(s) : (size_t)count;
    char* u = (char*)malloc(n * 4 + 1);
    if (!u)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
    {
        uint32_t c = (uint32_t)s[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < n)
        {
            uint32_t lo = (uint32_t)s[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000)
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), ++i;
        }
        k += put_utf8(u + k, c);
    }
    u[k] = 0;
    *out_n = k;
    return u;
}

static int font_draww(Font* f, const wchar_t* s, int count, XRECT* r, uint32_t fmt, uint32_t color)
{
    if (!s)
        return 0;
    size_t n;
    char* u = wide_utf8(s, count, &n);
    if (!u)
        return 0;
    int h = font_draw_utf8(f, u, n, r, fmt, color);
    free(u);
    return h;
}

static Font* font_from_logfont(const XLOGFONTA* lf)
{
    Font* f = (Font*)obj_alloc(sizeof(Font), K_FONT, NULL);
    if (!f)
        return NULL;
    f->lf = *lf;
    memcpy(f->family, lf->face, 32);
    f->family[32] = 0;
    if (!f->family[0])
        snprintf(f->family, sizeof f->family, "Arial");
    /* negative: the character height; positive: the cell height (about 1.2x the character) */
    f->size = lf->height < 0 ? (float)-lf->height * 1.15f : lf->height > 0 ? (float)lf->height : 16.0f;
    f->bold = lf->weight >= 600;
    f->italic = lf->italic != 0;
    return f;
}

/* --- IUnknown for the static objects -------------------------------------------------------- */

static HR static_qi(Com* self, const void* iid, void** out)
{
    (void)iid;
    if (!out)
        return E_NOINTERFACE_;
    *out = self;
    return S_OK_;
}

static uint32_t tex_addref_(Com* c) { return com_addref(c); }

/* --- the tables ----------------------------------------------------------------------------- */

#define SET(k, n, f) vt_set(k, n, (void*)(f))

static void build(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    for (int k = 0; k < K_KINDS; ++k)
        for (int i = 0; i < 97; ++i)
            g_vt[k][i] = STUBS[i];
    /* device */
    SET(K_DEVICE, "QueryInterface", static_qi);
    SET(K_DEVICE, "AddRef", dev_addref);
    SET(K_DEVICE, "Release", dev_release);
    SET(K_DEVICE, "TestCooperativeLevel", dev_ok);
    SET(K_DEVICE, "GetAvailableTextureMem", dev_texmem);
    SET(K_DEVICE, "ResourceManagerDiscardBytes", dev_discard);
    SET(K_DEVICE, "GetDirect3D", dev_getd3d);
    SET(K_DEVICE, "GetDeviceCaps", dev_caps);
    SET(K_DEVICE, "GetDisplayMode", dev_displaymode);
    SET(K_DEVICE, "GetCreationParameters", dev_creation);
    SET(K_DEVICE, "SetCursorProperties", dev_ok);
    SET(K_DEVICE, "SetCursorPosition", dev_cursorpos);
    SET(K_DEVICE, "ShowCursor", dev_showcursor);
    SET(K_DEVICE, "Reset", dev_reset);
    SET(K_DEVICE, "Present", dev_present);
    SET(K_DEVICE, "GetBackBuffer", dev_backbuffer);
    SET(K_DEVICE, "GetRasterStatus", dev_rasterstatus);
    SET(K_DEVICE, "SetGammaRamp", dev_setgamma);
    SET(K_DEVICE, "GetGammaRamp", dev_getgamma);
    SET(K_DEVICE, "CreateTexture", dev_createtexture);
    SET(K_DEVICE, "CreateVertexBuffer", dev_createvb);
    SET(K_DEVICE, "CreateIndexBuffer", dev_createib);
    SET(K_DEVICE, "CreateImageSurface", dev_createimage);
    SET(K_DEVICE, "CopyRects", dev_copyrects);
    SET(K_DEVICE, "UpdateTexture", dev_updatetexture);
    SET(K_DEVICE, "SetRenderTarget", dev_setrendertarget);
    SET(K_DEVICE, "GetRenderTarget", dev_getrendertarget);
    SET(K_DEVICE, "GetDepthStencilSurface", dev_getdepth);
    SET(K_DEVICE, "BeginScene", dev_ok);
    SET(K_DEVICE, "EndScene", dev_ok);
    SET(K_DEVICE, "Clear", dev_clear);
    SET(K_DEVICE, "SetTransform", dev_settransform);
    SET(K_DEVICE, "GetTransform", dev_gettransform);
    SET(K_DEVICE, "MultiplyTransform", dev_multransform);
    SET(K_DEVICE, "SetViewport", dev_setviewport);
    SET(K_DEVICE, "GetViewport", dev_getviewport);
    SET(K_DEVICE, "SetMaterial", dev_setmaterial);
    SET(K_DEVICE, "GetMaterial", dev_getmaterial);
    SET(K_DEVICE, "SetLight", dev_setlight);
    SET(K_DEVICE, "GetLight", dev_getlight);
    SET(K_DEVICE, "LightEnable", dev_lightenable);
    SET(K_DEVICE, "GetLightEnable", dev_getlightenable);
    SET(K_DEVICE, "SetClipPlane", dev_setclipplane);
    SET(K_DEVICE, "GetClipPlane", dev_getclipplane);
    SET(K_DEVICE, "SetRenderState", dev_setrs);
    SET(K_DEVICE, "GetRenderState", dev_getrs);
    SET(K_DEVICE, "BeginStateBlock", dev_beginblock);
    SET(K_DEVICE, "EndStateBlock", dev_endblock);
    SET(K_DEVICE, "ApplyStateBlock", dev_applyblock);
    SET(K_DEVICE, "CaptureStateBlock", dev_captureblock);
    SET(K_DEVICE, "DeleteStateBlock", dev_deleteblock);
    SET(K_DEVICE, "CreateStateBlock", dev_createblock);
    SET(K_DEVICE, "SetClipStatus", dev_setclipstatus);
    SET(K_DEVICE, "GetClipStatus", dev_getclipstatus);
    SET(K_DEVICE, "GetTexture", dev_gettexture);
    SET(K_DEVICE, "SetTexture", dev_settexture);
    SET(K_DEVICE, "GetTextureStageState", dev_gettss);
    SET(K_DEVICE, "SetTextureStageState", dev_settss);
    SET(K_DEVICE, "ValidateDevice", dev_validate);
    SET(K_DEVICE, "GetInfo", dev_getinfo);
    SET(K_DEVICE, "SetPaletteEntries", dev_setpalette);
    SET(K_DEVICE, "GetPaletteEntries", dev_getpalette);
    SET(K_DEVICE, "SetCurrentTexturePalette", dev_setcurpalette);
    SET(K_DEVICE, "GetCurrentTexturePalette", dev_getcurpalette);
    SET(K_DEVICE, "DrawPrimitive", dev_drawprim);
    SET(K_DEVICE, "DrawIndexedPrimitive", dev_drawindexed);
    SET(K_DEVICE, "DrawPrimitiveUP", dev_drawup);
    SET(K_DEVICE, "DrawIndexedPrimitiveUP", dev_drawindexedup);
    SET(K_DEVICE, "CreateVertexShader", dev_createvs);
    SET(K_DEVICE, "SetVertexShader", dev_setvs);
    SET(K_DEVICE, "GetVertexShader", dev_getvs);
    SET(K_DEVICE, "DeleteVertexShader", dev_deletevs);
    SET(K_DEVICE, "SetVertexShaderConstant", dev_setconst);
    SET(K_DEVICE, "GetVertexShaderConstant", dev_getconst);
    SET(K_DEVICE, "SetStreamSource", dev_setstream);
    SET(K_DEVICE, "GetStreamSource", dev_getstream);
    SET(K_DEVICE, "SetIndices", dev_setindices);
    SET(K_DEVICE, "GetIndices", dev_getindices);
    SET(K_DEVICE, "CreatePixelShader", dev_createps);
    SET(K_DEVICE, "SetPixelShader", dev_setps);
    SET(K_DEVICE, "GetPixelShader", dev_getps);
    SET(K_DEVICE, "DeletePixelShader", dev_deletevs);
    SET(K_DEVICE, "SetPixelShaderConstant", dev_setconst);
    SET(K_DEVICE, "GetPixelShaderConstant", dev_getconst);
    /* IDirect3D8 */
    SET(K_D3D8, "QueryInterface", static_qi);
    SET(K_D3D8, "AddRef", d8_addref);
    SET(K_D3D8, "Release", d8_addref);
    SET(K_D3D8, "GetAdapterCount", d8_count);
    SET(K_D3D8, "GetAdapterIdentifier", d8_identifier);
    SET(K_D3D8, "GetAdapterModeCount", d8_modecount);
    SET(K_D3D8, "EnumAdapterModes", d8_enummode);
    SET(K_D3D8, "GetAdapterDisplayMode", d8_mode);
    SET(K_D3D8, "CheckDeviceType", d8_ok);
    SET(K_D3D8, "CheckDeviceFormat", d8_ok);
    SET(K_D3D8, "CheckDeviceMultiSampleType", d8_multisample);
    SET(K_D3D8, "CheckDepthStencilMatch", d8_ok);
    SET(K_D3D8, "GetDeviceCaps", d8_caps);
    SET(K_D3D8, "GetAdapterMonitor", d8_monitor);
    SET(K_D3D8, "CreateDevice", d8_createdevice);
    /* textures */
    SET(K_TEXTURE, "QueryInterface", com_qi);
    SET(K_TEXTURE, "AddRef", tex_addref_);
    SET(K_TEXTURE, "Release", tex_release);
    SET(K_TEXTURE, "GetDevice", res_getdevice);
    SET(K_TEXTURE, "SetPrivateData", res_privdata);
    SET(K_TEXTURE, "GetPrivateData", res_privdata);
    SET(K_TEXTURE, "FreePrivateData", res_privdata);
    SET(K_TEXTURE, "SetPriority", res_priority);
    SET(K_TEXTURE, "GetPriority", res_priority);
    SET(K_TEXTURE, "PreLoad", res_preload);
    SET(K_TEXTURE, "GetType", tex_type);
    SET(K_TEXTURE, "SetLOD", tex_lod);
    SET(K_TEXTURE, "GetLOD", tex_lod);
    SET(K_TEXTURE, "GetLevelCount", tex_levelcount);
    SET(K_TEXTURE, "GetLevelDesc", tex_leveldesc);
    SET(K_TEXTURE, "GetSurfaceLevel", tex_getsurface);
    SET(K_TEXTURE, "LockRect", tex_lock);
    SET(K_TEXTURE, "UnlockRect", tex_unlock);
    SET(K_TEXTURE, "AddDirtyRect", tex_dirty);
    /* surfaces */
    SET(K_SURFACE, "QueryInterface", com_qi);
    SET(K_SURFACE, "AddRef", com_addref);
    SET(K_SURFACE, "Release", surf_release);
    SET(K_SURFACE, "GetDevice", res_getdevice);
    SET(K_SURFACE, "SetPrivateData", res_privdata);
    SET(K_SURFACE, "GetPrivateData", res_privdata);
    SET(K_SURFACE, "FreePrivateData", res_privdata);
    SET(K_SURFACE, "GetContainer", surf_container);
    SET(K_SURFACE, "GetDesc", surf_desc);
    SET(K_SURFACE, "LockRect", surf_lock);
    SET(K_SURFACE, "UnlockRect", surf_unlock);
    /* vertex and index buffers */
    for (int k = K_VERTEXBUFFER; k <= K_INDEXBUFFER; ++k)
    {
        SET(k, "QueryInterface", com_qi);
        SET(k, "AddRef", com_addref);
        SET(k, "Release", buf_release);
        SET(k, "GetDevice", res_getdevice);
        SET(k, "SetPrivateData", res_privdata);
        SET(k, "GetPrivateData", res_privdata);
        SET(k, "FreePrivateData", res_privdata);
        SET(k, "SetPriority", res_priority);
        SET(k, "GetPriority", res_priority);
        SET(k, "PreLoad", res_preload);
        SET(k, "GetType", buf_type);
        SET(k, "Lock", buf_lock);
        SET(k, "Unlock", buf_unlock);
        SET(k, "GetDesc", buf_desc);
    }
    /* sprites */
    SET(K_SPRITE, "QueryInterface", com_qi);
    SET(K_SPRITE, "AddRef", com_addref);
    SET(K_SPRITE, "Release", sprite_release);
    SET(K_SPRITE, "GetDevice", res_getdevice);
    SET(K_SPRITE, "Begin", sprite_begin);
    SET(K_SPRITE, "Draw", sprite_draw);
    SET(K_SPRITE, "DrawTransform", sprite_drawxf);
    SET(K_SPRITE, "End", sprite_end);
    SET(K_SPRITE, "OnLostDevice", sprite_ok);
    SET(K_SPRITE, "OnResetDevice", sprite_ok);
    /* fonts */
    SET(K_FONT, "QueryInterface", com_qi);
    SET(K_FONT, "AddRef", com_addref);
    SET(K_FONT, "Release", font_release);
    SET(K_FONT, "GetDevice", res_getdevice);
    SET(K_FONT, "GetLogFont", font_getlogfont);
    SET(K_FONT, "Begin", font_ok);
    SET(K_FONT, "DrawTextA", font_drawa);
    SET(K_FONT, "DrawTextW", font_draww);
    SET(K_FONT, "End", font_ok);
    SET(K_FONT, "OnLostDevice", font_ok);
    SET(K_FONT, "OnResetDevice", font_ok);
    /* the static objects */
    g_device.h.vtbl = g_vt[K_DEVICE], g_device.h.magic = MAGIC, g_device.h.kind = K_DEVICE, g_device.h.refs = 1;
    g_d3d8.vtbl = g_vt[K_D3D8], g_d3d8.magic = MAGIC, g_d3d8.kind = K_D3D8, g_d3d8.refs = 1;
    state_defaults(&g_device.s);
    g_device.frame = xi_gui_d3d_frame();
}

void* xi_d3d8_device(void)
{
    build();
    return &g_device;
}

/* --- D3DX ----------------------------------------------------------------------------------- */

static uint8_t* read_file(const char* path, size_t* n)
{
    char host[1200];
    xi_host_path(path, host, sizeof host);
    FILE* f = fopen(host, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = sz > 0 && sz < (1L << 30) ? (uint8_t*)malloc((size_t)sz) : NULL;
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = p ? (size_t)sz : 0;
    return p;
}

static char* wide_path(const wchar_t* w)
{
    size_t n;
    return w ? wide_utf8(w, -1, &n) : NULL;
}

static uint32_t pick_size(uint32_t want, uint32_t img)
{
    return want == 0 || want == D3DX_DEFAULT || want == D3DX_DEFAULT_NONPOW2 ? img : want;
}

static HR tex_from_memory(void* dev, const void* data, uint32_t n, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
    uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t key, XiImageInfo* info_out, void* palette,
    Texture** out)
{
    (void)dev, (void)filter, (void)mipfilter, (void)palette;
    build();
    if (!out || !data || !n)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    XiImageInfo info;
    uint8_t* px = xi_image_decode((const uint8_t*)data, n, &info);
    if (!px)
        return D3DXERR_INVALIDDATA;
    if (info_out)
        *info_out = info;
    uint32_t tw = pick_size(w, info.width), th = pick_size(h, info.height);
    if (fmt == FMT_UNKNOWN || fmt == D3DX_DEFAULT || !fmt_ok(fmt))
        fmt = FMT_A8R8G8B8;
    Texture* t = tex_create(tw, th, mips == D3DX_DEFAULT || mips == 0 ? 1 : mips, usage, fmt, pool);
    if (!t)
    {
        free(px);
        return D3DERR_INVALIDCALL;
    }
    Surface tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.h.magic = MAGIC, tmp.h.kind = K_SURFACE, tmp.parent = t, tmp.level = 0, tmp.format = t->format;
    HR hr = surf_write(&tmp, NULL, px, (int)info.width, (int)info.height, key);
    free(px);
    if (hr != S_OK_)
    {
        tex_release(t);
        return hr;
    }
    *out = t;
    return S_OK_;
}

XI_FFI HR D3DXCreateTextureFromFileInMemoryEx(void* dev, const void* data, uint32_t n, uint32_t w, uint32_t h, uint32_t mips,
    uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t key, XiImageInfo* info,
    void* palette, Texture** out)
{
    return tex_from_memory(dev, data, n, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, out);
}

XI_FFI HR D3DXCreateTextureFromFileInMemory(void* dev, const void* data, uint32_t n, Texture** out)
{
    return tex_from_memory(dev, data, n, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, FMT_UNKNOWN, 1, D3DX_DEFAULT,
        D3DX_DEFAULT, 0, NULL, NULL, out);
}

XI_FFI HR D3DXCreateTextureFromFileExA(void* dev, const char* path, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
    uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t key, XiImageInfo* info, void* palette,
    Texture** out)
{
    if (!path || !out)
        return D3DERR_INVALIDCALL;
    size_t n;
    uint8_t* data = read_file(path, &n);
    if (!data)
    {
        *out = NULL;
        return D3DERR_INVALIDCALL; /* D3DX: D3DERR_INVALIDCALL or D3DXERR_INVALIDDATA */
    }
    HR hr = tex_from_memory(dev, data, (uint32_t)n, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, out);
    free(data);
    return hr;
}

XI_FFI HR D3DXCreateTextureFromFileA(void* dev, const char* path, Texture** out)
{
    return D3DXCreateTextureFromFileExA(dev, path, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, FMT_UNKNOWN, 1, D3DX_DEFAULT,
        D3DX_DEFAULT, 0, NULL, NULL, out);
}

XI_FFI HR D3DXCreateTextureFromFileExW(void* dev, const wchar_t* path, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
    uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t key, XiImageInfo* info, void* palette,
    Texture** out)
{
    char* p = wide_path(path);
    HR hr = D3DXCreateTextureFromFileExA(dev, p, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, out);
    free(p);
    return hr;
}

XI_FFI HR D3DXCreateTextureFromFileW(void* dev, const wchar_t* path, Texture** out)
{
    char* p = wide_path(path);
    HR hr = D3DXCreateTextureFromFileA(dev, p, out);
    free(p);
    return hr;
}

XI_FFI HR D3DXCreateTextureFromResourceA(void* dev, void* module, const char* name, Texture** out)
{
    (void)dev, (void)module, (void)name;
    unsupported("D3DXCreateTextureFromResource");
    if (out)
        *out = NULL;
    return D3DXERR_INVALIDDATA;
}

XI_FFI HR D3DXCreateTextureFromResourceW(void* dev, void* module, const wchar_t* name, Texture** out)
{
    (void)name;
    return D3DXCreateTextureFromResourceA(dev, module, NULL, out);
}

XI_FFI HR D3DXCheckTextureRequirements(void* dev, uint32_t* w, uint32_t* h, uint32_t* mips, uint32_t usage, uint32_t* fmt, uint32_t pool)
{
    (void)dev, (void)usage, (void)pool;
    if (w && (*w == 0 || *w == D3DX_DEFAULT))
        *w = 256;
    if (h && (*h == 0 || *h == D3DX_DEFAULT))
        *h = 256;
    if (fmt && !fmt_ok(*fmt))
        *fmt = FMT_A8R8G8B8;
    if (mips)
    {
        uint32_t full = 1, m = w && h ? (*w > *h ? *w : *h) : 256;
        for (; m > 1; m >>= 1)
            ++full;
        if (*mips == 0 || *mips == D3DX_DEFAULT || *mips > full)
            *mips = full;
    }
    return S_OK_;
}

XI_FFI HR D3DXCreateTexture(void* dev, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage, uint32_t fmt, uint32_t pool,
    Texture** out)
{
    build();
    D3DXCheckTextureRequirements(dev, &w, &h, &mips, usage, &fmt, pool);
    return dev_createtexture(&g_device, w, h, mips, usage, fmt, pool, out);
}

XI_FFI HR D3DXGetImageInfoFromFileInMemory(const void* data, uint32_t n, XiImageInfo* info)
{
    if (!data || !info)
        return D3DERR_INVALIDCALL;
    uint8_t* px = xi_image_decode((const uint8_t*)data, n, info);
    if (!px)
        return D3DXERR_INVALIDDATA;
    free(px);
    return S_OK_;
}

XI_FFI HR D3DXGetImageInfoFromFileA(const char* path, XiImageInfo* info)
{
    if (!path)
        return D3DERR_INVALIDCALL;
    size_t n;
    uint8_t* data = read_file(path, &n);
    if (!data)
        return D3DERR_INVALIDCALL;
    HR hr = D3DXGetImageInfoFromFileInMemory(data, (uint32_t)n, info);
    free(data);
    return hr;
}

XI_FFI HR D3DXGetImageInfoFromFileW(const wchar_t* path, XiImageInfo* info)
{
    char* p = wide_path(path);
    HR hr = D3DXGetImageInfoFromFileA(p, info);
    free(p);
    return hr;
}

XI_FFI HR D3DXLoadSurfaceFromFileInMemory(Surface* dst, const void* dpal, const XRECT* drect, const void* data, uint32_t n,
    const XRECT* srect, uint32_t filter, uint32_t key, XiImageInfo* info_out)
{
    (void)dpal, (void)filter;
    if (!valid(dst, K_SURFACE) || !data)
        return D3DERR_INVALIDCALL;
    XiImageInfo info;
    uint8_t* px = xi_image_decode((const uint8_t*)data, n, &info);
    if (!px)
        return D3DXERR_INVALIDDATA;
    if (info_out)
        *info_out = info;
    int sw = (int)info.width, sh = (int)info.height;
    uint8_t* src = px;
    if (srect)
    {
        if (srect->left < 0 || srect->top < 0 || srect->right > sw || srect->bottom > sh || srect->left >= srect->right || srect->top >= srect->bottom)
        {
            free(px);
            return D3DERR_INVALIDCALL;
        }
        int w = srect->right - srect->left, h = srect->bottom - srect->top;
        src = (uint8_t*)malloc((size_t)w * h * 4);
        for (int y = 0; src && y < h; ++y)
            memcpy(src + (size_t)y * w * 4, px + ((size_t)(srect->top + y) * sw + (size_t)srect->left) * 4, (size_t)w * 4);
        sw = w, sh = h;
    }
    HR hr = src ? surf_write(dst, drect, src, sw, sh, key) : E_OUTOFMEMORY_;
    if (src != px)
        free(src);
    free(px);
    return hr;
}

XI_FFI HR D3DXLoadSurfaceFromFileA(Surface* dst, const void* dpal, const XRECT* drect, const char* path, const XRECT* srect,
    uint32_t filter, uint32_t key, XiImageInfo* info)
{
    if (!path)
        return D3DERR_INVALIDCALL;
    size_t n;
    uint8_t* data = read_file(path, &n);
    if (!data)
        return D3DERR_INVALIDCALL;
    HR hr = D3DXLoadSurfaceFromFileInMemory(dst, dpal, drect, data, (uint32_t)n, srect, filter, key, info);
    free(data);
    return hr;
}

XI_FFI HR D3DXLoadSurfaceFromFileW(Surface* dst, const void* dpal, const XRECT* drect, const wchar_t* path, const XRECT* srect,
    uint32_t filter, uint32_t key, XiImageInfo* info)
{
    char* p = wide_path(path);
    HR hr = D3DXLoadSurfaceFromFileA(dst, dpal, drect, p, srect, filter, key, info);
    free(p);
    return hr;
}

XI_FFI HR D3DXLoadSurfaceFromMemory(Surface* dst, const void* dpal, const XRECT* drect, const void* src, uint32_t fmt,
    uint32_t pitch, const void* spal, const XRECT* srect, uint32_t filter, uint32_t key)
{
    (void)dpal, (void)spal, (void)filter;
    if (!valid(dst, K_SURFACE) || !src || !srect || !xi_fmt_bpp(fmt))
        return D3DERR_INVALIDCALL;
    int w = srect->right - srect->left, h = srect->bottom - srect->top;
    if (w <= 0 || h <= 0)
        return D3DERR_INVALIDCALL;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    if (!px)
        return E_OUTOFMEMORY_;
    int dxt = xi_fmt_rows(fmt, 4) == 1;
    const uint8_t* at = (const uint8_t*)src + (dxt ? (size_t)(srect->top / 4) * pitch + (size_t)(srect->left / 4) * (uint32_t)xi_fmt_bpp(fmt) * 2
                                                  : (size_t)srect->top * pitch + (size_t)srect->left * (uint32_t)xi_fmt_bpp(fmt) / 8);
    xi_fmt_to_bgra(fmt, at, (int)pitch, w, h, px, w * 4);
    HR hr = surf_write(dst, drect, px, w, h, key);
    free(px);
    return hr;
}

XI_FFI HR D3DXLoadSurfaceFromSurface(Surface* dst, const void* dpal, const XRECT* drect, Surface* src, const void* spal,
    const XRECT* srect, uint32_t filter, uint32_t key)
{
    (void)dpal, (void)spal, (void)filter;
    if (!valid(dst, K_SURFACE) || !valid(src, K_SURFACE))
        return D3DERR_INVALIDCALL;
    int w, h;
    uint8_t* px = surf_read(src, srect, &w, &h);
    if (!px)
        return D3DERR_INVALIDCALL;
    HR hr = surf_write(dst, drect, px, w, h, key);
    free(px);
    return hr;
}

XI_FFI HR D3DXFilterTexture(void* tex, const void* pal, uint32_t level, uint32_t filter)
{
    (void)tex, (void)pal, (void)level, (void)filter;
    return S_OK_; /* only level 0 is ever drawn */
}

XI_FFI HR D3DXCreateSprite(void* dev, Sprite** out)
{
    (void)dev;
    build();
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = (Sprite*)obj_alloc(sizeof(Sprite), K_SPRITE, NULL);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

XI_FFI HR D3DXCreateFontIndirect(void* dev, const XLOGFONTA* lf, Font** out)
{
    (void)dev;
    build();
    if (!out || !lf)
        return D3DERR_INVALIDCALL;
    *out = font_from_logfont(lf);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

XI_FFI HR D3DXCreateFont(void* dev, void* hfont, Font** out)
{
    (void)hfont;
    XLOGFONTA lf;
    memset(&lf, 0, sizeof lf);
    lf.height = -16, lf.weight = 400;
    snprintf(lf.face, sizeof lf.face, "Arial");
    unsupported("D3DXCreateFont from an HFONT (Arial 16 stands in)");
    return D3DXCreateFontIndirect(dev, &lf, out);
}

XI_FFI HR D3DXGetErrorStringA(HR hr, char* buf, uint32_t n)
{
    if (!buf || !n)
        return D3DERR_INVALIDCALL;
    const char* s = hr == S_OK_ ? "S_OK" : hr == D3DERR_INVALIDCALL ? "D3DERR_INVALIDCALL" : hr == D3DXERR_INVALIDDATA ? "D3DXERR_INVALIDDATA"
        : hr == E_NOTIMPL_ ? "E_NOTIMPL" : hr == E_OUTOFMEMORY_ ? "E_OUTOFMEMORY" : NULL;
    if (s)
        snprintf(buf, n, "%s", s);
    else
        snprintf(buf, n, "Unknown error 0x%08X", (unsigned)hr);
    return S_OK_;
}

XI_FFI HR D3DXGetErrorStringW(HR hr, wchar_t* buf, uint32_t n)
{
    char tmp[64];
    D3DXGetErrorStringA(hr, tmp, sizeof tmp);
    if (!buf || !n)
        return D3DERR_INVALIDCALL;
    uint32_t i = 0;
    for (; tmp[i] && i + 1 < n; ++i)
        buf[i] = (wchar_t)tmp[i];
    buf[i] = 0;
    return S_OK_;
}

/* shaders, effects, saving: not supported */
#define NOT_SUPPORTED(name) \
    XI_FFI HR name(void) { unsupported(#name); return E_NOTIMPL_; }
NOT_SUPPORTED(D3DXAssembleShader)
NOT_SUPPORTED(D3DXAssembleShaderFromFileA)
NOT_SUPPORTED(D3DXAssembleShaderFromFileW)
NOT_SUPPORTED(D3DXSaveSurfaceToFileA)
NOT_SUPPORTED(D3DXSaveSurfaceToFileW)
NOT_SUPPORTED(D3DXSaveTextureToFileA)
NOT_SUPPORTED(D3DXSaveTextureToFileW)
NOT_SUPPORTED(D3DXCreateCubeTexture)
NOT_SUPPORTED(D3DXCreateVolumeTexture)
NOT_SUPPORTED(D3DXCreateCubeTextureFromFileA)
NOT_SUPPORTED(D3DXCreateCubeTextureFromFileW)
NOT_SUPPORTED(D3DXCreateCubeTextureFromFileInMemory)
NOT_SUPPORTED(D3DXCreateVolumeTextureFromFileA)
NOT_SUPPORTED(D3DXCreateVolumeTextureFromFileW)
#undef NOT_SUPPORTED

void* xi_d3d_texture_bgra(const uint8_t* bgra, int w, int h)
{
    build();
    Texture* t = tex_create((uint32_t)w, (uint32_t)h, 1, 0, FMT_A8R8G8B8, 1 /* MANAGED */);
    if (!t)
        return NULL;
    for (int y = 0; y < h; ++y)
        memcpy(t->lv[0].data + (size_t)y * t->lv[0].pitch, bgra + (size_t)y * w * 4, (size_t)w * 4);
    tex_upload(t);
    return t;
}

/* --- Lua ------------------------------------------------------------------------------------ */

static int l_device(lua_State* L)
{
    lua_pushnumber(L, (double)(uintptr_t)xi_d3d8_device());
    return 1;
}

/* xi.d3d8_stats(): the overlay's Direct3D list this frame (commands, vertices) - tests. */
static int l_stats(lua_State* L)
{
    uint32_t c, v;
    xi_gui_d3d_stats(&c, &v);
    lua_pushinteger(L, (lua_Integer)c);
    lua_pushinteger(L, (lua_Integer)v);
    return 2;
}

/* xi.d3d8_texture_id(ptr or number): the overlay texture an IDirect3DTexture8 is (0: none). */
static int l_texture_id(lua_State* L)
{
    /* a pointer cdata: lua_topointer gives where the pointer is held */
    const void* p = lua_type(L, 1) == 10 ? *(void* const*)lua_topointer(L, 1) : (const void*)(uintptr_t)luaL_optnumber(L, 1, 0);
    lua_pushinteger(L, valid(p, K_TEXTURE) ? (lua_Integer)((const Texture*)p)->id : 0);
    return 1;
}

void xi_d3d_ffi_open(lua_State* L)
{
    build();
    lua_pushcfunction(L, l_device);
    lua_setfield(L, -2, "d3d8_device");
    lua_pushcfunction(L, l_stats);
    lua_setfield(L, -2, "d3d8_stats");
    lua_pushcfunction(L, l_texture_id);
    lua_setfield(L, -2, "d3d8_texture_id");
}
