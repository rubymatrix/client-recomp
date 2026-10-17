/* Navmesh debug overlay, the drawing (navdraw.h): part of gfx9_metal.m, included after gfx9_fx.h
 * and gfx9_upscale.h (it uses the device, the scene's depth and the scene effects' view-projection
 * vote).
 *
 * g9_overlay runs once a frame, when the game presents, before the back buffer is shot or shown: a
 * pass over the back buffer draws the zone's navmesh as translucent faces, then its edges and
 * off-mesh links as screen-space quads, for each zone navdraw_frame gives (the region's other zones
 * first, faded: less alpha, less colour). No depth buffer is attached: each fragment reads the
 * scene's depth (fx_note_draw's, this frame's) and is dropped when the scene is nearer. Both
 * depths are compared as view distances (the projection's z = A w + B, from its rows), so the
 * bias is in metres at any distance. Nothing runs, and nothing is noted per draw, while the overlay
 * is off. */

#include "navdraw.h"

static const char ND_MSL[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct NV { packed_float3 p; uint c; };\n"
    "struct NU { float4 r[4]; float4 target; float4 depth; float4 zlin; float4 opt; float4 style; };\n"
    /* target: w, h, 1/w, 1/h; depth: tex w, h, has depth; zlin: A, B, linear, bias m;
     * opt: lift (faces), lift (lines), half line width px, relative bias; style: alpha scale, desaturation */
    "struct NO { float4 p [[position]]; float4 c; float w; };\n"
    "static float4 nd_clip(constant NU& u, float3 p) { float4 v = float4(p, 1.0);\n"
    "  return float4(dot(u.r[0], v), dot(u.r[1], v), dot(u.r[2], v), dot(u.r[3], v)); }\n"
    "vertex NO nd_tri(uint vid [[vertex_id]], const device NV* v [[buffer(0)]], constant NU& u [[buffer(1)]]) {\n"
    "  NO o; float3 p = float3(v[vid].p); p.y += u.opt.x; o.p = nd_clip(u, p); o.w = o.p.w;\n"
    "  o.c = unpack_unorm4x8_to_float(v[vid].c); return o; }\n"
    "vertex NO nd_line(uint vid [[vertex_id]], uint iid [[instance_id]], const device NV* v [[buffer(0)]],\n"
    "                  constant NU& u [[buffer(1)]]) {\n"
    "  float3 pa = float3(v[iid * 2].p), pb = float3(v[iid * 2 + 1].p); pa.y += u.opt.y; pb.y += u.opt.y;\n"
    "  float4 a = nd_clip(u, pa), b = nd_clip(u, pb); const float n = 0.05;\n"
    "  NO o; o.c = unpack_unorm4x8_to_float(v[iid * 2 + (vid >> 1)].c);\n"
    "  if (a.w < n && b.w < n) { o.p = float4(0, 0, 0, -1); o.w = -1; return o; }\n"
    "  if (a.w < n) a = mix(a, b, (n - a.w) / (b.w - a.w));\n"
    "  if (b.w < n) b = mix(b, a, (n - b.w) / (a.w - b.w));\n"
    "  float2 sa = a.xy / a.w * u.target.xy, sb = b.xy / b.w * u.target.xy, d = sb - sa;\n"
    "  float l = length(d); d = l > 1e-4 ? d / l : float2(1, 0);\n"
    "  float2 nrm = float2(-d.y, d.x) * u.opt.z * ((vid & 1) ? 1.0 : -1.0);\n"
    "  float4 e = (vid >> 1) ? b : a;\n"
    "  e.xy += nrm * u.target.zw * 2.0 * e.w;\n"
    "  o.p = e; o.w = e.w; return o; }\n"
    "fragment float4 nd_frag(NO i [[stage_in]], depth2d<float> dt [[texture(0)]], constant NU& u [[buffer(0)]]) {\n"
    "  if (u.depth.z > 0.5) {\n"
    "    uint2 q = uint2(min(i.p.xy * u.depth.xy * u.target.zw, u.depth.xy - 1.0));\n"
    "    float sd = dt.read(q);\n"
    "    if (u.zlin.z > 0.5) {\n"
    "      float den = sd - u.zlin.x; float ws = abs(den) > 1e-9 ? u.zlin.y / den : 1e9;\n"
    "      if (ws < 0.0) ws = 1e9;\n"
    "      if (i.w > ws + u.zlin.w + ws * u.opt.w) discard_fragment();\n"
    "    } else if (abs(i.p.z - sd) > u.zlin.w * 1e-3 && (u.zlin.x > 0.0 ? i.p.z < sd : i.p.z > sd)) discard_fragment();\n"
    "  }\n"
    "  float4 c = i.c; float l = dot(c.rgb, float3(0.299, 0.587, 0.114));\n"
    "  c.rgb = mix(c.rgb, float3(l), u.style.y); c.a *= u.style.x; return c; }\n";

static struct
{
    int tried, ok;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> tri, line;
    MTLPixelFormat fmt;
    struct
    {
        uint32_t id;
        int used;
        id<MTLBuffer> tris, lines;
    } buf[NAVDRAW_MAX];
    id<MTLTexture> nodepth;
    int warned_cam, warned_depth;
} g_nd;

/* The scene effects' view-projection vote (fx_note_draw: VP = W^-1 * WVP, row vectors), as rows.
 * The game draws its scene in two depth phases on one depth buffer, each after a depth clear: the
 * distant one (near plane about 50 m) and the near one (about 0.25 m). The depth left at present is
 * the near phase's, so the camera is the vote with the nearest near plane (z = A w + B is 0 at
 * w = -B / A), not simply the commonest. */
static int nd_camera_vote(float m[16])
{
    XVote* best = NULL;
    float best_near = INFINITY;
    for (int i = 0; i < 4; ++i)
    {
        const XVote* v = &g_xfx.vote[i];
        if (v->n < 2)
            continue;
        /* rows: row 2 = column 2 of the row-vector matrix, row 3 = column 3 */
        float r3 = v->m[3] * v->m[3] + v->m[7] * v->m[7] + v->m[11] * v->m[11];
        if (r3 < 1e-12f)
            continue;
        float a = (v->m[2] * v->m[3] + v->m[6] * v->m[7] + v->m[10] * v->m[11]) / r3;
        float b = v->m[14] - a * v->m[15];
        float nearp = fabsf(a) > 1e-9f ? -b / a : INFINITY;
        if (nearp > 0 && nearp < best_near)
            best_near = nearp, best = (XVote*)v;
    }
    if (!best)
        return 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[r * 4 + c] = best->m[c * 4 + r];
    return 1;
}

__attribute__((constructor)) static void nd_register(void)
{
    if (!navdraw_camera_hook)
        navdraw_camera_hook = nd_camera_vote;
}

static int nd_pipes(MTLPixelFormat fmt)
{
    if (g_nd.ok && g_nd.fmt == fmt)
        return 1;
    if (g_nd.tried && !g_nd.lib)
        return 0;
    g_nd.tried = 1;
    NSError* err = nil;
    if (!g_nd.lib)
    {
        g_nd.lib = [g_dev newLibraryWithSource:@(ND_MSL) options:nil error:&err];
        if (!g_nd.lib)
        {
            rt_log("[navdraw] shaders: %s\n", err.localizedDescription.UTF8String);
            return 0;
        }
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead;
        g_nd.nodepth = [g_dev newTextureWithDescriptor:td];
    }
    [g_nd.tri release];
    [g_nd.line release];
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.fragmentFunction = [[g_nd.lib newFunctionWithName:@"nd_frag"] autorelease];
    pd.colorAttachments[0].pixelFormat = fmt;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    pd.vertexFunction = [[g_nd.lib newFunctionWithName:@"nd_tri"] autorelease];
    g_nd.tri = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.vertexFunction = [[g_nd.lib newFunctionWithName:@"nd_line"] autorelease];
    g_nd.line = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    g_nd.ok = g_nd.tri && g_nd.line;
    g_nd.fmt = fmt;
    if (!g_nd.ok)
        rt_log("[navdraw] pipelines: %s\n", err.localizedDescription.UTF8String);
    return g_nd.ok;
}

typedef struct NdU
{
    float r[16];
    float target[4], depth[4], zlin[4], opt[4], style[4];
} NdU;

/* A GPU buffer over a zone's vertex memory: wrapped without a copy (*wrapped = 1: the buffer then
 * owns the memory and frees it once the GPU is done with it), or copied when that fails. */
static id<MTLBuffer> nd_buffer(const NavDrawVert* v, uint32_t n, size_t bytes, int* wrapped)
{
    *wrapped = 0;
    if (!v || !n)
        return nil;
    void* p = (void*)v;
    id<MTLBuffer> b = [g_dev newBufferWithBytesNoCopy:p
                                               length:bytes
                                              options:MTLResourceStorageModeShared
                                          deallocator:^(void* q, NSUInteger len) {
                                              navdraw_vfree(q, len);
                                          }];
    if (b)
    {
        *wrapped = 1;
        return b;
    }
    return [g_dev newBufferWithBytes:v length:n * sizeof(NavDrawVert) options:MTLResourceStorageModeShared];
}

/* the GPU copies of this frame's zones: made for new ones, released for the ones gone */
static void nd_sync(const NavDrawSet* set)
{
    for (int k = 0; k < NAVDRAW_MAX; ++k)
        g_nd.buf[k].used = 0;
    for (uint32_t i = 0; i < set->n; ++i)
    {
        const NavDrawGeom* geo = set->g[i];
        int k = 0;
        while (k < NAVDRAW_MAX && !(g_nd.buf[k].id == geo->id && (g_nd.buf[k].tris || g_nd.buf[k].lines)))
            ++k;
        if (k == NAVDRAW_MAX)
        {
            for (k = 0; k < NAVDRAW_MAX && (g_nd.buf[k].tris || g_nd.buf[k].lines || g_nd.buf[k].used); ++k)
                ;
            if (k == NAVDRAW_MAX)
                continue;
            int wt = 0, wl = 0;
            g_nd.buf[k].id = geo->id;
            g_nd.buf[k].tris = nd_buffer(geo->tris, geo->ntris, geo->tris_bytes, &wt);
            g_nd.buf[k].lines = nd_buffer(geo->lines, geo->nlines, geo->lines_bytes, &wl);
            navdraw_handoff(geo, wt, wl);
        }
        g_nd.buf[k].used = 1;
    }
    for (int k = 0; k < NAVDRAW_MAX; ++k)
        if (!g_nd.buf[k].used && (g_nd.buf[k].tris || g_nd.buf[k].lines))
        {
            [g_nd.buf[k].tris release];
            [g_nd.buf[k].lines release];
            g_nd.buf[k].tris = g_nd.buf[k].lines = nil;
            g_nd.buf[k].id = 0;
        }
}

void g9_overlay(G9Tex* bb)
{
    if (!navdraw_on || !bb || !bb->tex)
        return;
    @autoreleasepool
    {
        const NavDrawSet* set = navdraw_frame();
        if (!set)
            return;
        NdU u;
        memset(&u, 0, sizeof u);
        if (!navdraw_get_camera(u.r))
        {
            if (!g_nd.warned_cam)
                rt_log("[navdraw] no camera yet (the scene's draws gave no view-projection)\n"), g_nd.warned_cam = 1;
            return;
        }
        if (!nd_pipes(bb->tex.pixelFormat))
            return;
        nd_sync(set);
        if (g_pending_clear)
            begin_pass();
        end_pass();

        float tw = (float)bb->tex.width, th = (float)bb->tex.height;
        u.target[0] = tw, u.target[1] = th, u.target[2] = 1.0f / tw, u.target[3] = 1.0f / th;
        G9Tex* ds = g_xfx.ds;
        id<MTLTexture> dt = ds && ds->tex ? ds->tex : nil;
        if (dt && dt.textureType == MTLTextureType2D)
        {
            u.depth[0] = (float)dt.width, u.depth[1] = (float)dt.height, u.depth[2] = 1;
        }
        else
        {
            dt = g_nd.nodepth;
            if (!g_nd.warned_depth)
                rt_log("[navdraw] no scene depth this frame: drawn without the depth test\n"), g_nd.warned_depth = 1;
        }
        /* z_clip = A w_clip + B for a perspective projection: row 2 = A row 3 + (0, 0, 0, B) */
        float r3 = u.r[12] * u.r[12] + u.r[13] * u.r[13] + u.r[14] * u.r[14];
        static float bias = -1, rel;
        if (bias < 0)
        {
            bias = getenv("XIV_NAVDRAW_BIAS") ? strtof(getenv("XIV_NAVDRAW_BIAS"), NULL) : 0.15f;
            rel = getenv("XIV_NAVDRAW_BIAS_REL") ? strtof(getenv("XIV_NAVDRAW_BIAS_REL"), NULL) : 0.004f;
        }
        if (r3 > 1e-12f)
        {
            float a = (u.r[8] * u.r[12] + u.r[9] * u.r[13] + u.r[10] * u.r[14]) / r3;
            u.zlin[0] = a, u.zlin[1] = u.r[11] - a * u.r[15], u.zlin[2] = 1;
        }
        u.zlin[3] = bias;
        static float width = -1;
        if (width < 0)
            width = getenv("XIV_NAVDRAW_WIDTH") ? strtof(getenv("XIV_NAVDRAW_WIDTH"), NULL) : 1.0f;
        u.opt[0] = navdraw_lift(), u.opt[1] = navdraw_lift() + 0.02f;
        u.opt[2] = width * fmaxf(1.0f, th / 1080.0f), u.opt[3] = rel;

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = bb->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e setViewport:(MTLViewport){ 0, 0, tw, th, 0, 1 }];
        [e setCullMode:MTLCullModeNone];
        [e setFragmentTexture:dt atIndex:0];
        static float dim_a = -1, dim_s;
        if (dim_a < 0)
        {
            dim_a = getenv("XIV_NAVDRAW_DIM") ? strtof(getenv("XIV_NAVDRAW_DIM"), NULL) : 0.45f;
            dim_s = 0.7f;
        }
        for (uint32_t i = 0; i < set->n; ++i)
        {
            const NavDrawGeom* geo = set->g[i];
            int k = 0;
            while (k < NAVDRAW_MAX && !(g_nd.buf[k].used && g_nd.buf[k].id == geo->id))
                ++k;
            if (k == NAVDRAW_MAX)
                continue;
            u.style[0] = geo->dim ? dim_a : 1.0f, u.style[1] = geo->dim ? dim_s : 0.0f;
            [e setFragmentBytes:&u length:sizeof u atIndex:0];
            [e setVertexBytes:&u length:sizeof u atIndex:1];
            if (g_nd.buf[k].tris && geo->tris_shown)
            {
                [e setRenderPipelineState:g_nd.tri];
                [e setVertexBuffer:g_nd.buf[k].tris offset:0 atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:geo->tris_shown];
            }
            if (g_nd.buf[k].lines && geo->lines_shown >= 2)
            {
                [e setRenderPipelineState:g_nd.line];
                [e setVertexBuffer:g_nd.buf[k].lines offset:0 atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:geo->lines_shown / 2];
            }
        }
        [e endEncoding];
        g_targets_changed = 1;
    }
}

/* navdraw.c (the files, the zone, the keys) is built as part of this file until tools/build_xiv.py
 * lists it among the host sources. */
#include "navdraw.c"
