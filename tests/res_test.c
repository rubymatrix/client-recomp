/* Cross-check of host/addons/res.c (the DAT resource reader) and its Lua binding against Windower's
 * generated resources (addon-deps/Resources/resources_data/<name>.lua).
 *
 * Build (from the repo root):
 *   cc -std=c11 -O2 -Ihost/addons -Ithird_party/luajit/src tests/res_test.c host/addons/res.c \
 *      host/addons/res_lua.c build/third_party/luajit.a -lm -o build/res_test
 * Run:
 *   build/res_test ["<FINAL FANTASY XI folder>" [<resources_data folder>]]
 * Defaults: ../FINAL FANTASY XI and ../addon-deps/Resources/resources_data (next to the repo).
 * Exits 1 if a name check falls under 99%. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "res.h"

int xi_res_open(lua_State* L);

static const char* const g_script =
    "local W = ...\n"
    "local res = res\n"
    "local function load(n) return dofile(W .. '/' .. n .. '.lua') end\n"
    "local failed = false\n"
    "local function report(what, ok, total, bad, required)\n"
    "  local pct = total > 0 and ok * 100 / total or 0\n"
    "  print(string.format('%-34s %6d / %-6d %7.3f%%', what, ok, total, pct))\n"
    "  for i = 1, math.min(#bad, 3) do print('      ' .. bad[i]) end\n"
    "  if required and pct < 99 then failed = true end\n"
    "end\n"
    "-- entries Windower's generator renames by hand (numbered dazes / finishing moves, job 23)\n"
    "local skip = nil\n"
    "local function check(what, wt, get, wfield, ofield, conv, required)\n"
    "  local ok, total, bad = 0, 0, {}\n"
    "  for id, w in pairs(wt) do\n"
    "    local want = w[wfield]\n"
    "    if want ~= nil and not (skip and skip[id]) then\n"
    "      total = total + 1\n"
    "      local r = get(id)\n"
    "      local have = r and ofield(r)\n"
    "      if conv and have ~= nil then have = conv(have) end\n"
    "      if have == want then ok = ok + 1\n"
    "      elseif #bad < 3 then bad[#bad + 1] = string.format('%s: want %q have %q', tostring(id), tostring(want), tostring(have)) end\n"
    "    end\n"
    "  end\n"
    "  report(what, ok, total, bad, required)\n"
    "end\n"
    "local function f(k, sub) return function(r) if sub then return r[k] and r[k][sub] end return r[k] end end\n"
    "local q = function(v) return v / 4 end\n"
    "local t0 = os.clock()\n"
    "local items = load('items')\n"
    "local cache = {}\n"
    "local function item(id) local r = cache[id]; if r == nil then r = res.item(id) or false; cache[id] = r end; return r or nil end\n"
    "item(1)\n"
    "print(string.format('items loaded in %.2fs (%d ids)', os.clock() - t0, #res.item_ids()))\n"
    "check('items en', items, item, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('items ja', items, item, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "check('items enl', items, item, 'enl', f('log_singular_utf8', 'en'), nil, false)\n"
    "check('items category', items, item, 'category', f('category'))\n"
    "for _, k in ipairs({'flags', 'stack', 'type', 'targets', 'level', 'slots', 'races', 'jobs', 'damage', 'delay', 'skill',\n"
    "                    'shield_size', 'item_level', 'superior_level', 'max_charges', 'cast_delay', 'recast_delay'}) do\n"
    "  check('items ' .. k, items, item, k, f(k))\n"
    "end\n"
    "check('items cast_time', items, item, 'cast_time', f('cast_time'), q)\n"
    "local descs = load('item_descriptions')\n"
    "local pua = function(s) return res.to_utf8(s, res.UTF8_PUA_ICONS) end\n"
    "check('item descriptions en', descs, item, 'en', f('description', 'en'), pua)\n"
    "check('item descriptions ja', descs, item, 'ja', f('description', 'ja'), pua)\n"
    "local spells = load('spells')\n"
    "check('spells en', spells, res.spell, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('spells ja', spells, res.spell, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "for _, p in ipairs({{'mp_cost', 'mp_cost'}, {'element', 'element'}, {'targets', 'targets'}, {'skill', 'skill'},\n"
    "                    {'icon_id_nq', 'icon_nq'}, {'recast_id', 'index'}, {'requirements', 'requirements'},\n"
    "                    {'type', 'type_name'}}) do\n"
    "  check('spells ' .. p[1], spells, res.spell, p[1], f(p[2]))\n"
    "end\n"
    "check('spells icon_id', spells, res.spell, 'icon_id', f('icon_hq'), function(v) return v == 65535 and -1 or v end)\n"
    "check('spells cast_time', spells, res.spell, 'cast_time', f('cast_time'), q)\n"
    "-- recast bytes 0xC0 / 0xD0 / 0xE0 are 4 / 3 / 2 minutes in Windower's data, anything else quarter seconds\n"
    "check('spells recast', spells, res.spell, 'recast', f('recast_delay'), function(v) return ({[0xC0] = 240, [0xD0] = 180, [0xE0] = 120})[v] or v / 4 end)\n"
    "check('spells range', spells, res.spell, 'range', f('range'), function(v) return v == 15 and 0 or v end)\n"
    "do local ok, total = 0, 0\n"
    "  for id, w in pairs(spells) do total = total + 1; local s = res.spell(id); local good = s ~= nil\n"
    "    if s then for j = 1, 22 do local want = w.levels[j] or -1; local have = s.levels[j]; if want ~= have then good = false end end end\n"
    "    if good then ok = ok + 1 end end\n"
    "  report('spells levels', ok, total, {}) end\n"
    "local ja = load('job_abilities')\n"
    "local function jab(id) return res.ability(id + 0x200) end\n"
    "check('job abilities en', ja, jab, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('job abilities ja', ja, jab, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "for _, p in ipairs({{'icon_id', 'icon_id'}, {'mp_cost', 'mp_cost'}, {'recast_id', 'recast_id'}, {'targets', 'targets'}, {'type', 'type_name'}}) do\n"
    "  check('job abilities ' .. p[1], ja, jab, p[1], f(p[2]))\n"
    "end\n"
    "check('job abilities tp_cost', ja, jab, 'tp_cost', f('tp_cost'), function(v) return v < 0 and 0 or v end)\n"
    "check('job abilities element', ja, jab, 'element', f('element'), function(v) return v % 8 end)\n"
    "local ws = load('weapon_skills')\n"
    "check('weapon skills en', ws, res.ability, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('weapon skills ja', ws, res.ability, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "check('weapon skills icon_id', ws, res.ability, 'icon_id', f('icon_id'))\n"
    "check('weapon skills targets', ws, res.ability, 'targets', f('targets'))\n"
    "check('weapon skills range', ws, res.ability, 'range', f('range'))\n"
    "local buffs = load('buffs')\n"
    "skip = {}; for i = 381, 400 do skip[i] = true end; for i = 448, 452 do skip[i] = true end\n"
    "check('buffs en', buffs, res.status, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('buffs ja', buffs, res.status, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "check('buffs enl', buffs, res.status, 'enl', f('log_name_utf8'))\n"
    "local ki = load('key_items')\n"
    "skip = nil\n"
    "check('key items en', ki, res.key_item, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('key items ja', ki, res.key_item, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "local zones = load('zones')\n"
    "check('zones en', zones, res.zone, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('zones ja', zones, res.zone, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "check('zones search', zones, res.zone, 'search', f('search_utf8'))\n"
    "local jobs = load('jobs')\n"
    "skip = {[23] = true}\n"
    "check('jobs en', jobs, res.job, 'en', f('name_utf8', 'en'), nil, true)\n"
    "check('jobs ja', jobs, res.job, 'ja', f('name_utf8', 'ja'), nil, true)\n"
    "check('jobs ens', jobs, res.job, 'ens', f('abbr_utf8', 'en'))\n"
    "skip = nil\n"
    "local at = load('auto_translates')\n"
    "check('auto-translate en', at, function(id) return res.auto_translate(2, 2, math.floor(id / 256), id % 256, true) end, 'en', function(s) return s end)\n"
    "check('auto-translate ja', at, function(id) return res.auto_translate(2, 1, math.floor(id / 256), id % 256, true) end, 'ja', function(s) return s end)\n"
    "-- spot checks of the Ashita-style and icon paths\n"
    "local function expect(what, cond) print(string.format('%-34s %s', what, cond and 'ok' or 'FAILED')); if not cond then failed = true end end\n"
    "expect('string zones.names 230', res.string('zones.names', 230, 2) == \"Southern San d'Oria\")\n"
    "expect('string_find zones.names', res.string_find('zones.names', \"southern san d'oria\", 2) == 230)\n"
    "expect('string keyitems.names 1', res.string('keyitems.names', 1, 2) == 'Zeruhn report')\n"
    "expect('string action.messages 1', res.string('action.messages', 1, 2, true) == '[The /] [hits/hit] [the /] for  point[/s] of damage.\\n')\n"
    "expect('item_by_name Fire Crystal', (res.item_by_name('fire crystal', 2) or {}).id == 4096)\n"
    "expect('spell_by_name Cure', (res.spell_by_name('Cure', 2) or {}).index == 1)\n"
    "expect('ability_by_name Provoke', (res.ability_by_name('Provoke', 2) or {}).id == 0x223)\n"
    "local p, w, h = res.item_icon_rgba(4096)\n"
    "expect('item icon 4096 32x32 rgba', p ~= nil and w == 32 and h == 32 and #p == 32 * 32 * 4)\n"
    "local dib, w2, h2, bpp = res.item_icon(4096)\n"
    "expect('item icon 4096 dib 8bpp', dib ~= nil and bpp == 8 and #dib == 40 + 1024 + 1024)\n"
    "local p2, w3, h3 = res.status_icon_rgba(1)\n"
    "expect('status icon 1 32x32 rgba', p2 ~= nil and w3 == 32 and h3 == 32)\n"
    "expect('to_utf8 element icon', res.to_utf8('\\239\\031+10') == 'Fire+10')\n"
    "expect('to_utf8 FFXI latin', res.to_utf8('Fu\\133Rs') == 'Fu\\226\\128\\153s')\n"
    "expect('to_utf8 auto-translate', res.to_utf8('\\253\\002\\002\\001\\001\\253') == '{Nice to meet you.}')\n"
    "return failed\n";

int main(int argc, char** argv)
{
    const char* game = argc > 1 ? argv[1] : "../FINAL FANTASY XI";
    const char* wres = argc > 2 ? argv[2] : "../addon-deps/Resources/resources_data";
    res_init(game, NULL, 0);

    char path[1024];
    if (!res_file_path(73, path, sizeof path))
    {
        fprintf(stderr, "no VTABLE/FTABLE under %s\n", game);
        return 2;
    }
    printf("file 73 -> %s\n", path);

    clock_t t = clock();
    uint32_t n = 0;
    res_items(&n);
    printf("C: %u items in %.2fs\n", n, (double)(clock() - t) / CLOCKS_PER_SEC);

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    xi_res_open(L);
    lua_setglobal(L, "res");
    if (luaL_loadbuffer(L, g_script, strlen(g_script), "=res_test"))
    {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 2;
    }
    lua_pushstring(L, wres);
    if (lua_pcall(L, 1, 1, 0))
    {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 2;
    }
    int failed = lua_toboolean(L, -1);
    lua_close(L);
    res_shutdown();
    printf(failed ? "FAILED\n" : "PASSED\n");
    return failed;
}
