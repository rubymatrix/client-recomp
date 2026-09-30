/* Lua binding of Ashita v4's IGuiManager: the object AshitaCore:GetGuiManager() returns and Ashita's
 * libs/imgui.lua wraps (see gui_lua.h for the conventions).
 *
 * Most functions and methods are generated (tools/gen_imgui_lua.py -> gui_lua_gen.inc) from Ashita's
 * annotations and our imgui.h. This file holds the runtime they use (argument conversion, live objects)
 * and the hand-written functions whose Lua shapes the generator can't derive (hand_*).
 *
 * Live objects: a full userdata { magic, class id, pointer, array ops } whose metatable resolves methods,
 * then fields (getters/setters generated per struct member), then numeric indices (ImVec2/ImVec4
 * components, array elements, or elements of a pointed-to array). Value results (an ImVec2 from
 * viewport:GetCenter()) carry their bytes inline after the header.
 *
 * Errors: bindings never raise Lua errors for bad arguments (wrong types take the ImGui default, like a
 * nil does); Lua callbacks (InputText, size constraints) run under lua_pcall and their errors are
 * printed, so no longjmp ever crosses ImGui's frames. */
#include "gui_lua.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "imgui.h"
#include "imgui_internal.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------
// Object model.

constexpr uint32_t kMagic = 0x474d4958u; // 'XIMG'

struct ArrayOps
{
    int (*len)(void* base);
    void (*get)(lua_State* L, void* base, int i);        // i is 0-based
    void (*set)(lua_State* L, void* base, int i, int v); // value at stack index v
};

struct Obj
{
    uint32_t magic;
    int32_t cls;
    void* p;
    const ArrayOps* ops; // non-null: an array proxy (p = first element or the ImVector)
};

struct FieldInfo
{
    const char* name;
    void (*get)(lua_State* L, void* p);
    void (*set)(lua_State* L, void* p, int v);
};

struct ClassInfo
{
    const char* name;
    size_t size;
    const FieldInfo* fields;
    const luaL_Reg* methods;
};

template <class T> struct ClassOf
{
    static constexpr int id = -1;
};

#define XI_GUI_GEN_TRAITS
#include "gui_lua_gen.inc"
#undef XI_GUI_GEN_TRAITS

constexpr int CLS_ARRAY = CLS_COUNT; // metatable slot for array proxies

char g_key_meta; // registry[&g_key_meta] = { [cls + 1] = metatable }
char g_key_mgr;  // registry[&g_key_mgr] = manager table
char g_key_sizecb; // registry[&g_key_sizecb] = pending SetNextWindowSizeConstraints callback

int g_visible = 1;

void ensure_meta(lua_State* L);

void set_meta(lua_State* L, int cls)
{
    lua_pushlightuserdata(L, &g_key_meta);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        ensure_meta(L);
        lua_pushlightuserdata(L, &g_key_meta);
        lua_rawget(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, -1, cls + 1);
    lua_setmetatable(L, -3);
    lua_pop(L, 1);
}

void push_obj(lua_State* L, void* p, int cls)
{
    Obj* o = (Obj*)lua_newuserdata(L, sizeof(Obj));
    o->magic = kMagic;
    o->cls = cls;
    o->p = p;
    o->ops = nullptr;
    set_meta(L, cls);
}

template <class T> void push_val(lua_State* L, const T& v, int cls)
{
    static_assert(sizeof(Obj) % 8 == 0, "inline storage alignment");
    Obj* o = (Obj*)lua_newuserdata(L, sizeof(Obj) + sizeof(T));
    o->magic = kMagic;
    o->cls = cls;
    o->p = (char*)o + sizeof(Obj);
    o->ops = nullptr;
    memcpy(o->p, &v, sizeof(T));
    set_meta(L, cls);
}

void push_array(lua_State* L, void* base, const ArrayOps* ops)
{
    Obj* o = (Obj*)lua_newuserdata(L, sizeof(Obj));
    o->magic = kMagic;
    o->cls = CLS_ARRAY;
    o->p = base;
    o->ops = ops;
    set_meta(L, CLS_ARRAY);
}

Obj* to_obj(lua_State* L, int idx)
{
    if (lua_type(L, idx) != LUA_TUSERDATA || lua_objlen(L, idx) < sizeof(Obj))
        return nullptr;
    Obj* o = (Obj*)lua_touserdata(L, idx);
    return o->magic == kMagic ? o : nullptr;
}

void* self_ptr(lua_State* L, int cls)
{
    Obj* o = to_obj(L, 1);
    return (o && !o->ops && o->cls == cls) ? o->p : nullptr;
}

void* opt_obj(lua_State* L, int idx, int cls)
{
    Obj* o = to_obj(L, idx);
    return (o && !o->ops && o->cls == cls) ? o->p : nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Argument conversion. Missing, nil or wrongly typed arguments take the default.

bool is_none(lua_State* L, int idx) { return lua_type(L, idx) <= LUA_TNIL; }

const char* opt_str(lua_State* L, int idx, const char* def, size_t* n)
{
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING || t == LUA_TNUMBER)
        return lua_tolstring(L, idx, n);
    *n = def ? strlen(def) : 0;
    return def;
}

bool opt_bool(lua_State* L, int idx, bool def) { return is_none(L, idx) ? def : lua_toboolean(L, idx) != 0; }

double opt_num(lua_State* L, int idx, double def)
{
    if (lua_type(L, idx) == LUA_TNUMBER || (lua_type(L, idx) == LUA_TSTRING && lua_isnumber(L, idx)))
        return lua_tonumber(L, idx);
    if (lua_type(L, idx) == LUA_TBOOLEAN)
        return lua_toboolean(L, idx) ? 1.0 : 0.0;
    return def;
}

// Integers/flags: bit.bor() results are signed 32-bit; 0xFFFFFFFF literals are positive. Both wrap right.
int64_t to_i64(double d)
{
    if (d != d)
        return 0;
    if (d >= 9.2e18)
        return INT64_MAX;
    if (d <= -9.2e18)
        return INT64_MIN;
    return (int64_t)d;
}
int64_t opt_int(lua_State* L, int idx, double def) { return to_i64(opt_num(L, idx, def)); }
uint32_t opt_u32(lua_State* L, int idx, uint32_t def) { return (uint32_t)to_i64(opt_num(L, idx, (double)def)); }
int64_t opt_i64(lua_State* L, int idx, int64_t def) { return to_i64(opt_num(L, idx, (double)def)); }

float tfield(lua_State* L, int t, int i, const char* k, float def)
{
    lua_rawgeti(L, t, i);
    if (lua_isnil(L, -1) && k)
    {
        lua_pop(L, 1);
        lua_getfield(L, t, k);
    }
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

// Reads n floats (2: ImVec2, 4: ImVec4) from a {x, y[, z, w]} table (or .x/.y/.z/.w) or a vec object.
bool read_floats(lua_State* L, int idx, float* out, int n)
{
    static const char* const keys[4] = {"x", "y", "z", "w"};
    if (lua_istable(L, idx))
    {
        idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
        for (int i = 0; i < n; i++)
            out[i] = tfield(L, idx, i + 1, keys[i], out[i]);
        return true;
    }
    Obj* o = to_obj(L, idx);
    if (o && !o->ops && o->p && (o->cls == CLS_ImVec2 || o->cls == CLS_ImVec4))
    {
        int have = o->cls == CLS_ImVec2 ? 2 : 4;
        for (int i = 0; i < n && i < have; i++)
            out[i] = ((float*)o->p)[i];
        return true;
    }
    return false;
}

ImVec2 opt_vec2(lua_State* L, int idx, ImVec2 def)
{
    float f[2] = {def.x, def.y};
    read_floats(L, idx, f, 2);
    return ImVec2(f[0], f[1]);
}

ImVec4 opt_vec4(lua_State* L, int idx, ImVec4 def)
{
    float f[4] = {def.x, def.y, def.z, def.w};
    read_floats(L, idx, f, 4);
    return ImVec4(f[0], f[1], f[2], f[3]);
}

const ImVec2* opt_pvec2(lua_State* L, int idx, ImVec2* tmp)
{
    *tmp = ImVec2(0, 0);
    return read_floats(L, idx, &tmp->x, 2) ? tmp : nullptr;
}

const ImVec4* opt_pvec4(lua_State* L, int idx, ImVec4* tmp)
{
    *tmp = ImVec4(0, 0, 0, 0);
    return read_floats(L, idx, &tmp->x, 4) ? tmp : nullptr;
}

// ImTextureID is a number (whatever the renderer handed out); texture refs/data objects work too.
ImTextureRef opt_tex(lua_State* L, int idx)
{
    ImTextureRef r;
    switch (lua_type(L, idx))
    {
    case LUA_TNUMBER:
        r._TexID = (ImTextureID)(uint64_t)to_i64(lua_tonumber(L, idx));
        break;
    case LUA_TLIGHTUSERDATA:
        r._TexID = (ImTextureID)(uintptr_t)lua_touserdata(L, idx);
        break;
    case LUA_TUSERDATA:
        if (Obj* o = to_obj(L, idx))
        {
            if (o->cls == CLS_ImTextureRef && o->p)
                r = *(ImTextureRef*)o->p;
            else if (o->cls == CLS_ImTextureData && o->p)
                r._TexData = (ImTextureData*)o->p;
        }
        break;
    }
    return r;
}

void* opt_rawptr(lua_State* L, int idx)
{
    switch (lua_type(L, idx))
    {
    case LUA_TLIGHTUSERDATA:
        return lua_touserdata(L, idx);
    case LUA_TUSERDATA:
        if (Obj* o = to_obj(L, idx))
            return o->p;
        return lua_touserdata(L, idx);
    case LUA_TNUMBER:
        return (void*)(uintptr_t)to_i64(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return (void*)lua_tostring(L, idx);
    default:
        return (void*)lua_topointer(L, idx); // cdata
    }
}

// Pointer parameters: a table read/written at [1]. Nullable ones (p_open) are NULL without a table.
bool* get_pbool(lua_State* L, int idx, bool* v, bool nullable)
{
    if (lua_istable(L, idx))
    {
        lua_rawgeti(L, idx, 1);
        *v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    }
    if (lua_type(L, idx) == LUA_TBOOLEAN && !nullable)
        *v = lua_toboolean(L, idx) != 0;
    return nullable ? nullptr : v;
}

template <class T> T num_as(double d)
{
    if constexpr (std::is_floating_point_v<T>)
        return (T)d;
    else
        return (T)to_i64(d);
}

template <class T> T* get_pnum(lua_State* L, int idx, T* v, bool nullable)
{
    if (lua_istable(L, idx))
    {
        lua_rawgeti(L, idx, 1);
        *v = lua_isnumber(L, -1) ? num_as<T>(lua_tonumber(L, -1)) : (T)0;
        lua_pop(L, 1);
        return v;
    }
    if (lua_type(L, idx) == LUA_TNUMBER && !nullable)
        *v = num_as<T>(lua_tonumber(L, idx));
    return nullable ? nullptr : v;
}

void set_t1_bool(lua_State* L, int idx, bool v)
{
    if (!lua_istable(L, idx))
        return;
    lua_pushboolean(L, v);
    lua_rawseti(L, idx, 1);
}

void set_t1_num(lua_State* L, int idx, double v)
{
    if (!lua_istable(L, idx))
        return;
    lua_pushnumber(L, v);
    lua_rawseti(L, idx, 1);
}

template <class T> void read_arr(lua_State* L, int idx, T* a, int n)
{
    if (lua_istable(L, idx))
    {
        for (int i = 0; i < n; i++)
        {
            lua_rawgeti(L, idx, i + 1);
            if (lua_isnumber(L, -1))
                a[i] = num_as<T>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        return;
    }
    float f[4] = {0, 0, 0, 0};
    if (n <= 4 && read_floats(L, idx, f, n))
        for (int i = 0; i < n; i++)
            a[i] = (T)f[i];
}

template <class T> void write_arr(lua_State* L, int idx, const T* a, int n)
{
    if (lua_istable(L, idx))
    {
        for (int i = 0; i < n; i++)
        {
            lua_pushnumber(L, (lua_Number)a[i]);
            lua_rawseti(L, idx, i + 1);
        }
        return;
    }
    Obj* o = to_obj(L, idx);
    if (o && !o->ops && o->p && (o->cls == CLS_ImVec2 || o->cls == CLS_ImVec4))
    {
        int have = o->cls == CLS_ImVec2 ? 2 : 4;
        for (int i = 0; i < n && i < have; i++)
            ((float*)o->p)[i] = (float)a[i];
    }
}

// Overload choice by Lua argument types. sig chars: s string, n number, b boolean, t table (or vec
// object), u userdata/light userdata/cdata/object, f function, ? anything. A nil/missing argument fits
// any parameter; a present one of the wrong type rules the overload out. The overload matching the
// most present arguments wins; ties go to the first declared.
bool sig_match(lua_State* L, int idx, char c)
{
    int t = lua_type(L, idx);
    switch (c)
    {
    case 's':
        return t == LUA_TSTRING;
    case 'n':
        return t == LUA_TNUMBER;
    case 'b':
        return t == LUA_TBOOLEAN;
    case 't':
        if (t == LUA_TTABLE)
            return true;
        if (Obj* o = to_obj(L, idx))
            return o->cls == CLS_ImVec2 || o->cls == CLS_ImVec4;
        return false;
    case 'u':
        return t == LUA_TUSERDATA || t == LUA_TLIGHTUSERDATA || t == 10 /* LuaJIT cdata */;
    case 'f':
        return t == LUA_TFUNCTION;
    default:
        return true;
    }
}

int pick_overload(lua_State* L, int base, const char* const* sigs, int n)
{
    int best = -1, best_score = -1;
    for (int k = 0; k < n; k++)
    {
        int score = 0;
        bool ok = true;
        for (int i = 0; sigs[k][i]; i++)
        {
            if (is_none(L, base + i))
                continue;
            if (!sig_match(L, base + i, sigs[k][i]))
            {
                ok = false;
                break;
            }
            score++;
        }
        if (ok && score > best_score)
            best = k, best_score = score;
    }
    return best < 0 ? 0 : best;
}

// ---------------------------------------------------------------------------------------------------
// Field access (templates instantiated by the generated getters/setters).

template <class T> struct ImVectorOf : std::false_type
{
};
template <class T> struct ImVectorOf<ImVector<T>> : std::true_type
{
    using elem = T;
};

std::set<std::string>& interned()
{
    static std::set<std::string> s; // strings assigned to const char* members (io.IniFilename ...)
    return s;
}

template <class T> void push_ref(lua_State* L, T& f);
template <class T> void set_ref(lua_State* L, T& f, int idx);

template <class E, int N> struct CArrayOps
{
    static int len(void*) { return N; }
    static void get(lua_State* L, void* b, int i) { push_ref(L, ((E*)b)[i]); }
    static void set(lua_State* L, void* b, int i, int v) { set_ref(L, ((E*)b)[i], v); }
    static constexpr ArrayOps ops = {len, get, set};
};

template <class E> struct VectorOps
{
    static int len(void* b) { return ((ImVector<E>*)b)->Size; }
    static void get(lua_State* L, void* b, int i) { push_ref(L, ((ImVector<E>*)b)->Data[i]); }
    static void set(lua_State* L, void* b, int i, int v) { set_ref(L, ((ImVector<E>*)b)->Data[i], v); }
    static constexpr ArrayOps ops = {len, get, set};
};

template <class T> void push_ref(lua_State* L, T& f)
{
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        lua_pushboolean(L, f ? 1 : 0);
    else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
        lua_pushnumber(L, (lua_Number)f);
    else if constexpr (std::is_same_v<U, ImVec2>)
        push_obj(L, (void*)&f, CLS_ImVec2);
    else if constexpr (std::is_same_v<U, ImVec4>)
        push_obj(L, (void*)&f, CLS_ImVec4);
    else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>)
    {
        if (f)
            lua_pushstring(L, f);
        else
            lua_pushnil(L);
    }
    else if constexpr (std::is_array_v<U>)
    {
        using E = std::remove_extent_t<U>;
        constexpr int N = (int)std::extent_v<U>;
        if constexpr (std::is_same_v<std::remove_cv_t<E>, char>)
            lua_pushlstring(L, f, strnlen(f, N));
        else
            push_array(L, (void*)&f[0], &CArrayOps<std::remove_cv_t<E>, N>::ops);
    }
    else if constexpr (ImVectorOf<U>::value)
        push_array(L, (void*)&f, &VectorOps<typename ImVectorOf<U>::elem>::ops);
    else if constexpr (std::is_pointer_v<U>)
    {
        using P = std::remove_cv_t<std::remove_pointer_t<U>>;
        if constexpr (std::is_function_v<P>)
            lua_pushnil(L);
        else if (!f)
            lua_pushnil(L);
        else if constexpr (ClassOf<P>::id >= 0)
            push_obj(L, (void*)f, ClassOf<P>::id);
        else
            lua_pushlightuserdata(L, (void*)f);
    }
    else if constexpr (ClassOf<U>::id >= 0)
        push_obj(L, (void*)&f, ClassOf<U>::id);
    else
        lua_pushnil(L);
}

template <class T> void set_ref(lua_State* L, T& f, int idx)
{
    using U = T;
    if constexpr (std::is_const_v<U> || std::is_array_v<U> || ImVectorOf<U>::value)
        return;
    else if constexpr (std::is_same_v<U, bool>)
        f = lua_toboolean(L, idx) != 0;
    else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
    {
        if (lua_isnumber(L, idx))
            f = (U)num_as<std::conditional_t<std::is_enum_v<U>, int64_t, U>>(lua_tonumber(L, idx));
        else if (lua_type(L, idx) == LUA_TBOOLEAN)
            f = (U)(lua_toboolean(L, idx) ? 1 : 0);
    }
    else if constexpr (std::is_same_v<U, ImVec2>)
        f = opt_vec2(L, idx, f);
    else if constexpr (std::is_same_v<U, ImVec4>)
        f = opt_vec4(L, idx, f);
    else if constexpr (std::is_same_v<U, const char*>)
    {
        if (lua_isnil(L, idx))
            f = nullptr;
        else if (lua_type(L, idx) == LUA_TSTRING)
            f = interned().insert(std::string(lua_tostring(L, idx), lua_objlen(L, idx))).first->c_str();
    }
    else if constexpr (std::is_pointer_v<U>)
    {
        using P = std::remove_cv_t<std::remove_pointer_t<U>>;
        if constexpr (std::is_function_v<P> || std::is_same_v<P, char>)
            return;
        else if (lua_isnil(L, idx))
            f = nullptr;
        else if constexpr (ClassOf<P>::id >= 0)
        {
            if (void* p = opt_obj(L, idx, ClassOf<P>::id))
                f = (U)p;
        }
        else if (lua_type(L, idx) == LUA_TLIGHTUSERDATA)
            f = (U)lua_touserdata(L, idx);
    }
    else
        (void)L, (void)f, (void)idx;
}

// ---------------------------------------------------------------------------------------------------
// Helpers for the hand-written functions.

void log_callback_error(lua_State* L, const char* what)
{
    const char* msg = lua_tostring(L, -1);
    fprintf(stderr, "[imgui] %s callback error: %s\n", what, msg ? msg : "(non-string error)");
    lua_pop(L, 1);
}

bool guarded() { return !xi_gui_in_frame() || !ImGui::GetCurrentContext(); }

ImGuiWindow* cur_window() { return GImGui ? GImGui->CurrentWindow : nullptr; }

// Preconditions of the generated bindings (see PRECOND in tools/gen_imgui_lua.py).
bool range_ok(lua_State* L, int idx, int lo, int hi)
{
    if (is_none(L, idx))
        return true; // the ImGui default
    double v = opt_num(L, idx, -1);
    return v >= lo && v < hi;
}

bool key_ok(lua_State* L, int idx)
{
    int64_t k = opt_int(L, idx, 0);
    return k >= INT32_MIN && k <= INT32_MAX && ImGui::IsNamedKeyOrMod((ImGuiKey)k);
}

bool chord_ok(lua_State* L, int idx)
{
    int64_t c = opt_int(L, idx, 0);
    if (c < INT32_MIN || c > INT32_MAX)
        return false;
    int key = (int)c & ~ImGuiMod_Mask_;
    return key == 0 ? ((int)c & ImGuiMod_Mask_) != 0 : ImGui::IsNamedKey((ImGuiKey)key);
}

bool nonempty_str(lua_State* L, int idx)
{
    int t = lua_type(L, idx);
    return (t == LUA_TSTRING && lua_objlen(L, idx) > 0) || t == LUA_TNUMBER;
}

int ret_false(lua_State* L)
{
    lua_pushboolean(L, 0);
    return 1;
}

int ret_num0(lua_State* L)
{
    lua_pushnumber(L, 0);
    return 1;
}

// Strings of a Lua list (items tables for Combo/ListBox). The table keeps them alive.
int read_items(lua_State* L, int idx, int count, std::vector<const char*>& out)
{
    if (!lua_istable(L, idx))
        return 0;
    int n = (int)lua_objlen(L, idx);
    if (count <= 0 || count > n)
        count = n;
    out.resize(count);
    for (int i = 0; i < count; i++)
    {
        lua_rawgeti(L, idx, i + 1);
        const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        out[i] = s ? s : "";
        lua_pop(L, 1); // still referenced by the table
    }
    return count;
}

// ImGuiDataType scalars in 8-byte slots.
struct Scalar
{
    unsigned char b[8];
};

void scalar_set(ImGuiDataType dt, double v, void* dst)
{
    switch (dt)
    {
    case ImGuiDataType_S8: *(ImS8*)dst = (ImS8)to_i64(v); break;
    case ImGuiDataType_U8: *(ImU8*)dst = (ImU8)to_i64(v); break;
    case ImGuiDataType_S16: *(ImS16*)dst = (ImS16)to_i64(v); break;
    case ImGuiDataType_U16: *(ImU16*)dst = (ImU16)to_i64(v); break;
    case ImGuiDataType_S32: *(ImS32*)dst = (ImS32)to_i64(v); break;
    case ImGuiDataType_U32: *(ImU32*)dst = (ImU32)to_i64(v); break;
    case ImGuiDataType_S64: *(ImS64*)dst = (ImS64)to_i64(v); break;
    case ImGuiDataType_U64: *(ImU64*)dst = (ImU64)to_i64(v); break;
    case ImGuiDataType_Float: *(float*)dst = (float)v; break;
    case ImGuiDataType_Double: *(double*)dst = v; break;
    case ImGuiDataType_Bool: *(bool*)dst = v != 0; break;
    default: *(ImS32*)dst = (ImS32)to_i64(v); break;
    }
}

double scalar_get(ImGuiDataType dt, const void* src)
{
    switch (dt)
    {
    case ImGuiDataType_S8: return *(const ImS8*)src;
    case ImGuiDataType_U8: return *(const ImU8*)src;
    case ImGuiDataType_S16: return *(const ImS16*)src;
    case ImGuiDataType_U16: return *(const ImU16*)src;
    case ImGuiDataType_S32: return *(const ImS32*)src;
    case ImGuiDataType_U32: return *(const ImU32*)src;
    case ImGuiDataType_S64: return (double)*(const ImS64*)src;
    case ImGuiDataType_U64: return (double)*(const ImU64*)src;
    case ImGuiDataType_Float: return *(const float*)src;
    case ImGuiDataType_Double: return *(const double*)src;
    case ImGuiDataType_Bool: return *(const bool*)src ? 1 : 0;
    default: return *(const ImS32*)src;
    }
}

ImGuiDataType opt_dt(lua_State* L, int idx)
{
    int64_t dt = opt_int(L, idx, ImGuiDataType_S32);
    return (dt < 0 || dt >= ImGuiDataType_COUNT) ? ImGuiDataType_S32 : (ImGuiDataType)dt;
}

// p_data table [1..n] -> slots; returns false when there's no table (the widget then edits a temp).
void scalars_read(lua_State* L, int idx, ImGuiDataType dt, Scalar* s, int n)
{
    memset(s, 0, sizeof(Scalar) * n);
    for (int i = 0; i < n; i++)
    {
        double v = 0;
        if (lua_istable(L, idx))
        {
            lua_rawgeti(L, idx, i + 1);
            v = opt_num(L, -1, 0);
            lua_pop(L, 1);
        }
        else if (i == 0)
            v = opt_num(L, idx, 0);
        scalar_set(dt, v, s[i].b);
    }
}

void scalars_write(lua_State* L, int idx, ImGuiDataType dt, const Scalar* s, const Scalar* orig, int n)
{
    if (!lua_istable(L, idx))
        return;
    for (int i = 0; i < n; i++)
        if (memcmp(s[i].b, orig[i].b, 8) != 0)
        {
            if (dt == ImGuiDataType_Bool)
                lua_pushboolean(L, *(const bool*)s[i].b);
            else
                lua_pushnumber(L, scalar_get(dt, s[i].b));
            lua_rawseti(L, idx, i + 1);
        }
}

// Optional scalar bound (p_min/p_max/p_step): a table's [1] or a plain number; NULL when absent.
const void* opt_scalar(lua_State* L, int idx, ImGuiDataType dt, Scalar* tmp)
{
    if (lua_istable(L, idx))
    {
        lua_rawgeti(L, idx, 1);
        bool have = lua_isnumber(L, -1) != 0;
        double v = have ? lua_tonumber(L, -1) : 0;
        lua_pop(L, 1);
        if (!have)
            return nullptr;
        scalar_set(dt, v, tmp->b);
        return tmp->b;
    }
    if (lua_type(L, idx) == LUA_TNUMBER)
    {
        scalar_set(dt, lua_tonumber(L, idx), tmp->b);
        return tmp->b;
    }
    return nullptr;
}

} // namespace

// =====================================================================================================
// Hand-written IGuiManager functions (names referenced by the generated function table).

namespace {

// ---- Manager / context. Addons don't own the frame or the context: these are no-ops.

int hand_GetVisible(lua_State* L)
{
    lua_pushboolean(L, g_visible);
    return 1;
}

int hand_SetVisible(lua_State* L)
{
    // Called as mgr:SetVisible(v) (per the annotations); tolerate mgr.SetVisible(v) too.
    int idx = (lua_type(L, 1) == LUA_TBOOLEAN && lua_gettop(L) == 1) ? 1 : 2;
    g_visible = lua_toboolean(L, idx) ? 1 : 0;
    return 0;
}

int hand_CreateContext(lua_State* L)
{
    if (ImGuiContext* c = ImGui::GetCurrentContext())
        push_obj(L, c, CLS_ImGuiContext);
    else
        lua_pushnil(L);
    return 1;
}

int hand_noop(lua_State*) { return 0; }
int hand_DestroyContext(lua_State* L) { return hand_noop(L); }
int hand_SetCurrentContext(lua_State* L) { return hand_noop(L); }
int hand_NewFrame(lua_State* L) { return hand_noop(L); }
int hand_EndFrame(lua_State* L) { return hand_noop(L); }
int hand_Render(lua_State* L) { return hand_noop(L); }
int hand_UpdatePlatformWindows(lua_State* L) { return hand_noop(L); }
int hand_RenderPlatformWindowsDefault(lua_State* L) { return hand_noop(L); }
int hand_DestroyPlatformWindows(lua_State* L) { return hand_noop(L); }

// ---- Windows.

lua_State* g_sizecb_state; // state whose registry holds the pending size callback

void size_callback(ImGuiSizeCallbackData* d)
{
    lua_State* L = g_sizecb_state;
    if (!L)
        return;
    lua_pushlightuserdata(L, &g_key_sizecb);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    push_obj(L, d, CLS_ImGuiSizeCallbackData);
    if (lua_pcall(L, 1, 0, 0) != 0)
        log_callback_error(L, "SetNextWindowSizeConstraints");
}

int hand_SetNextWindowSizeConstraints(lua_State* L)
{
    if (guarded())
        return 0;
    ImVec2 mn = opt_vec2(L, 1, ImVec2(0, 0)), mx = opt_vec2(L, 2, ImVec2(FLT_MAX, FLT_MAX));
    if (lua_isfunction(L, 3))
    {
        lua_pushlightuserdata(L, &g_key_sizecb);
        lua_pushvalue(L, 3);
        lua_rawset(L, LUA_REGISTRYINDEX);
        g_sizecb_state = L;
        ImGui::SetNextWindowSizeConstraints(mn, mx, size_callback, nullptr);
    }
    else
        ImGui::SetNextWindowSizeConstraints(mn, mx);
    return 0;
}

// ---- Fonts.

// PushFont(font, size): 1.92 semantics; a missing size keeps pre-1.92 addons working (font's own size).
int hand_PushFont(lua_State* L)
{
    if (guarded())
        return 0;
    ImFont* font = (ImFont*)opt_obj(L, 1, CLS_ImFont);
    float size = lua_isnumber(L, 2) ? (float)lua_tonumber(L, 2) : (font ? font->LegacySize : 0.0f);
    ImGui::PushFont(font, size);
    return 0;
}

ImFontAtlas* unlocked_atlas()
{
    if (!ImGui::GetCurrentContext())
        return nullptr;
    ImFontAtlas* a = ImGui::GetIO().Fonts;
    return (a && !a->Locked) ? a : nullptr; // locked between NewFrame and Render without RendererHasTextures
}

int push_font(lua_State* L, ImFont* f)
{
    if (f)
        push_obj(L, f, CLS_ImFont);
    else
        lua_pushnil(L);
    return 1;
}

int hand_AddFontFromFileTTF(lua_State* L)
{
    ImFontAtlas* a = unlocked_atlas();
    size_t n;
    const char* path = opt_str(L, 1, nullptr, &n);
    if (!a || !path)
        return push_font(L, nullptr);
    FILE* fp = fopen(path, "rb"); // ImGui asserts on a missing file; answer nil instead
    if (!fp)
        return push_font(L, nullptr);
    fclose(fp);
    return push_font(L, a->AddFontFromFileTTF(path, (float)opt_num(L, 2, 0)));
}

int hand_AddFontFromMemoryCompressedTTF(lua_State* L)
{
    ImFontAtlas* a = unlocked_atlas();
    const void* data = opt_rawptr(L, 1);
    int size = (int)opt_int(L, 2, lua_type(L, 1) == LUA_TSTRING ? (double)lua_objlen(L, 1) : 0);
    if (!a || !data || size <= 0)
        return push_font(L, nullptr);
    return push_font(L, a->AddFontFromMemoryCompressedTTF(data, size, (float)opt_num(L, 3, 0))); // copies
}

int hand_AddFontFromMemoryCompressedBase85TTF(lua_State* L)
{
    ImFontAtlas* a = unlocked_atlas();
    size_t n;
    const char* s = opt_str(L, 1, nullptr, &n);
    if (!a || !s)
        return push_font(L, nullptr);
    return push_font(L, a->AddFontFromMemoryCompressedBase85TTF(s, (float)opt_num(L, 2, 0)));
}

// ImFontAtlas:AddFontFromMemoryTTF(data, size, size_pixels, cfg, ranges): the atlas owns (and later
// frees) the data, so a Lua string is copied into ImGui's allocator; a raw pointer is handed over.
int font_mem(lua_State* L, bool compressed)
{
    ImFontAtlas* a = (ImFontAtlas*)self_ptr(L, CLS_ImFontAtlas);
    if (!a || a->Locked)
        return push_font(L, nullptr);
    int size = (int)opt_int(L, 3, lua_type(L, 2) == LUA_TSTRING ? (double)lua_objlen(L, 2) : 0);
    void* data = opt_rawptr(L, 2);
    if (!data || size <= 0)
        return push_font(L, nullptr);
    float px = (float)opt_num(L, 4, 0);
    const ImFontConfig* cfg = (const ImFontConfig*)opt_obj(L, 5, CLS_ImFontConfig);
    const ImWchar* ranges = (const ImWchar*)(lua_isnil(L, 6) || lua_isnone(L, 6) ? nullptr : opt_rawptr(L, 6));
    if (compressed)
        return push_font(L, a->AddFontFromMemoryCompressedTTF(data, size, px, cfg, ranges));
    if (lua_type(L, 2) == LUA_TSTRING)
    {
        size = (int)ImMin((size_t)size, lua_objlen(L, 2));
        void* copy = IM_ALLOC((size_t)size);
        memcpy(copy, data, (size_t)size);
        data = copy;
    }
    return push_font(L, a->AddFontFromMemoryTTF(data, size, px, cfg, ranges));
}

int hand_ImFontAtlas_AddFontFromMemoryTTF(lua_State* L) { return font_mem(L, false); }
int hand_ImFontAtlas_AddFontFromMemoryCompressedTTF(lua_State* L) { return font_mem(L, true); }

// ImFont:RenderText(draw_list, size, pos, col, clip_rect, text_begin, text_end, wrap_width, cpu_fine_clip)
int hand_ImFont_RenderText(lua_State* L)
{
    ImFont* f = (ImFont*)self_ptr(L, CLS_ImFont);
    ImDrawList* dl = (ImDrawList*)opt_obj(L, 2, CLS_ImDrawList);
    if (!f || !dl || guarded())
        return 0;
    size_t n;
    const char* s = opt_str(L, 7, "", &n);
    ImVec4 clip = opt_vec4(L, 6, dl->_CmdHeader.ClipRect);
    f->RenderText(dl, (float)opt_num(L, 3, ImGui::GetFontSize()), opt_vec2(L, 4, ImVec2(0, 0)), opt_u32(L, 5, IM_COL32_WHITE), clip, s,
                  s + n, (float)opt_num(L, 9, 0), !is_none(L, 10) && lua_toboolean(L, 10));
    return 0;
}

// ---- Style/colour.

// GetColorU32(idx[, alpha_mul]) | GetColorU32({r, g, b, a}) | GetColorU32(col_u32[, alpha_mul]).
int hand_GetColorU32(lua_State* L)
{
    if (!ImGui::GetCurrentContext())
        return ret_num0(L);
    ImU32 r;
    if (lua_istable(L, 1) || to_obj(L, 1))
        r = ImGui::GetColorU32(opt_vec4(L, 1, ImVec4(0, 0, 0, 0)));
    else
    {
        double v = opt_num(L, 1, 0);
        float mul = (float)opt_num(L, 2, 1.0);
        if (v >= 0 && v < ImGuiCol_COUNT && v == (double)(int)v)
            r = ImGui::GetColorU32((ImGuiCol)(int)v, mul);
        else
            r = ImGui::GetColorU32((ImU32)to_i64(v), mul);
    }
    lua_pushnumber(L, (lua_Number)r);
    return 1;
}

int hand_ColorConvertRGBtoHSV(lua_State* L)
{
    float h = 0, s = 0, v = 0;
    ImGui::ColorConvertRGBtoHSV((float)opt_num(L, 1, 0), (float)opt_num(L, 2, 0), (float)opt_num(L, 3, 0), h, s, v);
    lua_pushnumber(L, h);
    lua_pushnumber(L, s);
    lua_pushnumber(L, v);
    return 3;
}

int hand_ColorConvertHSVtoRGB(lua_State* L)
{
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB((float)opt_num(L, 1, 0), (float)opt_num(L, 2, 0), (float)opt_num(L, 3, 0), r, g, b);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    return 3;
}

int hand_ColorPicker4(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    float col[4] = {0, 0, 0, 1}, orig[4];
    read_arr(L, 2, col, 4);
    memcpy(orig, col, sizeof(col));
    float ref[4] = {0, 0, 0, 1};
    bool have_ref = lua_istable(L, 4) || to_obj(L, 4);
    if (have_ref)
        read_arr(L, 4, ref, 4);
    bool r = ImGui::ColorPicker4(label, col, (ImGuiColorEditFlags)opt_int(L, 3, 0), have_ref ? ref : nullptr);
    if (memcmp(col, orig, sizeof(col)) != 0)
        write_arr(L, 2, col, 4);
    lua_pushboolean(L, r);
    return 1;
}

// ---- IDs.

// PushID/GetID: string | (string, string) | number | userdata/table (by pointer identity).
template <class Fn> auto with_id(lua_State* L, Fn fn)
{
    switch (lua_type(L, 1))
    {
    case LUA_TSTRING:
    {
        size_t n;
        const char* s = lua_tolstring(L, 1, &n);
        return fn(s, s + n, nullptr, 0, 0);
    }
    case LUA_TNUMBER:
        return fn(nullptr, nullptr, nullptr, (int)to_i64(lua_tonumber(L, 1)), 1);
    case LUA_TBOOLEAN:
        return fn(nullptr, nullptr, nullptr, lua_toboolean(L, 1) ? 1 : 0, 1);
    case LUA_TNIL:
    case LUA_TNONE:
        return fn("", nullptr, nullptr, 0, 0);
    default:
        return fn(nullptr, nullptr, lua_topointer(L, 1) ? lua_topointer(L, 1) : opt_rawptr(L, 1), 0, 2);
    }
}

int hand_PushID(lua_State* L)
{
    if (guarded())
        return 0;
    with_id(L, [](const char* b, const char* e, const void* p, int i, int kind) {
        if (kind == 0)
            ImGui::PushID(b, e);
        else if (kind == 1)
            ImGui::PushID(i);
        else
            ImGui::PushID(p);
        return 0;
    });
    return 0;
}

int hand_GetID(lua_State* L)
{
    if (guarded())
        return ret_num0(L);
    ImGuiID id = with_id(L, [](const char* b, const char* e, const void* p, int i, int kind) {
        if (kind == 0)
            return ImGui::GetID(b, e);
        if (kind == 1)
            return ImGui::GetID(i);
        return ImGui::GetID(p);
    });
    lua_pushnumber(L, (lua_Number)id);
    return 1;
}

// ---- Widgets.

// CheckboxFlags(label, {flags, flags_value}) or CheckboxFlags(label, {flags}, flags_value).
int hand_CheckboxFlags(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    int64_t flags = 0, value = 0;
    if (lua_istable(L, 2))
    {
        lua_rawgeti(L, 2, 1);
        flags = opt_int(L, -1, 0);
        lua_rawgeti(L, 2, 2);
        value = opt_int(L, -1, 0);
        lua_pop(L, 2);
    }
    if (lua_type(L, 3) == LUA_TNUMBER)
        value = opt_int(L, 3, 0);
    int f = (int)flags;
    bool r = ImGui::CheckboxFlags(label, &f, (int)value);
    if (f != (int)flags)
        set_t1_num(L, 2, (double)f);
    lua_pushboolean(L, r);
    return 1;
}

// Combo(label, {cur}, items_table, count[, popup_max]) | Combo(label, {cur}, 'a\0b\0\0'[, popup_max]).
int hand_Combo(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    int cur = 0;
    int* pcur = get_pnum<int>(L, 2, &cur, false);
    const int orig = cur;
    bool r = false;
    if (lua_type(L, 3) == LUA_TSTRING)
    {
        size_t len;
        const char* s = lua_tolstring(L, 3, &len);
        std::string z(s, len);
        z.append(2, '\0'); // ImGui scans to a double NUL
        r = ImGui::Combo(label, pcur, z.c_str(), (int)opt_int(L, 4, -1));
    }
    else
    {
        std::vector<const char*> items;
        int count = read_items(L, 3, (int)opt_int(L, 4, -1), items);
        r = ImGui::Combo(label, pcur, items.data(), count, (int)opt_int(L, 5, -1));
    }
    if (cur != orig)
        set_t1_num(L, 2, cur);
    lua_pushboolean(L, r);
    return 1;
}

int hand_ListBox(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    int cur = 0;
    int* pcur = get_pnum<int>(L, 2, &cur, false);
    const int orig = cur;
    std::vector<const char*> items;
    int count = read_items(L, 3, (int)opt_int(L, 4, -1), items);
    bool r = ImGui::ListBox(label, pcur, items.data(), count, (int)opt_int(L, 5, -1));
    if (cur != orig)
        set_t1_num(L, 2, cur);
    lua_pushboolean(L, r);
    return 1;
}

// Scalar widgets. kind: 0 Drag, 1 Slider, 2 Input; n > 1 for the ...N variants.
int scalar_widget(lua_State* L, int kind, bool multi, bool vertical)
{
    if (guarded())
        return ret_false(L);
    int a = 1;
    size_t ln;
    const char* label = opt_str(L, a++, "", &ln);
    ImVec2 size = vertical ? opt_vec2(L, a++, ImVec2(0, 0)) : ImVec2(0, 0);
    ImGuiDataType dt = opt_dt(L, a++);
    int data_idx = a++;
    int comps = multi ? (int)ImClamp<int64_t>(opt_int(L, a++, 1), 1, 64) : 1;
    std::vector<Scalar> data(comps), orig(comps);
    scalars_read(L, data_idx, dt, data.data(), comps);
    orig = data;
    bool r = false;
    Scalar t1{}, t2{};
    if (kind == 0)
    {
        float speed = (float)opt_num(L, a++, 1.0);
        const void* mn = opt_scalar(L, a++, dt, &t1);
        const void* mx = opt_scalar(L, a++, dt, &t2);
        size_t fn;
        const char* fmt = opt_str(L, a++, nullptr, &fn);
        ImGuiSliderFlags flags = (ImGuiSliderFlags)opt_int(L, a++, 0);
        r = multi ? ImGui::DragScalarN(label, dt, data.data(), comps, speed, mn, mx, fmt, flags)
                  : ImGui::DragScalar(label, dt, data.data(), speed, mn, mx, fmt, flags);
    }
    else if (kind == 1)
    {
        Scalar zero{};
        scalar_set(dt, 0, zero.b);
        const void* mn = opt_scalar(L, a++, dt, &t1);
        const void* mx = opt_scalar(L, a++, dt, &t2);
        if (!mn)
            mn = zero.b; // ImGui requires both bounds for sliders
        if (!mx)
            mx = zero.b;
        size_t fn;
        const char* fmt = opt_str(L, a++, nullptr, &fn);
        ImGuiSliderFlags flags = (ImGuiSliderFlags)opt_int(L, a++, 0);
        if (vertical)
            r = ImGui::VSliderScalar(label, size, dt, data.data(), mn, mx, fmt, flags);
        else
            r = multi ? ImGui::SliderScalarN(label, dt, data.data(), comps, mn, mx, fmt, flags)
                      : ImGui::SliderScalar(label, dt, data.data(), mn, mx, fmt, flags);
    }
    else
    {
        const void* st = opt_scalar(L, a++, dt, &t1);
        const void* sf = opt_scalar(L, a++, dt, &t2);
        size_t fn;
        const char* fmt = opt_str(L, a++, nullptr, &fn);
        ImGuiInputTextFlags flags = (ImGuiInputTextFlags)opt_int(L, a++, 0);
        r = multi ? ImGui::InputScalarN(label, dt, data.data(), comps, st, sf, fmt, flags)
                  : ImGui::InputScalar(label, dt, data.data(), st, sf, fmt, flags);
    }
    scalars_write(L, data_idx, dt, data.data(), orig.data(), comps);
    lua_pushboolean(L, r);
    return 1;
}

int hand_DragScalar(lua_State* L) { return scalar_widget(L, 0, false, false); }
int hand_DragScalarN(lua_State* L) { return scalar_widget(L, 0, true, false); }
int hand_SliderScalar(lua_State* L) { return scalar_widget(L, 1, false, false); }
int hand_SliderScalarN(lua_State* L) { return scalar_widget(L, 1, true, false); }
int hand_VSliderScalar(lua_State* L) { return scalar_widget(L, 1, false, true); }
int hand_InputScalar(lua_State* L) { return scalar_widget(L, 2, false, false); }
int hand_InputScalarN(lua_State* L) { return scalar_widget(L, 2, true, false); }

// InputText family: buffer = { 'text' } table, buffer_size = bytes including the NUL.
struct TextCtx
{
    lua_State* L;
    int fn; // stack index of the Lua callback, 0 when none
    std::vector<char>* buf;
};

int text_callback(ImGuiInputTextCallbackData* d)
{
    TextCtx* c = (TextCtx*)d->UserData;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        c->buf->resize((size_t)d->BufSize + 1);
        d->Buf = c->buf->data();
        return 0;
    }
    if (!c->fn)
        return 0;
    lua_State* L = c->L;
    lua_pushvalue(L, c->fn);
    push_obj(L, d, CLS_ImGuiInputTextCallbackData);
    if (lua_pcall(L, 1, 1, 0) != 0)
    {
        log_callback_error(L, "InputText");
        return 0;
    }
    int r = lua_isnumber(L, -1) ? (int)lua_tonumber(L, -1) : (lua_toboolean(L, -1) ? 1 : 0);
    lua_pop(L, 1);
    return r;
}

int input_text(lua_State* L, int variant)
{
    if (guarded())
        return ret_false(L);
    int a = 1;
    size_t n;
    const char* label = opt_str(L, a++, "", &n);
    const char* hint = variant == 2 ? opt_str(L, a++, "", &n) : nullptr;
    int buf_idx = a++;
    int size = (int)opt_int(L, a++, 0);
    ImVec2 box = variant == 1 ? opt_vec2(L, a++, ImVec2(0, 0)) : ImVec2(0, 0);
    ImGuiInputTextFlags flags = (ImGuiInputTextFlags)opt_int(L, a++, 0);
    int fn_idx = lua_isfunction(L, a) ? a : 0;

    std::string orig;
    if (lua_istable(L, buf_idx))
    {
        lua_rawgeti(L, buf_idx, 1);
        if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
        {
            size_t len;
            const char* s = lua_tolstring(L, -1, &len);
            orig.assign(s, strnlen(s, len));
        }
        lua_pop(L, 1);
    }
    else if (lua_type(L, buf_idx) == LUA_TSTRING)
        orig = lua_tostring(L, buf_idx);
    if (size <= 0)
        size = (int)ImMax<size_t>(256, orig.size() + 1);
    std::vector<char> buf((size_t)size, '\0');
    memcpy(buf.data(), orig.data(), ImMin(orig.size(), (size_t)size - 1));

    TextCtx ctx = {L, fn_idx, &buf};
    ImGuiInputTextCallback cb = (fn_idx || (flags & ImGuiInputTextFlags_CallbackResize)) ? text_callback : nullptr;
    if (!fn_idx)
        flags &= ~(ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways |
                   ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit);
    bool r;
    if (variant == 1)
        r = ImGui::InputTextMultiline(label, buf.data(), buf.size(), box, flags, cb, &ctx);
    else if (variant == 2)
        r = ImGui::InputTextWithHint(label, hint, buf.data(), buf.size(), flags, cb, &ctx);
    else
        r = ImGui::InputText(label, buf.data(), buf.size(), flags, cb, &ctx);
    const char* now = buf.data();
    size_t now_len = strnlen(now, buf.size());
    if (lua_istable(L, buf_idx) && (now_len != orig.size() || memcmp(now, orig.data(), now_len) != 0))
    {
        lua_pushlstring(L, now, now_len);
        lua_rawseti(L, buf_idx, 1);
    }
    lua_pushboolean(L, r);
    return 1;
}

int hand_InputText(lua_State* L) { return input_text(L, 0); }
int hand_InputTextMultiline(lua_State* L) { return input_text(L, 1); }
int hand_InputTextWithHint(lua_State* L) { return input_text(L, 2); }

// PlotLines/PlotHistogram(label, values, count, offset, overlay, scale_min, scale_max, graph_size, stride)
int plot(lua_State* L, bool hist)
{
    if (guarded())
        return 0;
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    std::vector<float> v;
    if (lua_istable(L, 2))
    {
        int len = (int)lua_objlen(L, 2);
        int count = (int)opt_int(L, 3, len);
        if (count < 0 || count > len)
            count = len;
        v.resize(count);
        for (int i = 0; i < count; i++)
        {
            lua_rawgeti(L, 2, i + 1);
            v[i] = (float)opt_num(L, -1, 0);
            lua_pop(L, 1);
        }
    }
    int offset = (int)opt_int(L, 4, 0);
    const char* overlay = opt_str(L, 5, nullptr, &n);
    float smin = (float)opt_num(L, 6, FLT_MAX), smax = (float)opt_num(L, 7, FLT_MAX);
    ImVec2 gs = opt_vec2(L, 8, ImVec2(0, 0));
    // stride (arg 9) describes C memory; a Lua list is always contiguous floats here.
    if (hist)
        ImGui::PlotHistogram(label, v.data(), (int)v.size(), offset, overlay, smin, smax, gs, sizeof(float));
    else
        ImGui::PlotLines(label, v.data(), (int)v.size(), offset, overlay, smin, smax, gs, sizeof(float));
    return 0;
}

int hand_PlotLines(lua_State* L) { return plot(L, false); }
int hand_PlotHistogram(lua_State* L) { return plot(L, true); }

// Value(prefix, bool) | Value(prefix, number[, float_format]) | Value(prefix, {v}).
int hand_Value(lua_State* L)
{
    if (guarded())
        return 0;
    size_t n;
    const char* prefix = opt_str(L, 1, "", &n);
    int vi = 2;
    if (lua_istable(L, 2))
    {
        lua_rawgeti(L, 2, 1);
        vi = lua_gettop(L);
    }
    if (lua_type(L, vi) == LUA_TBOOLEAN)
        ImGui::Value(prefix, lua_toboolean(L, vi) != 0);
    else
    {
        double v = opt_num(L, vi, 0);
        const char* fmt = opt_str(L, 3, nullptr, &n);
        if (!fmt && v == (double)(int64_t)v && v >= INT32_MIN && v <= UINT32_MAX)
        {
            if (v < 0 || v <= INT32_MAX)
                ImGui::Value(prefix, (int)v);
            else
                ImGui::Value(prefix, (unsigned int)v);
        }
        else
            ImGui::Value(prefix, (float)v, fmt);
    }
    return 0;
}

int hand_SetDragDropPayload(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* type = opt_str(L, 1, "", &n);
    const void* data = nullptr;
    size_t sz = 0;
    double num = 0;
    switch (lua_type(L, 2))
    {
    case LUA_TSTRING:
        data = lua_tolstring(L, 2, &sz);
        break;
    case LUA_TNUMBER:
        num = lua_tonumber(L, 2);
        data = &num;
        sz = sizeof(num);
        break;
    case LUA_TNIL:
    case LUA_TNONE:
        break;
    default:
        data = opt_rawptr(L, 2);
        sz = 0;
        break;
    }
    if (lua_type(L, 3) == LUA_TNUMBER && data)
    {
        size_t want = (size_t)ImMax<int64_t>(0, opt_int(L, 3, 0));
        sz = (lua_type(L, 2) == LUA_TSTRING || lua_type(L, 2) == LUA_TNUMBER) ? ImMin(sz, want) : want;
    }
    if (!data)
        sz = 0;
    lua_pushboolean(L, ImGui::SetDragDropPayload(type, sz ? data : nullptr, sz, (ImGuiCond)opt_int(L, 4, 0)));
    return 1;
}

int hand_BeginMenuEx(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    const char* icon = opt_str(L, 2, nullptr, &n);
    lua_pushboolean(L, ImGui::BeginMenuEx(label, icon, opt_bool(L, 3, true)));
    return 1;
}

int hand_MenuItemEx(lua_State* L)
{
    if (guarded())
        return ret_false(L);
    size_t n;
    const char* label = opt_str(L, 1, "", &n);
    const char* icon = opt_str(L, 2, nullptr, &n);
    const char* shortcut = opt_str(L, 3, nullptr, &n);
    lua_pushboolean(L, ImGui::MenuItemEx(label, icon, shortcut, opt_bool(L, 4, false), opt_bool(L, 5, true)));
    return 1;
}

// ---- Settings, memory.

int hand_SaveIniSettingsToMemory(lua_State* L)
{
    if (!ImGui::GetCurrentContext())
        return 0;
    size_t sz = 0;
    const char* s = ImGui::SaveIniSettingsToMemory(&sz);
    set_t1_num(L, 1, (double)sz);
    lua_pushlstring(L, s ? s : "", s ? sz : 0);
    return 1;
}

int hand_MemAlloc(lua_State* L)
{
    size_t sz = (size_t)ImMax<int64_t>(0, opt_int(L, 1, 0));
    void* p = ImGui::GetCurrentContext() && sz ? ImGui::MemAlloc(sz) : nullptr;
    lua_pushnumber(L, (lua_Number)(uintptr_t)p);
    return 1;
}

int hand_MemFree(lua_State* L)
{
    void* p = opt_rawptr(L, 1);
    if (p && ImGui::GetCurrentContext())
        ImGui::MemFree(p);
    return 0;
}

// ---- Draw list polygons: points = { {x, y}, ... }.

int poly(lua_State* L, int which)
{
    ImDrawList* dl = (ImDrawList*)self_ptr(L, CLS_ImDrawList);
    if (!dl || guarded() || !lua_istable(L, 2))
        return 0;
    int len = (int)lua_objlen(L, 2);
    int count = (int)opt_int(L, 3, len);
    if (count < 0 || count > len)
        count = len;
    std::vector<ImVec2> pts((size_t)count);
    for (int i = 0; i < count; i++)
    {
        lua_rawgeti(L, 2, i + 1);
        pts[i] = opt_vec2(L, -1, ImVec2(0, 0));
        lua_pop(L, 1);
    }
    ImU32 col = opt_u32(L, 4, 0);
    if (which == 0)
        dl->AddPolyline(pts.data(), count, col, (ImDrawFlags)opt_int(L, 5, 0), (float)opt_num(L, 6, 1.0));
    else if (which == 1)
        dl->AddConvexPolyFilled(pts.data(), count, col);
    else
        dl->AddConcavePolyFilled(pts.data(), count, col);
    return 0;
}

int hand_ImDrawList_AddPolyline(lua_State* L) { return poly(L, 0); }
int hand_ImDrawList_AddConvexPolyFilled(lua_State* L) { return poly(L, 1); }
int hand_ImDrawList_AddConcavePolyFilled(lua_State* L) { return poly(L, 2); }

} // namespace

// =====================================================================================================
// Generated bindings and tables.

namespace {

#define XI_GUI_GEN_BODY
#include "gui_lua_gen.inc"
#undef XI_GUI_GEN_BODY

// ---- Object metatables.

int obj_index(lua_State* L)
{
    Obj* o = to_obj(L, 1);
    if (!o)
        return 0;
    if (o->ops)
    {
        if (lua_type(L, 2) == LUA_TNUMBER && o->p)
        {
            int i = (int)lua_tonumber(L, 2);
            if (i >= 1 && i <= o->ops->len(o->p))
            {
                o->ops->get(L, o->p, i - 1);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        int i = (int)lua_tonumber(L, 2);
        if (!o->p || i < 1)
            lua_pushnil(L);
        else if (o->cls == CLS_ImVec2 || o->cls == CLS_ImVec4)
        {
            if (i <= (o->cls == CLS_ImVec2 ? 2 : 4))
                lua_pushnumber(L, ((float*)o->p)[i - 1]);
            else
                lua_pushnil(L);
        }
        else if (i == 1)
            lua_pushvalue(L, 1);
        else if (o->cls == CLS_ImGuiTableColumnSortSpecs) // sort_specs.Specs[i]: pointer to SpecsCount entries
            push_obj(L, (char*)o->p + (size_t)(i - 1) * sizeof(ImGuiTableColumnSortSpecs), o->cls);
        else
            lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1)); // methods
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(2)); // fields
    if (lua_islightuserdata(L, -1) && o->p)
    {
        const FieldInfo* fi = (const FieldInfo*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        fi->get(L, o->p);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int obj_newindex(lua_State* L)
{
    Obj* o = to_obj(L, 1);
    if (!o || !o->p)
        return 0;
    if (o->ops)
    {
        if (lua_type(L, 2) == LUA_TNUMBER)
        {
            int i = (int)lua_tonumber(L, 2);
            if (i >= 1 && i <= o->ops->len(o->p))
                o->ops->set(L, o->p, i - 1, 3);
        }
        return 0;
    }
    if (lua_type(L, 2) == LUA_TNUMBER && (o->cls == CLS_ImVec2 || o->cls == CLS_ImVec4))
    {
        int i = (int)lua_tonumber(L, 2);
        if (i >= 1 && i <= (o->cls == CLS_ImVec2 ? 2 : 4) && lua_isnumber(L, 3))
            ((float*)o->p)[i - 1] = (float)lua_tonumber(L, 3);
        return 0;
    }
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1)); // fields
    if (lua_islightuserdata(L, -1))
    {
        const FieldInfo* fi = (const FieldInfo*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        fi->set(L, o->p, 3);
    }
    return 0; // unknown fields (newer ImGui members some addons list) are ignored
}

int obj_eq(lua_State* L)
{
    Obj *a = to_obj(L, 1), *b = to_obj(L, 2);
    lua_pushboolean(L, a && b && a->p == b->p && a->cls == b->cls);
    return 1;
}

int obj_len(lua_State* L)
{
    Obj* o = to_obj(L, 1);
    int n = 0;
    if (o && o->ops && o->p)
        n = o->ops->len(o->p);
    else if (o && o->cls == CLS_ImVec2)
        n = 2;
    else if (o && o->cls == CLS_ImVec4)
        n = 4;
    lua_pushnumber(L, n);
    return 1;
}

int obj_tostring(lua_State* L)
{
    Obj* o = to_obj(L, 1);
    const char* name = !o ? "?" : o->ops ? "array" : g_classes[o->cls].name;
    if (o && !o->ops && o->p && o->cls == CLS_ImVec2)
        lua_pushfstring(L, "ImVec2(%f, %f)", (double)((float*)o->p)[0], (double)((float*)o->p)[1]);
    else if (o && !o->ops && o->p && o->cls == CLS_ImVec4)
        lua_pushfstring(L, "ImVec4(%f, %f, %f, %f)", (double)((float*)o->p)[0], (double)((float*)o->p)[1], (double)((float*)o->p)[2],
                        (double)((float*)o->p)[3]);
    else
        lua_pushfstring(L, "%s: %p", name, o ? o->p : nullptr);
    return 1;
}

void ensure_meta(lua_State* L)
{
    lua_newtable(L); // all metatables
    // Shared metamethod function objects (Lua 5.1 __eq requires the very same function on both sides).
    lua_pushcfunction(L, obj_eq);
    int eq = lua_gettop(L);
    lua_pushcfunction(L, obj_len);
    int len = lua_gettop(L);
    lua_pushcfunction(L, obj_tostring);
    int tostr = lua_gettop(L);
    for (int c = 0; c <= CLS_ARRAY; c++)
    {
        lua_newtable(L); // mt
        lua_newtable(L); // methods
        lua_newtable(L); // fields
        if (c < CLS_COUNT)
        {
            for (const luaL_Reg* m = g_classes[c].methods; m->name; m++)
            {
                lua_pushcfunction(L, m->func);
                lua_setfield(L, -3, m->name);
            }
            for (const FieldInfo* f = g_classes[c].fields; f->name; f++)
            {
                lua_pushlightuserdata(L, (void*)f);
                lua_setfield(L, -2, f->name);
            }
        }
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, obj_index, 2);
        lua_setfield(L, -4, "__index");
        lua_pushcclosure(L, obj_newindex, 1); // consumes fields
        lua_setfield(L, -3, "__newindex");
        lua_pop(L, 1); // methods
        lua_pushvalue(L, eq);
        lua_setfield(L, -2, "__eq");
        lua_pushvalue(L, len);
        lua_setfield(L, -2, "__len");
        lua_pushvalue(L, tostr);
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, c < CLS_COUNT ? g_classes[c].name : "array");
        lua_setfield(L, -2, "__name");
        lua_rawseti(L, eq - 1, c + 1);
    }
    lua_pop(L, 3);
    lua_pushlightuserdata(L, &g_key_meta);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

} // namespace

// =====================================================================================================

extern "C" void xi_gui_lua_push_manager(lua_State* L)
{
    lua_pushlightuserdata(L, &g_key_mgr);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &g_key_meta);
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool have_meta = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!have_meta)
        ensure_meta(L);
    lua_newtable(L);
    for (const luaL_Reg* f = g_manager_funcs; f->name; f++)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_pushlightuserdata(L, &g_key_mgr);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

extern "C" void xi_gui_lua_forget_state(lua_State* L)
{
    if (g_sizecb_state == L)
        g_sizecb_state = nullptr;
}

extern "C" int xi_gui_lua_visible(void) { return g_visible; }
extern "C" void xi_gui_lua_set_visible(int visible) { g_visible = visible ? 1 : 0; }
