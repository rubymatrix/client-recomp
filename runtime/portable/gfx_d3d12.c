/* The graphics back end on Direct3D 12 (Windows x64): what gfx.h asks for, on one ID3D12Device.
 * The same design as gfx_metal.m, in D3D12's terms:
 *
 *   - Frames: one command list recording at a time, executed at Present (or when the CPU needs a
 *     result, or in chunks mid-frame so the GPU starts early); up to three frames in flight, each
 *     with its own command allocator and upload ring - vertex, index and uniform bytes are copied
 *     into the ring per draw, so the game may rewrite a buffer the moment a draw returns, as D3D
 *     lets it. One fence counts submissions; a resource remembers the submission that last used it.
 *   - Resource states: textures carry their state (whole resource) and transition on use; buffers
 *     decay to COMMON after every submission, so they are tracked per command list.
 *   - Binding: one root signature for everything - the uniforms (b0), the draw's texture and sampler
 *     heap indices as root constants (b1), the vertex streams as raw root SRVs (t0..t3), and the whole
 *     shader-visible heaps as unbounded Texture2D / TextureCube / sampler tables. A texture's SRV lives
 *     in the heap for its lifetime; a draw binds nothing but 16 indices.
 *   - Pipelines come from the generated HLSL (gfx_hlsl.c), compiled with D3DCompile (SM 5.1) and
 *     built off the game's thread, cached by key: shaders, blending, depth-stencil, rasterizer and
 *     topology type (D3D12 bakes all of them into the pipeline). Samplers are cached by key.
 *   - Clears are D3D12's own (with rectangles); the back buffer reaches the window through a flip
 *     swap chain on the SDL window, drawn scaled with the frame-rate overlay on top.
 *   - Textures are default-heap resources filled through the ring with CopyTextureRegion. Formats
 *     D3D12 has no sampled match for (the 16-bit color ones) are widened to BGRA8 on upload, as on
 *     Metal; the swizzled ones (X8R8G8B8, L8, A8L8) are SRV component mappings. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <SDL3/SDL.h>
#if defined(FFXI_UWP)
#include "uwp_bridge.h"
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_hlsl.h"

#define FRAMES 3
#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)
#define SRV_HEAP 65536u    /* shader-visible CBV/SRV/UAV descriptors: one per texture */
#define SAMPLER_HEAP 2048u /* the most a shader-visible sampler heap may hold */
#define RTV_HEAP 4096u
#define DSV_HEAP 512u
#define SRV_NULL_2D 0 /* heap slots holding null views: an unbound Texture2D / TextureCube */
#define SRV_NULL_CUBE 1

/* D3DFORMAT values the back end maps */
enum
{
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_L8 = 50,
    F_A8L8 = 51,
    F_V8U8 = 60,
    F_D24S8 = 75,
    F_D24X8 = 77,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum
{
    CONV_NONE,
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
};

/* the root signature's parameters */
enum
{
    ROOT_U,       /* CBV b0: GfxU (the utility functions' own constants too) */
    ROOT_BIND,    /* 16 constants b1: texture heap indices ti[8], sampler heap indices si[8] */
    ROOT_STREAM0, /* raw SRVs t0..t3: the vertex streams */
    ROOT_TEX2D = ROOT_STREAM0 + GFX_NSTREAMS,
    ROOT_TEXCUBE,
    ROOT_SAMPLERS,
    ROOT_COUNT,
};

struct GfxTex
{
    ID3D12Resource* res;
    DXGI_FORMAT dxfmt;
    int type, use, conv;
    uint32_t fmt, w, h, levels; /* D3D's sizes */
    uint32_t rw, rh;            /* the resource's: DXT top levels round up to whole blocks */
    uint32_t faces;
    uint32_t block;       /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel;       /* bytes per texel in D3D12's layout */
    uint64_t used;        /* the last submission that referenced it */
    int has_stencil, x8;
    D3D12_RESOURCE_STATES state;
    int32_t srv;          /* its slot in the shader-visible heap, or -1 */
    int32_t* views;       /* RTV (or DSV) slots per subresource, created on first use; -1 until then */
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the submission each was recorded in */
    ID3D12Resource* rb[GFX_READBACKS];
    uint8_t* rb_cpu[GFX_READBACKS];
    uint64_t rb_size[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
};

/* Static buffers live in slabs: one resource carved into equal blocks of a power-of-two size (256 bytes
 * to 256 KB), or, for a larger buffer, a resource of its own. A committed resource per buffer cost
 * 0.7 ms each on an Xbox One X, and a zone load creates over a thousand. Resource states are the
 * slab's: a barrier covers every block in it. */
typedef struct Slab
{
    ID3D12Resource* res;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    D3D12_RESOURCE_STATES state; /* in command list `list`; COMMON in any other (buffers decay) */
    uint32_t list;
    int cls;                     /* the block size's class, or -1: the whole resource is one buffer */
    uint32_t* free_blocks;       /* offsets of the blocks not in use */
    uint32_t nfree;
    struct Slab* next;           /* the class's slabs */
} Slab;

struct GfxBuf
{
    Slab* slab;
    uint32_t off;                  /* in the slab's resource */
    D3D12_GPU_VIRTUAL_ADDRESS gpu; /* the slab's, plus off */
    uint32_t size;
};

typedef struct Chunk
{
    ID3D12Resource* res;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint32_t used, size;
} Chunk;

typedef struct Frame
{
    ID3D12CommandAllocator* alloc;
    Chunk* chunks;
    uint32_t nchunks, cur;
    uint64_t fence;   /* the last submission of the frame */
    int timed;        /* its GPU timestamps were resolved (profile) */
} Frame;

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static ID3D12GraphicsCommandList* g_list;
static int g_list_open;
static uint32_t g_list_id;    /* counts command lists recorded (buffer state decay) */
static ID3D12Fence* g_fence;
static HANDLE g_fence_event;
static uint64_t g_fence_value; /* the last submission signalled */
static ID3D12RootSignature* g_root;
static Frame g_frames[FRAMES];
static uint32_t g_frame;       /* index into g_frames */
static uint64_t g_serial = 1;  /* the frame being recorded */
static int g_frame_open;
static uint32_t g_cmd_draws;   /* draws in the command list being recorded */
static uint64_t g_gpu_ns;      /* GPU time of finished frames (profile) */

/* descriptor heaps: shader-visible SRVs and samplers, CPU-only RTVs and DSVs */
typedef struct Heap
{
    ID3D12DescriptorHeap* heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    UINT inc;
    int32_t* free_slots;
    uint32_t nfree;
} Heap;

static Heap g_srv, g_samp, g_rtv, g_dsv;

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static GfxTex* g_scratch_depth;
static int g_targets_bound;
static ID3D12PipelineState* g_bound_pso;
static int g_bound_topo;
static ID3D12Resource* g_dummy;
static ID3D12Resource* g_sync_rb; /* gfx_tex_read's staging buffer */
static uint64_t g_sync_rb_size;

/* the window */
static SDL_Window* g_window;
static IDXGIFactory4* g_factory;
static IDXGISwapChain3* g_swap;
static ID3D12Resource* g_swap_buf[FRAMES];
static D3D12_RESOURCE_STATES g_swap_state[FRAMES];
static int32_t g_swap_rtv[FRAMES];
static uint32_t g_swap_w, g_swap_h, g_want_w, g_want_h;
static int g_vsync, g_tearing;
static ID3D12PipelineState *g_present_pso, *g_overlay_pso;
static uint32_t g_present_samp;
/* the frame-rate overlay: presents counted over half-second windows */
static int g_overlay = 1;
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";

/* FFXI_D3D12_DEBUG=1: the debug layer's messages, printed after each submission */
static ID3D12InfoQueue* g_info;

/* GPU timestamps, two per frame (profile) */
static ID3D12QueryHeap* g_queries;
static ID3D12Resource* g_query_rb;
static uint64_t* g_query_cpu;
static uint64_t g_ts_freq;

/* --- small hash maps (key bytes -> pointer) ------------------------------------------------------------ */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    void* obj;
} MapEnt;

typedef struct Map
{
    MapEnt* e;
    uint32_t cap, n;
} Map;

static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1;
}

static MapEnt* map_find(Map* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e;
    }
}

static void* map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : NULL;
}

static void map_put(Map* m, const void* key, size_t klen, void* obj)
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map old = *m;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (MapEnt*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].hash)
            {
                uint32_t j = (uint32_t)old.e[i].hash & (m->cap - 1);
                while (m->e[j].hash)
                    j = (j + 1) & (m->cap - 1);
                m->e[j] = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = fnv(key, klen);
    uint32_t j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].hash)
        j = (j + 1) & (m->cap - 1);
    m->e[j].hash = h;
    m->e[j].key = malloc(klen);
    memcpy(m->e[j].key, key, klen);
    m->e[j].klen = klen;
    m->e[j].obj = obj;
    m->n++;
}

static Map g_pipes, g_samplers;

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;

uint64_t gfx_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

static DWORD g_present_thread;

void gfx_prof_shim(uint64_t ns)
{
    if (GetCurrentThreadId() == g_present_thread)
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went */
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
    double front = (double)g_prof.front_ns * ms, enc = (double)g_prof.draw_ns * ms;
    double sem = (double)g_prof.sem_ns * ms, drw = (double)g_prof.drawable_ns * ms, pres = (double)g_prof.present_ns * ms;
    double shims = (double)g_prof.shim_ns * ms, probe = (double)g_prof.probe_ns * ms;
    double gpu = (double)g_gpu_ns * ms;
    g_gpu_ns = 0;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [swap chain %.2f], other %.2f) | GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, gpu, (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0,
            (unsigned long long)g_prof.pipelines);
    else
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, swap chain %.2f) | "
            "GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw, gpu,
            (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

static volatile LONG g_failures;

uint32_t gfx_failures(void) { return (uint32_t)g_failures; }

/* --- descriptor heaps ------------------------------------------------------------------------------------- */
static int heap_init(Heap* h, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t n, int visible, uint32_t reserved)
{
    D3D12_DESCRIPTOR_HEAP_DESC d = { type, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_dev, &d, &IID_ID3D12DescriptorHeap, (void**)&h->heap)))
        return 0;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->heap, &h->cpu);
    if (visible)
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h->heap, &h->gpu);
    h->inc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, type);
    h->free_slots = (int32_t*)malloc(sizeof(int32_t) * n);
    h->nfree = 0;
    for (uint32_t i = n; i-- > reserved;) /* low slots first */
        h->free_slots[h->nfree++] = (int32_t)i;
    return 1;
}

static int32_t heap_alloc(Heap* h)
{
    return h->nfree ? h->free_slots[--h->nfree] : -1;
}

static void heap_free(Heap* h, int32_t slot)
{
    if (slot >= 0)
        h->free_slots[h->nfree++] = slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(const Heap* h, int32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c = { h->cpu.ptr + (SIZE_T)slot * h->inc };
    return c;
}

/* --- lifetimes: what the GPU may still read is released once the fence passes it --------------------------- */
typedef struct Dead
{
    IUnknown* obj;
    Heap* heap; /* or a descriptor slot of this heap */
    int32_t slot;
    uint64_t fence;
    Slab* slab; /* or a slab's block, at offset slot */
} Dead;

static Dead* g_dead;
static uint32_t g_ndead, g_cap_dead;

static uint64_t completed(void)
{
    return ID3D12Fence_GetCompletedValue(g_fence);
}

/* the submission the commands being recorded now will belong to */
static uint64_t pending(void)
{
    return g_fence_value + 1;
}

static void defer(IUnknown* obj, Heap* heap, int32_t slot)
{
    if (!obj && (!heap || slot < 0))
        return;
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ obj, heap, slot, pending(), NULL };
}

/* a slab's block, back in its free list once the GPU is past what may read it */
static void defer_block(Slab* s, uint32_t off)
{
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ NULL, NULL, (int32_t)off, pending(), s };
}

static void collect(void)
{
    uint64_t done = completed();
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_ndead; ++i)
    {
        Dead* d = &g_dead[i];
        if (d->fence <= done)
        {
            if (d->slab)
                d->slab->free_blocks[d->slab->nfree++] = (uint32_t)d->slot;
            else if (d->obj)
                IUnknown_Release(d->obj);
            else
                heap_free(d->heap, d->slot);
        }
        else
            g_dead[k++] = *d;
    }
    g_ndead = k;
}

static void print_messages(void)
{
    UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(g_info);
    for (UINT64 i = 0; i < n; ++i)
    {
        SIZE_T len = 0;
        ID3D12InfoQueue_GetMessage(g_info, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(ID3D12InfoQueue_GetMessage(g_info, i, m, &len)))
            fprintf(stderr, "[d3d12] %s\n", m->pDescription);
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(g_info);
}

static void wait_fence(uint64_t v)
{
    if (completed() >= v)
        return;
    ID3D12Fence_SetEventOnCompletion(g_fence, v, g_fence_event);
    WaitForSingleObject(g_fence_event, INFINITE);
}

/* --- frames, command lists and the upload ring ------------------------------------------------------------ */
static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_fence(f->fence);
    if (gfx_profiling)
    {
        g_prof.sem_ns += gfx_now_ns() - t0;
        if (f->timed && g_query_cpu && g_ts_freq)
        {
            uint64_t a = g_query_cpu[2 * g_frame], b = g_query_cpu[2 * g_frame + 1];
            if (b > a)
                g_gpu_ns += (uint64_t)((double)(b - a) * 1e9 / (double)g_ts_freq);
        }
    }
    f->timed = 0;
    ID3D12CommandAllocator_Reset(f->alloc);
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
    collect();
    g_frame_open = 1;
}

/* the command list being recorded, opened (and set up) if none is */
static ID3D12GraphicsCommandList* list(void)
{
    frame_begin();
    if (g_list_open)
        return g_list;
    Frame* f = &g_frames[g_frame];
    ID3D12GraphicsCommandList_Reset(g_list, f->alloc, NULL);
    ID3D12DescriptorHeap* heaps[2] = { g_srv.heap, g_samp.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_root);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEX2D, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEXCUBE, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_SAMPLERS, g_samp.gpu);
    if (gfx_profiling && g_queries && !f->timed)
    {
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame);
        f->timed = 1;
    }
    g_list_open = 1;
    g_list_id++;
    g_targets_bound = 0;
    g_bound_pso = NULL;
    g_bound_topo = -1;
    return g_list;
}

/* executes what is recorded; the frame stays open (its ring is still in use) */
static void submit(int wait)
{
    if (!g_list_open)
        return;
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12CommandList* l = (ID3D12CommandList*)g_list;
    ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, &l);
    ID3D12CommandQueue_Signal(g_queue, g_fence, ++g_fence_value);
    g_frames[g_frame].fence = g_fence_value;
    g_list_open = 0;
    g_cmd_draws = 0;
    if (wait)
        wait_fence(g_fence_value);
    if (g_info)
        print_messages();
}

static ID3D12Resource* make_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void**)&r)))
    {
        fprintf(stderr, "[recomp] gfx: buffer of %llu bytes failed\n", (unsigned long long)size);
        return NULL;
    }
    return r;
}

typedef struct Alloc
{
    uint8_t* cpu;
    ID3D12Resource* res;
    uint64_t off;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
} Alloc;

/* n bytes of this frame's ring (a large request gets an upload buffer of its own) */
static Alloc ring(size_t n, size_t align)
{
    frame_begin();
    Alloc a = { 0 };
    g_prof.bytes += n;
    if (n > RING_CHUNK / 2)
    {
        a.res = make_buffer(n, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!a.res)
            return a;
        ID3D12Resource_Map(a.res, 0, NULL, (void**)&a.cpu);
        a.gpu = ID3D12Resource_GetGPUVirtualAddress(a.res);
        defer((IUnknown*)a.res, NULL, -1); /* released once this frame's work is done */
        return a;
    }
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            Chunk c = { 0 };
            c.res = make_buffer(RING_CHUNK, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!c.res)
                return a;
            ID3D12Resource_Map(c.res, 0, NULL, (void**)&c.cpu);
            c.gpu = ID3D12Resource_GetGPUVirtualAddress(c.res);
            c.size = RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks++] = c;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            a.cpu = c->cpu + at, a.res = c->res, a.off = at, a.gpu = c->gpu + at;
            return a;
        }
    }
}

static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list(), 1, &b);
}

static void tex_state(GfxTex* t, D3D12_RESOURCE_STATES want)
{
    if (t->state != want)
    {
        barrier(t->res, t->state, want);
        t->state = want;
    }
    t->used = pending();
}

#define BUF_READ (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER)

static void buf_state(GfxBuf* b, D3D12_RESOURCE_STATES want)
{
    list();
    Slab* s = b->slab;
    D3D12_RESOURCE_STATES cur = s->list == g_list_id ? s->state : D3D12_RESOURCE_STATE_COMMON;
    if (cur != want)
        barrier(s->res, cur, want);
    s->state = want, s->list = g_list_id;
}

/* --- formats ---------------------------------------------------------------------------------------------- */
#define MAP4(r, g, b, a) D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a)
#define ONE D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1

static DXGI_FORMAT dx_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, UINT* mapping)
{
    *conv = CONV_NONE, *block = 0, *texel = 4;
    *mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    switch (fmt)
    {
    case F_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X8R8G8B8: *mapping = MAP4(0, 1, 2, ONE); return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_R5G6B5: *conv = CONV_565; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X1R5G5B5: *conv = CONV_X555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A1R5G5B5: *conv = CONV_1555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A4R4G4B4: *conv = CONV_4444; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A8: *texel = 1; return DXGI_FORMAT_A8_UNORM;
    case F_L8: *texel = 1, *mapping = MAP4(0, 0, 0, ONE); return DXGI_FORMAT_R8_UNORM;
    case F_A8L8: *texel = 2, *mapping = MAP4(0, 0, 0, 1); return DXGI_FORMAT_R8G8_UNORM;
    case F_V8U8: *texel = 2; return DXGI_FORMAT_R8G8_SNORM;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, DXGI_FORMAT_BC1_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, DXGI_FORMAT_BC2_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, DXGI_FORMAT_BC3_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

static uint32_t d3d_bpp(uint32_t fmt)
{
    switch (fmt)
    {
    case F_A8:
    case F_L8: return 1;
    case F_R5G6B5:
    case F_X1R5G5B5:
    case F_A1R5G5B5:
    case F_A4R4G4B4:
    case F_A8L8:
    case F_V8U8: return 2;
    default: return 4;
    }
}

static uint32_t expand(uint32_t v, int bits)
{
    return bits == 1 ? (v ? 255u : 0u) : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

/* one row of a 16-bit D3D format into BGRA8 */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t p = (uint32_t)src[2 * x] | ((uint32_t)src[2 * x + 1] << 8), r, g, b, a;
        switch (conv)
        {
        case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
        case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
        case CONV_1555:
            r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
            break;
        default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* --- static buffers --------------------------------------------------------------------------------------------- */
#define SLAB_SIZE (2u << 20)
#define SLAB_MIN_SHIFT 8  /* 256-byte blocks, the smallest class */
#define SLAB_CLASSES 11   /* up to 256 KB blocks; larger buffers get a resource each */
static Slab* g_slabs[SLAB_CLASSES];

static Slab* slab_new(uint64_t size, int cls)
{
    Slab* s = (Slab*)calloc(1, sizeof *s);
    s->res = make_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!s->res)
    {
        free(s);
        return NULL;
    }
    s->gpu = ID3D12Resource_GetGPUVirtualAddress(s->res);
    s->state = D3D12_RESOURCE_STATE_COMMON;
    s->cls = cls;
    if (cls >= 0)
    {
        uint32_t block = 1u << (SLAB_MIN_SHIFT + cls), n = SLAB_SIZE / block;
        s->free_blocks = (uint32_t*)malloc(n * sizeof *s->free_blocks);
        for (uint32_t i = 0; i < n; ++i) /* handed out from offset 0 up */
            s->free_blocks[i] = (n - 1 - i) * block;
        s->nfree = n;
    }
    return s;
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    uint32_t need = (size + 15) & ~15u;
    int cls = 0;
    while (cls < SLAB_CLASSES && (1u << (SLAB_MIN_SHIFT + cls)) < need)
        cls++;
    if (cls == SLAB_CLASSES)
    {
        b->slab = slab_new(need, -1);
        b->off = 0;
    }
    else
    {
        Slab* s = g_slabs[cls];
        while (s && !s->nfree)
            s = s->next;
        if (!s && (s = slab_new(SLAB_SIZE, cls)) != NULL)
            s->next = g_slabs[cls], g_slabs[cls] = s;
        b->slab = s;
        if (s)
            b->off = s->free_blocks[--s->nfree];
    }
    if (!b->slab)
    {
        free(b);
        return NULL;
    }
    b->gpu = b->slab->gpu + b->off;
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    if (b->slab->cls < 0)
    {
        defer((IUnknown*)b->slab->res, NULL, -1);
        free(b->slab);
    }
    else
        defer_block(b->slab, b->off);
    free(b);
}

/* in command order: draws recorded before this read the old contents, draws after the new */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    if (size > b->size)
        size = b->size;
    if (!size)
        return;
    Alloc a = ring(size, 16);
    if (!a.cpu)
        return;
    memcpy(a.cpu, data, size);
    buf_state(b, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(list(), b->slab->res, b->off, a.res, a.off, size);
    buf_state(b, BUF_READ);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
static uint32_t subresource(const GfxTex* t, uint32_t face, uint32_t level)
{
    return level + face * t->levels;
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    UINT mapping;
    t->dxfmt = dx_format(fmt, use, &t->conv, &t->block, &t->texel, &mapping);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    if (type == GFX_TEX_CUBE)
        t->h = t->w;
    t->faces = type == GFX_TEX_CUBE ? 6 : 1;
    t->rw = t->w, t->rh = t->h;
    if (t->block) /* D3D12 wants the top level of a block-compressed texture in whole blocks */
        t->rw = (t->rw + 3) & ~3u, t->rh = (t->rh + 3) & ~3u;
    uint32_t most = 1;
    for (uint32_t s = t->rw > t->rh ? t->rw : t->rh; s > 1; s >>= 1)
        most++;
    if (t->levels > most)
        t->levels = most;
    t->has_stencil = t->dxfmt == DXGI_FORMAT_D24_UNORM_S8_UINT;
    t->x8 = fmt == F_X8R8G8B8;
    t->srv = -1;
    D3D12_RESOURCE_FLAGS flags = use == GFX_USE_RT ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        : use == GFX_USE_DEPTH ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
                               : D3D12_RESOURCE_FLAG_NONE;
    t->state = use == GFX_USE_RT ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : use == GFX_USE_DEPTH  ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                : D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, t->rw, t->rh, (UINT16)t->faces, (UINT16)t->levels, t->dxfmt,
        { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, t->state, NULL, &IID_ID3D12Resource,
            (void**)&t->res)))
    {
        fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    if (use != GFX_USE_DEPTH)
    {
        t->srv = heap_alloc(&g_srv);
        if (t->srv >= 0)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            v.Shader4ComponentMapping = mapping;
            if (type == GFX_TEX_CUBE)
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, v.TextureCube.MipLevels = t->levels;
            else
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = t->levels;
            ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->srv));
        }
        else
            fprintf(stderr, "[recomp] gfx: out of texture descriptors\n");
    }
    if (use != GFX_USE_SAMPLE)
    {
        uint32_t n = t->faces * t->levels;
        t->views = (int32_t*)malloc(sizeof(int32_t) * n);
        for (uint32_t i = 0; i < n; ++i)
            t->views[i] = -1;
    }
    return t;
}

static void release_views(GfxTex* t)
{
    if (!t->views)
        return;
    for (uint32_t i = 0; i < t->faces * t->levels; ++i)
        defer(NULL, t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv, t->views[i]);
    free(t->views);
    t->views = NULL;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL, g_targets_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_targets_bound = 0;
    defer((IUnknown*)t->res, NULL, -1);
    defer(NULL, &g_srv, t->srv);
    release_views(t);
    for (int i = 0; i < GFX_READBACKS; ++i)
        defer((IUnknown*)t->rb[i], NULL, -1);
    free(t);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = t->h >> level ? t->h >> level : 1;
}

/* the resource's own size of a level (differs from D3D's only for rounded-up DXT) */
static void phys_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->rw >> level ? t->rw >> level : 1;
    *h = t->rh >> level ? t->rh >> level : 1;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || !w || !h || t->use == GFX_USE_DEPTH)
        return;
    uint32_t lw, lh;
    level_size(t, level, &lw, &lh);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    uint32_t rows = t->block ? (h + 3) / 4 : h;
    uint32_t cols = t->block ? (w + 3) / 4 : w; /* blocks or texels per row */
    if (t->block)
    {
        uint32_t pw, ph;
        phys_size(t, level, &pw, &ph);
        uint32_t most_c = (pw + 3) / 4 - x / 4, most_r = (ph + 3) / 4 - y / 4;
        cols = cols < most_c ? cols : most_c;
        rows = rows < most_r ? rows : most_r;
    }
    uint32_t row_bytes = t->block ? cols * t->block : cols * t->texel;
    uint32_t up_pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Alloc a = ring((size_t)up_pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t r = 0; r < rows; ++r)
    {
        if (t->conv)
            convert_row(t->conv, s + (size_t)r * pitch, a.cpu + (size_t)r * up_pitch, cols);
        else
            memcpy(a.cpu + (size_t)r * up_pitch, s + (size_t)r * pitch, row_bytes);
    }
    tex_state(t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = subresource(t, face, level);
    from.pResource = a.res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = a.off;
    from.PlacedFootprint.Footprint.Format = t->dxfmt;
    from.PlacedFootprint.Footprint.Width = t->block ? cols * 4 : cols;
    from.PlacedFootprint.Footprint.Height = t->block ? rows * 4 : rows;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = up_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, x, y, 0, &from, NULL);
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in D3D12's layout -> D3D's */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, uint32_t row, void* dst, uint32_t pitch)
{
    uint32_t bpp = d3d_bpp(t->fmt);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* d = (uint8_t*)dst + (size_t)y * pitch;
        if (!t->conv)
            memcpy(d, s + (size_t)y * row, (size_t)w * bpp);
        else /* 16-bit color targets: back from BGRA8 */
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = s + (size_t)y * row + 4 * x;
                uint32_t b = p[0], g = p[1], r = p[2], a = p[3], v;
                switch (t->conv)
                {
                case CONV_565: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
                case CONV_4444: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
                default: v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
                }
                d[2 * x] = (uint8_t)v, d[2 * x + 1] = (uint8_t)(v >> 8);
            }
    }
}

static uint32_t readback_row(const GfxTex* t, uint32_t w)
{
    return (w * t->texel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

/* a copy of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t row, ID3D12Resource* buf)
{
    tex_state(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    from.pResource = t->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(t, face, level);
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = t->dxfmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row;
    D3D12_BOX box = { 0, 0, 0, w, h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, 0, 0, 0, &from, &box);
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (size > g_sync_rb_size)
    {
        defer((IUnknown*)g_sync_rb, NULL, -1);
        g_sync_rb = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        g_sync_rb_size = g_sync_rb ? size : 0;
        if (!g_sync_rb)
            return;
    }
    queue_readback(t, face, level, w, h, row, g_sync_rb);
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    submit(1);
    if (gfx_profiling)
        g_prof.probe_ns += gfx_now_ns() - t0;
    uint8_t* p = NULL;
    D3D12_RANGE r = { 0, (SIZE_T)size };
    if (SUCCEEDED(ID3D12Resource_Map(g_sync_rb, 0, &r, (void**)&p)))
    {
        copy_out(t, p, w, h, row, dst, pitch);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(g_sync_rb, 0, &none);
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
        return;
    }
    uint64_t done = completed();
    int best = -1;
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
            t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
            best = i;
    if (best < 0)
        gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
    else
        copy_out(t, t->rb_cpu[best], w, h, row, dst, pitch);
    /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
    done = completed();
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
        if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot] || t->rb_size[slot] < size)
    {
        defer((IUnknown*)t->rb[slot], NULL, -1);
        t->rb[slot] = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        t->rb_size[slot] = t->rb[slot] ? size : 0;
        t->rb_cpu[slot] = NULL;
        if (!t->rb[slot])
            return;
        ID3D12Resource_Map(t->rb[slot], 0, NULL, (void**)&t->rb_cpu[slot]); /* readback memory stays mapped */
    }
    queue_readback(t, face, level, w, h, row, t->rb[slot]);
    t->rb_serial[slot] = pending(), t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src == dst || src->dxfmt != dst->dxfmt || src->conv != dst->conv || !w || !h)
        return;
    if (slevel >= src->levels || dlevel >= dst->levels || sface >= src->faces || dface >= dst->faces)
        return;
    if (src->use == GFX_USE_DEPTH) /* D3D12 copies depth whole subresources only */
    {
        uint32_t sw, sh, dw, dh;
        level_size(src, slevel, &sw, &sh);
        level_size(dst, dlevel, &dw, &dh);
        if (sx || sy || dx || dy || w != sw || h != sh || sw != dw || sh != dh)
            return;
    }
    tex_state(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = src->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(src, sface, slevel);
    to.pResource = dst->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = subresource(dst, dface, dlevel);
    D3D12_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, dx, dy, 0, &from, &box);
}

/* --- render targets ---------------------------------------------------------------------------------------------- */
/* Draws recorded since the last submission. A frame goes to the GPU in chunks - executed when the
 * targets change with this many behind it, or mid-scene every SPLIT_DRAWS - so the GPU works while
 * the game builds the rest, and a mid-frame readback (the game's per-frame probe) waits only for
 * the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320

static D3D12_CPU_DESCRIPTOR_HANDLE target_view(GfxTex* t, uint32_t face, uint32_t level)
{
    uint32_t i = subresource(t, face, level);
    Heap* h = t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv;
    if (t->views[i] < 0)
    {
        t->views[i] = heap_alloc(h);
        if (t->views[i] < 0)
        {
            fprintf(stderr, "[recomp] gfx: out of %s descriptors\n", t->use == GFX_USE_DEPTH ? "DSV" : "RTV");
            t->views[i] = 0;
        }
        if (t->use == GFX_USE_DEPTH)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipSlice = level;
            ID3D12Device_CreateDepthStencilView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            if (t->type == GFX_TEX_CUBE)
            {
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipSlice = level, v.Texture2DArray.FirstArraySlice = face, v.Texture2DArray.ArraySize = 1;
            }
            else
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D, v.Texture2D.MipSlice = level;
            ID3D12Device_CreateRenderTargetView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
    }
    return heap_cpu(h, t->views[i]);
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth surface for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D8 lets depth be larger; D3D12 wants them equal) */
static GfxTex* depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return NULL;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return g_ds;
    if (!g_scratch_depth || g_scratch_depth->w != w || g_scratch_depth->h != h || g_scratch_depth->dxfmt != g_ds->dxfmt)
    {
        gfx_tex_destroy(g_scratch_depth);
        g_scratch_depth = gfx_tex_create(GFX_TEX_2D, g_ds->fmt, w, h, 1, GFX_USE_DEPTH);
    }
    return g_scratch_depth;
}

/* the targets bound on the command list, in their target states */
static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    ID3D12GraphicsCommandList* l = list();
    GfxTex* ds = depth_attachment();
    if (g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET || (ds && ds->state != D3D12_RESOURCE_STATE_DEPTH_WRITE))
        g_targets_bound = 0;
    tex_state(g_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ds)
        tex_state(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!g_targets_bound)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_view(g_rt, g_rt_face, g_rt_level), dsv;
        if (ds)
            dsv = target_view(ds, 0, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, ds ? &dsv : NULL);
        uint32_t w, h;
        color_size(&w, &h);
        D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        g_targets_bound = 1;
    }
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    if (g_cmd_draws >= CHUNK_DRAWS)
        submit(0);
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_targets_bound = 0;
}

/* --- pipelines, built off the game's thread ---------------------------------------------------------------
 * A pipeline for a key seen for the first time is built on the thread pool; the draws that need it
 * are skipped until it is ready (a new effect may miss its first frames; the game never waits for
 * the shader compiler). Every key built is recorded in the pipeline cache file, and at start-up the
 * recorded keys are built again in the background, so a second session has them before it needs
 * them. gfx_set_sync_pipelines(1) (the tests) builds in place and records nothing. */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    uint32_t color, dsv; /* DXGI_FORMAT */
    int32_t zbias;
    uint8_t x8, cull, fill, topo; /* topo: D3D12_PRIMITIVE_TOPOLOGY_TYPE */
} PipeKey;

#define PIPE_FAILED ((void*)1)
#define PIPE_MAGIC 0x31443344u /* "D3D1" */

typedef struct PipeEntry
{
    void* volatile state; /* NULL while building, PIPE_FAILED, or the ID3D12PipelineState */
} PipeEntry;

typedef struct PipeJob
{
    PipeKey k;
    uint32_t *vs, *ps; /* copies of the shader tokens behind k.lib.vs.prog / fs.prog */
    uint32_t nvs, nps;
    PipeEntry* e;
    int record;
} PipeJob;

/* compiled shaders, shared by the pipelines that differ only in fixed state (the thread pool's) */
typedef struct Shaders
{
    ID3DBlob *vs, *ps;
} Shaders;

static int g_sync_pipelines;
static char g_pipe_cache[1024];
static CRITICAL_SECTION g_pipe_file_lock;
static SRWLOCK g_shader_lock = SRWLOCK_INIT;
static Map g_shaders; /* LibKey -> Shaders*, under g_shader_lock */
static volatile LONG g_pipes_building;

void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }

static uint32_t* copy_tok(const uint32_t* t, uint32_t* n)
{
    *n = 0;
    if (!t)
        return NULL;
    uint32_t k = 0;
    while (k < 65536 && t[k] != 0x0000FFFFu)
        k++;
    k++;
    uint32_t* c = (uint32_t*)malloc(4u * k);
    memcpy(c, t, 4u * k);
    *n = k;
    return c;
}

static ID3DBlob* compile(const char* src, const char* entry, const char* target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
        D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: HLSL compile failed (%s): %s\n%s\n", entry, err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?",
            src);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static Shaders* shaders(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    AcquireSRWLockShared(&g_shader_lock);
    Shaders* s = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    ReleaseSRWLockShared(&g_shader_lock);
    if (s)
        return s->vs && s->ps ? s : NULL;
    s = (Shaders*)calloc(1, sizeof *s);
    char* src = gfx_hlsl_generate(&k->vs, &k->fs, vs, ps);
    if (src)
    {
        s->vs = compile(src, "vs_main", "vs_5_1");
        s->ps = s->vs ? compile(src, "fs_main", "ps_5_1") : NULL;
        free(src);
    }
    else
        InterlockedIncrement(&g_failures);
    AcquireSRWLockExclusive(&g_shader_lock);
    Shaders* had = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    if (had) /* another job compiled it meanwhile */
    {
        if (s->vs)
            ID3D10Blob_Release(s->vs);
        if (s->ps)
            ID3D10Blob_Release(s->ps);
        free(s);
        s = had;
    }
    else
        map_put(&g_shaders, k, sizeof *k, s);
    ReleaseSRWLockExclusive(&g_shader_lock);
    return s->vs && s->ps ? s : NULL;
}

static D3D12_BLEND blend_factor(uint32_t f, int x8, int alpha)
{
    switch (f)
    {
    case 1: return D3D12_BLEND_ZERO;
    case 2: return D3D12_BLEND_ONE;
    case 3: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR; /* the alpha factors take no colors */
    case 4: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 5: return D3D12_BLEND_SRC_ALPHA;
    case 6: return D3D12_BLEND_INV_SRC_ALPHA;
    case 7: return x8 ? D3D12_BLEND_ONE : D3D12_BLEND_DEST_ALPHA;
    case 8: return x8 ? D3D12_BLEND_ZERO : D3D12_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_BLEND_OP_SUBTRACT;
    case 3: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D12_BLEND_OP_MIN;
    case 5: return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_COMPARISON_FUNC compare(uint32_t f)
{
    switch (f)
    {
    case 1: return D3D12_COMPARISON_FUNC_NEVER;
    case 2: return D3D12_COMPARISON_FUNC_LESS;
    case 3: return D3D12_COMPARISON_FUNC_EQUAL;
    case 4: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 5: return D3D12_COMPARISON_FUNC_GREATER;
    case 6: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 7: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

static D3D12_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_STENCIL_OP_ZERO;
    case 3: return D3D12_STENCIL_OP_REPLACE;
    case 4: return D3D12_STENCIL_OP_INCR_SAT;
    case 5: return D3D12_STENCIL_OP_DECR_SAT;
    case 6: return D3D12_STENCIL_OP_INVERT;
    case 7: return D3D12_STENCIL_OP_INCR;
    case 8: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

/* the pipeline for a key, or NULL */
static ID3D12PipelineState* build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    Shaders* s = shaders(&k->lib, vs, ps);
    if (!s)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s->vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s->ps);
    D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
    uint32_t wm = k->pipe.write_mask;
    c->RenderTargetWriteMask = (UINT8)(((wm & 1) ? D3D12_COLOR_WRITE_ENABLE_RED : 0) | ((wm & 2) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((wm & 4) ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) | ((wm & 8) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->LogicOp = D3D12_LOGIC_OP_NOOP;
    if (k->pipe.blend)
    {
        uint32_t sf = k->pipe.src, df = k->pipe.dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        c->BlendEnable = TRUE;
        c->SrcBlend = blend_factor(sf, k->x8, 0), c->SrcBlendAlpha = blend_factor(sf, k->x8, 1);
        c->DestBlend = blend_factor(df, k->x8, 0), c->DestBlendAlpha = blend_factor(df, k->x8, 1);
        c->BlendOp = c->BlendOpAlpha = blend_op(k->pipe.op);
    }
    pd.SampleMask = UINT_MAX;
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    pd.RasterizerState.FillMode = k->fill == 2 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = k->cull == 3 ? D3D12_CULL_MODE_BACK : k->cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthBias = -k->zbias;
    pd.RasterizerState.SlopeScaledDepthBias = -(float)k->zbias * 0.5f;
    pd.RasterizerState.DepthClipEnable = TRUE;
    const GfxDepthKey* dk = &k->depth;
    pd.DepthStencilState.DepthEnable = dk->zenable ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = dk->zenable && dk->zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = dk->zenable ? compare(dk->zfunc) : D3D12_COMPARISON_FUNC_ALWAYS;
    if (dk->stencil)
    {
        D3D12_DEPTH_STENCILOP_DESC st = { stencil_op(dk->sfail), stencil_op(dk->szfail), stencil_op(dk->spass), compare(dk->sfunc) };
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = dk->sread;
        pd.DepthStencilState.StencilWriteMask = dk->swrite;
        pd.DepthStencilState.FrontFace = st;
        pd.DepthStencilState.BackFace = st;
    }
    pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)k->topo;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = (DXGI_FORMAT)k->color;
    pd.DSVFormat = (DXGI_FORMAT)k->dsv;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: pipeline failed (%08lx)\n", (unsigned long)hr);
        return NULL;
    }
    return p;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const PipeJob* j)
{
    if (!g_pipe_cache[0])
        return;
    uint32_t hdr[2] = { PIPE_MAGIC, (uint32_t)sizeof(PipeKey) };
    EnterCriticalSection(&g_pipe_file_lock);
    FILE* f = fopen(g_pipe_cache, "ab");
    if (f)
    {
        fwrite(hdr, 4, 2, f);
        fwrite(&j->k, sizeof j->k, 1, f);
        fwrite(&j->nvs, 4, 1, f);
        if (j->nvs)
            fwrite(j->vs, 4, j->nvs, f);
        fwrite(&j->nps, 4, 1, f);
        if (j->nps)
            fwrite(j->ps, 4, j->nps, f);
        fclose(f);
    }
    LeaveCriticalSection(&g_pipe_file_lock);
}

static void run_job(PipeJob* j)
{
    ID3D12PipelineState* p = build_pipeline(&j->k, j->vs, j->ps);
    InterlockedExchangePointer(&j->e->state, p ? (void*)p : PIPE_FAILED);
    if (p && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
    InterlockedDecrement(&g_pipes_building);
}

static void CALLBACK job_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    run_job((PipeJob*)ctx);
}

static void queue_job(const PipeKey* k, const uint32_t* vs, uint32_t nvs, const uint32_t* ps, uint32_t nps, int record)
{
    if (map_get(&g_pipes, k, sizeof *k))
        return;
    PipeEntry* e = (PipeEntry*)calloc(1, sizeof *e);
    map_put(&g_pipes, k, sizeof *k, e);
    PipeJob* j = (PipeJob*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->record = record;
    if (nvs)
        j->vs = (uint32_t*)malloc(4u * nvs), memcpy(j->vs, vs, 4u * nvs), j->nvs = nvs;
    if (nps)
        j->ps = (uint32_t*)malloc(4u * nps), memcpy(j->ps, ps, 4u * nps), j->nps = nps;
    g_prof.pipelines++;
    InterlockedIncrement(&g_pipes_building);
    if (g_sync_pipelines || !TrySubmitThreadpoolCallback(job_callback, j, NULL))
        run_job(j);
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_pipelines(void)
{
    const char* dir = getenv("FFXI_CACHE_DIR");
    char path[900];
    if (dir && *dir)
        snprintf(path, sizeof path, "%s", dir);
    else if (getenv("LOCALAPPDATA"))
        snprintf(path, sizeof path, "%s\\FFXI", getenv("LOCALAPPDATA"));
    else
        return;
    CreateDirectoryA(path, NULL);
    snprintf(g_pipe_cache, sizeof g_pipe_cache, "%s\\pipelines.d3d12.v1", path);
    FILE* f = fopen(g_pipe_cache, "rb");
    if (!f)
        return;
    uint32_t n = 0, hdr[2];
    while (fread(hdr, 4, 2, f) == 2 && hdr[0] == PIPE_MAGIC && hdr[1] == sizeof(PipeKey))
    {
        PipeKey k;
        uint32_t nvs = 0, nps = 0, *vs = NULL, *ps = NULL;
        int ok = fread(&k, sizeof k, 1, f) == 1 && fread(&nvs, 4, 1, f) == 1 && nvs <= 65536;
        if (ok && nvs)
            vs = (uint32_t*)malloc(4u * nvs), ok = fread(vs, 4, nvs, f) == nvs;
        ok = ok && fread(&nps, 4, 1, f) == 1 && nps <= 65536;
        if (ok && nps)
            ps = (uint32_t*)malloc(4u * nps), ok = fread(ps, 4, nps, f) == nps;
        if (ok)
            queue_job(&k, vs, nvs, ps, nps, 0), n++;
        free(vs);
        free(ps);
        if (!ok)
            break;
    }
    fclose(f);
    fprintf(stderr, "[recomp] gfx: building %u pipelines from %s in the background\n", n, g_pipe_cache);
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case GFX_LINELIST:
    case GFX_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

static ID3D12PipelineState* pipeline(const GfxDraw* d, GfxTex* ds)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.color = (uint32_t)g_rt->dxfmt;
    k.dsv = ds ? (uint32_t)ds->dxfmt : (uint32_t)DXGI_FORMAT_UNKNOWN;
    if (ds)
    {
        k.depth = d->depth;
        if (!ds->has_stencil)
            k.depth.stencil = 0;
        if (!k.depth.stencil)
            k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
    }
    k.x8 = (uint8_t)g_rt->x8;
    k.cull = d->cull, k.fill = d->fill == 2 ? 2 : 3;
    k.zbias = ds ? d->zbias : 0;
    k.topo = (uint8_t)topology_type(d->prim);
    /* runs of draws share a pipeline: the last one found is checked before hashing the key (the hash
     * and the map's probe were 3% of the game thread on an Xbox One X) */
    static PipeKey last_key;
    static ID3D12PipelineState* last_state;
    if (last_state && !memcmp(&k, &last_key, sizeof k))
        return last_state;
    PipeEntry* e = (PipeEntry*)map_get(&g_pipes, &k, sizeof k);
    if (!e)
    {
        uint32_t nvs = 0, nps = 0;
        uint32_t* vs = d->vs.prog ? copy_tok(d->vs_tokens, &nvs) : NULL;
        uint32_t* ps = d->fs.prog ? copy_tok(d->ps_tokens, &nps) : NULL;
        queue_job(&k, vs, nvs, ps, nps, !g_sync_pipelines);
        free(vs);
        free(ps);
        e = (PipeEntry*)map_get(&g_pipes, &k, sizeof k);
    }
    void* st = InterlockedCompareExchangePointer(&e->state, NULL, NULL);
    if (!st || st == PIPE_FAILED)
        return NULL;
    last_key = k, last_state = (ID3D12PipelineState*)st;
    return last_state;
}

static D3D12_TEXTURE_ADDRESS_MODE address(uint32_t a)
{
    switch (a)
    {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

/* the sampler heap slot for a key: made on first use, kept */
static uint32_t sampler(const GfxSampler* k)
{
    void* s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return (uint32_t)(uintptr_t)s - 1;
    int32_t slot = heap_alloc(&g_samp);
    if (slot < 0)
        return g_present_samp; /* the heap is full: any sampler beats none */
    D3D12_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    D3D12_FILTER_TYPE mn = k->min >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mg = k->mag >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mp = k->mip >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mn, mg, mp, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.MaxAnisotropy = 1;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        sd.Filter = D3D12_FILTER_ANISOTROPIC, sd.MaxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    sd.AddressU = address(k->addr_u);
    sd.AddressV = address(k->addr_v);
    sd.AddressW = address(k->addr_w);
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.BorderColor[0] = ((k->border >> 16) & 255) / 255.0f;
    sd.BorderColor[1] = ((k->border >> 8) & 255) / 255.0f;
    sd.BorderColor[2] = (k->border & 255) / 255.0f;
    sd.BorderColor[3] = (k->border >> 24) / 255.0f;
    sd.MinLOD = k->max_level;
    sd.MaxLOD = k->mip == 0 ? (float)k->max_level : D3D12_FLOAT32_MAX; /* no mipmapping: that one level */
    ID3D12Device_CreateSampler(g_dev, &sd, heap_cpu(&g_samp, slot));
    map_put(&g_samplers, k, sizeof *k, (void*)(uintptr_t)(slot + 1));
    return (uint32_t)slot;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    float x = (float)vp[0], y = (float)vp[1], vw = (float)vp[2], vh = (float)vp[3];
    if (x > w)
        x = (float)w;
    if (y > h)
        y = (float)h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    D3D12_VIEWPORT v = { x, y, vw, vh, zmin, zmax };
    ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v);
}

/* GfxDraw.scissor, clamped to the target; none: all of it */
static void set_scissor(const int32_t sc[4])
{
    uint32_t w, h;
    color_size(&w, &h);
    LONG x0 = 0, y0 = 0, x1 = (LONG)w, y1 = (LONG)h;
    if (sc[2] > 0)
    {
        x0 = sc[0] < 0 ? 0 : sc[0];
        y0 = sc[1] < 0 ? 0 : sc[1];
        x1 = sc[0] + sc[2];
        y1 = sc[1] + sc[3];
        if (x1 > (LONG)w)
            x1 = (LONG)w;
        if (y1 > (LONG)h)
            y1 = (LONG)h;
        if (x1 < x0)
            x1 = x0;
        if (y1 < y0)
            y1 = y0;
    }
    D3D12_RECT r = { x0, y0, x1, y1 };
    ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &r);
}

/* index count for a D3D primitive count */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP: return n + 2;
    case GFX_TRIANGLEFAN: return n * 3; /* as a list */
    default: return 0;
    }
}

static D3D12_PRIMITIVE_TOPOLOGY topology(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case GFX_LINELIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case GFX_LINESTRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case GFX_TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static void set_pso(ID3D12PipelineState* p)
{
    if (p != g_bound_pso)
        ID3D12GraphicsCommandList_SetPipelineState(g_list, p), g_bound_pso = p;
}

static void set_topology(D3D12_PRIMITIVE_TOPOLOGY t)
{
    if ((int)t != g_bound_topo)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, t), g_bound_topo = (int)t;
}

static void draw_encode(const GfxDraw* d);

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_encode(d);
    /* a long scene goes to the GPU in pieces */
    if (g_cmd_draws >= SPLIT_DRAWS)
        submit(0);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

static void draw_encode(const GfxDraw* d)
{
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* ds = depth_attachment();
    ID3D12PipelineState* p = pipeline(d, ds);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* still building (or failed) */
        return;
    }
    ID3D12GraphicsCommandList* l = g_list;
    /* textures first: their transitions go before the draw */
    uint32_t bind[16];
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        bind[i] = wanted == 2 ? SRV_NULL_CUBE : SRV_NULL_2D;
        bind[8 + i] = g_present_samp;
        if (!wanted)
            continue;
        bind[8 + i] = sampler(&d->samp[i]);
        if (!t || t->srv < 0 || t == g_rt || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            continue; /* unbound, or the target being drawn to: reads as the null view */
        tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        bind[i] = (uint32_t)t->srv;
    }
    if (g_targets_bound == 0 || g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
        bind_targets();
    set_pso(p);
    set_topology(topology(d->prim));
    set_viewport(d->vp);
    set_scissor(d->scissor);
    if (ds && ds->has_stencil && d->depth.stencil)
        ID3D12GraphicsCommandList_OMSetStencilRef(l, d->stencil_ref);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);

    /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's
     * constants only for a vertex shader, the pixel shader's only for a pixel shader (the ring
     * keeps room for the whole struct: that is what the functions are compiled against) */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    Alloc u = ring(sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!u.cpu)
        return;
    memcpy(u.cpu, &d->u, need);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, u.gpu);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(g_dummy);
        if (d->buf[s])
        {
            buf_state(d->buf[s], BUF_READ);
            va = d->buf[s]->gpu + d->buf_off[s];
        }
        else if (d->data[s] && d->size[s])
        {
            Alloc v = ring(d->size[s] + 16, 16); /* the slack: a last element read whole */
            if (!v.cpu)
                return;
            memcpy(v.cpu, d->data[s], d->size[s]);
            va = v.gpu;
        }
        ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + s, va);
    }

    uint32_t n = vertex_count(d->prim, d->count);
    D3D12_INDEX_BUFFER_VIEW ibv;
    if (d->prim == GFX_TRIANGLEFAN)
    {
        /* no fans in D3D12: a list with the same vertices */
        Alloc a = ring((size_t)n * 4, 16);
        if (!a.cpu)
            return;
        uint32_t* idx = (uint32_t*)a.cpu;
        for (uint32_t i = 0; i < d->count; ++i)
        {
            uint32_t k[3] = { 0, i + 1, i + 2 };
            for (int j = 0; j < 3; ++j)
            {
                if (!d->indices)
                    idx[3 * i + j] = d->vertex_start + k[j];
                else if (d->index_size == 2)
                    idx[3 * i + j] = ((const uint16_t*)d->indices)[k[j]];
                else
                    idx[3 * i + j] = ((const uint32_t*)d->indices)[k[j]];
            }
        }
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * 4, ibv.Format = DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else if (d->ibuf)
    {
        buf_state(d->ibuf, BUF_READ);
        ibv.BufferLocation = d->ibuf->gpu + d->ibuf_off;
        ibv.SizeInBytes = d->ibuf->size - d->ibuf_off;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else if (d->indices)
    {
        Alloc a = ring((size_t)n * d->index_size, 16);
        if (!a.cpu)
            return;
        memcpy(a.cpu, d->indices, (size_t)n * d->index_size);
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * d->index_size;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, 1, 0, 0, 0);
    }
    else
        ID3D12GraphicsCommandList_DrawInstanced(l, n, 1, d->vertex_start, 0);
    g_cmd_draws++;
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags || !bind_targets())
        return;
    float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
    uint32_t w, h;
    color_size(&w, &h);
    int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
    /* the viewport, intersected with each rectangle */
    D3D12_RECT stack[16], *r = stack;
    uint32_t n = 0;
    if (!whole)
    {
        int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
        int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
        if (!nrects)
            rects = whole_rect, nrects = 1;
        if (nrects > 16)
            r = (D3D12_RECT*)malloc(sizeof(D3D12_RECT) * nrects);
        for (uint32_t i = 0; i < nrects; ++i)
        {
            int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
            int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
            x1 = x1 < (int32_t)w ? x1 : (int32_t)w, y1 = y1 < (int32_t)h ? y1 : (int32_t)h;
            x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;
            if (x1 > x0 && y1 > y0)
                r[n++] = (D3D12_RECT){ x0, y0, x1, y1 };
        }
        if (!n)
            goto out;
    }
    if (flags & 1)
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, target_view(g_rt, g_rt_face, g_rt_level), c, n, whole ? NULL : r);
    GfxTex* ds = depth_attachment();
    if (ds && (flags & 6))
    {
        D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)(((flags & 2) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & 4) && ds->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
        if (f)
            ID3D12GraphicsCommandList_ClearDepthStencilView(g_list, target_view(ds, 0, 0), f, z, (UINT8)stencil, n, whole ? NULL : r);
    }
out:
    if (r != stack)
        free(r);
}

/* --- the window ------------------------------------------------------------------------------------------------ */
static void swap_release(void)
{
    for (int i = 0; i < FRAMES; ++i)
    {
        if (g_swap_buf[i])
            ID3D12Resource_Release(g_swap_buf[i]);
        g_swap_buf[i] = NULL;
    }
}

static void swap_acquire(void)
{
    for (UINT i = 0; i < FRAMES; ++i)
    {
        IDXGISwapChain3_GetBuffer(g_swap, i, &IID_ID3D12Resource, (void**)&g_swap_buf[i]);
        g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
        if (g_swap_rtv[i] < 0)
            g_swap_rtv[i] = heap_alloc(&g_rtv);
        ID3D12Device_CreateRenderTargetView(g_dev, g_swap_buf[i], NULL, heap_cpu(&g_rtv, g_swap_rtv[i]));
    }
}

/* the swap chain at the window's size in pixels (all GPU work finished first: its buffers go) */
static void swap_fit(void)
{
    int pw = 0, ph = 0;
    if (g_window)
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        pw = (int)g_want_w, ph = (int)g_want_h;
    if (pw <= 0 || ph <= 0 || ((uint32_t)pw == g_swap_w && (uint32_t)ph == g_swap_h))
        return;
    submit(1);
    swap_release();
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(g_swap, FRAMES, (UINT)pw, (UINT)ph, DXGI_FORMAT_B8G8R8A8_UNORM,
        g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
        fprintf(stderr, "[recomp] gfx: swap chain resize to %dx%d failed (%08lx)\n", pw, ph, (unsigned long)hr);
    else
        g_swap_w = (uint32_t)pw, g_swap_h = (uint32_t)ph;
    swap_acquire();
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void frame_end(void)
{
    list();
    if (gfx_profiling && g_queries && g_frames[g_frame].timed)
    {
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame + 1);
        ID3D12GraphicsCommandList_ResolveQueryData(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * g_frame, 2, g_query_rb,
            16ull * g_frame);
    }
    submit(0);
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    double now = (double)gfx_now_ns() * 1e-9;
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = now - g_fps_since;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(uint32_t w, uint32_t h)
{
    struct
    {
        float rect[4], scale;
        uint32_t n;
        float size[2];
        uint32_t text[32];
    } u;
    memset(&u, 0, sizeof u);
    for (const char* c = g_fps_text; *c && u.n < 32; ++c)
    {
        uint32_t k = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12
            : *c == 'm' ? 13 : *c == 's' ? 14 : *c == '.' ? 15 : 16;
        u.text[u.n++] = k;
    }
    u.scale = (float)(h >= 1400 ? 3 : 2);
    u.rect[0] = u.rect[1] = 4 * u.scale;
    u.rect[2] = (float)(u.n * 6 + 3) * u.scale, u.rect[3] = 11 * u.scale;
    u.size[0] = (float)w, u.size[1] = (float)h;
    Alloc a = ring(sizeof u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    memcpy(a.cpu, &u, sizeof u);
    set_pso(g_overlay_pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, ROOT_U, a.gpu);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 4, 1, 0, 0);
}

/* the scene effects are Metal's so far (gfx_metal.m) */
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
/* but for the frame-rate overlay's */
void gfx_fx_set(const char* key, float v)
{
    if (!strcmp(key, "fps"))
        g_overlay = v != 0.0f;
}
float gfx_fx_get(const char* key) { return !strcmp(key, "fps") ? (float)g_overlay : 0.0f; }
void gfx_trace_dump(const char* path) { (void)path; }

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = GetCurrentThreadId();
    int presented = 0;
    if (g_swap && bb && bb->srv >= 0 && g_present_pso)
    {
        swap_fit();
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(g_swap);
        ID3D12GraphicsCommandList* l = list();
        tex_state(bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(g_swap_buf[i], g_swap_state[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(&g_rtv, g_swap_rtv[i]);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
        D3D12_VIEWPORT v = { 0, 0, (float)g_swap_w, (float)g_swap_h, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)g_swap_w, (LONG)g_swap_h };
        ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        uint32_t bind[16] = { 0 };
        bind[0] = (uint32_t)bb->srv, bind[8] = g_present_samp;
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        set_pso(g_present_pso);
        set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
        if (g_overlay && g_overlay_pso)
            draw_overlay(g_swap_w, g_swap_h);
        barrier(g_swap_buf[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
        g_targets_bound = 0;
        presented = 1;
    }
    fps_tick();
    frame_end();
    if (presented)
    {
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        HRESULT hr = IDXGISwapChain3_Present(g_swap, g_vsync ? 1 : 0, !g_vsync && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (gfx_profiling)
            g_prof.drawable_ns += gfx_now_ns() - t0;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            static int told;
            if (!told++)
                fprintf(stderr, "[recomp] gfx: device removed (%08lx)\n", (unsigned long)ID3D12Device_GetDeviceRemovedReason(g_dev));
        }
    }
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!g_dev)
        return;
    submit(1);
}

void gfx_resize(uint32_t w, uint32_t h)
{
    g_want_w = w, g_want_h = h;
}

/* --- start-up -------------------------------------------------------------------------------------------------- */
static ID3D12RootSignature* make_root(void)
{
    D3D12_DESCRIPTOR_RANGE tex2d = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
    D3D12_DESCRIPTOR_RANGE texcube = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0 };
    D3D12_DESCRIPTOR_RANGE samps = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0, 0 };
    D3D12_ROOT_PARAMETER p[ROOT_COUNT];
    memset(p, 0, sizeof p);
    p[ROOT_U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[ROOT_U].Descriptor.ShaderRegister = 0;
    p[ROOT_U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[ROOT_BIND].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[ROOT_BIND].Constants.ShaderRegister = 1;
    p[ROOT_BIND].Constants.Num32BitValues = 16;
    p[ROOT_BIND].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        p[ROOT_STREAM0 + s].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[ROOT_STREAM0 + s].Descriptor.ShaderRegister = (UINT)s;
        p[ROOT_STREAM0 + s].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    p[ROOT_TEX2D].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEX2D].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEX2D].DescriptorTable.pDescriptorRanges = &tex2d;
    p[ROOT_TEX2D].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_TEXCUBE].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEXCUBE].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEXCUBE].DescriptorTable.pDescriptorRanges = &texcube;
    p[ROOT_TEXCUBE].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_SAMPLERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_SAMPLERS].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_SAMPLERS].DescriptorTable.pDescriptorRanges = &samps;
    p[ROOT_SAMPLERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = { ROOT_COUNT, p, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS };
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature* root = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        fprintf(stderr, "[recomp] gfx: root signature: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?");
    else if (FAILED(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                 &IID_ID3D12RootSignature, (void**)&root)))
        root = NULL;
    if (blob)
        ID3D10Blob_Release(blob);
    if (err)
        ID3D10Blob_Release(err);
    return root;
}

/* the present or overlay pipeline, onto the swap chain's format */
static ID3D12PipelineState* util_pipeline(const char* vs, const char* ps, int blend)
{
    ID3DBlob* v = compile(gfx_hlsl_util, vs, "vs_5_1");
    ID3DBlob* f = v ? compile(gfx_hlsl_util, ps, "ps_5_1") : NULL;
    ID3D12PipelineState* p = NULL;
    if (v && f)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        memset(&pd, 0, sizeof pd);
        pd.pRootSignature = g_root;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(v), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(v);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(f), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(f);
        D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
        c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
        c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
        c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (blend)
            c->BlendEnable = TRUE, c->SrcBlend = D3D12_BLEND_SRC_ALPHA, c->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
            p = NULL;
    }
    if (v)
        ID3D10Blob_Release(v);
    if (f)
        ID3D10Blob_Release(f);
    return p;
}

/* the device on the high-performance adapter, else the default one */
static int make_device(void)
{
    const char* dbg = getenv("FFXI_D3D12_DEBUG");
    if (dbg && dbg[0] == '1')
    {
        ID3D12Debug* d = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&d)))
        {
            ID3D12Debug_EnableDebugLayer(d);
            ID3D12Debug_Release(d);
            fprintf(stderr, "[recomp] gfx: D3D12 debug layer on\n");
        }
    }
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&g_factory)))
        g_factory = NULL;
    IDXGIFactory6* f6 = NULL;
    IDXGIAdapter1* adapter = NULL;
    if (g_factory && SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory6, (void**)&f6)))
    {
        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void**)&adapter);
        IDXGIFactory6_Release(f6);
    }
    HRESULT hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    if (FAILED(hr) && adapter)
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    char name[128] = "?";
    if (adapter)
    {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
        IDXGIAdapter1_Release(adapter);
    }
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: no Direct3D 12 device (%08lx)\n", (unsigned long)hr);
        g_dev = NULL;
        return 0;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    memset(&o, 0, sizeof o);
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    if (o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    {
        fprintf(stderr, "[recomp] gfx: %s has resource binding tier %d; the back end needs tier 2\n", name, (int)o.ResourceBindingTier);
        ID3D12Device_Release(g_dev);
        g_dev = NULL;
        return 0;
    }
    fprintf(stderr, "[recomp] gfx: Direct3D 12 on %s\n", name);
    return 1;
}

#if defined(FFXI_UWP)
/* UWP: no window handle; the swap chain is for composition, and the app shows it in a SwapChainPanel */
static void make_swap_chain(SDL_Window* win)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    g_tearing = 0; /* composition swap chains cannot tear */
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForComposition(g_factory, (IUnknown*)g_queue, &sd, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    uwp_attach_swapchain(sc1);
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#else
static void make_swap_chain(SDL_Window* win)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd || !g_factory)
    {
        fprintf(stderr, "[recomp] gfx: no window handle for the swap chain: %s\n", SDL_GetError());
        return;
    }
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory5, (void**)&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            g_tearing = allow != FALSE;
        IDXGIFactory5_Release(f5);
    }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(g_factory, (IUnknown*)g_queue, hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory4_MakeWindowAssociation(g_factory, hwnd, DXGI_MWA_NO_ALT_ENTER); /* full screen is the window's (SDL) */
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#endif

int gfx_init(void* window, int vsync)
{
    if (g_dev)
        return 1;
    if (!make_device())
        return 0;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_queue);
    for (int i = 0; i < FRAMES; ++i)
        ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&g_frames[i].alloc);
    ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, NULL, &IID_ID3D12GraphicsCommandList,
        (void**)&g_list);
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g_fence);
    g_fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_pipe_file_lock);
    g_root = make_root();
    if (!g_queue || !g_list || !g_fence || !g_root || !heap_init(&g_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP, 1, 2) ||
        !heap_init(&g_samp, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP, 1, 0) ||
        !heap_init(&g_rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP, 0, 0) ||
        !heap_init(&g_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP, 0, 0))
    {
        fprintf(stderr, "[recomp] gfx: Direct3D 12 set-up failed\n");
        ID3D12Device_Release(g_dev);
        g_dev = NULL;
        return 0;
    }
    /* the null views every unbound slot reads, and a sampler for the present */
    D3D12_SHADER_RESOURCE_VIEW_DESC nv;
    memset(&nv, 0, sizeof nv);
    nv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, nv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_2D));
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, nv.TextureCube.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_CUBE));
    GfxSampler linear;
    memset(&linear, 0, sizeof linear);
    linear.addr_u = linear.addr_v = linear.addr_w = 3, linear.mag = linear.min = 2;
    g_present_samp = sampler(&linear);
    g_dummy = make_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);

    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    if (gfx_profiling)
    {
        D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * FRAMES, 0 };
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(g_dev, &qh, &IID_ID3D12QueryHeap, (void**)&g_queries)))
        {
            g_query_rb = make_buffer(16 * FRAMES, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            if (g_query_rb)
                ID3D12Resource_Map(g_query_rb, 0, NULL, (void**)&g_query_cpu);
            ID3D12CommandQueue_GetTimestampFrequency(g_queue, &g_ts_freq);
        }
    }
    const char* show = getenv("FFXI_FPS");
    g_overlay = !(show && show[0] == '0');
    g_present_pso = util_pipeline("present_vs", "present_fs", 0);
    g_overlay_pso = util_pipeline("overlay_vs", "overlay_fs", 1);
    if (!g_sync_pipelines)
        prewarm_pipelines();
    g_vsync = vsync;
    if (window)
    {
        g_window = (SDL_Window*)window;
        make_swap_chain(g_window);
    }
    return 1;
}
