--[==[
Windower 4's packet-derived events and game state, for the Windower layer (lua/windower.lua).

    local wev = require('xi.windower_events')
    wev.install{ raise = fn(name, ...), has = fn(name) }

One copy lives in every Windower addon's state; each parses the packets it sees. install subscribes
to xi's packet_in / packet_out / frame events and raises Windower's events through raise() with
Windower's positional arguments:

    incoming chunk / outgoing chunk (id, original, modified, injected, blocked)
        raise returns true to block, or a string that replaces the packet
    action (act)                                           0x028, Windower's action table
    action message (actor_id, target_id, actor_index, target_index, message_id, param_1, param_2, param_3)  0x029
    login (name) / logout (name)                           0x00A (first zone-in) / 0x00B type 1
    zone change (new_id, old_id)                           0x00A
    job change (main_job_id, main_job_level, sub_job_id, sub_job_level)   0x00A 0x01B 0x061 0x0DF
    status change (new, old)                               0x037
    gain buff / lose buff (buff_id)                        0x037
    hp/mp/tp/hpp/mpp change (new, old)                     0x0DF 0x0DD 0x0E2 (the player's id)
    level up / level down (level)                          0x02D messages 9 / 11
    gain experience (amount, chain_number, limit_mode)     0x02D messages 8 105 253 371 372
    party invite (sender, sender_id)                       0x0DC
    add item / remove item (bag, index, id, count)         0x01E 0x01F 0x020, after 0x01D says bags are loaded
    linkshell change (new_name, old_name)                  the equipped linkshell's item (status 0x13)
    emote (emote_id, sender_id, target_id, motion_only)    0x05A
    examined (sender_name, sender_index)                   0x009 message 89
    chat message (message, sender, mode, gm)               0x017 (message raw Shift-JIS, as the game has it)
    weather change (new, old)                              0x00A 0x057
    time change / day change / moon change / moon pct change (new, old)   the Vana'diel clock, each frame
    target change (index)                                  the game's target, polled each frame

State getters (Windower's shapes, for windower.ffxi.*): get_items, get_key_items, get_spells,
get_abilities, get_ability_recasts, get_spell_recasts, get_mjob_data, get_sjob_data,
get_player_extra, get_info (also get_info_extra); parse_action(packet) for
windower.packets.parse_action. Where the game's memory holds the data (items, key items, spells,
recasts: xi.game) it is read from memory; otherwise from the packets this state has seen, primed at
install from the last packet of each id (xi.packets.last) so an addon loaded mid-session has them.

wev.provides[name] is true for every event raised here.
--]==]

local bit = require('bit')
local band, rshift = bit.band, bit.rshift
local byte, sub, char = string.byte, string.sub, string.char
local floor, abs, min = math.floor, math.abs, math.min
local select, type, pairs, ipairs = select, type, pairs, ipairs

local xi = rawget(_G, 'xi')
local game = xi and xi.game
local native_packets = xi and xi.packets

local wev = {}

-- wev.memory = false: read only the packet-fed state (tests), never the game's memory
wev.memory = true
local function mgame()
    if wev.memory == false then return nil end
    return game
end

-------------------------------------------------------------------------------- reading

-- o: the packet offset (0-based, header included), as fields.lua documents them
local function u8(s, o) return byte(s, o + 1) or 0 end
local function u16(s, o)
    local a, b = byte(s, o + 1, o + 2)
    return (a or 0) + (b or 0) * 256
end
local function u32(s, o)
    local a, b, c, d = byte(s, o + 1, o + 4)
    return (a or 0) + (b or 0) * 256 + (c or 0) * 65536 + (d or 0) * 16777216
end
local function cstr(s, o, n)
    local t = sub(s, o + 1, n and o + n or #s)
    local z = t:find('\0', 1, true)
    if z then t = sub(t, 1, z - 1) end
    return t
end
-- n bits (n <= 32) at bit position pos (0-based from the packet's start), least significant first
local function bits(s, pos, n)
    local v, got = 0, 0
    local i, off = floor(pos / 8), pos % 8
    while got < n do
        local b = byte(s, i + 1) or 0
        local take = min(8 - off, n - got)
        v = v + (floor(b / 2 ^ off) % 2 ^ take) * 2 ^ got
        got, off, i = got + take, 0, i + 1
    end
    return v
end
local function bit_set(s, i) -- bit i of a byte string (1-based string, bit 0 = lsb of byte 1)
    local b = byte(s, floor(i / 8) + 1)
    return b ~= nil and band(b, 2 ^ (i % 8)) ~= 0
end

wev.bits = bits

-------------------------------------------------------------------------------- 0x028

-- Windower's action table (windower.packets.parse_action), as libs/actions.lua reads and writes it.
function wev.parse_action(data)
    local act = {
        size = u8(data, 4),
        actor_id = bits(data, 40, 32),
        target_count = bits(data, 72, 10),
        category = bits(data, 82, 4),
        param = bits(data, 86, 16),
        unknown = bits(data, 102, 16),
        recast = bits(data, 118, 32),
        targets = {},
    }
    local pos, limit = 150, #data * 8
    for i = 1, act.target_count do
        if pos + 36 > limit then break end
        local t = { id = bits(data, pos, 32), action_count = bits(data, pos + 32, 4), actions = {} }
        pos = pos + 36
        for j = 1, t.action_count do
            if pos + 87 > limit then break end
            local a = {
                reaction = bits(data, pos, 5),
                animation = bits(data, pos + 5, 12),
                effect = bits(data, pos + 17, 4),
                stagger = bits(data, pos + 21, 3),
                knockback = bits(data, pos + 24, 3),
                param = bits(data, pos + 27, 17),
                message = bits(data, pos + 44, 10),
                unknown = bits(data, pos + 54, 31),
                has_add_effect = bits(data, pos + 85, 1) == 1,
                add_effect_animation = 0, add_effect_effect = 0, add_effect_param = 0, add_effect_message = 0,
                spike_effect_animation = 0, spike_effect_effect = 0, spike_effect_param = 0, spike_effect_message = 0,
            }
            pos = pos + 86
            if a.has_add_effect then
                a.add_effect_animation = bits(data, pos, 6)
                a.add_effect_effect = bits(data, pos + 6, 4)
                a.add_effect_param = bits(data, pos + 10, 17)
                a.add_effect_message = bits(data, pos + 27, 10)
                pos = pos + 37
            end
            a.has_spike_effect = bits(data, pos, 1) == 1
            pos = pos + 1
            if a.has_spike_effect then
                a.spike_effect_animation = bits(data, pos, 6)
                a.spike_effect_effect = bits(data, pos + 6, 4)
                a.spike_effect_param = bits(data, pos + 10, 14)
                a.spike_effect_message = bits(data, pos + 24, 10)
                pos = pos + 34
            end
            t.actions[j] = a
        end
        act.targets[i] = t
    end
    return act
end

-------------------------------------------------------------------------------- names

local bag_names = { [0] = 'inventory', 'safe', 'storage', 'temporary', 'locker', 'satchel', 'sack', 'case',
    'wardrobe', 'safe2', 'wardrobe2', 'wardrobe3', 'wardrobe4', 'wardrobe5', 'wardrobe6', 'wardrobe7',
    'wardrobe8', 'recycle' }
local BAGS = 18
local bag_ids = {}
for i = 0, BAGS - 1 do bag_ids[bag_names[i]] = i end
wev.bag_names = bag_names

local slot_names = { [0] = 'main', 'sub', 'range', 'ammo', 'head', 'body', 'hands', 'legs', 'feet', 'neck',
    'waist', 'left_ear', 'right_ear', 'left_ring', 'right_ring', 'back' }

local job_ens = { [0] = 'NON', 'WAR', 'MNK', 'WHM', 'BLM', 'RDM', 'THF', 'PLD', 'DRK', 'BST', 'BRD', 'RNG',
    'SAM', 'NIN', 'DRG', 'SMN', 'BLU', 'COR', 'PUP', 'DNC', 'SCH', 'GEO', 'RUN', 'MON' }

-- res.skills, in Windower's get_player().skills key form
local skill_names = {
    [1] = 'hand_to_hand', [2] = 'dagger', [3] = 'sword', [4] = 'great_sword', [5] = 'axe',
    [6] = 'great_axe', [7] = 'scythe', [8] = 'polearm', [9] = 'katana', [10] = 'great_katana',
    [11] = 'club', [12] = 'staff',
    [22] = 'automaton_melee', [23] = 'automaton_archery', [24] = 'automaton_magic', [25] = 'archery',
    [26] = 'marksmanship', [27] = 'throwing', [28] = 'guard', [29] = 'evasion', [30] = 'shield',
    [31] = 'parrying', [32] = 'divine_magic', [33] = 'healing_magic', [34] = 'enhancing_magic',
    [35] = 'enfeebling_magic', [36] = 'elemental_magic', [37] = 'dark_magic', [38] = 'summoning_magic',
    [39] = 'ninjutsu', [40] = 'singing', [41] = 'stringed_instrument', [42] = 'wind_instrument',
    [43] = 'blue_magic', [44] = 'geomancy', [45] = 'handbell', [48] = 'fishing', [49] = 'woodworking',
    [50] = 'smithing', [51] = 'goldsmithing', [52] = 'clothcraft', [53] = 'leathercraft',
    [54] = 'bonecraft', [55] = 'alchemy', [56] = 'cooking', [57] = 'synergy',
}

-- Windower's res files (merit and job point names), from wherever the Windower tree put them
local res_cache = {}
local function res_file(name)
    local r = res_cache[name]
    if r ~= nil then return r or nil end
    r = false
    local info = xi and xi.info
    local roots = {}
    if info and info.root then
        roots[#roots + 1] = info.root .. 'res/'
        roots[#roots + 1] = info.root .. 'addons/res/'
        roots[#roots + 1] = info.root .. 'resources/'
    end
    for _, root in ipairs(roots) do
        local chunk = loadfile(root .. name .. '.lua')
        if chunk then
            local ok, t = pcall(chunk)
            if ok and type(t) == 'table' then
                r = t
                break
            end
        end
    end
    res_cache[name] = r
    return r or nil
end

local function snake(s)
    return (s:lower():gsub('[:\'%.]', ''):gsub('[%s%-]+', '_'))
end

local merit_key_cache = {}
local function merit_key(id)
    local k = merit_key_cache[id]
    if k then return k end
    local r = res_file('merit_points')
    local e = r and r[id]
    if e and e.en then
        local n = e.en:gsub(' Skill$', '')
        if n ~= e.en then n = n:gsub(' Magic$', ''):gsub(' Instrument$', '') end
        k = snake(n)
    else
        k = id
    end
    merit_key_cache[id] = k
    return k
end

local function job_point_key(id)
    local r = res_file('job_points')
    local e = r and r[id]
    return e and e.en and snake(e.en) or id
end

-------------------------------------------------------------------------------- state

local S -- this state's view of the game, from packets

local function reset()
    S = {
        logged_in = false,
        jobs = {},
        vitals = {},
        buffs = nil,         -- list of ids (0x037)
        buff_times = {},     -- id list and end times (0x063 type 9)
        bags = {},           -- [bag] = { size=, enabled=, [slot] = item }
        equipment = {},      -- [slot] = { index, bag }
        inv_loaded = false,
        key_items = {},      -- [type 0-5] = 64 bytes
        spells = nil,        -- 128 bytes
        abilities = nil,     -- 0x0AC payload
        mounts = nil,
        job_data = {},       -- [job] = 0x044 packet
        mjob_packet = nil, sjob_packet = nil,
        merits = {}, job_points = {}, jp_levels = {},
        skills = {},
        recasts = {},        -- [recast id] = end (xi clock)
        treasure = {},
        wardrobe_enabled = {},
    }
end
reset()

local function clock() return xi and xi.clock and xi.clock() or os.clock() end

local function my_id()
    local game = mgame()
    if S.player_id then return S.player_id end
    if game then
        local ok, i = pcall(game.player_index)
        if ok and i then
            local ok2, id = pcall(game.entity, i, 'ServerId')
            if ok2 and id and id ~= 0 then return id end
        end
    end
    return nil
end

-------------------------------------------------------------------------------- events

local raise_fn, has_fn = function() end, function() return false end
local queue
local priming = false

local function wants(name) return not priming and has_fn(name) end

local function emit(name, ...)
    if priming or not has_fn(name) then return end
    local q = queue
    if not q then
        raise_fn(name, ...)
        return
    end
    q[#q + 1] = { name, select('#', ...), ... }
end

local function flush(q)
    for i = 1, #q do
        local ev = q[i]
        raise_fn(ev[1], unpack(ev, 3, ev[2] + 2))
    end
end

wev.provides = {}
for _, n in ipairs({ 'incoming chunk', 'outgoing chunk', 'action', 'action message', 'login', 'logout',
    'zone change', 'job change', 'status change', 'gain buff', 'lose buff', 'hp change', 'mp change',
    'tp change', 'hpp change', 'mpp change', 'level up', 'level down', 'gain experience', 'party invite',
    'add item', 'remove item', 'linkshell change', 'emote', 'examined', 'chat message', 'weather change',
    'time change', 'day change', 'moon change', 'moon pct change', 'target change' }) do
    wev.provides[n] = true
end

-------------------------------------------------------------------------------- helpers

local function set_vital(key, event, v)
    local old = S.vitals[key]
    S.vitals[key] = v
    if old ~= nil and old ~= v then emit(event, v, old) end
end

local function vitals(hp, mp, tp, hpp, mpp)
    set_vital('hp', 'hp change', hp)
    set_vital('mp', 'mp change', mp)
    set_vital('tp', 'tp change', tp)
    set_vital('hpp', 'hpp change', hpp)
    set_vital('mpp', 'mpp change', mpp)
end

local function jobs(main, main_level, sub, sub_level)
    local om, os_ = S.main_job, S.sub_job
    S.main_job, S.main_job_level, S.sub_job, S.sub_job_level = main, main_level, sub, sub_level
    if main and main ~= 0 then S.jobs[main] = S.jobs[main] or main_level end
    if om ~= nil and (om ~= main or os_ ~= sub) then
        emit('job change', main, main_level, sub, sub_level)
    end
end

local function set_weather(w)
    local old = S.weather
    S.weather = w
    if old ~= nil and old ~= w then emit('weather change', w, old) end
end

local function bag(b)
    local t = S.bags[b]
    if not t then
        t = {}
        S.bags[b] = t
    end
    return t
end

local ZERO24 = string.rep('\0', 24)

-- the equipped linkshell's name, from its item's extdata (6-bit characters, most significant first)
local function ls_name_of(ext)
    if not ext or #ext < 10 or byte(ext, 9) == 0 then return nil end
    local out, pos = {}, 72 -- bit 0 of byte 10
    local total = #ext * 8
    while pos + 6 <= total do
        local v = 0
        for k = 0, 5 do
            local p = pos + k
            local b = byte(ext, floor(p / 8) + 1)
            v = v * 2 + (floor(b / 2 ^ (7 - p % 8)) % 2)
        end
        if v >= 1 and v <= 26 then out[#out + 1] = char(96 + v)
        elseif v >= 27 and v <= 52 then out[#out + 1] = char(64 + v - 26)
        else break end
        pos = pos + 6
    end
    return #out > 0 and table.concat(out) or nil
end

local function update_linkshell()
    local name
    for b = 0, BAGS - 1 do
        local t = S.bags[b]
        if t then
            for s, it in pairs(t) do
                if type(s) == 'number' and it.status == 0x13 and it.id ~= 0 then
                    name = ls_name_of(it.extdata)
                    S.linkshell_slot = s
                    break
                end
            end
        end
        if name then break end
    end
    local old = S.linkshell
    S.linkshell = name
    if old ~= name and S.inv_loaded then emit('linkshell change', name or '', old or '') end
end

local function item_changed(b, s, it, old_id, old_count, old_status)
    if S.inv_loaded then
        if old_id ~= 0 and (it.id ~= old_id or it.count == 0) then
            emit('remove item', b, s, old_id, old_count)
        end
        if it.id ~= 0 and it.count ~= 0 and it.id ~= old_id then
            emit('add item', b, s, it.id, it.count)
        end
    end
    if it.status == 0x13 or old_status == 0x13 then update_linkshell() end
end

local function slot_item(b, s)
    local t = bag(b)
    local it = t[s]
    if not it then
        it = { id = 0, count = 0, status = 0, bazaar = 0, extdata = ZERO24 }
        t[s] = it
    end
    return it
end

-- Vana'diel time as the server keeps it: 25x real time from 2002-01-01 00:00 JST, which was
-- 886/01/01 00:00. The week (8 days) and the moon (84 days: new at day 42, full at 0) follow.
local VBASE = 1009810800
local function vana_minutes(now)
    return floor(((now or os.time()) - VBASE) * 25 / 60) + 886 * 360 * 1440
end

local function clock_info(now)
    local m = vana_minutes(now)
    local days = floor(m / 1440)
    local c = (days + 26) % 84
    local pct = c >= 42 and floor(100 * (c - 42) / 42 + 0.5) or floor(100 * (1 - c / 42) + 0.5)
    local x = (c - 42) % 84 -- 0 new moon, 42 full
    return {
        time = m % 1440,
        day = days % 8,
        moon = pct,
        moon_phase = floor((x + 3) / 7) % 12,
        year = floor(days / 360),
        month = floor(days / 30) % 12 + 1,
        date = days % 30 + 1,
    }
end
wev.clock_info = clock_info

-- a buff time (0x063: 1/60 s since 2002, wrapping every 2^32) as a unix time, the wrap nearest now
local function buff_unix(t)
    if t == 0 or t == 0x7FFFFFFF then return nil end
    local now, best, bd = os.time(), nil, nil
    for k = 0, 24 do
        local u = VBASE + t / 60 + k * 0x100000000 / 60
        local d = abs(u - now)
        if not bd or d < bd then best, bd = u, d end
    end
    return best
end

-------------------------------------------------------------------------------- incoming packets

local P = {}   -- state parsers: run on the packet as it arrived, before the chunk event
local D = {}   -- derived events: run on the packet the game gets, after the chunk event

P[0x00A] = function(d)
    S.player_id = u32(d, 0x04)
    S.player_index = u16(d, 0x08)
    local name = cstr(d, 0x84, 16)
    S.name = name
    local zone = u16(d, 0x30)
    local old = S.zone
    S.zone = zone
    S.mog_house = u8(d, 0x80) ~= 0
    S.status = u8(d, 0x1F)
    set_weather(u16(d, 0x68))
    for j = 0, 15 do
        local l = u8(d, 0xBC + j)
        if l ~= 0 or S.jobs[j] == nil then S.jobs[j] = l end
    end
    local mj, sj = u8(d, 0xB4), u8(d, 0xB7)
    jobs(mj, S.jobs[mj] or S.main_job_level or 0, sj, S.jobs[sj] or S.sub_job_level or 0)
    S.vitals.max_hp, S.vitals.max_mp = u32(d, 0xE8), u32(d, 0xEC)
    if not S.logged_in then
        S.logged_in = true
        emit('login', name)
    elseif old and old ~= zone then
        emit('zone change', zone, old)
    end
end

P[0x00B] = function(d)
    if u32(d, 0x04) == 1 then
        local name = S.name
        local keep = S
        reset()
        S.zone_before_logout = keep.zone
        emit('logout', name or '')
    end
end

P[0x01B] = function(d)
    for j = 1, 22 do S.jobs[j] = u8(d, 0x48 + j) end
    S.jobs[0] = 0
    local mj, sj = u8(d, 0x08), u8(d, 0x0B)
    jobs(mj, S.jobs[mj] or 0, sj, S.jobs[sj] or 0)
    S.vitals.max_hp, S.vitals.max_mp = u32(d, 0x3C), u32(d, 0x40)
end

P[0x01C] = function(d)
    for b = 0, BAGS - 1 do
        local t = bag(b)
        t.size = u8(d, 0x04 + b)
        t.enabled = u16(d, 0x24 + 2 * b) ~= 0
    end
end

P[0x01D] = function(d)
    if u8(d, 0x04) == 1 then S.inv_loaded = true end
end

P[0x01E] = function(d)
    local b, s = u8(d, 0x08), u8(d, 0x09)
    local it = slot_item(b, s)
    local oid, oc, os_ = it.id, it.count, it.status
    it.count, it.status = u32(d, 0x04), u8(d, 0x0A)
    if it.count == 0 then it.id, it.bazaar, it.extdata = 0, 0, ZERO24 end
    item_changed(b, s, it, oid, oc, os_)
end

P[0x01F] = function(d)
    local b, s = u8(d, 0x0A), u8(d, 0x0B)
    local it = slot_item(b, s)
    local oid, oc, os_ = it.id, it.count, it.status
    it.count, it.id, it.status = u32(d, 0x04), u16(d, 0x08), u8(d, 0x0C)
    if it.id ~= oid then it.bazaar, it.extdata = 0, ZERO24 end
    if it.id == 0 then it.count = 0 end
    item_changed(b, s, it, oid, oc, os_)
end

P[0x020] = function(d)
    local b, s = u8(d, 0x0E), u8(d, 0x0F)
    local it = slot_item(b, s)
    local oid, oc, os_ = it.id, it.count, it.status
    it.count, it.bazaar, it.id, it.status = u32(d, 0x04), u32(d, 0x08), u16(d, 0x0C), u8(d, 0x10)
    it.extdata = sub(d, 0x12, 0x29)
    if #it.extdata < 24 then it.extdata = it.extdata .. string.rep('\0', 24 - #it.extdata) end
    if it.id == 0 then it.count = 0 end
    item_changed(b, s, it, oid, oc, os_)
end

P[0x02D] = function(d)
    local id = my_id()
    if not id or u32(d, 0x04) ~= id then return end
    local p1, p2, msg = u32(d, 0x10), u32(d, 0x14), u16(d, 0x18)
    if msg == 8 then emit('gain experience', p1, 0, false)
    elseif msg == 105 then emit('gain experience', p2, 0, false)
    elseif msg == 253 then emit('gain experience', p1, p2, false)
    elseif msg == 371 then emit('gain experience', p1, 0, true)
    elseif msg == 372 then emit('gain experience', p1, p2, true)
    elseif msg == 9 then emit('level up', p1)
    elseif msg == 11 then emit('level down', p1)
    end
end

P[0x037] = function(d)
    S.player_id = u32(d, 0x24)
    local list = {}
    for i = 0, 31 do
        local hi = floor(u8(d, 0x4C + floor(i / 4)) / 4 ^ (i % 4)) % 4
        local id = u8(d, 0x04 + i) + 256 * hi
        if id ~= 255 then list[#list + 1] = id end
    end
    local old = S.buffs
    S.buffs = list
    if old then
        local count = {}
        for _, v in ipairs(old) do count[v] = (count[v] or 0) + 1 end
        local gained = {}
        for _, v in ipairs(list) do
            if (count[v] or 0) > 0 then count[v] = count[v] - 1 else gained[#gained + 1] = v end
        end
        for _, v in ipairs(old) do
            if (count[v] or 0) > 0 then
                count[v] = count[v] - 1
                emit('lose buff', v)
            end
        end
        for _, v in ipairs(gained) do emit('gain buff', v) end
    end
    local st, os_ = u8(d, 0x30), S.status
    S.status = st
    if os_ ~= nil and os_ ~= st then emit('status change', st, os_) end
    -- wardrobes 3-8 available
    local w = u8(d, 0x5C)
    S.wardrobe_enabled[11] = band(w, 0x01) ~= 0
    S.wardrobe_enabled[12] = band(w, 0x02) ~= 0
    S.wardrobe_enabled[13] = band(w, 0x08) ~= 0
    S.wardrobe_enabled[14] = band(w, 0x10) ~= 0
    S.wardrobe_enabled[15] = band(w, 0x20) ~= 0
    S.wardrobe_enabled[16] = band(w, 0x40) ~= 0
end

P[0x044] = function(d)
    local job, is_sub = u8(d, 0x04), u8(d, 0x05) ~= 0
    S.job_data[job] = d
    if is_sub then S.sjob_packet = d else S.mjob_packet = d end
end

P[0x050] = function(d)
    local s = u8(d, 0x05)
    S.equipment[s] = { u8(d, 0x04), u8(d, 0x06) }
end

P[0x055] = function(d)
    local t = u32(d, 0x84)
    if t < 16 then S.key_items[t] = sub(d, 0x05, 0x44) end
end

P[0x057] = function(d)
    set_weather(u8(d, 0x08))
end

P[0x061] = function(d)
    S.vitals.max_hp, S.vitals.max_mp = u32(d, 0x04), u32(d, 0x08)
    local mj, ml, sj, sl = u8(d, 0x0C), u8(d, 0x0D), u8(d, 0x0E), u8(d, 0x0F)
    jobs(mj, ml, sj, sl)
    local x = S.stats or {}
    S.stats = x
    x.exp, x.exp_required = u16(d, 0x10), u16(d, 0x12)
    local names = { 'str', 'dex', 'vit', 'agi', 'int', 'mnd', 'chr' }
    x.base, x.added = {}, {}
    for i, n in ipairs(names) do
        x.base[n] = u16(d, 0x14 + 2 * (i - 1))
        local a = u16(d, 0x22 + 2 * (i - 1))
        x.added[n] = a >= 32768 and a - 65536 or a
    end
    x.attack, x.defense = u16(d, 0x30), u16(d, 0x32)
    x.title, x.nation_rank, x.rank_points, x.home_point = u16(d, 0x44), u16(d, 0x46), u16(d, 0x48) % 0x1000, u16(d, 0x4A)
    x.nation, x.superior_level, x.item_level = u8(d, 0x50), u8(d, 0x52), u8(d, 0x54)
    x.unity_id = bits(d, 0x58 * 8, 5)
    x.unity_rank = bits(d, 0x58 * 8 + 5, 5)
    x.unity_points = bits(d, 0x58 * 8 + 10, 17)
    x.master_level = u8(d, 0x65)
    x.master_breaker = band(u8(d, 0x66), 1) ~= 0
    x.exemplar, x.exemplar_required = u32(d, 0x68), u32(d, 0x6C)
end

P[0x062] = function(d)
    for i = 0, 0x2F do
        local v = u16(d, 0x80 + 2 * i)
        S.skills[i] = { level = v % 32768, capped = v >= 32768 }
    end
    for i = 0, 9 do
        local v = u16(d, 0xE0 + 2 * i)
        S.skills[0x30 + i] = { level = floor(v / 32) % 1024, rank = v % 32, capped = v >= 32768 }
    end
end

P[0x063] = function(d)
    local order = u16(d, 0x04)
    if order == 2 then
        S.limit_points = u16(d, 0x08)
        S.merit_points = bits(d, 0x0A * 8, 7)
        S.assimilation = bits(d, 0x0A * 8 + 7, 6)
        S.limit_breaker = bits(d, 0x0A * 8 + 13, 1) == 1
        S.exp_capped = bits(d, 0x0A * 8 + 14, 1) == 1
        S.limit_mode = bits(d, 0x0A * 8 + 15, 1) == 1
        S.max_merit_points = u8(d, 0x0C)
    elseif order == 5 then
        for j = 0, 23 do
            local o = 0x0C + 6 * j
            S.job_points[j] = { cp = u16(d, o), jp = u16(d, o + 2), jp_spent = u16(d, o + 4) }
        end
    elseif order == 9 then
        local list = {}
        for i = 0, 31 do
            local id = u16(d, 0x08 + 2 * i)
            if id ~= 0xFF and id ~= 0 then
                list[#list + 1] = { id = id, time = buff_unix(u32(d, 0x48 + 4 * i)), raw_time = u32(d, 0x48 + 4 * i) }
            end
        end
        S.buff_times = list
    end
end

P[0x08C] = function(d)
    local n = u8(d, 0x04)
    for k = 0, n - 1 do
        local o = 0x08 + 4 * k
        if o + 4 > #d then break end
        local id = u16(d, o)
        if id ~= 0 then S.merits[id] = { value = u8(d, o + 3), next_cost = u8(d, o + 2) } end
    end
end

P[0x08D] = function(d)
    for o = 0x04, #d - 4, 4 do
        local id = u16(d, o)
        if id ~= 0 then S.jp_levels[id] = floor(u16(d, o + 2) / 1024) end
    end
end

P[0x0AA] = function(d) S.spells = sub(d, 0x05, 0x84) end
P[0x0AC] = function(d) S.abilities = d end
P[0x0AE] = function(d) S.mounts = sub(d, 0x05) end

P[0x0D2] = function(d)
    local i = u8(d, 0x14)
    S.treasure[i] = { item_id = u16(d, 0x10), dropper_id = u32(d, 0x08), count = u32(d, 0x0C), timestamp = u32(d, 0x18) }
end

P[0x0D3] = function(d)
    local i = u8(d, 0x14)
    if u8(d, 0x15) ~= 0 then S.treasure[i] = nil end
end

local function self_vitals(d, id_o, hpp_o, mpp_o)
    local id = my_id()
    if not id or u32(d, id_o) ~= id then return false end
    vitals(u32(d, 0x08), u32(d, 0x0C), u32(d, 0x10), u8(d, hpp_o), u8(d, mpp_o))
    return true
end

P[0x0DD] = function(d) self_vitals(d, 0x04, 0x1D, 0x1E) end
P[0x0E2] = function(d) self_vitals(d, 0x04, 0x1D, 0x1E) end
P[0x0DF] = function(d)
    if self_vitals(d, 0x04, 0x16, 0x17) then
        local mj, sj = u8(d, 0x20), u8(d, 0x22)
        if mj ~= 0 then jobs(mj, u8(d, 0x21), sj, u8(d, 0x23)) end
    end
end

P[0x119] = function(d)
    local now = clock()
    S.recasts = {}
    for i = 0, 30 do
        local o = 0x04 + 8 * i
        local dur, id = u16(d, o), u8(d, o + 3)
        if dur > 0 then S.recasts[id] = now + dur end
    end
end

D[0x028] = function(d)
    if wants('action') then emit('action', wev.parse_action(d)) end
end

D[0x029] = function(d)
    emit('action message', u32(d, 0x04), u32(d, 0x08), u16(d, 0x14), u16(d, 0x16), u16(d, 0x18),
        u32(d, 0x0C), u32(d, 0x10), u16(d, 0x1A))
end

D[0x017] = function(d)
    emit('chat message', cstr(d, 0x17), cstr(d, 0x08, 15), u8(d, 0x04), u8(d, 0x05) ~= 0)
end

D[0x05A] = function(d)
    emit('emote', u16(d, 0x10), u32(d, 0x04), u32(d, 0x08), u8(d, 0x16) == 2)
end

D[0x009] = function(d)
    if u16(d, 0x0A) ~= 89 then return end
    local o = 0x0D
    local c = u8(d, o)
    if c < 0x20 or c >= 0x7F then o = 0x0E end
    emit('examined', cstr(d, o, 16), u16(d, 0x08))
end

D[0x0DC] = function(d)
    emit('party invite', cstr(d, 0x0C, 16), u32(d, 0x04))
end

-- which events a derived parser can raise: skip the work when the addon listens to none
local D_events = {
    [0x028] = { 'action' }, [0x029] = { 'action message' }, [0x017] = { 'chat message' },
    [0x05A] = { 'emote' }, [0x009] = { 'examined' }, [0x0DC] = { 'party invite' },
}

local function any_wanted(list)
    for i = 1, #list do
        if has_fn(list[i]) then return true end
    end
    return false
end

-------------------------------------------------------------------------------- the xi handlers

local function on_packet(e, parsers, derived, chunk_event)
    local id, data = e.id, e.data
    local p = parsers and parsers[id]
    local dp = derived and derived[id]
    if not p and not dp then
        -- most packets: only the chunk event
        if has_fn(chunk_event) then
            local r = raise_fn(chunk_event, id, data, e.modified or data, e.injected and true or false, e.blocked and true or false)
            if r == true then
                e.blocked = true
            elseif type(r) == 'string' then
                e.modified = r
            end
        end
        return
    end
    local q = {}
    local prev = queue
    queue = q
    if p and data then
        local ok, err = pcall(p, data)
        if not ok then xi.log('windower_events: packet 0x' .. string.format('%03X', id) .. ': ' .. tostring(err)) end
    end
    if has_fn(chunk_event) then
        queue = prev
        local r = raise_fn(chunk_event, id, data, e.modified or data, e.injected and true or false, e.blocked and true or false)
        queue = q
        if r == true then
            e.blocked = true
        elseif type(r) == 'string' then
            e.modified = r
        end
    end
    if dp and not e.blocked and any_wanted(D_events[id]) then
        local ok, err = pcall(dp, e.modified or data)
        if not ok then xi.log('windower_events: packet 0x' .. string.format('%03X', id) .. ': ' .. tostring(err)) end
    end
    queue = prev
    flush(q)
end

local last_clock, last_target, frame_tick = nil, nil, 0
local target_paths

local function current_target()
    if not game then return nil end
    if not target_paths then
        target_paths = {
            sub = game.path('target_t', 'IsSubTargetActive') or 'IsSubTargetActive',
            i0 = game.path('target_t', 'Targets[0].Index') or 'Targets[0].Index',
            i1 = game.path('target_t', 'Targets[1].Index') or 'Targets[1].Index',
            a0 = game.path('target_t', 'Targets[0].IsActive') or 'Targets[0].IsActive',
            a1 = game.path('target_t', 'Targets[1].IsActive') or 'Targets[1].IsActive',
        }
    end
    local tp = target_paths
    local ok, subactive = pcall(game.target, tp.sub)
    if not ok or subactive == nil then return nil end
    local which = (subactive ~= 0) and 1 or 0
    local active = game.target(which == 1 and tp.a1 or tp.a0)
    if not active or active == 0 then return 0 end
    return game.target(which == 1 and tp.i1 or tp.i0) or 0
end

local function on_frame()
    local q = {}
    queue = q
    -- the clock: Windower's time/day/moon events
    if has_fn('time change') or has_fn('day change') or has_fn('moon change') or has_fn('moon pct change') then
        local c = clock_info()
        local o = last_clock
        last_clock = c
        if o then
            if c.time ~= o.time then emit('time change', c.time, o.time) end
            if c.day ~= o.day then emit('day change', c.day, o.day) end
            if c.moon_phase ~= o.moon_phase then emit('moon change', c.moon_phase, o.moon_phase) end
            if c.moon ~= o.moon then emit('moon pct change', c.moon, o.moon) end
        end
    else
        last_clock = nil
    end
    if has_fn('target change') then
        local t = current_target()
        if t ~= nil then
            if last_target ~= nil and t ~= last_target then emit('target change', t) end
            last_target = t
        end
    else
        last_target = nil
    end
    queue = nil
    flush(q)
end

-------------------------------------------------------------------------------- getters

local function empty_item(s)
    return { id = 0, count = 0, status = 0, bazaar = 0, extdata = ZERO24, slot = s }
end

local item_paths
local function memory_bag(b)
    local game = mgame()
    if not game or not game.container_max then return nil end
    local ok, raw = pcall(game.container_max, b)
    if not ok or not raw or raw <= 0 then return nil end
    if not item_paths then
        item_paths = {}
        for _, f in ipairs({ 'Id', 'Count', 'Flags', 'Price', 'Extra' }) do
            item_paths[f] = game.path('item_t', f) or f
        end
    end
    local ip = item_paths
    local t = { max = raw - 1, count = 0 }
    local ok2, cap2 = pcall(game.inventory, 'ContainerMaxCapacity2', b)
    t.enabled = (ok2 and cap2 and cap2 ~= 0) or (b >= 11 and b <= 16 and S.wardrobe_enabled[b]) or b == 0
    for s = 1, t.max do
        local id = game.item(b, s, ip.Id) or 0
        if id ~= 0 then
            local ext = game.item(b, s, ip.Extra) or ''
            t[s] = { id = id, count = game.item(b, s, ip.Count) or 0, status = game.item(b, s, ip.Flags) or 0,
                bazaar = game.item(b, s, ip.Price) or 0, extdata = sub(ext .. ZERO24, 1, 24), slot = s }
            t.count = t.count + 1
        else
            t[s] = empty_item(s)
        end
    end
    t.gil = b == 0 and (game.item(0, 0, ip.Count) or 0) or nil
    return t
end

local function packet_bag(b)
    local src = S.bags[b] or {}
    local size = src.size or 0
    local t = { max = size > 0 and size - 1 or 0, count = 0 }
    if b >= 11 and b <= 16 and S.wardrobe_enabled[b] ~= nil then
        t.enabled = S.wardrobe_enabled[b] and size > 0
    else
        t.enabled = src.enabled
        if t.enabled == nil then t.enabled = size > 0 end
    end
    for s = 1, t.max do
        local it = src[s]
        if it and it.id ~= 0 then
            t[s] = { id = it.id, count = it.count, status = it.status, bazaar = it.bazaar, extdata = it.extdata, slot = s }
            t.count = t.count + 1
        else
            t[s] = empty_item(s)
        end
    end
    local g = src[0]
    t.gil = b == 0 and (g and g.count or 0) or nil
    return t
end

local function get_bag(b)
    local t = memory_bag(b) or packet_bag(b)
    t.gil = nil
    return t
end

local function get_equipment()
    local game = mgame()
    local e = {}
    local mem = false
    if game and game.equipment then
        local ok, v = pcall(game.equipment, 0)
        mem = ok and v ~= nil
    end
    for s = 0, 15 do
        local idx, b = 0, 0
        if mem then
            local _, index = game.equipment(s)
            if index then idx, b = band(index, 0xFF), rshift(band(index, 0xFF00), 8) end
        else
            local q = S.equipment[s]
            if q then idx, b = q[1], q[2] end
        end
        e[slot_names[s]] = idx
        e[slot_names[s] .. '_bag'] = b
    end
    return e
end

local function get_treasure()
    local game = mgame()
    local t = {}
    local mem = false
    if game and game.treasure then
        local ok, v = pcall(game.treasure, 0, 'ItemId')
        mem = ok and v ~= nil
    end
    for i = 0, 9 do
        if mem then
            local id = game.treasure(i, 'ItemId') or 0
            local use = game.treasure(i, 'Use') or 0
            if id ~= 0 and use ~= 0 then
                t[i] = { item_id = id, count = game.treasure(i, 'Count') or 1, timestamp = game.treasure(i, 'DropTime') or 0,
                    dropper_id = 0 }
            end
        elseif S.treasure[i] then
            local x = S.treasure[i]
            t[i] = { item_id = x.item_id, count = x.count, timestamp = x.timestamp, dropper_id = x.dropper_id }
        end
    end
    return t
end

local function get_gil()
    local game = mgame()
    if game and game.container_max and (game.container_max(0) or 0) > 0 then
        return game.item(0, 0, 'Count') or 0
    end
    local g = S.bags[0] and S.bags[0][0]
    return g and g.count or 0
end

-- get_items(): every bag, equipment, gil, treasure; get_items(bag [, index]) with a bag id or name.
function wev.get_items(b, index)
    if b == nil then
        local t = {}
        for i = 0, BAGS - 1 do t[bag_names[i]] = get_bag(i) end
        t.equipment = get_equipment()
        t.treasure = get_treasure()
        t.gil = get_gil()
        t.max_inventory = t.inventory.max
        t.count_inventory = t.inventory.count
        return t
    end
    if type(b) == 'string' then
        if b == 'equipment' then return get_equipment() end
        if b == 'treasure' then return get_treasure() end
        if b == 'gil' then return get_gil() end
        b = bag_ids[b]
        if not b then return nil end
    end
    if type(b) ~= 'number' or b < 0 or b >= BAGS then return nil end
    local t = get_bag(b)
    if index ~= nil then return t[index] end
    return t
end

-- get_key_items(): the ids of the key items the player has
function wev.get_key_items()
    local game = mgame()
    local list = {}
    if game and game.key_item then
        local ok, v = pcall(game.key_item, 0)
        if ok and v ~= nil then
            for id = 0, 16 * 512 - 1 do
                if game.key_item(id) then list[#list + 1] = id end
            end
            if #list > 0 or not next(S.key_items) then return list end
            list = {}
        end
    end
    for t = 0, 15 do
        local s = S.key_items[t]
        if s then
            for i = 0, 511 do
                if bit_set(s, i) then list[#list + 1] = t * 512 + i end
            end
        end
    end
    table.sort(list)
    return list
end

-- get_spells(): [id] = known, for every spell id 0-1023
function wev.get_spells()
    local game = mgame()
    local t = {}
    if game and game.has_spell_data and game.has_spell_data() then
        for id = 0, 1023 do t[id] = game.spell_known(id) and true or false end
        return t
    end
    local s = S.spells
    for id = 0, 1023 do t[id] = s ~= nil and bit_set(s, id) or false end
    return t
end

-- get_abilities(): the lists Windower gives (res.job_abilities / weapon_skills / job_traits / mounts ids)
function wev.get_abilities()
    local r = { job_abilities = {}, weapon_skills = {}, job_traits = {}, pet_commands = {}, mounts = {} }
    local d = S.abilities
    if d then
        local ws, ja, pet, tr = sub(d, 0x05, 0x44), sub(d, 0x45, 0x84), sub(d, 0x85, 0xC4), sub(d, 0xC5, 0xE4)
        for i = 0, 511 do
            if bit_set(ws, i) then r.weapon_skills[#r.weapon_skills + 1] = i end
            if bit_set(ja, i) then r.job_abilities[#r.job_abilities + 1] = i end
            if bit_set(pet, i) then r.pet_commands[#r.pet_commands + 1] = 512 + i end
            if i < 256 and bit_set(tr, i) then r.job_traits[#r.job_traits + 1] = i end
        end
    end
    local m = S.mounts
    if m then
        for i = 0, #m * 8 - 1 do
            if bit_set(m, i) then r.mounts[#r.mounts + 1] = i end
        end
    end
    return r
end

-- get_ability_recasts(): [recast id] = seconds left (0 when ready), ids 0-255 always present
function wev.get_ability_recasts()
    local game = mgame()
    local t = {}
    for i = 0, 255 do t[i] = 0 end
    local mem = false
    if game and game.ability_recast then
        local ok, v = pcall(game.ability_recast, 0)
        mem = ok and v ~= nil
    end
    if mem then
        for i = 0, 30 do
            local timer, rid = game.ability_recast(i)
            if timer and timer > 0 and rid then t[rid] = timer / 60 end
        end
    else
        local now = clock()
        for rid, e in pairs(S.recasts) do
            local left = e - now
            if left > 0 then t[rid] = left end
        end
    end
    return t
end

-- get_spell_recasts(): [spell id] = 1/60 seconds left, 0-1023
function wev.get_spell_recasts()
    local game = mgame()
    local t = {}
    local mem = game and game.spell_recast and game.spell_recast(0) ~= nil
    for id = 0, 1023 do
        t[id] = mem and (game.spell_recast(id) or 0) or 0
    end
    return t
end

local function job_data(d)
    if not d then return {} end
    local job = u8(d, 0x04)
    if job == 16 then -- BLU: the 20 set spells (512 = empty)
        local spells = {}
        for i = 0, 19 do spells[i + 1] = 512 + u8(d, 0x08 + i) end
        return { spells = spells }
    elseif job == 18 then -- PUP
        local r = {
            head = 0x2000 + u8(d, 0x08), frame = 0x2000 + u8(d, 0x09),
            attachments = {}, available_heads = {}, available_frames = {}, available_attachments = {},
            name = cstr(d, 0x58, 16),
            hp = u16(d, 0x68), max_hp = u16(d, 0x6A), mp = u16(d, 0x6C), max_mp = u16(d, 0x6E),
            melee = u16(d, 0x70), max_melee = u16(d, 0x72), ranged = u16(d, 0x74), max_ranged = u16(d, 0x76),
            magic = u16(d, 0x78), max_magic = u16(d, 0x7A),
        }
        for i = 0, 11 do
            local a = u8(d, 0x0A + i)
            if a ~= 0 then r.attachments[i + 1] = 0x2100 + a end
        end
        local heads, frames = sub(d, 0x19, 0x1C), sub(d, 0x1D, 0x20)
        for i = 0, 31 do
            if bit_set(heads, i) then r.available_heads[#r.available_heads + 1] = 0x2000 + i end
            if bit_set(frames, i) then r.available_frames[#r.available_frames + 1] = 0x2020 + i end
        end
        local att = sub(d, 0x39, 0x58)
        for i = 0, 255 do
            if bit_set(att, i) then r.available_attachments[#r.available_attachments + 1] = 0x2100 + i end
        end
        local names = { 'str', 'dex', 'vit', 'agi', 'int', 'mnd', 'chr' }
        for i, n in ipairs(names) do
            r[n] = u16(d, 0x80 + 4 * (i - 1))
            r[n .. '_modifier'] = u16(d, 0x82 + 4 * (i - 1))
        end
        return r
    elseif job == 23 then -- MON
        local r = { species = u16(d, 0x08), instinct = {}, name1 = u8(d, 0x24), name2 = u8(d, 0x25) }
        for i = 0, 11 do r.instinct[i + 1] = u16(d, 0x0C + 2 * i) end
        r.instincts = r.instinct
        return r
    end
    return {}
end

function wev.get_mjob_data()
    local d = S.mjob_packet
    if d and S.main_job and u8(d, 0x04) ~= S.main_job then d = S.job_data[S.main_job] end
    return job_data(d)
end

function wev.get_sjob_data()
    local d = S.sjob_packet
    return job_data(d)
end

-- get_player_extra(): what Windower's get_player() has that comes from packets
function wev.get_player_extra()
    local p = {
        id = S.player_id, index = S.player_index, name = S.name, status = S.status,
        main_job_id = S.main_job, main_job_level = S.main_job_level,
        sub_job_id = S.sub_job ~= 0 and S.sub_job or nil, sub_job_level = S.sub_job ~= 0 and S.sub_job_level or nil,
        buffs = {}, buff_details = {}, skills = {}, jobs = {}, merits = {}, job_points = {},
        vitals = {
            hp = S.vitals.hp, max_hp = S.vitals.max_hp, hpp = S.vitals.hpp,
            mp = S.vitals.mp, max_mp = S.vitals.max_mp, mpp = S.vitals.mpp, tp = S.vitals.tp,
        },
        linkshell = S.linkshell, linkshell_slot = S.linkshell_slot,
        limit_points = S.limit_points, merit_points = S.merit_points, max_merit_points = S.max_merit_points,
        limit_breaker = S.limit_breaker, exp_capped = S.exp_capped, limit_mode = S.limit_mode,
    }
    if S.buffs then
        for i, v in ipairs(S.buffs) do p.buffs[i] = v end
    end
    for i, b in ipairs(S.buff_times) do p.buff_details[i] = { id = b.id, time = b.time } end
    for id, sk in pairs(S.skills) do
        local n = skill_names[id]
        if n then p.skills[n] = sk.level end
    end
    for j = 1, 23 do p.jobs[job_ens[j]] = S.jobs[j] or 0 end
    for id, m in pairs(S.merits) do p.merits[merit_key(id)] = m.value end
    for j = 1, 22 do
        local k = job_ens[j]:lower()
        local jp = S.job_points[j]
        local t = { cp = jp and jp.cp or 0, jp = jp and jp.jp or 0, jp_spent = jp and jp.jp_spent or 0 }
        p.job_points[k] = t
    end
    for id, lvl in pairs(S.jp_levels) do
        local j = floor(id / 64)
        local k = job_ens[j] and job_ens[j]:lower()
        if k and p.job_points[k] then p.job_points[k][job_point_key(id)] = lvl end
    end
    local x = S.stats
    if x then
        p.nation, p.title, p.item_level, p.superior_level = x.nation, x.title, x.item_level, x.superior_level
        p.master_level, p.exp, p.max_exp = x.master_level, x.exp, x.exp_required
        p.stats = x.base
        p.stats_bonus = x.added
        p.attack, p.defense = x.attack, x.defense
        p.rank, p.rank_points, p.homepoint = x.nation_rank, x.rank_points, x.home_point
        p.unity = { id = x.unity_id, rank = x.unity_rank, points = x.unity_points }
    end
    return p
end

-- get_info(): the packet- and clock-derived parts of windower.ffxi.get_info()
function wev.get_info()
    local c = clock_info()
    return {
        logged_in = S.logged_in, zone = S.zone, mog_house = S.mog_house or false, weather = S.weather or 0,
        day = c.day, moon = c.moon, moon_phase = c.moon_phase, time = c.time,
        language = 'English',
    }
end
wev.get_info_extra = wev.get_info

-- for tests and the layer: the raw packet state
function wev.state() return S end

-------------------------------------------------------------------------------- install

-- feeds the last packet of each id the host has seen, so state is there for an addon loaded late
local PRIME = { 0x00A, 0x01B, 0x01C, 0x061, 0x062, 0x063, 0x08C, 0x08D, 0x0AA, 0x0AC, 0x0AE, 0x044,
    0x057, 0x037, 0x055, 0x050, 0x0DF, 0x119 }

local function prime()
    if not native_packets or not native_packets.last then return end
    priming = true
    for _, id in ipairs(PRIME) do
        local d = native_packets.last(false, id)
        if d and P[id] then pcall(P[id], d) end
    end
    priming = false
    S.inv_loaded = S.logged_in
end

local installed = false

function wev.install(opts)
    raise_fn = opts.raise or raise_fn
    has_fn = opts.has or has_fn
    wev.windower = opts.windower
    if installed then return wev end
    installed = true
    prime()
    xi.events.on('packet_in', function(e) on_packet(e, P, D, 'incoming chunk') end, 'windower_events.in')
    xi.events.on('packet_out', function(e) on_packet(e, nil, nil, 'outgoing chunk') end, 'windower_events.out')
    xi.events.on('frame', on_frame, 'windower_events.frame')
    return wev
end

return wev
