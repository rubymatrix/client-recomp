/* Windower's regular expressions (windower.regex.*): ECMAScript std::regex, as Windower's are.
 * Registered by windower_native.c into xi.windower_native:
 *
 *   regex_find_all(str, pattern) -> { {start, end, capture1, ...}, ... }   (1-based, inclusive;
 *       a capture that didn't take part is false)
 *
 * Patterns are compiled once and cached. A bad pattern raises a Lua error with std::regex's text;
 * no C++ object is alive when it does. */
#include <cstdio>
#include <map>
#include <regex>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

static char g_err[300];

/* NULL (g_err set) when the pattern doesn't compile. */
static const std::regex* compile(const char* pattern)
{
    static std::map<std::string, std::regex> cache;
    try
    {
        auto it = cache.find(pattern);
        if (it != cache.end())
            return &it->second;
        if (cache.size() > 256)
            cache.clear();
        return &cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first->second;
    }
    catch (const std::exception& e)
    {
        snprintf(g_err, sizeof g_err, "regex: %s: %s", pattern, e.what());
        return nullptr;
    }
}

/* Pushes the matches table; 0 on failure (g_err set). */
static int find_all(lua_State* L, const char* s, size_t len, const std::regex* re)
{
    try
    {
        lua_newtable(L);
        int n = 0;
        for (std::cregex_iterator it(s, s + len, *re), end; it != end; ++it)
        {
            const std::cmatch& m = *it;
            lua_newtable(L);
            lua_pushnumber(L, (lua_Number)m.position(0) + 1);
            lua_rawseti(L, -2, 1);
            lua_pushnumber(L, (lua_Number)(m.position(0) + m.length(0)));
            lua_rawseti(L, -2, 2);
            for (size_t i = 1; i < m.size(); ++i)
            {
                if (m[i].matched)
                    lua_pushlstring(L, s + m.position(i), (size_t)m.length(i));
                else
                    lua_pushboolean(L, 0);
                lua_rawseti(L, -2, (int)i + 2);
            }
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    catch (const std::exception& e)
    {
        snprintf(g_err, sizeof g_err, "regex: %s", e.what());
        return 0;
    }
}

extern "C" int xi_windower_regex_find_all(lua_State* L)
{
    size_t len;
    const char* s = luaL_checklstring(L, 1, &len);
    const std::regex* re = compile(luaL_checkstring(L, 2));
    if (!re || !find_all(L, s, len, re))
        return luaL_error(L, "%s", g_err);
    return 1;
}
