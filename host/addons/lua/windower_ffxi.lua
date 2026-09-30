--[==[
windower.ffxi and windower.packets: the game's state as Windower's tables.

    require('xi.windower_ffxi')(windower, helpers)

Read at call time from the game's memory (xi.game, game_lua.c) where it is there, and from the
packet-fed state of windower_events (windower.__events: get_items, get_key_items, get_spells,
get_abilities, get_*_recasts, get_mjob_data, get_player_extra, get_info) where Windower's own
come from packets or memory has nothing (before login, the harness). The actions that Windower
performs with packets (set_equip, cancel_buff, lot/pass, item moves) send those packets.
--]==]

local game = xi.game
local bit = require('bit')

return function(windower, h)
    local unsupported, stub, partial = h.unsupported, h.stub, h.partial

    local function wev() return rawget(windower, "__events") end
    local function from_wev(name, ...)
        local e = wev()
        local f = e and e[name]
        if f then return f(...) end
        return nil
    end

    ---------------------------------------------------------------------------- paths

    local ids = {}
    local function P(struct, path)
        local k = struct .. '\0' .. path
        local id = ids[k]
        if id == nil then
            id = game.path(struct, path) or path
            ids[k] = id
        end
        return id
    end
    local function ent(i, path) return game.entity(i, P('entity_t', path)) end
    local function member(i, path) return game.party_member(i, P('partymember_t', path)) end
    local function player(path) return game.player(P('player_t', path)) end
    local function num(v) return type(v) == 'number' and v or 0 end
    local function cstr(v)
        if type(v) ~= 'string' then return nil end
        local z = v:find('\0', 1, true)
        return z and v:sub(1, z - 1) or v
    end

    ---------------------------------------------------------------------------- resources

    local res_cache = {}
    local function res(name)
        local r = res_cache[name]
        if r == nil then
            local ok, t = pcall(dofile, windower.windower_path .. 'res/' .. name .. '.lua')
            r = ok and type(t) == 'table' and t or false
            res_cache[name] = r
        end
        return r or nil
    end

    local JOBS = { [0] = 'NON', 'WAR', 'MNK', 'WHM', 'BLM', 'RDM', 'THF', 'PLD', 'DRK', 'BST', 'BRD', 'RNG',
        'SAM', 'NIN', 'DRG', 'SMN', 'BLU', 'COR', 'PUP', 'DNC', 'SCH', 'GEO', 'RUN', 'MON' }
    local function job(id)
        local r = res('jobs')
        local j = r and r[id]
        if j then return j.ens or j.english_short, j.en or j.english end
        return JOBS[id], JOBS[id]
    end

    ---------------------------------------------------------------------------- state

    local function player_index()
        local i = game.player_index()
        if i and i > 0 then return i end
        local t = member(0, 'TargetIndex')
        if t and t > 0 then return t end
        return nil
    end

    local function logged_in()
        local s = game.login_status()
        if s ~= nil then return s == 2 end
        return player_index() ~= nil
    end

    -- Vana'diel time from the clock (the client does the same): minutes since its epoch
    local function vana_minutes()
        return math.floor((os.time() - 1009810800) / 60 * 25) + 886 * 518400
    end

    -- the party's and alliance's server ids, read once a frame (every mob table wants them)
    local frame, party_cache = 0, {}
    xi.events.on('frame', function() frame = frame + 1 end, 'windower_ffxi.frame')

    local function party_ids(limit)
        local c = party_cache[limit]
        if c and c.frame == frame then return c.set end
        local set = {}
        for i = 0, limit - 1 do
            if num(member(i, 'IsActive')) ~= 0 then
                local id = member(i, 'ServerId')
                if id and id ~= 0 then set[id] = true end
            end
        end
        party_cache[limit] = { frame = frame, set = set }
        return set
    end

    ---------------------------------------------------------------------------- mobs

    local function mob(i)
        i = tonumber(i)
        if not i or i < 0 or not game.base('entity', i) then return nil end
        local id = ent(i, 'ServerId')
        if not id then return nil end
        local spawn = num(ent(i, 'SpawnFlags'))
        local render = num(ent(i, 'Render.Flags0'))
        local party, alliance = party_ids(6), party_ids(18)
        local lc = num(ent(i, 'LinkshellColor'))
        local m = {
            id = id,
            index = i,
            name = cstr(ent(i, 'Name')) or '',
            x = num(ent(i, 'Movement.LocalPosition.X')),
            y = num(ent(i, 'Movement.LocalPosition.Y')),
            z = num(ent(i, 'Movement.LocalPosition.Z')),
            facing = num(ent(i, 'Heading')),
            heading = num(ent(i, 'Heading')),
            distance = num(ent(i, 'Distance')), -- squared, as Windower's
            hpp = num(ent(i, 'HPPercent')),
            status = num(ent(i, 'Status')),
            race = num(ent(i, 'Race')),
            spawn_type = spawn,
            entity_type = num(ent(i, 'Type')),
            mob_type = num(ent(i, 'Type')),
            is_npc = bit.band(spawn, 0x1) == 0,
            valid_target = bit.band(render, 0x200) ~= 0,
            charmed = false,
            in_party = party[id] or false,
            in_alliance = alliance[id] or false,
            claim_id = num(ent(i, 'ClaimStatus')),
            target_index = num(ent(i, 'TargetedIndex')),
            pet_index = (function(p) return p and p ~= 0 and p or nil end)(ent(i, 'PetTargetIndex')),
            fellow_index = (function(p) return p and p ~= 0 and p or nil end)(ent(i, 'FellowTargetIndex')),
            model_size = num(ent(i, 'ModelSize')),
            model_scale = num(ent(i, 'ModelHitboxSize')),
            movement_speed = num(ent(i, 'MovementSpeed')),
            linkshell_color = { red = bit.band(lc, 0xFF), green = bit.band(bit.rshift(lc, 8), 0xFF), blue = bit.band(bit.rshift(lc, 16), 0xFF) },
            models = {
                num(ent(i, 'Look.Head')), num(ent(i, 'Look.Body')), num(ent(i, 'Look.Hands')), num(ent(i, 'Look.Legs')),
                num(ent(i, 'Look.Feet')), num(ent(i, 'Look.Main')), num(ent(i, 'Look.Sub')), num(ent(i, 'Look.Ranged')),
                [0] = num(ent(i, 'Look.Hair')),
            },
        }
        return m
    end

    local ffxi = {}

    ffxi.get_mob_by_index = mob

    function ffxi.get_mob_by_id(id)
        id = tonumber(id)
        if not id then return nil end
        local i = game.entity_by_server_id(id)
        return i and mob(i) or nil
    end

    local function target_index(sub)
        local s = num(game.target(P('target_t', 'IsSubTargetActive')))
        local slot = sub and 1 or s
        if sub and s == 0 then return nil end
        local i = game.target(P('target_t', ('Targets[%d].Index'):format(slot)))
        if not i or i == 0 then return nil end
        return i
    end

    function ffxi.get_mob_by_target(t)
        if t == nil then return nil end
        t = tostring(t):lower():gsub('^<(.*)>$', '%1')
        local pi = player_index()
        if t == 'me' then return pi and mob(pi) end
        if t == 't' then return (function(i) return i and mob(i) end)(target_index(false)) end
        if t == 'st' then return (function(i) return i and mob(i) end)(target_index(true)) end
        if t == 'lastst' then
            local i = game.target(P('target_t', 'LastTargetIndex'))
            return i and i ~= 0 and mob(i) or nil
        end
        if t == 'pet' then
            local i = pi and ent(pi, 'PetTargetIndex')
            return i and i ~= 0 and mob(i) or nil
        end
        if t == 'bt' then
            -- the battle target: a monster claimed by the party
            local party = party_ids(18)
            for i = 0, math.min(game.entity_count() or 0, 0x400) - 1 do
                local c = ent(i, 'ClaimStatus')
                if c and c ~= 0 and party[c] and num(ent(i, 'HPPercent')) > 0 then return mob(i) end
            end
            return nil
        end
        local p = t:match('^p(%d)$')
        local a1, a2 = t:match('^a([12])(%d)$')
        local slot = p and tonumber(p) or a1 and (tonumber(a1) * 6 + tonumber(a2)) or nil
        if slot then
            if num(member(slot, 'IsActive')) == 0 then return nil end
            local i = member(slot, 'TargetIndex')
            return i and i ~= 0 and mob(i) or nil
        end
        return nil
    end

    function ffxi.get_mob_by_name(name)
        if name == nil then return nil end
        name = tostring(name):lower()
        for i = 0, (game.entity_count() or 0) - 1 do
            local n = game.base('entity', i) and cstr(ent(i, 'Name'))
            if n and n:lower() == name then return mob(i) end
        end
        return nil
    end

    function ffxi.get_mob_array()
        local t = {}
        for i = 0, (game.entity_count() or 0) - 1 do
            if game.base('entity', i) then t[i] = mob(i) end
        end
        return t
    end

    function ffxi.get_mob_list(name)
        local t = {}
        name = name and tostring(name):lower()
        for i = 0, (game.entity_count() or 0) - 1 do
            local n = game.base('entity', i) and cstr(ent(i, 'Name'))
            if n and n ~= '' and (not name or n:lower() == name) then t[i] = n end
        end
        return t
    end

    ---------------------------------------------------------------------------- player

    local SKILLS = {
        { 'HandToHand', 'hand_to_hand' }, { 'Dagger', 'dagger' }, { 'Sword', 'sword' }, { 'GreatSword', 'great_sword' },
        { 'Axe', 'axe' }, { 'GreatAxe', 'great_axe' }, { 'Scythe', 'scythe' }, { 'Polearm', 'polearm' },
        { 'Katana', 'katana' }, { 'GreatKatana', 'great_katana' }, { 'Club', 'club' }, { 'Staff', 'staff' },
        { 'AutomatonMelee', 'automaton_melee' }, { 'AutomatonRanged', 'automaton_archery' },
        { 'AutomatonMagic', 'automaton_magic' }, { 'Archery', 'archery' }, { 'Marksmanship', 'marksmanship' },
        { 'Throwing', 'throwing' }, { 'Guarding', 'guard' }, { 'Evasion', 'evasion' }, { 'Shield', 'shield' },
        { 'Parrying', 'parrying' }, { 'Divine', 'divine_magic' }, { 'Healing', 'healing_magic' },
        { 'Enhancing', 'enhancing_magic' }, { 'Enfeebling', 'enfeebling_magic' }, { 'Elemental', 'elemental_magic' },
        { 'Dark', 'dark_magic' }, { 'Summon', 'summoning_magic' }, { 'Ninjutsu', 'ninjutsu' }, { 'Singing', 'singing' },
        { 'String', 'stringed_instrument' }, { 'Wind', 'wind_instrument' }, { 'BlueMagic', 'blue_magic' },
        { 'Geomancy', 'geomancy' }, { 'Handbell', 'handbell' },
    }
    local CRAFTS = {
        { 'Fishing', 'fishing' }, { 'Woodworking', 'woodworking' }, { 'Smithing', 'smithing' },
        { 'Goldsmithing', 'goldsmithing' }, { 'Clothcraft', 'clothcraft' }, { 'Leathercraft', 'leathercraft' },
        { 'Bonecraft', 'bonecraft' }, { 'Alchemy', 'alchemy' }, { 'Cooking', 'cooking' }, { 'Synergy', 'synergy' },
    }

    function ffxi.get_player()
        if not logged_in() then
            -- memory has no player (the harness, a moment at login): the packets' player, if any
            local info = from_wev('get_info')
            if not (type(info) == 'table' and info.logged_in) then return nil end
            local p = from_wev('get_player_extra')
            if type(p) ~= 'table' then return nil end
            p.name = p.name or ''
            p.main_job_id = p.main_job_id or 0
            p.main_job, p.main_job_full = job(p.main_job_id)
            p.main_job_level = p.main_job_level or 0
            if p.sub_job_id then p.sub_job, p.sub_job_full = job(p.sub_job_id) end
            p.status = p.status or 0
            p.in_combat = p.status == 1
            p.buffs, p.skills, p.jobs = p.buffs or {}, p.skills or {}, p.jobs or {}
            p.merits, p.job_points = p.merits or {}, p.job_points or {}
            p.vitals = p.vitals or { hp = 0, mp = 0, tp = 0, hpp = 0, mpp = 0, max_hp = 0, max_mp = 0 }
            return p
        end
        local pi = player_index()
        local main, sub = num(player('MainJob')), num(player('SubJob'))
        local main_s, main_f = job(main)
        local p = {
            name = (pi and cstr(ent(pi, 'Name'))) or cstr(member(0, 'Name')) or '',
            id = (pi and ent(pi, 'ServerId')) or member(0, 'ServerId') or 0,
            index = pi or 0,
            main_job_id = main, main_job = main_s, main_job_full = main_f,
            main_job_level = num(player('MainJobLevel')),
            status = pi and num(ent(pi, 'Status')) or 0,
            nation = num(player('Nation')),
            item_level = num(player('ItemLevel')),
            superior_level = num(player('SuLevel')),
            title = num(player('Title')),
            vitals = {
                hp = num(member(0, 'HP')), mp = num(member(0, 'MP')), tp = num(member(0, 'TP')),
                hpp = num(member(0, 'HPPercent')), mpp = num(member(0, 'MPPercent')),
                max_hp = num(player('HPMax')), max_mp = num(player('MPMax')),
            },
            buffs = {},
            jobs = {},
            skills = {},
            linkshell = '',
            linkshell_slot = 0,
        }
        p.in_combat = p.status == 1
        if sub > 0 then
            p.sub_job_id = sub
            p.sub_job, p.sub_job_full = job(sub)
            p.sub_job_level = num(player('SubJobLevel'))
        end
        for id = 1, 22 do
            p.jobs[JOBS[id]] = game.job_level(id) or 0
        end
        local buffs = game.player('Buffs')
        if type(buffs) == 'table' then
            for _, b in ipairs(buffs) do
                if b >= 0 and b ~= 255 and b ~= 0xFFFF then p.buffs[#p.buffs + 1] = b end
            end
        end
        for _, s in ipairs(SKILLS) do
            p.skills[s[2]] = bit.band(num(player('CombatSkills.' .. s[1] .. '.Raw')), 0x7FFF)
        end
        for _, s in ipairs(CRAFTS) do
            p.skills[s[2]] = bit.rshift(bit.band(num(player('CraftSkills.' .. s[1] .. '.Raw')), 0x1FE0), 5)
        end
        local t = target_index(false)
        p.target_index = t
        p.target_locked = num(game.target(P('target_t', 'LockedOnFlags'))) ~= 0
        local f = game.autofollow(P('autofollow_t', 'FollowTargetIndex'))
        p.follow_index = f and f ~= 0 and f or nil
        p.autorun = num(game.autofollow(P('autofollow_t', 'IsAutoRunning'))) ~= 0
        -- what packets know better: merits, job points, (linkshell)
        local extra = from_wev('get_player_extra')
        if type(extra) == 'table' then
            for k, v in pairs(extra) do
                if p[k] == nil or k == 'merits' or k == 'job_points' or k == 'linkshell' or k == 'linkshell_slot' then
                    p[k] = v
                end
            end
        end
        p.merits = p.merits or {}
        p.job_points = p.job_points or {}
        return p
    end

    function ffxi.get_info()
        local t = vana_minutes()
        local day_count = math.floor(t / 1440)
        local daysmod = (day_count + 26) % 84
        local moon = daysmod >= 42 and math.floor(100 * (daysmod - 42) / 42 + 0.5) or math.floor(100 * (1 - daysmod / 42) + 0.5)
        local li = logged_in()
        local i = {
            logged_in = li,
            language = 'English',
            time = t % 1440,
            day = math.floor((t % 11520) / 1440),
            moon = moon,
            moon_phase = math.floor(((daysmod + 45) % 84) / 7),
            weather = 0,
            zone = li and num(member(0, 'Zone')) or 0,
            mog_house = false,
            menu_open = false,
        }
        if li then
            local _, open = xi.chat.input()
            i.chat_open = open and true or false
            local tx = game.target(P('target_t', 'TargetPosF'))
            if type(tx) == 'table' and target_index(false) then
                i.target_arrow = { x = tx[1] or 0, y = tx[3] or 0, z = tx[2] or 0 }
            end
        end
        local extra = from_wev('get_info') or from_wev('get_info_extra')
        if type(extra) == 'table' then
            for k, v in pairs(extra) do i[k] = v end
        end
        return i
    end

    function ffxi.get_party_info()
        local r = {}
        for n = 1, 3 do
            r['party' .. n .. '_count'] = num(game.alliance(P('allianceinfo_t', 'PartyMemberCount' .. n)))
            r['party' .. n .. '_leader'] = game.alliance(P('allianceinfo_t', 'PartyLeaderServerId' .. n))
        end
        r.alliance_leader = game.alliance(P('allianceinfo_t', 'AllianceLeaderServerId'))
        return r
    end

    function ffxi.get_party()
        local party = ffxi.get_party_info()
        if not game.base('party') or num(member(0, 'IsActive')) == 0 then
            -- no party in memory: the player alone, from the packets
            local me = ffxi.get_player()
            if me then
                party.party1_count = 1
                party.p0 = {
                    name = me.name, id = me.id, zone = ffxi.get_info().zone,
                    hp = me.vitals.hp, mp = me.vitals.mp, tp = me.vitals.tp, hpp = me.vitals.hpp, mpp = me.vitals.mpp,
                    mob = me.index and mob(me.index) or nil,
                }
            end
            return party
        end
        for slot = 0, 17 do
            if num(member(slot, 'IsActive')) ~= 0 then
                local key = slot < 6 and ('p' .. slot) or ('a' .. (math.floor(slot / 6)) .. (slot % 6))
                local ti = member(slot, 'TargetIndex')
                party[key] = {
                    name = cstr(member(slot, 'Name')) or '',
                    id = member(slot, 'ServerId'),
                    hp = num(member(slot, 'HP')), mp = num(member(slot, 'MP')), tp = num(member(slot, 'TP')),
                    hpp = num(member(slot, 'HPPercent')), mpp = num(member(slot, 'MPPercent')),
                    zone = num(member(slot, 'Zone')),
                    main_job = num(member(slot, 'MainJob')), main_job_level = num(member(slot, 'MainJobLevel')),
                    sub_job = num(member(slot, 'SubJob')), sub_job_level = num(member(slot, 'SubJobLevel')),
                    mob = ti and ti ~= 0 and mob(ti) or nil,
                }
            end
        end
        return party
    end

    ---------------------------------------------------------------------------- items

    local BAGS = { [0] = 'inventory', 'safe', 'storage', 'temporary', 'locker', 'satchel', 'sack', 'case', 'wardrobe',
        'safe2', 'wardrobe2', 'wardrobe3', 'wardrobe4', 'wardrobe5', 'wardrobe6', 'wardrobe7', 'wardrobe8', 'recycle' }
    local BAG_IDS = {}
    for id = 0, #BAGS do BAG_IDS[BAGS[id]] = id end
    BAG_IDS.bank = 1
    BAG_IDS.bank2 = 9

    local SLOTS = { [0] = 'main', 'sub', 'range', 'ammo', 'head', 'body', 'hands', 'legs', 'feet', 'neck', 'waist',
        'left_ear', 'right_ear', 'left_ring', 'right_ring', 'back' }

    local function bag_id(b)
        if type(b) == 'number' then return b end
        if type(b) == 'string' then return BAG_IDS[b:lower()] end
        return nil
    end

    local function mem_item(bag, slot)
        local base = game.base('item', bag, slot)
        if not base then return nil end
        local extra = game.item(bag, slot, 'Extra') or ''
        return {
            id = num(game.item(bag, slot, 'Id')),
            count = num(game.item(bag, slot, 'Count')),
            status = num(game.item(bag, slot, 'Flags')),
            bazaar = num(game.item(bag, slot, 'Price')),
            extdata = type(extra) == 'string' and extra:sub(1, 24) or ('\0'):rep(24),
            slot = slot,
        }
    end

    local function mem_bag(bag)
        if not game.base('inventory') then return nil end
        local max = game.container_max(bag) or 0
        local t = { max = max > 0 and max - 1 or 0, count = game.container_count(bag) or 0, enabled = max > 0 }
        for slot = 1, 80 do
            t[slot] = mem_item(bag, slot) or { id = 0, count = 0, status = 0, bazaar = 0, extdata = ('\0'):rep(24), slot = slot }
        end
        return t
    end

    local function equipment()
        local e = {}
        for s = 0, 15 do
            local _, index = game.equipment(s)
            index = index or 0
            e[SLOTS[s]] = bit.band(index, 0xFF)
            e[SLOTS[s] .. '_bag'] = bit.rshift(bit.band(index, 0xFF00), 8)
        end
        return e
    end

    local function treasure()
        local t = {}
        for i = 0, 9 do
            local id = game.treasure(i, P('treasureitem_t', 'ItemId'))
            if id and id ~= 0 then
                t[i] = {
                    item_id = id, count = num(game.treasure(i, P('treasureitem_t', 'Count'))),
                    timestamp = num(game.treasure(i, P('treasureitem_t', 'DropTime'))),
                    lot = num(game.treasure(i, P('treasureitem_t', 'Lot'))),
                    winning_lot = num(game.treasure(i, P('treasureitem_t', 'WinningLot'))),
                    winning_name = cstr(game.treasure(i, P('treasureitem_t', 'WinningEntityName'))),
                }
            end
        end
        return t
    end

    local function empty_bag()
        local t = { max = 0, count = 0, enabled = false }
        for slot = 1, 80 do t[slot] = { id = 0, count = 0, status = 0, bazaar = 0, extdata = ('\0'):rep(24), slot = slot } end
        return t
    end

    -- before login (nothing in memory, no packets yet): Windower's shapes, empty
    local function empty_items()
        local all = { equipment = {}, treasure = {}, gil = 0 }
        for s = 0, 15 do
            all.equipment[SLOTS[s]] = 0
            all.equipment[SLOTS[s] .. '_bag'] = 0
        end
        for id = 0, #BAGS do
            all[BAGS[id]] = empty_bag()
            all['max_' .. BAGS[id]], all['count_' .. BAGS[id]], all['enabled_' .. BAGS[id]] = 0, 0, false
        end
        return all
    end

    function ffxi.get_items(bag, index)
        local mem = logged_in() and game.base('inventory') ~= nil
        if not mem then
            local w = from_wev('get_items', bag, index)
            if w ~= nil or index ~= nil then return w end
            if bag == nil then return empty_items() end
            if bag == 'gil' then return 0 end
            if bag == 'equipment' or bag == 'treasure' then return empty_items()[bag] end
            return bag_id(bag) and empty_bag() or nil
        end
        if bag == nil then
            local all = { equipment = equipment(), treasure = treasure(), gil = num(game.item(0, 0, 'Count')) }
            for id = 0, #BAGS do
                local b = mem_bag(id)
                if b then
                    all[BAGS[id]] = b
                    all['max_' .. BAGS[id]] = b.max
                    all['count_' .. BAGS[id]] = b.count
                    all['enabled_' .. BAGS[id]] = b.enabled
                end
            end
            return all
        end
        if bag == 'equipment' then return equipment() end
        if bag == 'treasure' then return treasure() end
        if bag == 'gil' then return num(game.item(0, 0, 'Count')) end
        local id = bag_id(bag)
        if id == nil then return nil end
        if index ~= nil then
            return mem_item(id, tonumber(index) or 0)
        end
        return mem_bag(id)
    end

    function ffxi.get_bag_info(bag)
        if bag == nil then
            local r = {}
            for id = 0, #BAGS do r[BAGS[id]] = ffxi.get_bag_info(id) end
            return r
        end
        local id = bag_id(bag)
        if id == nil then return nil end
        if not (logged_in() and game.base('inventory')) then
            local b = from_wev('get_items', id)
            if type(b) == 'table' then return { max = b.max or 0, count = b.count or 0, enabled = b.enabled or false } end
            return from_wev('get_bag_info', id) or { max = 0, count = 0, enabled = false }
        end
        local max = game.container_max(id) or 0
        return { max = max > 0 and max - 1 or 0, count = game.container_count(id) or 0, enabled = max > 0 }
    end

    function ffxi.get_key_items()
        local w = from_wev('get_key_items')
        if w then return w end
        local t = {}
        if game.key_item(1) == nil then return t end
        for id = 0, 4095 do
            if game.key_item(id) then t[#t + 1] = id end
        end
        return t
    end

    function ffxi.get_spells()
        if game.has_spell_data() then
            local t = {}
            for id = 0, 1023 do t[id] = game.spell_known(id) and true or false end
            return t
        end
        return from_wev('get_spells') or {}
    end

    function ffxi.get_abilities()
        return from_wev('get_abilities') or { job_abilities = {}, weapon_skills = {}, job_traits = {}, pet_commands = {} }
    end

    local zero = { __index = function(_, k) if type(k) == 'number' then return 0 end end }

    function ffxi.get_ability_recasts()
        if game.base('recast_abilities') then
            local t = setmetatable({}, zero)
            for i = 0, 30 do
                local timer, id = game.ability_recast(i)
                if timer and timer > 0 and id then t[id] = math.max(t[id] or 0, timer / 60) end
            end
            return t
        end
        local w = from_wev('get_ability_recasts')
        return setmetatable(w or {}, zero)
    end

    function ffxi.get_spell_recasts()
        if game.base('recast_spells') then
            local t = setmetatable({}, zero)
            for id = 0, 1023 do t[id] = game.spell_recast(id) or 0 end
            return t
        end
        local w = from_wev('get_spell_recasts')
        return setmetatable(w or {}, zero)
    end

    function ffxi.get_mjob_data() return from_wev('get_mjob_data') or {} end
    function ffxi.get_sjob_data() return from_wev('get_sjob_data') or {} end

    ---------------------------------------------------------------------------- actions

    local function out(id, payload)
        local p = string.char(bit.band(id, 0xFF), bit.band(bit.rshift(id, 8), 1), 0, 0) .. payload
        if #p % 4 ~= 0 then p = p .. ('\0'):rep(4 - #p % 4) end
        xi.packets.inject(true, p)
    end
    local u8, u16, u32 = function(v) return string.char(bit.band(tonumber(v) or 0, 0xFF)) end,
        function(v) return string.pack('H', tonumber(v) or 0) end,
        function(v) return string.pack('I', tonumber(v) or 0) end

    -- the equip packet (0x050): inventory index, equipment slot, bag
    function ffxi.set_equip(index, slot, bag)
        if type(slot) == 'string' then
            for id, n in pairs(SLOTS) do if n == slot then slot = id end end
        end
        out(0x050, u8(index) .. u8(slot) .. u8(bag_id(bag) or 0) .. u8(0))
    end

    function ffxi.cancel_buff(id) out(0x0F1, u16(id) .. u16(0)) end
    function ffxi.lot_item(slot) out(0x041, u8(slot) .. u8(0) .. u16(0)) end
    function ffxi.pass_item(slot) out(0x042, u8(slot) .. u8(0) .. u16(0)) end
    function ffxi.stack_items(bag) out(0x03A, u8(bag_id(bag) or 0) .. u8(0) .. u16(0)) end

    -- item moves (0x029): count, from bag, to bag, from index, to index (0x52: the first free)
    local function move(count, from_bag, to_bag, index, to_index)
        out(0x029, u32(count or 1) .. u8(from_bag) .. u8(to_bag) .. u8(index) .. u8(to_index or 0x52))
    end
    function ffxi.get_item(bag, index, count)
        move(count or (ffxi.get_items(bag, index) or {}).count, bag_id(bag), 0, index)
    end
    function ffxi.put_item(bag, index, count)
        move(count or (ffxi.get_items(0, index) or {}).count, 0, bag_id(bag), index)
    end
    function ffxi.move_item(from_bag, index, count, to_bag, to_index)
        move(count, bag_id(from_bag), bag_id(to_bag) or 0, index, to_index)
    end

    -- Movement goes through the game's auto-run (the auto-follow object's direction vector).
    function ffxi.run(dir, y, z)
        local af = game.base('autofollow')
        if not af then return end
        if dir == false then
            game.set_autofollow(P('autofollow_t', 'IsAutoRunning'), 0)
            return
        end
        local dx, dy
        if type(dir) == 'number' and y ~= nil then
            dx, dy = dir, y
        elseif type(dir) == 'number' then
            dx, dy = math.cos(dir), -math.sin(dir)
        else
            local pi = player_index()
            local hd = pi and num(ent(pi, 'Heading')) or 0
            dx, dy = math.cos(hd), -math.sin(hd)
        end
        game.set_autofollow(P('autofollow_t', 'FollowDeltaX'), dx)
        game.set_autofollow(P('autofollow_t', 'FollowDeltaY'), dy)
        game.set_autofollow(P('autofollow_t', 'FollowDeltaZ'), 0)
        game.set_autofollow(P('autofollow_t', 'IsAutoRunning'), 1)
    end

    function ffxi.turn(heading)
        local pi = player_index()
        if pi then game.set_entity(pi, P('entity_t', 'Heading'), tonumber(heading) or 0) end
    end

    function ffxi.follow(index)
        if index == nil then
            game.set_autofollow(P('autofollow_t', 'FollowTargetIndex'), 0)
            game.set_autofollow(P('autofollow_t', 'FollowTargetServerId'), 0)
            return
        end
        local id = ent(index, 'ServerId')
        if not id then return end
        game.set_autofollow(P('autofollow_t', 'FollowTargetIndex'), index)
        game.set_autofollow(P('autofollow_t', 'FollowTargetServerId'), id)
    end

    ffxi.set_blue_magic_spell = stub('windower.ffxi.set_blue_magic_spell')
    ffxi.remove_blue_magic_spell = stub('windower.ffxi.remove_blue_magic_spell')
    ffxi.reset_blue_magic_spells = stub('windower.ffxi.reset_blue_magic_spells')
    ffxi.set_attachment = stub('windower.ffxi.set_attachment')
    ffxi.remove_attachment = stub('windower.ffxi.remove_attachment')
    ffxi.reset_attachments = stub('windower.ffxi.reset_attachments')
    ffxi.set_lockstyle = stub('windower.ffxi.set_lockstyle')

    windower.ffxi = partial(ffxi, 'windower.ffxi')

    ---------------------------------------------------------------------------- packets

    local packets = {}

    local function with_id(id, data)
        data = tostring(data or '')
        if #data < 4 then data = data .. ('\0'):rep(4 - #data) end
        local h = bit.bor(bit.band(data:byte(1) + data:byte(2) * 256, 0xFE00), bit.band(tonumber(id) or 0, 0x1FF))
        return string.char(bit.band(h, 0xFF), bit.rshift(h, 8)) .. data:sub(3)
    end

    function packets.inject_incoming(id, data) xi.packets.inject(false, with_id(id, data)) end
    function packets.inject_outgoing(id, data) xi.packets.inject(true, with_id(id, data)) end
    function packets.last_incoming(id) return xi.packets.last(false, tonumber(id) or 0) end
    function packets.last_outgoing(id) return xi.packets.last(true, tonumber(id) or 0) end

    function packets.parse_action(data)
        local e = wev()
        if e and e.parse_action then return e.parse_action(data) end
        unsupported('windower.packets.parse_action (needs windower_events)')
    end

    windower.packets = partial(packets, 'windower.packets')
end
