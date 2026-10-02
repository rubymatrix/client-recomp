/* The game's sound effects. See sewave.h. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "sewave.h"

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int sewave_load(const char* game_dir, uint32_t id, SeWave* out)
{
    memset(out, 0, sizeof *out);
    char path[1200];
    snprintf(path, sizeof path, "%s/sound/win/se/se%03u/se%06u.spw", game_dir, id / 1000, id);
    size_t size = 0;
    uint8_t* d = plat_read_file(path, &size);
    if (!d || size < 0x30 || memcmp(d, "SeWave\0\0", 8))
    {
        free(d);
        return 0;
    }
    uint32_t codec = rd32(d + 0xc), length = rd32(d + 0x14), off = rd32(d + 0x24);
    int ch = d[0x2a], spf = d[0x2c] ? d[0x2c] : 16;
    out->rate = (rd32(d + 0x1c) + rd32(d + 0x20)) & 0x7fffffffu;
    out->channels = ch;
    size_t frames = codec == 1 ? length : (size_t)length * (size_t)spf;
    size_t need = codec == 1 ? frames * ch * 2 : (size_t)length * ch * (1 + spf / 2);
    if ((codec != 0 && codec != 1) || ch < 1 || ch > 2 || !out->rate || off > size || need > size - off ||
        !(out->pcm = malloc(frames * ch * sizeof *out->pcm + 1)))
    {
        free(d);
        return 0;
    }
    const uint8_t* p = d + off;
    if (codec == 1)
        for (size_t i = 0; i < frames * ch; ++i)
            out->pcm[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
    else
    {
        /* the PS2's ADPCM filters; the history is kept as it comes, the output its low 16 bits */
        static const int C0[5] = { 0, 60, 115, 98, 122 }, C1[5] = { 0, 0, -52, -55, -60 };
        int h1[2] = { 0, 0 }, h2[2] = { 0, 0 };
        for (uint32_t f = 0; f < length; ++f)
            for (int c = 0; c < ch; ++c, p += 1 + spf / 2)
            {
                int filt = p[0] >> 4, shift = p[0] & 15, c0 = filt < 5 ? C0[filt] : 0, c1 = filt < 5 ? C1[filt] : 0;
                for (int i = 0; i < spf; ++i)
                {
                    int nib = i & 1 ? p[1 + i / 2] >> 4 : p[1 + i / 2] & 15;
                    int s = ((int16_t)(nib << 12) >> shift) + ((c0 * h1[c] + c1 * h2[c]) >> 6);
                    h2[c] = h1[c], h1[c] = s;
                    out->pcm[((size_t)f * spf + i) * ch + c] = (int16_t)s;
                }
            }
    }
    out->frames = (uint32_t)frames;
    free(d);
    return 1;
}

void sewave_free(SeWave* w)
{
    free(w->pcm);
    memset(w, 0, sizeof *w);
}

/* the sound effects' volume table (0x103889f0): 0..4096 for an index 0..127 */
static const int16_t VOLUME[128] = {
    0, 2340, 2511, 2633, 2727, 2804, 2869, 2925, 2975, 3019, 3060, 3096, 3130, 3161, 3191, 3218,
    3243, 3268, 3290, 3312, 3333, 3352, 3371, 3389, 3406, 3423, 3439, 3454, 3469, 3483, 3497, 3510,
    3523, 3536, 3548, 3560, 3572, 3583, 3594, 3605, 3615, 3625, 3635, 3645, 3654, 3664, 3673, 3682,
    3690, 3699, 3707, 3715, 3723, 3731, 3739, 3747, 3754, 3761, 3769, 3776, 3783, 3790, 3796, 3803,
    3809, 3816, 3822, 3829, 3835, 3841, 3847, 3853, 3858, 3864, 3870, 3875, 3881, 3886, 3892, 3897,
    3902, 3908, 3913, 3918, 3923, 3928, 3933, 3937, 3942, 3947, 3951, 3956, 3961, 3965, 3970, 3974,
    3978, 3983, 3987, 3991, 3996, 4000, 4004, 4008, 4012, 4016, 4020, 4024, 4028, 4032, 4035, 4039,
    4043, 4047, 4050, 4054, 4058, 4061, 4065, 4068, 4072, 4075, 4079, 4082, 4085, 4089, 4092, 4096,
};

float se_menu_gain(int setting)
{
    if (setting <= 0)
        return 0;
    int idx = (setting > 100 ? 100 : setting) * 127 / 100;
    int v = VOLUME[idx] * 3974 >> 12; /* centre pan (0x103888f0[0x3f]) */
    int mb = (v * 10000 >> 12) - 10000;
    return mb <= -10000 ? 0 : powf(10.0f, (float)mb / 2000.0f);
}

enum
{
    VOICES = 8,
};

struct SePlayer
{
    SDL_AudioDeviceID dev;
    SDL_AudioStream* voice[VOICES];
    SDL_AudioSpec spec[VOICES];
};

SePlayer* se_open(void)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return NULL;
    SePlayer* p = calloc(1, sizeof *p);
    if (p && !(p->dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL)))
    {
        fprintf(stderr, "[se] no audio device: %s\n", SDL_GetError());
        free(p);
        p = NULL;
    }
    return p;
}

void se_play(SePlayer* p, int slot, const SeWave* w, float gain)
{
    if (!p || slot < 0 || slot >= VOICES || !w->pcm)
        return;
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, w->channels, (int)w->rate };
    SDL_AudioStream** s = &p->voice[slot];
    if (*s && memcmp(&p->spec[slot], &want, sizeof want))
    {
        SDL_DestroyAudioStream(*s);
        *s = NULL;
    }
    if (!*s)
    {
        SDL_AudioSpec out;
        SDL_GetAudioDeviceFormat(p->dev, &out, NULL);
        if (!(*s = SDL_CreateAudioStream(&want, &out)) || !SDL_BindAudioStream(p->dev, *s))
        {
            SDL_DestroyAudioStream(*s);
            *s = NULL;
            return;
        }
        p->spec[slot] = want;
    }
    SDL_ClearAudioStream(*s);
    SDL_SetAudioStreamGain(*s, gain);
    SDL_PutAudioStreamData(*s, w->pcm, (int)(w->frames * w->channels * sizeof *w->pcm));
}

void se_close(SePlayer* p)
{
    if (!p)
        return;
    for (int i = 0; i < VOICES; ++i)
        SDL_DestroyAudioStream(p->voice[i]);
    SDL_CloseAudioDevice(p->dev);
    free(p);
}
