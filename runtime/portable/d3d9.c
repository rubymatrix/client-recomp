/* Direct3D 9 for 64-bit hosts: the interface FINAL FANTASY XIV 1.23b draws through, implemented on
 * the Metal back end (gfx9.h).
 *
 * The COM objects live in guest memory (a vtable pointer and our object index); every method is
 * a thunk named "d3d9.dll!IDirect3DDevice9::SetRenderState" and so on, in the D3D9 ABI's vtable
 * order, so a method the game reaches that is not written yet traps with its name.
 *
 * Which copy of a resource is current:
 *   - textures and surfaces the game fills: guest memory; each UnlockRect uploads what it covered
 *     (the upload goes into the frame's command stream in call order);
 *   - render targets and depth: the GPU; a lock reads the level back, and an unlock that may have
 *     written uploads it again;
 *   - vertex and index buffers: guest memory. A static buffer (no D3DUSAGE_DYNAMIC) also has a GPU
 *     copy, updated at each unlock; a dynamic one is copied per draw, just the range drawn.
 *
 * Device state keeps D3D9's defaults and readback (Get* returns what Set* stored). State blocks
 * record and apply what the game set while recording, or capture the whole device.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx9.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define D3D_OK 0u
#define S_FALSE 1u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_NOTFOUND 0x88760866u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_OUTOFMEMORY 0x8007000Eu

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_D24S8 75u
#define FMT_INDEX16 101u
#define FMT_INDEX32 102u

#define USAGE_RENDERTARGET 0x1u
#define USAGE_DEPTHSTENCIL 0x2u
#define USAGE_DYNAMIC 0x200u
#define USAGE_AUTOGENMIPMAP 0x400u
#define POOL_DEFAULT 0u
#define POOL_MANAGED 1u
#define POOL_SYSTEMMEM 2u

#define LOCK_READONLY 0x10u
#define LOCK_DISCARD 0x2000u

enum
{
    O_FREE, O_D3D, O_DEVICE, O_TEXTURE, O_CUBE, O_VOLTEX, O_SURFACE, O_VOLUME, O_VB, O_IB, O_DECL, O_VS, O_PS,
    O_QUERY, O_SB, O_SWAP, O_KINDS
};

/* D3DRESOURCETYPE */
enum { RTYPE_SURFACE = 1, RTYPE_VOLUME, RTYPE_TEXTURE, RTYPE_VOLUMETEXTURE, RTYPE_CUBETEXTURE, RTYPE_VB, RTYPE_IB };

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* --- formats ----------------------------------------------------------------------------------- */

static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block, or 0 */
{
    if (f == FOURCC('D', 'X', 'T', '1') || f == FOURCC('A', 'T', 'I', '1'))
        return 8;
    if (f == FOURCC('D', 'X', 'T', '2') || f == FOURCC('D', 'X', 'T', '3') || f == FOURCC('D', 'X', 'T', '4') ||
        f == FOURCC('D', 'X', 'T', '5') || f == FOURCC('A', 'T', 'I', '2'))
        return 16;
    return 0;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel, uncompressed */
{
    switch (f)
    {
    case 28: case 50: case 41: case 52: return 1;             /* A8 L8 P8 A4L4 */
    case 23: case 24: case 25: case 26: case 30: case 51: case 60: case 81: case 111: case 70: case 80: case 73:
        return 2;                                                /* 16-bit */
    case 36: case 113: case 115: case 64 + 46: return 8;         /* A16B16G16R16, A16F, G32R32F */
    case 116: return 16;                                         /* A32B32G32R32F */
    default: return 4;
    }
}

static uint32_t fmt_pitch(uint32_t f, uint32_t w)
{
    uint32_t b = fmt_block(f);
    return b ? ((w + 3) / 4) * b : w * fmt_bytes(f);
}

static uint32_t fmt_rows(uint32_t f, uint32_t h) { return fmt_block(f) ? (h + 3) / 4 : h; }

static uint32_t lvl(uint32_t s, uint32_t level) { return s >> level ? s >> level : 1; }

/* --- objects ----------------------------------------------------------------------------------- */

typedef struct Elem9
{
    uint16_t stream, offset;
    uint8_t type, method, usage, index;
} Elem9;

typedef struct Obj
{
    uint8_t kind, implicit;
    int32_t refs;
    uint32_t guest;
    uint32_t container;         /* surfaces/volumes of a texture: its COM pointer */
    uint32_t format, usage, pool, width, height, depth, levels, size, fvf, type;
    uint32_t mem;               /* guest memory (all levels and faces), on first lock */
    uint32_t* subs;             /* textures: their surfaces (face * levels + level) */
    uint32_t nsubs;
    uint32_t face, level;       /* a texture's surface: where in it */
    uint32_t lod, autogen;
    G9Tex* gpu;
    G9Buf* gbuf;
    /* the current lock, for the upload at unlock */
    uint32_t lock_flags, lock_level, lock_face;
    int32_t lock_rect[6];       /* left top right bottom (front back) */
    uint32_t lock_off, lock_size;
    int locked;
    /* declarations, shaders */
    Elem9 el[G9_MAX_ELEMS];
    uint32_t nel;
    G9Shader* shader;
    uint32_t tokens, ntokens;   /* guest copy of the shader code (GetFunction) */
    /* queries */
    uint32_t qtype, issued;
    /* state blocks */
    struct State* sb;
    struct Mask* sbm;
} Obj;

/* Objects live in chunks that never move: the per-draw methods run without the guest lock
 * (thunk.c kNoLock) on the render thread while another thread may be creating a resource. */
#define OBJ_CHUNK 1024u
#define OBJ_CHUNKS 4096u
static Obj* g_obj_chunks[OBJ_CHUNKS];
static volatile uint32_t g_nobjs;
static uint32_t g_free_hint = 1;
static uint32_t g_vtbl[O_KINDS];
#define OBJ_AT(i) (&g_obj_chunks[(i) / OBJ_CHUNK][(i) % OBJ_CHUNK])

static uint32_t obj_new(int kind)
{
    uint32_t i = g_free_hint;
    while (i < g_nobjs && OBJ_AT(i)->kind != O_FREE)
        i++;
    if (i >= g_nobjs)
    {
        uint32_t n = g_nobjs ? g_nobjs : 1;
        if (n / OBJ_CHUNK >= OBJ_CHUNKS)
            return 0;
        if (!g_obj_chunks[n / OBJ_CHUNK])
            g_obj_chunks[n / OBJ_CHUNK] = (Obj*)calloc(OBJ_CHUNK, sizeof(Obj));
        if (!g_obj_chunks[0])
            g_obj_chunks[0] = (Obj*)calloc(OBJ_CHUNK, sizeof(Obj));
        i = n;
        __atomic_store_n(&g_nobjs, n + 1, __ATOMIC_RELEASE);
    }
    g_free_hint = i + 1;
    Obj* o = OBJ_AT(i);
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->guest = gheap_alloc(8, 1);
    wr32(o->guest, g_vtbl[kind]);
    wr32(o->guest + 4, i);
    return o->guest;
}

static Obj* obj(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    if (!i || i >= __atomic_load_n(&g_nobjs, __ATOMIC_ACQUIRE))
        return NULL;
    Obj* o = OBJ_AT(i);
    return o->kind != O_FREE && o->guest == p ? o : NULL;
}

static uint32_t obj_release(uint32_t p);

static void obj_destroy(Obj* o)
{
    for (uint32_t i = 0; i < o->nsubs; ++i)
    {
        Obj* s = obj(o->subs[i]);
        if (s)
        {
            s->container = 0;
            s->refs = 1;
            s->gpu = NULL; /* the texture's, freed below */
            obj_release(s->guest);
        }
    }
    free(o->subs);
    if (!o->container)
        g9_tex_destroy(o->gpu);
    g9_buf_destroy(o->gbuf);
    g9_shader_destroy(o->shader);
    free(o->sb);
    free(o->sbm);
    if (o->mem)
        gheap_free(o->mem);
    if (o->tokens)
        gheap_free(o->tokens);
    uint32_t idx = rd32(o->guest + 4);
    wr32(o->guest, 0);
    gheap_free(o->guest);
    o->kind = O_FREE;
    if (idx < g_free_hint)
        g_free_hint = idx;
}

static uint32_t obj_addref(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_addref(o->container);
    return (uint32_t)++o->refs;
}

static uint32_t obj_release(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_release(o->container);
    if (o->refs > 0)
        o->refs--;
    if (!o->refs && !o->implicit && o->kind != O_DEVICE && o->kind != O_D3D)
    {
        obj_destroy(o);
        return 0;
    }
    return (uint32_t)o->refs;
}

static void bind(uint32_t* slot, uint32_t p)
{
    if (*slot == p)
        return;
    if (p)
        obj_addref(p);
    uint32_t old = *slot;
    *slot = p;
    if (old)
        obj_release(old);
}

static uint32_t obj_mem(Obj* o)
{
    if (!o->mem)
        o->mem = gheap_alloc(o->size ? o->size : 16, 1);
    return o->mem;
}

/* --- the device state -------------------------------------------------------------------------- */

#define NRS 256
#define NSAMP G9_SAMPLERS
#define NSS 14
#define NTSS 33
#define NTS 512 /* transforms: D3DTS_VIEW 2, PROJECTION 3, TEXTURE0-7 16-23, WORLD 256-511 */

typedef struct Stream9
{
    uint32_t vb, offset, stride, freq;
} Stream9;

typedef struct State
{
    uint32_t rs[NRS];
    uint32_t ss[NSAMP][NSS];
    uint32_t tss[8][NTSS];
    uint32_t tex[NSAMP];
    Stream9 stream[G9_MAX_STREAMS];
    uint32_t indices, decl, fvf, vs, ps;
    float vsc[G9_VS_FLOATS][4];
    int32_t vsi[16][4];
    uint32_t vsb[16];
    float psc[G9_PS_FLOATS][4];
    int32_t psi[16][4];
    uint32_t psb[16];
    uint32_t vp[6];      /* D3DVIEWPORT9 */
    int32_t scissor[4];  /* RECT */
    float ts[NTS][16];
    float material[17];
    float clip[6][4];
} State;

/* which parts of State a recorded state block holds */
typedef struct Mask
{
    uint8_t rs[NRS], ss[NSAMP][NSS], tss[8][NTSS], tex[NSAMP], stream[G9_MAX_STREAMS];
    uint8_t indices, decl, fvf, vs, ps, vp, scissor, material;
    uint8_t vsc[G9_VS_FLOATS], vsi[16], vsb[16], psc[G9_PS_FLOATS], psi[16], psb[16];
    uint8_t ts[NTS], clip[6];
} Mask;

typedef struct Dev
{
    uint32_t guest, d3d, hwnd, behavior;
    uint32_t pp[14];      /* D3DPRESENT_PARAMETERS */
    uint32_t backbuffer, autodepth, swap;
    uint32_t rt[G9_MAX_RT], ds;
    State st;
    uint32_t recording;   /* a state block being recorded: Set* goes to it */
    uint32_t in_scene;
    uint64_t frames;
} Dev;

static Dev g_dev;

static void state_defaults(State* s, uint32_t w, uint32_t h, int zbuffer)
{
    memset(s, 0, sizeof *s);
    uint32_t* rs = s->rs;
    rs[7] = zbuffer ? 1 : 0;      /* ZENABLE */
    rs[8] = 3;                    /* FILLMODE SOLID */
    rs[9] = 2;                    /* SHADEMODE GOURAUD */
    rs[14] = 1;                   /* ZWRITEENABLE */
    rs[16] = 1;                   /* LASTPIXEL */
    rs[19] = 2;                   /* SRCBLEND ONE */
    rs[20] = 1;                   /* DESTBLEND ZERO */
    rs[22] = 3;                   /* CULLMODE CCW */
    rs[23] = 4;                   /* ZFUNC LESSEQUAL */
    rs[25] = 8;                   /* ALPHAFUNC ALWAYS */
    rs[37] = f2u(1.0f);           /* FOGEND */
    rs[38] = f2u(1.0f);           /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1; /* STENCIL ops KEEP */
    rs[56] = 8;                   /* STENCILFUNC ALWAYS */
    rs[58] = rs[59] = 0xFFFFFFFFu;
    rs[60] = 0xFFFFFFFFu;         /* TEXTUREFACTOR */
    rs[136] = 1;                  /* CLIPPING */
    rs[137] = 1;                  /* LIGHTING */
    rs[141] = 1;                  /* COLORVERTEX */
    rs[142] = 1;                  /* LOCALVIEWER */
    rs[145] = 1;                  /* DIFFUSEMATERIALSOURCE COLOR1 */
    rs[146] = 2;                  /* SPECULARMATERIALSOURCE COLOR2 */
    rs[154] = f2u(1.0f);          /* POINTSIZE */
    rs[155] = f2u(1.0f);
    rs[158] = f2u(1.0f);          /* POINTSCALE_A */
    rs[161] = 1;                  /* MULTISAMPLEANTIALIAS */
    rs[162] = 0xFFFFFFFFu;
    rs[166] = f2u(64.0f);         /* POINTSIZE_MAX */
    rs[168] = 0xF;                /* COLORWRITEENABLE */
    rs[171] = 1;                  /* BLENDOP ADD */
    rs[178] = rs[179] = f2u(1.0f);
    rs[182] = f2u(1.0f);          /* ADAPTIVETESS_Z */
    rs[186] = rs[187] = rs[188] = 1;
    rs[189] = 8;                  /* CCW_STENCILFUNC ALWAYS */
    rs[190] = rs[191] = rs[192] = 0xF;
    rs[193] = 0xFFFFFFFFu;        /* BLENDFACTOR */
    rs[207] = 2;                  /* SRCBLENDALPHA ONE */
    rs[208] = 1;                  /* DESTBLENDALPHA ZERO */
    rs[209] = 1;                  /* BLENDOPALPHA ADD */
    for (int i = 0; i < NSAMP; ++i)
    {
        s->ss[i][1] = s->ss[i][2] = s->ss[i][3] = 1; /* ADDRESS WRAP */
        s->ss[i][5] = s->ss[i][6] = 1;               /* MAG/MIN POINT */
        s->ss[i][10] = 1;                            /* MAXANISOTROPY */
    }
    for (int i = 0; i < 8; ++i)
    {
        s->tss[i][1] = i ? 1 : 4; /* COLOROP: DISABLE, stage 0 MODULATE */
        s->tss[i][2] = 2;         /* COLORARG1 TEXTURE */
        s->tss[i][3] = 1;         /* COLORARG2 CURRENT */
        s->tss[i][4] = i ? 1 : 2; /* ALPHAOP: DISABLE, stage 0 SELECTARG1 */
        s->tss[i][5] = 2;
        s->tss[i][6] = 1;
        s->tss[i][11] = (uint32_t)i; /* TEXCOORDINDEX */
    }
    for (int i = 0; i < G9_MAX_STREAMS; ++i)
        s->stream[i].freq = 1;
    s->vp[2] = w, s->vp[3] = h, s->vp[5] = f2u(1.0f);
    s->scissor[2] = (int32_t)w, s->scissor[3] = (int32_t)h;
    for (int t = 0; t < NTS; ++t)
        for (int k = 0; k < 4; ++k)
            s->ts[t][5 * k] = 1.0f;
}

/* the state Set* writes: the device's, or the state block being recorded */
static State* wstate(Mask** m)
{
    Obj* sb = g_dev.recording ? obj(g_dev.recording) : NULL;
    if (sb)
    {
        *m = sb->sbm;
        return sb->sb;
    }
    *m = NULL;
    return &g_dev.st;
}

/* --- COM basics -------------------------------------------------------------------------------- */

static void Unknown_QueryInterface(Guest* g)
{
    if (ARG(2))
        wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}
static void Unknown_AddRef(Guest* g) { RET(obj_addref(ARG(0)), 1); }
static void Unknown_Release(Guest* g) { RET(obj_release(ARG(0)), 1); }
static void Any_GetDevice(Guest* g)
{
    obj_addref(g_dev.guest);
    wr32(ARG(1), g_dev.guest);
    RET(D3D_OK, 2);
}
static void Any_SetPrivateData(Guest* g) { RET(D3D_OK, 5); }
static void Any_GetPrivateData(Guest* g) { RET(D3DERR_NOTFOUND, 4); }
static void Any_FreePrivateData(Guest* g) { RET(D3D_OK, 2); }
static void Resource_SetPriority(Guest* g) { RET(0, 2); }
static void Resource_GetPriority(Guest* g) { RET(0, 1); }
static void Resource_PreLoad(Guest* g) { RET(0, 1); }
static void Resource_GetType(Guest* g)
{
    Obj* o = obj(ARG(0));
    static const uint32_t types[O_KINDS] = { [O_SURFACE] = RTYPE_SURFACE, [O_VOLUME] = RTYPE_VOLUME,
                                             [O_TEXTURE] = RTYPE_TEXTURE, [O_VOLTEX] = RTYPE_VOLUMETEXTURE,
                                             [O_CUBE] = RTYPE_CUBETEXTURE, [O_VB] = RTYPE_VB, [O_IB] = RTYPE_IB };
    RET(o ? types[o->kind] : 0, 1);
}

/* --- IDirect3D9 -------------------------------------------------------------------------------- */

/* D3DCAPS9 (304 bytes): an SM3 NVIDIA part of the game's era (GeForce GTX 280 class) - what the
 * client checks before choosing its renderer paths. Everything here is within Metal's reach. */
static void write_caps(uint32_t p)
{
    uint8_t c[304];
    memset(c, 0, sizeof c);
#define W(off, v) do { uint32_t v_ = (uint32_t)(v); memcpy(c + (off), &v_, 4); } while (0)
#define WF(off, v) do { float v_ = (float)(v); memcpy(c + (off), &v_, 4); } while (0)
    W(0, 1);                 /* DeviceType HAL */
    W(4, 0);                 /* AdapterOrdinal */
    W(8, 0x00020000);        /* Caps: READ_SCANLINE */
    W(12, 0xE0020000);       /* Caps2: FULLSCREENGAMMA, CANAUTOGENMIPMAP, DYNAMICTEXTURES, CANMANAGERESOURCE */
    W(16, 0x000003A0);       /* Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, LINEAR_TO_SRGB, COPY_TO_VIDMEM/SYSTEMMEM */
    W(20, 0x8000000F);       /* PresentationIntervals: IMMEDIATE, ONE..THREE */
    W(24, 0x00000001);       /* CursorCaps */
    W(28, 0x001BBEF0);       /* DevCaps: HW T&L, pure, draw primitives 2, ... */
    W(32, 0x003F2FF2);       /* PrimitiveMiscCaps: masks, cull modes, blend op, MRT independent masks, sep. alpha */
    W(36, 0x0F736191);       /* RasterCaps: dither, zbias, scissor, slope-scale depth bias, depth bias, aniso */
    W(40, 0xFF);             /* ZCmpCaps: all */
    W(44, 0x3FFF);           /* SrcBlendCaps */
    W(48, 0x3FFF);           /* DestBlendCaps */
    W(52, 0xFF);             /* AlphaCmpCaps */
    W(56, 0x00084208);       /* ShadeCaps */
    W(60, 0x0001EC45);       /* TextureCaps: perspective, alpha, cubemap, mipmap, volume, mipcube, pow2 not required */
    W(64, 0x03030700);       /* TextureFilterCaps */
    W(68, 0x03030300);       /* CubeTextureFilterCaps */
    W(72, 0x03030300);       /* VolumeTextureFilterCaps */
    W(76, 0x3F);             /* TextureAddressCaps */
    W(80, 0x3F);             /* VolumeTextureAddressCaps */
    W(84, 0x1F);             /* LineCaps */
    W(88, 8192);             /* MaxTextureWidth */
    W(92, 8192);             /* MaxTextureHeight */
    W(96, 2048);             /* MaxVolumeExtent */
    W(100, 8192);            /* MaxTextureRepeat */
    W(104, 8192);            /* MaxTextureAspectRatio */
    W(108, 16);              /* MaxAnisotropy */
    WF(112, 1e10f);          /* MaxVertexW */
    WF(116, -1e8f); WF(120, -1e8f); WF(124, 1e8f); WF(128, 1e8f); /* guard band */
    WF(132, 0.0f);           /* ExtentsAdjust */
    W(136, 0x1FF);           /* StencilCaps: all ops, two-sided */
    W(140, 0x00180008);      /* FVFCaps: 8 texcoords, PSIZE */
    W(144, 0x03FEFFFF);      /* TextureOpCaps */
    W(148, 8);               /* MaxTextureBlendStages */
    W(152, 8);               /* MaxSimultaneousTextures */
    W(156, 0x0000017B);      /* VertexProcessingCaps */
    W(160, 8);               /* MaxActiveLights */
    W(164, 8);               /* MaxUserClipPlanes */
    W(168, 4);               /* MaxVertexBlendMatrices */
    W(172, 0);               /* MaxVertexBlendMatrixIndex */
    WF(176, 8192.0f);        /* MaxPointSize */
    W(180, 0x00FFFFFF);      /* MaxPrimitiveCount */
    W(184, 0x00FFFFFF);      /* MaxVertexIndex */
    W(188, 16);              /* MaxStreams */
    W(192, 255);             /* MaxStreamStride */
    W(196, 0xFFFE0300);      /* VertexShaderVersion 3.0 */
    W(200, 256);             /* MaxVertexShaderConst */
    W(204, 0xFFFF0300);      /* PixelShaderVersion 3.0 */
    WF(208, 65504.0f);       /* PixelShader1xMaxValue */
    W(212, 0x00000051);      /* DevCaps2: STREAMOFFSET, VERTEXELEMENTSCANSHARESTREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES */
    WF(216, 0.0f);
    W(224, 0); W(228, 0); W(232, 1);
    W(236, 0x0000030F);      /* DeclTypes: UBYTE4N, SHORT2N, SHORT4N, USHORT2N, USHORT4N, FLOAT16_2, FLOAT16_4 */
    W(240, 4);               /* NumSimultaneousRTs */
    W(244, 0x03000300);      /* StretchRectFilterCaps: min/mag point and linear */
    W(248, 1); W(252, 24); W(256, 32); W(260, 4);           /* VS20Caps */
    W(264, 0x1F); W(268, 24); W(272, 32); W(276, 4); W(280, 512); /* PS20Caps */
    W(284, 0x03000300);      /* VertexTextureFilterCaps */
    W(288, 65535);           /* MaxVShaderInstructionsExecuted */
    W(292, 65535);           /* MaxPShaderInstructionsExecuted */
    W(296, 4096);            /* MaxVertexShader30InstructionSlots */
    W(300, 4096);            /* MaxPixelShader30InstructionSlots */
#undef W
#undef WF
    memcpy(GUEST_PTR(p), c, sizeof c);
}

static void IDirect3D9_RegisterSoftwareDevice(Guest* g) { RET(D3DERR_INVALIDCALL, 2); }
static void IDirect3D9_GetAdapterCount(Guest* g) { RET(1, 1); }

static void IDirect3D9_GetAdapterIdentifier(Guest* g)
{
    uint32_t p = ARG(3);
    memset(GUEST_PTR(p), 0, 1100);
    strcpy((char*)GUEST_PTR(p), "nvd3dum.dll");
    strcpy((char*)GUEST_PTR(p + 512), "NVIDIA GeForce GTX 280");
    strcpy((char*)GUEST_PTR(p + 1024), "\\\\.\\DISPLAY1");
    wr32(p + 1056, 0x000F0000u | 0x1AB4); /* DriverVersion low: 6.14.11.6836 */
    wr32(p + 1060, 0x0006000Eu);
    wr32(p + 1064, 0x10DE);   /* VendorId: NVIDIA */
    wr32(p + 1068, 0x05E1);   /* DeviceId: GTX 280 */
    wr32(p + 1072, 0);
    wr32(p + 1076, 0xA1);
    wr32(p + 1096, 1);        /* WHQLLevel */
    RET(D3D_OK, 4);
}

static uint32_t mode_list(uint32_t (*out)[3], uint32_t max)
{
    uint32_t w, h, hz;
    user32_desktop_mode(&w, &h, &hz);
    static const uint32_t common[][2] = { { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 },
                                          { 2560, 1440 } };
    uint32_t n = 0;
    for (uint32_t i = 0; i < sizeof common / sizeof common[0] && n < max; ++i)
        if (common[i][0] <= w && common[i][1] <= h)
            out[n][0] = common[i][0], out[n][1] = common[i][1], out[n][2] = hz ? hz : 60, n++;
    if (n < max)
        out[n][0] = w, out[n][1] = h, out[n][2] = hz ? hz : 60, n++;
    return n;
}

static void IDirect3D9_GetAdapterModeCount(Guest* g)
{
    uint32_t m[16][3];
    RET(mode_list(m, 16), 3);
}

static void IDirect3D9_EnumAdapterModes(Guest* g)
{
    uint32_t m[16][3], n = mode_list(m, 16), i = ARG(3), p = ARG(4);
    if (i >= n)
        RET(D3DERR_INVALIDCALL, 5);
    wr32(p, m[i][0]), wr32(p + 4, m[i][1]), wr32(p + 8, m[i][2]), wr32(p + 12, FMT_X8R8G8B8);
    RET(D3D_OK, 5);
}

static void IDirect3D9_GetAdapterDisplayMode(Guest* g)
{
    uint32_t w, h, hz, p = ARG(2);
    user32_desktop_mode(&w, &h, &hz);
    wr32(p, w), wr32(p + 4, h), wr32(p + 8, hz ? hz : 60), wr32(p + 12, FMT_X8R8G8B8);
    RET(D3D_OK, 3);
}

static void IDirect3D9_CheckDeviceType(Guest* g) { RET(D3D_OK, 6); }

/* CheckDeviceFormat(adapter, type, adapter fmt, usage, rtype, fmt) */
static void IDirect3D9_CheckDeviceFormat(Guest* g)
{
    uint32_t usage = ARG(4), rtype = ARG(5), f = ARG(6);
    int use = (usage & USAGE_DEPTHSTENCIL) ? G9_USE_DEPTH : (usage & USAGE_RENDERTARGET) ? G9_USE_RT : G9_USE_SAMPLE;
    int ok = g9_format_ok(f, use);
    if (rtype == RTYPE_VB || rtype == RTYPE_IB)
        ok = 1;
    if (ok && (usage & USAGE_AUTOGENMIPMAP) && fmt_block(f))
        RET(0x0876087Fu, 7); /* D3DOK_NOAUTOGEN */
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void IDirect3D9_CheckDeviceMultiSampleType(Guest* g)
{
    if (ARG(6))
        wr32(ARG(6), 1);
    RET(ARG(5) <= 1 ? D3D_OK : D3DERR_NOTAVAILABLE, 7); /* no MSAA yet */
}
static void IDirect3D9_CheckDepthStencilMatch(Guest* g) { RET(D3D_OK, 6); }
static void IDirect3D9_CheckDeviceFormatConversion(Guest* g) { RET(D3D_OK, 5); }
static void IDirect3D9_GetDeviceCaps(Guest* g) { write_caps(ARG(3)); RET(D3D_OK, 4); }
static void IDirect3D9_GetAdapterMonitor(Guest* g) { RET(0x00010001u, 2); }

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, uint32_t container);

static void make_targets(Dev* d)
{
    uint32_t w = d->pp[0], h = d->pp[1];
    uint32_t bb = new_surface(d->pp[2] ? d->pp[2] : FMT_X8R8G8B8, w, h, USAGE_RENDERTARGET, POOL_DEFAULT, 0);
    obj(bb)->implicit = 1;
    d->backbuffer = bb;
    d->rt[0] = bb;
    obj_addref(bb);
    if (d->pp[9]) /* EnableAutoDepthStencil */
    {
        uint32_t ds = new_surface(d->pp[10] ? d->pp[10] : FMT_D24S8, w, h, USAGE_DEPTHSTENCIL, POOL_DEFAULT, 0);
        obj(ds)->implicit = 1;
        d->autodepth = ds;
        d->ds = ds;
        obj_addref(ds);
    }
}

/* CreateDevice(adapter, type, focus window, behavior, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**) */
static void IDirect3D9_CreateDevice(Guest* g)
{
    Dev* d = &g_dev;
    if (d->guest)
    {
        rt_log("[d3d9] a second device: not supported\n");
        RET(D3DERR_NOTAVAILABLE, 7);
    }
    uint32_t pp = ARG(5);
    for (int i = 0; i < 14; ++i)
        d->pp[i] = rd32(pp + 4u * i);
    d->d3d = ARG(0);
    d->hwnd = d->pp[7] ? d->pp[7] : ARG(3);
    d->behavior = ARG(4);
    if (!d->pp[0] || !d->pp[1])
        user32_client_size(d->hwnd, &d->pp[0], &d->pp[1]);
    if (!d->pp[2] || d->pp[2] == 0)
        d->pp[2] = FMT_X8R8G8B8;
    wr32(pp, d->pp[0]);
    wr32(pp + 4, d->pp[1]);
    wr32(pp + 8, d->pp[2]);
    user32_set_fullscreen(d->hwnd, !d->pp[8]);
    /* uncapped: the display's sync only when asked for (XIV_VSYNC=1), whatever interval the game set */
    const char* vs = getenv("XIV_VSYNC");
    if (!g9_init(user32_sdl_window(d->hwnd), vs && strcmp(vs, "0") && d->pp[13] != 0x80000000u))
    {
        rt_log("[d3d9] the Metal back end did not start\n");
        RET(D3DERR_NOTAVAILABLE, 7);
    }
    g9_resize(d->pp[0], d->pp[1]);
    d->guest = obj_new(O_DEVICE);
    obj_addref(d->d3d);
    state_defaults(&d->st, d->pp[0], d->pp[1], d->pp[9] != 0);
    make_targets(d);
    rt_log("[d3d9] device %ux%u, back buffer format %u, %s, depth %s %u\n", d->pp[0], d->pp[1], d->pp[2],
        d->pp[8] ? "windowed" : "full screen", d->pp[9] ? "auto" : "none", d->pp[10]);
    wr32(ARG(6), d->guest);
    RET(D3D_OK, 7);
}

/* --- device: basics ---------------------------------------------------------------------------- */

static void Dev_TestCooperativeLevel(Guest* g) { RET(D3D_OK, 1); }
static void Dev_GetAvailableTextureMem(Guest* g) { RET(0x7FF00000u, 1); }
static void Dev_EvictManagedResources(Guest* g) { RET(D3D_OK, 1); }
static void Dev_GetDirect3D(Guest* g)
{
    obj_addref(g_dev.d3d);
    wr32(ARG(1), g_dev.d3d);
    RET(D3D_OK, 2);
}
static void Dev_GetDeviceCaps(Guest* g) { write_caps(ARG(1)); RET(D3D_OK, 2); }
static void Dev_GetDisplayMode(Guest* g)
{
    uint32_t p = ARG(2), w, h, hz;
    user32_desktop_mode(&w, &h, &hz);
    if (g_dev.pp[8])
        wr32(p, w), wr32(p + 4, h);
    else
        wr32(p, g_dev.pp[0]), wr32(p + 4, g_dev.pp[1]);
    wr32(p + 8, hz ? hz : 60), wr32(p + 12, FMT_X8R8G8B8);
    RET(D3D_OK, 3);
}
static void Dev_GetCreationParameters(Guest* g)
{
    uint32_t p = ARG(1);
    wr32(p, 0), wr32(p + 4, 1), wr32(p + 8, g_dev.hwnd), wr32(p + 12, g_dev.behavior);
    RET(D3D_OK, 2);
}
static void Dev_SetCursorProperties(Guest* g) { RET(D3D_OK, 4); }
static void Dev_SetCursorPosition(Guest* g) { RET(0, 4); }
static void Dev_ShowCursor(Guest* g) { RET(1, 2); }
static void Dev_GetNumberOfSwapChains(Guest* g) { RET(1, 1); }
static void Dev_SetDialogBoxMode(Guest* g) { RET(D3D_OK, 2); }
static void Dev_SetGammaRamp(Guest* g) { RET(0, 4); }
static void Dev_GetGammaRamp(Guest* g)
{
    uint32_t p = ARG(2);
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t i = 0; i < 256; ++i)
            wr16(p + 512 * c + 2 * i, (uint16_t)(i * 257));
    RET(0, 3);
}

static void Dev_GetRasterStatus(Guest* g)
{
    uint32_t p = ARG(2);
    wr32(p, 0);
    wr32(p + 4, (uint32_t)((rt_monotonic_ns() / 15000u) % 1080u));
    RET(D3D_OK, 3);
}

/* Reset(D3DPRESENT_PARAMETERS*) */
static void Dev_Reset(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t pp = ARG(1);
    for (int i = 0; i < 14; ++i)
        d->pp[i] = rd32(pp + 4u * i);
    if (d->pp[7])
        d->hwnd = d->pp[7];
    if (!d->pp[0] || !d->pp[1])
        user32_client_size(d->hwnd, &d->pp[0], &d->pp[1]);
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    wr32(pp, d->pp[0]);
    wr32(pp + 4, d->pp[1]);
    g9_finish();
    for (int i = 0; i < G9_MAX_RT; ++i)
        bind(&d->rt[i], 0);
    bind(&d->ds, 0);
    Obj* bb = obj(d->backbuffer);
    if (bb)
        bb->implicit = 0, bb->refs = 1, obj_release(d->backbuffer);
    Obj* ad = obj(d->autodepth);
    if (ad)
        ad->implicit = 0, ad->refs = 1, obj_release(d->autodepth);
    d->backbuffer = d->autodepth = 0;
    user32_set_fullscreen(d->hwnd, !d->pp[8]);
    g9_resize(d->pp[0], d->pp[1]);
    State old = d->st;
    state_defaults(&d->st, d->pp[0], d->pp[1], d->pp[9] != 0);
    /* Reset keeps no bindings: the stream, texture and shader slots return to their defaults */
    for (int i = 0; i < NSAMP; ++i)
        if (old.tex[i])
            obj_release(old.tex[i]);
    for (int i = 0; i < G9_MAX_STREAMS; ++i)
        if (old.stream[i].vb)
            obj_release(old.stream[i].vb);
    if (old.indices)
        obj_release(old.indices);
    if (old.decl)
        obj_release(old.decl);
    if (old.vs)
        obj_release(old.vs);
    if (old.ps)
        obj_release(old.ps);
    make_targets(d);
    rt_log("[d3d9] Reset: %ux%u %s\n", d->pp[0], d->pp[1], d->pp[8] ? "windowed" : "full screen");
    RET(D3D_OK, 2);
}

static void Dev_GetBackBuffer(Guest* g)
{
    if (ARG(2) != 0 || !g_dev.backbuffer)
        RET(D3DERR_INVALIDCALL, 5);
    obj_addref(g_dev.backbuffer);
    wr32(ARG(4), g_dev.backbuffer);
    RET(D3D_OK, 5);
}

static void Dev_GetSwapChain(Guest* g)
{
    if (!g_dev.swap)
        g_dev.swap = obj_new(O_SWAP);
    obj_addref(g_dev.swap);
    wr32(ARG(2), g_dev.swap);
    RET(D3D_OK, 3);
}

static G9Tex* surface_gpu(const Obj* s, uint32_t* face, uint32_t* level);

/* XIV_SHOT=<file.ppm> [XIV_SHOT_FRAME=n]: the back buffer at frame n (default 300) as a PPM */
static void debug_shot(G9Tex* bb, uint32_t w, uint32_t h)
{
    static const char* path;
    static uint64_t at, at_ns, t0;
    static int done;
    if (!path)
    {
        path = getenv("XIV_SHOT") ? getenv("XIV_SHOT") : "";
        at = getenv("XIV_SHOT_FRAME") ? strtoull(getenv("XIV_SHOT_FRAME"), NULL, 0) : 300;
        /* XIV_SHOT_SEC=s: at that many seconds instead, and the share of the screen's centre that
         * is lit logged (0% with the 3D scene missing behind the interface) */
        at_ns = getenv("XIV_SHOT_SEC") ? strtoull(getenv("XIV_SHOT_SEC"), NULL, 0) * 1000000000ull : 0;
        t0 = rt_monotonic_ns();
    }
    /* XIV_SHOT_EVERY=s: <XIV_SHOT>.<n>.ppm every s seconds as well (calibrating test timings) */
    {
        static uint64_t every, last_shot;
        static int nshot;
        static int init;
        if (!init)
            init = 1, every = getenv("XIV_SHOT_EVERY") ? strtoull(getenv("XIV_SHOT_EVERY"), NULL, 0) * 1000000000ull : 0;
        if (every && *path && bb && rt_monotonic_ns() - t0 >= last_shot + every)
        {
            last_shot += every;
            uint8_t* q = (uint8_t*)malloc((size_t)w * h * 4);
            g9_tex_read(bb, 0, 0, q, w * 4);
            char name[512];
            snprintf(name, sizeof name, "%s.%02d.ppm", path, ++nshot);
            FILE* f = fopen(name, "wb");
            if (f)
            {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                for (size_t i = 0; i < (size_t)w * h; ++i)
                    fputc(q[i * 4 + 2], f), fputc(q[i * 4 + 1], f), fputc(q[i * 4], f);
                fclose(f);
            }
            free(q);
        }
    }
    if (!*path || !bb || done)
        return;
    if (at_ns ? rt_monotonic_ns() - t0 < at_ns : g_dev.frames != at)
        return;
    done = 1;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    g9_tex_read(bb, 0, 0, px, w * 4);
    {
        uint64_t lit = 0, n = 0;
        for (uint32_t y = h / 4; y < h * 3 / 4; ++y)
            for (uint32_t x = w / 3; x < w * 2 / 3; ++x, ++n)
            {
                const uint8_t* q = px + ((size_t)y * w + x) * 4;
                lit += q[0] + q[1] + q[2] > 48;
            }
        rt_log("[shot] centre lit %.1f%%\n", 100.0 * lit / (n ? n : 1));
    }
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            uint8_t rgb[3] = { px[4 * i + 2], px[4 * i + 1], px[4 * i] };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        rt_log("[d3d9] frame %llu written to %s\n", (unsigned long long)g_dev.frames, path);
    }
    free(px);
}

extern void (*thunk_timer)(uint64_t ns);
extern uint32_t thunk_prof_thread;
void thunk_prof_report(void);
void thunk_lock_report(void);
void xiv_ui_report(void); /* xiv_ui.c: the interface's animation counters (XIV_ANIMLOG) */
static void prof_noop(uint64_t ns) { (void)ns; }
static uint64_t g_present_ns; /* time in g9_present, in all */

static void present(void)
{
    uint32_t f, l;
    Obj* bb = obj(g_dev.backbuffer);
    G9Tex* t = bb ? surface_gpu(bb, &f, &l) : NULL;
    if (bb)
        debug_shot(t, bb->width, bb->height);
    uint64_t p0 = rt_monotonic_ns();
    g9_present(t);
    g_present_ns += rt_monotonic_ns() - p0;
    g_dev.frames++;
    /* XIV_PEEK=base+off,...: logs rd32(rd32(base) + off) every 300 frames (debugging) */
    if (getenv("XIV_PEEK") && g_dev.frames % 300 == 0)
        for (const char* k = getenv("XIV_PEEK"); k && *k;)
        {
            char* end;
            uint32_t base = (uint32_t)strtoul(k, &end, 0), off = 0;
            if (*end == '+')
                off = (uint32_t)strtoul(end + 1, &end, 0);
            uint32_t p = rd32(base), v = p ? rd32(p + off) : 0;
            rt_log("[peek] frame %llu: [%08x]=%08x, [+%x]=%08x\n", (unsigned long long)g_dev.frames, base, p, off, v);
            k = *end == ',' ? end + 1 : end;
            if (!*end)
                break;
        }
    /* XIV_AUTOKEYS_MS=ms:vk,ms:vk,...: key presses that many milliseconds after the first frame
     * (vk 0: activate the window). Wall-clock waits, unlike frame counts, mean the same at any
     * frame rate. */
    {
        static uint64_t t0;
        static unsigned next;
        const char* tk = getenv("XIV_AUTOKEYS_MS");
        if (tk)
        {
            if (!t0)
                t0 = rt_monotonic_ns();
            uint64_t ms = (rt_monotonic_ns() - t0) / 1000000u;
            unsigned idx = 0;
            for (const char* k = tk; *k; ++idx)
            {
                char* end;
                unsigned long at = strtoul(k, &end, 0);
                if (*end != ':')
                    break;
                unsigned long vk = strtoul(end + 1, &end, 0);
                if (idx == next && ms >= at)
                {
                    rt_log("[d3d9] %llu ms: key %#lx\n", (unsigned long long)ms, vk);
                    user32_press_key((uint32_t)vk);
                    next++;
                }
                if (*end != ',')
                    break;
                k = end + 1;
            }
        }
    }
    /* XIV_AUTOKEYS=frame:vk,frame:vk,...: key presses at those frames (tests) */
    const char* keys = getenv("XIV_AUTOKEYS");
    for (const char* k = keys; k && *k;)
    {
        char* end;
        unsigned long fr = strtoul(k, &end, 0);
        if (*end != ':')
            break;
        unsigned long vk = strtoul(end + 1, &end, 0);
        if (fr == g_dev.frames)
        {
            rt_log("[d3d9] frame %lu: key %#lx\n", fr, vk);
            user32_press_key((uint32_t)vk);
        }
        k = *end == ',' ? end + 1 : end;
        if (!*end)
            break;
    }
    if (g_dev.frames == 1) /* the thread presenting is the frame's critical path */
    {
        gt_set_long_quantum();
        if (getenv("XIV_ANIMLOG"))
            rt_log("[d3d9] render thread %u\n", plat_thread_id());
    }
    /* XIV_PROF=1: where the render thread spends its time in the host, every 600 frames */
    static int prof = -1;
    if (prof < 0 && (prof = getenv("XIV_PROF") != NULL))
    {
        thunk_timer = prof_noop;
        if (strcmp(getenv("XIV_PROF"), "logic"))
            thunk_prof_thread = plat_thread_id();
    }
    if (prof && g_dev.frames % 600 == 0)
        thunk_prof_report();
    xiv_ui_report();
    if (g_dev.frames == 1 || g_dev.frames % 600 == 0)
    {
        static uint64_t last_ns, last_frames, last_wait, last_out, last_present;
        uint64_t now = rt_monotonic_ns();
        if (last_ns)
            rt_log("[d3d9] %llu frames (%.1f fps; render thread: %.0f%% lock wait, %.0f%% blocked, %.0f%% present)\n",
                   (unsigned long long)g_dev.frames, (g_dev.frames - last_frames) * 1e9 / (double)(now - last_ns),
                   100.0 * (gt_wait_ns() - last_wait) / (double)(now - last_ns),
                   100.0 * ((gt_out_ns() - last_out) - (gt_wait_ns() - last_wait)) / (double)(now - last_ns),
                   100.0 * (g_present_ns - last_present) / (double)(now - last_ns));
        else
            rt_log("[d3d9] %llu frames\n", (unsigned long long)g_dev.frames);
        thunk_lock_report();
        if (last_ns && getenv("XIV_LOCKLOG"))
            gt_lock_report(now - last_ns);
        last_ns = now, last_frames = g_dev.frames, last_wait = gt_wait_ns(), last_out = gt_out_ns(),
        last_present = g_present_ns;
    }
}

static void Dev_Present(Guest* g) { present(); RET(D3D_OK, 5); }
static void Dev_BeginScene(Guest* g) { g_dev.in_scene = 1; RET(D3D_OK, 1); }
static void Dev_EndScene(Guest* g) { g_dev.in_scene = 0; RET(D3D_OK, 1); }

/* --- surfaces and textures --------------------------------------------------------------------- */

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, uint32_t container)
{
    uint32_t p = obj_new(O_SURFACE);
    Obj* s = obj(p);
    s->format = fmt;
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->depth = 1;
    s->usage = usage;
    s->pool = pool;
    s->levels = 1;
    s->size = fmt_pitch(fmt, s->width) * fmt_rows(fmt, s->height);
    s->container = container;
    if (!container && pool != POOL_SYSTEMMEM && (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)))
        s->gpu = g9_tex_create(G9_TEX_2D, fmt, s->width, s->height, 1, 1,
            (usage & USAGE_DEPTHSTENCIL) ? G9_USE_DEPTH : G9_USE_RT);
    else if (!container && pool != POOL_SYSTEMMEM)
        s->gpu = g9_tex_create(G9_TEX_2D, fmt, s->width, s->height, 1, 1, G9_USE_SAMPLE);
    return p;
}

static G9Tex* surface_gpu(const Obj* s, uint32_t* face, uint32_t* level)
{
    if (s->container)
    {
        Obj* t = obj(s->container);
        *face = s->face, *level = s->level;
        return t ? t->gpu : NULL;
    }
    *face = *level = 0;
    return s->gpu;
}

/* the byte offset of a level (and face) in a texture's guest memory */
static uint32_t level_offset(const Obj* t, uint32_t face, uint32_t level)
{
    uint32_t off = 0, per_face = 0;
    for (uint32_t l = 0; l < t->levels; ++l)
        per_face += fmt_pitch(t->format, lvl(t->width, l)) * fmt_rows(t->format, lvl(t->height, l)) * lvl(t->depth, l);
    off = face * per_face;
    for (uint32_t l = 0; l < level; ++l)
        off += fmt_pitch(t->format, lvl(t->width, l)) * fmt_rows(t->format, lvl(t->height, l)) * lvl(t->depth, l);
    return off;
}

static uint32_t new_texture(int kind, uint32_t w, uint32_t h, uint32_t dd, uint32_t levels, uint32_t usage, uint32_t fmt,
    uint32_t pool)
{
    uint32_t p = obj_new(kind);
    Obj* t = obj(p);
    t->format = fmt, t->usage = usage, t->pool = pool;
    t->width = w ? w : 1;
    t->height = kind == O_CUBE ? t->width : (h ? h : 1);
    t->depth = kind == O_VOLTEX ? (dd ? dd : 1) : 1;
    uint32_t maxl = 1;
    for (uint32_t s = t->width > t->height ? t->width : t->height; s > 1; s >>= 1)
        maxl++;
    if (usage & USAGE_AUTOGENMIPMAP)
        t->autogen = 1, levels = 0;
    t->levels = levels && levels <= maxl ? levels : maxl;
    uint32_t faces = kind == O_CUBE ? 6 : 1;
    t->size = level_offset(t, faces, 0);
    if (pool != POOL_SYSTEMMEM)
        t->gpu = g9_tex_create(kind == O_CUBE ? G9_TEX_CUBE : kind == O_VOLTEX ? G9_TEX_3D : G9_TEX_2D, fmt, t->width,
            t->height, t->depth, t->levels,
            (usage & USAGE_DEPTHSTENCIL) ? G9_USE_DEPTH : (usage & USAGE_RENDERTARGET) ? G9_USE_RT : G9_USE_SAMPLE);
    if (t->gpu)
        t->levels = t->levels; /* the back end may have clamped tiny DXT levels, not the count */
    uint32_t subs = faces * (t->autogen ? 1 : t->levels);
    t->nsubs = faces * t->levels;
    t->subs = (uint32_t*)calloc(t->nsubs, 4);
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < t->levels; ++l)
        {
            uint32_t s = obj_new(kind == O_VOLTEX ? O_VOLUME : O_SURFACE);
            Obj* so = obj(s);
            so->format = fmt, so->usage = usage, so->pool = pool;
            so->width = lvl(t->width, l), so->height = lvl(t->height, l), so->depth = lvl(t->depth, l);
            so->levels = 1;
            so->size = fmt_pitch(fmt, so->width) * fmt_rows(fmt, so->height) * so->depth;
            so->container = p;
            so->face = f, so->level = l;
            t->subs[f * t->levels + l] = s;
        }
    (void)subs;
    return p;
}

/* CreateTexture(w, h, levels, usage, fmt, pool, IDirect3DTexture9**, shared) */
static void Dev_CreateTexture(Guest* g)
{
    uint32_t p = new_texture(O_TEXTURE, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6));
    if (!obj(p)->gpu && ARG(6) != POOL_SYSTEMMEM)
    {
        obj_release(p);
        RET(D3DERR_INVALIDCALL, 9);
    }
    wr32(ARG(7), p);
    RET(D3D_OK, 9);
}
/* CreateVolumeTexture(w, h, depth, levels, usage, fmt, pool, IDirect3DVolumeTexture9**, shared) */
static void Dev_CreateVolumeTexture(Guest* g)
{
    wr32(ARG(8), new_texture(O_VOLTEX, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7)));
    RET(D3D_OK, 10);
}
/* CreateCubeTexture(edge, levels, usage, fmt, pool, IDirect3DCubeTexture9**, shared) */
static void Dev_CreateCubeTexture(Guest* g)
{
    wr32(ARG(6), new_texture(O_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5)));
    RET(D3D_OK, 8);
}

/* CreateRenderTarget(w, h, fmt, ms, msq, lockable, IDirect3DSurface9**, shared) */
static void Dev_CreateRenderTarget(Guest* g)
{
    wr32(ARG(7), new_surface(ARG(3), ARG(1), ARG(2), USAGE_RENDERTARGET, POOL_DEFAULT, 0));
    RET(D3D_OK, 9);
}
/* CreateDepthStencilSurface(w, h, fmt, ms, msq, discard, IDirect3DSurface9**, shared) */
static void Dev_CreateDepthStencilSurface(Guest* g)
{
    wr32(ARG(7), new_surface(ARG(3), ARG(1), ARG(2), USAGE_DEPTHSTENCIL, POOL_DEFAULT, 0));
    RET(D3D_OK, 9);
}
/* CreateOffscreenPlainSurface(w, h, fmt, pool, IDirect3DSurface9**, shared) */
static void Dev_CreateOffscreenPlainSurface(Guest* g)
{
    wr32(ARG(5), new_surface(ARG(3), ARG(1), ARG(2), 0, ARG(4), 0));
    RET(D3D_OK, 7);
}

/* the guest memory of a surface (its own, or its place in its texture's) */
static uint32_t surface_mem(Obj* s)
{
    if (s->container)
    {
        Obj* t = obj(s->container);
        return obj_mem(t) + level_offset(t, s->face, s->level);
    }
    return obj_mem(s);
}

static int gpu_owned(const Obj* s)
{
    return (s->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && s->pool == POOL_DEFAULT;
}

/* uploads a box of a surface's guest memory to its GPU texture */
static void upload_box(Obj* s, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t front, uint32_t back)
{
    uint32_t face, level;
    G9Tex* gpu = surface_gpu(s, &face, &level);
    if (!gpu)
        return;
    if (fmt_block(s->format))
    {
        l &= ~3, t &= ~3;
        r = (r + 3) & ~3, b = (b + 3) & ~3;
    }
    uint32_t pitch = fmt_pitch(s->format, s->width), slice = pitch * fmt_rows(s->format, s->height);
    uint32_t base = surface_mem(s) + front * slice + (uint32_t)(fmt_rows(s->format, (uint32_t)t) * pitch) +
                    (fmt_block(s->format) ? (uint32_t)l / 4 * fmt_block(s->format) : (uint32_t)l * fmt_bytes(s->format));
    g9_tex_upload(gpu, face, level, (uint32_t)l, (uint32_t)t, front, (uint32_t)(r - l), (uint32_t)(b - t), back - front,
        GUEST_PTR(base), pitch, slice);
}

/* LockRect on a surface: D3DLOCKED_RECT { Pitch, pBits } */
static uint32_t lock_rect(Obj* s, uint32_t out, uint32_t rect, uint32_t flags)
{
    int32_t l = 0, t = 0, r = (int32_t)s->width, b = (int32_t)s->height;
    if (rect)
        l = (int32_t)rd32(rect), t = (int32_t)rd32(rect + 4), r = (int32_t)rd32(rect + 8), b = (int32_t)rd32(rect + 12);
    uint32_t pitch = fmt_pitch(s->format, s->width);
    uint32_t mem = surface_mem(s);
    if (gpu_owned(s) && !(flags & LOCK_DISCARD))
    {
        uint32_t face, level;
        G9Tex* gpu = surface_gpu(s, &face, &level);
        if (gpu && !fmt_block(s->format))
            g9_tex_read(gpu, face, level, GUEST_PTR(mem), pitch);
    }
    s->locked = 1;
    s->lock_flags = flags;
    s->lock_rect[0] = l, s->lock_rect[1] = t, s->lock_rect[2] = r, s->lock_rect[3] = b;
    uint32_t off = fmt_rows(s->format, (uint32_t)t) * pitch +
                   (fmt_block(s->format) ? (uint32_t)l / 4 * fmt_block(s->format) : (uint32_t)l * fmt_bytes(s->format));
    wr32(out, pitch);
    wr32(out + 4, mem + off);
    return D3D_OK;
}

static void unlock_rect(Obj* s)
{
    if (!s->locked)
        return;
    s->locked = 0;
    if (!(s->lock_flags & LOCK_READONLY))
        upload_box(s, s->lock_rect[0], s->lock_rect[1], s->lock_rect[2], s->lock_rect[3], 0, 1);
    Obj* t = s->container ? obj(s->container) : NULL;
    if (t && t->autogen && s->level == 0)
        g9_tex_gen_mips(t->gpu);
}

static Obj* sub(Obj* t, uint32_t face, uint32_t level)
{
    if (!t || face * t->levels + level >= t->nsubs || level >= t->levels)
        return NULL;
    return obj(t->subs[face * t->levels + level]);
}

static void Surface_GetContainer(Guest* g)
{
    Obj* s = obj(ARG(0));
    uint32_t c = s && s->container ? s->container : g_dev.guest;
    obj_addref(c);
    wr32(ARG(2), c);
    RET(D3D_OK, 3);
}

/* D3DSURFACE_DESC { Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height } */
static void write_surface_desc(uint32_t p, const Obj* s)
{
    wr32(p, s->format), wr32(p + 4, RTYPE_SURFACE), wr32(p + 8, s->usage), wr32(p + 12, s->pool);
    wr32(p + 16, 0), wr32(p + 20, 0), wr32(p + 24, s->width), wr32(p + 28, s->height);
}

static void Surface_GetDesc(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    write_surface_desc(ARG(1), s);
    RET(D3D_OK, 2);
}
static void Surface_LockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    RET(s ? lock_rect(s, ARG(1), ARG(2), ARG(3)) : D3DERR_INVALIDCALL, 4);
}
static void Surface_UnlockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (s)
        unlock_rect(s);
    RET(D3D_OK, 1);
}
static void Surface_GetDC(Guest* g) { RET(D3DERR_INVALIDCALL, 2); }
static void Surface_ReleaseDC(Guest* g) { RET(D3D_OK, 2); }

static void BaseTex_SetLOD(Guest* g) { Obj* t = obj(ARG(0)); uint32_t old = t ? t->lod : 0; if (t) t->lod = ARG(1); RET(old, 2); }
static void BaseTex_GetLOD(Guest* g) { Obj* t = obj(ARG(0)); RET(t ? t->lod : 0, 1); }
static void BaseTex_GetLevelCount(Guest* g) { Obj* t = obj(ARG(0)); RET(t ? (t->autogen ? 1 : t->levels) : 0, 1); }
static void BaseTex_SetAutoGenFilterType(Guest* g) { RET(D3D_OK, 2); }
static void BaseTex_GetAutoGenFilterType(Guest* g) { RET(2, 1); }
static void BaseTex_GenerateMipSubLevels(Guest* g) { Obj* t = obj(ARG(0)); if (t) g9_tex_gen_mips(t->gpu); RET(0, 1); }

static void Tex_GetLevelDesc(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}
static void Tex_GetSurfaceLevel(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    obj_addref(s->guest);
    wr32(ARG(2), s->guest);
    RET(D3D_OK, 3);
}
static void Tex_LockRect(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), 0, ARG(1));
    RET(s ? lock_rect(s, ARG(2), ARG(3), ARG(4)) : D3DERR_INVALIDCALL, 5);
}
static void Tex_UnlockRect(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), 0, ARG(1));
    if (s)
        unlock_rect(s);
    RET(D3D_OK, 2);
}
static void Tex_AddDirtyRect(Guest* g) { RET(D3D_OK, 2); }

static void Cube_GetLevelDesc(Guest* g) { Tex_GetLevelDesc(g); }
static void Cube_GetCubeMapSurface(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    obj_addref(s->guest);
    wr32(ARG(3), s->guest);
    RET(D3D_OK, 4);
}
static void Cube_LockRect(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), ARG(1), ARG(2));
    RET(s ? lock_rect(s, ARG(3), ARG(4), ARG(5)) : D3DERR_INVALIDCALL, 6);
}
static void Cube_UnlockRect(Guest* g)
{
    Obj* s = sub(obj(ARG(0)), ARG(1), ARG(2));
    if (s)
        unlock_rect(s);
    RET(D3D_OK, 3);
}
static void Cube_AddDirtyRect(Guest* g) { RET(D3D_OK, 3); }

/* volumes: D3DLOCKED_BOX { RowPitch, SlicePitch, pBits }, D3DBOX { Left, Top, Right, Bottom, Front, Back } */
static uint32_t lock_box(Obj* v, uint32_t out, uint32_t box, uint32_t flags)
{
    int32_t b[6] = { 0, 0, (int32_t)v->width, (int32_t)v->height, 0, (int32_t)v->depth };
    if (box)
        for (int i = 0; i < 6; ++i)
            b[i] = (int32_t)rd32(box + 4u * i);
    uint32_t pitch = fmt_pitch(v->format, v->width), slice = pitch * fmt_rows(v->format, v->height);
    v->locked = 1, v->lock_flags = flags;
    memcpy(v->lock_rect, b, sizeof b);
    wr32(out, pitch);
    wr32(out + 4, slice);
    wr32(out + 8, surface_mem(v) + (uint32_t)b[4] * slice + fmt_rows(v->format, (uint32_t)b[1]) * pitch +
                      (uint32_t)b[0] * fmt_bytes(v->format));
    return D3D_OK;
}
static void unlock_box(Obj* v)
{
    if (!v->locked)
        return;
    v->locked = 0;
    if (!(v->lock_flags & LOCK_READONLY))
        upload_box(v, v->lock_rect[0], v->lock_rect[1], v->lock_rect[2], v->lock_rect[3], (uint32_t)v->lock_rect[4],
            (uint32_t)v->lock_rect[5]);
}
static void VolTex_GetLevelDesc(Guest* g)
{
    Obj* v = sub(obj(ARG(0)), 0, ARG(1));
    if (!v)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t p = ARG(2); /* D3DVOLUME_DESC { Format, Type, Usage, Pool, Width, Height, Depth } */
    wr32(p, v->format), wr32(p + 4, RTYPE_VOLUME), wr32(p + 8, v->usage), wr32(p + 12, v->pool);
    wr32(p + 16, v->width), wr32(p + 20, v->height), wr32(p + 24, v->depth);
    RET(D3D_OK, 3);
}
static void VolTex_GetVolumeLevel(Guest* g)
{
    Obj* v = sub(obj(ARG(0)), 0, ARG(1));
    if (!v)
        RET(D3DERR_INVALIDCALL, 3);
    obj_addref(v->guest);
    wr32(ARG(2), v->guest);
    RET(D3D_OK, 3);
}
static void VolTex_LockBox(Guest* g)
{
    Obj* v = sub(obj(ARG(0)), 0, ARG(1));
    RET(v ? lock_box(v, ARG(2), ARG(3), ARG(4)) : D3DERR_INVALIDCALL, 5);
}
static void VolTex_UnlockBox(Guest* g)
{
    Obj* v = sub(obj(ARG(0)), 0, ARG(1));
    if (v)
        unlock_box(v);
    RET(D3D_OK, 2);
}
static void VolTex_AddDirtyBox(Guest* g) { RET(D3D_OK, 2); }
static void Volume_GetDesc(Guest* g)
{
    Obj* v = obj(ARG(0));
    if (!v)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t p = ARG(1);
    wr32(p, v->format), wr32(p + 4, RTYPE_VOLUME), wr32(p + 8, v->usage), wr32(p + 12, v->pool);
    wr32(p + 16, v->width), wr32(p + 20, v->height), wr32(p + 24, v->depth);
    RET(D3D_OK, 2);
}
static void Volume_LockBox(Guest* g) { Obj* v = obj(ARG(0)); RET(v ? lock_box(v, ARG(1), ARG(2), ARG(3)) : D3DERR_INVALIDCALL, 4); }
static void Volume_UnlockBox(Guest* g) { Obj* v = obj(ARG(0)); if (v) unlock_box(v); RET(D3D_OK, 1); }

/* UpdateSurface(src, srcrect, dst, dstpoint): system memory to a default-pool surface */
static void Dev_UpdateSurface(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(3));
    if (!s || !d)
        RET(D3DERR_INVALIDCALL, 5);
    int32_t r[4] = { 0, 0, (int32_t)s->width, (int32_t)s->height };
    if (ARG(2))
        for (int i = 0; i < 4; ++i)
            r[i] = (int32_t)rd32(ARG(2) + 4u * i);
    int32_t dx = ARG(4) ? (int32_t)rd32(ARG(4)) : r[0], dy = ARG(4) ? (int32_t)rd32(ARG(4) + 4) : r[1];
    uint32_t face, level;
    G9Tex* gpu = surface_gpu(d, &face, &level);
    if (gpu)
    {
        uint32_t pitch = fmt_pitch(s->format, s->width);
        uint32_t base = surface_mem(s) + fmt_rows(s->format, (uint32_t)r[1]) * pitch +
                        (fmt_block(s->format) ? (uint32_t)r[0] / 4 * fmt_block(s->format) : (uint32_t)r[0] * fmt_bytes(s->format));
        g9_tex_upload(gpu, face, level, (uint32_t)dx, (uint32_t)dy, 0, (uint32_t)(r[2] - r[0]), (uint32_t)(r[3] - r[1]), 1,
            GUEST_PTR(base), pitch, 0);
    }
    RET(D3D_OK, 5);
}

/* UpdateTexture(src, dst): every level of a system-memory texture to its default-pool twin */
static void Dev_UpdateTexture(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(2));
    if (!s || !d || !d->gpu)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t faces = s->kind == O_CUBE ? 6 : 1;
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < s->levels && l < d->levels; ++l)
        {
            Obj* ss = sub(s, f, l);
            if (!ss)
                continue;
            uint32_t pitch = fmt_pitch(s->format, ss->width), slice = pitch * fmt_rows(s->format, ss->height);
            g9_tex_upload(d->gpu, f, l, 0, 0, 0, ss->width, ss->height, ss->depth, GUEST_PTR(surface_mem(ss)), pitch, slice);
        }
    RET(D3D_OK, 3);
}

/* GetRenderTargetData(rt, system memory surface) */
static void Dev_GetRenderTargetData(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(2));
    if (!s || !d)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t face, level;
    G9Tex* gpu = surface_gpu(s, &face, &level);
    if (gpu)
        g9_tex_read(gpu, face, level, GUEST_PTR(surface_mem(d)), fmt_pitch(d->format, d->width));
    RET(D3D_OK, 3);
}
static void Dev_GetFrontBufferData(Guest* g) { RET(D3D_OK, 3); }

/* StretchRect(src, srcrect, dst, dstrect, filter) */
static void Dev_StretchRect(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(3));
    if (!s || !d)
        RET(D3DERR_INVALIDCALL, 6);
    uint32_t sf, sl, df, dl;
    G9Tex* sg = surface_gpu(s, &sf, &sl);
    G9Tex* dg = surface_gpu(d, &df, &dl);
    int32_t sr[4], dr[4];
    for (int i = 0; i < 4; ++i)
    {
        sr[i] = ARG(2) ? (int32_t)rd32(ARG(2) + 4u * i) : 0;
        dr[i] = ARG(4) ? (int32_t)rd32(ARG(4) + 4u * i) : 0;
    }
    g9_stretch(sg, sf, sl, ARG(2) ? sr : NULL, dg, df, dl, ARG(4) ? dr : NULL, ARG(5) == 2 /* LINEAR */);
    RET(D3D_OK, 6);
}

/* ColorFill(surface, rect, color) */
static void Dev_ColorFill(Guest* g)
{
    Obj* s = obj(ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t f, l;
    int32_t r[4];
    if (ARG(2))
        for (int i = 0; i < 4; ++i)
            r[i] = (int32_t)rd32(ARG(2) + 4u * i);
    g9_fill(surface_gpu(s, &f, &l), f, l, ARG(2) ? r : NULL, ARG(3));
    RET(D3D_OK, 4);
}

/* --- vertex and index buffers ------------------------------------------------------------------- */

/* CreateVertexBuffer(length, usage, fvf, pool, IDirect3DVertexBuffer9**, shared) */
static void Dev_CreateVertexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_VB);
    Obj* b = obj(p);
    b->size = ARG(1) ? ARG(1) : 4, b->usage = ARG(2), b->fvf = ARG(3), b->pool = ARG(4);
    obj_mem(b);
    if (!(b->usage & USAGE_DYNAMIC) && b->pool != POOL_SYSTEMMEM)
        b->gbuf = g9_buf_create(b->size);
    wr32(ARG(5), p);
    RET(D3D_OK, 7);
}
/* CreateIndexBuffer(length, usage, format, pool, IDirect3DIndexBuffer9**, shared) */
static void Dev_CreateIndexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_IB);
    Obj* b = obj(p);
    b->size = ARG(1) ? ARG(1) : 4, b->usage = ARG(2), b->format = ARG(3), b->pool = ARG(4);
    obj_mem(b);
    if (!(b->usage & USAGE_DYNAMIC) && b->pool != POOL_SYSTEMMEM)
        b->gbuf = g9_buf_create(b->size);
    wr32(ARG(5), p);
    RET(D3D_OK, 7);
}

/* Lock(offset, size, void**, flags) */
static void Buffer_Lock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 5);
    uint32_t off = ARG(1), size = ARG(2);
    if (off > b->size)
        off = b->size;
    if (!size || off + size > b->size)
        size = b->size - off;
    b->lock_off = b->locked ? (off < b->lock_off ? off : b->lock_off) : off;
    uint32_t end = off + size, old_end = b->locked ? b->lock_off + b->lock_size : end;
    b->lock_size = (end > old_end ? end : old_end) - b->lock_off;
    b->lock_flags = ARG(4);
    b->locked = 1;
    wr32(ARG(3), obj_mem(b) + off);
    RET(D3D_OK, 5);
}
static void Buffer_Unlock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (b && b->locked)
    {
        b->locked = 0;
        if (b->gbuf && !(b->lock_flags & LOCK_READONLY))
            g9_buf_upload(b->gbuf, b->lock_off, GUEST_PTR(b->mem + b->lock_off), b->lock_size);
    }
    RET(D3D_OK, 1);
}
/* D3DVERTEXBUFFER_DESC { Format, Type, Usage, Pool, Size, FVF }; index: { Format, Type, Usage, Pool, Size } */
static void Buffer_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    uint32_t p = ARG(1);
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(p, b->kind == O_IB ? b->format : 100 /* VERTEXDATA */);
    wr32(p + 4, b->kind == O_IB ? RTYPE_IB : RTYPE_VB);
    wr32(p + 8, b->usage), wr32(p + 12, b->pool), wr32(p + 16, b->size);
    if (b->kind == O_VB)
        wr32(p + 20, b->fvf);
    RET(D3D_OK, 2);
}

/* --- targets, clear, viewport --------------------------------------------------------------------- */

static void Dev_SetRenderTarget(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= G9_MAX_RT || (i == 0 && !ARG(2)))
        RET(D3DERR_INVALIDCALL, 3);
    bind(&g_dev.rt[i], ARG(2));
    if (i == 0)
    {
        /* D3D9: setting render target 0 resets the viewport (and scissor) to it */
        Obj* s = obj(ARG(2));
        if (s)
        {
            g_dev.st.vp[0] = g_dev.st.vp[1] = 0;
            g_dev.st.vp[2] = s->width, g_dev.st.vp[3] = s->height;
            g_dev.st.vp[4] = 0, g_dev.st.vp[5] = f2u(1.0f);
            g_dev.st.scissor[0] = g_dev.st.scissor[1] = 0;
            g_dev.st.scissor[2] = (int32_t)s->width, g_dev.st.scissor[3] = (int32_t)s->height;
        }
    }
    RET(D3D_OK, 3);
}
static void Dev_GetRenderTarget(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= G9_MAX_RT || !g_dev.rt[i])
    {
        wr32(ARG(2), 0);
        RET(D3DERR_NOTFOUND, 3);
    }
    obj_addref(g_dev.rt[i]);
    wr32(ARG(2), g_dev.rt[i]);
    RET(D3D_OK, 3);
}
static void Dev_SetDepthStencilSurface(Guest* g) { bind(&g_dev.ds, ARG(1)); RET(D3D_OK, 2); }
static void Dev_GetDepthStencilSurface(Guest* g)
{
    if (!g_dev.ds)
    {
        wr32(ARG(1), 0);
        RET(D3DERR_NOTFOUND, 2);
    }
    obj_addref(g_dev.ds);
    wr32(ARG(1), g_dev.ds);
    RET(D3D_OK, 2);
}

static void apply_targets(void)
{
    G9Tex* c[G9_MAX_RT] = { 0 };
    uint8_t f[G9_MAX_RT] = { 0 }, l[G9_MAX_RT] = { 0 };
    for (int i = 0; i < G9_MAX_RT; ++i)
    {
        Obj* s = obj(g_dev.rt[i]);
        if (s)
        {
            uint32_t ff, ll;
            c[i] = surface_gpu(s, &ff, &ll);
            f[i] = (uint8_t)ff, l[i] = (uint8_t)ll;
        }
    }
    Obj* ds = obj(g_dev.ds);
    uint32_t df, dl;
    g9_set_targets(c, f, l, ds ? surface_gpu(ds, &df, &dl) : NULL);
}

static G9Viewport viewport(void)
{
    const uint32_t* v = g_dev.st.vp;
    G9Viewport vp = { v[0], v[1], v[2], v[3], u2f(v[4]), u2f(v[5]) };
    return vp;
}

static int32_t* scissor_rect(void)
{
    return g_dev.st.rs[174] ? g_dev.st.scissor : NULL;
}

/* Clear(count, rects, flags, color, z, stencil) */
static void Dev_Clear(Guest* g)
{
    apply_targets();
    uint32_t n = ARG(1), rects = ARG(2);
    int32_t rs[64 * 4];
    if (n > 64)
        n = 64;
    for (uint32_t i = 0; rects && i < 4 * n; ++i)
        rs[i] = (int32_t)rd32(rects + 4 * i);
    G9Viewport vp = viewport();
    g9_clear(rects ? n : 0, rs, ARG(3), ARG(4), u2f(ARG(5)), ARG(6), &vp, scissor_rect());
    RET(D3D_OK, 7);
}

static void Dev_SetViewport(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    for (int i = 0; i < 6; ++i)
        s->vp[i] = rd32(ARG(1) + 4u * i);
    if (m)
        m->vp = 1;
    RET(D3D_OK, 2);
}
static void Dev_GetViewport(Guest* g)
{
    for (int i = 0; i < 6; ++i)
        wr32(ARG(1) + 4u * i, g_dev.st.vp[i]);
    RET(D3D_OK, 2);
}
static void Dev_SetScissorRect(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    for (int i = 0; i < 4; ++i)
        s->scissor[i] = (int32_t)rd32(ARG(1) + 4u * i);
    if (m)
        m->scissor = 1;
    RET(D3D_OK, 2);
}
static void Dev_GetScissorRect(Guest* g)
{
    for (int i = 0; i < 4; ++i)
        wr32(ARG(1) + 4u * i, (uint32_t)g_dev.st.scissor[i]);
    RET(D3D_OK, 2);
}

/* --- fixed-function state the shaders do not read: stored for readback ---------------------------- */

static void Dev_SetTransform(Guest* g)
{
    uint32_t t = ARG(1);
    if (t >= NTS)
        RET(D3D_OK, 3);
    Mask* m;
    State* s = wstate(&m);
    memcpy(s->ts[t], GUEST_PTR(ARG(2)), 64);
    if (m)
        m->ts[t] = 1;
    RET(D3D_OK, 3);
}
static void Dev_GetTransform(Guest* g)
{
    uint32_t t = ARG(1);
    if (t < NTS)
        memcpy(GUEST_PTR(ARG(2)), g_dev.st.ts[t], 64);
    RET(D3D_OK, 3);
}
static void Dev_MultiplyTransform(Guest* g)
{
    uint32_t t = ARG(1);
    if (t >= NTS)
        RET(D3D_OK, 3);
    float m[16], r[16];
    memcpy(m, GUEST_PTR(ARG(2)), 64);
    const float* a = g_dev.st.ts[t];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[4 * i + j] = m[4 * i] * a[j] + m[4 * i + 1] * a[4 + j] + m[4 * i + 2] * a[8 + j] + m[4 * i + 3] * a[12 + j];
    memcpy(g_dev.st.ts[t], r, 64);
    RET(D3D_OK, 3);
}
static void Dev_SetMaterial(Guest* g) { memcpy(g_dev.st.material, GUEST_PTR(ARG(1)), 68); RET(D3D_OK, 2); }
static void Dev_GetMaterial(Guest* g) { memcpy(GUEST_PTR(ARG(1)), g_dev.st.material, 68); RET(D3D_OK, 2); }
static void Dev_SetLight(Guest* g) { RET(D3D_OK, 3); }
static void Dev_GetLight(Guest* g) { memset(GUEST_PTR(ARG(2)), 0, 104); RET(D3D_OK, 3); }
static void Dev_LightEnable(Guest* g) { RET(D3D_OK, 3); }
static void Dev_GetLightEnable(Guest* g) { wr32(ARG(2), 0); RET(D3D_OK, 3); }
static void Dev_SetClipPlane(Guest* g)
{
    if (ARG(1) < 6)
        memcpy(g_dev.st.clip[ARG(1)], GUEST_PTR(ARG(2)), 16);
    RET(D3D_OK, 3);
}
static void Dev_GetClipPlane(Guest* g)
{
    if (ARG(1) < 6)
        memcpy(GUEST_PTR(ARG(2)), g_dev.st.clip[ARG(1)], 16);
    RET(D3D_OK, 3);
}
static void Dev_SetClipStatus(Guest* g) { RET(D3D_OK, 2); }
static void Dev_GetClipStatus(Guest* g) { memset(GUEST_PTR(ARG(1)), 0, 8); RET(D3D_OK, 2); }

static void Dev_SetRenderState(Guest* g)
{
    uint32_t r = ARG(1);
    if (r < NRS)
    {
        Mask* m;
        State* s = wstate(&m);
        s->rs[r] = ARG(2);
        if (m)
            m->rs[r] = 1;
    }
    RET(D3D_OK, 3);
}
static void Dev_GetRenderState(Guest* g)
{
    uint32_t r = ARG(1);
    wr32(ARG(2), r < NRS ? g_dev.st.rs[r] : 0);
    RET(D3D_OK, 3);
}

static int sampler_slot(uint32_t s)
{
    if (s < G9_PS_SAMPLERS)
        return (int)s;
    if (s >= 257 && s <= 260) /* D3DVERTEXTEXTURESAMPLER0-3 */
        return G9_PS_SAMPLERS + (int)(s - 257);
    return -1;
}

static void Dev_GetTexture(Guest* g)
{
    int s = sampler_slot(ARG(1));
    uint32_t t = s >= 0 ? g_dev.st.tex[s] : 0;
    if (t)
        obj_addref(t);
    wr32(ARG(2), t);
    RET(D3D_OK, 3);
}
static void Dev_SetTexture(Guest* g)
{
    int s = sampler_slot(ARG(1));
    if (s >= 0)
    {
        Mask* m;
        State* st = wstate(&m);
        if (m)
        {
            st->tex[s] = ARG(2);
            m->tex[s] = 1;
        }
        else
            bind(&st->tex[s], ARG(2));
    }
    RET(D3D_OK, 3);
}
static void Dev_GetTextureStageState(Guest* g)
{
    uint32_t st = ARG(1), t = ARG(2);
    wr32(ARG(3), st < 8 && t < NTSS ? g_dev.st.tss[st][t] : 0);
    RET(D3D_OK, 4);
}
static void Dev_SetTextureStageState(Guest* g)
{
    uint32_t st = ARG(1), t = ARG(2);
    if (st < 8 && t < NTSS)
    {
        Mask* m;
        State* s = wstate(&m);
        s->tss[st][t] = ARG(3);
        if (m)
            m->tss[st][t] = 1;
    }
    RET(D3D_OK, 4);
}
static void Dev_GetSamplerState(Guest* g)
{
    int s = sampler_slot(ARG(1));
    uint32_t t = ARG(2);
    wr32(ARG(3), s >= 0 && t < NSS ? g_dev.st.ss[s][t] : 0);
    RET(D3D_OK, 4);
}
static void Dev_SetSamplerState(Guest* g)
{
    int s = sampler_slot(ARG(1));
    uint32_t t = ARG(2);
    if (s >= 0 && t < NSS)
    {
        Mask* m;
        State* st = wstate(&m);
        st->ss[s][t] = ARG(3);
        if (m)
            m->ss[s][t] = 1;
    }
    RET(D3D_OK, 4);
}
static void Dev_ValidateDevice(Guest* g) { if (ARG(1)) wr32(ARG(1), 1); RET(D3D_OK, 2); }
static void Dev_SetPaletteEntries(Guest* g) { RET(D3D_OK, 3); }
static void Dev_GetPaletteEntries(Guest* g) { RET(D3D_OK, 3); }
static void Dev_SetCurrentTexturePalette(Guest* g) { RET(D3D_OK, 2); }
static void Dev_GetCurrentTexturePalette(Guest* g) { wr32(ARG(1), 0); RET(D3D_OK, 2); }
static void Dev_SetSoftwareVertexProcessing(Guest* g) { RET(D3D_OK, 2); }
static void Dev_GetSoftwareVertexProcessing(Guest* g) { RET(0, 1); }
static void Dev_SetNPatchMode(Guest* g) { RET(D3D_OK, 2); }
static void Dev_GetNPatchMode(Guest* g) { RET(0, 1); }

/* --- declarations, shaders, constants, streams ---------------------------------------------------- */

static void Dev_CreateVertexDeclaration(Guest* g)
{
    uint32_t p = obj_new(O_DECL);
    Obj* d = obj(p);
    for (uint32_t e = ARG(1); d->nel < G9_MAX_ELEMS && rd16(e) != 0xFF; e += 8)
    {
        Elem9* x = &d->el[d->nel++];
        x->stream = rd16(e), x->offset = rd16(e + 2);
        x->type = rd8(e + 4), x->method = rd8(e + 5), x->usage = rd8(e + 6), x->index = rd8(e + 7);
    }
    wr32(ARG(2), p);
    RET(D3D_OK, 3);
}
static void Decl_GetDeclaration(Guest* g)
{
    Obj* d = obj(ARG(0));
    if (!d)
        RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1))
    {
        for (uint32_t i = 0; i < d->nel; ++i)
        {
            uint32_t e = ARG(1) + 8 * i;
            wr16(e, d->el[i].stream), wr16(e + 2, d->el[i].offset);
            wr8(e + 4, d->el[i].type), wr8(e + 5, d->el[i].method), wr8(e + 6, d->el[i].usage), wr8(e + 7, d->el[i].index);
        }
        uint32_t e = ARG(1) + 8 * d->nel; /* D3DDECL_END */
        wr16(e, 0xFF), wr16(e + 2, 0), wr8(e + 4, 17), wr8(e + 5, 0), wr8(e + 6, 0), wr8(e + 7, 0);
    }
    wr32(ARG(2), d->nel + 1);
    RET(D3D_OK, 3);
}

static void Dev_SetVertexDeclaration(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    if (m)
        s->decl = ARG(1), m->decl = 1;
    else
        bind(&s->decl, ARG(1));
    s->fvf = 0;
    RET(D3D_OK, 2);
}
static void Dev_GetVertexDeclaration(Guest* g)
{
    if (g_dev.st.decl)
        obj_addref(g_dev.st.decl);
    wr32(ARG(1), g_dev.st.decl);
    RET(D3D_OK, 2);
}

/* SetFVF: the layout as a declaration (D3DFVF bits) */
static uint32_t fvf_decl(uint32_t fvf, Elem9* el, uint32_t* n)
{
    uint32_t off = 0, k = 0;
    uint32_t pos = fvf & 0x400E;
    if (pos)
    {
        uint32_t type = pos == 0x004 ? 3 : 2; /* XYZRHW: FLOAT4 */
        el[k++] = (Elem9){ 0, (uint16_t)off, (uint8_t)type, 0, (uint8_t)(pos == 0x004 ? 9 : 0), 0 };
        off += pos == 0x004 ? 16 : 12;
        if (pos >= 0x006 && pos <= 0x00E && pos != 0x004) /* blend weights */
        {
            uint32_t nb = (pos - 0x006) / 2 + 1;
            el[k++] = (Elem9){ 0, (uint16_t)off, (uint8_t)(nb - 1), 0, 1, 0 };
            off += 4 * nb;
        }
    }
    if (fvf & 0x010) el[k++] = (Elem9){ 0, (uint16_t)off, 2, 0, 3, 0 }, off += 12;  /* NORMAL */
    if (fvf & 0x020) el[k++] = (Elem9){ 0, (uint16_t)off, 0, 0, 4, 0 }, off += 4;   /* PSIZE */
    if (fvf & 0x040) el[k++] = (Elem9){ 0, (uint16_t)off, 4, 0, 10, 0 }, off += 4;  /* DIFFUSE */
    if (fvf & 0x080) el[k++] = (Elem9){ 0, (uint16_t)off, 4, 0, 10, 1 }, off += 4;  /* SPECULAR */
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t t = 0; t < ntex && k < G9_MAX_ELEMS; ++t)
    {
        uint32_t f = (fvf >> (16 + 2 * t)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        uint32_t nc = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        el[k++] = (Elem9){ 0, (uint16_t)off, (uint8_t)(nc - 1), 0, 5, (uint8_t)t };
        off += 4 * nc;
    }
    *n = k;
    return off;
}

static void Dev_SetFVF(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    s->fvf = ARG(1);
    if (m)
        m->fvf = 1;
    RET(D3D_OK, 2);
}
static void Dev_GetFVF(Guest* g) { wr32(ARG(1), g_dev.st.fvf); RET(D3D_OK, 2); }

static uint32_t create_shader(int kind, uint32_t code, uint32_t out)
{
    uint32_t n = 0;
    while (n < (1u << 18))
    {
        uint32_t t = rd32(code + 4 * n);
        n++;
        if (t == 0x0000FFFFu)
            break;
        if ((t & 0xFFFF) == 0xFFFE)
            n += (t >> 16) & 0x7FFF;
    }
    uint32_t p = obj_new(kind);
    Obj* o = obj(p);
    o->ntokens = n;
    o->tokens = gheap_alloc(4 * n, 0);
    memcpy(GUEST_PTR(o->tokens), GUEST_PTR(code), 4 * n);
    o->shader = g9_shader_create((const uint32_t*)GUEST_PTR(o->tokens), n);
    wr32(out, p);
    return D3D_OK;
}
static void Dev_CreateVertexShader(Guest* g) { RET(create_shader(O_VS, ARG(1), ARG(2)), 3); }
static void Dev_CreatePixelShader(Guest* g) { RET(create_shader(O_PS, ARG(1), ARG(2)), 3); }
static void Shader_GetFunction(Guest* g)
{
    Obj* o = obj(ARG(0));
    if (!o)
        RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1))
        memcpy(GUEST_PTR(ARG(1)), GUEST_PTR(o->tokens), 4 * o->ntokens);
    wr32(ARG(2), 4 * o->ntokens);
    RET(D3D_OK, 3);
}

static void set_shader(uint32_t* slot, uint8_t* mask_slot, uint32_t p)
{
    Mask* m;
    wstate(&m);
    if (m)
        *slot = p, *mask_slot = 1;
    else
        bind(slot, p);
}
static void Dev_SetVertexShader(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    set_shader(&s->vs, m ? &m->vs : NULL, ARG(1));
    RET(D3D_OK, 2);
}
static void Dev_GetVertexShader(Guest* g) { if (g_dev.st.vs) obj_addref(g_dev.st.vs); wr32(ARG(1), g_dev.st.vs); RET(D3D_OK, 2); }
static void Dev_SetPixelShader(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    set_shader(&s->ps, m ? &m->ps : NULL, ARG(1));
    RET(D3D_OK, 2);
}
static void Dev_GetPixelShader(Guest* g) { if (g_dev.st.ps) obj_addref(g_dev.st.ps); wr32(ARG(1), g_dev.st.ps); RET(D3D_OK, 2); }

/* Set*ShaderConstantF/I/B(start, data, count) */
static void set_consts(Guest* g, void* base, uint8_t* mask, uint32_t limit, uint32_t elem)
{
    uint32_t start = ARG(1), n = ARG(3);
    if (start >= limit)
        RET(D3D_OK, 4);
    if (start + n > limit)
        n = limit - start;
    memcpy((uint8_t*)base + (size_t)start * elem, GUEST_PTR(ARG(2)), (size_t)n * elem);
    if (mask)
        memset(mask + start, 1, n);
    RET(D3D_OK, 4);
}
static void get_consts(Guest* g, const void* base, uint32_t limit, uint32_t elem)
{
    uint32_t start = ARG(1), n = ARG(3);
    if (start >= limit)
        RET(D3D_OK, 4);
    if (start + n > limit)
        n = limit - start;
    memcpy(GUEST_PTR(ARG(2)), (const uint8_t*)base + (size_t)start * elem, (size_t)n * elem);
    RET(D3D_OK, 4);
}
#define CONSTS(name, field, limit, elem)                                                           \
    static void Dev_Set##name(Guest* g) { Mask* m; State* s = wstate(&m); set_consts(g, s->field, m ? m->field : NULL, limit, elem); } \
    static void Dev_Get##name(Guest* g) { get_consts(g, g_dev.st.field, limit, elem); }
CONSTS(VertexShaderConstantF, vsc, G9_VS_FLOATS, 16)
CONSTS(VertexShaderConstantI, vsi, 16, 16)
CONSTS(VertexShaderConstantB, vsb, 16, 4)
CONSTS(PixelShaderConstantF, psc, G9_PS_FLOATS, 16)
CONSTS(PixelShaderConstantI, psi, 16, 16)
CONSTS(PixelShaderConstantB, psb, 16, 4)

/* SetStreamSource(stream, vb, offset, stride) */
static void Dev_SetStreamSource(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= G9_MAX_STREAMS)
        RET(D3DERR_INVALIDCALL, 5);
    Mask* m;
    State* s = wstate(&m);
    if (m)
        s->stream[i].vb = ARG(2), m->stream[i] = 1;
    else
        bind(&s->stream[i].vb, ARG(2));
    s->stream[i].offset = ARG(3);
    s->stream[i].stride = ARG(4);
    RET(D3D_OK, 5);
}
static void Dev_GetStreamSource(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= G9_MAX_STREAMS)
        RET(D3DERR_INVALIDCALL, 5);
    const Stream9* st = &g_dev.st.stream[i];
    if (st->vb)
        obj_addref(st->vb);
    wr32(ARG(2), st->vb), wr32(ARG(3), st->offset), wr32(ARG(4), st->stride);
    RET(D3D_OK, 5);
}
static void Dev_SetStreamSourceFreq(Guest* g)
{
    uint32_t i = ARG(1);
    if (i < G9_MAX_STREAMS)
        g_dev.st.stream[i].freq = ARG(2);
    RET(D3D_OK, 3);
}
static void Dev_GetStreamSourceFreq(Guest* g)
{
    uint32_t i = ARG(1);
    wr32(ARG(2), i < G9_MAX_STREAMS ? g_dev.st.stream[i].freq : 1);
    RET(D3D_OK, 3);
}
static void Dev_SetIndices(Guest* g)
{
    Mask* m;
    State* s = wstate(&m);
    if (m)
        s->indices = ARG(1), m->indices = 1;
    else
        bind(&s->indices, ARG(1));
    RET(D3D_OK, 2);
}
static void Dev_GetIndices(Guest* g)
{
    if (g_dev.st.indices)
        obj_addref(g_dev.st.indices);
    wr32(ARG(1), g_dev.st.indices);
    RET(D3D_OK, 2);
}

/* --- drawing ------------------------------------------------------------------------------------ */

static uint32_t g_ff_warned;

static G9Sampler sampler_of(const uint32_t* ss)
{
    G9Sampler s;
    memset(&s, 0, sizeof s);
    s.addr_u = (uint8_t)ss[1], s.addr_v = (uint8_t)ss[2], s.addr_w = (uint8_t)ss[3];
    s.border = ss[4];
    s.mag = (uint8_t)ss[5], s.min = (uint8_t)ss[6], s.mip = (uint8_t)ss[7];
    s.lod_bias = u2f(ss[8]);
    s.max_level = (uint8_t)ss[9];
    s.max_aniso = (uint8_t)ss[10];
    s.srgb = ss[11] != 0;
    return s;
}

static G9States states(void)
{
    const uint32_t* rs = g_dev.st.rs;
    G9States r;
    memset(&r, 0, sizeof r);
    r.zenable = rs[7] != 0, r.zwrite = rs[14] != 0, r.zfunc = (uint8_t)rs[23];
    r.stencil = rs[52] != 0, r.two_sided = rs[185] != 0;
    r.sfail = (uint8_t)rs[53], r.szfail = (uint8_t)rs[54], r.spass = (uint8_t)rs[55], r.sfunc = (uint8_t)rs[56];
    r.ccw_sfail = (uint8_t)rs[186], r.ccw_szfail = (uint8_t)rs[187], r.ccw_spass = (uint8_t)rs[188], r.ccw_sfunc = (uint8_t)rs[189];
    r.stencil_ref = rs[57], r.sread = (uint8_t)rs[58], r.swrite = (uint8_t)rs[59];
    r.blend = rs[27] != 0, r.src = (uint8_t)rs[19], r.dst = (uint8_t)rs[20], r.op = (uint8_t)rs[171];
    r.sep_alpha = rs[206] != 0, r.src_a = (uint8_t)rs[207], r.dst_a = (uint8_t)rs[208], r.op_a = (uint8_t)rs[209];
    r.write_mask[0] = (uint8_t)rs[168], r.write_mask[1] = (uint8_t)rs[190], r.write_mask[2] = (uint8_t)rs[191],
    r.write_mask[3] = (uint8_t)rs[192];
    r.alpha_func = rs[15] ? (uint8_t)rs[25] : 0;
    r.alpha_ref = rs[24];
    r.cull = (uint8_t)rs[22], r.fill = (uint8_t)rs[8];
    r.scissor = rs[174] != 0, r.srgb_write = rs[194] != 0;
    r.blend_factor = rs[193];
    r.depth_bias = u2f(rs[195]), r.slope_bias = u2f(rs[175]);
    return r;
}

static uint32_t verts_for(uint32_t prim, uint32_t count)
{
    switch (prim)
    {
    case 1: return count;
    case 2: return count * 2;
    case 3: return count + 1;
    case 4: return count * 3;
    case 5: case 6: return count + 2;
    default: return 0;
    }
}

/* Fills a draw from the device state. vfirst/vcount: the vertex range the draw reads (for copying
 * dynamic streams), base: the vertex index 0 maps to. */
static int prepare(G9Draw* d, uint32_t vfirst, uint32_t vcount)
{
    State* s = &g_dev.st;
    memset(d, 0, sizeof *d);
    Obj* vs = obj(s->vs);
    Obj* ps = obj(s->ps);
    if (!vs || !ps)
    {
        if (!g_ff_warned++)
            rt_log("[d3d9] a fixed-function draw (vs %08x, ps %08x): not drawn (this message once)\n", s->vs, s->ps);
        return 0;
    }
    d->vs = vs->shader, d->ps = ps->shader;
    Obj* decl = obj(s->decl);
    Elem9 fvf_el[G9_MAX_ELEMS];
    const Elem9* el = NULL;
    uint32_t nel = 0;
    if (decl)
        el = decl->el, nel = decl->nel;
    else if (s->fvf)
    {
        fvf_decl(s->fvf, fvf_el, &nel);
        el = fvf_el;
    }
    for (uint32_t i = 0; i < nel && i < G9_MAX_ELEMS; ++i)
        d->el[i] = (G9Elem){ (uint8_t)el[i].stream, el[i].type, el[i].usage, el[i].index, el[i].offset, 0 };
    d->nel = nel;
    uint32_t instances = 1;
    if ((s->stream[0].freq & 0x40000000u) && (s->stream[0].freq & 0x3FFFFFFFu)) /* INDEXEDDATA | n */
        instances = s->stream[0].freq & 0x3FFFFFFFu;
    d->instances = instances;
    for (uint32_t i = 0; i < G9_MAX_STREAMS; ++i)
    {
        const Stream9* st = &s->stream[i];
        Obj* vb = obj(st->vb);
        if (!vb || !st->stride)
            continue;
        G9Stream* gs = &d->stream[i];
        gs->stride = st->stride;
        gs->freq = (st->freq & 0x80000000u) ? st->freq : 0;
        if (vb->gbuf)
        {
            gs->buf = vb->gbuf;
            gs->offset = st->offset;
            /* a misaligned offset goes through the ring: give the bytes too */
            gs->data = GUEST_PTR(vb->mem + st->offset);
            gs->size = vb->size > st->offset ? vb->size - st->offset : 0;
        }
        else
        {
            /* dynamic: copy just what this draw reads */
            uint32_t first = gs->freq ? 0 : vfirst, n = gs->freq ? instances : vcount;
            uint32_t start = st->offset + first * st->stride;
            uint32_t size = n * st->stride;
            if (start > vb->size)
                start = vb->size;
            if (start + size > vb->size)
                size = vb->size - start;
            gs->data = GUEST_PTR(vb->mem + start);
            gs->size = size;
            gs->offset = 0;
        }
    }
    d->vsc = &s->vsc[0][0], d->vsi = &s->vsi[0][0], d->vsb = s->vsb;
    d->psc = &s->psc[0][0], d->psi = &s->psi[0][0], d->psb = s->psb;
    for (int i = 0; i < NSAMP; ++i)
    {
        Obj* t = obj(s->tex[i]);
        if (t)
            d->tex[i] = t->kind == O_SURFACE ? NULL : t->gpu;
        d->samp[i] = sampler_of(s->ss[i]);
    }
    d->rs = states();
    d->vp = viewport();
    memcpy(d->scissor, s->scissor, sizeof d->scissor);
    apply_targets();
    return 1;
}

/* for dynamic streams the copied range starts at vertex vfirst: rebase the draw on it */
static void rebase_dynamic(G9Draw* d, uint32_t vfirst, int indexed)
{
    int dynamic = 0;
    for (uint32_t i = 0; i < G9_MAX_STREAMS; ++i)
        if (d->stream[i].stride && !d->stream[i].buf && !d->stream[i].freq)
            dynamic = 1;
    if (!dynamic)
        return;
    /* static streams are bound from vertex 0: give them the same base as the dynamic copies */
    for (uint32_t i = 0; i < G9_MAX_STREAMS; ++i)
        if (d->stream[i].buf && !d->stream[i].freq)
        {
            d->stream[i].offset += vfirst * d->stream[i].stride;
            d->stream[i].data = (const uint8_t*)d->stream[i].data + vfirst * d->stream[i].stride;
            if (d->stream[i].size > vfirst * d->stream[i].stride)
                d->stream[i].size -= vfirst * d->stream[i].stride;
        }
    if (indexed)
        d->base_vertex -= (int32_t)vfirst;
    else
        d->first -= vfirst;
}

/* DrawPrimitive(type, start vertex, count) */
static void Dev_DrawPrimitive(Guest* g)
{
    G9Draw d;
    uint32_t n = verts_for(ARG(1), ARG(3));
    if (prepare(&d, ARG(2), n))
    {
        d.prim = ARG(1), d.count = ARG(3), d.first = ARG(2);
        rebase_dynamic(&d, ARG(2), 0);
        g9_draw(&d);
    }
    RET(D3D_OK, 4);
}

/* DrawIndexedPrimitive(type, base vertex, min index, num vertices, start index, count) */
static void Dev_DrawIndexedPrimitive(Guest* g)
{
    G9Draw d;
    int32_t base = (int32_t)ARG(2);
    uint32_t minv = ARG(3), nv = ARG(4);
    Obj* ib = obj(g_dev.st.indices);
    if (!ib)
        RET(D3DERR_INVALIDCALL, 7);
    uint32_t vfirst = (uint32_t)(base + (int32_t)minv);
    if (prepare(&d, vfirst, nv))
    {
        d.prim = ARG(1), d.count = ARG(6), d.first = ARG(5);
        d.index_size = ib->format == FMT_INDEX32 ? 4 : 2;
        d.ibuf = ib->gbuf;
        d.idata = GUEST_PTR(ib->mem);
        d.base_vertex = base;
        rebase_dynamic(&d, vfirst, 1);
        g9_draw(&d);
    }
    RET(D3D_OK, 7);
}

/* DrawPrimitiveUP(type, count, data, stride): stream 0 from the caller's memory, then unset */
static void Dev_DrawPrimitiveUP(Guest* g)
{
    G9Draw d;
    uint32_t n = verts_for(ARG(1), ARG(2));
    State* s = &g_dev.st;
    bind(&s->stream[0].vb, 0);
    s->stream[0].offset = 0, s->stream[0].stride = 0;
    if (prepare(&d, 0, n))
    {
        d.stream[0].stride = ARG(4);
        d.stream[0].data = GUEST_PTR(ARG(3));
        d.stream[0].size = n * ARG(4);
        d.prim = ARG(1), d.count = ARG(2), d.first = 0;
        g9_draw(&d);
    }
    RET(D3D_OK, 5);
}

/* DrawIndexedPrimitiveUP(type, min index, num vertices, count, indices, index format, data, stride) */
static void Dev_DrawIndexedPrimitiveUP(Guest* g)
{
    G9Draw d;
    State* s = &g_dev.st;
    bind(&s->stream[0].vb, 0);
    s->stream[0].offset = 0, s->stream[0].stride = 0;
    bind(&s->indices, 0);
    uint32_t minv = ARG(2), nv = ARG(3), stride = ARG(8);
    if (prepare(&d, 0, minv + nv))
    {
        d.stream[0].stride = stride;
        d.stream[0].data = GUEST_PTR(ARG(7));
        d.stream[0].size = (minv + nv) * stride;
        d.prim = ARG(1), d.count = ARG(4), d.first = 0;
        d.index_size = ARG(6) == FMT_INDEX32 ? 4 : 2;
        d.idata = GUEST_PTR(ARG(5));
        g9_draw(&d);
    }
    RET(D3D_OK, 9);
}

static void Dev_ProcessVertices(Guest* g) { RET(D3DERR_INVALIDCALL, 7); }
static void Dev_DrawRectPatch(Guest* g) { RET(D3DERR_INVALIDCALL, 4); }
static void Dev_DrawTriPatch(Guest* g) { RET(D3DERR_INVALIDCALL, 4); }
static void Dev_DeletePatch(Guest* g) { RET(D3D_OK, 2); }

/* --- state blocks ------------------------------------------------------------------------------- */

static void state_apply(State* to, const State* from, const Mask* m)
{
    for (int i = 0; i < NRS; ++i)
        if (!m || m->rs[i]) to->rs[i] = from->rs[i];
    for (int i = 0; i < NSAMP; ++i)
        for (int k = 0; k < NSS; ++k)
            if (!m || m->ss[i][k]) to->ss[i][k] = from->ss[i][k];
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < NTSS; ++k)
            if (!m || m->tss[i][k]) to->tss[i][k] = from->tss[i][k];
    for (int i = 0; i < NSAMP; ++i)
        if (!m || m->tex[i]) bind(&to->tex[i], from->tex[i]);
    for (int i = 0; i < G9_MAX_STREAMS; ++i)
        if (!m || m->stream[i])
        {
            bind(&to->stream[i].vb, from->stream[i].vb);
            to->stream[i].offset = from->stream[i].offset, to->stream[i].stride = from->stream[i].stride;
        }
    if (!m || m->indices) bind(&to->indices, from->indices);
    if (!m || m->decl) bind(&to->decl, from->decl);
    if (!m || m->fvf) to->fvf = from->fvf;
    if (!m || m->vs) bind(&to->vs, from->vs);
    if (!m || m->ps) bind(&to->ps, from->ps);
    for (int i = 0; i < G9_VS_FLOATS; ++i)
        if (!m || m->vsc[i]) memcpy(to->vsc[i], from->vsc[i], 16);
    for (int i = 0; i < G9_PS_FLOATS; ++i)
        if (!m || m->psc[i]) memcpy(to->psc[i], from->psc[i], 16);
    for (int i = 0; i < 16; ++i)
    {
        if (!m || m->vsi[i]) memcpy(to->vsi[i], from->vsi[i], 16);
        if (!m || m->psi[i]) memcpy(to->psi[i], from->psi[i], 16);
        if (!m || m->vsb[i]) to->vsb[i] = from->vsb[i];
        if (!m || m->psb[i]) to->psb[i] = from->psb[i];
    }
    if (!m || m->vp) memcpy(to->vp, from->vp, sizeof to->vp);
    if (!m || m->scissor) memcpy(to->scissor, from->scissor, sizeof to->scissor);
    if (!m || m->material) memcpy(to->material, from->material, sizeof to->material);
    for (int i = 0; i < NTS; ++i)
        if (!m || m->ts[i]) memcpy(to->ts[i], from->ts[i], 64);
}

/* a block's own copy of the bound objects: it holds references to them */
static void block_hold(State* b, const Mask* m, int add)
{
    uint32_t* slots[NSAMP + G9_MAX_STREAMS + 5];
    int n = 0;
    for (int i = 0; i < NSAMP; ++i)
        if (!m || m->tex[i]) slots[n++] = &b->tex[i];
    for (int i = 0; i < G9_MAX_STREAMS; ++i)
        if (!m || m->stream[i]) slots[n++] = &b->stream[i].vb;
    if (!m || m->indices) slots[n++] = &b->indices;
    if (!m || m->decl) slots[n++] = &b->decl;
    if (!m || m->vs) slots[n++] = &b->vs;
    if (!m || m->ps) slots[n++] = &b->ps;
    for (int i = 0; i < n; ++i)
        if (*slots[i])
            add ? obj_addref(*slots[i]) : obj_release(*slots[i]);
}

static void Dev_CreateStateBlock(Guest* g)
{
    uint32_t p = obj_new(O_SB);
    Obj* b = obj(p);
    b->sb = (State*)malloc(sizeof(State));
    *b->sb = g_dev.st; /* every state (D3DSBT_ALL; the subsets capture a superset) */
    block_hold(b->sb, NULL, 1);
    wr32(ARG(2), p);
    RET(D3D_OK, 3);
}
static void Dev_BeginStateBlock(Guest* g)
{
    uint32_t p = obj_new(O_SB);
    Obj* b = obj(p);
    b->sb = (State*)calloc(1, sizeof(State));
    b->sbm = (Mask*)calloc(1, sizeof(Mask));
    g_dev.recording = p;
    RET(D3D_OK, 1);
}
static void Dev_EndStateBlock(Guest* g)
{
    Obj* b = obj(g_dev.recording);
    if (b)
        block_hold(b->sb, b->sbm, 1);
    wr32(ARG(1), g_dev.recording);
    g_dev.recording = 0;
    RET(D3D_OK, 2);
}
static void SB_Capture(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (b && b->sb)
    {
        block_hold(b->sb, b->sbm, 0);
        State tmp = *b->sb;
        memset(b->sb, 0, sizeof *b->sb);
        state_apply(b->sb, &g_dev.st, b->sbm); /* binds: references taken here */
        (void)tmp;
    }
    RET(D3D_OK, 1);
}
static void SB_Apply(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (b && b->sb)
        state_apply(&g_dev.st, b->sb, b->sbm);
    RET(D3D_OK, 1);
}

/* --- queries ------------------------------------------------------------------------------------ */

/* CreateQuery(type, IDirect3DQuery9**): with no out pointer, whether the type is supported */
static void Dev_CreateQuery(Guest* g)
{
    uint32_t type = ARG(1);
    int ok = type == 8 /* EVENT */ || type == 9 /* OCCLUSION */ || type == 10 /* TIMESTAMP */ ||
             type == 11 || type == 12; /* TIMESTAMPDISJOINT, TIMESTAMPFREQ */
    if (!ok)
        RET(D3DERR_NOTAVAILABLE, 3);
    if (!ARG(2))
        RET(D3D_OK, 3);
    uint32_t p = obj_new(O_QUERY);
    obj(p)->qtype = type;
    wr32(ARG(2), p);
    RET(D3D_OK, 3);
}
static void Query_GetType(Guest* g) { Obj* q = obj(ARG(0)); RET(q ? q->qtype : 0, 1); }
static void Query_GetDataSize(Guest* g)
{
    Obj* q = obj(ARG(0));
    uint32_t t = q ? q->qtype : 0;
    RET(t == 8 ? 4 : t == 9 ? 4 : t == 10 ? 8 : t == 11 ? 4 : t == 12 ? 8 : 0, 1);
}
static void Query_Issue(Guest* g) { Obj* q = obj(ARG(0)); if (q) q->issued = 1; RET(D3D_OK, 2); }
/* GetData(data, size, flags): results are always ready - an event has passed, every occlusion
 * query saw its object (the GPU work is not waited for) */
static void Query_GetData(Guest* g)
{
    Obj* q = obj(ARG(0));
    uint32_t p = ARG(1), size = ARG(2);
    if (!q)
        RET(D3DERR_INVALIDCALL, 4);
    uint64_t v = q->qtype == 8 ? 1 : q->qtype == 9 ? 100000 : q->qtype == 10 ? rt_monotonic_ns() : q->qtype == 12 ? 1000000000ull : 0;
    if (p && size)
        memcpy(GUEST_PTR(p), &v, size < 8 ? size : 8);
    RET(D3D_OK, 4);
}

/* --- swap chain -------------------------------------------------------------------------------- */

static void Swap_Present(Guest* g) { present(); RET(D3D_OK, 6); }
static void Swap_GetBackBuffer(Guest* g)
{
    obj_addref(g_dev.backbuffer);
    wr32(ARG(3), g_dev.backbuffer);
    RET(D3D_OK, 4);
}
static void Swap_GetRasterStatus(Guest* g) { wr32(ARG(1), 0), wr32(ARG(1) + 4, 0); RET(D3D_OK, 2); }
static void Swap_GetDisplayMode(Guest* g)
{
    uint32_t w, h, hz, p = ARG(1);
    user32_desktop_mode(&w, &h, &hz);
    wr32(p, w), wr32(p + 4, h), wr32(p + 8, hz ? hz : 60), wr32(p + 12, FMT_X8R8G8B8);
    RET(D3D_OK, 2);
}
static void Swap_GetPresentParameters(Guest* g)
{
    for (int i = 0; i < 14; ++i)
        wr32(ARG(1) + 4u * i, g_dev.pp[i]);
    RET(D3D_OK, 2);
}
static void Swap_GetFrontBufferData(Guest* g) { RET(D3D_OK, 2); }

/* --- Direct3DCreate9 ----------------------------------------------------------------------------- */

static void make_vtbls(void);

static void sh_Direct3DCreate9(Guest* g)
{
    static uint32_t d3d;
    if (!g_vtbl[O_D3D])
        make_vtbls();
    if (!d3d)
        d3d = obj_new(O_D3D);
    else
        obj_addref(d3d);
    rt_log("[d3d9] Direct3DCreate9(%u)\n", ARG(0));
    RET(d3d, 1);
}

/* --- vtables (D3D9 ABI order) -------------------------------------------------------------------- */

#define UNK "QueryInterface", "AddRef", "Release"
#define RES UNK, "GetDevice", "SetPrivateData", "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority", \
            "PreLoad", "GetType"
#define BASETEX RES, "SetLOD", "GetLOD", "GetLevelCount", "SetAutoGenFilterType", "GetAutoGenFilterType", "GenerateMipSubLevels"

static const char* const kD3D[] = { UNK, "RegisterSoftwareDevice", "GetAdapterCount", "GetAdapterIdentifier",
    "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType", "CheckDeviceFormat",
    "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "CheckDeviceFormatConversion", "GetDeviceCaps",
    "GetAdapterMonitor", "CreateDevice", NULL };
static const char* const kDevice[] = { UNK, "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources",
    "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters", "SetCursorProperties",
    "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains", "Reset",
    "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp",
    "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer",
    "CreateRenderTarget", "CreateDepthStencilSurface", "UpdateSurface", "UpdateTexture", "GetRenderTargetData",
    "GetFrontBufferData", "StretchRect", "ColorFill", "CreateOffscreenPlainSurface", "SetRenderTarget",
    "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear",
    "SetTransform", "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial",
    "SetLight", "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState",
    "GetRenderState", "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette",
    "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
    "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetVertexShaderConstantI",
    "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource",
    "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader",
    "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF", "GetPixelShaderConstantF",
    "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB",
    "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery", NULL };
static const char* const kTexture[] = { BASETEX, "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kCube[] = { BASETEX, "GetLevelDesc", "GetCubeMapSurface", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kVolTex[] = { BASETEX, "GetLevelDesc", "GetVolumeLevel", "LockBox", "UnlockBox", "AddDirtyBox", NULL };
static const char* const kSurface[] = { RES, "GetContainer", "GetDesc", "LockRect", "UnlockRect", "GetDC", "ReleaseDC", NULL };
static const char* const kVolume[] = { UNK, "GetDevice", "SetPrivateData", "GetPrivateData", "FreePrivateData",
    "GetContainer", "GetDesc", "LockBox", "UnlockBox", NULL };
static const char* const kVB[] = { RES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kDecl[] = { UNK, "GetDevice", "GetDeclaration", NULL };
static const char* const kShader[] = { UNK, "GetDevice", "GetFunction", NULL };
static const char* const kQuery[] = { UNK, "GetDevice", "GetType", "GetDataSize", "Issue", "GetData", NULL };
static const char* const kSB[] = { UNK, "GetDevice", "Capture", "Apply", NULL };
static const char* const kSwap[] = { UNK, "Present", "GetFrontBufferData", "GetBackBuffer", "GetRasterStatus",
    "GetDisplayMode", "GetDevice", "GetPresentParameters", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("d3d9.dll", full));
    }
    return v;
}

static void make_vtbls(void)
{
    g_vtbl[O_D3D] = make_vtbl("IDirect3D9", kD3D);
    g_vtbl[O_DEVICE] = make_vtbl("IDirect3DDevice9", kDevice);
    g_vtbl[O_TEXTURE] = make_vtbl("IDirect3DTexture9", kTexture);
    g_vtbl[O_CUBE] = make_vtbl("IDirect3DCubeTexture9", kCube);
    g_vtbl[O_VOLTEX] = make_vtbl("IDirect3DVolumeTexture9", kVolTex);
    g_vtbl[O_SURFACE] = make_vtbl("IDirect3DSurface9", kSurface);
    g_vtbl[O_VOLUME] = make_vtbl("IDirect3DVolume9", kVolume);
    g_vtbl[O_VB] = make_vtbl("IDirect3DVertexBuffer9", kVB);
    g_vtbl[O_IB] = make_vtbl("IDirect3DIndexBuffer9", kVB);
    g_vtbl[O_DECL] = make_vtbl("IDirect3DVertexDeclaration9", kDecl);
    g_vtbl[O_VS] = make_vtbl("IDirect3DVertexShader9", kShader);
    g_vtbl[O_PS] = make_vtbl("IDirect3DPixelShader9", kShader);
    g_vtbl[O_QUERY] = make_vtbl("IDirect3DQuery9", kQuery);
    g_vtbl[O_SB] = make_vtbl("IDirect3DStateBlock9", kSB);
    g_vtbl[O_SWAP] = make_vtbl("IDirect3DSwapChain9", kSwap);
}

#define M(iface, name, fn) { "d3d9.dll", iface "::" name, fn }
#define RESOURCE(iface)                                                                            \
    M(iface, "QueryInterface", Unknown_QueryInterface), M(iface, "AddRef", Unknown_AddRef),          \
    M(iface, "Release", Unknown_Release), M(iface, "GetDevice", Any_GetDevice),                     \
    M(iface, "SetPrivateData", Any_SetPrivateData), M(iface, "GetPrivateData", Any_GetPrivateData), \
    M(iface, "FreePrivateData", Any_FreePrivateData), M(iface, "SetPriority", Resource_SetPriority), \
    M(iface, "GetPriority", Resource_GetPriority), M(iface, "PreLoad", Resource_PreLoad),           \
    M(iface, "GetType", Resource_GetType)
#define BASETEXTURE(iface)                                                                         \
    RESOURCE(iface), M(iface, "SetLOD", BaseTex_SetLOD), M(iface, "GetLOD", BaseTex_GetLOD),         \
    M(iface, "GetLevelCount", BaseTex_GetLevelCount), M(iface, "SetAutoGenFilterType", BaseTex_SetAutoGenFilterType), \
    M(iface, "GetAutoGenFilterType", BaseTex_GetAutoGenFilterType),                                 \
    M(iface, "GenerateMipSubLevels", BaseTex_GenerateMipSubLevels)
#define SIMPLE(iface)                                                                              \
    M(iface, "QueryInterface", Unknown_QueryInterface), M(iface, "AddRef", Unknown_AddRef),          \
    M(iface, "Release", Unknown_Release), M(iface, "GetDevice", Any_GetDevice)
#define DEV(name) M("IDirect3DDevice9", #name, Dev_##name)

static const ShimDef D3D9[] = {
    { "d3d9.dll", "Direct3DCreate9", sh_Direct3DCreate9 },
    M("IDirect3D9", "QueryInterface", Unknown_QueryInterface),
    M("IDirect3D9", "AddRef", Unknown_AddRef),
    M("IDirect3D9", "Release", Unknown_Release),
    M("IDirect3D9", "RegisterSoftwareDevice", IDirect3D9_RegisterSoftwareDevice),
    M("IDirect3D9", "GetAdapterCount", IDirect3D9_GetAdapterCount),
    M("IDirect3D9", "GetAdapterIdentifier", IDirect3D9_GetAdapterIdentifier),
    M("IDirect3D9", "GetAdapterModeCount", IDirect3D9_GetAdapterModeCount),
    M("IDirect3D9", "EnumAdapterModes", IDirect3D9_EnumAdapterModes),
    M("IDirect3D9", "GetAdapterDisplayMode", IDirect3D9_GetAdapterDisplayMode),
    M("IDirect3D9", "CheckDeviceType", IDirect3D9_CheckDeviceType),
    M("IDirect3D9", "CheckDeviceFormat", IDirect3D9_CheckDeviceFormat),
    M("IDirect3D9", "CheckDeviceMultiSampleType", IDirect3D9_CheckDeviceMultiSampleType),
    M("IDirect3D9", "CheckDepthStencilMatch", IDirect3D9_CheckDepthStencilMatch),
    M("IDirect3D9", "CheckDeviceFormatConversion", IDirect3D9_CheckDeviceFormatConversion),
    M("IDirect3D9", "GetDeviceCaps", IDirect3D9_GetDeviceCaps),
    M("IDirect3D9", "GetAdapterMonitor", IDirect3D9_GetAdapterMonitor),
    M("IDirect3D9", "CreateDevice", IDirect3D9_CreateDevice),
    M("IDirect3DDevice9", "QueryInterface", Unknown_QueryInterface),
    M("IDirect3DDevice9", "AddRef", Unknown_AddRef),
    M("IDirect3DDevice9", "Release", Unknown_Release),
    DEV(TestCooperativeLevel), DEV(GetAvailableTextureMem), DEV(EvictManagedResources), DEV(GetDirect3D),
    DEV(GetDeviceCaps), DEV(GetDisplayMode), DEV(GetCreationParameters), DEV(SetCursorProperties),
    DEV(SetCursorPosition), DEV(ShowCursor), DEV(GetSwapChain), DEV(GetNumberOfSwapChains), DEV(Reset),
    DEV(Present), DEV(GetBackBuffer), DEV(GetRasterStatus), DEV(SetDialogBoxMode), DEV(SetGammaRamp),
    DEV(GetGammaRamp), DEV(CreateTexture), DEV(CreateVolumeTexture), DEV(CreateCubeTexture),
    DEV(CreateVertexBuffer), DEV(CreateIndexBuffer), DEV(CreateRenderTarget), DEV(CreateDepthStencilSurface),
    DEV(UpdateSurface), DEV(UpdateTexture), DEV(GetRenderTargetData), DEV(GetFrontBufferData), DEV(StretchRect),
    DEV(ColorFill), DEV(CreateOffscreenPlainSurface), DEV(SetRenderTarget), DEV(GetRenderTarget),
    DEV(SetDepthStencilSurface), DEV(GetDepthStencilSurface), DEV(BeginScene), DEV(EndScene), DEV(Clear),
    DEV(SetTransform), DEV(GetTransform), DEV(MultiplyTransform), DEV(SetViewport), DEV(GetViewport),
    DEV(SetMaterial), DEV(GetMaterial), DEV(SetLight), DEV(GetLight), DEV(LightEnable), DEV(GetLightEnable),
    DEV(SetClipPlane), DEV(GetClipPlane), DEV(SetRenderState), DEV(GetRenderState), DEV(CreateStateBlock),
    DEV(BeginStateBlock), DEV(EndStateBlock), DEV(SetClipStatus), DEV(GetClipStatus), DEV(GetTexture),
    DEV(SetTexture), DEV(GetTextureStageState), DEV(SetTextureStageState), DEV(GetSamplerState),
    DEV(SetSamplerState), DEV(ValidateDevice), DEV(SetPaletteEntries), DEV(GetPaletteEntries),
    DEV(SetCurrentTexturePalette), DEV(GetCurrentTexturePalette), DEV(SetScissorRect), DEV(GetScissorRect),
    DEV(SetSoftwareVertexProcessing), DEV(GetSoftwareVertexProcessing), DEV(SetNPatchMode), DEV(GetNPatchMode),
    DEV(DrawPrimitive), DEV(DrawIndexedPrimitive), DEV(DrawPrimitiveUP), DEV(DrawIndexedPrimitiveUP),
    DEV(ProcessVertices), DEV(CreateVertexDeclaration), DEV(SetVertexDeclaration), DEV(GetVertexDeclaration),
    DEV(SetFVF), DEV(GetFVF), DEV(CreateVertexShader), DEV(SetVertexShader), DEV(GetVertexShader),
    DEV(SetVertexShaderConstantF), DEV(GetVertexShaderConstantF), DEV(SetVertexShaderConstantI),
    DEV(GetVertexShaderConstantI), DEV(SetVertexShaderConstantB), DEV(GetVertexShaderConstantB),
    DEV(SetStreamSource), DEV(GetStreamSource), DEV(SetStreamSourceFreq), DEV(GetStreamSourceFreq),
    DEV(SetIndices), DEV(GetIndices), DEV(CreatePixelShader), DEV(SetPixelShader), DEV(GetPixelShader),
    DEV(SetPixelShaderConstantF), DEV(GetPixelShaderConstantF), DEV(SetPixelShaderConstantI),
    DEV(GetPixelShaderConstantI), DEV(SetPixelShaderConstantB), DEV(GetPixelShaderConstantB), DEV(DrawRectPatch),
    DEV(DrawTriPatch), DEV(DeletePatch), DEV(CreateQuery),
    BASETEXTURE("IDirect3DTexture9"),
    M("IDirect3DTexture9", "GetLevelDesc", Tex_GetLevelDesc),
    M("IDirect3DTexture9", "GetSurfaceLevel", Tex_GetSurfaceLevel),
    M("IDirect3DTexture9", "LockRect", Tex_LockRect),
    M("IDirect3DTexture9", "UnlockRect", Tex_UnlockRect),
    M("IDirect3DTexture9", "AddDirtyRect", Tex_AddDirtyRect),
    BASETEXTURE("IDirect3DCubeTexture9"),
    M("IDirect3DCubeTexture9", "GetLevelDesc", Cube_GetLevelDesc),
    M("IDirect3DCubeTexture9", "GetCubeMapSurface", Cube_GetCubeMapSurface),
    M("IDirect3DCubeTexture9", "LockRect", Cube_LockRect),
    M("IDirect3DCubeTexture9", "UnlockRect", Cube_UnlockRect),
    M("IDirect3DCubeTexture9", "AddDirtyRect", Cube_AddDirtyRect),
    BASETEXTURE("IDirect3DVolumeTexture9"),
    M("IDirect3DVolumeTexture9", "GetLevelDesc", VolTex_GetLevelDesc),
    M("IDirect3DVolumeTexture9", "GetVolumeLevel", VolTex_GetVolumeLevel),
    M("IDirect3DVolumeTexture9", "LockBox", VolTex_LockBox),
    M("IDirect3DVolumeTexture9", "UnlockBox", VolTex_UnlockBox),
    M("IDirect3DVolumeTexture9", "AddDirtyBox", VolTex_AddDirtyBox),
    RESOURCE("IDirect3DSurface9"),
    M("IDirect3DSurface9", "GetContainer", Surface_GetContainer),
    M("IDirect3DSurface9", "GetDesc", Surface_GetDesc),
    M("IDirect3DSurface9", "LockRect", Surface_LockRect),
    M("IDirect3DSurface9", "UnlockRect", Surface_UnlockRect),
    M("IDirect3DSurface9", "GetDC", Surface_GetDC),
    M("IDirect3DSurface9", "ReleaseDC", Surface_ReleaseDC),
    SIMPLE("IDirect3DVolume9"),
    M("IDirect3DVolume9", "SetPrivateData", Any_SetPrivateData),
    M("IDirect3DVolume9", "GetPrivateData", Any_GetPrivateData),
    M("IDirect3DVolume9", "FreePrivateData", Any_FreePrivateData),
    M("IDirect3DVolume9", "GetContainer", Surface_GetContainer),
    M("IDirect3DVolume9", "GetDesc", Volume_GetDesc),
    M("IDirect3DVolume9", "LockBox", Volume_LockBox),
    M("IDirect3DVolume9", "UnlockBox", Volume_UnlockBox),
    RESOURCE("IDirect3DVertexBuffer9"),
    M("IDirect3DVertexBuffer9", "Lock", Buffer_Lock),
    M("IDirect3DVertexBuffer9", "Unlock", Buffer_Unlock),
    M("IDirect3DVertexBuffer9", "GetDesc", Buffer_GetDesc),
    RESOURCE("IDirect3DIndexBuffer9"),
    M("IDirect3DIndexBuffer9", "Lock", Buffer_Lock),
    M("IDirect3DIndexBuffer9", "Unlock", Buffer_Unlock),
    M("IDirect3DIndexBuffer9", "GetDesc", Buffer_GetDesc),
    SIMPLE("IDirect3DVertexDeclaration9"),
    M("IDirect3DVertexDeclaration9", "GetDeclaration", Decl_GetDeclaration),
    SIMPLE("IDirect3DVertexShader9"),
    M("IDirect3DVertexShader9", "GetFunction", Shader_GetFunction),
    SIMPLE("IDirect3DPixelShader9"),
    M("IDirect3DPixelShader9", "GetFunction", Shader_GetFunction),
    SIMPLE("IDirect3DQuery9"),
    M("IDirect3DQuery9", "GetType", Query_GetType),
    M("IDirect3DQuery9", "GetDataSize", Query_GetDataSize),
    M("IDirect3DQuery9", "Issue", Query_Issue),
    M("IDirect3DQuery9", "GetData", Query_GetData),
    SIMPLE("IDirect3DStateBlock9"),
    M("IDirect3DStateBlock9", "Capture", SB_Capture),
    M("IDirect3DStateBlock9", "Apply", SB_Apply),
    M("IDirect3DSwapChain9", "QueryInterface", Unknown_QueryInterface),
    M("IDirect3DSwapChain9", "AddRef", Unknown_AddRef),
    M("IDirect3DSwapChain9", "Release", Unknown_Release),
    M("IDirect3DSwapChain9", "Present", Swap_Present),
    M("IDirect3DSwapChain9", "GetFrontBufferData", Swap_GetFrontBufferData),
    M("IDirect3DSwapChain9", "GetBackBuffer", Swap_GetBackBuffer),
    M("IDirect3DSwapChain9", "GetRasterStatus", Swap_GetRasterStatus),
    M("IDirect3DSwapChain9", "GetDisplayMode", Swap_GetDisplayMode),
    M("IDirect3DSwapChain9", "GetDevice", Any_GetDevice),
    M("IDirect3DSwapChain9", "GetPresentParameters", Swap_GetPresentParameters),
    { NULL, NULL, NULL },
};

void d3d9_init(void) { thunk_register(D3D9); }

/* For D3DX (d3dx9.c): sets shader constants as Set{Vertex,Pixel}ShaderConstant{B,I,F} would.
 * set: 0 bool, 1 int4, 2 float4. */
void d3d9_set_constants(int pixel, int set, uint32_t start, const void* data, uint32_t count)
{
    State* s = &g_dev.st;
    void* base = set == 2 ? (pixel ? (void*)s->psc : (void*)s->vsc) : set == 1 ? (pixel ? (void*)s->psi : (void*)s->vsi)
                                                                               : (pixel ? (void*)s->psb : (void*)s->vsb);
    uint32_t limit = set == 2 ? (pixel ? G9_PS_FLOATS : G9_VS_FLOATS) : 16, elem = set == 0 ? 4 : 16;
    if (start >= limit)
        return;
    if (start + count > limit)
        count = limit - start;
    memcpy((uint8_t*)base + (size_t)start * elem, data, (size_t)count * elem);
}
