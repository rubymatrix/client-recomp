/* DirectSound 8 for 64-bit hosts (R3.1), on SDL3 audio (decided 2026-09-24: SDL3 for window,
 * input and audio).
 *
 * FFXiMain plays everything through DirectSound secondary buffers - PCM in a ring it refills from
 * its own sound thread, positioned by the play and write cursors. Those cursors have to move in
 * real time, so this is a software mixer and not a stub: SDL pulls 48 kHz stereo float from us,
 * and we mix every playing buffer into it (linear resampling to the buffer's frequency, volume and
 * pan in DirectSound's hundredths of a decibel), advancing its play cursor, looping or stopping at
 * the end, and signalling the IDirectSoundNotify events the cursor crosses.
 *
 * Buffer memory lives in the guest window, where the game's Lock pointers point; the mixer reads
 * it from SDL's audio thread, as the Windows mixer reads a locked-and-written buffer. Buffer state
 * is shared with that thread under one mutex. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsound.h"
#include "gthread.h"
#include "gwin.h"
#include "kobj.h"
#include "thunk.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_NOAGGREGATION 0x80040110u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define DSERR_NODRIVER 0x88780078u
#define E_NOINTERFACE 0x80004002u

#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100u
#define DSBCAPS_LOCSOFTWARE 0x00000008u
#define DSBSTATUS_PLAYING 1u
#define DSBSTATUS_LOOPING 4u
#define DSBLOCK_FROMWRITECURSOR 1u
#define DSBLOCK_ENTIREBUFFER 2u
#define DSBPLAY_LOOPING 1u
#define DSBPN_OFFSETSTOP 0xFFFFFFFFu

#define OUT_RATE 48000
#define WRITE_LEAD_MS 20 /* how far the write cursor runs ahead of the play cursor */
#define MAX_BUFS 512
#define MAX_NOTIFY 64

typedef struct Data /* the sample memory, shared by DuplicateSoundBuffer */
{
    uint32_t mem, size;
    int refs;
} Data;

typedef struct Buf
{
    int used, primary;
    int32_t refs;
    uint32_t guest, notify_guest; /* COM pointers: IDirectSoundBuffer8, IDirectSoundNotify */
    uint32_t flags;
    Data* data;
    uint16_t channels, bits, block;
    uint32_t rate, freq;
    int32_t volume, pan;
    float gain_l, gain_r;
    int playing, looping;
    uint64_t pos; /* frames, 32.32 fixed point */
    uint32_t nnotify;
    uint32_t notify_off[MAX_NOTIFY], notify_ev[MAX_NOTIFY];
} Buf;

static Buf g_bufs[MAX_BUFS];
static SDL_Mutex* g_mix;
static SDL_AudioStream* g_stream;
static uint32_t g_ds;      /* the IDirectSound8 object */
static int32_t g_ds_refs;
static uint32_t g_vtbl_ds, g_vtbl_buf, g_vtbl_notify;
static float g_master = 1.0f;

static const uint8_t IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_IDirectSoundBuffer[16] = { 0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                                    0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
static const uint8_t IID_IDirectSoundBuffer8[16] = { 0x49, 0xA4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4D,
                                                     0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E };
static const uint8_t IID_IDirectSoundNotify[16] = { 0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11,
                                                    0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 };

static float db_gain(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : powf(10.0f, (float)hundredths / 2000.0f); }

static void update_gain(Buf* b)
{
    float v = db_gain(b->volume);
    b->gain_l = v * (b->pan > 0 ? db_gain(-b->pan) : 1.0f);
    b->gain_r = v * (b->pan < 0 ? db_gain(b->pan) : 1.0f);
}

static Buf* buf(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < MAX_BUFS && g_bufs[i].used && (g_bufs[i].guest == p || g_bufs[i].notify_guest == p) ? &g_bufs[i] : NULL;
}

/* --- the mixer (SDL's audio thread) ------------------------------------------------------------------ */
static uint32_t cursor_bytes(const Buf* b) { return (uint32_t)(b->pos >> 32) * b->block; }

static void notify_crossed(Buf* b, uint32_t from, uint32_t to, int wrapped)
{
    for (uint32_t i = 0; i < b->nnotify; ++i)
    {
        uint32_t o = b->notify_off[i];
        if (o != DSBPN_OFFSETSTOP && (wrapped ? (o >= from || o < to) : (o >= from && o < to)))
            k_event_set(b->notify_ev[i], 1);
    }
}

static void notify_stop(Buf* b)
{
    for (uint32_t i = 0; i < b->nnotify; ++i)
        if (b->notify_off[i] == DSBPN_OFFSETSTOP)
            k_event_set(b->notify_ev[i], 1);
}

static float sample(const uint8_t* m, const Buf* b, uint32_t frame, int ch)
{
    const uint8_t* p = m + (size_t)frame * b->block + (ch < b->channels ? ch : 0) * (b->bits / 8);
    return b->bits == 8 ? ((float)*p - 128.0f) / 128.0f : (float)(int16_t)(p[0] | (p[1] << 8)) / 32768.0f;
}

static void mix(float* out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    for (int i = 0; i < MAX_BUFS; ++i)
    {
        Buf* b = &g_bufs[i];
        if (!b->used || b->primary || !b->playing || !b->data || !b->data->mem)
            continue;
        const uint8_t* m = GUEST_PTR(b->data->mem);
        uint32_t nframes = b->data->size / b->block;
        if (!nframes)
            continue;
        uint64_t step = ((uint64_t)b->freq << 32) / OUT_RATE, end = (uint64_t)nframes << 32;
        uint32_t start = cursor_bytes(b);
        int wrapped = 0;
        for (int f = 0; f < frames; ++f)
        {
            uint32_t fr = (uint32_t)(b->pos >> 32), nx = fr + 1 < nframes ? fr + 1 : (b->looping ? 0 : fr);
            float t = (float)(uint32_t)b->pos / 4294967296.0f;
            float l = sample(m, b, fr, 0) * (1 - t) + sample(m, b, nx, 0) * t;
            float r = b->channels > 1 ? sample(m, b, fr, 1) * (1 - t) + sample(m, b, nx, 1) * t : l;
            out[2 * f] += l * b->gain_l;
            out[2 * f + 1] += r * b->gain_r;
            b->pos += step;
            if (b->pos >= end)
            {
                if (!b->looping)
                {
                    b->pos = 0;
                    b->playing = 0;
                    notify_crossed(b, start, b->data->size, 0);
                    notify_stop(b);
                    start = 0;
                    break;
                }
                b->pos -= end;
                wrapped = 1;
            }
        }
        if (b->playing)
            notify_crossed(b, start, cursor_bytes(b), wrapped);
    }
    for (int f = 0; f < 2 * frames; ++f)
    {
        float v = out[f] * g_master;
        out[f] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    }
}

static void SDLCALL audio_callback(void* user, SDL_AudioStream* stream, int additional, int total)
{
    (void)user;
    (void)total;
    float chunk[2 * 512];
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0)
    {
        int n = frames < 512 ? frames : 512;
        SDL_LockMutex(g_mix);
        mix(chunk, n);
        SDL_UnlockMutex(g_mix);
        SDL_PutAudioStreamData(stream, chunk, n * (int)(2 * sizeof(float)));
        frames -= n;
    }
}

static int audio_open(void)
{
    if (g_stream)
        return 1;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        rt_log("[recomp] dsound: SDL audio: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480"); /* 10 ms: the cursors move in steps this size */
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, OUT_RATE };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!g_stream)
    {
        rt_log("[recomp] dsound: no audio device: %s\n", SDL_GetError());
        return 0;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return 1;
}

/* --- objects ---------------------------------------------------------------------------------------------- */
static uint32_t com_new(uint32_t vtbl, uint32_t index)
{
    uint32_t p = gheap_alloc(8, 1);
    wr32(p, vtbl);
    wr32(p + 4, index);
    return p;
}

static void buf_free(Buf* b)
{
    if (b->data && --b->data->refs == 0)
    {
        if (b->data->mem)
            gheap_free(b->data->mem);
        free(b->data);
    }
    gheap_free(b->guest);
    if (b->notify_guest)
        gheap_free(b->notify_guest);
    memset(b, 0, sizeof *b);
}

static Buf* buf_new(void)
{
    for (int i = 0; i < MAX_BUFS; ++i)
        if (!g_bufs[i].used)
        {
            Buf* b = &g_bufs[i];
            memset(b, 0, sizeof *b);
            b->used = 1;
            b->refs = 1;
            b->guest = com_new(g_vtbl_buf, (uint32_t)i);
            return b;
        }
    return NULL;
}

/* WAVEFORMATEX: PCM (tag 1) only, 8/16 bit, 1/2 channels */
static int read_format(uint32_t w, Buf* b)
{
    if (!w || rd16(w) != 1)
        return 0;
    uint16_t ch = rd16(w + 2), bits = rd16(w + 14);
    uint32_t rate = rd32(w + 4);
    if ((ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 100 || rate > 200000)
        return 0;
    b->channels = ch, b->bits = bits, b->rate = b->freq = rate, b->block = (uint16_t)(ch * bits / 8);
    return 1;
}

/* --- IDirectSound8 -------------------------------------------------------------------------------------- */
static void DS_QueryInterface(Guest* g)
{
    wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}
static void DS_AddRef(Guest* g) { RET((uint32_t)++g_ds_refs, 1); }
static void DS_Release(Guest* g) { RET((uint32_t)(g_ds_refs > 0 ? --g_ds_refs : 0), 1); }

/* CreateSoundBuffer(pcDSBufferDesc, ppDSBuffer, pUnkOuter) */
static void DS_CreateSoundBuffer(Guest* g)
{
    uint32_t d = ARG(1);
    wr32(ARG(2), 0);
    if (ARG(3))
        RET(DSERR_NOAGGREGATION, 4);
    if (!d || rd32(d) < 20)
        RET(DSERR_INVALIDPARAM, 4);
    uint32_t flags = rd32(d + 4), bytes = rd32(d + 8), wfx = rd32(d + 16);
    SDL_LockMutex(g_mix);
    Buf* b = buf_new();
    if (!b)
    {
        SDL_UnlockMutex(g_mix);
        RET(DSERR_OUTOFMEMORY, 4);
    }
    b->flags = flags | DSBCAPS_LOCSOFTWARE;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (b->primary)
    {
        b->channels = 2, b->bits = 16, b->rate = b->freq = 22050, b->block = 4;
    }
    else if (!read_format(wfx, b) || bytes < 4 || bytes > 0x0FFFFFFFu)
    {
        buf_free(b);
        SDL_UnlockMutex(g_mix);
        RET(bytes < 4 ? DSERR_INVALIDPARAM : DSERR_BADFORMAT, 4);
    }
    else
    {
        b->data = (Data*)calloc(1, sizeof(Data));
        b->data->refs = 1;
        b->data->size = bytes - bytes % b->block;
        b->data->mem = gheap_alloc(b->data->size, 1);
        if (b->bits == 8)
            memset(GUEST_PTR(b->data->mem), 0x80, b->data->size);
    }
    update_gain(b);
    wr32(ARG(2), b->guest);
    SDL_UnlockMutex(g_mix);
    RET(DS_OK, 4);
}

static void DS_GetCaps(Guest* g)
{
    uint32_t c = ARG(1);
    memset(GUEST_PTR(c + 4), 0, 92);
    wr32(c + 4, 0x00000F5Fu); /* primary and secondary mono/stereo/8/16-bit, continuous rate */
    wr32(c + 8, 100);         /* dwMinSecondarySampleRate */
    wr32(c + 12, 200000);     /* dwMaxSecondarySampleRate */
    wr32(c + 16, 1);          /* dwPrimaryBuffers */
    RET(DS_OK, 2);
}

/* DuplicateSoundBuffer(pDSBufferOriginal, ppDSBufferDuplicate): the same samples, its own cursor */
static void DS_DuplicateSoundBuffer(Guest* g)
{
    wr32(ARG(2), 0);
    SDL_LockMutex(g_mix);
    Buf* o = buf(ARG(1));
    Buf* b = o && !o->primary ? buf_new() : NULL;
    if (!b)
    {
        SDL_UnlockMutex(g_mix);
        RET(DSERR_INVALIDPARAM, 3);
    }
    Buf* src = buf(ARG(1)); /* o is unchanged: the table does not move */
    b->flags = src->flags, b->data = src->data, b->data->refs++;
    b->channels = src->channels, b->bits = src->bits, b->block = src->block, b->rate = src->rate, b->freq = src->freq;
    b->volume = src->volume, b->pan = src->pan;
    update_gain(b);
    wr32(ARG(2), b->guest);
    SDL_UnlockMutex(g_mix);
    RET(DS_OK, 3);
}

static void DS_SetCooperativeLevel(Guest* g) { RET(DS_OK, 3); }
static void DS_Compact(Guest* g) { RET(DS_OK, 1); }

static void DS_GetSpeakerConfig(Guest* g)
{
    wr32(ARG(1), 0x00000004u); /* DSSPEAKER_STEREO */
    RET(DS_OK, 2);
}

static void DS_SetSpeakerConfig(Guest* g) { RET(DS_OK, 2); }
static void DS_Initialize(Guest* g) { RET(DS_OK, 2); }

/* --- IDirectSoundBuffer8 ---------------------------------------------------------------------------------- */
static int iid_is(uint32_t p, const uint8_t* iid) { return !memcmp(GUEST_PTR(p), iid, 16); }

static void DSB_QueryInterface(Guest* g)
{
    Buf* b = buf(ARG(0));
    uint32_t out = 0;
    if (b && (iid_is(ARG(1), IID_IUnknown) || iid_is(ARG(1), IID_IDirectSoundBuffer) || iid_is(ARG(1), IID_IDirectSoundBuffer8)))
        out = b->guest;
    else if (b && !b->primary && iid_is(ARG(1), IID_IDirectSoundNotify) && (b->flags & DSBCAPS_CTRLPOSITIONNOTIFY))
    {
        if (!b->notify_guest)
            b->notify_guest = com_new(g_vtbl_notify, rd32(b->guest + 4));
        out = b->notify_guest;
    }
    wr32(ARG(2), out);
    if (!out)
        RET(E_NOINTERFACE, 3);
    b->refs++;
    RET(DS_OK, 3);
}

static void DSB_AddRef(Guest* g)
{
    Buf* b = buf(ARG(0));
    RET(b ? (uint32_t)++b->refs : 0, 1);
}

static void DSB_Release(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    uint32_t n = 0;
    if (b && (n = (uint32_t)--b->refs) == 0)
        buf_free(b);
    SDL_UnlockMutex(g_mix);
    RET(n, 1);
}

static void DSB_GetCaps(Guest* g)
{
    Buf* b = buf(ARG(0));
    if (!b)
        RET(DSERR_INVALIDPARAM, 2);
    wr32(ARG(1) + 4, b->flags);
    wr32(ARG(1) + 8, b->data ? b->data->size : 0);
    wr32(ARG(1) + 12, 0);
    wr32(ARG(1) + 16, 0);
    RET(DS_OK, 2);
}

/* GetCurrentPosition(pdwCurrentPlayCursor, pdwCurrentWriteCursor) */
static void DSB_GetCurrentPosition(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    uint32_t play = 0, write = 0;
    if (b && b->data && b->data->size)
    {
        play = cursor_bytes(b);
        write = play;
        if (b->playing)
        {
            uint32_t lead = (b->freq * WRITE_LEAD_MS / 1000) * b->block;
            write = (play + lead) % b->data->size;
        }
    }
    SDL_UnlockMutex(g_mix);
    if (ARG(1))
        wr32(ARG(1), play);
    if (ARG(2))
        wr32(ARG(2), write);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 3);
}

/* GetFormat(pwfxFormat, dwSizeAllocated, pdwSizeWritten) */
static void DSB_GetFormat(Guest* g)
{
    Buf* b = buf(ARG(0));
    if (!b)
        RET(DSERR_INVALIDPARAM, 4);
    if (ARG(3))
        wr32(ARG(3), 18);
    if (ARG(1) && ARG(2) >= 16)
    {
        uint32_t w = ARG(1);
        wr16(w, 1);
        wr16(w + 2, b->channels);
        wr32(w + 4, b->rate);
        wr32(w + 8, b->rate * b->block);
        wr16(w + 12, b->block);
        wr16(w + 14, b->bits);
        if (ARG(2) >= 18)
            wr16(w + 16, 0);
    }
    RET(DS_OK, 4);
}

static void DSB_GetVolume(Guest* g)
{
    Buf* b = buf(ARG(0));
    wr32(ARG(1), b ? (uint32_t)b->volume : 0);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_GetPan(Guest* g)
{
    Buf* b = buf(ARG(0));
    wr32(ARG(1), b ? (uint32_t)b->pan : 0);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_GetFrequency(Guest* g)
{
    Buf* b = buf(ARG(0));
    wr32(ARG(1), b ? b->freq : 0);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_GetStatus(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    uint32_t s = b && b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0) : 0;
    SDL_UnlockMutex(g_mix);
    wr32(ARG(1), s);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

/* Lock(dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags) */
static void DSB_Lock(Guest* g)
{
    Buf* b = buf(ARG(0));
    if (!b || b->primary || !b->data)
        RET(DSERR_INVALIDCALL, 8);
    uint32_t size = b->data->size, off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & DSBLOCK_FROMWRITECURSOR)
    {
        SDL_LockMutex(g_mix);
        off = cursor_bytes(b);
        if (b->playing)
            off = (off + (b->freq * WRITE_LEAD_MS / 1000) * b->block) % size;
        SDL_UnlockMutex(g_mix);
    }
    if (flags & DSBLOCK_ENTIREBUFFER)
        n = size;
    if (off >= size || n > size || !n)
        RET(DSERR_INVALIDPARAM, 8);
    uint32_t first = n < size - off ? n : size - off;
    wr32(ARG(3), b->data->mem + off);
    wr32(ARG(4), first);
    if (ARG(5))
        wr32(ARG(5), n > first ? b->data->mem : 0);
    if (ARG(6))
        wr32(ARG(6), n - first);
    RET(DS_OK, 8);
}

/* Play(dwReserved1, dwPriority, dwFlags) */
static void DSB_Play(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b)
    {
        b->playing = 1;
        b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    }
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 4);
}

static void DSB_SetCurrentPosition(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b && b->data && ARG(1) < b->data->size)
        b->pos = (uint64_t)(ARG(1) / b->block) << 32;
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

/* the primary buffer's format is the output's business, not ours: accepted and remembered */
static void DSB_SetFormat(Guest* g)
{
    Buf* b = buf(ARG(0));
    if (!b || !b->primary)
        RET(DSERR_INVALIDCALL, 2);
    Buf tmp = *b;
    if (!read_format(ARG(1), &tmp))
        RET(DSERR_BADFORMAT, 2);
    b->channels = tmp.channels, b->bits = tmp.bits, b->rate = b->freq = tmp.rate, b->block = tmp.block;
    RET(DS_OK, 2);
}

static void DSB_SetVolume(Guest* g)
{
    int32_t v = (int32_t)ARG(1);
    if (v > 0 || v < -10000)
        RET(DSERR_INVALIDPARAM, 2);
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b)
    {
        b->volume = v;
        update_gain(b);
        if (b->primary)
            g_master = db_gain(v);
    }
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_SetPan(Guest* g)
{
    int32_t p = (int32_t)ARG(1);
    if (p > 10000 || p < -10000)
        RET(DSERR_INVALIDPARAM, 2);
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b)
    {
        b->pan = p;
        update_gain(b);
    }
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_SetFrequency(Guest* g)
{
    uint32_t f = ARG(1);
    if (f && (f < 100 || f > 200000))
        RET(DSERR_INVALIDPARAM, 2);
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b && !b->primary)
        b->freq = f ? f : b->rate; /* DSBFREQUENCY_ORIGINAL */
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}

static void DSB_Stop(Guest* g)
{
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (b && b->playing)
    {
        b->playing = 0;
        notify_stop(b);
    }
    SDL_UnlockMutex(g_mix);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 1);
}

static void DSB_Unlock(Guest* g) { RET(buf(ARG(0)) ? DS_OK : DSERR_INVALIDPARAM, 5); }
static void DSB_Restore(Guest* g) { RET(DS_OK, 1); }

/* --- IDirectSoundNotify: shares the buffer's reference count ------------------------------------------------ */
/* SetNotificationPositions(dwPositionNotifies, pcPositionNotifies): DSBPOSITIONNOTIFY {offset, event} */
static void DSN_SetNotificationPositions(Guest* g)
{
    uint32_t n = ARG(1), p = ARG(2);
    SDL_LockMutex(g_mix);
    Buf* b = buf(ARG(0));
    if (!b || b->playing || n > MAX_NOTIFY)
    {
        SDL_UnlockMutex(g_mix);
        RET(b && b->playing ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM, 3);
    }
    b->nnotify = n;
    for (uint32_t i = 0; i < n; ++i)
    {
        b->notify_off[i] = rd32(p + 8 * i);
        b->notify_ev[i] = rd32(p + 8 * i + 4);
    }
    SDL_UnlockMutex(g_mix);
    RET(DS_OK, 3);
}

/* --- entry point and registration ---------------------------------------------------------------------------- */
/* DirectSoundCreate8(pcGuidDevice, ppDS8, pUnkOuter): ordinal 11 */
static void sh_DirectSoundCreate8(Guest* g)
{
    wr32(ARG(1), 0);
    if (ARG(2))
        RET(DSERR_NOAGGREGATION, 3);
    if (!g_mix)
        g_mix = SDL_CreateMutex();
    if (!audio_open())
        RET(DSERR_NODRIVER, 3);
    if (!g_ds)
        g_ds = com_new(g_vtbl_ds, 0);
    g_ds_refs++;
    wr32(ARG(1), g_ds);
    RET(DS_OK, 3);
}

#define S(i, m, f) { "dsound.dll", i "::" m, f }
static const ShimDef DSOUND[] = {
    { "dsound.dll", "#11", sh_DirectSoundCreate8 },
    { "dsound.dll", "DirectSoundCreate8", sh_DirectSoundCreate8 },
    S("IDirectSound8", "QueryInterface", DS_QueryInterface),
    S("IDirectSound8", "AddRef", DS_AddRef),
    S("IDirectSound8", "Release", DS_Release),
    S("IDirectSound8", "CreateSoundBuffer", DS_CreateSoundBuffer),
    S("IDirectSound8", "GetCaps", DS_GetCaps),
    S("IDirectSound8", "DuplicateSoundBuffer", DS_DuplicateSoundBuffer),
    S("IDirectSound8", "SetCooperativeLevel", DS_SetCooperativeLevel),
    S("IDirectSound8", "Compact", DS_Compact),
    S("IDirectSound8", "GetSpeakerConfig", DS_GetSpeakerConfig),
    S("IDirectSound8", "SetSpeakerConfig", DS_SetSpeakerConfig),
    S("IDirectSound8", "Initialize", DS_Initialize),
    S("IDirectSoundBuffer8", "QueryInterface", DSB_QueryInterface),
    S("IDirectSoundBuffer8", "AddRef", DSB_AddRef),
    S("IDirectSoundBuffer8", "Release", DSB_Release),
    S("IDirectSoundBuffer8", "GetCaps", DSB_GetCaps),
    S("IDirectSoundBuffer8", "GetCurrentPosition", DSB_GetCurrentPosition),
    S("IDirectSoundBuffer8", "GetFormat", DSB_GetFormat),
    S("IDirectSoundBuffer8", "GetVolume", DSB_GetVolume),
    S("IDirectSoundBuffer8", "GetPan", DSB_GetPan),
    S("IDirectSoundBuffer8", "GetFrequency", DSB_GetFrequency),
    S("IDirectSoundBuffer8", "GetStatus", DSB_GetStatus),
    S("IDirectSoundBuffer8", "Lock", DSB_Lock),
    S("IDirectSoundBuffer8", "Play", DSB_Play),
    S("IDirectSoundBuffer8", "SetCurrentPosition", DSB_SetCurrentPosition),
    S("IDirectSoundBuffer8", "SetFormat", DSB_SetFormat),
    S("IDirectSoundBuffer8", "SetVolume", DSB_SetVolume),
    S("IDirectSoundBuffer8", "SetPan", DSB_SetPan),
    S("IDirectSoundBuffer8", "SetFrequency", DSB_SetFrequency),
    S("IDirectSoundBuffer8", "Stop", DSB_Stop),
    S("IDirectSoundBuffer8", "Unlock", DSB_Unlock),
    S("IDirectSoundBuffer8", "Restore", DSB_Restore),
    S("IDirectSoundNotify", "QueryInterface", DSB_QueryInterface),
    S("IDirectSoundNotify", "AddRef", DSB_AddRef),
    S("IDirectSoundNotify", "Release", DSB_Release),
    S("IDirectSoundNotify", "SetNotificationPositions", DSN_SetNotificationPositions),
    { NULL, NULL, NULL },
};

static const char* const kDS8[] = { "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
    "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize",
    "VerifyCertification", NULL };
static const char* const kDSB8[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
    "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play",
    "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore", "SetFX",
    "AcquireResources", "GetObjectInPath", NULL };
static const char* const kNotify[] = { "QueryInterface", "AddRef", "Release", "SetNotificationPositions", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        SDL_snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("dsound.dll", full));
    }
    return v;
}

void dsound_init(void)
{
    thunk_register(DSOUND);
}

void dsound_setup(void)
{
    g_vtbl_ds = make_vtbl("IDirectSound8", kDS8);
    g_vtbl_buf = make_vtbl("IDirectSoundBuffer8", kDSB8);
    g_vtbl_notify = make_vtbl("IDirectSoundNotify", kNotify);
}
