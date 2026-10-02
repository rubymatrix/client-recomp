/* XAudio2 2.4 (March 2009 DirectX SDK) for FINAL FANTASY XIV 1.23b's sound driver
 * (SQEX::CDev::Engine::Sd::Driver::XA2).
 *
 * The game makes the engine with CoGetClassObject(CLSID_XAudio2 2.4) and its class factory, and
 * its effects (reverb, volume meter) with CoCreateInstance; it asserts and stops when any of
 * them fail. This is the whole interface it reaches: the engine, mastering, submix and source
 * voices, and effect objects.
 *
 * An audio thread mixes as XAudio2 does: each started source voice (PCM 8/16/24/32, float, MS
 * ADPCM) is resampled by its rate times its frequency ratio, scaled by its volumes and sent
 * through its output matrices to its submixes and the mastering voice; submixes run in stage
 * order; the master is folded to stereo for an SDL audio stream, whose queue sets the pace. The
 * game's callbacks run as XAudio2 would run them (OnVoiceProcessingPassStart, OnBufferStart/End,
 * OnLoopEnd, OnStreamEnd, the engine's pass callbacks). Effects are not run: a reverb passes on
 * only its dry part (1 - WetDryMix), a volume meter reads silence.
 *
 * Vtables in XAudio2 2.4 order. Voices are not COM objects: their vtables start at
 * GetVoiceDetails. Every method is stdcall with `this` first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"

#include <SDL3/SDL.h>

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u

extern int (*ole_class_hook)(uint32_t clsid, uint32_t riid, uint32_t* obj);

static const uint8_t CLSID_XAudio2_4[16] = { 0x78, 0x9e, 0x21, 0x03, 0xc3, 0x5b, 0xd1, 0x44,
                                              0xb9, 0x2e, 0xf6, 0x3d, 0x89, 0xcc, 0x65, 0x26 };
/* the game's reverb (and its debug variant) and volume meter classes: any other CLSID under
 * CoCreateInstance that the sound driver asks for gets an effect object too (see is_effect) */
static const uint8_t CLSID_Reverb_A[16] = { 0x8b, 0x77, 0xb7, 0x8b, 0x5b, 0x64, 0x75, 0x44,
                                             0x9a, 0x73, 0x1d, 0xe3, 0x17, 0x0b, 0xd3, 0xaf };
static const uint8_t CLSID_VolumeMeter[16] = { 0x95, 0x8b, 0x33, 0xc7, 0xb8, 0x52, 0x42, 0x45,
                                                0xaa, 0x79, 0x42, 0xeb, 0x01, 0x6c, 0x8c, 0x1c };
static const uint8_t CLSID_Reverb_B[16] = { 0xa2, 0x38, 0x77, 0xda, 0x2c, 0xcd, 0x67, 0x43,
                                             0x9a, 0xac, 0xd7, 0xea, 0xd7, 0xc6, 0x4f, 0x98 };

/* --- objects ----------------------------------------------------------------------------------- */

enum { V_FREE, V_ENGINE, V_MASTER, V_SUBMIX, V_SOURCE, V_EFFECT, V_FACTORY };
/* source sample formats */
enum { F_NONE, F_PCM8, F_PCM16, F_PCM24, F_PCM32, F_FLOAT, F_ADPCM };
/* effect kinds */
enum { FX_OTHER, FX_REVERB, FX_METER };

typedef struct Buf
{
    uint32_t flags, bytes, data, play_begin, play_length, loop_begin, loop_length, loop_count, context;
} Buf;

/* an output of a voice: the voice it feeds and its matrix (dc rows of sc levels), when one is set */
typedef struct Send
{
    int dst;
    uint32_t sc, dc;
    int set;
    float m[64];
} Send;

#define MAX_QUEUE 64
#define MAX_SENDS 4
#define MAX_FX 4
#define MAX_FRAMES 4800 /* the most output frames one pass makes: 100 ms at 48 kHz */

typedef struct Voice
{
    int kind;
    uint32_t guest;
    int32_t refs;           /* engine, effects, factory */
    uint32_t channels, rate, flags, stage;
    uint32_t callback;      /* source: IXAudio2VoiceCallback* */
    uint16_t format_tag, block_align, samples_per_block;
    int fmt;
    float volume, ratio;
    int started;
    Buf queue[MAX_QUEUE];
    uint32_t nqueue;
    double pos;             /* the current buffer's frame being played (from its start) */
    int buffer_started;
    uint32_t loops_done;
    double played;          /* frames */
    float chvol[8];
    Send sends[MAX_SENDS];
    int nsends;
    /* the effect chain: reverbs pass on only their dry part */
    int nfx, fx_reverb[MAX_FX], fx_on[MAX_FX];
    float fx_wet[MAX_FX];   /* WetDryMix, 0..100 */
    int fxkind;             /* an effect object: what it is */
    float* mix;             /* submix, master: what the pass sends them */
    /* MS ADPCM: the coefficients and the last block decoded */
    int16_t coef[64];
    uint32_t ncoef, adpcm_at;
    int16_t* adpcm;
} Voice;

#define MAX_VOICES 512
static Voice g_v[MAX_VOICES];
static uint32_t g_vtbl[V_FACTORY + 1];
static uint32_t g_engine_callbacks[16];
static uint32_t g_nengine_cb;
static int g_engine_started;
static volatile int g_thread_up;
static int g_master;        /* the mastering voice's index */
static int g_log;           /* XIV_AUDIOLOG */

static Voice* voice(uint32_t p)
{
    if (!p)
        return NULL;
    /* p may be anything (the sends are probed in two layouts): only a voice's own address is
     * read, and only once it is known to be one */
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
        if (g_v[i].guest == p && g_v[i].kind != V_FREE)
            return rd32(p + 4) == i ? &g_v[i] : NULL;
    return NULL;
}

static uint32_t voice_new(int kind)
{
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
        if (g_v[i].kind == V_FREE)
        {
            Voice* v = &g_v[i];
            float* mix = v->mix;
            int16_t* adpcm = v->adpcm;
            memset(v, 0, sizeof *v);
            free(mix);
            free(adpcm);
            v->kind = kind;
            v->refs = 1;
            v->volume = v->ratio = 1.0f;
            for (int c = 0; c < 8; ++c)
                v->chvol[c] = 1.0f;
            v->nsends = -1; /* to the mastering voice */
            v->guest = gheap_alloc(8, 1);
            wr32(v->guest, g_vtbl[kind]);
            wr32(v->guest + 4, i);
            return v->guest;
        }
    rt_log("[xaudio2] out of voices\n");
    return 0;
}

static void voice_free(Voice* v)
{
    wr32(v->guest, 0);
    gheap_free(v->guest);
    if ((int)(v - g_v) == g_master)
        g_master = 0;
    v->kind = V_FREE;
}

/* frames in a whole buffer */
static uint32_t buf_total(const Voice* v, const Buf* b)
{
    if (v->format_tag == 2 && v->block_align) /* MS ADPCM */
        return (b->bytes / v->block_align) * v->samples_per_block;
    return v->block_align ? b->bytes / v->block_align : 0;
}

/* the frame a buffer's playable region ends at */
static uint32_t buf_end(const Voice* v, const Buf* b)
{
    uint32_t total = buf_total(v, b);
    uint32_t end = b->play_length ? b->play_begin + b->play_length : total;
    return end < total ? end : total;
}

/* --- decoding ------------------------------------------------------------------------------------- */

static const int16_t kAdpcmCoef[14] = { 256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232 };
static const int kAdpcmAdapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

static int16_t clamp16(int x) { return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); }

/* one MS ADPCM block (block_align bytes at p) to samples_per_block interleaved frames */
static void adpcm_block(const Voice* v, const uint8_t* p, int16_t* out)
{
    int ch = (int)v->channels, spb = v->samples_per_block;
    int pred[2], delta[2], s1[2], s2[2];
    for (int c = 0; c < ch; ++c)
    {
        int k = p[c] < v->ncoef ? p[c] : 0;
        pred[c] = k;
        delta[c] = (int16_t)(p[ch + 2 * c] | p[ch + 2 * c + 1] << 8);
        s1[c] = (int16_t)(p[3 * ch + 2 * c] | p[3 * ch + 2 * c + 1] << 8);
        s2[c] = (int16_t)(p[5 * ch + 2 * c] | p[5 * ch + 2 * c + 1] << 8);
        out[c] = (int16_t)s2[c];
        out[ch + c] = (int16_t)s1[c];
    }
    const uint8_t* d = p + 7 * ch;
    int n = (spb - 2) * ch;
    if (7 * ch + (n + 1) / 2 > v->block_align)
        n = (v->block_align - 7 * ch) * 2;
    for (int k = 0; k < n; ++k)
    {
        int c = k % ch;
        int nib = k & 1 ? d[k >> 1] & 15 : d[k >> 1] >> 4;
        int sn = nib >= 8 ? nib - 16 : nib;
        int pr = (s1[c] * v->coef[2 * pred[c]] + s2[c] * v->coef[2 * pred[c] + 1]) >> 8;
        pr = clamp16(pr + sn * delta[c]);
        s2[c] = s1[c];
        s1[c] = pr;
        delta[c] = kAdpcmAdapt[nib] * delta[c] >> 8;
        if (delta[c] < 16)
            delta[c] = 16;
        out[(2 + k / ch) * ch + c] = (int16_t)pr;
    }
}

/* frame fi of buffer b, one float per channel */
static void fetch(Voice* v, const Buf* b, uint32_t fi, float* out)
{
    uint32_t ch = v->channels;
    switch (v->fmt)
    {
    case F_PCM16:
    {
        const uint8_t* p = GUEST_PTR(b->data + fi * v->block_align);
        for (uint32_t c = 0; c < ch; ++c)
            out[c] = (int16_t)(p[2 * c] | p[2 * c + 1] << 8) * (1.0f / 32768.0f);
        return;
    }
    case F_PCM8:
    {
        const uint8_t* p = GUEST_PTR(b->data + fi * v->block_align);
        for (uint32_t c = 0; c < ch; ++c)
            out[c] = ((int)p[c] - 128) * (1.0f / 128.0f);
        return;
    }
    case F_PCM24:
    {
        const uint8_t* p = GUEST_PTR(b->data + fi * v->block_align);
        for (uint32_t c = 0; c < ch; ++c)
            out[c] = (int32_t)((uint32_t)p[3 * c] << 8 | (uint32_t)p[3 * c + 1] << 16 | (uint32_t)p[3 * c + 2] << 24) * (1.0f / 2147483648.0f);
        return;
    }
    case F_PCM32:
    case F_FLOAT:
    {
        const uint8_t* p = GUEST_PTR(b->data + fi * v->block_align);
        for (uint32_t c = 0; c < ch; ++c)
        {
            uint32_t u;
            memcpy(&u, p + 4 * c, 4);
            if (v->fmt == F_FLOAT)
                memcpy(&out[c], &u, 4);
            else
                out[c] = (int32_t)u * (1.0f / 2147483648.0f);
        }
        return;
    }
    case F_ADPCM:
    {
        uint32_t spb = v->samples_per_block, blk = fi / spb;
        uint32_t at = b->data + blk * v->block_align;
        if (!v->adpcm)
            v->adpcm = malloc((size_t)spb * ch * sizeof(int16_t));
        if (v->adpcm_at != at)
        {
            adpcm_block(v, GUEST_PTR(at), v->adpcm);
            v->adpcm_at = at;
        }
        const int16_t* s = v->adpcm + (fi % spb) * ch;
        for (uint32_t c = 0; c < ch; ++c)
            out[c] = s[c] * (1.0f / 32768.0f);
        return;
    }
    default:
        for (uint32_t c = 0; c < ch; ++c)
            out[c] = 0;
    }
}

/* --- the game's callbacks ----------------------------------------------------------------------- */

/* IXAudio2VoiceCallback (2.4): OnVoiceProcessingPassStart(bytes), OnVoiceProcessingPassEnd,
 * OnStreamEnd, OnBufferStart(ctx), OnBufferEnd(ctx), OnLoopEnd(ctx), OnVoiceError(ctx, hr) */
static void vcb(uint32_t cb, unsigned slot, unsigned nargs, uint32_t a0, uint32_t a1)
{
    if (!cb)
        return;
    uint32_t args[3] = { cb, a0, a1 };
    guest_call(rd32(rd32(cb) + 4 * slot), nargs + 1, args);
}

/* IXAudio2EngineCallback: OnProcessingPassStart, OnProcessingPassEnd, OnCriticalError(hr) */
static void ecb(unsigned slot)
{
    for (uint32_t i = 0; i < g_nengine_cb; ++i)
    {
        uint32_t cb = g_engine_callbacks[i];
        uint32_t args[1] = { cb };
        guest_call(rd32(rd32(cb) + 4 * slot), 1, args);
    }
}

/* --- mixing ------------------------------------------------------------------------------------- */

/* the level from source channel s to destination channel d when no matrix is set */
static float default_level(uint32_t sc, uint32_t dc, uint32_t s, uint32_t d)
{
    if (dc == 1)
        return 1.0f / (float)sc;
    if (sc == 1)
        return d < 2 ? 1.0f : 0.0f;
    return s == d ? 1.0f : 0.0f;
}

/* what reaches a voice's outputs of its input: its reverbs keep only their dry part */
static float voice_gain(const Voice* v)
{
    float g = v->volume;
    for (int i = 0; i < v->nfx; ++i)
        if (v->fx_reverb[i] && v->fx_on[i])
            g *= 1.0f - v->fx_wet[i] / 100.0f;
    return g;
}

/* frame `in` (sc channels, gains applied) to v's outputs at output frame i */
static void send_frame(Voice* v, const float* in, uint32_t i)
{
    int n = v->nsends < 0 ? 1 : v->nsends;
    for (int k = 0; k < n; ++k)
    {
        int di = v->nsends < 0 ? g_master : v->sends[k].dst;
        Voice* d = di > 0 && di < MAX_VOICES ? &g_v[di] : NULL;
        if (!d || (d->kind != V_SUBMIX && d->kind != V_MASTER) || !d->mix || d == v)
            continue;
        uint32_t sc = v->channels, dc = d->channels;
        const Send* s = v->nsends < 0 ? NULL : &v->sends[k];
        int set = s && s->set && s->sc == sc && s->dc == dc;
        float* o = d->mix + (size_t)i * dc;
        for (uint32_t c = 0; c < dc; ++c)
        {
            float acc = 0;
            for (uint32_t x = 0; x < sc; ++x)
                acc += in[x] * (set ? s->m[c * sc + x] : default_level(sc, dc, x, c));
            o[c] += acc;
        }
    }
}

/* source voice v: n output frames at out_rate into its outputs, its buffers and callbacks as
 * XAudio2 runs them */
static void mix_source(Voice* v, uint32_t n, uint32_t out_rate)
{
    uint32_t guest = v->guest;
    double step = v->rate * (double)v->ratio / out_rate;
    float a[8], b2[8], f[8];
    uint32_t i = 0;
    int guard = 0;
    while (i < n && v->nqueue)
    {
        Buf* b = &v->queue[0];
        if (!v->buffer_started)
        {
            v->buffer_started = 1;
            v->pos = b->play_begin;
            v->loops_done = 0;
            vcb(v->callback, 3, 1, b->context, 0);
            if (voice(guest) != v || !v->nqueue)
                return;
            b = &v->queue[0];
        }
        uint32_t end = buf_end(v, b);
        int looping = b->loop_count && v->loops_done < b->loop_count;
        uint32_t lend = end;
        if (looping)
        {
            lend = b->loop_length ? b->loop_begin + b->loop_length : end;
            if (lend > end)
                lend = end;
            if (lend <= b->loop_begin)
                looping = 0, lend = end;
        }
        if (v->pos >= lend)
        {
            if (++guard > 64)
                break; /* buffers with nothing in them: on to the next pass */
            if (looping)
            {
                v->pos = b->loop_begin + (v->pos - lend);
                if (v->pos >= lend)
                    v->pos = b->loop_begin;
                if (b->loop_count != 255) /* XAUDIO2_LOOP_INFINITE */
                    v->loops_done++;
                vcb(v->callback, 5, 1, b->context, 0);
                if (voice(guest) != v)
                    return;
                continue;
            }
            Buf done = *b;
            memmove(&v->queue[0], &v->queue[1], (v->nqueue - 1) * sizeof(Buf));
            v->nqueue--;
            v->pos = 0;
            v->buffer_started = 0;
            v->loops_done = 0;
            vcb(v->callback, 4, 1, done.context, 0);
            if (voice(guest) != v)
                return;
            if (done.flags & 0x40) /* XAUDIO2_END_OF_STREAM */
            {
                vcb(v->callback, 2, 0, 0, 0);
                if (voice(guest) != v)
                    return;
            }
            continue;
        }
        guard = 0;
        uint32_t ch = v->channels > 8 ? 8 : v->channels;
        /* as many frames as this buffer (or its loop) has before its end */
        float gain = voice_gain(v);
        while (i < n && v->pos < lend)
        {
            uint32_t fi = (uint32_t)v->pos;
            float t = (float)(v->pos - fi);
            fetch(v, b, fi, a);
            if (t > 0 && fi + 1 < lend)
            {
                fetch(v, b, fi + 1, b2);
                for (uint32_t c = 0; c < ch; ++c)
                    a[c] += (b2[c] - a[c]) * t;
            }
            for (uint32_t c = 0; c < ch; ++c)
                f[c] = a[c] * gain * v->chvol[c];
            send_frame(v, f, i);
            v->pos += step;
            v->played += step;
            i++;
        }
    }
}

/* a submix or the master: its volume, then (a submix) on to its outputs */
static void mix_bus(Voice* v, uint32_t n)
{
    float gain = voice_gain(v), f[8];
    uint32_t ch = v->channels > 8 ? 8 : v->channels;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* in = v->mix + (size_t)i * v->channels;
        for (uint32_t c = 0; c < ch; ++c)
            f[c] = in[c] * gain * v->chvol[c];
        if (gain != 0)
            send_frame(v, f, i);
    }
}

static float g_out[MAX_FRAMES * 2];

/* one processing pass of n frames at out_rate: the speakers' stereo into g_out */
static void pass(uint32_t n, uint32_t out_rate)
{
    if (g_engine_started)
        ecb(0);
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
    {
        Voice* v = &g_v[i];
        if ((v->kind == V_SUBMIX || v->kind == V_MASTER) && v->channels)
        {
            if (!v->mix)
                v->mix = malloc((size_t)MAX_FRAMES * v->channels * sizeof(float));
            memset(v->mix, 0, (size_t)n * v->channels * sizeof(float));
        }
    }
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
    {
        Voice* v = &g_v[i];
        if (v->kind != V_SOURCE || !v->started)
            continue;
        uint32_t guest = v->guest;
        if (v->callback)
        {
            uint32_t need = v->nqueue ? 0 : (uint32_t)(n * v->rate * (double)v->ratio / out_rate) * v->block_align;
            vcb(v->callback, 0, 1, need, 0);
            if (voice(guest) != v)
                continue; /* the callback destroyed it */
        }
        mix_source(v, n, out_rate);
        if (voice(guest) == v && v->callback)
            vcb(v->callback, 1, 0, 0, 0);
    }
    /* submixes in processing stage order, each before what it feeds */
    for (int stage = -1;;)
    {
        uint32_t next = 0xFFFFFFFFu;
        for (uint32_t i = 1; i < MAX_VOICES; ++i)
            if (g_v[i].kind == V_SUBMIX && (int64_t)g_v[i].stage > stage && g_v[i].stage < next)
                next = g_v[i].stage;
        if (next == 0xFFFFFFFFu)
            break;
        for (uint32_t i = 1; i < MAX_VOICES; ++i)
            if (g_v[i].kind == V_SUBMIX && g_v[i].stage == next && g_v[i].mix)
                mix_bus(&g_v[i], n);
        stage = (int)next;
    }
    Voice* m = g_master ? &g_v[g_master] : NULL;
    if (m && m->kind == V_MASTER && m->mix)
    {
        float gain = m->volume;
        uint32_t ch = m->channels;
        for (uint32_t i = 0; i < n; ++i)
        {
            const float* in = m->mix + (size_t)i * ch;
            float l, r;
            if (ch == 1)
                l = r = in[0] * m->chvol[0];
            else
            {
                l = in[0] * m->chvol[0], r = in[1] * m->chvol[1];
                if (ch >= 3) /* centre (and the surrounds) folded into the front pair; LFE dropped */
                    l += 0.7071f * in[2] * m->chvol[2], r += 0.7071f * in[2] * m->chvol[2];
                if (ch >= 6)
                    l += 0.7071f * in[4] * m->chvol[4], r += 0.7071f * in[5] * m->chvol[5];
            }
            l *= gain, r *= gain;
            g_out[2 * i] = l < -1 ? -1 : l > 1 ? 1 : l;
            g_out[2 * i + 1] = r < -1 ? -1 : r > 1 ? 1 : r;
        }
    }
    else
        memset(g_out, 0, (size_t)n * 2 * sizeof(float));
    if (g_engine_started)
        ecb(1);
}

/* The speakers set the pace: each pass tops the device's queue up to ~40 ms. With no device the
 * clock runs on the wall clock instead. */
static void audio_thread(void* arg)
{
    (void)arg;
    SDL_AudioStream* stream = NULL;
    uint32_t rate = 48000;
    uint64_t last = rt_monotonic_ns();
    int tried = 0;
    FILE* dump = getenv("XIV_AUDIODUMP") ? fopen(getenv("XIV_AUDIODUMP"), "wb") : NULL;
    for (;;)
    {
        plat_sleep_ms(5);
        if (!stream && !tried && g_master)
        {
            tried = 1;
            rate = g_v[g_master].rate ? g_v[g_master].rate : 48000;
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                rt_log("[xaudio2] SDL audio: %s\n", SDL_GetError());
            else
            {
                SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, (int)rate };
                stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
                if (stream)
                    SDL_ResumeAudioStreamDevice(stream);
                else
                    rt_log("[xaudio2] no audio device: %s\n", SDL_GetError());
            }
            rt_log("[xaudio2] output %s at %u Hz\n", stream ? "on" : "off", rate);
        }
        uint32_t n;
        uint64_t now = rt_monotonic_ns();
        if (stream)
        {
            uint32_t queued = (uint32_t)SDL_GetAudioStreamQueued(stream) / 8;
            uint32_t target = rate / 25;
            if (queued >= target)
                continue;
            n = target - queued;
        }
        else
        {
            double ms = (now - last) / 1e6;
            if (ms < 10)
                continue;
            n = (uint32_t)(rate * (ms > 100 ? 100 : ms) / 1000.0);
        }
        last = now;
        if (n > MAX_FRAMES)
            n = MAX_FRAMES;
        if (!n)
            continue;
        gt_lock();
        pass(n, rate);
        gt_unlock();
        if (stream)
            SDL_PutAudioStreamData(stream, g_out, (int)(n * 2 * sizeof(float)));
        if (dump) /* XIV_AUDIODUMP=<file>: the output as raw stereo float32 */
            fwrite(g_out, 2 * sizeof(float), n, dump);
    }
}

static void start_thread(void)
{
    if (!g_thread_up)
    {
        g_thread_up = 1;
        g_log = getenv("XIV_AUDIOLOG") != NULL;
        plat_thread_start(audio_thread, NULL);
    }
}

/* SetOutputVoices / the sends at creation: XAUDIO2_VOICE_SENDS { count, IXAudio2Voice** } in 2.4
 * (later versions point at { flags, voice } descriptors instead: both are read) */
static void set_sends(Voice* v, uint32_t p)
{
    if (!p)
    {
        v->nsends = -1;
        return;
    }
    uint32_t count = rd32(p), arr = rd32(p + 4);
    v->nsends = 0;
    for (uint32_t i = 0; i < count && v->nsends < MAX_SENDS; ++i)
    {
        Voice* d = voice(rd32(arr + 4 * i));
        if (!d)
            d = voice(rd32(arr + 8 * i + 4));
        if (!d)
            continue;
        Send* s = &v->sends[v->nsends++];
        memset(s, 0, sizeof *s);
        s->dst = (int)(d - g_v);
    }
    if (g_log)
        rt_log("[xaudio2] voice %d sends %d (first -> %d)\n", (int)(v - g_v), v->nsends, v->nsends > 0 ? v->sends[0].dst : -1);
}

/* SetEffectChain / the chain at creation: XAUDIO2_EFFECT_CHAIN { count, { IUnknown*, initial
 * state, output channels }* } */
static void set_effects(Voice* v, uint32_t p)
{
    v->nfx = 0;
    if (!p)
        return;
    uint32_t count = rd32(p), arr = rd32(p + 4);
    for (uint32_t i = 0; i < count && v->nfx < MAX_FX; ++i)
    {
        Voice* e = voice(rd32(arr + 12 * i));
        int k = v->nfx++;
        v->fx_reverb[k] = e && e->fxkind == FX_REVERB;
        v->fx_on[k] = rd32(arr + 12 * i + 4) != 0;
        v->fx_wet[k] = 100.0f;
    }
    if (g_log)
        rt_log("[xaudio2] voice %d effects %d\n", (int)(v - g_v), v->nfx);
}

/* --- IUnknown pieces: the engine, the factory, effects -------------------------------------------- */

static void Com_QueryInterface(Guest* g)
{
    /* every interface of these objects is the object itself */
    Voice* v = voice(ARG(0));
    if (!v || !ARG(2))
        RET(E_NOINTERFACE, 3);
    v->refs++;
    wr32(ARG(2), ARG(0));
    RET(S_OK, 3);
}
static void Com_AddRef(Guest* g) { Voice* v = voice(ARG(0)); RET(v ? (uint32_t)++v->refs : 0, 1); }
static void Com_Release(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (!v)
        RET(0, 1);
    if (--v->refs > 0)
        RET((uint32_t)v->refs, 1);
    if (v->kind != V_ENGINE) /* the engine lives on: its voices are still in the game's hands */
        voice_free(v);
    RET(0, 1);
}

/* IClassFactory::CreateInstance(outer, riid, out) / LockServer(lock) */
static void CF_CreateInstance(Guest* g)
{
    uint32_t e = voice_new(V_ENGINE);
    wr32(ARG(3), e);
    RET(e ? S_OK : E_NOINTERFACE, 4);
}
static void CF_LockServer(Guest* g) { RET(S_OK, 2); }

/* --- IXAudio2 ------------------------------------------------------------------------------------- */

static void XA_GetDeviceCount(Guest* g) { wr32(ARG(1), 1); RET(S_OK, 2); }

/* GetDeviceDetails(index, XAUDIO2_DEVICE_DETAILS*): DeviceID, DisplayName (WCHAR[256] each),
 * Role, WAVEFORMATEXTENSIBLE OutputFormat */
static void XA_GetDeviceDetails(Guest* g)
{
    uint32_t p = ARG(2);
    memset(GUEST_PTR(p), 0, 1068);
    static const char id[] = "Default", name[] = "Speakers";
    for (uint32_t i = 0; id[i]; ++i)
        wr16(p + 2 * i, (uint8_t)id[i]);
    for (uint32_t i = 0; name[i]; ++i)
        wr16(p + 512 + 2 * i, (uint8_t)name[i]);
    wr32(p + 1024, 0xF);                 /* GlobalDefaultDevice */
    uint32_t f = p + 1028;
    wr16(f, 0xFFFE), wr16(f + 2, 2), wr32(f + 4, 48000), wr32(f + 8, 48000 * 8), wr16(f + 12, 8), wr16(f + 14, 32);
    wr16(f + 16, 22), wr16(f + 18, 32), wr32(f + 20, 3); /* stereo */
    static const uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    memcpy(GUEST_PTR(f + 24), KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16);
    RET(S_OK, 3);
}
static void XA_Initialize(Guest* g) { start_thread(); RET(S_OK, 3); }
static void XA_RegisterForCallbacks(Guest* g)
{
    if (g_nengine_cb < 16)
        g_engine_callbacks[g_nengine_cb++] = ARG(1);
    RET(S_OK, 2);
}
static void XA_UnregisterForCallbacks(Guest* g)
{
    for (uint32_t i = 0; i < g_nengine_cb; ++i)
        if (g_engine_callbacks[i] == ARG(1))
            g_engine_callbacks[i] = g_engine_callbacks[--g_nengine_cb];
    RET(0, 2);
}

/* CreateSourceVoice(out, WAVEFORMATEX*, flags, max ratio, callback, sends, effects) */
static void XA_CreateSourceVoice(Guest* g)
{
    uint32_t p = voice_new(V_SOURCE);
    Voice* v = voice(p);
    if (!v)
        RET(E_INVALIDARG, 8);
    uint32_t wf = ARG(2);
    v->format_tag = rd16(wf), v->channels = rd16(wf + 2), v->rate = rd32(wf + 4), v->block_align = rd16(wf + 12);
    uint32_t tag = v->format_tag, bits = rd16(wf + 14);
    if (tag == 0xFFFE && rd16(wf + 16) >= 22) /* WAVE_FORMAT_EXTENSIBLE: its subformat's tag */
        tag = rd32(wf + 24);
    if (tag == 1)
        v->fmt = bits == 8 ? F_PCM8 : bits == 16 ? F_PCM16 : bits == 24 ? F_PCM24 : bits == 32 ? F_PCM32 : F_NONE;
    else if (tag == 3 && bits == 32)
        v->fmt = F_FLOAT;
    else if (tag == 2 && v->channels <= 2) /* ADPCMWAVEFORMAT: samples per block, its coefficients */
    {
        v->fmt = F_ADPCM;
        v->samples_per_block = rd16(wf + 18);
        v->ncoef = rd16(wf + 20);
        if (v->ncoef > 32)
            v->ncoef = 32;
        for (uint32_t i = 0; i < 2 * v->ncoef; ++i)
            v->coef[i] = (int16_t)rd16(wf + 22 + 2 * i);
        if (!v->ncoef)
            memcpy(v->coef, kAdpcmCoef, sizeof kAdpcmCoef), v->ncoef = 7;
        if (v->samples_per_block < 2)
            v->fmt = F_NONE;
    }
    if (v->fmt == F_NONE || v->channels == 0 || v->channels > 8)
    {
        rt_log("[xaudio2] source format %04x, %u bits, %u channels: silent\n", tag, bits, v->channels);
        v->fmt = F_NONE;
    }
    v->flags = ARG(3);
    v->callback = ARG(5);
    if (!v->rate)
        v->rate = 48000;
    set_sends(v, ARG(6));
    set_effects(v, ARG(7));
    if (g_log)
        rt_log("[xaudio2] source %d: format %04x %u Hz %u ch %u bits\n", (int)(v - g_v), tag, v->rate, v->channels, bits);
    wr32(ARG(1), p);
    RET(S_OK, 8);
}
/* CreateSubmixVoice(out, channels, rate, flags, stage, sends, effects) */
static void XA_CreateSubmixVoice(Guest* g)
{
    uint32_t p = voice_new(V_SUBMIX);
    Voice* v = voice(p);
    if (v)
    {
        v->channels = ARG(2) ? ARG(2) : 2, v->rate = ARG(3), v->flags = ARG(4), v->stage = ARG(5);
        if (v->channels > 8)
            v->channels = 8;
        set_sends(v, ARG(6));
        set_effects(v, ARG(7));
        if (g_log)
            rt_log("[xaudio2] submix %d: %u ch, stage %u\n", (int)(v - g_v), v->channels, v->stage);
    }
    wr32(ARG(1), p);
    RET(p ? S_OK : E_INVALIDARG, 8);
}
/* CreateMasteringVoice(out, channels, rate, flags, device, effects) */
static void XA_CreateMasteringVoice(Guest* g)
{
    uint32_t p = voice_new(V_MASTER);
    Voice* v = voice(p);
    if (v)
    {
        v->channels = ARG(2) ? ARG(2) : 2, v->rate = ARG(3) ? ARG(3) : 48000;
        if (v->channels > 8)
            v->channels = 8;
        set_effects(v, ARG(6));
        g_master = (int)(v - g_v);
    }
    wr32(ARG(1), p);
    RET(p ? S_OK : E_INVALIDARG, 7);
}
static void XA_StartEngine(Guest* g) { g_engine_started = 1; start_thread(); RET(S_OK, 1); }
static void XA_StopEngine(Guest* g) { g_engine_started = 0; RET(0, 1); }
static void XA_CommitChanges(Guest* g) { RET(S_OK, 2); }
static void XA_GetPerformanceData(Guest* g) { memset(GUEST_PTR(ARG(1)), 0, 64); RET(0, 2); }
static void XA_SetDebugConfiguration(Guest* g) { RET(0, 3); }

/* --- IXAudio2Voice (2.4) ------------------------------------------------------------------------- */

/* GetVoiceDetails(XAUDIO2_VOICE_DETAILS*): CreationFlags, InputChannels, InputSampleRate */
static void V_GetVoiceDetails(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        wr32(ARG(1), v->flags), wr32(ARG(1) + 4, v->channels), wr32(ARG(1) + 8, v->rate);
    RET(0, 2);
}
static void V_SetOutputVoices(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v && v->kind != V_MASTER)
        set_sends(v, ARG(1));
    RET(S_OK, 2);
}
static void V_SetEffectChain(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        set_effects(v, ARG(1));
    RET(S_OK, 2);
}
static void V_EnableEffect(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v && ARG(1) < (uint32_t)v->nfx)
        v->fx_on[ARG(1)] = 1;
    RET(S_OK, 3);
}
static void V_DisableEffect(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v && ARG(1) < (uint32_t)v->nfx)
        v->fx_on[ARG(1)] = 0;
    RET(S_OK, 3);
}
static void V_GetEffectState(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (ARG(2))
        wr32(ARG(2), v && ARG(1) < (uint32_t)v->nfx ? (uint32_t)v->fx_on[ARG(1)] : 1);
    RET(0, 3);
}
/* SetEffectParameters(index, params, size, operation set): a reverb's WetDryMix (its first float) */
static void V_SetEffectParameters(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v && ARG(1) < (uint32_t)v->nfx && v->fx_reverb[ARG(1)] && ARG(2) && ARG(3) >= 4)
    {
        float w;
        memcpy(&w, GUEST_PTR(ARG(2)), 4);
        if (w >= 0 && w <= 100)
            v->fx_wet[ARG(1)] = w;
    }
    RET(S_OK, 5);
}
static void V_GetEffectParameters(Guest* g) { if (ARG(2) && ARG(3)) memset(GUEST_PTR(ARG(2)), 0, ARG(3)); RET(S_OK, 4); }
static void V_SetFilterParameters(Guest* g) { RET(S_OK, 3); }
static void V_GetFilterParameters(Guest* g)
{
    /* XAUDIO2_FILTER_PARAMETERS { Type, Frequency, OneOverQ }: the default, a wide-open low pass */
    if (ARG(1))
    {
        float f = 1.0f, q = 1.0f;
        wr32(ARG(1), 0);
        memcpy(GUEST_PTR(ARG(1) + 4), &f, 4);
        memcpy(GUEST_PTR(ARG(1) + 8), &q, 4);
    }
    RET(0, 2);
}
static void V_SetOutputFilterParameters(Guest* g) { RET(S_OK, 4); }
static void V_GetOutputFilterParameters(Guest* g) { V_GetFilterParameters(g); }
static void V_SetVolume(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        memcpy(&v->volume, GUEST_PTR(g->esp + 8), 4);
    RET(S_OK, 3);
}
static void V_GetVolume(Guest* g)
{
    Voice* v = voice(ARG(0));
    float f = v ? v->volume : 1.0f;
    memcpy(GUEST_PTR(ARG(1)), &f, 4);
    RET(0, 2);
}
static void V_SetChannelVolumes(Guest* g)
{
    Voice* v = voice(ARG(0));
    for (uint32_t c = 0; v && c < ARG(1) && c < 8; ++c)
        memcpy(&v->chvol[c], GUEST_PTR(ARG(2) + 4 * c), 4);
    RET(S_OK, 4);
}
static void V_GetChannelVolumes(Guest* g)
{
    Voice* v = voice(ARG(0));
    for (uint32_t c = 0; c < ARG(1); ++c)
    {
        float f = v && c < 8 ? v->chvol[c] : 1.0f;
        memcpy(GUEST_PTR(ARG(2) + 4 * c), &f, 4);
    }
    RET(0, 3);
}
/* the send of v to the voice at guest address dest (NULL: its only one) */
static Send* find_send(Voice* v, uint32_t dest)
{
    if (!v)
        return NULL;
    if (v->nsends < 0) /* to the master by default: made explicit to hold a matrix */
    {
        if (!g_master)
            return NULL;
        v->nsends = 1;
        memset(&v->sends[0], 0, sizeof v->sends[0]);
        v->sends[0].dst = g_master;
    }
    Voice* d = voice(dest);
    for (int i = 0; i < v->nsends; ++i)
        if (!dest || (d && v->sends[i].dst == (int)(d - g_v)))
            return &v->sends[i];
    return NULL;
}
/* SetOutputMatrix(destination, source channels, destination channels, levels, operation set):
 * levels[source channels * d + s] */
static void V_SetOutputMatrix(Guest* g)
{
    Send* s = find_send(voice(ARG(0)), ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3);
    if (s && ARG(4) && sc * dc <= 64)
    {
        memcpy(s->m, GUEST_PTR(ARG(4)), 4 * sc * dc);
        s->sc = sc, s->dc = dc, s->set = 1;
    }
    RET(S_OK, 6);
}
/* GetOutputMatrix(destination, source channels, destination channels, float*) */
static void V_GetOutputMatrix(Guest* g)
{
    Send* sd = find_send(voice(ARG(0)), ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3), p = ARG(4);
    for (uint32_t d = 0; d < dc; ++d)
        for (uint32_t s = 0; s < sc; ++s)
        {
            float f = sd && sd->set && sd->sc == sc && sd->dc == dc ? sd->m[d * sc + s] : default_level(sc, dc, s, d);
            memcpy(GUEST_PTR(p + 4 * (d * sc + s)), &f, 4);
        }
    RET(0, 5);
}
static void V_DestroyVoice(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        voice_free(v);
    RET(0, 1);
}

/* --- IXAudio2SourceVoice ------------------------------------------------------------------------- */

static void SV_Start(Guest* g) { Voice* v = voice(ARG(0)); if (v) v->started = 1; RET(S_OK, 3); }
static void SV_Stop(Guest* g) { Voice* v = voice(ARG(0)); if (v) v->started = 0; RET(S_OK, 3); }
/* SubmitSourceBuffer(XAUDIO2_BUFFER*, XAUDIO2_BUFFER_WMA*) */
static void SV_SubmitSourceBuffer(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (!v || v->nqueue >= MAX_QUEUE)
        RET(0x88960001u, 3); /* XAUDIO2_E_INVALID_CALL */
    uint32_t b = ARG(1);
    Buf* q = &v->queue[v->nqueue++];
    q->flags = rd32(b), q->bytes = rd32(b + 4), q->data = rd32(b + 8);
    q->play_begin = rd32(b + 12), q->play_length = rd32(b + 16);
    q->loop_begin = rd32(b + 20), q->loop_length = rd32(b + 24), q->loop_count = rd32(b + 28);
    q->context = rd32(b + 32);
    RET(S_OK, 3);
}
static void SV_FlushSourceBuffers(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (!v)
        RET(S_OK, 1);
    /* every queued buffer but the playing one ends now, with its OnBufferEnd */
    uint32_t keep = v->buffer_started && v->nqueue ? 1 : 0;
    uint32_t n = v->nqueue;
    Buf ended[MAX_QUEUE];
    memcpy(ended, v->queue + keep, (n - keep) * sizeof(Buf));
    v->nqueue = keep;
    for (uint32_t i = 0; i < n - keep; ++i)
        vcb(v->callback, 4, 1, ended[i].context, 0);
    RET(S_OK, 1);
}
static void SV_Discontinuity(Guest* g) { RET(S_OK, 1); }
static void SV_ExitLoop(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v && v->nqueue)
        v->queue[0].loop_count = 0;
    RET(S_OK, 2);
}
/* GetState(XAUDIO2_VOICE_STATE*): pCurrentBufferContext, BuffersQueued, SamplesPlayed */
static void SV_GetState(Guest* g)
{
    Voice* v = voice(ARG(0));
    uint32_t p = ARG(1);
    wr32(p, v && v->nqueue ? v->queue[0].context : 0);
    wr32(p + 4, v ? v->nqueue : 0);
    wr64(p + 8, v ? (uint64_t)v->played : 0);
    RET(0, 2);
}
static void SV_SetFrequencyRatio(Guest* g)
{
    Voice* v = voice(ARG(0));
    if (v)
        memcpy(&v->ratio, GUEST_PTR(g->esp + 8), 4);
    RET(S_OK, 3);
}
static void SV_GetFrequencyRatio(Guest* g)
{
    Voice* v = voice(ARG(0));
    float f = v ? v->ratio : 1.0f;
    memcpy(GUEST_PTR(ARG(1)), &f, 4);
    RET(0, 2);
}

/* --- creation: CoGetClassObject for the engine, CoCreateInstance for effects ----------------------- */

static void make_vtbls(void);

/* CoGetClassObject(clsid, context, server, riid, out) */
static void sh_CoGetClassObject(Guest* g)
{
    make_vtbls();
    if (ARG(4))
        wr32(ARG(4), 0);
    if (!memcmp(GUEST_PTR(ARG(0)), CLSID_XAudio2_4, 16))
    {
        uint32_t f = voice_new(V_FACTORY);
        wr32(ARG(4), f);
        RET(f ? S_OK : E_NOINTERFACE, 5);
    }
    const uint8_t* c = GUEST_PTR(ARG(0));
    rt_log("[xaudio2] CoGetClassObject({%02X%02X%02X%02X-...}): not a class of this host\n", c[3], c[2], c[1], c[0]);
    RET(0x80040154u, 5); /* REGDB_E_CLASSNOTREG */
}

static int class_hook(uint32_t clsid, uint32_t riid, uint32_t* obj)
{
    (void)riid;
    make_vtbls();
    const uint8_t* c = GUEST_PTR(clsid);
    if (!memcmp(c, CLSID_XAudio2_4, 16)) /* XAudio2Create: the engine itself */
    {
        *obj = voice_new(V_ENGINE);
        return 1;
    }
    if (!memcmp(c, CLSID_Reverb_A, 16) || !memcmp(c, CLSID_Reverb_B, 16) || !memcmp(c, CLSID_VolumeMeter, 16))
    {
        *obj = voice_new(V_EFFECT);
        Voice* e = voice(*obj);
        if (e)
            e->fxkind = memcmp(c, CLSID_VolumeMeter, 16) ? FX_REVERB : FX_METER;
        return 1;
    }
    return 0;
}

#define VOICE_NAMES "GetVoiceDetails", "SetOutputVoices", "SetEffectChain", "EnableEffect", "DisableEffect", \
    "GetEffectState", "SetEffectParameters", "GetEffectParameters", "SetFilterParameters", "GetFilterParameters", \
    "SetOutputFilterParameters", "GetOutputFilterParameters", "SetVolume", "GetVolume", "SetChannelVolumes", \
    "GetChannelVolumes", "SetOutputMatrix", "GetOutputMatrix", "DestroyVoice"
static const char* const kEngine[] = { "QueryInterface", "AddRef", "Release", "GetDeviceCount", "GetDeviceDetails",
    "Initialize", "RegisterForCallbacks", "UnregisterForCallbacks", "CreateSourceVoice", "CreateSubmixVoice",
    "CreateMasteringVoice", "StartEngine", "StopEngine", "CommitChanges", "GetPerformanceData",
    "SetDebugConfiguration", NULL };
static const char* const kVoice[] = { VOICE_NAMES, NULL };
static const char* const kSource[] = { VOICE_NAMES, "Start", "Stop", "SubmitSourceBuffer", "FlushSourceBuffers",
    "Discontinuity", "ExitLoop", "GetState", "SetFrequencyRatio", "GetFrequencyRatio", NULL };
static const char* const kEffect[] = { "QueryInterface", "AddRef", "Release", NULL };
static const char* const kFactory[] = { "QueryInterface", "AddRef", "Release", "CreateInstance", "LockServer", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    for (uint32_t i = 0; i < n; ++i)
    {
        char full[96];
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("xaudio2_4.dll", full));
    }
    return v;
}

static void make_vtbls(void)
{
    if (g_vtbl[V_ENGINE])
        return;
    g_vtbl[V_ENGINE] = make_vtbl("IXAudio2", kEngine);
    g_vtbl[V_MASTER] = make_vtbl("IXAudio2MasteringVoice", kVoice);
    g_vtbl[V_SUBMIX] = make_vtbl("IXAudio2SubmixVoice", kVoice);
    g_vtbl[V_SOURCE] = make_vtbl("IXAudio2SourceVoice", kSource);
    g_vtbl[V_EFFECT] = make_vtbl("XAPO", kEffect);
    g_vtbl[V_FACTORY] = make_vtbl("IClassFactory", kFactory);
}

#define X(iface, name, fn) { "xaudio2_4.dll", iface "::" name, fn }
#define VOICE(iface)                                                                               \
    X(iface, "GetVoiceDetails", V_GetVoiceDetails), X(iface, "SetOutputVoices", V_SetOutputVoices), \
    X(iface, "SetEffectChain", V_SetEffectChain), X(iface, "EnableEffect", V_EnableEffect),          \
    X(iface, "DisableEffect", V_DisableEffect), X(iface, "GetEffectState", V_GetEffectState),        \
    X(iface, "SetEffectParameters", V_SetEffectParameters),                                          \
    X(iface, "GetEffectParameters", V_GetEffectParameters),                                          \
    X(iface, "SetFilterParameters", V_SetFilterParameters),                                          \
    X(iface, "GetFilterParameters", V_GetFilterParameters),                                          \
    X(iface, "SetOutputFilterParameters", V_SetOutputFilterParameters),                              \
    X(iface, "GetOutputFilterParameters", V_GetOutputFilterParameters),                              \
    X(iface, "SetVolume", V_SetVolume), X(iface, "GetVolume", V_GetVolume),                          \
    X(iface, "SetChannelVolumes", V_SetChannelVolumes), X(iface, "GetChannelVolumes", V_GetChannelVolumes), \
    X(iface, "SetOutputMatrix", V_SetOutputMatrix), X(iface, "GetOutputMatrix", V_GetOutputMatrix),  \
    X(iface, "DestroyVoice", V_DestroyVoice)

static const ShimDef XAUDIO2[] = {
    { "ole32.dll", "CoGetClassObject", sh_CoGetClassObject },
    X("IClassFactory", "QueryInterface", Com_QueryInterface),
    X("IClassFactory", "AddRef", Com_AddRef),
    X("IClassFactory", "Release", Com_Release),
    X("IClassFactory", "CreateInstance", CF_CreateInstance),
    X("IClassFactory", "LockServer", CF_LockServer),
    X("XAPO", "QueryInterface", Com_QueryInterface),
    X("XAPO", "AddRef", Com_AddRef),
    X("XAPO", "Release", Com_Release),
    X("IXAudio2", "QueryInterface", Com_QueryInterface),
    X("IXAudio2", "AddRef", Com_AddRef),
    X("IXAudio2", "Release", Com_Release),
    X("IXAudio2", "GetDeviceCount", XA_GetDeviceCount),
    X("IXAudio2", "GetDeviceDetails", XA_GetDeviceDetails),
    X("IXAudio2", "Initialize", XA_Initialize),
    X("IXAudio2", "RegisterForCallbacks", XA_RegisterForCallbacks),
    X("IXAudio2", "UnregisterForCallbacks", XA_UnregisterForCallbacks),
    X("IXAudio2", "CreateSourceVoice", XA_CreateSourceVoice),
    X("IXAudio2", "CreateSubmixVoice", XA_CreateSubmixVoice),
    X("IXAudio2", "CreateMasteringVoice", XA_CreateMasteringVoice),
    X("IXAudio2", "StartEngine", XA_StartEngine),
    X("IXAudio2", "StopEngine", XA_StopEngine),
    X("IXAudio2", "CommitChanges", XA_CommitChanges),
    X("IXAudio2", "GetPerformanceData", XA_GetPerformanceData),
    X("IXAudio2", "SetDebugConfiguration", XA_SetDebugConfiguration),
    VOICE("IXAudio2MasteringVoice"),
    VOICE("IXAudio2SubmixVoice"),
    VOICE("IXAudio2SourceVoice"),
    X("IXAudio2SourceVoice", "Start", SV_Start),
    X("IXAudio2SourceVoice", "Stop", SV_Stop),
    X("IXAudio2SourceVoice", "SubmitSourceBuffer", SV_SubmitSourceBuffer),
    X("IXAudio2SourceVoice", "FlushSourceBuffers", SV_FlushSourceBuffers),
    X("IXAudio2SourceVoice", "Discontinuity", SV_Discontinuity),
    X("IXAudio2SourceVoice", "ExitLoop", SV_ExitLoop),
    X("IXAudio2SourceVoice", "GetState", SV_GetState),
    X("IXAudio2SourceVoice", "SetFrequencyRatio", SV_SetFrequencyRatio),
    X("IXAudio2SourceVoice", "GetFrequencyRatio", SV_GetFrequencyRatio),
    { NULL, NULL, NULL },
};

void xaudio2_init(void)
{
    thunk_register(XAUDIO2);
    ole_class_hook = class_hook;
}
