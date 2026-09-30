/* Native helpers for the Ashita layer (host/addons/lua/ashita*.lua): xi.ashita_native.
 *
 *   regex_find(subject, pattern[, init[, icase]]) -> start, stop, { group strings } | nil
 *       One match at or after byte `init` (1-based), with ECMAScript-style patterns (Ashita uses
 *       std::regex) translated to POSIX extended: \d \w \s and their negations, escapes inside
 *       brackets, (?: ) groups (dropped from the captures), lazy quantifiers taken as greedy.
 *       Returns 1-based inclusive positions; groups[1] is the whole match. Errors on a bad pattern.
 *   clipboard_get() -> string | nil          clipboard_set(text) -> boolean
 *   play_sound(path) -> boolean               a WAV file through SDL's default playback device
 *
 * Self-contained: nothing else in the host calls these. */
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"

#include "lauxlib.h"
#include "lua.h"

/* --- regex ------------------------------------------------------------------------------------ */

typedef struct Buf
{
    char* p;
    size_t n, cap;
} Buf;

static void put(Buf* b, const char* s, size_t n)
{
    if (b->n + n + 1 > b->cap)
    {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->p = (char*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf* b, const char* s) { put(b, s, strlen(s)); }

/* ECMAScript -> POSIX ERE. keep[i] = 1 when POSIX group i+1 is a capture in the original. */
static int translate(const char* re, Buf* out, unsigned char* keep, int max_groups, int* ngroups)
{
    int in_class = 0, groups = 0;
    for (const char* p = re; *p; ++p)
    {
        char c = *p;
        if (c == '\\' && p[1])
        {
            char e = *++p;
            const char* cls = NULL;
            switch (e)
            {
            case 'd': cls = in_class ? "0-9" : "[0-9]"; break;
            case 'D': cls = in_class ? NULL : "[^0-9]"; break;
            case 'w': cls = in_class ? "[:alnum:]_" : "[[:alnum:]_]"; break;
            case 'W': cls = in_class ? NULL : "[^[:alnum:]_]"; break;
            case 's': cls = in_class ? "[:space:]" : "[[:space:]]"; break;
            case 'S': cls = in_class ? NULL : "[^[:space:]]"; break;
            case 't': cls = "\t"; break;
            case 'n': cls = "\n"; break;
            case 'r': cls = "\r"; break;
            default: break;
            }
            if (cls)
            {
                puts_(out, cls);
                continue;
            }
            if (in_class)
            {
                /* brackets: a backslash is literal in POSIX; ECMAScript escapes mean the character */
                if (e == ']' || e == '\\' || e == '^' || e == '-')
                {
                    /* move these to safe places is hard in general: use a class of their own */
                    if (e == ']')
                        puts_(out, "[.].]");
                    else if (e == '\\')
                        puts_(out, "\\");
                    else if (e == '-')
                        puts_(out, "[.-.]");
                    else
                        puts_(out, "[.^.]");
                }
                else
                    put(out, &e, 1);
                continue;
            }
            if (e == 'b' || e == 'B' || e == '<' || e == '>')
            {
#if defined(__APPLE__) || defined(__GLIBC__)
                char two[2] = { '\\', e };
                put(out, two, 2);
#endif
                continue;
            }
            if (isalnum((unsigned char)e))
            {
                /* \1 back references and anything else alphanumeric: as it is */
                char two[2] = { '\\', e };
                put(out, two, 2);
                continue;
            }
            char two[2] = { '\\', e };
            put(out, two, 2);
            continue;
        }
        if (in_class)
        {
            if (c == ']')
                in_class = 0;
            put(out, &c, 1);
            continue;
        }
        if (c == '[')
        {
            in_class = 1;
            put(out, &c, 1);
            if (p[1] == '^')
                put(out, ++p, 1);
            if (p[1] == ']')
                put(out, ++p, 1);
            continue;
        }
        if (c == '(')
        {
            int capture = 1;
            if (p[1] == '?')
            {
                if (p[2] == ':')
                    p += 2, capture = 0;
                else
                    return 0; /* lookarounds: no POSIX equivalent */
            }
            if (groups < max_groups)
                keep[groups] = (unsigned char)capture;
            groups++;
            put(out, "(", 1);
            continue;
        }
        if (c == '{')
        {
            /* POSIX caps bounds at RE_DUP_MAX (255): a larger one becomes open-ended */
            unsigned lo = 0, hi = 0;
            int used = 0, comma = 0;
            if (sscanf(p, "{%u,%u}%n", &lo, &hi, &used) == 2 && used)
                comma = 1;
            else if (sscanf(p, "{%u}%n", &lo, &used) == 1 && used)
                hi = lo;
            else
                used = 0;
            if (used && (lo > 255 || hi > 255))
            {
                char q[32];
                if (lo > 255)
                    lo = 255;
                snprintf(q, sizeof q, comma || hi != lo ? "{%u,}" : "{%u}", lo);
                puts_(out, q);
                p += used - 1;
                continue;
            }
        }
        if ((c == '?' || c == '+') && out->n && (p[-1] == '*' || p[-1] == '+' || p[-1] == '?' || p[-1] == '}') &&
            !(p - 1 > re && p[-2] == '\\'))
            continue; /* lazy / possessive: greedy here */
        put(out, &c, 1);
    }
    *ngroups = groups;
    return !in_class;
}

#define MAX_GROUPS 32

typedef struct Compiled
{
    regex_t re;
    unsigned char keep[MAX_GROUPS];
    int groups;
} Compiled;

static int compile(lua_State* L, const char* pattern, int icase, Compiled* c)
{
    Buf b = { 0 };
    puts_(&b, "");
    memset(c, 0, sizeof *c);
    if (!translate(pattern, &b, c->keep, MAX_GROUPS, &c->groups))
    {
        free(b.p);
        return luaL_error(L, "regex: unsupported pattern: %s", pattern);
    }
    int flags = REG_EXTENDED | (icase ? REG_ICASE : 0);
#if defined(REG_ENHANCED)
    flags |= REG_ENHANCED;
#endif
    int r = regcomp(&c->re, b.p, flags);
    free(b.p);
    if (r)
    {
        char msg[200];
        regerror(r, &c->re, msg, sizeof msg);
        return luaL_error(L, "regex: %s: %s", msg, pattern);
    }
    return 0;
}

static int n_regex_find(lua_State* L)
{
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    const char* pattern = luaL_checkstring(L, 2);
    size_t init = (size_t)luaL_optnumber(L, 3, 1);
    int icase = lua_toboolean(L, 4);
    if (init < 1)
        init = 1;
    if (init > n + 1)
        return lua_pushnil(L), 1;
    Compiled c;
    compile(L, pattern, icase, &c);
    regmatch_t m[MAX_GROUPS + 1];
    int eflags = init > 1 ? REG_NOTBOL : 0;
#if defined(REG_STARTEND)
    m[0].rm_so = (regoff_t)(init - 1);
    m[0].rm_eo = (regoff_t)n;
    int r = regexec(&c.re, s, MAX_GROUPS + 1, m, eflags | REG_STARTEND);
    size_t base = 0;
#else
    int r = regexec(&c.re, s + init - 1, MAX_GROUPS + 1, m, eflags);
    size_t base = init - 1;
#endif
    if (r)
    {
        regfree(&c.re);
        return lua_pushnil(L), 1;
    }
    lua_pushnumber(L, (double)(base + (size_t)m[0].rm_so + 1));
    lua_pushnumber(L, (double)(base + (size_t)m[0].rm_eo));
    lua_newtable(L);
    int k = 1;
    lua_pushlstring(L, s + base + m[0].rm_so, (size_t)(m[0].rm_eo - m[0].rm_so));
    lua_rawseti(L, -2, k++);
    for (int g = 1; g <= c.groups && g <= MAX_GROUPS; ++g)
    {
        if (!c.keep[g - 1])
            continue;
        if (m[g].rm_so < 0)
            lua_pushstring(L, "");
        else
            lua_pushlstring(L, s + base + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
        lua_rawseti(L, -2, k++);
    }
    regfree(&c.re);
    return 3;
}

/* --- clipboard, sound ------------------------------------------------------------------------- */

static int n_clipboard_get(lua_State* L)
{
    if (xi_headless || !SDL_HasClipboardText())
        return lua_pushnil(L), 1;
    char* t = SDL_GetClipboardText();
    if (!t)
        return lua_pushnil(L), 1;
    lua_pushstring(L, t);
    SDL_free(t);
    return 1;
}

static int n_clipboard_set(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    lua_pushboolean(L, !xi_headless && SDL_SetClipboardText(s));
    return 1;
}

static SDL_AudioStream* g_sounds[16];

static int n_play_sound(lua_State* L)
{
    char host[1200];
    xi_host_path(luaL_checkstring(L, 1), host, sizeof host);
    if (xi_headless)
        return lua_pushboolean(L, 0), 1;
    /* streams that finished playing */
    for (unsigned i = 0; i < sizeof g_sounds / sizeof *g_sounds; ++i)
        if (g_sounds[i] && SDL_GetAudioStreamQueued(g_sounds[i]) <= 0)
            SDL_DestroyAudioStream(g_sounds[i]), g_sounds[i] = NULL;
    unsigned slot = 0;
    while (slot < sizeof g_sounds / sizeof *g_sounds && g_sounds[slot])
        ++slot;
    if (slot == sizeof g_sounds / sizeof *g_sounds)
        return lua_pushboolean(L, 0), 1;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return lua_pushboolean(L, 0), 1;
    SDL_AudioSpec spec;
    Uint8* data = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(host, &spec, &data, &len))
    {
        xi_log_once(host, "play_sound: cannot load %s: %s", host, SDL_GetError());
        return lua_pushboolean(L, 0), 1;
    }
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s)
    {
        SDL_PutAudioStreamData(s, data, (int)len);
        SDL_FlushAudioStream(s);
        SDL_ResumeAudioStreamDevice(s);
        g_sounds[slot] = s;
    }
    SDL_free(data);
    lua_pushboolean(L, s != NULL);
    return 1;
}

static const luaL_Reg FNS[] = {
    { "regex_find", n_regex_find },
    { "clipboard_get", n_clipboard_get },
    { "clipboard_set", n_clipboard_set },
    { "play_sound", n_play_sound },
    { NULL, NULL },
};

void xi_ashita_open(lua_State* L)
{
    lua_newtable(L);
    luaL_register(L, NULL, FNS);
}
