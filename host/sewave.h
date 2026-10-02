/* The game's sound effects ("SeWave", sound/win/se/se<id/1000>/se<id>.spw), read and played by
 * host64's own screens (sign-in) with SDL, before the game runs.
 *
 * A file (little-endian): "SeWave\0\0", u32 file size, u32 codec (0 ADPCM, 1 PCM16), u32 id, u32
 * length (ADPCM frames a channel, or PCM sample frames), u32 loop start (0xFFFFFFFF: none), u32 a,
 * u32 b (the rate is (a + b) & 0x7fffffff), u32 data offset, u8 volume index, u8 flags, u8
 * channels, u8 bits, u8 samples an ADPCM frame (16). An ADPCM frame, a channel's in turn: a byte
 * of filter (high nibble) and shift (low), then the samples, 4 bits each, low nibble first:
 * s = (nibble << 12 as int16 >> shift) + (c0 * s1 + c1 * s2 >> 6), the PS2's filters. */
#pragma once

#include <stdint.h>

typedef struct SeWave
{
    int16_t* pcm; /* interleaved */
    uint32_t frames, rate;
    int channels;
} SeWave;

/* Effect id's file under the install's folder, decoded. 0 when it cannot be read. */
int sewave_load(const char* game_dir, uint32_t id, SeWave* out);
void sewave_free(SeWave* w);

/* The gain the game gives a menu's sound at sound-effect volume setting (0..100): its index
 * setting * 127 / 100 into the volume table, at centre pan, to DirectSound's hundredths of a
 * decibel (FFXiMain 2026-09-03: 0x10038330, 0x10248900). 0 is silence; 100 is -2.98 dB. */
float se_menu_gain(int setting);

/* Playing: a voice an effect, each restarting when played again. */
typedef struct SePlayer SePlayer;
SePlayer* se_open(void); /* NULL when there is no audio device */
/* Plays w (kept by the caller while the player is open) on voice slot (0..7) at gain (1 = as
 * recorded). */
void se_play(SePlayer* p, int slot, const SeWave* w, float gain);
void se_close(SePlayer* p);
