/* XAudio2 2.4 (March 2009 DirectX SDK) for FINAL FANTASY XIV 1.23b's sound driver
 * (SQEX::CDev::Engine::Sd::Driver::XA2).
 *
 * The game makes the engine with CoGetClassObject(CLSID_XAudio2 2.4) and its class factory, and
 * its effects (reverb, volume meter) with CoCreateInstance; it asserts and stops when any of
 * them fail. This is the whole interface it reaches: the engine, mastering, submix and source
 * voices, and effect objects.
 *
 * Sound is not output yet: the engine is a clock. Each started source voice consumes its queued
 * buffers at its sample rate times its frequency ratio, and the game's callbacks run as XAudio2
 * would run them (OnVoiceProcessingPassStart, OnBufferStart/End, OnLoopEnd, OnStreamEnd, the
 * engine's pass callbacks), every 10 ms on an audio thread, so its streaming and its timing see
 * what they expect. Mixing to the speakers goes in where the clock advances.
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

typedef struct Buf
{
    uint32_t flags, bytes, data, play_begin, play_length, loop_begin, loop_length, loop_count, context;
} Buf;

#define MAX_QUEUE 64

typedef struct Voice
{
    int kind;
    uint32_t guest;
    int32_t refs;           /* engine, effects, factory */
    uint32_t channels, rate, flags;
    uint32_t callback;      /* source: IXAudio2VoiceCallback* */
    uint16_t format_tag, block_align, samples_per_block;
    float volume, ratio;
    int started;
    Buf queue[MAX_QUEUE];
    uint32_t nqueue;
    double pos;             /* frames into the current buffer */
    int buffer_started;
    uint32_t loops_done;
    uint64_t played;        /* frames */
    float chvol[8];
} Voice;

#define MAX_VOICES 512
static Voice g_v[MAX_VOICES];
static uint32_t g_vtbl[V_FACTORY + 1];
static uint32_t g_engine_callbacks[16];
static uint32_t g_nengine_cb;
static int g_engine_started;
static volatile int g_thread_up;

static Voice* voice(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < MAX_VOICES && g_v[i].kind != V_FREE && g_v[i].guest == p ? &g_v[i] : NULL;
}

static uint32_t voice_new(int kind)
{
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
        if (g_v[i].kind == V_FREE)
        {
            Voice* v = &g_v[i];
            memset(v, 0, sizeof *v);
            v->kind = kind;
            v->refs = 1;
            v->volume = v->ratio = 1.0f;
            for (int c = 0; c < 8; ++c)
                v->chvol[c] = 1.0f;
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
    v->kind = V_FREE;
}

/* frames in a buffer's playable region */
static uint32_t buf_frames(const Voice* v, const Buf* b)
{
    if (b->play_length)
        return b->play_length;
    uint32_t frames;
    if (v->format_tag == 2 && v->block_align) /* MS ADPCM */
        frames = (b->bytes / v->block_align) * v->samples_per_block;
    else
        frames = v->block_align ? b->bytes / v->block_align : 0;
    return frames > b->play_begin ? frames - b->play_begin : 0;
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

/* one processing pass of `ms` milliseconds */
static void pass(double ms)
{
    if (g_engine_started)
        ecb(0);
    for (uint32_t i = 1; i < MAX_VOICES; ++i)
    {
        Voice* v = &g_v[i];
        if (v->kind != V_SOURCE || !v->started)
            continue;
        uint32_t guest = v->guest;
        if (v->callback)
        {
            uint32_t need = v->nqueue ? 0 : (uint32_t)(v->rate * ms / 1000.0) * v->block_align;
            vcb(v->callback, 0, 1, need, 0);
            if (voice(guest) != v)
                continue; /* the callback destroyed it */
        }
        double frames = v->rate * (double)v->ratio * ms / 1000.0;
        while (frames > 0 && v->nqueue && voice(guest) == v)
        {
            Buf* b = &v->queue[0];
            if (!v->buffer_started)
            {
                v->buffer_started = 1;
                vcb(v->callback, 3, 1, b->context, 0);
                if (voice(guest) != v || !v->nqueue)
                    break;
                b = &v->queue[0];
            }
            uint32_t total = buf_frames(v, b);
            double left = (double)total - v->pos;
            if (frames < left)
            {
                v->pos += frames;
                v->played += (uint64_t)frames;
                frames = 0;
                break;
            }
            frames -= left;
            v->played += (uint64_t)left;
            if (b->loop_count && v->loops_done < b->loop_count)
            {
                /* a loop: back to its start (255 = forever) */
                if (b->loop_count != 255)
                    v->loops_done++;
                v->pos = b->loop_begin;
                vcb(v->callback, 5, 1, b->context, 0);
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
                break;
            if (done.flags & 0x40) /* XAUDIO2_END_OF_STREAM */
                vcb(v->callback, 2, 0, 0, 0);
        }
        if (voice(guest) == v && v->callback)
            vcb(v->callback, 1, 0, 0, 0);
    }
    if (g_engine_started)
        ecb(1);
}

static void audio_thread(void* arg)
{
    (void)arg;
    uint64_t last = rt_monotonic_ns();
    for (;;)
    {
        plat_sleep_ms(10);
        uint64_t now = rt_monotonic_ns();
        double ms = (now - last) / 1e6;
        last = now;
        if (ms > 100)
            ms = 100;
        gt_lock();
        pass(ms);
        gt_unlock();
    }
}

static void start_thread(void)
{
    if (!g_thread_up)
    {
        g_thread_up = 1;
        plat_thread_start(audio_thread, NULL);
    }
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
    if (v->format_tag == 2)
        v->samples_per_block = rd16(wf + 18 + 2);
    v->flags = ARG(3);
    v->callback = ARG(5);
    if (!v->rate)
        v->rate = 48000;
    wr32(ARG(1), p);
    RET(S_OK, 8);
}
/* CreateSubmixVoice(out, channels, rate, flags, stage, sends, effects) */
static void XA_CreateSubmixVoice(Guest* g)
{
    uint32_t p = voice_new(V_SUBMIX);
    Voice* v = voice(p);
    if (v)
        v->channels = ARG(2), v->rate = ARG(3), v->flags = ARG(4);
    wr32(ARG(1), p);
    RET(p ? S_OK : E_INVALIDARG, 8);
}
/* CreateMasteringVoice(out, channels, rate, flags, device, effects) */
static void XA_CreateMasteringVoice(Guest* g)
{
    uint32_t p = voice_new(V_MASTER);
    Voice* v = voice(p);
    if (v)
        v->channels = ARG(2) ? ARG(2) : 2, v->rate = ARG(3) ? ARG(3) : 48000;
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
static void V_SetOutputVoices(Guest* g) { RET(S_OK, 2); }
static void V_SetEffectChain(Guest* g) { RET(S_OK, 2); }
static void V_EnableEffect(Guest* g) { RET(S_OK, 3); }
static void V_DisableEffect(Guest* g) { RET(S_OK, 3); }
static void V_GetEffectState(Guest* g) { if (ARG(2)) wr32(ARG(2), 1); RET(0, 3); }
static void V_SetEffectParameters(Guest* g) { RET(S_OK, 5); }
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
static void V_SetOutputMatrix(Guest* g) { RET(S_OK, 6); }
/* GetOutputMatrix(destination, source channels, destination channels, float*) */
static void V_GetOutputMatrix(Guest* g)
{
    uint32_t sc = ARG(2), dc = ARG(3), p = ARG(4);
    for (uint32_t d = 0; d < dc; ++d)
        for (uint32_t s = 0; s < sc; ++s)
        {
            float f = (d == s || sc == 1) ? 1.0f : 0.0f;
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
    wr64(p + 8, v ? v->played : 0);
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
