/* The Direct3D 9 back end on Metal (gfx9.h). Manual reference counting (built without ARC).
 *
 * Frames: up to FRAMES command buffers in flight; each frame has an upload ring (vertex data the
 * draws copy, index data, texture uploads' staging) that the GPU reads while the next frame
 * records into another. Everything a frame does goes into its one command buffer, in call order:
 * texture uploads and copies end the render pass and encode a blit, so the GPU sees them where
 * the game made them.
 *
 * Render passes begin lazily, at the first draw or clear after the targets change; a clear at the
 * start of a pass that covers the whole target becomes the pass's load action, any other is drawn
 * as a quad.
 *
 * Pipelines are cached by everything Metal bakes into one: the two shaders (each translated per
 * variant: the vertex layout's integer attributes, the outputs the pixel shader reads, alpha test,
 * depth samplers), the vertex layout, the target formats and the blend states.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include "sdl_main.h"
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx9.h"
#include "sm_msl.h"

void rt_log(const char* fmt, ...);

#define FRAMES 3
#define RING_SIZE (64u << 20)

/* D3DFORMAT */
enum
{
    F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25, F_A4R4G4B4 = 26, F_A8 = 28,
    F_X4R4G4B4 = 30, F_A2B10G10R10 = 31, F_A8B8G8R8 = 32, F_X8B8G8R8 = 33, F_G16R16 = 34, F_A2R10G10B10 = 35,
    F_A16B16G16R16 = 36, F_L8 = 50, F_A8L8 = 51, F_A4L4 = 52, F_V8U8 = 60, F_Q8W8V8U8 = 63, F_V16U16 = 64,
    F_D16_LOCKABLE = 70, F_D32 = 71, F_D15S1 = 73, F_D24S8 = 75, F_D24X8 = 77, F_D24X4S4 = 79, F_D16 = 80, F_L16 = 81,
    F_D32F_LOCKABLE = 82, F_D24FS8 = 83, F_R16F = 111, F_G16R16F = 112, F_A16B16G16R16F = 113, F_R32F = 114,
    F_G32R32F = 115, F_A32B32G32R32F = 116,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define F_DXT1 FOURCC('D', 'X', 'T', '1')
#define F_DXT2 FOURCC('D', 'X', 'T', '2')
#define F_DXT3 FOURCC('D', 'X', 'T', '3')
#define F_DXT4 FOURCC('D', 'X', 'T', '4')
#define F_DXT5 FOURCC('D', 'X', 'T', '5')
#define F_ATI1 FOURCC('A', 'T', 'I', '1')
#define F_ATI2 FOURCC('A', 'T', 'I', '2')
#define F_INTZ FOURCC('I', 'N', 'T', 'Z')
#define F_DF24 FOURCC('D', 'F', '2', '4')
#define F_DF16 FOURCC('D', 'F', '1', '6')
#define F_NULL FOURCC('N', 'U', 'L', 'L')

enum { CONV_NONE, CONV_565, CONV_X555, CONV_1555, CONV_4444, CONV_X444, CONV_A4L4, CONV_X8 };

typedef struct Fmt
{
    MTLPixelFormat pf;
    int conv;          /* the bytes D3D writes are converted on upload (16-bit formats to BGRA8) */
    uint32_t block;    /* bytes per 4x4 block for compressed formats, else 0 */
    uint32_t texel;    /* bytes per texel as D3D lays them out */
    uint32_t mtexel;   /* ... and as Metal holds them */
    int swizzle;       /* 0 none, 1 L (rrr1), 2 AL (rrrg), 3 A (000r) */
    int depth;         /* 1 depth(-stencil) */
    int depth_read;    /* sampled for its values (INTZ, DF24), not compared */
} Fmt;

static Fmt fmt_of(uint32_t f)
{
    Fmt r = { MTLPixelFormatBGRA8Unorm, CONV_NONE, 0, 4, 4, 0, 0, 0 };
    switch (f)
    {
    case F_A8R8G8B8: case F_X8R8G8B8: break;
    case F_A8B8G8R8: case F_X8B8G8R8: r.pf = MTLPixelFormatRGBA8Unorm; break;
    case F_R5G6B5: r.conv = CONV_565, r.texel = 2; break;
    case F_X1R5G5B5: r.conv = CONV_X555, r.texel = 2; break;
    case F_A1R5G5B5: r.conv = CONV_1555, r.texel = 2; break;
    case F_A4R4G4B4: r.conv = CONV_4444, r.texel = 2; break;
    case F_X4R4G4B4: r.conv = CONV_X444, r.texel = 2; break;
    case F_A8: r.pf = MTLPixelFormatR8Unorm, r.texel = r.mtexel = 1, r.swizzle = 3; break;
    case F_L8: r.pf = MTLPixelFormatR8Unorm, r.texel = r.mtexel = 1, r.swizzle = 1; break;
    case F_A8L8: r.pf = MTLPixelFormatRG8Unorm, r.texel = r.mtexel = 2, r.swizzle = 2; break;
    case F_A4L4: r.conv = CONV_A4L4, r.pf = MTLPixelFormatRG8Unorm, r.texel = 1, r.mtexel = 2, r.swizzle = 2; break;
    case F_L16: r.pf = MTLPixelFormatR16Unorm, r.texel = r.mtexel = 2, r.swizzle = 1; break;
    case F_V8U8: r.pf = MTLPixelFormatRG8Snorm, r.texel = r.mtexel = 2; break;
    case F_Q8W8V8U8: r.pf = MTLPixelFormatRGBA8Snorm; break;
    case F_V16U16: r.pf = MTLPixelFormatRG16Snorm; break;
    case F_G16R16: r.pf = MTLPixelFormatRG16Unorm; break;
    case F_A16B16G16R16: r.pf = MTLPixelFormatRGBA16Unorm, r.texel = r.mtexel = 8; break;
    case F_A2B10G10R10: r.pf = MTLPixelFormatRGB10A2Unorm; break;
    case F_A2R10G10B10: r.pf = MTLPixelFormatBGR10A2Unorm; break;
    case F_R16F: r.pf = MTLPixelFormatR16Float, r.texel = r.mtexel = 2; break;
    case F_G16R16F: r.pf = MTLPixelFormatRG16Float; break;
    case F_A16B16G16R16F: r.pf = MTLPixelFormatRGBA16Float, r.texel = r.mtexel = 8; break;
    case F_R32F: r.pf = MTLPixelFormatR32Float; break;
    case F_G32R32F: r.pf = MTLPixelFormatRG32Float, r.texel = r.mtexel = 8; break;
    case F_A32B32G32R32F: r.pf = MTLPixelFormatRGBA32Float, r.texel = r.mtexel = 16; break;
    case F_DXT1: r.pf = MTLPixelFormatBC1_RGBA, r.block = 8; break;
    case F_DXT2: case F_DXT3: r.pf = MTLPixelFormatBC2_RGBA, r.block = 16; break;
    case F_DXT4: case F_DXT5: r.pf = MTLPixelFormatBC3_RGBA, r.block = 16; break;
    case F_ATI1: r.pf = MTLPixelFormatBC4_RUnorm, r.block = 8; break;
    case F_ATI2: r.pf = MTLPixelFormatBC5_RGUnorm, r.block = 16; break;
    case F_D16: case F_D16_LOCKABLE: r.pf = MTLPixelFormatDepth32Float, r.depth = 1; break;
    case F_D32: case F_D32F_LOCKABLE: case F_D24X8: r.pf = MTLPixelFormatDepth32Float, r.depth = 1; break;
    case F_D24S8: case F_D24FS8: case F_D24X4S4: case F_D15S1: r.pf = MTLPixelFormatDepth32Float_Stencil8, r.depth = 1; break;
    case F_INTZ: r.pf = MTLPixelFormatDepth32Float_Stencil8, r.depth = 1, r.depth_read = 1; break;
    case F_DF24: case F_DF16: r.pf = MTLPixelFormatDepth32Float, r.depth = 1, r.depth_read = 1; break;
    case F_NULL: r.pf = MTLPixelFormatBGRA8Unorm; break;
    default: break;
    }
    return r;
}

int g9_format_ok(uint32_t f, int use)
{
    switch (f)
    {
    case F_A8R8G8B8: case F_X8R8G8B8: case F_A8B8G8R8: case F_X8B8G8R8: case F_R5G6B5: case F_X1R5G5B5:
    case F_A1R5G5B5: case F_A4R4G4B4: case F_A2B10G10R10: case F_A2R10G10B10: case F_G16R16: case F_A16B16G16R16:
    case F_R16F: case F_G16R16F: case F_A16B16G16R16F: case F_R32F: case F_G32R32F: case F_A32B32G32R32F:
        return use != G9_USE_DEPTH;
    case F_A8: case F_L8: case F_A8L8: case F_A4L4: case F_L16: case F_V8U8: case F_Q8W8V8U8: case F_V16U16:
    case F_DXT1: case F_DXT2: case F_DXT3: case F_DXT4: case F_DXT5: case F_ATI1: case F_ATI2: case F_X4R4G4B4:
        return use == G9_USE_SAMPLE;
    case F_D16: case F_D16_LOCKABLE: case F_D32: case F_D24X8: case F_D24S8: case F_D24FS8: case F_D32F_LOCKABLE:
        return use == G9_USE_DEPTH || use == G9_USE_SAMPLE;
    case F_INTZ: case F_DF24: case F_DF16:
        return use == G9_USE_DEPTH || use == G9_USE_SAMPLE;
    case F_NULL:
        return use == G9_USE_RT;
    default:
        return 0;
    }
}

struct G9Tex
{
    id<MTLTexture> tex;
    id<MTLTexture> srgb; /* a BGRA8 texture's sRGB view, made on first use */
    int type, use;
    uint32_t fmt, w, h, d, levels;
    Fmt f;
    /* upscaling (gfx9_upscale.h): a scene target's texture is made at the render scale; w x h stay
     * the game's size and rw x rh are the texture's. full: its texture at the game's size */
    uint32_t rw, rh;
    id<MTLTexture> full;
    int scene;
};

struct G9Buf
{
    id<MTLBuffer> buf;
    uint32_t size;
};

/* A translated function or a pipeline, ready or still compiling (gfx9_metal.m "background
 * compiles"): state 0 compiling, 1 done (obj nil if it failed). */
typedef struct Compiled
{
    id obj;
    volatile int state;
} Compiled;

typedef struct Variant
{
    uint64_t key;
    Compiled* fn; /* shared through g_fns */
} Variant;

struct G9Shader
{
    uint32_t* tokens;
    uint32_t ntokens;
    SmInfo info;
    Variant* var;
    uint32_t nvar, capvar;
    uint64_t content; /* the tokens' hash (shader_variant's shared cache), 0 until needed */
};

/* --- the device ------------------------------------------------------------------------------- */

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer* g_layer;
static SDL_MetalView g_view;
static dispatch_semaphore_t g_sem;
static id<MTLCommandBuffer> g_cmd;
static id<MTLRenderCommandEncoder> g_enc;
static id<MTLBuffer> g_ring[FRAMES];
static uint32_t g_ring_used, g_frame;
static id<MTLBuffer> g_default_attr; /* (0, 0, 0, 1): what an attribute no stream supplies reads */
static id<MTLLibrary> g_util;
static id<MTLRenderPipelineState> g_present_pipe;
static id<MTLRenderPipelineState> g_text_pipe; /* the FPS counter (XIV_FPS=0 hides it) */
static id<MTLSamplerState> g_linear, g_point;

/* the targets */
static G9Tex* g_rt[G9_MAX_RT];
static uint8_t g_rt_face[G9_MAX_RT], g_rt_level[G9_MAX_RT];
static G9Tex* g_ds;
static int g_targets_changed = 1;
static uint32_t g_pass_draws;
static uint32_t g_pending_clear; /* D3DCLEAR_* to fold into the next pass's load actions */
static float g_clear_color[4], g_clear_z;
static uint32_t g_clear_stencil;

/* --- small caches: key bytes -> object ---------------------------------------------------------- */

typedef struct CacheEnt
{
    uint64_t h;
    void* key;
    uint32_t klen;
    id obj;
} CacheEnt;
typedef struct Cache
{
    CacheEnt* e;
    uint32_t n, cap;
} Cache;

static uint64_t fnv64(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static id cache_get(Cache* c, const void* key, uint32_t klen, uint64_t* hout)
{
    uint64_t h = fnv64(key, klen);
    *hout = h;
    if (!c->cap)
        return nil;
    for (uint32_t i = h % c->cap, k = 0; k < c->cap; ++k, i = (i + 1) % c->cap)
    {
        CacheEnt* e = &c->e[i];
        if (!e->key)
            return nil;
        if (e->h == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e->obj;
    }
    return nil;
}

static void cache_put(Cache* c, const void* key, uint32_t klen, uint64_t h, id obj)
{
    if ((c->n + 1) * 2 > c->cap)
    {
        Cache old = *c;
        c->cap = c->cap ? c->cap * 2 : 256;
        c->e = (CacheEnt*)calloc(c->cap, sizeof *c->e);
        c->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].key)
                cache_put(c, old.e[i].key, old.e[i].klen, old.e[i].h, old.e[i].obj);
        free(old.e);
    }
    for (uint32_t i = h % c->cap;; i = (i + 1) % c->cap)
        if (!c->e[i].key)
        {
            c->e[i].h = h;
            c->e[i].key = malloc(klen);
            memcpy(c->e[i].key, key, klen);
            c->e[i].klen = klen;
            c->e[i].obj = obj; /* takes the reference */
            c->n++;
            return;
        }
}

static Cache g_pipes, g_depths, g_samplers, g_util_pipes;

/* --- frames and the upload ring ----------------------------------------------------------------- */

static id<MTLCommandBuffer> cmd(void)
{
    if (!g_cmd)
    {
        dispatch_semaphore_wait(g_sem, DISPATCH_TIME_FOREVER);
        g_cmd = [[g_queue commandBuffer] retain];
        dispatch_semaphore_t sem = g_sem;
        [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
          (void)b;
          dispatch_semaphore_signal(sem);
        }];
        g_ring_used = 0;
    }
    return g_cmd;
}

/* n bytes of this frame's ring; its buffer and offset. */
static void* ring(uint32_t n, uint32_t align, id<MTLBuffer>* buf, uint32_t* off)
{
    cmd();
    uint32_t at = (g_ring_used + align - 1) & ~(align - 1);
    if (at + n > RING_SIZE)
    {
        /* this frame's ring is full: hand the work so far to the GPU and wait for a fresh ring */
        extern void g9_flush_frame(int wait);
        g9_flush_frame(0);
        cmd();
        at = 0;
        if (n > RING_SIZE)
        {
            rt_log("[gfx9] upload of %u bytes is larger than the ring\n", n);
            return NULL;
        }
    }
    g_ring_used = at + n;
    *buf = g_ring[g_frame];
    *off = at;
    return (uint8_t*)[g_ring[g_frame] contents] + at;
}

static void end_pass(void)
{
    if (g_enc)
    {
        [g_enc endEncoding];
        [g_enc release];
        g_enc = nil;
    }
}

void g9_flush_frame(int wait)
{
    end_pass();
    if (g_cmd)
    {
        [g_cmd commit];
        if (wait)
            [g_cmd waitUntilCompleted];
        [g_cmd release];
        g_cmd = nil;
        g_frame = (g_frame + 1) % FRAMES;
    }
    g_targets_changed = 1;
}

/* --- init ------------------------------------------------------------------------------------- */

static const char UTIL_MSL[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V util_quad(uint i [[vertex_id]], constant float4& r [[buffer(0)]], constant float4& uvr [[buffer(1)]]) {\n"
    "  float2 c = float2(i & 1, i >> 1); V o; o.p = float4(mix(r.xy, r.zw, c), 0.0, 1.0);\n"
    "  o.uv = mix(uvr.xy, uvr.zw, c); return o; }\n"
    "fragment float4 util_copy(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, i.uv); }\n"
    "struct CO { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; float d [[depth(any)]]; };\n"
    "fragment CO util_clear(V i [[stage_in]], constant float4& col [[buffer(0)]], constant float4& z [[buffer(1)]]) {\n"
    "  CO o; o.c0 = o.c1 = o.c2 = o.c3 = col; o.d = z.x; return o; }\n"
    /* the FPS counter: 3x5 glyphs (0-9 . F P S M space), rows of 3 bits from the top, on a dark box.
     * info: the box's top left in drawable pixels, pixels per font pixel, characters */
    "constant ushort glyphs[16] = { 0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,\n"
    "  0x7BEF, 0x7BCF, 0x0002, 0x79A4, 0x7BE4, 0x79CF, 0x5F6D, 0x0000 };\n"
    "fragment float4 util_text(V i [[stage_in]], constant uint* t [[buffer(0)]], constant float4& info [[buffer(1)]]) {\n"
    "  float2 p = (i.p.xy - info.xy) / info.z - 1.0;\n"
    "  int cx = int(floor(p.x)), cy = int(floor(p.y)), ci = cx / 4, gx = cx % 4;\n"
    "  float4 bg = float4(0.0, 0.0, 0.0, 0.55);\n"
    "  if (p.x < 0.0 || p.y < 0.0 || cy >= 5 || ci >= int(info.w) || gx == 3) return bg;\n"
    "  uint bit = (uint(glyphs[t[ci]]) >> (14 - (cy * 3 + gx))) & 1u;\n"
    "  return bit ? float4(1.0, 0.95, 0.4, 1.0) : bg; }\n";

/* Full screen over the whole display: while the game is the active app the menu bar and the Dock are
 * hidden (macOS brings them back when another app is active), and the window covers the screen's
 * frame. SDL's full screen leaves the window under the menu bar's strip on displays that keep the
 * menu bar in full screen (3440x1410 of 3440x1440). */
typedef struct CoverArgs
{
    SDL_Window* w;
    int on;
} CoverArgs;

static void cover_main(void* p)
{
    CoverArgs* a = (CoverArgs*)p;
    NSWindow* nw = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(a->w),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nw)
        return;
    if (a->on)
    {
        [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
        [nw setFrame:nw.screen.frame display:YES];
        rt_log("[gfx9] full screen over %.0fx%.0f\n", nw.screen.frame.size.width, nw.screen.frame.size.height);
    }
    else
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
}

static void cover_screen(SDL_Window* w, int on)
{
    CoverArgs a = { w, on };
    sdl_main_run(cover_main, &a);
}

__attribute__((constructor)) static void cover_register(void)
{
    extern void (*user32_cover_screen)(SDL_Window * w, int on);
    user32_cover_screen = cover_screen; /* before the device: the game goes full screen first */
}

static void* g_sdl_window;

int g9_init(void* sdl_window, int vsync)
{
    @autoreleasepool
    {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev)
            return 0;
        g_queue = [g_dev newCommandQueue];
        g_sdl_window = sdl_window;
        g_view = SDL_Metal_CreateView((SDL_Window*)sdl_window);
        g_layer = (CAMetalLayer*)SDL_Metal_GetLayer(g_view);
        g_layer.device = g_dev;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = YES;
        g_layer.displaySyncEnabled = vsync ? YES : NO;
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels((SDL_Window*)sdl_window, &w, &h);
        g_layer.drawableSize = CGSizeMake(w > 0 ? w : 1280, h > 0 ? h : 720);
        g_sem = dispatch_semaphore_create(FRAMES);
        for (int i = 0; i < FRAMES; ++i)
            g_ring[i] = [g_dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
        static const float def[4] = { 0, 0, 0, 1 };
        g_default_attr = [g_dev newBufferWithBytes:def length:sizeof def options:MTLResourceStorageModeShared];
        NSError* err = nil;
        g_util = [g_dev newLibraryWithSource:@(UTIL_MSL) options:nil error:&err];
        if (!g_util)
        {
            rt_log("[gfx9] utility shaders: %s\n", err.localizedDescription.UTF8String);
            return 0;
        }
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor new] autorelease];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        g_linear = [g_dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        g_point = [g_dev newSamplerStateWithDescriptor:sd];
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[g_util newFunctionWithName:@"util_quad"] autorelease];
        pd.fragmentFunction = [[g_util newFunctionWithName:@"util_copy"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_present_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        const char* fps = getenv("XIV_FPS");
        if (!fps || strcmp(fps, "0"))
        {
            pd.fragmentFunction = [[g_util newFunctionWithName:@"util_text"] autorelease];
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            g_text_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        }
        rt_log("[gfx9] Metal on %s\n", g_dev.name.UTF8String);
        return g_present_pipe != nil;
    }
}

void g9_resize(uint32_t w, uint32_t h)
{
    (void)w, (void)h; /* the drawable follows the view (fit_drawable); present scales the back buffer into it */
}

/* The drawable at the view's own size in pixels: the window's content area is what the screen
 * shows, and the back buffer (the game's resolution) is scaled into it with its aspect kept. A
 * drawable of another size would be stretched or offset into the view by Core Animation. */
static void fit_drawable(G9Tex* bb)
{
    CGSize b = g_layer.bounds.size;
    CGFloat scale = g_layer.contentsScale > 0 ? g_layer.contentsScale : 1;
    CGFloat w = floor(b.width * scale), h = floor(b.height * scale);
    if (w < 1 || h < 1)
        return;
    if (g_layer.drawableSize.width != w || g_layer.drawableSize.height != h)
    {
        g_layer.drawableSize = CGSizeMake(w, h);
        rt_log("[gfx9] view %.0fx%.0f points x%.2f: drawable %.0fx%.0f, back buffer %ux%u\n", b.width, b.height, scale, w, h,
            bb ? bb->w : 0, bb ? bb->h : 0);
    }
}

/* --- textures --------------------------------------------------------------------------------- */

static void up_track(G9Tex* t); /* gfx9_upscale.h */
static void up_forget(G9Tex* t);

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use)
{
    @autoreleasepool
    {
        G9Tex* t = (G9Tex*)calloc(1, sizeof *t);
        t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->d = d ? d : 1;
        t->f = fmt_of(fmt);
        uint32_t maxl = 1;
        for (uint32_t s = t->w > t->h ? t->w : t->h; s > 1; s >>= 1)
            maxl++;
        t->levels = levels && levels <= maxl ? levels : maxl;
        if (t->f.block && (t->w < 4 || t->h < 4))
            t->f = fmt_of(F_A8R8G8B8), t->f.conv = CONV_NONE; /* Metal wants whole blocks: tiny DXT levels are rare */
        MTLTextureDescriptor* td = [[MTLTextureDescriptor new] autorelease];
        td.textureType = type == G9_TEX_CUBE ? MTLTextureTypeCube : type == G9_TEX_3D ? MTLTextureType3D : MTLTextureType2D;
        td.pixelFormat = t->f.pf;
        td.width = t->w;
        td.height = type == G9_TEX_CUBE ? t->w : t->h;
        td.depth = type == G9_TEX_3D ? t->d : 1;
        td.mipmapLevelCount = t->levels;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead;
        if (use == G9_USE_RT || use == G9_USE_DEPTH)
            td.usage |= MTLTextureUsageRenderTarget;
        if (t->f.pf == MTLPixelFormatBGRA8Unorm)
            td.usage |= MTLTextureUsagePixelFormatView;
        if (t->f.swizzle && use == G9_USE_SAMPLE) /* Metal: no swizzle on render targets */
        {
            static const MTLTextureSwizzleChannels sw[4] = {
                { 0 },
                { MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne },
                { MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen },
                { MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleRed },
            };
            td.swizzle = sw[t->f.swizzle];
        }
        if (use == G9_USE_SAMPLE && (fmt == F_X8R8G8B8 || fmt == F_X8B8G8R8 || fmt == F_X1R5G5B5 || fmt == F_X4R4G4B4))
            td.swizzle = (MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                                      MTLTextureSwizzleOne };
        t->tex = [g_dev newTextureWithDescriptor:td];
        if (!t->tex)
        {
            rt_log("[gfx9] cannot make a %ux%ux%u texture of format %08x\n", t->w, t->h, t->d, fmt);
            free(t);
            return NULL;
        }
        t->rw = (uint32_t)t->tex.width, t->rh = (uint32_t)t->tex.height;
        up_track(t);
        return t;
    }
}

void g9_tex_destroy(G9Tex* t)
{
    if (!t)
        return;
    for (int i = 0; i < G9_MAX_RT; ++i)
        if (g_rt[i] == t)
            g_rt[i] = NULL, g_targets_changed = 1;
    if (g_ds == t)
        g_ds = NULL, g_targets_changed = 1;
    up_forget(t);
    [t->tex release];
    [t->srgb release];
    free(t);
}

static uint32_t expand(uint32_t v, int bits) { return bits >= 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

static void convert_row(int conv, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t v = conv == CONV_A4L4 ? s[x] : (uint32_t)(s[2 * x] | (s[2 * x + 1] << 8));
        uint32_t r = 0, g = 0, b = 0, a = 255;
        switch (conv)
        {
        case CONV_565: r = expand(v >> 11, 5), g = expand((v >> 5) & 63, 6), b = expand(v & 31, 5); break;
        case CONV_X555: r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5); break;
        case CONV_1555: r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5), a = (v >> 15) ? 255 : 0; break;
        case CONV_4444: a = ((v >> 12) & 15) * 17, r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17; break;
        case CONV_X444: r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17; break;
        case CONV_A4L4: d[2 * x] = (uint8_t)((v & 15) * 17), d[2 * x + 1] = (uint8_t)((v >> 4) * 17); continue;
        default: break;
        }
        d[4 * x] = (uint8_t)b, d[4 * x + 1] = (uint8_t)g, d[4 * x + 2] = (uint8_t)r, d[4 * x + 3] = (uint8_t)a;
    }
}

static void level_dims(const G9Tex* t, uint32_t level, uint32_t* w, uint32_t* h, uint32_t* d)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = (t->type == G9_TEX_CUBE ? t->w : t->h) >> level;
    if (!*h)
        *h = 1;
    *d = t->type == G9_TEX_3D ? (t->d >> level ? t->d >> level : 1) : 1;
}

/* A rectangle in the game's pixels of t's level 0, in its texture's (a scene target made at the
 * render scale: gfx9_upscale.h) */
static void scale_rect(const G9Tex* t, int32_t r[4])
{
    if (t->rw == t->w && t->rh == t->h)
        return;
    uint32_t h = t->type == G9_TEX_CUBE ? t->w : t->h;
    for (int i = 0; i < 4; ++i)
    {
        int64_t num = i & 1 ? t->rh : t->rw, den = i & 1 ? h : t->w;
        r[i] = (int32_t)(((int64_t)r[i] * num + den / 2) / den);
    }
}

static void real_dims(const G9Tex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    uint32_t d;
    level_dims(t, level, w, h, &d);
    if (t->rw != t->w || t->rh != t->h)
        *w = t->rw, *h = t->rh;
}

static id<MTLBlitCommandEncoder> blit(void)
{
    end_pass();
    return [cmd() blitCommandEncoder];
}

void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
    uint32_t d, const void* src, uint32_t pitch, uint32_t slice)
{
    if (!t || level >= t->levels)
        return;
    uint32_t lw, lh, ld;
    level_dims(t, level, &lw, &lh, &ld);
    if (x + w > lw || y + h > lh)
    {
        w = x < lw ? lw - x : 0;
        h = y < lh ? lh - y : 0;
    }
    if (!w || !h)
        return;
    @autoreleasepool
    {
        uint32_t rows, row_bytes;
        if (t->f.block)
        {
            /* Metal copies whole blocks: a level smaller than a block is copied as one */
            rows = (h + 3) / 4;
            row_bytes = ((w + 3) / 4) * t->f.block;
            w = (w + 3) & ~3u;
            h = (h + 3) & ~3u;
            if (x + w > ((lw + 3) & ~3u) || y + h > ((lh + 3) & ~3u))
                return;
        }
        else
        {
            rows = h;
            row_bytes = w * t->f.mtexel;
        }
        uint32_t mslice = rows * row_bytes;
        id<MTLBuffer> buf;
        uint32_t off;
        uint8_t* dst = (uint8_t*)ring(mslice * d, 16, &buf, &off);
        if (!dst)
            return;
        for (uint32_t k = 0; k < d; ++k)
            for (uint32_t r = 0; r < rows; ++r)
            {
                const uint8_t* s = (const uint8_t*)src + (size_t)k * slice + (size_t)r * pitch;
                uint8_t* o = dst + (size_t)k * mslice + (size_t)r * row_bytes;
                if (t->f.conv)
                    convert_row(t->f.conv, s, o, w);
                else
                    memcpy(o, s, row_bytes);
            }
        id<MTLBlitCommandEncoder> b = blit();
        [b copyFromBuffer:buf
                   sourceOffset:off
              sourceBytesPerRow:row_bytes
            sourceBytesPerImage:t->type == G9_TEX_3D ? mslice : 0
                     sourceSize:MTLSizeMake(w, h, d)
                      toTexture:t->tex
               destinationSlice:t->type == G9_TEX_CUBE ? face : 0
               destinationLevel:level
              destinationOrigin:MTLOriginMake(x, y, z)];
        [b endEncoding];
    }
}

void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->f.depth)
        return;
    if (t->rw != t->w || t->rh != t->h)
    {
        /* a scene target at the render scale: read at the game's size */
        G9Tex* full = g9_tex_create(G9_TEX_2D, t->fmt, t->w, t->h, 1, 1, G9_USE_SAMPLE);
        if (full)
        {
            g9_stretch(t, 0, 0, NULL, full, 0, 0, NULL, 1);
            g9_tex_read(full, 0, 0, dst, pitch);
            g9_tex_destroy(full);
        }
        return;
    }
    @autoreleasepool
    {
        uint32_t w, h, d;
        level_dims(t, level, &w, &h, &d);
        uint32_t row = t->f.block ? ((w + 3) / 4) * t->f.block : w * t->f.mtexel;
        uint32_t rows = t->f.block ? (h + 3) / 4 : h;
        id<MTLBuffer> buf = [[g_dev newBufferWithLength:(NSUInteger)row * rows options:MTLResourceStorageModeShared] autorelease];
        id<MTLBlitCommandEncoder> b = blit();
        [b copyFromTexture:t->tex
                     sourceSlice:t->type == G9_TEX_CUBE ? face : 0
                     sourceLevel:level
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:row * rows];
        [b endEncoding];
        g9_flush_frame(1);
        const uint8_t* s = (const uint8_t*)[buf contents];
        uint32_t copy = row < pitch ? row : pitch;
        if (t->f.conv)
            copy = row; /* 16-bit formats come back as BGRA8: callers lock BGRA8 targets only */
        for (uint32_t r = 0; r < rows; ++r)
            memcpy((uint8_t*)dst + (size_t)r * pitch, s + (size_t)r * row, copy < pitch ? copy : pitch);
    }
}

void g9_tex_gen_mips(G9Tex* t)
{
    if (!t || t->levels < 2 || t->f.block || t->f.depth)
        return;
    id<MTLBlitCommandEncoder> b = blit();
    [b generateMipmapsForTexture:t->tex];
    [b endEncoding];
}

/* --- buffers and shaders ------------------------------------------------------------------------ */

G9Buf* g9_buf_create(uint32_t size)
{
    G9Buf* b = (G9Buf*)calloc(1, sizeof *b);
    b->size = size ? size : 16;
    b->buf = [g_dev newBufferWithLength:b->size options:MTLResourceStorageModePrivate];
    return b;
}

void g9_buf_destroy(G9Buf* b)
{
    if (!b)
        return;
    [b->buf release];
    free(b);
}

void g9_buf_upload(G9Buf* b, uint32_t offset, const void* data, uint32_t size)
{
    if (!b || offset >= b->size)
        return;
    if (offset + size > b->size)
        size = b->size - offset;
    id<MTLBuffer> src;
    uint32_t off;
    void* p = ring(size, 16, &src, &off);
    if (!p)
        return;
    memcpy(p, data, size);
    id<MTLBlitCommandEncoder> bl = blit();
    [bl copyFromBuffer:src sourceOffset:off toBuffer:b->buf destinationOffset:offset size:(size + 3) & ~3u];
    [bl endEncoding];
}

G9Shader* g9_shader_create(const uint32_t* tokens, uint32_t ntokens)
{
    G9Shader* s = (G9Shader*)calloc(1, sizeof *s);
    s->tokens = (uint32_t*)malloc(4 * ntokens);
    memcpy(s->tokens, tokens, 4 * ntokens);
    s->ntokens = ntokens;
    if (!sm_info(s->tokens, ntokens, &s->info))
        rt_log("[gfx9] shader %08x: not translatable\n", s->info.hash);
    return s;
}

void g9_shader_destroy(G9Shader* s)
{
    if (!s)
        return;
    free(s->var); /* the functions stay in g_fns: the game makes the same shaders again */
    free(s->tokens);
    free(s);
}

/* Translated functions by shader content and variant, across shader objects: the game creates the
 * same shaders again as it loads things.
 *
 * Background compiles: Metal takes tens of milliseconds for a shader or a pipeline, and the game
 * meets new ones whenever something comes into view (turning the camera in town). A draw of the 3D
 * scene compiles what it needs in the background and is skipped until it is ready (the object
 * appears a frame or so later); every other draw (the interface, copies, effects) waits for its
 * compile, so it is never missing. */
static Cache g_fns;
static uint32_t g_compiles, g_fn_reuse;

static int compiled_ready(Compiled* c, int wait)
{
    while (!__atomic_load_n(&c->state, __ATOMIC_ACQUIRE))
    {
        if (!wait)
            return 0;
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
    return 1;
}

static void compiled_set(Compiled* c, id obj)
{
    c->obj = obj;
    __atomic_store_n(&c->state, 1, __ATOMIC_RELEASE);
}

static Compiled* shader_variant(G9Shader* s, const SmOpts* o, int background)
{
    uint64_t key = fnv64(o, sizeof *o);
    for (uint32_t i = 0; i < s->nvar; ++i)
        if (s->var[i].key == key)
            return s->var[i].fn;
    if (!s->content)
        s->content = fnv64(s->tokens, 4ull * s->ntokens);
    struct { uint64_t content, opts; uint32_t ntokens, pad; } fk = { s->content, key, s->ntokens, 0 };
    uint64_t fh;
    Compiled* c = (Compiled*)cache_get(&g_fns, &fk, sizeof fk, &fh);
    if (c)
        g_fn_reuse++;
    else
    {
        c = (Compiled*)calloc(1, sizeof *c);
        cache_put(&g_fns, &fk, sizeof fk, fh, (id)c);
        if (++g_compiles % 100 == 0)
            rt_log("[gfx9] %u shader variants compiled (%u reused from other shader objects)\n", g_compiles, g_fn_reuse);
        static char src[1 << 20];
        char err[256];
        SmInfo info;
        if (sm_translate(s->tokens, s->ntokens, o, &info, src, sizeof src, err, sizeof err))
        {
            @autoreleasepool
            {
                MTLCompileOptions* co = [[MTLCompileOptions new] autorelease];
                co.languageVersion = MTLLanguageVersion3_0;
                co.mathMode = MTLMathModeSafe; /* D3D9's IEEE behaviour: no reassociation, NaN and inf kept */
                NSString* name = info.pixel ? @"pmain" : @"vmain";
                uint32_t hash = s->info.hash;
                void (^done)(id<MTLLibrary>, NSError*) = ^(id<MTLLibrary> lib, NSError* e) {
                    if (!lib)
                        rt_log("[gfx9] shader %08x: %s\n", hash, e.localizedDescription.UTF8String);
                    compiled_set(c, lib ? [lib newFunctionWithName:name] : nil);
                };
                if (background)
                    [g_dev newLibraryWithSource:@(src) options:co completionHandler:done];
                else
                {
                    NSError* e = nil;
                    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:co error:&e];
                    done(lib, e);
                    [lib release];
                }
            }
        }
        else
        {
            rt_log("[gfx9] shader %08x: %s\n", s->info.hash, err);
            compiled_set(c, nil);
        }
    }
    if (s->nvar == s->capvar)
    {
        s->capvar = s->capvar ? s->capvar * 2 : 4;
        s->var = (Variant*)realloc(s->var, s->capvar * sizeof *s->var);
    }
    s->var[s->nvar++] = (Variant){ key, c };
    return c;
}

/* --- render passes ------------------------------------------------------------------------------ */

static int begin_pass(void);

void g9_set_targets(G9Tex* const color[G9_MAX_RT], const uint8_t face[G9_MAX_RT], const uint8_t level[G9_MAX_RT],
    G9Tex* depth)
{
    int same = depth == g_ds;
    for (int i = 0; i < G9_MAX_RT; ++i)
        same &= color[i] == g_rt[i] && (!color[i] || (face[i] == g_rt_face[i] && level[i] == g_rt_level[i]));
    if (same)
        return;
    if (g_pending_clear)
        begin_pass(); /* a clear of the old targets with no draw after it still happens */
    end_pass();
    for (int i = 0; i < G9_MAX_RT; ++i)
        g_rt[i] = color[i], g_rt_face[i] = face[i], g_rt_level[i] = level[i];
    g_ds = depth;
    g_targets_changed = 1;
    g_pending_clear = 0;
}

static void target_size(uint32_t* w, uint32_t* h)
{
    for (int i = 0; i < G9_MAX_RT; ++i)
        if (g_rt[i])
        {
            uint32_t d;
            level_dims(g_rt[i], g_rt_level[i], w, h, &d);
            return;
        }
    if (g_ds)
    {
        *w = g_ds->w, *h = g_ds->h;
        return;
    }
    *w = *h = 1;
}

/* the bound targets' size in their textures' pixels, and per game pixel (a scene target made at
 * the render scale: gfx9_upscale.h) */
static void target_real_size(uint32_t* w, uint32_t* h, float* sx, float* sy)
{
    uint32_t lw, lh;
    target_size(&lw, &lh);
    *w = lw, *h = lh;
    const G9Tex* t = NULL;
    for (int i = 0; i < G9_MAX_RT && !t; ++i)
        t = g_rt[i];
    if (!t)
        t = g_ds;
    if (t && t->type == G9_TEX_2D && (t->rw != t->w || t->rh != t->h))
        real_dims(t, 0, w, h);
    *sx = (float)*w / lw, *sy = (float)*h / lh;
}

static int begin_pass(void)
{
    if (g_enc)
        return 1;
    @autoreleasepool
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        int any = 0;
        for (int i = 0; i < G9_MAX_RT; ++i)
        {
            G9Tex* t = g_rt[i];
            if (!t || t->fmt == F_NULL)
                continue;
            any = 1;
            MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[i];
            a.texture = t->tex;
            a.level = g_rt_level[i];
            a.slice = t->type == G9_TEX_CUBE ? g_rt_face[i] : 0;
            a.storeAction = MTLStoreActionStore;
            if (g_pending_clear & 1)
            {
                a.loadAction = MTLLoadActionClear;
                a.clearColor = MTLClearColorMake(g_clear_color[0], g_clear_color[1], g_clear_color[2], g_clear_color[3]);
            }
            else
                a.loadAction = MTLLoadActionLoad;
        }
        if (g_ds)
        {
            any = 1;
            rp.depthAttachment.texture = g_ds->tex;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.loadAction = (g_pending_clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = g_clear_z;
            if (g_ds->tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
            {
                rp.stencilAttachment.texture = g_ds->tex;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
                rp.stencilAttachment.loadAction = (g_pending_clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.clearStencil = g_clear_stencil;
            }
        }
        if (!any)
            return 0;
        g_pending_clear = 0;
        g_enc = [[cmd() renderCommandEncoderWithDescriptor:rp] retain];
        g_pass_draws = 0;
        g_targets_changed = 0;
        return 1;
    }
}

static MTLPixelFormat rt_format(int i) { return g_rt[i] && g_rt[i]->fmt != F_NULL ? g_rt[i]->tex.pixelFormat : MTLPixelFormatInvalid; }
static MTLPixelFormat ds_format(void) { return g_ds ? g_ds->tex.pixelFormat : MTLPixelFormatInvalid; }

/* a pipeline of the utility library for the current targets */
static id<MTLRenderPipelineState> util_pipe(NSString* frag, uint32_t write)
{
    struct { MTLPixelFormat c[G9_MAX_RT], d; uint32_t write; char name[16]; } k;
    memset(&k, 0, sizeof k);
    for (int i = 0; i < G9_MAX_RT; ++i)
        k.c[i] = rt_format(i);
    k.d = ds_format();
    k.write = write;
    snprintf(k.name, sizeof k.name, "%s", frag.UTF8String);
    uint64_t h;
    id p = cache_get(&g_util_pipes, &k, sizeof k, &h);
    if (p)
        return p;
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[g_util newFunctionWithName:@"util_quad"] autorelease];
        pd.fragmentFunction = [[g_util newFunctionWithName:frag] autorelease];
        for (int i = 0; i < G9_MAX_RT; ++i)
        {
            pd.colorAttachments[i].pixelFormat = k.c[i];
            pd.colorAttachments[i].writeMask = (write & 1) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        pd.depthAttachmentPixelFormat = k.d;
        if (k.d == MTLPixelFormatDepth32Float_Stencil8)
            pd.stencilAttachmentPixelFormat = k.d;
        NSError* e = nil;
        p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!p)
            rt_log("[gfx9] utility pipeline: %s\n", e.localizedDescription.UTF8String);
        cache_put(&g_util_pipes, &k, sizeof k, h, p);
        return p;
    }
}

static id<MTLDepthStencilState> clear_depth_state(int z, int stencil)
{
    struct { int z, s; } k = { z, stencil };
    uint64_t h;
    id d = cache_get(&g_depths, &k, sizeof k, &h);
    if (d)
        return d;
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = z ? YES : NO;
    if (stencil)
    {
        MTLStencilDescriptor* s = [[MTLStencilDescriptor new] autorelease];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.depthStencilPassOperation = MTLStencilOperationReplace;
        dd.frontFaceStencil = dd.backFaceStencil = s;
    }
    d = [g_dev newDepthStencilStateWithDescriptor:dd];
    cache_put(&g_depths, &k, sizeof k, h, d);
    return d;
}

void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const G9Viewport* vp, const int32_t* scissor)
{
    uint32_t tw, th;
    target_size(&tw, &th);
    int whole = !nrects && vp->x == 0 && vp->y == 0 && vp->w >= tw && vp->h >= th && !scissor;
    if (!g_ds)
        flags &= 1;
    else if (g_ds->tex.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
        flags &= 3;
    if (!flags)
        return;
    float col[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f,
                     (color >> 24) / 255.0f };
    if (whole && !g_enc)
    {
        g_pending_clear |= flags;
        memcpy(g_clear_color, col, sizeof col);
        g_clear_z = z;
        g_clear_stencil = stencil;
        return;
    }
    if (g_pending_clear)
    {
        /* an earlier whole-target clear still pending: start its pass first */
        if (!begin_pass())
            return;
    }
    if (whole)
    {
        /* a whole-target clear after draws: end the pass and start a clearing one */
        end_pass();
        g_pending_clear = flags;
        memcpy(g_clear_color, col, sizeof col);
        g_clear_z = z;
        g_clear_stencil = stencil;
        return;
    }
    if (!begin_pass())
        return;
    /* a quad over each rectangle (or the viewport), clipped by the scissor */
    int32_t one[4] = { (int32_t)vp->x, (int32_t)vp->y, (int32_t)(vp->x + vp->w), (int32_t)(vp->y + vp->h) };
    const int32_t* rs = nrects ? rects : one;
    uint32_t n = nrects ? nrects : 1;
    [g_enc setRenderPipelineState:util_pipe(@"util_clear", flags & 1)];
    [g_enc setDepthStencilState:clear_depth_state(flags & 2, flags & 4)];
    [g_enc setStencilReferenceValue:stencil];
    [g_enc setCullMode:MTLCullModeNone];
    [g_enc setTriangleFillMode:MTLTriangleFillModeFill];
    uint32_t rw, rh;
    float sx, sy;
    target_real_size(&rw, &rh, &sx, &sy);
    MTLViewport mv = { 0, 0, rw, rh, 0, 1 };
    [g_enc setViewport:mv];
    float zz[4] = { z, 0, 0, 0 };
    [g_enc setFragmentBytes:col length:16 atIndex:0];
    [g_enc setFragmentBytes:zz length:16 atIndex:1];
    for (uint32_t i = 0; i < n; ++i)
    {
        int32_t l = rs[4 * i], t = rs[4 * i + 1], r = rs[4 * i + 2], b = rs[4 * i + 3];
        if (l < (int32_t)vp->x) l = (int32_t)vp->x;
        if (t < (int32_t)vp->y) t = (int32_t)vp->y;
        if (r > (int32_t)(vp->x + vp->w)) r = (int32_t)(vp->x + vp->w);
        if (b > (int32_t)(vp->y + vp->h)) b = (int32_t)(vp->y + vp->h);
        if (scissor)
        {
            if (l < scissor[0]) l = scissor[0];
            if (t < scissor[1]) t = scissor[1];
            if (r > scissor[2]) r = scissor[2];
            if (b > scissor[3]) b = scissor[3];
        }
        l = (int32_t)lroundf(l * sx), t = (int32_t)lroundf(t * sy), r = (int32_t)lroundf(r * sx), b = (int32_t)lroundf(b * sy);
        if (r > (int32_t)rw) r = (int32_t)rw;
        if (b > (int32_t)rh) b = (int32_t)rh;
        if (r <= l || b <= t)
            continue;
        MTLScissorRect sr = { (NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t) };
        [g_enc setScissorRect:sr];
        float q[4] = { -1, 1, 1, -1 }, uv[4] = { 0, 0, 1, 1 };
        [g_enc setVertexBytes:q length:16 atIndex:0];
        [g_enc setVertexBytes:uv length:16 atIndex:1];
        [g_enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    MTLScissorRect full = { 0, 0, rw, rh };
    [g_enc setScissorRect:full];
}

/* --- StretchRect and ColorFill ------------------------------------------------------------------- */

static void fx_note_stretch(G9Tex* src, G9Tex* dst); /* gfx9_fx.h */

void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t sr[4], G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t dr[4], int linear)
{
    if (!src || !dst)
        return;
    fx_note_stretch(src, dst);
    uint32_t sw, sh, sd, dw, dh, dd;
    level_dims(src, slevel, &sw, &sh, &sd);
    level_dims(dst, dlevel, &dw, &dh, &dd);
    int32_t s[4] = { 0, 0, (int32_t)sw, (int32_t)sh }, d[4] = { 0, 0, (int32_t)dw, (int32_t)dh };
    if (sr)
        memcpy(s, sr, sizeof s);
    if (dr)
        memcpy(d, dr, sizeof d);
    /* the rectangles in the game's pixels, in the textures' (scene targets at the render scale) */
    scale_rect(src, s);
    scale_rect(dst, d);
    real_dims(src, slevel, &sw, &sh);
    real_dims(dst, dlevel, &dw, &dh);
    @autoreleasepool
    {
        if ((s[2] - s[0] == d[2] - d[0] && s[3] - s[1] == d[3] - d[1] && src->tex.pixelFormat == dst->tex.pixelFormat) ||
            src->f.depth)
        {
            /* same size and format (and every depth copy): a blit */
            if (src->tex.pixelFormat != dst->tex.pixelFormat || s[2] - s[0] != d[2] - d[0] || s[3] - s[1] != d[3] - d[1])
                return;
            id<MTLBlitCommandEncoder> b = blit();
            [b copyFromTexture:src->tex
                       sourceSlice:src->type == G9_TEX_CUBE ? sface : 0
                       sourceLevel:slevel
                      sourceOrigin:MTLOriginMake((NSUInteger)s[0], (NSUInteger)s[1], 0)
                        sourceSize:MTLSizeMake((NSUInteger)(s[2] - s[0]), (NSUInteger)(s[3] - s[1]), 1)
                         toTexture:dst->tex
                  destinationSlice:dst->type == G9_TEX_CUBE ? dface : 0
                  destinationLevel:dlevel
                 destinationOrigin:MTLOriginMake((NSUInteger)d[0], (NSUInteger)d[1], 0)];
            [b endEncoding];
            return;
        }
        /* scaled or converted: draw src into dst */
        end_pass();
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst->tex;
        rp.colorAttachments[0].level = dlevel;
        rp.colorAttachments[0].slice = dst->type == G9_TEX_CUBE ? dface : 0;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
        struct { MTLPixelFormat c; char name[16]; } k = { dst->tex.pixelFormat, "stretch" };
        uint64_t h;
        id<MTLRenderPipelineState> p = cache_get(&g_util_pipes, &k, sizeof k, &h);
        if (!p)
        {
            MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
            pd.vertexFunction = [[g_util newFunctionWithName:@"util_quad"] autorelease];
            pd.fragmentFunction = [[g_util newFunctionWithName:@"util_copy"] autorelease];
            pd.colorAttachments[0].pixelFormat = dst->tex.pixelFormat;
            p = [g_dev newRenderPipelineStateWithDescriptor:pd error:nil];
            cache_put(&g_util_pipes, &k, sizeof k, h, p);
        }
        [e setRenderPipelineState:p];
        MTLViewport mv = { 0, 0, dw, dh, 0, 1 };
        [e setViewport:mv];
        float q[4] = { 2.0f * d[0] / dw - 1, 1 - 2.0f * d[1] / dh, 2.0f * d[2] / dw - 1, 1 - 2.0f * d[3] / dh };
        float uv[4] = { (float)s[0] / sw, (float)s[1] / sh, (float)s[2] / sw, (float)s[3] / sh };
        [e setVertexBytes:q length:16 atIndex:0];
        [e setVertexBytes:uv length:16 atIndex:1];
        id<MTLTexture> view = src->tex;
        if (src->type == G9_TEX_CUBE || src->levels > 1)
            view = [[src->tex newTextureViewWithPixelFormat:src->tex.pixelFormat
                                                textureType:MTLTextureType2D
                                                     levels:NSMakeRange(slevel, 1)
                                                     slices:NSMakeRange(src->type == G9_TEX_CUBE ? sface : 0, 1)]
                autorelease];
        [e setFragmentTexture:view atIndex:0];
        [e setFragmentSamplerState:linear ? g_linear : g_point atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        g_targets_changed = 1;
    }
}

void g9_fill(G9Tex* t, uint32_t face, uint32_t level, const int32_t rect[4], uint32_t color)
{
    if (!t)
        return;
    G9Tex* save_rt[G9_MAX_RT];
    uint8_t save_face[G9_MAX_RT], save_level[G9_MAX_RT];
    G9Tex* save_ds = g_ds;
    memcpy(save_rt, g_rt, sizeof save_rt);
    memcpy(save_face, g_rt_face, sizeof save_face);
    memcpy(save_level, g_rt_level, sizeof save_level);
    G9Tex* one[G9_MAX_RT] = { t };
    uint8_t f[G9_MAX_RT] = { (uint8_t)face }, l[G9_MAX_RT] = { (uint8_t)level };
    g9_set_targets(one, f, l, NULL);
    uint32_t w, h, d;
    level_dims(t, level, &w, &h, &d);
    G9Viewport vp = { 0, 0, w, h, 0, 1 };
    g9_clear(rect ? 1 : 0, rect, 1, color, 0, 0, &vp, NULL);
    if (g_pending_clear)
        begin_pass();
    g9_set_targets(save_rt, save_face, save_level, save_ds);
}

/* --- draws -------------------------------------------------------------------------------------- */

static MTLVertexFormat vertex_format(uint32_t type, uint8_t* is_int)
{
    *is_int = 0;
    switch (type)
    {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: return MTLVertexFormatUChar4Normalized_BGRA;
    case 5: *is_int = 2; return MTLVertexFormatUChar4;
    case 6: *is_int = 1; return MTLVertexFormatShort2;
    case 7: *is_int = 1; return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 13: *is_int = 3; return MTLVertexFormatUInt; /* UDEC3, DEC3N: unpacked by the shader */
    case 14: *is_int = 4; return MTLVertexFormatUInt;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    default: return MTLVertexFormatInvalid;
    }
}

static MTLBlendFactor blend_factor(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static MTLCompareFunction compare(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static id<MTLDepthStencilState> depth_state(const G9States* rs)
{
    struct
    {
        uint8_t z, zw, zf, st, two, f[4], cf[4], rd, wr;
    } k;
    memset(&k, 0, sizeof k);
    int has_depth = g_ds != NULL, has_stencil = g_ds && g_ds->tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    k.z = has_depth && rs->zenable;
    k.zw = k.z && rs->zwrite;
    k.zf = k.z ? rs->zfunc : 8;
    k.st = has_stencil && rs->stencil;
    if (k.st)
    {
        k.two = rs->two_sided;
        k.f[0] = rs->sfail, k.f[1] = rs->szfail, k.f[2] = rs->spass, k.f[3] = rs->sfunc;
        if (k.two)
            k.cf[0] = rs->ccw_sfail, k.cf[1] = rs->ccw_szfail, k.cf[2] = rs->ccw_spass, k.cf[3] = rs->ccw_sfunc;
        k.rd = rs->sread, k.wr = rs->swrite;
    }
    uint64_t h;
    id d = cache_get(&g_depths, &k, sizeof k, &h);
    if (d)
        return d;
    @autoreleasepool
    {
        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor new] autorelease];
        dd.depthCompareFunction = k.z ? compare(k.zf) : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = k.zw ? YES : NO;
        if (k.st)
        {
            /* D3D9's front face is clockwise (CULL_CCW culls the counter-clockwise back); the
             * counter-clockwise stencil ops apply to the back */
            MTLStencilDescriptor* f = [[MTLStencilDescriptor new] autorelease];
            f.stencilFailureOperation = stencil_op(k.f[0]);
            f.depthFailureOperation = stencil_op(k.f[1]);
            f.depthStencilPassOperation = stencil_op(k.f[2]);
            f.stencilCompareFunction = compare(k.f[3]);
            f.readMask = k.rd;
            f.writeMask = k.wr;
            MTLStencilDescriptor* b = f;
            if (k.two)
            {
                b = [[MTLStencilDescriptor new] autorelease];
                b.stencilFailureOperation = stencil_op(k.cf[0]);
                b.depthFailureOperation = stencil_op(k.cf[1]);
                b.depthStencilPassOperation = stencil_op(k.cf[2]);
                b.stencilCompareFunction = compare(k.cf[3]);
                b.readMask = k.rd;
                b.writeMask = k.wr;
            }
            dd.frontFaceStencil = f;
            dd.backFaceStencil = b;
        }
        d = [g_dev newDepthStencilStateWithDescriptor:dd];
    }
    cache_put(&g_depths, &k, sizeof k, h, d);
    return d;
}

static MTLSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> sampler(const G9Sampler* s, int compare_mode)
{
    struct { G9Sampler s; int cmp; } k;
    memset(&k, 0, sizeof k);
    k.s = *s;
    k.s.srgb = 0;
    k.cmp = compare_mode;
    uint64_t h;
    id r = cache_get(&g_samplers, &k, sizeof k, &h);
    if (r)
        return r;
    @autoreleasepool
    {
        MTLSamplerDescriptor* d = [[MTLSamplerDescriptor new] autorelease];
        d.sAddressMode = address(s->addr_u);
        d.tAddressMode = address(s->addr_v);
        d.rAddressMode = address(s->addr_w);
        d.magFilter = s->mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = s->min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = s->mip == 0 ? MTLSamplerMipFilterNotMipmapped : s->mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        if (s->min == 3 || s->mag == 3) /* ANISOTROPIC */
            d.maxAnisotropy = s->max_aniso > 1 ? (s->max_aniso > 16 ? 16 : s->max_aniso) : 1;
        d.lodMinClamp = s->max_level;
        uint32_t bc = s->border;
        d.borderColor = (bc >> 24) == 0 ? MTLSamplerBorderColorTransparentBlack
                        : (bc & 0xFFFFFF) == 0xFFFFFF ? MTLSamplerBorderColorOpaqueWhite
                                                      : MTLSamplerBorderColorOpaqueBlack;
        if (compare_mode)
            d.compareFunction = MTLCompareFunctionLessEqual;
        d.supportArgumentBuffers = NO;
        r = [g_dev newSamplerStateWithDescriptor:d];
    }
    cache_put(&g_samplers, &k, sizeof k, h, r);
    return r;
}

typedef struct PipeKey
{
    uint64_t vs, ps; /* the variants' keys, combined with the shader hashes */
    struct { uint8_t fmt, stream, attr, pad; uint16_t offset, pad2; } attr[16];
    uint32_t stride[G9_MAX_STREAMS];
    uint8_t step_instance[G9_MAX_STREAMS];
    MTLPixelFormat color[G9_MAX_RT], depth;
    uint8_t blend, src, dst, op, sep, src_a, dst_a, op_a;
    uint8_t mask[G9_MAX_RT];
    uint8_t srgb, topology, pad[2];
} PipeKey;

static int is_depth_format(const G9Tex* t) { return t && t->f.depth; }

static void draw_prim(MTLPrimitiveType* mp, uint32_t prim, uint32_t count, uint32_t* nverts)
{
    switch (prim)
    {
    case 1: *mp = MTLPrimitiveTypePoint, *nverts = count; break;
    case 2: *mp = MTLPrimitiveTypeLine, *nverts = count * 2; break;
    case 3: *mp = MTLPrimitiveTypeLineStrip, *nverts = count + 1; break;
    case 5: *mp = MTLPrimitiveTypeTriangleStrip, *nverts = count + 2; break;
    case 6: *mp = MTLPrimitiveTypeTriangle, *nverts = count * 3; break; /* fan: expanded to a list */
    default: *mp = MTLPrimitiveTypeTriangle, *nverts = count * 3; break;
    }
}

static uint32_t g_draw_errors;

#include "gfx9_fx.h"
#include "gfx9_upscale.h"
#include "gfx9_navdraw.h"

/* --- one-frame draw trace (XIV_GFXTRACE=<dir>; touch <dir>/req to trace the next frame) ---------
 * <dir>/draws.txt: a line per draw (shaders, targets, blend, samplers); <dir>/consts.bin: per draw
 * the vertex then pixel float constants; <dir>/<vs|ps>_<hash>_<variant>.metal: the translations. */
static FILE* g_trace;
static const char* g_trace_dir;
static uint32_t g_trace_draw;
static G9Tex* g_trace_cubes[32];
static uint32_t g_trace_ncubes;

static void trace_cube(G9Tex* t)
{
    /* cube maps, and 2D render targets the frame samples (their contents at the frame's end) */
    if (!t || t->fmt != F_A8R8G8B8 || !(t->type == G9_TEX_CUBE || (t->type == G9_TEX_2D && t->use == G9_USE_RT)))
        return;
    for (uint32_t i = 0; i < g_trace_ncubes; ++i)
        if (g_trace_cubes[i] == t)
            return;
    if (g_trace_ncubes < 32)
        g_trace_cubes[g_trace_ncubes++] = t;
}

/* the traced frame's A8R8G8B8 cube maps: each face and level's mean colour (cubes.txt) */
static void trace_dump_cubes(void)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/cubes.txt", g_trace_dir);
    FILE* f = fopen(p, "w");
    if (!f)
        return;
    for (uint32_t i = 0; i < g_trace_ncubes; ++i)
    {
        G9Tex* t = g_trace_cubes[i];
        for (uint32_t l = 0; l < t->levels; ++l)
        {
            uint32_t w, h, d;
            level_dims(t, l, &w, &h, &d);
            uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
            fprintf(f, "%p L%u %ux%u%s:", (void*)t, l, w, h, t->type == G9_TEX_CUBE ? "" : " rt");
            for (uint32_t face = 0; face < (t->type == G9_TEX_CUBE ? 6u : 1u); ++face)
            {
                memset(px, 0, (size_t)w * h * 4);
                g9_tex_read(t, face, l, px, w * 4);
                double m[4] = { 0, 0, 0, 0 };
                for (uint32_t k = 0; k < w * h; ++k)
                    for (int c = 0; c < 4; ++c)
                        m[c] += px[4 * k + c];
                /* BGRA in memory: print R G B A */
                fprintf(f, "  f%u %3.0f %3.0f %3.0f %3.0f", face, m[2] / (w * h), m[1] / (w * h), m[0] / (w * h),
                    m[3] / (w * h));
                if (l == 0)
                {
                    char q[1024];
                    snprintf(q, sizeof q, "%s/%s_%p_f%u.ppm", g_trace_dir, t->type == G9_TEX_CUBE ? "cube" : "rt",
                        (void*)t, face);
                    FILE* pf = fopen(q, "wb");
                    if (pf)
                    {
                        fprintf(pf, "P6 %u %u 255\n", w, h);
                        for (uint32_t k = 0; k < w * h; ++k)
                        {
                            uint8_t rgb[3] = { px[4 * k + 2], px[4 * k + 1], px[4 * k] };
                            fwrite(rgb, 1, 3, pf);
                        }
                        fclose(pf);
                    }
                    if (t->type != G9_TEX_CUBE)
                    {
                        /* and its alpha */
                        snprintf(q, sizeof q, "%s/rt_%p_alpha.pgm", g_trace_dir, (void*)t);
                        if ((pf = fopen(q, "wb")))
                        {
                            fprintf(pf, "P5 %u %u 255\n", w, h);
                            for (uint32_t k = 0; k < w * h; ++k)
                                fwrite(&px[4 * k + 3], 1, 1, pf);
                            fclose(pf);
                        }
                    }
                }
            }
            fprintf(f, "\n");
            free(px);
        }
    }
    fclose(f);
    g_trace_ncubes = 0;
}

static void trace_frame_end(void)
{
    if (g_trace)
    {
        fprintf(g_trace, "present %p\n", (void*)g_xfx.bb);
        fclose(g_trace);
        g_trace = NULL;
        trace_dump_cubes();
        rt_log("[gfx9] trace: %u draws in %s\n", g_trace_draw, g_trace_dir);
    }
    if (!g_trace_dir && !(g_trace_dir = getenv("XIV_GFXTRACE")))
        g_trace_dir = "";
    if (!*g_trace_dir)
        return;
    char p[1024];
    snprintf(p, sizeof p, "%s/req", g_trace_dir);
    if (remove(p) != 0)
        return;
    snprintf(p, sizeof p, "%s/draws.txt", g_trace_dir);
    g_trace = fopen(p, "w");
    snprintf(p, sizeof p, "%s/consts.bin", g_trace_dir);
    FILE* cb = fopen(p, "wb");
    if (cb)
        fclose(cb);
    g_trace_draw = 0;
}

static void trace_msl(G9Shader* s, const SmOpts* o)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s_%08x_%016llx.metal", g_trace_dir, s->info.pixel ? "ps" : "vs", s->info.hash,
        (unsigned long long)fnv64(o, sizeof *o));
    FILE* f = fopen(p, "r");
    if (f)
    {
        fclose(f);
        return;
    }
    static char src[1 << 20];
    SmInfo info;
    size_t n = sm_translate(s->tokens, s->ntokens, o, &info, src, sizeof src, NULL, 0);
    if ((f = fopen(p, "w")))
    {
        fwrite(src, 1, n, f);
        fclose(f);
    }
}

static void trace_draw(const G9Draw* d, const SmOpts* vo, const SmOpts* po, const PipeKey* k)
{
    const SmInfo *vi = &d->vs->info, *pi = &d->ps->info;
    fprintf(g_trace, "#%u vs %08x ps %08x prim %u n %u inst %u rt %d/%d/%d/%d ds %d srgbw %d blend %d %d %d %d "
        "z %d/%d/%d cull %d atest %d/%u mask %x vp %ux%u dbias %g/%g st %d/%u/%u%u%u ref %u",
        g_trace_draw, vi->hash, pi->hash, d->prim, d->count, d->instances, (int)k->color[0], (int)k->color[1],
        (int)k->color[2], (int)k->color[3], (int)k->depth, d->rs.srgb_write, d->rs.blend, d->rs.src, d->rs.dst,
        d->rs.op, d->rs.zenable, d->rs.zwrite, d->rs.zfunc, d->rs.cull, d->rs.alpha_func, d->rs.alpha_ref,
        d->rs.write_mask[0], d->vp.w, d->vp.h, d->rs.depth_bias, d->rs.slope_bias, d->rs.stencil, d->rs.sfunc,
        d->rs.sfail, d->rs.szfail, d->rs.spass, d->rs.stencil_ref);
    if (g_rt[0])
        fprintf(g_trace, " rt0 %p %ux%u fmt %u face %u level %u", (void*)g_rt[0], g_rt[0]->w, g_rt[0]->h, g_rt[0]->fmt,
            g_rt_face[0], g_rt_level[0]);
    if (g_ds)
        fprintf(g_trace, " ds0 %p %ux%u fmt %u", (void*)g_ds, g_ds->w, g_ds->h, g_ds->fmt);
    trace_cube(g_rt[0]);
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
    {
        if (!pi->sampler[s])
            continue;
        const G9Tex* t = d->tex[s];
        const G9Sampler* sm = &d->samp[s];
        fprintf(g_trace, " | s%u type %u", s, pi->sampler[s]);
        trace_cube((G9Tex*)t);
        if (t)
            fprintf(g_trace, " tex %p fmt %u %ux%u L%u srgb %u addr %u%u%u flt %u%u%u%s%s", (void*)t, t->fmt, t->w,
                t->h, t->levels, sm->srgb, sm->addr_u, sm->addr_v, sm->addr_w, sm->min, sm->mag, sm->mip,
                (po->shadow_mask >> s) & 1 ? " SHADOW" : "", (po->depth_mask >> s) & 1 ? " DEPTH" : "");
        else
            fprintf(g_trace, " (none)");
    }
    fprintf(g_trace, " | el");
    for (uint32_t j = 0; j < d->nel; ++j)
        fprintf(g_trace, " %u.%u:t%u@%u/%u", d->el[j].usage, d->el[j].index, d->el[j].type, d->el[j].stream,
            d->el[j].offset);
    fprintf(g_trace, "\n");
    char p[1024];
    snprintf(p, sizeof p, "%s/consts.bin", g_trace_dir);
    FILE* cb = fopen(p, "ab");
    if (cb)
    {
        fwrite(d->vsc, 16, G9_VS_FLOATS, cb);
        fwrite(d->psc, 16, G9_PS_FLOATS, cb);
        fwrite(d->vsi, 16, 16, cb);
        fwrite(d->psi, 16, 16, cb);
        fwrite(d->vsb, 4, 16, cb);
        fwrite(d->psb, 4, 16, cb);
        fclose(cb);
    }
    trace_msl(d->vs, vo);
    trace_msl(d->ps, po);
    g_trace_draw++;
}

void g9_draw(const G9Draw* d)
{
    if (!d->vs || !d->ps || !d->count)
        return;
    fx_hook(d);
    fx_note_draw(d);
    if (!begin_pass())
        return;
    @autoreleasepool
    {
        /* --- the shader variants this draw needs --- */
        SmOpts vo, po;
        memset(&vo, 0, sizeof vo);
        memset(&po, 0, sizeof po);
        PipeKey k;
        memset(&k, 0, sizeof k);
        uint32_t streams_used = 0;
        const SmInfo* vi = &d->vs->info;
        for (uint32_t i = 0; i < vi->nin; ++i)
        {
            const SmDecl* in = &vi->in[i];
            uint32_t reg = in->reg;
            const G9Elem* e = NULL;
            for (uint32_t j = 0; j < d->nel; ++j)
                if (d->el[j].usage == in->usage && d->el[j].index == in->index)
                {
                    e = &d->el[j];
                    break;
                }
            if (!e && in->usage == SM_POSITION) /* POSITIONT feeds a shader's position too */
                for (uint32_t j = 0; j < d->nel; ++j)
                    if (d->el[j].usage == SM_POSITIONT && d->el[j].index == in->index)
                        e = &d->el[j];
            if (e && e->stream < G9_MAX_STREAMS && d->stream[e->stream].stride)
            {
                uint8_t is_int;
                MTLVertexFormat f = vertex_format(e->type, &is_int);
                vo.attr_int[reg] = is_int;
                streams_used |= 1u << e->stream;
                k.attr[reg].fmt = (uint8_t)f, k.attr[reg].stream = e->stream, k.attr[reg].offset = e->offset;
                k.attr[reg].attr = 1;
            }
            else
            {
                /* nothing supplies it: (0, 0, 0, 1) from the default buffer */
                streams_used |= 1u << 15;
                k.attr[reg].attr = 2;
            }
        }
        for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
            if (streams_used & (1u << s))
            {
                if (s == 15 && !(d->stream[15].stride))
                    continue;
                k.stride[s] = d->stream[s].stride;
                if (d->stream[s].freq & 0x80000000u) /* D3DSTREAMSOURCE_INSTANCEDATA */
                    k.step_instance[s] = 1;
            }
        /* what the pixel shader reads that the vertex shader does not write */
        const SmInfo* pi = &d->ps->info;
        for (uint32_t i = 0; i < pi->nin; ++i)
        {
            int found = 0;
            for (uint32_t j = 0; j < vi->nout; ++j)
                found |= vi->out[j].usage == pi->in[i].usage && vi->out[j].index == pi->in[i].index;
            if (!found && vo.nvs_extra < 16)
                vo.vs_extra[vo.nvs_extra++] = pi->in[i];
        }
        po.alpha_func = d->rs.alpha_func;
        /* a pixel shader's oDepth with no depth buffer bound: D3D9 drops it (the effects' projection
         * map pass draws so, e.g. the ground circles of Ifrit's Radiant Plume and Eruption) */
        po.no_depth_out = pi->writes_depth && !g_ds;
        for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
            if (is_depth_format(d->tex[s]) && pi->sampler[s] == 2)
            {
                if (d->tex[s]->f.depth_read)
                    po.depth_mask |= (uint16_t)(1u << s);
                else
                    po.shadow_mask |= (uint16_t)(1u << s);
            }
        int background = d->rs.zenable && g_ds; /* a draw of the 3D scene: background compiles */
        Compiled *vc = shader_variant(d->vs, &vo, background), *pc = shader_variant(d->ps, &po, background);
        if (!compiled_ready(vc, !background) || !compiled_ready(pc, !background))
            return;
        id<MTLFunction> vf = vc->obj, pf = pc->obj;
        if (!vf || !pf)
            return;
        k.vs = fnv64(&vo, sizeof vo) ^ ((uint64_t)vi->hash << 32);
        k.ps = fnv64(&po, sizeof po) ^ ((uint64_t)pi->hash << 32);
        for (int i = 0; i < G9_MAX_RT; ++i)
        {
            k.color[i] = rt_format(i);
            k.mask[i] = d->rs.write_mask[i];
        }
        if (d->rs.srgb_write && k.color[0] == MTLPixelFormatBGRA8Unorm)
            k.srgb = 1;
        k.depth = ds_format();
        if (d->rs.blend)
        {
            k.blend = 1, k.src = d->rs.src, k.dst = d->rs.dst, k.op = d->rs.op;
            k.sep = d->rs.sep_alpha;
            if (k.sep)
                k.src_a = d->rs.src_a, k.dst_a = d->rs.dst_a, k.op_a = d->rs.op_a;
        }

        k.topology = d->prim == 1 ? 1 : d->prim <= 3 ? 2 : 3;

        /* --- the pipeline --- */
        uint64_t h;
        Compiled* pcomp = (Compiled*)cache_get(&g_pipes, &k, sizeof k, &h);
        if (!pcomp)
        {
            pcomp = (Compiled*)calloc(1, sizeof *pcomp);
            cache_put(&g_pipes, &k, sizeof k, h, (id)pcomp);
            MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
            pd.vertexFunction = vf;
            pd.fragmentFunction = pf;
            /* the vertex layout, made only for a new pipeline: allocating one each draw was a
             * tenth of the render thread */
            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            for (uint32_t r = 0; r < sizeof k.attr / sizeof k.attr[0]; ++r)
                if (k.attr[r].attr == 1)
                {
                    vd.attributes[r].format = (MTLVertexFormat)k.attr[r].fmt;
                    vd.attributes[r].offset = k.attr[r].offset;
                    vd.attributes[r].bufferIndex = k.attr[r].stream;
                }
                else if (k.attr[r].attr == 2)
                {
                    vd.attributes[r].format = MTLVertexFormatFloat4;
                    vd.attributes[r].offset = 0;
                    vd.attributes[r].bufferIndex = 15;
                }
            for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
                if (streams_used & (1u << s))
                {
                    if (s == 15 && !(d->stream[15].stride))
                    {
                        vd.layouts[15].stepFunction = MTLVertexStepFunctionConstant;
                        vd.layouts[15].stepRate = 0;
                        vd.layouts[15].stride = 16;
                        continue;
                    }
                    vd.layouts[s].stride = (k.stride[s] + 3) & ~3u;
                    if (k.step_instance[s])
                    {
                        vd.layouts[s].stepFunction = MTLVertexStepFunctionPerInstance;
                        vd.layouts[s].stepRate = d->stream[s].freq & 0x3FFFFFFFu ? d->stream[s].freq & 0x3FFFFFFFu : 1;
                    }
                }
            pd.vertexDescriptor = vd;
            for (int i = 0; i < G9_MAX_RT; ++i)
            {
                MTLRenderPipelineColorAttachmentDescriptor* a = pd.colorAttachments[i];
                a.pixelFormat = k.color[i];
                if (k.color[i] == MTLPixelFormatInvalid)
                    continue;
                uint8_t m = k.mask[i];
                /* D3DCOLORWRITEENABLE: R 1, G 2, B 4, A 8 */
                a.writeMask = ((m & 1) ? MTLColorWriteMaskRed : 0) | ((m & 2) ? MTLColorWriteMaskGreen : 0) |
                              ((m & 4) ? MTLColorWriteMaskBlue : 0) | ((m & 8) ? MTLColorWriteMaskAlpha : 0);
                if (k.blend)
                {
                    a.blendingEnabled = YES;
                    a.sourceRGBBlendFactor = blend_factor(k.src);
                    a.destinationRGBBlendFactor = blend_factor(k.dst);
                    a.rgbBlendOperation = blend_op(k.op);
                    a.sourceAlphaBlendFactor = blend_factor(k.sep ? k.src_a : k.src);
                    a.destinationAlphaBlendFactor = blend_factor(k.sep ? k.dst_a : k.dst);
                    a.alphaBlendOperation = blend_op(k.sep ? k.op_a : k.op);
                    if (k.src == 12) /* BOTHSRCALPHA */
                        a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha,
                        a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    else if (k.src == 13) /* BOTHINVSRCALPHA */
                        a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha,
                        a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                }
            }
            pd.depthAttachmentPixelFormat = k.depth;
            if (k.depth == MTLPixelFormatDepth32Float_Stencil8)
                pd.stencilAttachmentPixelFormat = k.depth;
            pd.inputPrimitiveTopology = d->prim == 1 ? MTLPrimitiveTopologyClassPoint
                                        : d->prim <= 3 ? MTLPrimitiveTopologyClassLine
                                                       : MTLPrimitiveTopologyClassTriangle;
            static uint32_t made;
            if (++made % 100 == 0)
                rt_log("[gfx9] %u pipelines made\n", made);
            uint32_t vh = vi->hash, ph = pi->hash;
            Compiled* pc2 = pcomp;
            void (^done)(id<MTLRenderPipelineState>, NSError*) = ^(id<MTLRenderPipelineState> p, NSError* e) {
                if (!p && g_draw_errors++ < 20)
                    rt_log("[gfx9] pipeline (vs %08x, ps %08x): %s\n", vh, ph, e.localizedDescription.UTF8String);
                compiled_set(pc2, [p retain]);
            };
            if (background)
                [g_dev newRenderPipelineStateWithDescriptor:pd completionHandler:done];
            else
            {
                NSError* e = nil;
                id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
                done(p, e);
                [p release];
            }
        }
        if (!compiled_ready(pcomp, !background))
            return;
        id<MTLRenderPipelineState> pipe = pcomp->obj;
        if (!pipe)
            return;

        /* --- state --- */
        id<MTLRenderCommandEncoder> enc = g_enc;
        [enc setRenderPipelineState:pipe];
        [enc setDepthStencilState:depth_state(&d->rs)];
        [enc setStencilReferenceValue:d->rs.stencil_ref & 0xFF];
        /* D3DCULL: NONE 1, CW 2 (cull clockwise), CCW 3; D3D9's triangles are clockwise when front */
        [enc setFrontFacingWinding:MTLWindingClockwise];
        [enc setCullMode:d->rs.cull == 2 ? MTLCullModeFront : d->rs.cull == 3 ? MTLCullModeBack : MTLCullModeNone];
        [enc setTriangleFillMode:d->rs.fill == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [enc setDepthBias:d->rs.depth_bias * 16777215.0f slopeScale:d->rs.slope_bias clamp:0];
        uint32_t bf = d->rs.blend_factor;
        [enc setBlendColorRed:((bf >> 16) & 255) / 255.0f green:((bf >> 8) & 255) / 255.0f blue:(bf & 255) / 255.0f alpha:(bf >> 24) / 255.0f];
        /* the game's pixels in the targets' (scene targets at the render scale: gfx9_upscale.h) */
        uint32_t tw, th;
        float sx, sy;
        target_real_size(&tw, &th, &sx, &sy);
        float vw = d->vp.w * sx, vh = d->vp.h * sy;
        MTLViewport mv = { d->vp.x * sx, d->vp.y * sy, vw, vh, d->vp.minz, d->vp.maxz };
        [enc setViewport:mv];
        MTLScissorRect sc = { 0, 0, tw, th };
        if (d->rs.scissor)
        {
            int32_t s0 = (int32_t)lroundf(d->scissor[0] * sx), s1 = (int32_t)lroundf(d->scissor[1] * sy);
            int32_t s2 = (int32_t)lroundf(d->scissor[2] * sx), s3 = (int32_t)lroundf(d->scissor[3] * sy);
            int32_t l = s0 > 0 ? s0 : 0, t = s1 > 0 ? s1 : 0;
            int32_t r = s2 < (int32_t)tw ? s2 : (int32_t)tw;
            int32_t b = s3 < (int32_t)th ? s3 : (int32_t)th;
            if (r <= l || b <= t)
                return; /* nothing visible */
            sc = (MTLScissorRect){ (NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t) };
        }
        [enc setScissorRect:sc];

        /* --- constants --- */
        [enc setVertexBytes:d->vsc length:G9_VS_FLOATS * 16 atIndex:SM_BUF_FLOAT];
        [enc setVertexBytes:d->vsi length:16 * 16 atIndex:SM_BUF_INT];
        [enc setVertexBytes:d->vsb length:16 * 4 atIndex:SM_BUF_BOOL];
        [enc setFragmentBytes:d->psc length:G9_PS_FLOATS * 16 atIndex:SM_BUF_FLOAT];
        [enc setFragmentBytes:d->psi length:16 * 16 atIndex:SM_BUF_INT];
        [enc setFragmentBytes:d->psb length:16 * 4 atIndex:SM_BUF_BOOL];
        /* D3D9's pixel centres are on integer coordinates, Metal's on half-integers: geometry moves half a
         * pixel right and down (+1/w, -1/h in clip space; as wined3d and DXVK do) */
        SmAux aux = { { 1.0f / (vw >= 1.0f ? vw : 1.0f), -1.0f / (vh >= 1.0f ? vh : 1.0f), 0, 0 },
                      { (d->rs.alpha_ref & 255) / 255.0f, 0, 0, 0 } };
        for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
            aux.lod_bias[s] = d->samp[s].lod_bias;
        memcpy(aux.clip, d->clip, sizeof aux.clip);
        aux.clip_on[0] = d->clip_on;
        up_draw_aux(d, &aux, sx, sy);
        [enc setVertexBytes:&aux length:sizeof aux atIndex:SM_BUF_AUX];
        [enc setFragmentBytes:&aux length:sizeof aux atIndex:SM_BUF_AUX];

        /* --- textures --- */
        for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
        {
            if (!pi->sampler[s])
                continue;
            G9Tex* t = d->tex[s];
            id<MTLTexture> mt = t ? t->tex : nil;
            if (t && d->samp[s].srgb && t->tex.pixelFormat == MTLPixelFormatBGRA8Unorm)
            {
                if (!t->srgb)
                    t->srgb = [t->tex newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
                mt = t->srgb;
            }
            [enc setFragmentTexture:mt atIndex:s];
            G9Sampler aniso;
            [enc setFragmentSamplerState:sampler(fx_sampler(d, s, &aniso), (po.shadow_mask >> s) & 1) atIndex:s];
        }
        for (uint32_t s = 0; s < G9_VS_SAMPLERS; ++s)
        {
            if (!vi->sampler[s])
                continue;
            G9Tex* t = d->tex[G9_PS_SAMPLERS + s];
            [enc setVertexTexture:t ? t->tex : nil atIndex:s];
            [enc setVertexSamplerState:sampler(&d->samp[G9_PS_SAMPLERS + s], 0) atIndex:s];
        }

        /* --- vertices --- */
        for (uint32_t s = 0; s < G9_MAX_STREAMS; ++s)
        {
            if (!(streams_used & (1u << s)))
                continue;
            const G9Stream* st = &d->stream[s];
            if (s == 15 && !st->stride)
            {
                [enc setVertexBuffer:g_default_attr offset:0 atIndex:15];
                continue;
            }
            if (st->stride & 3)
            {
                /* Metal strides are multiples of 4: copy the vertices out at the layout's padded stride */
                if (!st->data)
                    return;
                uint32_t padded = (st->stride + 3) & ~3u, n = st->size / st->stride;
                id<MTLBuffer> rb;
                uint32_t off;
                uint8_t* p = (uint8_t*)ring(n * padded, 16, &rb, &off);
                if (!p)
                    return;
                for (uint32_t v = 0; v < n; ++v)
                    memcpy(p + v * padded, (const uint8_t*)st->data + v * st->stride, st->stride);
                [enc setVertexBuffer:rb offset:off atIndex:s];
            }
            else if (st->buf && !(st->offset & 3))
                [enc setVertexBuffer:st->buf->buf offset:st->offset atIndex:s];
            else if (st->data)
            {
                id<MTLBuffer> rb;
                uint32_t off;
                void* p = ring(st->size, 16, &rb, &off);
                if (!p)
                    return;
                memcpy(p, st->data, st->size);
                [enc setVertexBuffer:rb offset:off atIndex:s];
            }
            else
                return;
        }

        if (g_trace)
            trace_draw(d, &vo, &po, &k);

        /* --- the draw --- */
        MTLPrimitiveType mp;
        uint32_t n;
        draw_prim(&mp, d->prim, d->count, &n);
        uint32_t inst = d->instances ? d->instances : 1;
        if (d->index_size)
        {
            id<MTLBuffer> ib = nil;
            uint32_t ioff = 0;
            uint32_t isz = d->index_size;
            if (d->prim == 6 || (d->ibuf && ((d->ibuf_offset + d->first * isz) & 3)) || !d->ibuf)
            {
                /* fans become lists; unaligned or client indices go through the ring */
                const uint8_t* src = d->idata ? (const uint8_t*)d->idata : NULL;
                if (!src)
                    return; /* the front end passes idata for these */
                uint32_t* out = (uint32_t*)ring(n * 4, 16, &ib, &ioff);
                if (!out)
                    return;
                for (uint32_t i = 0; i < n; ++i)
                {
                    uint32_t at = d->prim == 6 ? (i % 3 == 0 ? 0 : i / 3 + i % 3) : i;
                    out[i] = isz == 2 ? ((const uint16_t*)src)[d->first + at] : ((const uint32_t*)src)[d->first + at];
                }
                isz = 4;
            }
            else
            {
                ib = d->ibuf->buf;
                ioff = d->ibuf_offset + d->first * isz;
            }
            [enc drawIndexedPrimitives:mp
                            indexCount:n
                             indexType:isz == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:ib
                     indexBufferOffset:ioff
                         instanceCount:inst
                            baseVertex:d->base_vertex
                          baseInstance:0];
        }
        else if (d->prim == 6)
        {
            id<MTLBuffer> ib;
            uint32_t ioff;
            uint32_t* out = (uint32_t*)ring(n * 4, 16, &ib, &ioff);
            if (!out)
                return;
            for (uint32_t i = 0; i < n; ++i)
                out[i] = d->first + (i % 3 == 0 ? 0 : i / 3 + i % 3);
            [enc drawIndexedPrimitives:mp indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:ioff
                         instanceCount:inst baseVertex:0 baseInstance:0];
        }
        else
            [enc drawPrimitives:mp vertexStart:d->first vertexCount:n instanceCount:inst];
        g_pass_draws++;
    }
}

/* --- present ------------------------------------------------------------------------------------ */

/* "60.0 FPS 16.7 MS" as glyph indices, remade every half second; returns the character count */
static int fps_text(uint32_t* out)
{
    static uint64_t since;
    static uint32_t frames;
    static char text[32] = "";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!since)
        since = now;
    frames++;
    if (now - since >= 500000000ull)
    {
        double sec = (now - since) / 1e9;
        snprintf(text, sizeof text, "%.1f FPS %.1f MS", frames / sec, 1000.0 * sec / frames);
        since = now;
        frames = 0;
    }
    int n = 0;
    for (const char* c = text; *c && n < 24; ++c)
        out[n++] = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == '.' ? 10 : *c == 'F' ? 11
                   : *c == 'P' ? 12 : *c == 'S' ? 13 : *c == 'M' ? 14 : 15;
    return n;
}

/* XIV_FRAMELOG=1: frame pacing, every 600 frames: the time between presents, and how many of the
 * display's refreshes each frame stayed on screen (Core Animation's presentedTime). A frame rate can
 * be high and still judder when frames alternate between one and two refreshes. */
#define FL_N 600
static int g_framelog = -1;
static double g_fl_cpu[FL_N], g_fl_shown[FL_N];
static volatile uint32_t g_fl_ncpu, g_fl_nshown, g_fl_dropped;

static int fl_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void fl_stats(const char* what, double* v, uint32_t n, double refresh)
{
    if (!n)
        return;
    qsort(v, n, sizeof *v, fl_cmp);
    double sum = 0, sq = 0;
    for (uint32_t i = 0; i < n; ++i)
        sum += v[i], sq += v[i] * v[i];
    double mean = sum / n, sd = sqrt(fmax(0, sq / n - mean * mean));
    char hist[160] = "";
    if (refresh > 0)
    {
        /* frames per whole number of refreshes they spanned */
        uint32_t bins[7] = { 0 };
        for (uint32_t i = 0; i < n; ++i)
        {
            long k = lround(v[i] / refresh);
            bins[k < 0 ? 0 : k > 6 ? 6 : k]++;
        }
        int o = snprintf(hist, sizeof hist, " | refreshes:");
        for (int k = 0; k < 7; ++k)
            if (bins[k])
                o += snprintf(hist + o, sizeof hist - (size_t)o, " %s%d=%u", k == 6 ? ">=" : "", k, bins[k]);
    }
    rt_log("[pace] %s ms: mean %.2f sd %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f%s\n", what, mean * 1e3, sd * 1e3,
           v[n / 2] * 1e3, v[n * 95 / 100] * 1e3, v[n * 99 / 100] * 1e3, v[n - 1] * 1e3, hist);
}

static void framelog(id<CAMetalDrawable> drawable)
{
    static double last_cpu;
    double now = CACurrentMediaTime();
    if (last_cpu)
        g_fl_cpu[g_fl_ncpu++ % FL_N] = now - last_cpu;
    last_cpu = now;
    if (drawable)
        [drawable addPresentedHandler:^(id<MTLDrawable> d) {
            /* on one Core Animation queue, in present order */
            static double last;
            double t = d.presentedTime;
            if (t == 0)
            {
                __atomic_fetch_add(&g_fl_dropped, 1, __ATOMIC_RELAXED);
                return;
            }
            if (last)
                g_fl_shown[__atomic_fetch_add(&g_fl_nshown, 1, __ATOMIC_RELAXED) % FL_N] = t - last;
            last = t;
        }];
    if (g_fl_ncpu < FL_N)
        return;
    float hz = 0;
    SDL_DisplayID disp = g_sdl_window ? SDL_GetDisplayForWindow((SDL_Window*)g_sdl_window) : 0;
    const SDL_DisplayMode* dm = disp ? SDL_GetCurrentDisplayMode(disp) : NULL;
    if (dm)
        hz = dm->refresh_rate;
    double refresh = hz > 0 ? 1.0 / hz : 0;
    static double cpu[FL_N], shown[FL_N];
    uint32_t ns = g_fl_nshown < FL_N ? g_fl_nshown : FL_N;
    memcpy(cpu, g_fl_cpu, sizeof cpu);
    memcpy(shown, g_fl_shown, ns * sizeof *shown);
    rt_log("[pace] %.0f Hz display, %u not shown\n", hz, g_fl_dropped);
    fl_stats("cpu  ", cpu, FL_N, refresh);
    fl_stats("shown", shown, ns, refresh);
    g_fl_ncpu = g_fl_nshown = g_fl_dropped = 0;
}

/* Pacing (XIV_PACE=1, with vsync; off by default: the game's main thread is the limit, and a frame
 * time near a refresh multiple held it a step lower): every frame stays on screen the same whole
 * number of refreshes k, and the game's frame limiter is told refresh/k (g9_pace_hook). Without it a
 * game at 60 fps on a 100 Hz display shows frames for 2, 2, 1 refreshes: smooth numbers, juddering
 * motion. k goes up when over a tenth of a half second's frames miss their refresh, and is tried one
 * lower after a run with no misses (backing off when that fails again). */
void (*g9_pace_hook)(uint32_t fps);
static int g_pace = -1;
static double g_refresh;
static volatile uint32_t g_pace_k = 1, g_pace_frames, g_pace_misses;

/* the display's refresh in ns while presents are paced, else 0 */
uint64_t g9_pace_refresh_ns(void)
{
    return g_pace > 0 && g_refresh > 0 ? (uint64_t)(g_refresh * 1e9 + 0.5) : 0;
}

static void pace_shown(double dt)
{
    double r = g_refresh;
    if (r <= 0)
        return;
    __atomic_fetch_add(&g_pace_frames, 1, __ATOMIC_RELAXED);
    if (dt > (__atomic_load_n(&g_pace_k, __ATOMIC_RELAXED) + 0.5) * r)
        __atomic_fetch_add(&g_pace_misses, 1, __ATOMIC_RELAXED);
}

static void pace_set(uint32_t k, const char* why)
{
    __atomic_store_n(&g_pace_k, k, __ATOMIC_RELAXED);
    uint32_t fps = (uint32_t)ceil(1.0 / (g_refresh * k));
    rt_log("[pace] %.0f Hz / %u: %u fps (%s)\n", 1.0 / g_refresh, k, fps, why);
    if (g9_pace_hook)
        g9_pace_hook(fps);
}

/* once a frame: the display's rate, and k from the last half second's misses */
static void pace_update(void)
{
    static double window_at, probe_at, probe_wait = 4, probed_from;
    double now = CACurrentMediaTime();
    if (!window_at || now - window_at >= 0.5)
    {
        float hz = 0;
        SDL_DisplayID disp = g_sdl_window ? SDL_GetDisplayForWindow((SDL_Window*)g_sdl_window) : 0;
        const SDL_DisplayMode* dm = disp ? SDL_GetCurrentDisplayMode(disp) : NULL;
        if (dm)
            hz = dm->refresh_rate;
        if (hz < 20)
            hz = 60;
        uint32_t frames = __atomic_exchange_n(&g_pace_frames, 0, __ATOMIC_RELAXED);
        uint32_t misses = __atomic_exchange_n(&g_pace_misses, 0, __ATOMIC_RELAXED);
        uint32_t k = g_pace_k;
        if (fabs(1.0 / hz - g_refresh) > 1e-4)
        {
            /* a new display: start at the game's 60 fps, rounded to whole refreshes */
            g_refresh = 1.0 / hz;
            k = (uint32_t)ceil(hz / 60.0 - 0.05);
            pace_set(k ? k : 1, "display");
            probe_at = now + probe_wait;
        }
        /* never paced below about 30 fps: a frame time that varies (a town full of people) misses
         * at any k, and each step down tells the game's limiter to wait longer still */
        else if (frames >= 4 && misses * 10 > frames && k < (uint32_t)fmax(1.0, floor(hz / 30.0)))
        {
            if (probed_from && now - probed_from < 1.5)
                probe_wait = fmin(probe_wait * 2, 64);
            probed_from = 0;
            pace_set(k + 1, "missed refreshes");
            probe_at = now + probe_wait;
        }
        else if (misses)
            probe_at = now + probe_wait;
        else if (k > 1 && now >= probe_at && 1.0 / (g_refresh * (k - 1)) <= 120.5)
        {
            probed_from = now;
            pace_set(k - 1, "trying faster");
            probe_at = now + probe_wait;
        }
        else if (probed_from && now - probed_from >= 1.5)
            probed_from = 0, probe_wait = fmax(4, probe_wait / 2);
        window_at = now;
    }
}

void g9_present(G9Tex* bb)
{
    @autoreleasepool
    {
        if (g_pending_clear)
            begin_pass();
        end_pass();
        fit_drawable(bb);
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (g_framelog < 0)
            g_framelog = getenv("XIV_FRAMELOG") && strcmp(getenv("XIV_FRAMELOG"), "0");
        if (g_framelog)
            framelog(drawable);
        if (g_pace < 0)
            g_pace = g_layer.displaySyncEnabled && getenv("XIV_PACE") && strcmp(getenv("XIV_PACE"), "0");
        if (g_pace)
        {
            pace_update();
            if (drawable)
                [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                    static double last;
                    double t = d.presentedTime;
                    if (t && last)
                        pace_shown(t - last);
                    if (t)
                        last = t;
                }];
        }
        if (drawable && bb)
        {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:g_present_pipe];
            /* the back buffer scaled to the drawable, aspect kept */
            float dw = (float)drawable.texture.width, dh = (float)drawable.texture.height;
            float s = fminf(dw / bb->w, dh / bb->h), w = bb->w * s / dw, h = bb->h * s / dh;
            float q[4] = { -w, h, w, -h }, uv[4] = { 0, 0, 1, 1 };
            [e setVertexBytes:q length:16 atIndex:0];
            [e setVertexBytes:uv length:16 atIndex:1];
            [e setFragmentTexture:bb->tex atIndex:0];
            [e setFragmentSamplerState:g_linear atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            uint32_t chars[24];
            int n = fps_text(chars);
            if (g_text_pipe && n)
            {
                /* top left of the picture, a little in */
                float px = fmaxf(2.0f, roundf(dh / 360.0f));
                float x0 = (1.0f - w) * 0.5f * dw + 4 * px, y0 = (1.0f - h) * 0.5f * dh + 4 * px;
                float bw = (n * 4 + 1) * px, bh = 7 * px;
                float r[4] = { x0 / dw * 2 - 1, 1 - y0 / dh * 2, (x0 + bw) / dw * 2 - 1, 1 - (y0 + bh) / dh * 2 };
                float info[4] = { x0, y0, px, (float)n };
                [e setRenderPipelineState:g_text_pipe];
                [e setVertexBytes:r length:16 atIndex:0];
                [e setVertexBytes:uv length:16 atIndex:1];
                [e setFragmentBytes:chars length:sizeof chars atIndex:0];
                [e setFragmentBytes:info length:16 atIndex:1];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [e endEncoding];
            if (g_pace && g_refresh > 0)
                [cmd() presentDrawable:drawable afterMinimumDuration:(g_pace_k - 0.5) * g_refresh];
            else
                [cmd() presentDrawable:drawable];
        }
        g9_flush_frame(0);
        trace_frame_end();
        up_frame_end(bb);
        fx_frame_end(bb);
    }
}

void g9_finish(void)
{
    g9_flush_frame(1);
}
