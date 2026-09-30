/* Lua access to the game resources (res.h).
 *
 * xi_res_open(L) pushes a table of functions. Records come back as fresh tables (cache them on the
 * Lua side if you call a lot). Text fields are {en = raw, ja = raw} tables with a twin *_utf8
 * ({en = ..., ja = ...}): "raw" is the game's Shift-JIS with its codes (what Ashita hands addons),
 * UTF-8 what Windower's res tables hold. Languages: 0 default, 1 Japanese, 2 English (Ashita's).
 *
 *   item(id)                 item_by_name(name [, lang])     item_ids()
 *   item_icon(id)            -> dib, width, height, bpp, name   (DIB: BITMAPINFOHEADER onward)
 *   item_icon_rgba(id)       -> rgba, width, height              (top-down RGBA8)
 *   spell(index)             spell_by_name(name [, lang])    spell_ids()
 *   ability(id)              ability_by_name(name [, lang])  ability_by_recast(id)  ability_ids()
 *   status(id)               status_by_index(index)          status_ids()
 *   status_icon(id)          status_icon_rgba(id)
 *   key_item(id)             key_item_ids()
 *   zone(id)                 zone_ids()
 *   job(id)                  job_ids()
 *   string(table, index [, lang [, utf8]])   -> string or nil
 *   string_find(table, str [, lang])         -> index or -1
 *   string_length(table, index [, lang])     -> byte length or -1
 *   string_count(table [, lang])             string_tables()
 *   to_utf8(raw [, flags])  flags: true or UTF8_KEEP_CODES (1) keeps codes; UTF8_PUA_ICONS (2)
 *                           element icons as U+E000.. (Windower's res text); UTF8_NO_ICONS (4) drops them
 *   auto_translate(kind, lang, b2, b3 [, utf8])
 *   file_path(id)            file_relpath(id)                set_default_lang(lang)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

#include "res.h"

int xi_res_open(lua_State* L);
void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n);

/* The host calls this once at startup: it only records the folders (res_init reads nothing; the
 * DATs load on first use). */
void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n)
{
    res_init(game_dir, overlays, (int)n);
}

/* ---- helpers --------------------------------------------------------------------------------- */

static void set_int(lua_State* L, const char* k, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}

static void set_str(lua_State* L, const char* k, const char* v)
{
    lua_pushstring(L, v ? v : "");
    lua_setfield(L, -2, k);
}

static void push_utf8(lua_State* L, const char* s, int flags)
{
    char buf[512];
    if (!s)
    {
        lua_pushstring(L, "");
        return;
    }
    size_t n = res_utf8(s, (size_t)-1, buf, sizeof buf, flags);
    if (n < sizeof buf)
    {
        lua_pushlstring(L, buf, n);
        return;
    }
    char* p = malloc(n + 1);
    if (!p)
    {
        lua_pushstring(L, "");
        return;
    }
    res_utf8(s, (size_t)-1, p, n + 1, flags);
    lua_pushlstring(L, p, n);
    free(p);
}

static void set_utf8(lua_State* L, const char* k, const char* v)
{
    push_utf8(L, v, 0);
    lua_setfield(L, -2, k);
}

/* t[k] = {en=, ja=} and t[k_utf8] = {en=, ja=} from [0] English, [1] Japanese */
static void set_text2(lua_State* L, const char* k, const char* const v[2])
{
    char k8[64];
    lua_createtable(L, 0, 2);
    set_str(L, "en", v[0]);
    set_str(L, "ja", v[1]);
    lua_setfield(L, -2, k);
    snprintf(k8, sizeof k8, "%s_utf8", k);
    lua_createtable(L, 0, 2);
    set_utf8(L, "en", v[0]);
    set_utf8(L, "ja", v[1]);
    lua_setfield(L, -2, k8);
}

static void set_text1(lua_State* L, const char* k, const char* v)
{
    char k8[64];
    set_str(L, k, v);
    snprintf(k8, sizeof k8, "%s_utf8", k);
    set_utf8(L, k8, v);
}

static int opt_lang(lua_State* L, int i)
{
    return (int)luaL_optinteger(L, i, RES_LANG_DEFAULT);
}

static uint32_t check_id(lua_State* L, int i)
{
    lua_Number n = luaL_checknumber(L, i);
    return n < 0 || n > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)n;
}

/* ---- items ----------------------------------------------------------------------------------- */

static void push_item(lua_State* L, const ResItem* it)
{
    if (!it)
    {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 64);
    set_int(L, "id", it->id);
    set_int(L, "kind", it->kind);
    set_str(L, "category", res_item_kind_name(it->kind));
    set_int(L, "legacy", it->legacy);
    set_int(L, "flags", it->flags);
    set_int(L, "stack", it->stack);
    set_int(L, "type", it->type);
    set_int(L, "resource_id", it->resource_id);
    set_int(L, "targets", it->targets);
    set_int(L, "level", it->level);
    set_int(L, "slots", it->slots);
    set_int(L, "races", it->races);
    set_int(L, "jobs", it->jobs);
    set_int(L, "superior_level", it->superior_level);
    set_int(L, "shield_size", it->shield_size);
    set_int(L, "max_charges", it->max_charges);
    set_int(L, "cast_time", it->cast_time);
    set_int(L, "cast_delay", it->cast_delay);
    set_int(L, "recast_delay", it->recast_delay);
    set_int(L, "base_item_id", it->base_item_id);
    set_int(L, "item_level", it->item_level);
    set_int(L, "damage", it->damage);
    set_int(L, "delay", it->delay);
    set_int(L, "dps", it->dps);
    set_int(L, "skill", it->skill);
    set_int(L, "jug_size", it->jug_size);
    set_int(L, "weapon_unknown", it->weapon_unknown);
    set_int(L, "range", it->range);
    set_int(L, "area_range", it->area_range);
    set_int(L, "area_shape", it->area_shape);
    set_int(L, "area_cursor", it->area_cursor);
    set_int(L, "element", it->element);
    set_int(L, "storage", it->storage);
    set_int(L, "instinct_cost", it->instinct_cost);
    set_int(L, "puppet_slot", it->puppet_slot);
    set_int(L, "puppet_elements", it->puppet_elements);
    set_int(L, "usable0", it->usable0);
    set_int(L, "usable1", it->usable1);
    set_int(L, "usable2", it->usable2);
    set_int(L, "article", it->article);
    if (it->kind == RES_ITEM_MONSTROSITY)
    {
        set_int(L, "monstrosity_id", it->monstrosity_id);
        set_str(L, "monstrosity_name", it->monstrosity_name);
        lua_pushlstring(L, (const char*)it->monstrosity_data, sizeof it->monstrosity_data);
        lua_setfield(L, -2, "monstrosity_data");
        lua_createtable(L, 16, 0);
        for (int i = 0; i < 16; ++i)
        {
            lua_createtable(L, 0, 3);
            set_int(L, "id", it->monstrosity_abilities[i].id);
            set_int(L, "level", it->monstrosity_abilities[i].level);
            set_int(L, "unknown", it->monstrosity_abilities[i].unknown);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "monstrosity_abilities");
    }
    set_text2(L, "name", it->name);
    set_text2(L, "description", it->description);
    set_text2(L, "log_singular", it->log_singular);
    set_text2(L, "log_plural", it->log_plural);
    lua_pushlstring(L, it->raw ? (const char*)it->raw : "", it->raw ? it->raw_size : 0);
    lua_setfield(L, -2, "raw");
}

static int l_item(lua_State* L)
{
    push_item(L, res_item(check_id(L, 1)));
    return 1;
}

static int l_item_by_name(lua_State* L)
{
    push_item(L, res_item_by_name(luaL_checkstring(L, 1), opt_lang(L, 2)));
    return 1;
}

static int l_item_ids(lua_State* L)
{
    uint32_t n = 0;
    const ResItem* const* all = res_items(&n);
    lua_createtable(L, (int)n, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        lua_pushnumber(L, all[i]->id);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int push_icon(lua_State* L, int ok, ResIcon* icon, int rgba)
{
    if (!ok)
    {
        lua_pushnil(L);
        return 1;
    }
    if (rgba)
    {
        uint8_t* p = res_icon_rgba(icon);
        if (!p)
        {
            res_icon_free(icon);
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, (const char*)p, (size_t)icon->width * (size_t)(icon->height < 0 ? -icon->height : icon->height) * 4);
        free(p);
        lua_pushnumber(L, icon->width);
        lua_pushnumber(L, icon->height < 0 ? -icon->height : icon->height);
        res_icon_free(icon);
        return 3;
    }
    lua_pushlstring(L, (const char*)icon->dib, icon->dib_size);
    lua_pushnumber(L, icon->width);
    lua_pushnumber(L, icon->height);
    lua_pushnumber(L, icon->bpp);
    lua_pushstring(L, icon->name);
    res_icon_free(icon);
    return 5;
}

static int l_item_icon(lua_State* L)
{
    ResIcon icon;
    return push_icon(L, res_item_icon(check_id(L, 1), &icon), &icon, 0);
}

static int l_item_icon_rgba(lua_State* L)
{
    ResIcon icon;
    return push_icon(L, res_item_icon(check_id(L, 1), &icon), &icon, 1);
}

/* ---- spells, abilities ----------------------------------------------------------------------- */

static void push_spell(lua_State* L, const ResSpell* s)
{
    if (!s)
    {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 32);
    set_int(L, "index", s->index);
    set_int(L, "type", s->type);
    set_str(L, "type_name", res_spell_type_name(s->type));
    set_int(L, "element", s->element);
    set_int(L, "targets", s->targets);
    set_int(L, "skill", s->skill);
    set_int(L, "mp_cost", s->mp_cost);
    set_int(L, "cast_time", s->cast_time);
    set_int(L, "recast_delay", s->recast_delay);
    lua_createtable(L, 24, 0);
    for (int j = 0; j < 24; ++j)
    {
        lua_pushnumber(L, s->levels[j]);
        lua_rawseti(L, -2, j); /* by job id; [0] is the unused "none" job */
    }
    lua_setfield(L, -2, "levels");
    set_int(L, "id", s->id);
    set_int(L, "icon_nq", s->icon_nq);
    set_int(L, "icon_hq", s->icon_hq);
    set_int(L, "requirements", s->requirements);
    set_int(L, "range", s->range);
    set_int(L, "area_range", s->area_range);
    set_int(L, "area_shape", s->area_shape);
    set_int(L, "cursor_target", s->cursor_target);
    set_int(L, "area_flags", s->area_flags);
    set_int(L, "job_point_mask", s->job_point_mask);
    set_text2(L, "name", s->name);
    set_text2(L, "description", s->description);
    lua_pushlstring(L, (const char*)s->raw, sizeof s->raw);
    lua_setfield(L, -2, "raw");
}

static void push_ability(lua_State* L, const ResAbility* a)
{
    if (!a)
    {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 24);
    set_int(L, "id", a->id);
    set_int(L, "type", a->type);
    set_str(L, "type_name", res_ability_type_name(a->type));
    set_int(L, "element", a->element);
    set_int(L, "icon_id", a->icon_id);
    set_int(L, "mp_cost", a->mp_cost);
    set_int(L, "recast_id", a->recast_id);
    set_int(L, "targets", a->targets);
    set_int(L, "tp_cost", a->tp_cost);
    set_int(L, "menu_category", a->menu_category);
    set_int(L, "monster_level", a->monster_level);
    set_int(L, "range", a->range);
    set_int(L, "area_range", a->area_range);
    set_int(L, "area_shape", a->area_shape);
    set_int(L, "cursor_target", a->cursor_target);
    set_text2(L, "name", a->name);
    set_text2(L, "description", a->description);
    lua_pushlstring(L, (const char*)a->raw, sizeof a->raw);
    lua_setfield(L, -2, "raw");
}

static int l_spell(lua_State* L)
{
    push_spell(L, res_spell(check_id(L, 1)));
    return 1;
}

static int l_spell_by_name(lua_State* L)
{
    push_spell(L, res_spell_by_name(luaL_checkstring(L, 1), opt_lang(L, 2)));
    return 1;
}

static int l_spell_ids(lua_State* L)
{
    uint32_t n = res_spell_count();
    int k = 0;
    lua_newtable(L);
    for (uint32_t i = 0; i < n; ++i)
        if (res_spell(i))
        {
            lua_pushnumber(L, i);
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

static int l_ability(lua_State* L)
{
    push_ability(L, res_ability(check_id(L, 1)));
    return 1;
}

static int l_ability_by_name(lua_State* L)
{
    push_ability(L, res_ability_by_name(luaL_checkstring(L, 1), opt_lang(L, 2)));
    return 1;
}

static int l_ability_by_recast(lua_State* L)
{
    push_ability(L, res_ability_by_recast(check_id(L, 1)));
    return 1;
}

static int l_ability_ids(lua_State* L)
{
    uint32_t n = res_ability_count();
    int k = 0;
    lua_newtable(L);
    for (uint32_t i = 0; i < n; ++i)
        if (res_ability(i))
        {
            lua_pushnumber(L, i);
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

/* ---- statuses -------------------------------------------------------------------------------- */

static void push_status(lua_State* L, const ResStatus* s)
{
    if (!s)
    {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 12);
    set_int(L, "id", s->id);
    set_int(L, "index", s->index); /* 65535: no icon record */
    set_int(L, "can_cancel", s->can_cancel);
    set_int(L, "hide_timer", s->hide_timer);
    set_text2(L, "name", s->name);
    set_text2(L, "description", s->description);
    set_text1(L, "log_name", s->log_name);
}

static int l_status(lua_State* L)
{
    push_status(L, res_status(check_id(L, 1)));
    return 1;
}

static int l_status_by_index(lua_State* L)
{
    push_status(L, res_status_by_index(check_id(L, 1)));
    return 1;
}

static int l_status_ids(lua_State* L)
{
    int k = 0;
    lua_newtable(L);
    for (uint32_t i = 0; i < 4096; ++i)
        if (res_status(i))
        {
            lua_pushnumber(L, i);
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

static int l_status_icon(lua_State* L)
{
    ResIcon icon;
    return push_icon(L, res_status_icon(check_id(L, 1), &icon), &icon, 0);
}

static int l_status_icon_rgba(lua_State* L)
{
    ResIcon icon;
    return push_icon(L, res_status_icon(check_id(L, 1), &icon), &icon, 1);
}

/* ---- key items, zones, jobs ------------------------------------------------------------------ */

static int l_key_item(lua_State* L)
{
    const ResKeyItem* k = res_key_item(check_id(L, 1));
    if (!k)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 8);
    set_int(L, "id", k->id);
    set_int(L, "category", k->category);
    set_text2(L, "name", k->name);
    set_text1(L, "plural", k->plural);
    set_text2(L, "description", k->description);
    return 1;
}

static int ids_until_nil(lua_State* L, int (*exists)(uint32_t), uint32_t limit)
{
    int k = 0;
    lua_newtable(L);
    for (uint32_t i = 0; i < limit; ++i)
        if (exists(i))
        {
            lua_pushnumber(L, i);
            lua_rawseti(L, -2, ++k);
        }
    return 1;
}

static int ki_exists(uint32_t i) { return res_key_item(i) != NULL; }
static int zone_exists(uint32_t i) { return res_zone(i) != NULL; }
static int job_exists(uint32_t i) { return res_job(i) != NULL; }

static int l_key_item_ids(lua_State* L)
{
    return ids_until_nil(L, ki_exists, res_string_count("keyitems.names", RES_LANG_EN) + 1024);
}

static int l_zone(lua_State* L)
{
    const ResZone* z = res_zone(check_id(L, 1));
    if (!z)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 8);
    set_int(L, "id", z->id);
    set_text2(L, "name", z->name);
    set_text1(L, "search", z->search);
    set_text1(L, "abbr", z->abbr);
    return 1;
}

static int l_zone_ids(lua_State* L)
{
    return ids_until_nil(L, zone_exists, res_string_count("zones.names", RES_LANG_EN));
}

static int l_job(lua_State* L)
{
    const ResJob* j = res_job(check_id(L, 1));
    if (!j)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    set_int(L, "id", j->id);
    set_text2(L, "name", j->name);
    set_text2(L, "abbr", j->abbr);
    return 1;
}

static int l_job_ids(lua_State* L)
{
    return ids_until_nil(L, job_exists, res_string_count("jobs.names", RES_LANG_EN));
}

/* ---- strings --------------------------------------------------------------------------------- */

static int opt_flags(lua_State* L, int i)
{
    if (lua_type(L, i) == LUA_TNUMBER)
        return (int)lua_tointeger(L, i);
    return lua_toboolean(L, i) ? RES_UTF8_KEEP_CODES : 0;
}

static void push_utf8_n(lua_State* L, const char* s, size_t n, int flags)
{
    size_t need = res_utf8(s, n, NULL, 0, flags);
    char* p = malloc(need + 1);
    if (!p)
    {
        lua_pushstring(L, "");
        return;
    }
    res_utf8(s, n, p, need + 1, flags);
    lua_pushlstring(L, p, need);
    free(p);
}

/* string(table, index [, lang [, utf8]]): utf8 true or a RES_UTF8_* flags number (the conversion
 * then keeps codes only when flags has 1) */
static int l_string(lua_State* L)
{
    size_t n = 0;
    const char* s = res_string_n(luaL_checkstring(L, 1), check_id(L, 2), opt_lang(L, 3), &n);
    if (!s)
        lua_pushnil(L);
    else if (lua_type(L, 4) == LUA_TNUMBER)
        push_utf8_n(L, s, n, (int)lua_tointeger(L, 4));
    else if (lua_toboolean(L, 4))
        push_utf8_n(L, s, n, 0);
    else
        lua_pushlstring(L, s, n);
    return 1;
}

static int l_string_find(lua_State* L)
{
    lua_pushnumber(L, res_string_find(luaL_checkstring(L, 1), luaL_checkstring(L, 2), opt_lang(L, 3)));
    return 1;
}

static int l_string_length(lua_State* L)
{
    size_t n = 0;
    const char* s = res_string_n(luaL_checkstring(L, 1), check_id(L, 2), opt_lang(L, 3), &n);
    lua_pushnumber(L, s ? (lua_Number)n : -1);
    return 1;
}

static int l_string_count(lua_State* L)
{
    lua_pushnumber(L, res_string_count(luaL_checkstring(L, 1), opt_lang(L, 2)));
    return 1;
}

static int l_string_tables(lua_State* L)
{
    const char* const* t = res_string_tables();
    lua_newtable(L);
    for (int i = 0; t[i]; ++i)
    {
        lua_pushstring(L, t[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_to_utf8(lua_State* L)
{
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    push_utf8_n(L, s, n, opt_flags(L, 2));
    return 1;
}

static int l_auto_translate(lua_State* L)
{
    const char* s = res_auto_translate((uint8_t)luaL_checkinteger(L, 1), (uint8_t)luaL_checkinteger(L, 2),
                                       (uint8_t)luaL_checkinteger(L, 3), (uint8_t)luaL_checkinteger(L, 4));
    if (!s)
        lua_pushnil(L);
    else if (lua_toboolean(L, 5))
        push_utf8(L, s, 0);
    else
        lua_pushstring(L, s);
    return 1;
}

static int l_file_path(lua_State* L)
{
    char p[1024];
    if (res_file_path(check_id(L, 1), p, sizeof p))
        lua_pushstring(L, p);
    else
        lua_pushnil(L);
    return 1;
}

static int l_file_relpath(lua_State* L)
{
    char p[256];
    if (res_file_relpath(check_id(L, 1), p, sizeof p))
        lua_pushstring(L, p);
    else
        lua_pushnil(L);
    return 1;
}

static int l_set_default_lang(lua_State* L)
{
    res_set_default_lang((int)luaL_checkinteger(L, 1));
    return 0;
}

int xi_res_open(lua_State* L)
{
    static const luaL_Reg fns[] = {
        {"item", l_item},
        {"item_by_name", l_item_by_name},
        {"item_ids", l_item_ids},
        {"item_icon", l_item_icon},
        {"item_icon_rgba", l_item_icon_rgba},
        {"spell", l_spell},
        {"spell_by_name", l_spell_by_name},
        {"spell_ids", l_spell_ids},
        {"ability", l_ability},
        {"ability_by_name", l_ability_by_name},
        {"ability_by_recast", l_ability_by_recast},
        {"ability_ids", l_ability_ids},
        {"status", l_status},
        {"status_by_index", l_status_by_index},
        {"status_ids", l_status_ids},
        {"status_icon", l_status_icon},
        {"status_icon_rgba", l_status_icon_rgba},
        {"key_item", l_key_item},
        {"key_item_ids", l_key_item_ids},
        {"zone", l_zone},
        {"zone_ids", l_zone_ids},
        {"job", l_job},
        {"job_ids", l_job_ids},
        {"string", l_string},
        {"string_find", l_string_find},
        {"string_length", l_string_length},
        {"string_count", l_string_count},
        {"string_tables", l_string_tables},
        {"to_utf8", l_to_utf8},
        {"auto_translate", l_auto_translate},
        {"file_path", l_file_path},
        {"file_relpath", l_file_relpath},
        {"set_default_lang", l_set_default_lang},
        {NULL, NULL},
    };
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_pushnumber(L, RES_LANG_DEFAULT);
    lua_setfield(L, -2, "LANG_DEFAULT");
    lua_pushnumber(L, RES_LANG_JA);
    lua_setfield(L, -2, "LANG_JA");
    lua_pushnumber(L, RES_LANG_EN);
    lua_setfield(L, -2, "LANG_EN");
    lua_pushnumber(L, RES_UTF8_KEEP_CODES);
    lua_setfield(L, -2, "UTF8_KEEP_CODES");
    lua_pushnumber(L, RES_UTF8_PUA_ICONS);
    lua_setfield(L, -2, "UTF8_PUA_ICONS");
    lua_pushnumber(L, RES_UTF8_NO_ICONS);
    lua_setfield(L, -2, "UTF8_NO_ICONS");
    return 1;
}
