/* Scene effects for the Direct3D 9 back end: part of gfx9_metal.m (included there; it uses that
 * file's device, command buffer and texture internals).
 *
 * The game draws its 3D scene and its own glare into render targets, then copies the finished
 * scene into the back buffer and draws the interface over it. The effects run once a frame, on
 * that finished scene, just before the copy (fx_hook): the interface stays crisp.
 *
 *   - FXAA (after Lottes' 3.11, quality preset 12 in brief);
 *   - bloom: what is over a threshold at a quarter size, blurred at two sizes, added (screen);
 *   - god rays: the sky's bright pixels around the sun, gathered along lines toward the sun's place
 *     on screen. The sun is the game's light: every scene pixel shader has its direction in c187
 *     and colour in c189. The view-projection comes from the vertex constants: most scene shaders
 *     have the world matrix in c0-c3 and world-view-projection in c8-c11 (rows, row vectors), so
 *     VP = W^-1 * WVP; the frame's draws vote, and the commonest wins;
 *   - a grade: saturation, a contrast curve;
 *   - sharpening (contrast-limited unsharp mask: never past the neighbourhood's range, no halos).
 * And for the scene's draws, anisotropic filtering of mipmapped textures (fx_sampler).
 *
 * Settings: XIV_FX_<KEY> in the environment (XIV_FX=0 turns everything off), then while the game
 * runs XIV_FX_FILE (default ~/Library/Caches/FFXIV/fx.txt, written with the defaults on the first
 * run), lines of key = value, read again when it changes. debug: 1 bloom, 2 rays' source, 3 rays,
 * 4 the effects off but the hook's target outlined red (finding the scene). */

#include <sys/stat.h>

static struct
{
    float fx, aa, sharpen, bloom, threshold, rays, rays_decay, rays_length, grade, sat, contrast, aniso, debug;
} g_xfs;

static const struct
{
    const char* key;
    size_t at;
    float def;
} XFX_SETTINGS[] = {
    { "fx", offsetof(__typeof__(g_xfs), fx), 1.0f },
    { "aa", offsetof(__typeof__(g_xfs), aa), 1.0f },
    { "sharpen", offsetof(__typeof__(g_xfs), sharpen), 0.35f },
    { "bloom", offsetof(__typeof__(g_xfs), bloom), 0.15f },
    { "threshold", offsetof(__typeof__(g_xfs), threshold), 0.7f },
    { "rays", offsetof(__typeof__(g_xfs), rays), 0.5f },
    { "rays_decay", offsetof(__typeof__(g_xfs), rays_decay), 0.96f },
    { "rays_length", offsetof(__typeof__(g_xfs), rays_length), 0.85f },
    { "grade", offsetof(__typeof__(g_xfs), grade), 1.0f },
    { "sat", offsetof(__typeof__(g_xfs), sat), 1.08f },
    { "contrast", offsetof(__typeof__(g_xfs), contrast), 0.12f },
    { "aniso", offsetof(__typeof__(g_xfs), aniso), 16.0f },
    { "debug", offsetof(__typeof__(g_xfs), debug), 0.0f },
};

static const char XFX_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct XU {\n"
    "  float4 size;   // target width, height, 1/width, 1/height\n"
    "  float4 bloom;  // threshold, strength, knee\n"
    "  float4 rays;   // strength, decay, length\n"
    "  float4 sun;    // its place on screen (0..1), how much of it shows, width / height\n"
    "  float4 suncol; // its colour\n"
    "  float4 grade;  // strength, saturation, contrast, debug\n"
    "  float4 sharp;  // amount\n"
    "};\n"
    "struct FO { float4 pos [[position]]; float2 uv; };\n"
    "vertex FO fx_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  FO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "constant float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
    "fragment float4 fx_copy(FO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "  return t.read(uint2(in.pos.xy));\n"
    "}\n"
    "constant float FXAA_Q[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
    "fragment float4 fx_fxaa(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "  float2 rcp = u.size.zw, uv = in.pos.xy * rcp;\n"
    "  float4 c4 = t.sample(s, uv);\n"
    "  float3 c = c4.rgb;\n"
    "  float lm = dot(c, LUMA);\n"
    "  float ln = dot(t.sample(s, uv + float2(0, -rcp.y)).rgb, LUMA), ls = dot(t.sample(s, uv + float2(0, rcp.y)).rgb, LUMA);\n"
    "  float lw = dot(t.sample(s, uv + float2(-rcp.x, 0)).rgb, LUMA), le = dot(t.sample(s, uv + float2(rcp.x, 0)).rgb, LUMA);\n"
    "  float mx = max(lm, max(max(ln, ls), max(lw, le))), mn = min(lm, min(min(ln, ls), min(lw, le))), range = mx - mn;\n"
    "  if (range < max(0.0312, mx * 0.125)) return c4;\n"
    "  float lnw = dot(t.sample(s, uv + float2(-rcp.x, -rcp.y)).rgb, LUMA), lne = dot(t.sample(s, uv + float2(rcp.x, -rcp.y)).rgb, LUMA);\n"
    "  float lsw = dot(t.sample(s, uv + float2(-rcp.x, rcp.y)).rgb, LUMA), lse = dot(t.sample(s, uv + float2(rcp.x, rcp.y)).rgb, LUMA);\n"
    "  float eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
    "  float ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
    "  bool horz = eh >= ev;\n"
    "  float l1 = horz ? ln : lw, l2 = horz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "  float stp = horz ? rcp.y : rcp.x, lavg, grad;\n"
    "  if (g1 >= g2) { stp = -stp; lavg = 0.5 * (l1 + lm); grad = g1; } else { lavg = 0.5 * (l2 + lm); grad = g2; }\n"
    "  float2 e = uv, along = horz ? float2(rcp.x, 0) : float2(0, rcp.y);\n"
    "  if (horz) e.y += stp * 0.5; else e.x += stp * 0.5;\n"
    "  float2 p1 = e - along, p2 = e + along;\n"
    "  float d1 = dot(t.sample(s, p1).rgb, LUMA) - lavg, d2 = dot(t.sample(s, p2).rgb, LUMA) - lavg;\n"
    "  bool r1 = abs(d1) >= grad * 0.25, r2 = abs(d2) >= grad * 0.25;\n"
    "  for (int i = 0; i < 10 && !(r1 && r2); ++i) {\n"
    "    if (!r1) { p1 -= along * FXAA_Q[i]; d1 = dot(t.sample(s, p1).rgb, LUMA) - lavg; r1 = abs(d1) >= grad * 0.25; }\n"
    "    if (!r2) { p2 += along * FXAA_Q[i]; d2 = dot(t.sample(s, p2).rgb, LUMA) - lavg; r2 = abs(d2) >= grad * 0.25; }\n"
    "  }\n"
    "  float dist1 = horz ? uv.x - p1.x : uv.y - p1.y, dist2 = horz ? p2.x - uv.x : p2.y - uv.y;\n"
    "  bool near1 = dist1 < dist2;\n"
    "  float dmin = min(dist1, dist2), len = dist1 + dist2;\n"
    "  bool mid_lower = lm < lavg, good = ((near1 ? d1 : d2) < 0.0) != mid_lower;\n"
    "  float off = good ? -dmin / len + 0.5 : 0.0;\n"
    "  float avg = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;\n"
    "  float sub = saturate(abs(avg - lm) / range);\n"
    "  sub = (-2.0 * sub + 3.0) * sub * sub;\n"
    "  off = max(off, sub * sub * 0.75);\n"
    "  float2 f = uv;\n"
    "  if (horz) f.y += off * stp; else f.x += off * stp;\n"
    "  return float4(t.sample(s, f).rgb, c4.a);\n"
    "}\n"
    /* bloom's source: the scene at a quarter size (four bilinear taps), what is over the threshold,
     * with a soft knee */
    "fragment float4 fx_bright(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                          sampler s [[sampler(0)]]) {\n"
    "  float2 uv = in.uv, t = u.size.zw;\n"
    "  float3 c = 0.25 * (src.sample(s, uv + t * float2(-1, -1)).rgb + src.sample(s, uv + t * float2(1, -1)).rgb +\n"
    "                     src.sample(s, uv + t * float2(-1, 1)).rgb + src.sample(s, uv + t * float2(1, 1)).rgb);\n"
    "  float l = max(c.r, max(c.g, c.b)), k = u.bloom.z;\n"
    "  float soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k + 1e-5);\n"
    "  return float4(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
    "}\n"
    "fragment float4 fx_down(FO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  float2 tx = 1.0 / float2(t.get_width(), t.get_height());\n"
    "  return 0.25 * (t.sample(s, in.uv + tx * float2(-1, -1)) + t.sample(s, in.uv + tx * float2(1, -1)) +\n"
    "                 t.sample(s, in.uv + tx * float2(-1, 1)) + t.sample(s, in.uv + tx * float2(1, 1)));\n"
    "}\n"
    /* a 9-tap gaussian in five bilinear taps, dir texels apart */
    "fragment float4 fx_gauss(FO in [[stage_in]], constant int2& dir [[buffer(1)]], texture2d<float> t [[texture(0)]],\n"
    "                         sampler s [[sampler(0)]]) {\n"
    "  float2 tx = float2(dir) / float2(t.get_width(), t.get_height());\n"
    "  float4 c = t.sample(s, in.uv) * 0.2270270;\n"
    "  c += (t.sample(s, in.uv + tx * 1.3846154) + t.sample(s, in.uv - tx * 1.3846154)) * 0.3162162;\n"
    "  c += (t.sample(s, in.uv + tx * 3.2307692) + t.sample(s, in.uv - tx * 3.2307692)) * 0.0702703;\n"
    "  return c;\n"
    "}\n"
    /* what the god rays start from: the sky's bright pixels (cleared depth), more of them nearer the
     * sun */
    "fragment float4 fx_raymask(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                           depth2d<float> dt [[texture(1)]], sampler s [[sampler(0)]]) {\n"
    "  uint2 dp = min(uint2(in.uv * float2(dt.get_width(), dt.get_height())), uint2(dt.get_width() - 1, dt.get_height() - 1));\n"
    "  if (dt.read(dp) < 0.9999995) return float4(0.0);\n"
    "  float3 c = src.sample(s, in.uv).rgb;\n"
    "  float2 d = (in.uv - u.sun.xy) * float2(u.sun.w, 1.0);\n"
    "  float glow = saturate(1.0 - length(d) / 0.6);\n"
    "  return float4(c * smoothstep(0.35, 0.9, dot(c, LUMA)) * glow * glow, 1.0);\n"
    "}\n"
    /* the mask gathered along the line toward the sun, fading with each step */
    "fragment float4 fx_rays(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> m [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "  const int NS = 64;\n"
    "  float2 uv = in.uv, step = (in.uv - u.sun.xy) * (u.rays.z / float(NS));\n"
    "  float3 acc = float3(0.0);\n"
    "  float w = 1.0;\n"
    "  for (int i = 0; i < NS; ++i) { acc += m.sample(s, uv).rgb * w; w *= u.rays.y; uv -= step; }\n"
    "  return float4(acc * (4.0 / float(NS)), 1.0);\n"
    "}\n"
    "static float3 screen(float3 a, float3 b) { return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); }\n"
    "fragment float4 fx_comp(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> src [[texture(0)]],\n"
    "                        texture2d<float> b1 [[texture(1)]], texture2d<float> b2 [[texture(2)]],\n"
    "                        texture2d<float> ry [[texture(3)]], texture2d<float> rm [[texture(4)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(u.size.xy) - 1;\n"
    "  float4 c = src.read(uint2(p));\n"
    "  int dbg = int(u.grade.w);\n"
    "  if (u.sharp.x > 0.0) {\n"
    "    float3 n = src.read(uint2(clamp(p + int2(0, -1), int2(0), hi))).rgb, so = src.read(uint2(clamp(p + int2(0, 1), int2(0), hi))).rgb;\n"
    "    float3 w = src.read(uint2(clamp(p + int2(-1, 0), int2(0), hi))).rgb, e = src.read(uint2(clamp(p + int2(1, 0), int2(0), hi))).rgb;\n"
    "    float3 mn = min(c.rgb, min(min(n, so), min(w, e))), mx = max(c.rgb, max(max(n, so), max(w, e)));\n"
    /* less where the neighbourhood already has contrast (after AMD's CAS) */
    "    float3 amt = u.sharp.x * sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
    "    c.rgb = clamp(c.rgb + (c.rgb - 0.25 * (n + so + w + e)) * amt * 2.0, mn, mx);\n"
    "  }\n"
    "  float3 add = float3(0.0);\n"
    "  if (u.bloom.y > 0.0) {\n"
    "    float3 bl = b1.sample(s, in.uv).rgb * 0.6 + b2.sample(s, in.uv).rgb * 0.8;\n"
    "    if (dbg == 1) return float4(bl, 1.0);\n"
    "    add += bl * u.bloom.y;\n"
    "  }\n"
    "  if (u.rays.x > 0.0 && u.sun.z > 0.0) {\n"
    "    if (dbg == 2) return float4(rm.sample(s, in.uv).rgb, 1.0);\n"
    "    float3 r = ry.sample(s, in.uv).rgb * u.suncol.rgb * u.sun.z;\n"
    "    if (dbg == 3) return float4(r, 1.0);\n"
    "    add += r * u.rays.x;\n"
    "  }\n"
    "  c.rgb = screen(c.rgb, add);\n"
    "  float3 x = mix(float3(dot(c.rgb, LUMA)), c.rgb, u.grade.y);\n"
    "  x = saturate(x);\n"
    "  x = mix(x, x * x * (3.0 - 2.0 * x), u.grade.z);\n"
    "  c.rgb = mix(c.rgb, x, u.grade.x);\n"
    "  return c;\n"
    "}\n"
    "fragment float4 fx_outline(FO in [[stage_in]], constant XU& u [[buffer(0)]], texture2d<float> src [[texture(0)]]) {\n"
    "  int2 p = int2(in.pos.xy), hi = int2(u.size.xy) - 1;\n"
    "  if (p.x < 4 || p.y < 4 || p.x > hi.x - 4 || p.y > hi.y - 4) return float4(1, 0, 0, 1);\n"
    "  return src.read(uint2(p));\n"
    "}\n";

typedef struct XU
{
    float size[4], bloom[4], rays[4], sun[4], suncol[4], grade[4], sharp[4];
} XU;

/* a view-projection candidate and the draws that agreed on it this frame */
typedef struct XVote
{
    float m[16];
    uint32_t n;
} XVote;

static struct
{
    int tried, ok;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> copy, fxaa, bright, down, gauss, raymask, rays, comp, outline;
    MTLPixelFormat scene_fmt;
    id<MTLTexture> a, q1, q2, e1, e2, rm, rr, dummy;
    id<MTLSamplerState> samp;
    G9Tex* bb;     /* the back buffer the last frame presented */
    G9Tex* ui;     /* what the game copies into it (StretchRect): the scene, then the interface over it */
    G9Tex* ds;     /* the scene's depth this frame */
    int done;      /* the effects ran this frame */
    int logged;
    XVote vote[4];
    float sun[3], suncol[3];
    int have_sun;
    float sunuv[3]; /* eased: x, y, how much shows */
    int eased;
    char file[1024];
    struct timespec mtime;
    double last_look;
} g_xfx;

static float* xfx_setting(const char* key)
{
    for (size_t i = 0; i < sizeof XFX_SETTINGS / sizeof XFX_SETTINGS[0]; ++i)
        if (!strcmp(XFX_SETTINGS[i].key, key))
            return (float*)((char*)&g_xfs + XFX_SETTINGS[i].at);
    return NULL;
}

static void xfx_write_defaults(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# FFXIV recomp scene effects: edit while the game runs (read again within half a second).\n"
               "# fx = 0 turns them all off. aa: 0 none, 1 FXAA. debug: 1 bloom, 2 rays' source, 3 rays, 4 outline the scene.\n");
    for (size_t i = 0; i < sizeof XFX_SETTINGS / sizeof XFX_SETTINGS[0]; ++i)
        fprintf(f, "%s = %g\n", XFX_SETTINGS[i].key, *(float*)((char*)&g_xfs + XFX_SETTINGS[i].at));
    fclose(f);
}

static void xfx_config(void)
{
    for (size_t i = 0; i < sizeof XFX_SETTINGS / sizeof XFX_SETTINGS[0]; ++i)
    {
        char name[64], *c;
        snprintf(name, sizeof name, i ? "XIV_FX_%s" : "XIV_FX", XFX_SETTINGS[i].key);
        for (c = name; *c; ++c)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32;
        const char* v = getenv(name);
        *(float*)((char*)&g_xfs + XFX_SETTINGS[i].at) = v && *v ? (float)atof(v) : XFX_SETTINGS[i].def;
    }
    const char* file = getenv("XIV_FX_FILE");
    if (file && *file)
        snprintf(g_xfx.file, sizeof g_xfx.file, "%s", file);
    else if (getenv("HOME"))
    {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/Library/Caches/FFXIV", getenv("HOME"));
        mkdir(dir, 0755);
        snprintf(g_xfx.file, sizeof g_xfx.file, "%s/fx.txt", dir);
    }
    struct stat st;
    if (g_xfx.file[0] && stat(g_xfx.file, &st) != 0)
        xfx_write_defaults(g_xfx.file);
}

/* the settings file, when it changed since the last look (from present, twice a second) */
static void xfx_reload(void)
{
    double now = CACurrentMediaTime();
    if (!g_xfx.file[0] || now - g_xfx.last_look < 0.5)
        return;
    g_xfx.last_look = now;
    struct stat st;
    if (stat(g_xfx.file, &st) ||
        (st.st_mtimespec.tv_sec == g_xfx.mtime.tv_sec && st.st_mtimespec.tv_nsec == g_xfx.mtime.tv_nsec))
        return;
    g_xfx.mtime = st.st_mtimespec;
    FILE* f = fopen(g_xfx.file, "r");
    if (!f)
        return;
    char line[256], key[64];
    float v, *p;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_] = %f", key, &v) == 2 && (p = xfx_setting(key)))
            *p = v;
    fclose(f);
    rt_log("[gfx9] scene effects %s (%s)\n", g_xfs.fx != 0.0f ? "on" : "off", g_xfx.file);
}

static id<MTLRenderPipelineState> xfx_pipe(NSString* frag, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.vertexFunction = [[g_xfx.lib newFunctionWithName:@"fx_vs"] autorelease];
    pd.fragmentFunction = [[g_xfx.lib newFunctionWithName:frag] autorelease];
    pd.colorAttachments[0].pixelFormat = fmt;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p)
        rt_log("[gfx9] scene effect %s: %s\n", frag.UTF8String, err ? err.localizedDescription.UTF8String : "?");
    return p;
}

static int xfx_init(MTLPixelFormat scene_fmt)
{
    if (g_xfx.tried)
        return g_xfx.ok && scene_fmt == g_xfx.scene_fmt;
    g_xfx.tried = 1;
    @autoreleasepool
    {
        NSError* err = nil;
        g_xfx.lib = [g_dev newLibraryWithSource:@(XFX_MSL) options:nil error:&err];
        if (!g_xfx.lib)
        {
            rt_log("[gfx9] scene effects: %s\n", err.localizedDescription.UTF8String);
            return 0;
        }
        const MTLPixelFormat hf = MTLPixelFormatRGBA16Float;
        g_xfx.scene_fmt = scene_fmt;
        g_xfx.copy = xfx_pipe(@"fx_copy", scene_fmt);
        g_xfx.fxaa = xfx_pipe(@"fx_fxaa", scene_fmt);
        g_xfx.comp = xfx_pipe(@"fx_comp", scene_fmt);
        g_xfx.outline = xfx_pipe(@"fx_outline", scene_fmt);
        g_xfx.bright = xfx_pipe(@"fx_bright", hf);
        g_xfx.down = xfx_pipe(@"fx_down", hf);
        g_xfx.gauss = xfx_pipe(@"fx_gauss", hf);
        g_xfx.raymask = xfx_pipe(@"fx_raymask", hf);
        g_xfx.rays = xfx_pipe(@"fx_rays", hf);
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor new] autorelease];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_xfx.samp = [g_dev newSamplerStateWithDescriptor:sd];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:hf width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_xfx.dummy = [g_dev newTextureWithDescriptor:td];
        g_xfx.ok = g_xfx.copy && g_xfx.fxaa && g_xfx.comp && g_xfx.outline && g_xfx.bright && g_xfx.down && g_xfx.gauss &&
                   g_xfx.raymask && g_xfx.rays;
        if (g_xfx.ok)
            rt_log("[gfx9] scene effects ready\n");
    }
    return g_xfx.ok;
}

/* a private render target of this size and format in *t, made again when either changes */
static id<MTLTexture> xfx_tex(id<MTLTexture>* t, MTLPixelFormat fmt, NSUInteger w, NSUInteger h)
{
    w = w ? w : 1, h = h ? h : 1;
    if (*t && (*t).width == w && (*t).height == h && (*t).pixelFormat == fmt)
        return *t;
    [*t release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    *t = [g_dev newTextureWithDescriptor:d];
    return *t;
}

/* one full-screen pass into dst */
static void xfx_pass(id<MTLTexture> dst, id<MTLRenderPipelineState> p, id<MTLTexture> const* tex, int n, const XU* u,
    const int* dir)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    if (u)
        [e setFragmentBytes:u length:sizeof *u atIndex:0];
    if (dir)
        [e setFragmentBytes:dir length:8 atIndex:1];
    for (int i = 0; i < n; ++i)
        [e setFragmentTexture:tex[i] ? tex[i] : g_xfx.dummy atIndex:i];
    [e setFragmentSamplerState:g_xfx.samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static int invert4(const float* m, float* out)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabsf(det) > 1e-12f))
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return 1;
}

/* A scene draw: its depth buffer, its vote for the view-projection, the sun it was lit by. */
static void fx_note_draw(const G9Draw* d)
{
    G9Tex* final = g_xfx.ui ? g_xfx.ui : g_xfx.bb;
    if (g_xfs.fx == 0.0f || !final || !g_rt[0] || !g_ds || !d->rs.zenable || g_rt[0]->w != final->w ||
        g_rt[0]->h != final->h)
        return;
    g_xfx.ds = g_ds;
    const float* l = d->psc + 187 * 4;
    float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (len > 0.9f && len < 1.1f)
    {
        for (int i = 0; i < 3; ++i)
            g_xfx.sun[i] = -l[i] / len, g_xfx.suncol[i] = d->psc[189 * 4 + i];
        g_xfx.have_sun = 1;
    }
    float winv[16], vp[16];
    if (!invert4(d->vsc, winv))
        return;
    const float* wvp = d->vsc + 8 * 4;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            vp[r * 4 + c] = winv[r * 4 + 0] * wvp[c] + winv[r * 4 + 1] * wvp[4 + c] + winv[r * 4 + 2] * wvp[8 + c] +
                            winv[r * 4 + 3] * wvp[12 + c];
    XVote* least = &g_xfx.vote[0];
    for (int i = 0; i < 4; ++i)
    {
        XVote* v = &g_xfx.vote[i];
        if (v->n)
        {
            int same = 1;
            for (int k = 0; k < 16 && same; ++k)
                same = fabsf(v->m[k] - vp[k]) <= 1e-3f * (1.0f + fabsf(vp[k]));
            if (same)
            {
                v->n++;
                return;
            }
        }
        if (v->n < least->n)
            least = v;
    }
    memcpy(least->m, vp, sizeof vp);
    least->n = 1;
}

/* where the sun is on screen (0..1), and how much of it to show */
static void fx_sun(float* uv)
{
    uv[0] = uv[1] = 0.5f, uv[2] = 0.0f;
    XVote* best = &g_xfx.vote[0];
    for (int i = 1; i < 4; ++i)
        if (g_xfx.vote[i].n > best->n)
            best = &g_xfx.vote[i];
    if (!g_xfx.have_sun || best->n < 4)
        return;
    float clip[4];
    for (int c = 0; c < 4; ++c)
        clip[c] = g_xfx.sun[0] * best->m[c] + g_xfx.sun[1] * best->m[4 + c] + g_xfx.sun[2] * best->m[8 + c];
    if (clip[3] <= 1e-5f)
        return; /* behind the camera */
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    float far = fmaxf(fabsf(x), fabsf(y));
    uv[0] = x * 0.5f + 0.5f, uv[1] = 0.5f - y * 0.5f;
    uv[2] = fminf(fmaxf(2.0f - far, 0.0f), 1.0f); /* fading out from the edge to half a screen past it */
}

/* the effects on the finished scene t, in place */
static void fx_apply(G9Tex* t)
{
    @autoreleasepool
    {
        id<MTLTexture> s = t->tex;
        if (!xfx_init(s.pixelFormat))
            return;
        end_pass();
        NSUInteger w = s.width, h = s.height, w4 = (w + 3) / 4, h4 = (h + 3) / 4, w8 = (w4 + 1) / 2, h8 = (h4 + 1) / 2;
        const MTLPixelFormat hf = MTLPixelFormatRGBA16Float;
        XU u;
        memset(&u, 0, sizeof u);
        u.size[0] = (float)w, u.size[1] = (float)h, u.size[2] = 1.0f / w, u.size[3] = 1.0f / h;
        if ((int)g_xfs.debug == 4)
        {
            id<MTLTexture> a = xfx_tex(&g_xfx.a, s.pixelFormat, w, h);
            xfx_pass(a, g_xfx.copy, &s, 1, &u, NULL);
            xfx_pass(s, g_xfx.outline, &a, 1, &u, NULL);
            return;
        }
        u.bloom[0] = g_xfs.threshold, u.bloom[1] = g_xfs.bloom, u.bloom[2] = 0.2f;
        u.rays[0] = g_xfs.rays, u.rays[1] = g_xfs.rays_decay, u.rays[2] = g_xfs.rays_length;
        u.grade[0] = g_xfs.grade, u.grade[1] = g_xfs.sat, u.grade[2] = g_xfs.contrast, u.grade[3] = g_xfs.debug;
        u.sharp[0] = g_xfs.sharpen;
        float sun[3];
        fx_sun(sun);
        /* eased, so the rays do not jump with a vote that changes */
        for (int i = 0; i < 3; ++i)
            g_xfx.sunuv[i] = g_xfx.eased ? g_xfx.sunuv[i] + (sun[i] - g_xfx.sunuv[i]) * 0.3f : sun[i];
        g_xfx.eased = 1;
        u.sun[0] = g_xfx.sunuv[0], u.sun[1] = g_xfx.sunuv[1], u.sun[2] = g_xfx.sunuv[2], u.sun[3] = (float)w / h;
        for (int i = 0; i < 3; ++i)
            u.suncol[i] = fminf(g_xfx.suncol[i], 1.5f);

        /* the scene as it is (anti-aliased), for the passes to read */
        id<MTLTexture> a = xfx_tex(&g_xfx.a, s.pixelFormat, w, h);
        xfx_pass(a, g_xfs.aa >= 1.0f ? g_xfx.fxaa : g_xfx.copy, &s, 1, &u, NULL);

        id<MTLTexture> b1 = nil, b2 = nil, ry = nil, rm = nil;
        if (g_xfs.bloom > 0.0f)
        {
            static const int hz[2] = { 1, 0 }, vt[2] = { 0, 1 };
            id<MTLTexture> q1 = xfx_tex(&g_xfx.q1, hf, w4, h4), q2 = xfx_tex(&g_xfx.q2, hf, w4, h4);
            id<MTLTexture> e1 = xfx_tex(&g_xfx.e1, hf, w8, h8), e2 = xfx_tex(&g_xfx.e2, hf, w8, h8);
            xfx_pass(q1, g_xfx.bright, &a, 1, &u, NULL);
            xfx_pass(q2, g_xfx.gauss, &q1, 1, NULL, hz);
            xfx_pass(q1, g_xfx.gauss, &q2, 1, NULL, vt);
            xfx_pass(e1, g_xfx.down, &q1, 1, NULL, NULL);
            xfx_pass(e2, g_xfx.gauss, &e1, 1, NULL, hz);
            xfx_pass(e1, g_xfx.gauss, &e2, 1, NULL, vt);
            b1 = q1, b2 = e1;
        }
        if (g_xfs.rays > 0.0f && u.sun[2] > 0.0f && g_xfx.ds)
        {
            rm = xfx_tex(&g_xfx.rm, hf, w4, h4);
            ry = xfx_tex(&g_xfx.rr, hf, w4, h4);
            id<MTLTexture> in[2] = { a, g_xfx.ds->tex };
            xfx_pass(rm, g_xfx.raymask, in, 2, &u, NULL);
            xfx_pass(ry, g_xfx.rays, &rm, 1, &u, NULL);
        }
        else
            u.sun[2] = 0.0f;
        id<MTLTexture> in[5] = { a, b1, b2, ry, rm };
        xfx_pass(s, g_xfx.comp, in, 5, &u, NULL);
        int state = 1 | (g_xfx.ds ? 2 : 0) | (g_xfx.have_sun ? 4 : 0) | (sun[2] > 0.0f ? 8 : 0);
        if (state != g_xfx.logged)
        {
            g_xfx.logged = state;
            rt_log("[gfx9] scene effects on a %ux%u scene (depth %s, sun %s, %s at %.2f,%.2f)\n", (unsigned)w, (unsigned)h,
                g_xfx.ds ? "yes" : "no", g_xfx.have_sun ? "yes" : "no", sun[2] > 0.0f ? "on screen" : "not on screen",
                sun[0], sun[1]);
        }
    }
}

/* Before a draw: the first into the back buffer this frame copies the finished scene there, the
 * interface after it. The scene is the full-size render target it samples. */
static void fx_hook(const G9Draw* d)
{
    G9Tex* final = g_xfx.ui ? g_xfx.ui : g_xfx.bb;
    if (g_xfs.fx == 0.0f || g_xfx.done || !final || g_rt[0] != final)
        return;
    g_xfx.done = 1;
    const SmInfo* pi = &d->ps->info;
    for (uint32_t s = 0; s < G9_PS_SAMPLERS; ++s)
    {
        G9Tex* t = d->tex[s];
        if (pi->sampler[s] && t && t != final && t->use == G9_USE_RT && t->type == G9_TEX_2D &&
            t->w == final->w && t->h == final->h && !t->f.depth)
        {
            fx_apply(t);
            return;
        }
    }
}

/* the sampler for a scene draw's texture: anisotropic when it has mipmaps */
static const G9Sampler* fx_sampler(const G9Draw* d, uint32_t s, G9Sampler* tmp)
{
    const G9Sampler* sm = &d->samp[s];
    const G9Tex* t = d->tex[s];
    if (g_xfs.fx == 0.0f || g_xfs.aniso <= 1.0f || !d->rs.zenable || !t || t->levels < 2 || t->f.depth ||
        sm->min < 2 || sm->mip == 0)
        return sm;
    *tmp = *sm;
    tmp->min = 3, tmp->mag = tmp->mag >= 2 ? tmp->mag : 2, tmp->mip = 2;
    tmp->max_aniso = (uint8_t)(g_xfs.aniso > 16.0f ? 16.0f : g_xfs.aniso);
    return tmp;
}

/* A copy into the back buffer: its source is where the interface is drawn */
static void fx_note_stretch(G9Tex* src, G9Tex* dst)
{
    if (dst && dst == g_xfx.bb && src && src->type == G9_TEX_2D)
        g_xfx.ui = src;
}

/* At present: the frame is done; what the next one looks for */
static void fx_frame_end(G9Tex* bb)
{
    static int configured;
    if (!configured)
        configured = 1, xfx_config();
    xfx_reload();
    g_xfx.bb = bb;
    g_xfx.done = 0;
    g_xfx.ds = NULL;
    memset(g_xfx.vote, 0, sizeof g_xfx.vote);
}
