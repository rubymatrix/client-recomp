/* Native helpers for the Windower layer (lua/windower.lua): xi.windower_native.
 *
 *   pack(format, ...) -> string          unpack(string, format[, init[, bit]]) -> values...
 *       Windower's binary packing (its own variant of lpack, which its packets, extdata and dialog
 *       libraries are written against). The layer installs them as string.pack / string.unpack
 *       and as require('pack'), so 'I':pack(n) and data:unpack('H', 3) work.
 *   regex_find_all(str, pattern) -> {{start, end, captures...}, ...}
 *       Windower's regular expressions (ECMAScript std::regex): windower_regex.cpp.
 *   clipboard_get() -> string|nil   clipboard_set(string)
 *   play_sound(path) -> bool         a .wav file on the default output, mixed with the game's sound
 *
 * Format letters (a number after a letter is a count; '*' repeats to the end of the data):
 *   <  >  =    little, big, native (little) endian for what follows
 *   b[N]       an N-bit unsigned field (default 1), least significant bit first; one value
 *   q[N]       N one-bit booleans
 *   B          a byte as a boolean
 *   c C        signed / unsigned char       h H   short        i I   int       l L   64-bit
 *   f d        float, double
 *   z          a zero-terminated string
 *   S[N]       an N-byte string, cut at its first NUL (packed: padded with NULs)
 *   A[N]       N raw bytes (unpack: default 1; pack: the whole string when no N)
 *   p P a      a string after its length as a byte, a short, an int
 *   x[N]       N padding bytes
 * Any field that isn't a bit field starts on a byte boundary. unpack returns the values only (no
 * position), and stops at the end of the data. init is a 1-based byte offset, bit a bit offset. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "build.h"
#include "host.h"

#include "lauxlib.h"
#include "lua.h"

void xi_windower_open(lua_State* L);
int xi_windower_regex_find_all(lua_State* L); /* windower_regex.cpp */

/* --- formats ---------------------------------------------------------------------------------- */

typedef struct Op
{
    char c;
    int count;   /* -1: '*' */
    int given;   /* a count was written */
} Op;

static const char* next_op(lua_State* L, const char* f, Op* op, int* big)
{
    for (;;)
    {
        while (*f == ' ' || *f == '\t' || *f == '\n' || *f == '\r')
            ++f;
        if (*f == '<' || *f == '=')
        {
            *big = 0, ++f;
            continue;
        }
        if (*f == '>')
        {
            *big = 1, ++f;
            continue;
        }
        break;
    }
    if (!*f)
        return NULL;
    op->c = *f++;
    op->count = 1;
    op->given = 0;
    if (*f == '*')
    {
        op->count = -1, op->given = 1;
        ++f;
    }
    else if (*f >= '0' && *f <= '9')
    {
        int n = 0;
        while (*f >= '0' && *f <= '9')
            n = n * 10 + (*f++ - '0');
        op->count = n, op->given = 1;
    }
    if (!strchr("bqBcChHiIlLfdzSApPax", op->c))
        luaL_error(L, "pack: invalid format option '%c'", op->c);
    return f;
}

static int width_of(char c)
{
    switch (c)
    {
    case 'B': case 'c': case 'C': return 1;
    case 'h': case 'H': return 2;
    case 'i': case 'I': case 'f': return 4;
    case 'l': case 'L': case 'd': return 8;
    default: return 0;
    }
}

/* --- unpack ----------------------------------------------------------------------------------- */

static uint64_t get_uint(const uint8_t* p, int n, int big)
{
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= (uint64_t)p[big ? n - 1 - i : i] << (8 * i);
    return v;
}

static int l_unpack(lua_State* L)
{
    size_t len;
    const uint8_t* s = (const uint8_t*)luaL_checklstring(L, 1, &len);
    const char* f = luaL_checkstring(L, 2);
    lua_Integer init = luaL_optinteger(L, 3, 1);
    lua_Integer bitinit = luaL_optinteger(L, 4, 0);
    size_t pos = init > 0 ? (size_t)(init - 1) : 0;
    unsigned bit = bitinit > 0 ? (unsigned)bitinit : 0;
    pos += bit / 8, bit %= 8;
    int big = 0, n = 0;
    Op op;
    lua_settop(L, 2);
    while ((f = next_op(L, f, &op, &big)))
    {
        luaL_checkstack(L, 8, "unpack: too many values");
        if (op.c == 'b')
        {
            /* a field wider than 64 bits (packets' bit[128]) reads as its low 64 */
            int w = op.count < 0 ? 1 : op.count;
            if ((pos * 8 + bit + (size_t)w) > len * 8)
                break;
            uint64_t v = 0;
            for (int i = 0; i < w && i < 64; ++i)
            {
                size_t at = pos * 8 + bit + (size_t)i;
                v |= (uint64_t)((s[at / 8] >> (at % 8)) & 1) << i;
            }
            bit += (unsigned)w;
            pos += bit / 8, bit %= 8;
            lua_pushnumber(L, (lua_Number)v);
            n++;
            continue;
        }
        if (op.c == 'q')
        {
            int k = op.count;
            int stop = 0;
            for (int i = 0; k < 0 || i < k; ++i)
            {
                if (pos >= len)
                {
                    stop = 1;
                    break;
                }
                lua_pushboolean(L, (s[pos] >> bit) & 1);
                n++;
                if (++bit == 8)
                    bit = 0, pos++;
                luaL_checkstack(L, 4, "unpack: too many values");
            }
            if (stop && k >= 0)
                break;
            continue;
        }
        if (bit)
            bit = 0, pos++;
        int w = width_of(op.c);
        if (w)
        {
            int stop = 0;
            for (int i = 0; op.count < 0 || i < op.count; ++i)
            {
                if (pos + (size_t)w > len)
                {
                    stop = 1;
                    break;
                }
                uint64_t u = get_uint(s + pos, w, big);
                pos += (size_t)w;
                switch (op.c)
                {
                case 'B': lua_pushboolean(L, u != 0); break;
                case 'c': lua_pushnumber(L, (int8_t)u); break;
                case 'C': lua_pushnumber(L, (uint8_t)u); break;
                case 'h': lua_pushnumber(L, (int16_t)u); break;
                case 'H': lua_pushnumber(L, (uint16_t)u); break;
                case 'i': lua_pushnumber(L, (int32_t)u); break;
                case 'I': lua_pushnumber(L, (uint32_t)u); break;
                case 'l': lua_pushnumber(L, (lua_Number)(int64_t)u); break;
                case 'L': lua_pushnumber(L, (lua_Number)u); break;
                case 'f':
                {
                    uint32_t b32 = (uint32_t)u;
                    float v;
                    memcpy(&v, &b32, 4);
                    lua_pushnumber(L, v);
                    break;
                }
                case 'd':
                {
                    double v;
                    memcpy(&v, &u, 8);
                    lua_pushnumber(L, v);
                    break;
                }
                }
                n++;
                luaL_checkstack(L, 4, "unpack: too many values");
            }
            if (stop && op.count >= 0)
                break;
            continue;
        }
        switch (op.c)
        {
        case 'x':
            pos += op.count < 0 ? len - (pos < len ? pos : len) : (size_t)op.count;
            continue;
        case 'z':
        {
            if (pos > len)
                goto done;
            size_t e = pos;
            while (e < len && s[e])
                ++e;
            lua_pushlstring(L, (const char*)s + pos, e - pos);
            n++;
            pos = e < len ? e + 1 : e;
            continue;
        }
        case 'S':
        case 'A':
        {
            size_t k = op.count < 0 ? len - (pos < len ? pos : len) : (size_t)op.count;
            if (pos + k > len)
            {
                if (pos >= len)
                    goto done;
                k = len - pos; /* a string cut short by the data: what there is */
            }
            size_t e = k;
            if (op.c == 'S')
                for (e = 0; e < k && s[pos + e]; ++e)
                    ;
            lua_pushlstring(L, (const char*)s + pos, e);
            n++;
            pos += k;
            continue;
        }
        case 'p':
        case 'P':
        case 'a':
        {
            int lw = op.c == 'p' ? 1 : op.c == 'P' ? 2 : 4;
            if (pos + (size_t)lw > len)
                goto done;
            size_t k = (size_t)get_uint(s + pos, lw, big);
            pos += (size_t)lw;
            if (pos + k > len)
                goto done;
            lua_pushlstring(L, (const char*)s + pos, k);
            n++;
            pos += k;
            continue;
        }
        }
    }
done:
    return n;
}

/* --- pack ------------------------------------------------------------------------------------- */

typedef struct Buf
{
    uint8_t* p;
    size_t n, cap;
    unsigned bit; /* bits used in the last byte (0: byte aligned) */
} Buf;

static void grow(Buf* b, size_t more)
{
    if (b->n + more <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->n + more)
        cap *= 2;
    b->p = (uint8_t*)realloc(b->p, cap);
    b->cap = cap;
}

static void put_bytes(Buf* b, const void* s, size_t n)
{
    grow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void put_zero(Buf* b, size_t n)
{
    grow(b, n);
    memset(b->p + b->n, 0, n);
    b->n += n;
}

static void put_uint(Buf* b, uint64_t v, int w, int big)
{
    uint8_t t[8];
    for (int i = 0; i < w; ++i)
        t[big ? w - 1 - i : i] = (uint8_t)(v >> (8 * i));
    put_bytes(b, t, (size_t)w);
}

static void put_bit(Buf* b, int v)
{
    if (!b->bit)
        put_zero(b, 1);
    if (v)
        b->p[b->n - 1] |= (uint8_t)(1u << b->bit);
    b->bit = (b->bit + 1) & 7;
}

static uint64_t arg_uint(lua_State* L, int i)
{
    if (lua_isboolean(L, i))
        return (uint64_t)lua_toboolean(L, i);
    lua_Number d = lua_isnumber(L, i) ? lua_tonumber(L, i) : 0;
    if (d < 0)
        return (uint64_t)(int64_t)d;
    return (uint64_t)d;
}

static int l_pack(lua_State* L)
{
    const char* f = luaL_checkstring(L, 1);
    int arg = 2, top = lua_gettop(L), big = 0;
    Buf b = { 0 };
    Op op;
    while ((f = next_op(L, f, &op, &big)))
    {
        if (op.c == 'b')
        {
            int w = op.count < 0 ? 1 : op.count;
            uint64_t v = arg_uint(L, arg++);
            for (int i = 0; i < w; ++i)
                put_bit(&b, i < 64 ? (int)((v >> i) & 1) : 0);
            continue;
        }
        if (op.c == 'q')
        {
            int k = op.count < 0 ? top - arg + 1 : op.count;
            for (int i = 0; i < k; ++i, ++arg)
                put_bit(&b, lua_isnumber(L, arg) ? lua_tonumber(L, arg) != 0 : lua_toboolean(L, arg));
            continue;
        }
        b.bit = 0;
        int w = width_of(op.c);
        if (w)
        {
            int k = op.count < 0 ? top - arg + 1 : op.count;
            for (int i = 0; i < k; ++i, ++arg)
            {
                if (op.c == 'f')
                {
                    float v = (float)(lua_isnumber(L, arg) ? lua_tonumber(L, arg) : 0);
                    uint32_t u;
                    memcpy(&u, &v, 4);
                    put_uint(&b, u, 4, big);
                }
                else if (op.c == 'd')
                {
                    double v = lua_isnumber(L, arg) ? lua_tonumber(L, arg) : 0;
                    uint64_t u;
                    memcpy(&u, &v, 8);
                    put_uint(&b, u, 8, big);
                }
                else if (op.c == 'B')
                    put_uint(&b, lua_isnumber(L, arg) ? lua_tonumber(L, arg) != 0 : lua_toboolean(L, arg), 1, big);
                else
                    put_uint(&b, arg_uint(L, arg), w, big);
            }
            continue;
        }
        if (op.c == 'x')
        {
            put_zero(&b, op.count < 0 ? 0 : (size_t)op.count);
            continue;
        }
        size_t n = 0;
        const char* s = lua_isstring(L, arg) ? lua_tolstring(L, arg, &n) : "";
        if (!lua_isstring(L, arg))
            n = 0;
        arg++;
        switch (op.c)
        {
        case 'z':
            put_bytes(&b, s, strnlen(s, n));
            put_zero(&b, 1);
            break;
        case 'S':
        case 'A':
            if (op.count < 0 || (!op.given && op.c == 'A'))
                put_bytes(&b, s, n);
            else
            {
                size_t k = (size_t)op.count;
                put_bytes(&b, s, n < k ? n : k);
                if (n < k)
                    put_zero(&b, k - n);
            }
            break;
        case 'p':
            put_uint(&b, n > 0xFF ? 0xFF : n, 1, big);
            put_bytes(&b, s, n > 0xFF ? 0xFF : n);
            break;
        case 'P':
            put_uint(&b, n > 0xFFFF ? 0xFFFF : n, 2, big);
            put_bytes(&b, s, n > 0xFFFF ? 0xFFFF : n);
            break;
        case 'a':
            put_uint(&b, n, 4, big);
            put_bytes(&b, s, n);
            break;
        }
    }
    lua_pushlstring(L, b.p ? (const char*)b.p : "", b.n);
    free(b.p);
    return 1;
}

/* --- clipboard, sound ------------------------------------------------------------------------- */

/* Under --addon-harness nothing leaves the process: the clipboard is a string of our own and sounds
 * are only logged. */
static char* g_fake_clipboard;

static int l_clipboard_get(lua_State* L)
{
    if (xi_headless)
    {
        if (!g_fake_clipboard)
            return lua_pushnil(L), 1;
        lua_pushstring(L, g_fake_clipboard);
        return 1;
    }
    if (!SDL_HasClipboardText())
        return lua_pushnil(L), 1;
    char* s = SDL_GetClipboardText();
    if (!s)
        return lua_pushnil(L), 1;
    lua_pushstring(L, s);
    SDL_free(s);
    return 1;
}

static int l_clipboard_set(lua_State* L)
{
    if (xi_headless)
    {
        free(g_fake_clipboard);
        g_fake_clipboard = strdup(luaL_checkstring(L, 1));
        return lua_pushboolean(L, 1), 1;
    }
    lua_pushboolean(L, SDL_SetClipboardText(luaL_checkstring(L, 1)));
    return 1;
}

/* Each sound is its own stream on the default device; finished ones are closed on the next call. */
static SDL_AudioStream* g_sounds[16];

static int l_play_sound(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if (xi_headless)
    {
        xi_log("play_sound (harness: not played): %s", path);
        return lua_pushboolean(L, 1), 1;
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return lua_pushboolean(L, 0), 1;
    int slot = -1;
    for (int i = 0; i < 16; ++i)
    {
        if (g_sounds[i] && SDL_GetAudioStreamQueued(g_sounds[i]) <= 0)
        {
            SDL_DestroyAudioStream(g_sounds[i]);
            g_sounds[i] = NULL;
        }
        if (!g_sounds[i] && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return lua_pushboolean(L, 0), 1;
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len))
        return lua_pushboolean(L, 0), 1;
    SDL_AudioStream* st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (st)
    {
        SDL_PutAudioStreamData(st, buf, (int)len);
        SDL_FlushAudioStream(st);
        SDL_ResumeAudioStreamDevice(st);
        g_sounds[slot] = st;
    }
    SDL_free(buf);
    lua_pushboolean(L, st != NULL);
    return 1;
}

static void set(lua_State* L, const char* name, lua_CFunction f)
{
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
}

void xi_windower_open(lua_State* L)
{
    lua_newtable(L);
    set(L, "pack", l_pack);
    set(L, "unpack", l_unpack);
    set(L, "regex_find_all", xi_windower_regex_find_all);
    set(L, "clipboard_get", l_clipboard_get);
    set(L, "clipboard_set", l_clipboard_set);
    set(L, "play_sound", l_play_sound);
    lua_pushstring(L, FFXI_VERSION); /* the game's version as patch.ver has it: get_windower_settings */
    lua_setfield(L, -2, "ffxi_version");
}
