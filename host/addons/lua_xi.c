/* The native xi table: the primitives every addon API (ours, Ashita's, Windower's) is built on.
 *
 *   xi.memory  find, read_* / write_*, alloc / free, module: addresses in and out are the host
 *              addresses Lua can ffi.cast (a number below 2^32 is taken as a guest address)
 *   xi.chat    write (to the chat log), run (a line as if typed, next frame), input line
 *   xi.ui      text objects, primitives, textures, screen size; the ImGui manager (Ashita's)
 *   xi.packets inject, last
 *   xi.input   binds, key state
 *   xi.addons  load, unload, reload, list
 *   xi.fs      host paths, listing, folders
 *   xi.game    the game's data (game_lua.c), xi.res its resources (res_lua.c)
 *   xi.log, xi.clock, xi.time, xi.paths, xi.sjis_to_utf8 / utf8_to_sjis, xi.open_url */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "build.h"
#include "d3d8.h"
#include "gthread.h"
#include "gwin.h"
#include "host.h"
#include "res.h"
#include "plat.h"

#include "lauxlib.h"
#include "lua.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern int xi_game_open(lua_State* L);
extern int xi_res_open(lua_State* L);
extern void xi_gui_lua_push_manager(lua_State* L);
extern void xi_d3d_ffi_open(lua_State* L);

extern uint32_t xi_text_new(void);
extern void xi_text_delete(uint32_t id);
extern int xi_text_set(uint32_t id, const char* f, double n, const char* s);
extern int xi_text_get(uint32_t id, const char* f, double* n, const char** s);
extern uint32_t xi_prim_new(void);
extern void xi_prim_delete(uint32_t id);
extern int xi_prim_set(uint32_t id, const char* f, double n);
extern int xi_prim_get(uint32_t id, const char* f, double* n);
extern uint32_t xi_gui_texture_file(const char* path, int* w, int* h);
extern uint32_t xi_gui_texture_memory(const uint8_t* data, size_t n, int w, int h, int bgra, int* ow, int* oh);
extern void xi_gui_texture_free(uint32_t id);
extern int xi_gui_texture_size(uint32_t id, int* w, int* h);

static Addon* addon_of(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "xi.addon");
    Addon* a = (Addon*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return a;
}

/* --- addresses -------------------------------------------------------------------------------- */

static uint32_t arg_addr(lua_State* L, int i)
{
    if (lua_type(L, i) == 10 /* LUA_TCDATA: an ffi pointer */)
    {
        const void* p = lua_topointer(L, i);
        return xi_guest_addr((double)(uintptr_t)p, NULL);
    }
    int ok;
    uint32_t a = xi_guest_addr(luaL_checknumber(L, i), &ok);
    return ok ? a : 0;
}

static void push_addr(lua_State* L, uint32_t guest) { lua_pushnumber(L, xi_host_addr(guest)); }

/* Writes into FFXiMain's code change nothing by themselves (the recompiled C is what runs): each is
 * reported as a code patch (patch.c), which a translated variant may stand for. Called before the
 * bytes are written. */
static void code_write(lua_State* L, uint32_t a, const void* src, uint32_t n)
{
    Addon* ad = addon_of(L);
    xi_code_patch(ad, a, (const uint8_t*)src, n);
}

/* --- xi.memory -------------------------------------------------------------------------------- */

static int m_find(lua_State* L)
{
    /* find(module_or_address, size, pattern, offset, count) */
    uint32_t start = 0, size = 0;
    if (lua_type(L, 1) == LUA_TSTRING)
    {
        const char* mod = lua_tostring(L, 1);
        if (!strcasecmp(mod, "FFXiMain.dll") || !strcasecmp(mod, "FFXiMain"))
            xi_image(NULL, NULL, &start, &size);
        else if (!strcasecmp(mod, "FFXi.dll"))
            start = 0x0F000000u, size = 0x100000u;
        else
        {
            lua_pushnumber(L, 0);
            return 1;
        }
        if (lua_isnumber(L, 2) && lua_tonumber(L, 2) > 0)
            size = (uint32_t)lua_tonumber(L, 2);
    }
    else if (lua_isnumber(L, 1) && lua_tonumber(L, 1) == 0 && lua_tonumber(L, 2) == 0)
        xi_image(NULL, NULL, &start, &size); /* Ashita's find(0, 0, ...): FFXiMain */
    else
    {
        start = arg_addr(L, 1);
        size = (uint32_t)luaL_checknumber(L, 2);
    }
    const char* pattern = luaL_checkstring(L, 3);
    int32_t offset = (int32_t)luaL_optnumber(L, 4, 0);
    uint32_t count = (uint32_t)luaL_optnumber(L, 5, 0);
    uint32_t a = xi_find_pattern(start, size, pattern, offset, count);
    push_addr(L, a);
    return 1;
}

#define READ(name, T, rd)                                                                                              \
    static int m_read_##name(lua_State* L)                                                                             \
    {                                                                                                                  \
        uint32_t a = arg_addr(L, 1);                                                                                   \
        if (!xi_mapped(a, sizeof(T)))                                                                                  \
            return lua_pushnumber(L, 0), 1;                                                                            \
        T v;                                                                                                           \
        memcpy(&v, GUEST_PTR(a), sizeof v);                                                                            \
        lua_pushnumber(L, (lua_Number)v);                                                                              \
        return 1;                                                                                                      \
    }
READ(int8, int8_t, rd8)
READ(uint8, uint8_t, rd8)
READ(int16, int16_t, rd16)
READ(uint16, uint16_t, rd16)
READ(int32, int32_t, rd32)
READ(uint32, uint32_t, rd32)
READ(int64, int64_t, rd64)
READ(uint64, uint64_t, rd64)
READ(float, float, rdf32)
READ(double, double, rdf64)

#define WRITE(name, T)                                                                                                 \
    static int m_write_##name(lua_State* L)                                                                            \
    {                                                                                                                  \
        uint32_t a = arg_addr(L, 1);                                                                                   \
        T v = (T)luaL_checknumber(L, 2);                                                                               \
        if (!xi_mapped(a, sizeof(T)))                                                                                  \
            return lua_pushboolean(L, 0), 1;                                                                           \
        code_write(L, a, &v, sizeof(T));                                                                               \
        memcpy(GUEST_PTR(a), &v, sizeof v);                                                                            \
        lua_pushboolean(L, 1);                                                                                         \
        return 1;                                                                                                      \
    }
WRITE(int8, int8_t)
WRITE(uint8, uint8_t)
WRITE(int16, int16_t)
WRITE(uint16, uint16_t)
WRITE(int32, int32_t)
WRITE(uint32, uint32_t)
WRITE(int64, int64_t)
WRITE(uint64, uint64_t)
WRITE(float, float)
WRITE(double, double)

static int m_read_string(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    uint32_t max = (uint32_t)luaL_optnumber(L, 2, 0x10000);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (uint32_t i = 0; i < max && xi_mapped(a + i, 1); ++i)
    {
        char c = (char)rd8(a + i);
        if (!c && !lua_toboolean(L, 3))
            break;
        luaL_addchar(&b, c);
    }
    luaL_pushresult(&b);
    return 1;
}

static int m_write_string(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    size_t n;
    const char* s = luaL_checklstring(L, 2, &n);
    int terminate = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    if (!xi_mapped(a, (uint32_t)n + (terminate ? 1 : 0)))
        return lua_pushboolean(L, 0), 1;
    code_write(L, a, s, (uint32_t)n);
    memcpy(GUEST_PTR(a), s, n);
    if (terminate)
        wr8(a + (uint32_t)n, 0);
    lua_pushboolean(L, 1);
    return 1;
}

/* read_array(address, count) -> table of bytes (1-based) */
static int m_read_array(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    uint32_t n = (uint32_t)luaL_checknumber(L, 2);
    lua_createtable(L, (int)n, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        lua_pushnumber(L, xi_mapped(a + i, 1) ? rd8(a + i) : 0);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

/* write_array(address, table_of_bytes | string) */
static int m_write_array(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING)
    {
        size_t n;
        const char* s = lua_tolstring(L, 2, &n);
        if (!xi_mapped(a, (uint32_t)n))
            return lua_pushboolean(L, 0), 1;
        code_write(L, a, s, (uint32_t)n);
        memcpy(GUEST_PTR(a), s, n);
        return lua_pushboolean(L, 1), 1;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    uint32_t n = (uint32_t)lua_objlen(L, 2);
    if (!xi_mapped(a, n))
        return lua_pushboolean(L, 0), 1;
    uint8_t* bytes = (uint8_t*)malloc(n ? n : 1);
    for (uint32_t i = 0; i < n; ++i)
    {
        lua_rawgeti(L, 2, (int)i + 1);
        bytes[i] = (uint8_t)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    code_write(L, a, bytes, n);
    memcpy(GUEST_PTR(a), bytes, n);
    free(bytes);
    return lua_pushboolean(L, 1), 1;
}

/* read_bytes(address, n) -> string */
static int m_read_bytes(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    uint32_t n = (uint32_t)luaL_checknumber(L, 2);
    if (!xi_mapped(a, n))
        return lua_pushnil(L), 1;
    lua_pushlstring(L, (const char*)GUEST_PTR(a), n);
    return 1;
}

static int m_alloc(lua_State* L)
{
    uint32_t n = (uint32_t)luaL_checknumber(L, 1);
    uint32_t a = gheap_alloc(n ? n : 1, 1);
    push_addr(L, a);
    return 1;
}

static int m_free(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    if (a && gheap_size(a) != ~0u)
        gheap_free(a);
    return 0;
}

/* protect/unprotect: guest memory is always writable; Windows' old protection comes back */
static int m_protect(lua_State* L)
{
    lua_pushboolean(L, 1);
    lua_pushnumber(L, 0x40);
    return 2;
}

static int m_module(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    uint32_t base = 0, size = 0;
    if (!strcasecmp(name, "FFXiMain.dll") || !strcasecmp(name, "FFXiMain"))
        xi_image(&base, &size, NULL, NULL);
    else if (!strcasecmp(name, "FFXi.dll"))
        base = 0x0F000000u, size = 0x100000u;
    if (!base)
        return lua_pushnumber(L, 0), lua_pushnumber(L, 0), 2;
    push_addr(L, base);
    lua_pushnumber(L, size);
    return 2;
}

static int m_guest(lua_State* L)
{
    lua_pushnumber(L, arg_addr(L, 1));
    return 1;
}

static int m_host(lua_State* L)
{
    push_addr(L, (uint32_t)luaL_checknumber(L, 1));
    return 1;
}

static int m_base(lua_State* L)
{
    lua_pushnumber(L, (lua_Number)(uintptr_t)rt_guest_base);
    return 1;
}

static const luaL_Reg MEMORY[] = {
    { "find", m_find }, { "read_int8", m_read_int8 }, { "read_uint8", m_read_uint8 }, { "read_int16", m_read_int16 },
    { "read_uint16", m_read_uint16 }, { "read_int32", m_read_int32 }, { "read_uint32", m_read_uint32 },
    { "read_int64", m_read_int64 }, { "read_uint64", m_read_uint64 }, { "read_float", m_read_float },
    { "read_double", m_read_double }, { "read_string", m_read_string }, { "read_array", m_read_array },
    { "read_bytes", m_read_bytes }, { "write_int8", m_write_int8 }, { "write_uint8", m_write_uint8 },
    { "write_int16", m_write_int16 }, { "write_uint16", m_write_uint16 }, { "write_int32", m_write_int32 },
    { "write_uint32", m_write_uint32 }, { "write_int64", m_write_int64 }, { "write_uint64", m_write_uint64 },
    { "write_float", m_write_float }, { "write_double", m_write_double }, { "write_string", m_write_string },
    { "write_array", m_write_array }, { "alloc", m_alloc }, { "free", m_free }, { "protect", m_protect },
    { "unprotect", m_protect }, { "module", m_module }, { "guest", m_guest }, { "host", m_host }, { "base", m_base },
    { NULL, NULL },
};

/* --- xi.chat ---------------------------------------------------------------------------------- */

static int c_write(lua_State* L)
{
    /* write(text, mode) */
    const char* s = luaL_checkstring(L, 1);
    int mode = (int)luaL_optnumber(L, 2, 1);
    xi_chat_write(mode, s);
    return 0;
}

static int c_run(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    int mode = (int)luaL_optnumber(L, 2, 1);
    xi_chat_queue(mode, s);
    return 0;
}

static int c_input(lua_State* L)
{
    char buf[512];
    xi_chat_input(buf, sizeof buf);
    lua_pushstring(L, buf);
    lua_pushboolean(L, xi_chat_input_open());
    return 2;
}

static int c_set_input(lua_State* L)
{
    xi_chat_set_input(luaL_checkstring(L, 1));
    return 0;
}

static int c_exec(lua_State* L)
{
    lua_pushboolean(L, xi_exec_script(luaL_checkstring(L, 1), (int)luaL_optnumber(L, 2, -1)));
    return 1;
}

static int c_alias(lua_State* L)
{
    xi_alias_set(luaL_checkstring(L, 1), luaL_optstring(L, 2, NULL));
    return 0;
}

static const luaL_Reg CHAT[] = {
    { "write", c_write }, { "run", c_run }, { "input", c_input }, { "set_input", c_set_input }, { "exec", c_exec },
    { "alias", c_alias }, { NULL, NULL },
};

/* --- xi.ui ------------------------------------------------------------------------------------ */

static int u_text_new(lua_State* L)
{
    lua_pushnumber(L, xi_text_new());
    return 1;
}
static int u_text_delete(lua_State* L)
{
    xi_text_delete((uint32_t)luaL_checknumber(L, 1));
    return 0;
}
static int u_text_set(lua_State* L)
{
    uint32_t id = (uint32_t)luaL_checknumber(L, 1);
    const char* f = luaL_checkstring(L, 2);
    int ok = lua_type(L, 3) == LUA_TSTRING ? xi_text_set(id, f, 0, lua_tostring(L, 3))
                                           : xi_text_set(id, f, lua_isboolean(L, 3) ? lua_toboolean(L, 3) : lua_tonumber(L, 3), NULL);
    lua_pushboolean(L, ok);
    return 1;
}
static int u_text_get(lua_State* L)
{
    double n = 0;
    const char* s = NULL;
    if (!xi_text_get((uint32_t)luaL_checknumber(L, 1), luaL_checkstring(L, 2), &n, &s))
        return lua_pushnil(L), 1;
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnumber(L, n);
    return 1;
}
static int u_prim_new(lua_State* L)
{
    lua_pushnumber(L, xi_prim_new());
    return 1;
}
static int u_prim_delete(lua_State* L)
{
    xi_prim_delete((uint32_t)luaL_checknumber(L, 1));
    return 0;
}
static int u_prim_set(lua_State* L)
{
    lua_pushboolean(L, xi_prim_set((uint32_t)luaL_checknumber(L, 1), luaL_checkstring(L, 2),
                           lua_isboolean(L, 3) ? lua_toboolean(L, 3) : lua_tonumber(L, 3)));
    return 1;
}
static int u_prim_get(lua_State* L)
{
    double n = 0;
    if (!xi_prim_get((uint32_t)luaL_checknumber(L, 1), luaL_checkstring(L, 2), &n))
        return lua_pushnil(L), 1;
    lua_pushnumber(L, n);
    return 1;
}
static int u_texture_file(lua_State* L)
{
    int w = 0, h = 0;
    uint32_t id = xi_gui_texture_file(luaL_checkstring(L, 1), &w, &h);
    if (!id)
        return lua_pushnil(L), 1;
    lua_pushnumber(L, id);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 3;
}
static int u_texture_memory(lua_State* L)
{
    /* texture_memory(bytes[, width, height, bgra]): a file's bytes, or raw 32-bit pixels */
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    int w = (int)luaL_optnumber(L, 2, 0), h = (int)luaL_optnumber(L, 3, 0), ow = 0, oh = 0;
    uint32_t id = xi_gui_texture_memory((const uint8_t*)s, n, w, h, lua_toboolean(L, 4), &ow, &oh);
    if (!id)
        return lua_pushnil(L), 1;
    lua_pushnumber(L, id);
    lua_pushnumber(L, ow);
    lua_pushnumber(L, oh);
    return 3;
}
static int u_texture_free(lua_State* L)
{
    xi_gui_texture_free((uint32_t)luaL_checknumber(L, 1));
    return 0;
}
static int u_texture_size(lua_State* L)
{
    int w, h;
    if (!xi_gui_texture_size((uint32_t)luaL_checknumber(L, 1), &w, &h))
        return 0;
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}
static int u_screen(lua_State* L)
{
    uint32_t w = 0, h = 0;
    d3d8_backbuffer_size(&w, &h);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}
static int u_wants(lua_State* L)
{
    lua_pushboolean(L, xi_gui_wants_mouse());
    lua_pushboolean(L, xi_gui_wants_keyboard());
    return 2;
}
static int u_in_frame(lua_State* L)
{
    lua_pushboolean(L, xi_gui_in_frame());
    return 1;
}
static int u_imgui(lua_State* L)
{
    xi_gui_lua_push_manager(L);
    return 1;
}

static const luaL_Reg UI[] = {
    { "text_new", u_text_new }, { "text_delete", u_text_delete }, { "text_set", u_text_set }, { "text_get", u_text_get },
    { "prim_new", u_prim_new }, { "prim_delete", u_prim_delete }, { "prim_set", u_prim_set }, { "prim_get", u_prim_get },
    { "texture_file", u_texture_file }, { "texture_memory", u_texture_memory }, { "texture_free", u_texture_free },
    { "texture_size", u_texture_size }, { "screen", u_screen }, { "wants", u_wants }, { "in_frame", u_in_frame },
    { "imgui", u_imgui }, { NULL, NULL },
};

/* --- xi.packets ------------------------------------------------------------------------------- */

static int p_inject(lua_State* L)
{
    /* inject(outgoing, bytes) */
    int out = lua_toboolean(L, 1);
    size_t n;
    const char* s = luaL_checklstring(L, 2, &n);
    xi_packet_inject(out, (const uint8_t*)s, n);
    return 0;
}

static int p_last(lua_State* L)
{
    uint8_t buf[0x200];
    uint64_t when;
    size_t n = xi_packet_last(lua_toboolean(L, 1), (uint16_t)luaL_checknumber(L, 2), buf, sizeof buf, &when);
    if (!n)
        return lua_pushnil(L), 1;
    lua_pushlstring(L, (const char*)buf, n);
    lua_pushnumber(L, (double)when / 1000.0);
    return 2;
}

static const luaL_Reg PACKETS[] = { { "inject", p_inject }, { "last", p_last }, { NULL, NULL } };

/* --- xi.input --------------------------------------------------------------------------------- */

static int i_bind(lua_State* L)
{
    lua_pushboolean(L, xi_bind(luaL_checkstring(L, 1), luaL_checkstring(L, 2), lua_toboolean(L, 3) ? 2 : 1));
    return 1;
}
static int i_unbind(lua_State* L)
{
    lua_pushboolean(L, xi_unbind(lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1)));
    return 1;
}
static int i_key_down(lua_State* L)
{
    lua_pushboolean(L, xi_key_down((uint32_t)luaL_checknumber(L, 1)));
    return 1;
}
static const luaL_Reg INPUT[] = { { "bind", i_bind }, { "unbind", i_unbind }, { "key_down", i_key_down }, { NULL, NULL } };

/* --- xi.addons -------------------------------------------------------------------------------- */

static int a_load(lua_State* L)
{
    const char* k = luaL_optstring(L, 2, NULL);
    int kind = -1;
    for (int i = 0; k && i < XI_KINDS; ++i)
        if (!strcasecmp(k, xi_kind_name[i]))
            kind = i;
    /* next frame: a load from inside Lua would run the new addon nested in this one */
    char line[300];
    snprintf(line, sizeof line, "//addon load %s%s%s", luaL_checkstring(L, 1), kind >= 0 ? " " : "", kind >= 0 ? xi_kind_name[kind] : "");
    xi_chat_queue(1, line);
    return 0;
}
static int a_unload(lua_State* L)
{
    Addon* a = xi_addon_find(luaL_checkstring(L, 1));
    if (a)
        xi_addon_unload(a);
    lua_pushboolean(L, a != NULL);
    return 1;
}
static int a_reload(lua_State* L)
{
    Addon* a = xi_addon_find(luaL_checkstring(L, 1));
    if (a)
        xi_addon_reload(a);
    lua_pushboolean(L, a != NULL);
    return 1;
}
static int a_list(lua_State* L)
{
    lua_createtable(L, (int)xi_addon_count(), 0);
    for (unsigned i = 0; i < xi_addon_count(); ++i)
    {
        Addon* a = xi_addon_at(i);
        lua_createtable(L, 0, 3);
        lua_pushstring(L, a->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, xi_kind_name[a->kind]);
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, a->dir);
        lua_setfield(L, -2, "path");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
static int a_loaded(lua_State* L)
{
    lua_pushboolean(L, xi_addon_find(luaL_checkstring(L, 1)) != NULL);
    return 1;
}

/* send(name, ...): a value to another addon's 'message' event (Windower's IPC between addons,
 * Ashita's plugin events): strings only, delivered next frame. */
static int a_send(lua_State* L)
{
    const char* to = luaL_optstring(L, 1, "");
    size_t n;
    const char* s = luaL_checklstring(L, 2, &n);
    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = "message";
    e.data = (const uint8_t*)s, e.size = n;
    e.chunk = (const uint8_t*)to, e.chunk_size = strlen(to);
    xi_raise(&e);
    return 0;
}

static const luaL_Reg ADDONS[] = { { "load", a_load }, { "unload", a_unload }, { "reload", a_reload },
    { "list", a_list }, { "loaded", a_loaded }, { "send", a_send }, { NULL, NULL } };

/* --- xi.fs ------------------------------------------------------------------------------------ */

static int f_path(lua_State* L)
{
    char out[1200];
    lua_pushstring(L, xi_host_path(luaL_checkstring(L, 1), out, sizeof out));
    return 1;
}

static int f_exists(lua_State* L)
{
    char out[1200];
    PlatStat st;
    lua_pushboolean(L, plat_stat(xi_host_path(luaL_checkstring(L, 1), out, sizeof out), &st));
    return 1;
}

static int f_is_dir(lua_State* L)
{
    char out[1200];
#if !defined(_WIN32)
    struct stat st;
    lua_pushboolean(L, !stat(xi_host_path(luaL_checkstring(L, 1), out, sizeof out), &st) && S_ISDIR(st.st_mode));
#else
    DWORD a = GetFileAttributesA(xi_host_path(luaL_checkstring(L, 1), out, sizeof out));
    lua_pushboolean(L, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
#endif
    return 1;
}

static int f_mkdir(lua_State* L)
{
    char out[1200];
    xi_host_path(luaL_checkstring(L, 1), out, sizeof out);
    int ok = 1;
    for (char* c = out + 1; *c; ++c)
        if (*c == '/' || *c == '\\')
        {
            char k = *c;
            *c = 0;
            plat_mkdir(out);
            *c = k;
        }
    PlatStat st;
    if (!plat_stat(out, &st))
        ok = plat_mkdir(out);
    lua_pushboolean(L, ok);
    return 1;
}

static int f_list(lua_State* L)
{
    /* list(dir[, dirs_only]) -> names */
    char out[1200];
    xi_host_path(luaL_checkstring(L, 1), out, sizeof out);
    int dirs = lua_toboolean(L, 2);
    lua_newtable(L);
    int n = 0;
#if !defined(_WIN32)
    DIR* d = opendir(out);
    if (!d)
        return 1;
    struct dirent* e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (dirs >= 0 && lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        {
            char full[1500];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", out, e->d_name);
            int is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
            if (dirs != is_dir)
                continue;
        }
        lua_pushstring(L, e->d_name);
        lua_rawseti(L, -2, ++n);
    }
    closedir(d);
#else
    char pattern[1300];
    snprintf(pattern, sizeof pattern, "%s\\*", out);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do
    {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && dirs != !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        lua_pushstring(L, fd.cFileName);
        lua_rawseti(L, -2, ++n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    return 1;
}

static int f_remove(lua_State* L)
{
    char out[1200];
    lua_pushboolean(L, remove(xi_host_path(luaL_checkstring(L, 1), out, sizeof out)) == 0);
    return 1;
}

static const luaL_Reg FS[] = { { "path", f_path }, { "exists", f_exists }, { "is_dir", f_is_dir }, { "mkdir", f_mkdir },
    { "list", f_list }, { "remove", f_remove }, { NULL, NULL } };

/* --- misc ------------------------------------------------------------------------------------- */

static int x_log(lua_State* L)
{
    Addon* a = addon_of(L);
    xi_log("%s: %s", a ? a->name : "?", luaL_checkstring(L, 1));
    return 0;
}

static int x_error(lua_State* L)
{
    xi_addon_error(addon_of(L), luaL_optstring(L, 2, "handler"), luaL_checkstring(L, 1));
    return 0;
}

static int x_clock(lua_State* L)
{
    lua_pushnumber(L, (double)rt_monotonic_ns() / 1e9);
    return 1;
}

static int x_time(lua_State* L)
{
    lua_pushnumber(L, (double)time(NULL));
    return 1;
}

static int x_open_url(lua_State* L)
{
    lua_pushboolean(L, SDL_OpenURL(luaL_checkstring(L, 1)));
    return 1;
}

/* Shift-JIS (FFXI's text, with its colour and auto-translate codes) and UTF-8, through the resource
 * reader's CP932 table (res.c): no iconv, so every host converts the same way. */
static int x_sjis_to_utf8(lua_State* L)
{
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    int flags = (int)luaL_optnumber(L, 2, RES_UTF8_KEEP_CODES);
    size_t cap = n * 3 + 16;
    char* out = (char*)malloc(cap);
    size_t w = res_utf8(s, n, out, cap, flags);
    if (w >= cap) /* auto-translate phrases grow more than three times */
    {
        free(out);
        cap = w + 1;
        out = (char*)malloc(cap);
        w = res_utf8(s, n, out, cap, flags);
    }
    lua_pushlstring(L, out, w < cap ? w : cap - 1);
    free(out);
    return 1;
}

typedef struct Rev
{
    uint32_t cp;
    uint16_t sjis;
} Rev;
static Rev* g_rev;
static size_t g_nrev;

static int rev_cmp(const void* a, const void* b)
{
    uint32_t x = ((const Rev*)a)->cp, y = ((const Rev*)b)->cp;
    return x < y ? -1 : x > y;
}

static void build_rev(void)
{
    if (g_rev)
        return;
    g_rev = (Rev*)malloc(sizeof(Rev) * 64 * 190);
    for (unsigned lead = 0x81; lead <= 0xFC; ++lead)
    {
        if (lead > 0x9F && lead < 0xE0)
            continue;
        for (unsigned trail = 0x40; trail <= 0xFC; ++trail)
        {
            uint32_t cp = res_sjis_char((uint8_t)lead, (uint8_t)trail);
            if (cp && cp != 0xFFFD && g_nrev < 64 * 190)
                g_rev[g_nrev++] = (Rev){ cp, (uint16_t)(lead << 8 | trail) };
        }
    }
    qsort(g_rev, g_nrev, sizeof *g_rev, rev_cmp);
}

static int x_utf8_to_sjis(lua_State* L)
{
    size_t n;
    const unsigned char* s = (const unsigned char*)luaL_checklstring(L, 1, &n);
    build_rev();
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < n;)
    {
        uint32_t c = s[i];
        size_t k = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (k > 1 && i + k <= n)
        {
            c &= 0xFF >> (k + 1);
            for (size_t j = 1; j < k; ++j)
                c = (c << 6) | (s[i + j] & 0x3F);
        }
        else
            k = 1;
        i += k;
        if (c < 0x80)
            luaL_addchar(&b, (char)c);
        else if (c >= 0xFF61 && c <= 0xFF9F) /* half-width katakana: one byte */
            luaL_addchar(&b, (char)(c - 0xFF61 + 0xA1));
        else
        {
            Rev key = { c, 0 };
            const Rev* r = (const Rev*)bsearch(&key, g_rev, g_nrev, sizeof *g_rev, rev_cmp);
            if (r)
            {
                luaL_addchar(&b, (char)(r->sjis >> 8));
                luaL_addchar(&b, (char)(r->sjis & 0xFF));
            }
            else
                luaL_addchar(&b, '?');
        }
    }
    luaL_pushresult(&b);
    return 1;
}

/* embedded(name): the source of host/addons/lua/<name>.lua, or nil */
static int x_embedded(lua_State* L)
{
    size_t n;
    const char* s = xi_embedded(luaL_checkstring(L, 1), &n);
    if (!s)
        return lua_pushnil(L), 1;
    lua_pushlstring(L, s, n);
    return 1;
}

static const luaL_Reg MISC[] = { { "log", x_log }, { "embedded", x_embedded }, { "error", x_error }, { "clock", x_clock }, { "time", x_time },
    { "open_url", x_open_url }, { "sjis_to_utf8", x_sjis_to_utf8 }, { "utf8_to_sjis", x_utf8_to_sjis }, { NULL, NULL } };

static void sub(lua_State* L, const char* name, const luaL_Reg* fns)
{
    lua_newtable(L);
    luaL_register(L, NULL, fns);
    lua_setfield(L, -2, name);
}

void xi_lua_open(lua_State* L, Addon* a)
{
    lua_pushlightuserdata(L, a);
    lua_setfield(L, LUA_REGISTRYINDEX, "xi.addon");
    lua_newtable(L);
    luaL_register(L, NULL, MISC);
    sub(L, "memory", MEMORY);
    sub(L, "chat", CHAT);
    sub(L, "ui", UI);
    sub(L, "packets", PACKETS);
    sub(L, "input", INPUT);
    sub(L, "addons", ADDONS);
    sub(L, "fs", FS);
    xi_game_open(L);
    lua_setfield(L, -2, "game");
    xi_res_open(L);
    lua_setfield(L, -2, "res");
    /* paths */
    lua_newtable(L);
    lua_pushstring(L, xi_data_dir());
    lua_setfield(L, -2, "data");
    lua_pushstring(L, xi_game_dir());
    lua_setfield(L, -2, "game");
    for (int k = 0; k < XI_KINDS; ++k)
    {
        lua_pushstring(L, xi_kind_root(k));
        lua_setfield(L, -2, xi_kind_name[k]);
    }
    lua_setfield(L, -2, "paths");
    lua_pushstring(L, FFXI_BUILD);
    lua_setfield(L, -2, "build");
    xi_d3d_ffi_open(L); /* xi.d3d8_device and ffi.C (d3d_ffi.c) */
}
