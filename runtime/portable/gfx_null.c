/* The graphics back end where there is none (headless builds, Linux): everything is accepted and
 * nothing is drawn. It keeps the frame profile (FFXI_PROFILE=1) of the real back ends, with the GPU
 * at 0, so a headless run measures the game's CPU time alone. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "plat.h"
#include "runtime.h"

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, front_ns, present_ns, since_ns, shim_ns;
} Prof;

static Prof g_prof;
static uint32_t g_present_thread;

uint64_t gfx_now_ns(void) { return rt_monotonic_ns(); }
void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { (void)reason; }

void gfx_prof_shim(uint64_t ns)
{
    if (plat_thread_id() == g_present_thread)
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went (gfx_d3d12.c's format) */
static void prof_frame(uint64_t present_start)
{
    uint64_t now = gfx_now_ns();
    g_prof.present_ns += now - present_start;
    g_prof.frames++;
    if (!g_prof.since_ns)
        g_prof.since_ns = now;
    if (now - g_prof.since_ns < 2000000000ull)
        return;
    double f = (double)g_prof.frames, ms = 1e-6 / f;
    double frame = (double)(now - g_prof.since_ns) * ms;
    double front = (double)g_prof.front_ns * ms, pres = (double)g_prof.present_ns * ms, shims = (double)g_prof.shim_ns * ms;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode 0.00], probe wait 0.00, "
            "present %.2f [swap chain 0.00], other %.2f) | GPU 0.00 ms | %.0f draws, %.0f KB up, 0 new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, pres, shims - front - pres,
            (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0);
    else
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode 0.00) + present %.2f (gpu wait 0.00, swap chain 0.00) | "
            "GPU 0.00 ms | %.0f draws, %.0f KB up, 0 new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, pres, (double)g_prof.draws / f,
            (double)g_prof.bytes / f / 1024.0);
    fflush(stderr);
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

/* --- the device, which draws nothing ------------------------------------------------------------------ */
int gfx_init(void* sdl_window, int vsync)
{
    (void)sdl_window, (void)vsync;
    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    return 0;
}
void gfx_resize(uint32_t w, uint32_t h) { (void)w, (void)h; }
GfxBuf* gfx_buf_create(uint32_t size)
{
    (void)size;
    return NULL;
}
void gfx_buf_destroy(GfxBuf* b) { (void)b; }
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    (void)b, (void)data;
    g_prof.bytes += size;
}
GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    (void)type, (void)d3dfmt, (void)w, (void)h, (void)levels, (void)use;
    return NULL;
}
void gfx_tex_destroy(GfxTex* t) { (void)t; }
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)src, (void)pitch;
}
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)x, (void)y, (void)w, (void)src;
    g_prof.bytes += (uint64_t)h * pitch;
}
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    (void)src, (void)sface, (void)slevel, (void)sx, (void)sy, (void)w, (void)h, (void)dst, (void)dface, (void)dlevel, (void)dx, (void)dy;
}
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth) { (void)color, (void)face, (void)level, (void)depth; }
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil, (void)vp;
}
/* counted: frame_budget.py tells frames in a zone (>= 500 draws) from menus by it */
void gfx_draw(const GfxDraw* d)
{
    (void)d;
    g_prof.draws++;
}
void gfx_present(GfxTex* backbuffer)
{
    (void)backbuffer;
    if (!gfx_profiling)
        return;
    g_present_thread = plat_thread_id();
    prof_frame(gfx_now_ns());
}
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
void gfx_fx_set(const char* key, float v) { (void)key, (void)v; }
float gfx_fx_get(const char* key) { return (void)key, 0.0f; }
void gfx_trace_dump(const char* path) { (void)path; }
void gfx_finish(void) {}
uint32_t gfx_failures(void) { return 0; }
void gfx_set_sync_pipelines(int on) { (void)on; }
