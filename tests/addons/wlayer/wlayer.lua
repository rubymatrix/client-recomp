-- The Windower layer's test addon (tests/windower_layer_test.sh): a Windower 4 addon that uses
-- the raw windower.* API (no Windower libraries) and prints what it sees.
_addon.name = 'wlayer'
_addon.version = '1.0'
_addon.commands = { 'wlayer', 'wl' }

local function say(...)
    local t = {}
    for i = 1, select('#', ...) do t[i] = tostring((select(i, ...))) end
    windower.add_to_chat(1, table.concat(t, ' '))
end

local function hex(s)
    return (s:gsub('.', function(c) return ('%02x'):format(c:byte()) end))
end

-------------------------------------------------------------------------------- pack

local function pack_tests()
    local p = ('b9b7H'):pack(0x0DD, 5, 0x1234)
    say('pack b9b7H', hex(p), p:unpack('b9b7H'))
    say('pack ints', hex(('CcHhIi'):pack(200, -2, 60000, -300, 4000000000, -5)), ('CcHhIi'):pack(200, -2, 60000, -300, 4000000000, -5):unpack('CcHhIi'))
    say('pack big', hex(('>I'):pack(1)), hex(('<H>H'):pack(1, 1)))
    say('pack float', ('f'):pack(1.5):unpack('f'), ('d'):pack(-2.25):unpack('d'))
    say('pack strings', hex(('S4zA3'):pack('ab', 'hi', 'xyzw')), ('S4zA3'):pack('ab', 'hi', 'xyzw'):unpack('S4zA3'))
    say('pack bools', hex(('q3b5B'):pack(true, false, true, 3, true)), ('q3b5B'):pack(true, false, true, 3, true):unpack('q3b5B'))
    say('unpack at', ('\1\2\3\4\5\6'):unpack('H', 3), ('\1\2\3\4\5\6'):unpack('b4', 1, 4))
    say('unpack short', select('#', ('\1\2'):unpack('I')), select('#', ('\1\2\3\4\5'):unpack('II')))
    say('pack align', hex(('b3C'):pack(7, 0xAA)), hex(('b1b1b1b1b1b1b1b1b1'):pack(1, 0, 1, 0, 1, 0, 1, 0, 1)))
    say('pack require', require('pack') == string, string.pack ~= nil)
end

-------------------------------------------------------------------------------- misc

local function misc_tests()
    say('wc_match', windower.wc_match('Fire IV', 'fire*'), windower.wc_match('Fire', 'f?re'), windower.wc_match('Blizzard', 'fire*|bliz*'),
        windower.wc_match('Water', 'fire*|bliz*'))
    local m = windower.regex.match('abc 123 def 45', '(\\d+)')
    say('regex match', #m, m[1][0], m[1][1], m[2][1])
    say('regex replace', windower.regex.replace('a.b.c', '\\.', '_'), windower.regex.replace('x=1, y=2', '(\\w)=(\\d)', '$2$1'))
    say('regex split', table.concat(windower.regex.split('a, b,c', ',\\s*'), '|'), windower.regex.match('abc', 'z') == nil)
    say('regex (?:)', windower.regex.match('see http://x.org now', '[a-z]+://(?:[a-z]+\\.)+[a-z]{2,5}\\b')[1][0])
    say('shift_jis', windower.from_shift_jis(windower.to_shift_jis('abc')) == 'abc')
    local s = windower.get_windower_settings()
    say('settings', s.ui_x_res > 0, s.ui_y_res > 0, s.x_res == s.ui_x_res, type(s.ffxi_version))
    say('paths', windower.addon_path:sub(-8), windower.windower_path:sub(-9), windower.file_exists(windower.addon_path .. 'wlayer.lua'),
        windower.dir_exists(windower.addon_path), windower.file_exists(windower.addon_path .. 'nope.lua'))
    say('create_dir', windower.create_dir(windower.addon_path .. 'data'), windower.dir_exists(windower.addon_path .. 'data'),
        (windower.create_dir(windower.addon_path .. 'x/y')))
    local dir = windower.get_dir(windower.addon_path)
    table.sort(dir)
    say('get_dir', table.concat(dir, ','))
    say('has_focus', windower.has_focus())
    local ok, err = pcall(windower.execute, 'notepad.exe', {})
    say('execute', ok, (tostring(err):gsub('^.-: ', '')))
    say('unknown member', windower.not_a_thing)
    say('convert_auto_trans', windower.convert_auto_trans('plain'))
end

-------------------------------------------------------------------------------- ui

local function ui_tests()
    windower.text.create('wl_t')
    windower.text.set_text('wl_t', 'Hello')
    windower.text.set_location('wl_t', 100, 50)
    windower.text.set_font('wl_t', 'Consolas', 'Arial')
    windower.text.set_font_size('wl_t', 9)
    windower.text.set_color('wl_t', 255, 10, 20, 30)
    windower.text.set_bg_visibility('wl_t', true)
    windower.text.set_right_justified('wl_t', true)
    local x, y = windower.text.get_location('wl_t')
    local w, h = windower.text.get_extents('wl_t')
    say('text', x, y, w > 0, h > 0)
    windower.text.delete('wl_t')
    windower.text.set_text('wl_t', 'deleted: ignored')
    windower.prim.create('wl_p')
    windower.prim.set_position('wl_p', 10, 20)
    windower.prim.set_size('wl_p', 30, 40)
    windower.prim.set_color('wl_p', 128, 255, 0, 0)
    windower.prim.set_visibility('wl_p', false)
    windower.prim.set_texture('wl_p', windower.addon_path .. 'missing.png')
    windower.prim.delete('wl_p')
    say('prim', 'ok')
end

-------------------------------------------------------------------------------- state

local function state_tests()
    local info = windower.ffxi.get_info()
    say('info', info.logged_in, info.zone, info.time >= 0 and info.time < 1440, info.day >= 0 and info.day < 8, info.moon_phase >= 0 and info.moon_phase < 12)
    local p = windower.ffxi.get_player()
    if p then
        say('player', p.name, p.id, p.main_job, p.main_job_level, p.sub_job, p.sub_job_level, p.vitals.max_hp, p.vitals.hpp)
    else
        say('player', 'nil')
    end
    local items = windower.ffxi.get_items()
    say('items', type(items.inventory), items.max_inventory ~= nil, type(items.equipment), items.equipment.main_bag ~= nil)
    say('bag_info', type(windower.ffxi.get_bag_info(0).max))
    say('recasts', windower.ffxi.get_ability_recasts()[0], windower.ffxi.get_spell_recasts()[1])
    say('mob me', windower.ffxi.get_mob_by_target('me') == nil)
    local party = windower.ffxi.get_party()
    say('party', party.p0 and party.p0.name, party.party1_count)
end

-------------------------------------------------------------------------------- events

local seen = {}

windower.register_event('load', function()
    say('load event')
    pack_tests()
    misc_tests()
    ui_tests()
end)

windower.register_event('login', function(name) say('login event', name) end)

windower.register_event('addon command', function(cmd, ...)
    cmd = cmd and cmd:lower() or 'none'
    local args = { ... }
    if cmd == 'args' then
        say('args', #args, table.concat(args, '|'))
    elseif cmd == 'state' then
        state_tests()
    elseif cmd == 'sleep' then
        say('sleep: before')
        coroutine.sleep(0.1)
        say('sleep: after')
    elseif cmd == 'schedule' then
        coroutine.schedule(function() say('scheduled ran') end, 0.05)
        local co = coroutine.schedule(function() say('cancelled ran (wrong)') end, 0.05)
        coroutine.close(co)
    elseif cmd == 'send' then
        windower.send_command('wl args a b; wait 0.1; @wl args after; input /echo from input; load timers; setkey enter down')
    elseif cmd == 'unreg' then
        windower.unregister_event(seen.prerender)
        say('unregistered prerender')
    else
        say('command', cmd, #args)
    end
end)

local prerenders = 0
seen.prerender = windower.register_event('prerender', function()
    prerenders = prerenders + 1
    if prerenders == 1 then say('prerender event') end
end)

windower.register_event('incoming text', function(original, modified, mode, modified_mode, blocked)
    if original:find('secret') then return 'redacted', 5 end
    if original:find('hide') then return true end
end)

windower.register_event('outgoing text', function(original, modified, blocked)
    if original:find('/echo swap') then return '/echo swapped' end
end)

windower.register_event('keyboard', function(dik, down, flags, blocked)
    say('keyboard', ('%02x'):format(dik), down, blocked)
    return dik == 0x1F
end)

windower.register_event('mouse', function(type, x, y, delta, blocked)
    say('mouse', type, x, y, delta, blocked)
    return type == 1
end)

windower.register_event('incoming chunk', function(id, original, modified, injected, blocked)
    if id == 0x0AB then return true end
    if id == 0x0AC then return original:sub(1, 4) .. 'WXYZ' end
end)

windower.register_event('outgoing chunk', function(id, original, modified, injected, blocked)
    if id == 0x0AD then say('outgoing chunk', ('%03x'):format(id), #original, injected) end
end)

windower.register_event('unhandled command', function(cmd, ...)
    if cmd == 'wlunknown' then
        say('unhandled command', cmd, ...)
        return true
    end
end)

windower.register_event('unload', function() say('unload event', prerenders > 0) end)
