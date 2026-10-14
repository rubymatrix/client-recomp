/* Upscaling for the Direct3D 9 back end: part of gfx9_metal.m (included after gfx9_fx.h; it uses
 * that file's device, textures and the scene effects' view-projection vote).
 *
 * The game draws its 3D scene into render targets the size of the back buffer, copies the
 * finished scene into the target the interface is drawn on, then copies that into the back
 * buffer (gfx9_fx.h). Upscaling makes the scene's targets smaller than the game thinks:
 *
 *   - the scene's targets: every 2D render target and depth buffer the size of the back buffer,
 *     except the back buffer and the interface's target. Their textures are made at the render
 *     scale; the game keeps seeing its own size (G9Tex w, h), and viewports, scissors, clear and
 *     StretchRect rectangles and pixel shaders' vPos are scaled between the two (rw, rh);
 *   - at the copy into the interface's target (fx_hook), the scene is upscaled to the game's size
 *     and stands in for the scene's texture until the frame ends, so the copy and the effects
 *     read it at full size and the interface is drawn sharp over it;
 *   - spatial: MetalFX's spatial scaler on the finished scene;
 *   - temporal: MetalFX's temporal scaler. The scene's draws are jittered by a sub-pixel offset
 *     each frame (a Halton sequence, added to the half-pixel offset the vertex shaders already
 *     apply), textures get a negative LOD bias, and motion vectors come from the scene's depth
 *     and the camera: each pixel is taken back to the world with this frame's view-projection
 *     and forward with the last frame's (the scene effects' vote: VP = W^-1 * WVP). Things that
 *     move on their own (characters) get the camera's motion only: MetalFX's history rejection
 *     has to cope with them.
 *
 * Settings (fx.txt / XIV_FX_*): upscale 0 off, 1 spatial, 2 temporal; render_scale the scene's
 * share of the back buffer's size per axis (temporal at 1 is anti-aliasing only); up_debug 1
 * shows the motion vectors; up_jitter the jitter's size (0 none); up_jx, up_jy, up_mv the signs
 * of what MetalFX is told the jitter and motion are. */

#import <MetalFX/MetalFX.h>

static const char UP_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct UO { float4 pos [[position]]; };\n"
    "vertex UO up_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  UO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o;\n"
    "}\n"
    /* m: this frame's clip space (unjittered) to the last frame's, column vectors */
    "struct MU { float4x4 m; float4 jit; float4 size; };\n"
    "struct MO { float4 mv [[color(0)]]; float d [[depth(any)]]; };\n"
    /* motion in UV units (where the pixel was minus where it is) and the depth, copied to a
     * depth-only texture MetalFX takes */
    "fragment MO up_motion(UO in [[stage_in]], constant MU& u [[buffer(0)]], depth2d<float> dt [[texture(0)]]) {\n"
    "  float z = dt.read(uint2(in.pos.xy));\n"
    "  float2 ndc = in.pos.xy * u.size.zw * float2(2.0, -2.0) + float2(-1.0, 1.0) - u.jit.xy;\n"
    "  float4 q = u.m * float4(ndc, z, 1.0);\n"
    "  float2 mv = q.w > 1e-6 ? (q.xy / q.w - ndc) * float2(0.5, -0.5) : float2(0.0);\n"
    "  MO o; o.mv = float4(mv, 0.0, 0.0); o.d = z; return o;\n"
    "}\n"
    "fragment float4 up_show(UO in [[stage_in]], constant float4& r [[buffer(0)]], texture2d<float> mv [[texture(0)]]) {\n"
    "  float2 m = mv.read(uint2(in.pos.xy * r.xy)).xy * r.zw;\n"
    "  return float4(0.5 + m * 0.05, 0.5, 1.0);\n"
    "}\n";

typedef struct UpMU
{
    float m[16]; /* column-major for MSL's float4x4 */
    float jit[4], size[4];
} UpMU;

#define UP_MAX_RTS 512

static struct
{
    int tried, ok;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> motion, show;
    id<MTLDepthStencilState> zalways;
    G9Tex* rts[UP_MAX_RTS]; /* the 2D render targets and depth buffers that could be the scene's */
    uint32_t nrts;
    int mode;               /* this frame's: 0 off, 1 spatial, 2 temporal */
    float scale;            /* the render scale the scene's targets are made at */
    G9Tex* bb;              /* ... for this back buffer and interface target */
    G9Tex* ui;
    G9Tex* swapped;         /* the scene texture the upscaled picture stands in for this frame */
    id<MTLTexture> swapped_low;
    id<MTLFXSpatialScaler> spatial;
    id<MTLFXTemporalScaler> temporal;
    id<MTLTexture> out, mv, depth;
    float jx, jy;           /* this frame's jitter, pixels */
    uint32_t phase;
    float prev_vp[16];
    int have_prev, reset;
    int logged;
} g_up;

static int up_init(void)
{
    if (g_up.tried)
        return g_up.ok;
    g_up.tried = 1;
    @autoreleasepool
    {
        NSError* err = nil;
        g_up.lib = [g_dev newLibraryWithSource:@(UP_MSL) options:nil error:&err];
        if (!g_up.lib)
        {
            rt_log("[gfx9] upscaler shaders: %s\n", err.localizedDescription.UTF8String);
            return 0;
        }
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[g_up.lib newFunctionWithName:@"up_vs"] autorelease];
        pd.fragmentFunction = [[g_up.lib newFunctionWithName:@"up_motion"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        g_up.motion = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        pd.fragmentFunction = [[g_up.lib newFunctionWithName:@"up_show"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
        g_up.show = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor new] autorelease];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        g_up.zalways = [g_dev newDepthStencilStateWithDescriptor:dd];
        g_up.ok = g_up.motion && g_up.show && g_up.zalways;
        if (!g_up.ok)
            rt_log("[gfx9] upscaler pipelines: %s\n", err ? err.localizedDescription.UTF8String : "?");
        else
            rt_log("[gfx9] upscaler ready (MetalFX spatial %s, temporal %s, temporal scale %.2f-%.2f)\n",
                [MTLFXSpatialScalerDescriptor supportsDevice:g_dev] ? "yes" : "no",
                [MTLFXTemporalScalerDescriptor supportsDevice:g_dev] ? "yes" : "no",
                [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:g_dev],
                [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev]);
    }
    return g_up.ok;
}

/* --- the scene's targets at the render scale ------------------------------------------------------ */

static void up_rescale(G9Tex* t, float s);

/* a new render target: one the size of the scene's is made at the render scale at once (a frame
 * with its targets at different sizes cannot be drawn) */
static void up_track(G9Tex* t)
{
    if (t->type != G9_TEX_2D || t->levels != 1 || (t->use != G9_USE_RT && t->use != G9_USE_DEPTH) ||
        g_up.nrts >= UP_MAX_RTS)
        return;
    g_up.rts[g_up.nrts++] = t;
    if (g_up.bb && g_up.ui && t->w == g_up.bb->w && t->h == g_up.bb->h)
    {
        t->scene = 1;
        up_rescale(t, g_up.scale);
    }
}

static void up_set_tex(G9Tex* t, id<MTLTexture> tex)
{
    [t->tex release];
    t->tex = tex;
    t->rw = (uint32_t)tex.width, t->rh = (uint32_t)tex.height;
    [t->srgb release];
    t->srgb = nil;
}

/* the upscaled picture stood in for the scene's texture: the scene's own back */
static void up_unswap(void)
{
    if (!g_up.swapped)
        return;
    up_set_tex(g_up.swapped, g_up.swapped_low);
    g_up.swapped = NULL;
    g_up.swapped_low = nil;
}

/* t at s of its size (its own texture again at 1) */
static void up_rescale(G9Tex* t, float s)
{
    uint32_t w = s < 1.0f ? (uint32_t)fmaxf(1.0f, roundf(t->w * s)) : t->w;
    uint32_t h = s < 1.0f ? (uint32_t)fmaxf(1.0f, roundf(t->h * s)) : t->h;
    if (w == t->rw && h == t->rh)
        return;
    if (w == t->w && h == t->h)
    {
        id<MTLTexture> full = t->full;
        t->full = nil;
        up_set_tex(t, full);
        return;
    }
    id<MTLTexture> base = t->full ? t->full : t->tex;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:base.pixelFormat
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.usage = base.usage;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> low = [g_dev newTextureWithDescriptor:td];
    if (!low)
        return;
    if (!t->full)
    {
        t->full = t->tex;
        t->tex = nil;
    }
    up_set_tex(t, low);
}

static void up_forget(G9Tex* t)
{
    if (g_up.swapped == t)
        up_unswap();
    for (uint32_t i = 0; i < g_up.nrts; ++i)
        if (g_up.rts[i] == t)
        {
            g_up.rts[i] = g_up.rts[--g_up.nrts];
            break;
        }
    if (g_up.bb == t)
        g_up.bb = NULL;
    if (g_up.ui == t)
        g_up.ui = NULL;
    [t->full release];
    t->full = nil;
}

/* which targets are the scene's, and at what scale (between frames) */
static void up_choose(G9Tex* bb, G9Tex* ui, float scale)
{
    if (bb == g_up.bb && ui == g_up.ui && scale == g_up.scale)
        return;
    g_up.bb = bb, g_up.ui = ui, g_up.scale = scale;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_up.nrts; ++i)
    {
        G9Tex* t = g_up.rts[i];
        t->scene = bb && ui && t != bb && t != ui && t->w == bb->w && t->h == bb->h;
        up_rescale(t, t->scene ? scale : 1.0f);
        n += t->scene;
    }
    g_targets_changed = 1;
    g_up.reset = 1;
    rt_log("[gfx9] upscaling: %u scene targets at %.2f of %ux%u\n", n, scale, bb ? bb->w : 0, bb ? bb->h : 0);
}

/* --- the frame ---------------------------------------------------------------------------------- */

static int up_mode(void) { return g_up.mode; }

static float halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= b)
        f /= (float)b, r += f * (float)(i % b);
    return r;
}

/* A draw: the scene's are jittered and their textures' LOD biased, and every draw into a scaled
 * target has its pixel shader's vPos given in the game's pixels. */
static void up_draw_aux(const G9Draw* d, SmAux* aux, float sx, float sy)
{
    aux->rscale[0] = 1.0f / sx, aux->rscale[1] = 1.0f / sy;
    if (!g_up.mode || !d->rs.zenable || !g_ds || !g_ds->scene)
        return;
    if (g_up.mode == 2)
    {
        aux->half_pixel[0] += 2.0f * g_up.jx * aux->half_pixel[0];
        aux->half_pixel[1] += 2.0f * g_up.jy * aux->half_pixel[1];
    }
    float bias = log2f(sx < 1.0f ? sx : 1.0f);
    if (bias < 0.0f)
        for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
            aux->lod_bias[s] += bias;
}

static id<MTLTexture> up_texture(id<MTLTexture>* t, MTLPixelFormat fmt, NSUInteger w, NSUInteger h, MTLTextureUsage usage)
{
    if (*t && (*t).width == w && (*t).height == h && (*t).pixelFormat == fmt && ((*t).usage & usage) == usage)
        return *t;
    [*t release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModePrivate;
    *t = [g_dev newTextureWithDescriptor:d];
    return *t;
}

static id<MTLFXSpatialScaler> up_spatial(id<MTLTexture> in, NSUInteger ow, NSUInteger oh)
{
    id<MTLFXSpatialScaler> s = g_up.spatial;
    if (s && s.inputWidth == in.width && s.inputHeight == in.height && s.outputWidth == ow && s.outputHeight == oh &&
        s.colorTextureFormat == in.pixelFormat)
        return s;
    [g_up.spatial release];
    MTLFXSpatialScalerDescriptor* sd = [[MTLFXSpatialScalerDescriptor new] autorelease];
    sd.colorTextureFormat = sd.outputTextureFormat = in.pixelFormat;
    sd.inputWidth = in.width, sd.inputHeight = in.height;
    sd.outputWidth = ow, sd.outputHeight = oh;
    sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    g_up.spatial = [sd newSpatialScalerWithDevice:g_dev];
    if (!g_up.spatial)
        rt_log("[gfx9] no MetalFX spatial scaler for %lux%lu to %lux%lu\n", (unsigned long)in.width,
            (unsigned long)in.height, (unsigned long)ow, (unsigned long)oh);
    return g_up.spatial;
}

static id<MTLFXTemporalScaler> up_temporal(id<MTLTexture> in, NSUInteger ow, NSUInteger oh)
{
    id<MTLFXTemporalScaler> s = g_up.temporal;
    if (s && s.inputWidth == in.width && s.inputHeight == in.height && s.outputWidth == ow && s.outputHeight == oh &&
        s.colorTextureFormat == in.pixelFormat)
        return s;
    [g_up.temporal release];
    MTLFXTemporalScalerDescriptor* sd = [[MTLFXTemporalScalerDescriptor new] autorelease];
    sd.colorTextureFormat = sd.outputTextureFormat = in.pixelFormat;
    sd.depthTextureFormat = MTLPixelFormatDepth32Float;
    sd.motionTextureFormat = MTLPixelFormatRG16Float;
    sd.inputWidth = in.width, sd.inputHeight = in.height;
    sd.outputWidth = ow, sd.outputHeight = oh;
    sd.requiresSynchronousInitialization = YES;
    g_up.temporal = [sd newTemporalScalerWithDevice:g_dev];
    g_up.reset = 1;
    if (!g_up.temporal)
        rt_log("[gfx9] no MetalFX temporal scaler for %lux%lu to %lux%lu\n", (unsigned long)in.width,
            (unsigned long)in.height, (unsigned long)ow, (unsigned long)oh);
    return g_up.temporal;
}

/* the camera this frame (the scene effects' vote), 0 if the draws did not agree on one */
static int up_camera(float* vp)
{
    XVote* best = &g_xfx.vote[0];
    for (int i = 1; i < 4; ++i)
        if (g_xfx.vote[i].n > best->n)
            best = &g_xfx.vote[i];
    if (best->n < 4)
        return 0;
    memcpy(vp, best->m, 64);
    return 1;
}

/* The motion vectors and the depth MetalFX takes, from the scene's depth: m maps this frame's
 * clip space to the last frame's (row vectors). */
static void up_motion(G9Tex* ds, const float* m, NSUInteger w, NSUInteger h)
{
    id<MTLTexture> mv = up_texture(&g_up.mv, MTLPixelFormatRG16Float, w, h,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | g_up.temporal.motionTextureUsage);
    id<MTLTexture> dz = up_texture(&g_up.depth, MTLPixelFormatDepth32Float, w, h,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | g_up.temporal.depthTextureUsage);
    UpMU u;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            u.m[c * 4 + r] = m[r * 4 + c];
    /* what moved each pixel off the unjittered picture: the jitter and D3D9's half pixel */
    u.jit[0] = (2.0f * g_up.jx + 1.0f) / w, u.jit[1] = -(2.0f * g_up.jy + 1.0f) / h, u.jit[2] = u.jit[3] = 0;
    u.size[0] = (float)w, u.size[1] = (float)h, u.size[2] = 1.0f / w, u.size[3] = 1.0f / h;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = mv;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = dz;
    rp.depthAttachment.loadAction = MTLLoadActionDontCare;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:g_up.motion];
    [e setDepthStencilState:g_up.zalways];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e setFragmentTexture:ds->tex atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

/* row vectors: out = a * b */
static void mul4(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

/* The finished scene t (at the render scale) upscaled to the game's size; the result stands in
 * for t until the frame ends. */
static void up_apply(G9Tex* t)
{
    if (!g_up.mode || !t->scene || g_up.swapped || !up_init())
        return;
    @autoreleasepool
    {
        end_pass();
        id<MTLTexture> in = t->tex;
        NSUInteger ow = t->w, oh = t->h, iw = in.width, ih = in.height;
        int mode = g_up.mode;
        G9Tex* ds = g_xfx.ds && g_xfx.ds->scene && g_xfx.ds->rw == iw && g_xfx.ds->rh == ih ? g_xfx.ds : NULL;
        float vp[16], inv[16], m[16];
        int cam = up_camera(vp) && invert4(vp, inv);
        if (mode == 2 && (!ds || !cam))
            mode = 1, g_up.reset = 1; /* nothing to take the history along with */
        if (mode == 1 && iw == ow && ih == oh)
        {
            g_up.have_prev = 0;
            return;
        }
        id<MTLTexture> out = nil;
        if (mode == 2)
        {
            id<MTLFXTemporalScaler> s = up_temporal(in, ow, oh);
            if (!s)
                return;
            out = up_texture(&g_up.out, in.pixelFormat, ow, oh,
                s.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
            if (g_up.have_prev)
                mul4(inv, g_up.prev_vp, m);
            else
                memcpy(m, (float[16]){ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, 64), g_up.reset = 1;
            /* a cut: the middle of the screen came from more than half a screen away */
            float c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = 0.5f * m[8 + k] + m[12 + k];
            if (c[3] <= 1e-6f || fabsf(c[0] / c[3]) > 1.0f || fabsf(c[1] / c[3]) > 1.0f)
                g_up.reset = 1;
            up_motion(ds, m, iw, ih);
            s.colorTexture = in;
            s.depthTexture = g_up.depth;
            s.motionTexture = g_up.mv;
            s.outputTexture = out;
            s.inputContentWidth = iw, s.inputContentHeight = ih;
            s.jitterOffsetX = g_up.jx * g_xfs.up_jx;
            s.jitterOffsetY = g_up.jy * g_xfs.up_jy;
            s.motionVectorScaleX = (float)iw * g_xfs.up_mv;
            s.motionVectorScaleY = (float)ih * g_xfs.up_mv;
            s.reset = g_up.reset ? YES : NO;
            s.depthReversed = NO;
            [s encodeToCommandBuffer:cmd()];
            memcpy(g_up.prev_vp, vp, 64);
            g_up.have_prev = 1;
            g_up.reset = 0;
            if ((int)g_xfs.up_debug == 1)
            {
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = out;
                rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
                float r[4] = { (float)iw / ow, (float)ih / oh, (float)iw, (float)ih };
                [e setRenderPipelineState:g_up.show];
                [e setFragmentBytes:r length:16 atIndex:0];
                [e setFragmentTexture:g_up.mv atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            }
        }
        else
        {
            id<MTLFXSpatialScaler> s = up_spatial(in, ow, oh);
            if (!s)
                return;
            out = up_texture(&g_up.out, in.pixelFormat, ow, oh,
                s.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
            s.colorTexture = in;
            s.outputTexture = out;
            s.inputContentWidth = iw, s.inputContentHeight = ih;
            [s encodeToCommandBuffer:cmd()];
            g_up.have_prev = 0;
        }
        g_up.swapped = t;
        g_up.swapped_low = in;
        t->tex = nil; /* up_set_tex releases nothing: the scene's texture is kept in swapped_low */
        up_set_tex(t, [out retain]);
        int state = mode | (ds ? 4 : 0) | (cam ? 8 : 0);
        if (state != g_up.logged)
        {
            g_up.logged = state;
            rt_log("[gfx9] upscaling %lux%lu to %lux%lu: %s (depth %s, camera %s)\n", (unsigned long)iw, (unsigned long)ih,
                (unsigned long)ow, (unsigned long)oh, mode == 2 ? "MetalFX temporal" : "MetalFX spatial", ds ? "yes" : "no",
                cam ? "yes" : "no");
        }
    }
}

/* At present, before the scene effects forget the frame: the scene's textures back, the next
 * frame's jitter and targets. */
static void up_frame_end(G9Tex* bb)
{
    up_unswap();
    int mode = (int)g_xfs.upscale;
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (mode && ![MTLFXSpatialScalerDescriptor supportsDevice:g_dev])
        mode = 0;
    if (mode == 2 && ![MTLFXTemporalScalerDescriptor supportsDevice:g_dev])
        mode = 1;
    if (mode != g_up.mode)
        g_up.reset = 1, g_up.have_prev = 0;
    g_up.mode = mode;
    float s = g_xfs.render_scale;
    s = !mode ? 1.0f : s < 0.25f ? 0.25f : s > 1.0f ? 1.0f : s;
    if (mode == 2)
    {
        /* the temporal scaler takes at most so much upscaling */
        float most = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev];
        if (most > 1.0f && s < 1.0f / most)
            s = 1.0f / most;
    }
    up_choose(mode ? bb : NULL, mode ? g_xfx.ui : NULL, s);
    /* enough of the sequence to cover each output pixel about eight times */
    uint32_t n = (uint32_t)ceilf(8.0f / (s * s));
    n = n < 8 ? 8 : n > 64 ? 64 : n;
    g_up.phase = g_up.phase % n + 1;
    float amp = mode == 2 ? g_xfs.up_jitter : 0.0f;
    g_up.jx = (halton(g_up.phase, 2) - 0.5f) * amp;
    g_up.jy = (halton(g_up.phase, 3) - 0.5f) * amp;
}
