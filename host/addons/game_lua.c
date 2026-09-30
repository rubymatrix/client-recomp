/* Lua access to the game's live structures (game.h): xi.game.
 *
 * xi_game_open(L) pushes a table of primitives; the Ashita layer (lua/ashita_memory.lua) and the
 * Windower layer shape them. Everything reads guest memory at call time (no snapshots), and
 * returns nil when the structure isn't there (before login, while zoning, no cast bar...).
 *
 * `field` arguments are a path string in the structure ("ServerId", "Movement.LocalPosition.X",
 * "Look.Hair", "AbilityInfo[3].Recast") or a number from path(struct, path), which skips the
 * lookup (the Lua layers cache these). `sub` subscripts an array field (0-based). Values come
 * back as numbers; char arrays as strings (to the first NUL), uint8_t arrays as raw byte strings,
 * other arrays read without `sub` as 1-based tables, nested structs as their guest address plus
 * the struct's name. Addresses in and out are guest addresses (host addresses are accepted too).
 *
 *   descriptors  structs() -> {name, ...}      struct_info(name) -> {size=, fields={...}}
 *                path(struct, path) -> id|nil
 *   generic      read(struct, addr, field [, sub])          write(struct, addr, field, value [, sub])
 *                base(kind [, i [, j]]) -> guest address|nil   kinds: entity(i) party party_member(i)
 *                  alliance party_icons player target target_window char inventory item(c, s)
 *                  treasure(i) autofollow castbar recast_abilities recast_spells
 *   entities     entity(i, field [, sub])    set_entity(i, field, value [, sub])
 *                entity_count()  player_index()  entity_by_server_id(id)
 *   party        party_member(i, field [, sub])  set_party_member(i, field, value [, sub])
 *                alliance(field)  set_alliance(field, value)
 *                party_icons(member) -> {32 ids}   party_icons_field(member, field)
 *   player       player(field [, sub])  set_player(field, value [, sub])
 *                job_level(job)  master_level(job)  master_flags()  login_status()
 *                key_item(id)  key_item_seen(id)  spell_known(id)  has_spell_data()
 *                ability_known(bit)  has_ability_data()  pet() -> mpp, tp, index
 *   target       target(field [, sub])  set_target(...)  target_window(field [, sub])  set_target_window(...)
 *                set_target_index(i): target entity i as the game does (a guest call)
 *   inventory    inventory(field [, sub])  set_inventory(...)  item(c, slot, field [, sub])  set_item(...)
 *                treasure(i, field [, sub])  container_count(c)  container_max(c)  equipment(slot) -> slot, index
 *   misc         autofollow(field)  set_autofollow(field, value)  castbar(field)  set_castbar(field, value)
 *                ability_recast(i) -> timer, timer_id, recast, calc1, calc2
 *                spell_recast(id) -> timer (1/60 s)
 *                host_addr(guest)  pointers() -> {name = value}
 */
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

#include "game.h"
#include "xi.h"

int xi_game_open(lua_State* L);

static const char* const type_names[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "f32", "ptr", "chars", "bytes", "struct", "bits"};

/* ---- helpers --------------------------------------------------------------------------------- */

static uint32_t arg_addr(lua_State* L, int i)
{
    if (lua_isnoneornil(L, i))
        return 0;
    int ok = 0;
    uint32_t a = xi_guest_addr(luaL_checknumber(L, i), &ok);
    return ok ? a : 0;
}

static int32_t arg_sub(lua_State* L, int i)
{
    return lua_isnoneornil(L, i) ? -1 : (int32_t)luaL_checkinteger(L, i);
}

/* The field named by the argument at `i` in struct `sid` (a path id or a path string). */
static const xi_field* arg_field(lua_State* L, int i, int sid, uint32_t* extra)
{
    *extra = 0;
    if (lua_type(L, i) == LUA_TNUMBER)
    {
        int fsid;
        const xi_field* f = xi_game_path_field((int)lua_tointeger(L, i), extra, &fsid);
        return f && (sid < 0 || fsid == sid) ? f : NULL;
    }
    const char* p = luaL_checkstring(L, i);
    return xi_game_lookup(sid, p, extra);
}

static int push_value(lua_State* L, const xi_field* f, uint32_t base, uint32_t extra, int32_t sub)
{
    xi_value v;
    switch (f ? xi_game_read(f, base, extra, sub, &v) : XI_V_NIL)
    {
    case XI_V_NUM:
        lua_pushnumber(L, v.num);
        return 1;
    case XI_V_STR:
        lua_pushlstring(L, v.str, v.len);
        return 1;
    case XI_V_ADDR:
        lua_pushnumber(L, v.num);
        lua_pushstring(L, xi_game_struct(v.sid)->name);
        return 2;
    case XI_V_ARRAY:
        lua_createtable(L, (int)v.len, 0);
        for (uint32_t k = 0; k < v.len; k++)
        {
            xi_value e;
            if (xi_game_read(f, base, extra, (int32_t)k, &e) == XI_V_NUM)
                lua_pushnumber(L, e.num);
            else
                lua_pushnumber(L, 0);
            lua_rawseti(L, -2, (int)k + 1);
        }
        return 1;
    default:
        lua_pushnil(L);
        return 1;
    }
}

static int write_value(lua_State* L, const xi_field* f, uint32_t base, uint32_t extra, int vi, int32_t sub)
{
    int ok = 0;
    if (f && base)
    {
        switch (lua_type(L, vi))
        {
        case LUA_TNUMBER:
            ok = xi_game_write_num(f, base, extra, sub, lua_tonumber(L, vi));
            break;
        case LUA_TBOOLEAN:
            ok = xi_game_write_num(f, base, extra, sub, lua_toboolean(L, vi) ? 1 : 0);
            break;
        case LUA_TSTRING:
        {
            size_t n;
            const char* s = lua_tolstring(L, vi, &n);
            ok = xi_game_write_str(f, base, extra, s, n);
            break;
        }
        case LUA_TTABLE: /* a byte/number array, 1-based */
        {
            ok = 1;
            for (uint32_t k = 0; k < f->count; k++)
            {
                lua_rawgeti(L, vi, (int)k + 1);
                if (lua_type(L, -1) == LUA_TNUMBER)
                    ok &= xi_game_write_num(f, base, extra, (int32_t)k, lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            break;
        }
        default:
            break;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

static int sid_of(const char* name)
{
    return xi_game_struct_id(name);
}

/* get/set over a base address, fields of struct `sname`; the field argument is at `fi`. */
static int get_at(lua_State* L, const char* sname, uint32_t base, int fi)
{
    uint32_t extra;
    const xi_field* f = arg_field(L, fi, sid_of(sname), &extra);
    if (!f)
        return luaL_error(L, "%s has no field '%s'", sname, lua_tostring(L, fi));
    if (base == 0)
    {
        lua_pushnil(L);
        return 1;
    }
    return push_value(L, f, base, extra, arg_sub(L, fi + 1));
}

static int set_at(lua_State* L, const char* sname, uint32_t base, int fi)
{
    uint32_t extra;
    const xi_field* f = arg_field(L, fi, sid_of(sname), &extra);
    if (!f)
        return luaL_error(L, "%s has no field '%s'", sname, lua_tostring(L, fi));
    return write_value(L, f, base, extra, fi + 1, arg_sub(L, fi + 2));
}

static uint32_t index_arg(lua_State* L, int i)
{
    lua_Number n = luaL_checknumber(L, i);
    return n >= 0 && n < 4294967296.0 ? (uint32_t)n : 0xFFFFFFFFu;
}

/* ---- descriptors ----------------------------------------------------------------------------- */

static int l_structs(lua_State* L)
{
    int n = xi_game_struct_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++)
    {
        lua_pushstring(L, xi_game_struct(i)->name);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_struct_info(lua_State* L)
{
    int sid = sid_of(luaL_checkstring(L, 1));
    const xi_struct* s = xi_game_struct(sid);
    if (!s)
        return 0;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, s->name);
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, s->size);
    lua_setfield(L, -2, "size");
    lua_createtable(L, s->nfields, 0);
    for (unsigned i = 0; i < s->nfields; i++)
    {
        const xi_field* f = xi_game_field_at(s->first + i);
        lua_createtable(L, 0, 9);
        lua_pushstring(L, f->name);
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, f->offset);
        lua_setfield(L, -2, "offset");
        lua_pushstring(L, type_names[f->type]);
        lua_setfield(L, -2, "type");
        lua_pushstring(L, type_names[f->elem]);
        lua_setfield(L, -2, "elem");
        lua_pushnumber(L, f->count);
        lua_setfield(L, -2, "count");
        lua_pushnumber(L, f->stride);
        lua_setfield(L, -2, "stride");
        if (f->type == XI_T_BITS)
        {
            lua_pushnumber(L, f->bit_lo);
            lua_setfield(L, -2, "bit_lo");
            lua_pushnumber(L, f->bit_width);
            lua_setfield(L, -2, "bit_width");
        }
        if (f->sub >= 0)
        {
            lua_pushstring(L, xi_game_struct(f->sub)->name);
            lua_setfield(L, -2, "struct");
        }
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "fields");
    return 1;
}

static int l_path(lua_State* L)
{
    int sid = sid_of(luaL_checkstring(L, 1));
    int id = xi_game_path_id(sid, luaL_checkstring(L, 2));
    if (id < 0)
        return 0;
    lua_pushinteger(L, id);
    return 1;
}

static int l_read(lua_State* L)
{
    const char* sname = luaL_optstring(L, 1, NULL);
    uint32_t base = arg_addr(L, 2), extra;
    const xi_field* f = arg_field(L, 3, sname ? sid_of(sname) : -1, &extra);
    if (!f || base == 0)
        return 0;
    return push_value(L, f, base, extra, arg_sub(L, 4));
}

static int l_write(lua_State* L)
{
    const char* sname = luaL_optstring(L, 1, NULL);
    uint32_t base = arg_addr(L, 2), extra;
    const xi_field* f = arg_field(L, 3, sname ? sid_of(sname) : -1, &extra);
    return write_value(L, f, base, extra, 4, arg_sub(L, 5));
}

/* inventory_t offsets the helpers need, from the descriptors */
static uint32_t g_ofs_treasure, g_size_treasure, g_ofs_equipment;

static void init_offsets(void)
{
    if (g_size_treasure)
        return;
    uint32_t extra;
    const xi_field* f = xi_game_lookup(sid_of("inventory_t"), "TreasurePool", &extra);
    g_ofs_treasure = f ? f->offset : 0;
    g_size_treasure = f ? f->stride : 88;
    f = xi_game_lookup(sid_of("inventory_t"), "Equipment", &extra);
    g_ofs_equipment = f ? f->offset : 0;
}

static uint32_t treasure_base(uint32_t i)
{
    init_offsets();
    uint32_t inv = xi_game_inventory();
    return inv && g_ofs_treasure && i < 10 ? inv + g_ofs_treasure + i * g_size_treasure : 0;
}

static int l_base(lua_State* L)
{
    const char* k = luaL_checkstring(L, 1);
    uint32_t a = 0;
    init_offsets();
    if (!strcmp(k, "entity")) a = xi_game_entity(index_arg(L, 2));
    else if (!strcmp(k, "party")) a = xi_game_party();
    else if (!strcmp(k, "party_member")) a = xi_game_party_member(index_arg(L, 2));
    else if (!strcmp(k, "alliance")) a = xi_game_alliance();
    else if (!strcmp(k, "party_icons")) a = xi_game_party_icons();
    else if (!strcmp(k, "player")) a = xi_game_player();
    else if (!strcmp(k, "target")) a = xi_game_target();
    else if (!strcmp(k, "target_window")) a = xi_game_target_window();
    else if (!strcmp(k, "char")) a = xi_game_char();
    else if (!strcmp(k, "inventory")) a = xi_game_inventory();
    else if (!strcmp(k, "item")) a = xi_game_container_item(index_arg(L, 2), index_arg(L, 3));
    else if (!strcmp(k, "treasure")) a = treasure_base(index_arg(L, 2));
    else if (!strcmp(k, "autofollow")) a = xi_game_autofollow();
    else if (!strcmp(k, "castbar")) a = xi_game_castbar();
    else if (!strcmp(k, "recast_abilities")) a = xi_game_ptr_value(XI_P_RECAST_ABILITY);
    else if (!strcmp(k, "recast_spells")) a = xi_game_ptr_value(XI_P_RECAST_SPELL);
    else return luaL_error(L, "game.base: unknown kind '%s'", k);
    if (a == 0)
        return 0;
    lua_pushnumber(L, a);
    return 1;
}

/* ---- entities -------------------------------------------------------------------------------- */

static int l_entity(lua_State* L) { return get_at(L, "entity_t", xi_game_entity(index_arg(L, 1)), 2); }
static int l_set_entity(lua_State* L) { return set_at(L, "entity_t", xi_game_entity(index_arg(L, 1)), 2); }

static int l_entity_count(lua_State* L)
{
    lua_pushnumber(L, xi_game_entity_count());
    return 1;
}

static int l_player_index(lua_State* L)
{
    int32_t i = xi_game_player_index();
    if (i < 0)
        return 0;
    lua_pushnumber(L, i);
    return 1;
}

static int l_entity_by_server_id(lua_State* L)
{
    int32_t i = xi_game_entity_by_server_id(index_arg(L, 1));
    if (i < 0)
        return 0;
    lua_pushnumber(L, i);
    return 1;
}

/* ---- party ----------------------------------------------------------------------------------- */

static int l_party_member(lua_State* L) { return get_at(L, "partymember_t", xi_game_party_member(index_arg(L, 1)), 2); }
static int l_set_party_member(lua_State* L) { return set_at(L, "partymember_t", xi_game_party_member(index_arg(L, 1)), 2); }
static int l_alliance(lua_State* L) { return get_at(L, "allianceinfo_t", xi_game_alliance(), 1); }
static int l_set_alliance(lua_State* L) { return set_at(L, "allianceinfo_t", xi_game_alliance(), 1); }

static int l_party_icons(lua_State* L)
{
    int16_t icons[32];
    if (!xi_game_party_member_icons(index_arg(L, 1), icons))
        return 0;
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 32; i++)
    {
        lua_pushnumber(L, icons[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_party_icons_field(lua_State* L)
{
    uint32_t p = xi_game_party_icons(), m = index_arg(L, 1);
    return get_at(L, "statusiconsentry_t", p && m < 5 ? p + m * 48 : 0, 2);
}

/* ---- player ---------------------------------------------------------------------------------- */

static int l_player(lua_State* L) { return get_at(L, "player_t", xi_game_player(), 1); }
static int l_set_player(lua_State* L) { return set_at(L, "player_t", xi_game_player(), 1); }

static int push_int_or_nil(lua_State* L, int64_t v)
{
    if (v < 0)
        return 0;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

static int push_bool_or_nil(lua_State* L, int v)
{
    if (v < 0)
        return 0;
    lua_pushboolean(L, v);
    return 1;
}

static int l_job_level(lua_State* L) { return push_int_or_nil(L, xi_game_job_level(index_arg(L, 1))); }
static int l_master_level(lua_State* L) { return push_int_or_nil(L, xi_game_master_level(index_arg(L, 1))); }
static int l_master_flags(lua_State* L) { return push_int_or_nil(L, xi_game_master_flags()); }
static int l_login_status(lua_State* L) { return push_int_or_nil(L, xi_game_login_status()); }
static int l_key_item(lua_State* L) { return push_bool_or_nil(L, xi_game_key_item(index_arg(L, 1))); }
static int l_key_item_seen(lua_State* L) { return push_bool_or_nil(L, xi_game_key_item_seen(index_arg(L, 1))); }

static int l_spell_known(lua_State* L)
{
    lua_pushboolean(L, xi_game_spell_known(index_arg(L, 1)));
    return 1;
}

static int l_has_spell_data(lua_State* L)
{
    lua_pushboolean(L, xi_game_has_spell_data());
    return 1;
}

static int l_ability_known(lua_State* L)
{
    lua_pushboolean(L, xi_game_ability_bit(index_arg(L, 1)));
    return 1;
}

static int l_has_ability_data(lua_State* L)
{
    lua_pushboolean(L, xi_game_has_ability_data());
    return 1;
}

static int l_pet(lua_State* L)
{
    uint32_t mpp, tp, idx;
    if (!xi_game_pet(&mpp, &tp, &idx))
        return 0;
    lua_pushnumber(L, mpp);
    lua_pushnumber(L, tp);
    lua_pushnumber(L, idx);
    return 3;
}

/* ---- target, inventory, the rest ------------------------------------------------------------- */

static int l_target(lua_State* L) { return get_at(L, "target_t", xi_game_target(), 1); }
static int l_set_target(lua_State* L) { return set_at(L, "target_t", xi_game_target(), 1); }
static int l_set_target_index(lua_State* L)
{
    lua_pushboolean(L, xi_game_set_target(index_arg(L, 1)));
    return 1;
}

static int l_target_window(lua_State* L) { return get_at(L, "targetwindow_t", xi_game_target_window(), 1); }
static int l_set_target_window(lua_State* L) { return set_at(L, "targetwindow_t", xi_game_target_window(), 1); }
static int l_inventory(lua_State* L) { return get_at(L, "inventory_t", xi_game_inventory(), 1); }
static int l_set_inventory(lua_State* L) { return set_at(L, "inventory_t", xi_game_inventory(), 1); }

static int l_item(lua_State* L)
{
    return get_at(L, "item_t", xi_game_container_item(index_arg(L, 1), index_arg(L, 2)), 3);
}

static int l_set_item(lua_State* L)
{
    return set_at(L, "item_t", xi_game_container_item(index_arg(L, 1), index_arg(L, 2)), 3);
}

static int l_treasure(lua_State* L)
{
    init_offsets();
    return get_at(L, "treasureitem_t", treasure_base(index_arg(L, 1)), 2);
}

static int l_container_count(lua_State* L) { return push_int_or_nil(L, xi_game_container_count(index_arg(L, 1))); }
static int l_container_max(lua_State* L) { return push_int_or_nil(L, xi_game_container_max(index_arg(L, 1))); }

static int l_equipment(lua_State* L)
{
    init_offsets();
    uint32_t inv = xi_game_inventory(), s = index_arg(L, 1), e[2];
    if (inv == 0 || !g_ofs_equipment || s >= 16 || !xi_game_rd(inv + g_ofs_equipment + s * 8, e, 8))
        return 0;
    lua_pushnumber(L, e[0]);
    lua_pushnumber(L, e[1]);
    return 2;
}

static int l_autofollow(lua_State* L) { return get_at(L, "autofollow_t", xi_game_autofollow(), 1); }
static int l_set_autofollow(lua_State* L) { return set_at(L, "autofollow_t", xi_game_autofollow(), 1); }
static int l_castbar(lua_State* L) { return get_at(L, "castbar_t", xi_game_castbar(), 1); }
static int l_set_castbar(lua_State* L) { return set_at(L, "castbar_t", xi_game_castbar(), 1); }

static int l_ability_recast(lua_State* L)
{
    uint32_t t, id, rc, c1;
    int32_t c2;
    if (!xi_game_ability_recast(index_arg(L, 1), &t, &id, &rc, &c1, &c2))
        return 0;
    lua_pushnumber(L, t);
    lua_pushnumber(L, id);
    lua_pushnumber(L, rc);
    lua_pushnumber(L, c1);
    lua_pushnumber(L, c2);
    return 5;
}

static int l_spell_recast(lua_State* L) { return push_int_or_nil(L, xi_game_spell_recast(index_arg(L, 1))); }

static int l_host_addr(lua_State* L)
{
    uint32_t a = arg_addr(L, 1);
    lua_pushnumber(L, xi_host_addr(a));
    return 1;
}

static int l_pointers(lua_State* L)
{
    lua_createtable(L, 0, XI_P_COUNT);
    for (int i = 0; i < XI_P_COUNT; i++)
    {
        lua_pushnumber(L, xi_game_ptr_value(i));
        lua_setfield(L, -2, xi_game_ptr_name(i));
    }
    return 1;
}

int xi_game_open(lua_State* L)
{
    static const luaL_Reg fns[] = {
        {"structs", l_structs},
        {"struct_info", l_struct_info},
        {"path", l_path},
        {"read", l_read},
        {"write", l_write},
        {"base", l_base},
        {"entity", l_entity},
        {"set_entity", l_set_entity},
        {"entity_count", l_entity_count},
        {"player_index", l_player_index},
        {"entity_by_server_id", l_entity_by_server_id},
        {"party_member", l_party_member},
        {"set_party_member", l_set_party_member},
        {"alliance", l_alliance},
        {"set_alliance", l_set_alliance},
        {"party_icons", l_party_icons},
        {"party_icons_field", l_party_icons_field},
        {"player", l_player},
        {"set_player", l_set_player},
        {"job_level", l_job_level},
        {"master_level", l_master_level},
        {"master_flags", l_master_flags},
        {"login_status", l_login_status},
        {"key_item", l_key_item},
        {"key_item_seen", l_key_item_seen},
        {"spell_known", l_spell_known},
        {"has_spell_data", l_has_spell_data},
        {"ability_known", l_ability_known},
        {"has_ability_data", l_has_ability_data},
        {"pet", l_pet},
        {"target", l_target},
        {"set_target", l_set_target},
        {"set_target_index", l_set_target_index},
        {"target_window", l_target_window},
        {"set_target_window", l_set_target_window},
        {"inventory", l_inventory},
        {"set_inventory", l_set_inventory},
        {"item", l_item},
        {"set_item", l_set_item},
        {"treasure", l_treasure},
        {"container_count", l_container_count},
        {"container_max", l_container_max},
        {"equipment", l_equipment},
        {"autofollow", l_autofollow},
        {"set_autofollow", l_set_autofollow},
        {"castbar", l_castbar},
        {"set_castbar", l_set_castbar},
        {"ability_recast", l_ability_recast},
        {"spell_recast", l_spell_recast},
        {"host_addr", l_host_addr},
        {"pointers", l_pointers},
        {NULL, NULL},
    };
    lua_createtable(L, 0, (int)(sizeof fns / sizeof fns[0]));
    luaL_register(L, NULL, fns);
    return 1;
}
