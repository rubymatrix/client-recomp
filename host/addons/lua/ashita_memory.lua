--[[
Ashita v4's memory manager (AshitaCore:GetMemoryManager()) over xi.game's primitives.

    local game = ...            -- xi.game (host/addons/game_lua.c)
    return MemoryManager        -- :GetEntity() :GetParty() :GetPlayer() :GetTarget() :GetInventory()
                                -- :GetAutoFollow() :GetCastBar() :GetRecast()

Every method in Ashita's annotations (addons/libs/annotations/SDK/Memory/*.lua) exists here. Most
are generated at load from the struct descriptors (game.struct_info) by naming rules, the rest
are written out below. Reads go to guest memory on every call; missing structures (before
login, while zoning, no target) read as 0 for numbers and nil for strings and structs, like
Ashita's C++ returning 0 / nullptr.

Structs come back as live proxies: fields read and write guest memory on access
(`GetEntity(i).StatusServer = 8` works), nested structs are proxies, arrays are 1-based.

Also exported for the Ashita layer: MemoryManager.globals.GetEntity(index) / .GetPlayerEntity()
(Ashita's globals), MemoryManager.proxy(struct, guest_addr), MemoryManager.interfaces.
--]]

local game = ...
local bit = require('bit')

local type, rawget, setmetatable, tostring = type, rawget, setmetatable, tostring

------------------------------------------------------------------------------------------------
-- descriptors
------------------------------------------------------------------------------------------------

local infos = {}

-- struct_info with a by-name index and a path id per field
local function info(sname)
    local inf = infos[sname]
    if inf then return inf end
    inf = game.struct_info(sname)
    if not inf then error('ashita_memory: unknown struct ' .. tostring(sname), 2) end
    inf.byname = {}
    for _, f in ipairs(inf.fields) do
        f.id = game.path(sname, f.name) or f.name
        inf.byname[f.name] = f
    end
    infos[sname] = inf
    return inf
end

local ids = {}

-- a cached path id (or the path itself when it can't have one)
local function pid(sname, path)
    local k = sname .. '\0' .. path
    local id = ids[k]
    if id == nil then
        id = game.path(sname, path) or path
        ids[k] = id
    end
    return id
end

local function field(sname, name)
    return info(sname).byname[name]
end

-- the guest address of element i (0-based) of array field `name` in struct `sname` at `base`
local function elem(sname, base, name, i)
    if base == nil then return nil end
    local f = field(sname, name)
    i = tonumber(i) or -1
    if i < 0 or i >= f.count then return nil end
    return base + f.offset + i * f.stride, f.struct
end

------------------------------------------------------------------------------------------------
-- proxies
------------------------------------------------------------------------------------------------

local proxy
local mts = {}

local function array_proxy(sname, f, base)
    local a = base + f.offset
    local t = {}
    return setmetatable(t, {
        __index = function(_, k)
            if type(k) ~= 'number' or k < 1 or k > f.count then return nil end
            if f.type == 'struct' then
                return proxy(f.struct, a + (k - 1) * f.stride)
            end
            return game.read(sname, base, f.id, k - 1)
        end,
        __newindex = function(_, k, v)
            if type(k) == 'number' and k >= 1 and k <= f.count and f.type ~= 'struct' then
                game.write(sname, base, f.id, v, k - 1)
            end
        end,
        __len = function() return f.count end,
    })
end

local function mt_for(sname)
    local mt = mts[sname]
    if mt then return mt end
    local inf = info(sname)
    local byname = inf.byname
    mt = {
        __index = function(t, k)
            local f = byname[k]
            if f == nil then return nil end
            local a = rawget(t, '__addr')
            if f.type == 'struct' then
                if f.count == 1 then return proxy(f.struct, a + f.offset) end
                return array_proxy(sname, f, a)
            end
            if f.count > 1 and f.type ~= 'chars' and f.type ~= 'bytes' then
                return array_proxy(sname, f, a)
            end
            return game.read(sname, a, f.id)
        end,
        __newindex = function(t, k, v)
            local f = byname[k]
            if f ~= nil and f.type ~= 'struct' then
                game.write(sname, rawget(t, '__addr'), f.id, v)
            end
        end,
        __tostring = function(t) return ('%s @ 0x%08X'):format(sname, rawget(t, '__addr')) end,
    }
    mts[sname] = mt
    return mt
end

proxy = function(sname, addr)
    if addr == nil or addr == 0 then return nil end
    return setmetatable({ __addr = addr, __struct = sname }, mt_for(sname))
end

------------------------------------------------------------------------------------------------
-- helpers for generated methods
------------------------------------------------------------------------------------------------

local function num(v) if type(v) == 'number' then return v end return 0 end
local function str(v) if type(v) == 'string' then return v end return nil end

-- Ashita strings are C strings: stop at the first NUL (uint8_t "name" arrays read as raw bytes)
local function cstr(v)
    if type(v) ~= 'string' then return nil end
    local z = v:find('\0', 1, true)
    return z and v:sub(1, z - 1) or v
end

local function is_text(f) return f.type == 'chars' end
local function is_scalar(f) return f.count == 1 and f.type ~= 'struct' end

-- walk nested non-array structs: fn(path, f)
local function each_path(sname, fn, prefix)
    for _, f in ipairs(info(sname).fields) do
        local path = prefix and (prefix .. '.' .. f.name) or f.name
        if f.type == 'struct' and f.count == 1 then
            each_path(f.struct, fn, path)
        else
            fn(path, f)
        end
    end
end

local function singular(name)
    if name:sub(-1) == 's' then return name:sub(1, -2) end
    return name
end

-- adds Get<name>/Set<name> to obj over `get(fieldid, sub)` / `set(fieldid, value, sub)` with
-- an optional leading index (entity index, party member): indexed = true
local function add_accessors(obj, sname, method, path, f, indexed, get, set, opts)
    opts = opts or {}
    if f.name:find('^unknown') or f.name:find('^padding') then return end
    local id = pid(sname, path)
    local getname, setname = 'Get' .. method, 'Set' .. method
    local text = is_text(f) or opts.text
    local array = f.count > 1 and not (f.type == 'chars' or f.type == 'bytes')
    if f.type == 'bytes' and not opts.addr then
        -- raw bytes as a string
        if indexed then
            obj[getname] = obj[getname] or function(_, i) return str(get(i, id)) end
            obj[setname] = obj[setname] or function(_, i, v) set(i, id, v) end
        else
            obj[getname] = obj[getname] or function() return str(get(id)) end
            obj[setname] = obj[setname] or function(_, v) set(id, v) end
        end
        return
    end
    if f.type == 'bytes' and not opts.text then
        -- uint8_t* in C++: Lua gets the host address of the bytes (annotated as a number)
        if indexed then
            obj[getname] = obj[getname] or function(_, i) return opts.addr(i, path) end
        else
            obj[getname] = obj[getname] or function() return opts.addr(path) end
        end
        return
    end
    if indexed then
        if array then
            obj[getname] = obj[getname] or function(_, i, sub) return num(get(i, id, sub or 0)) end
            obj[setname] = obj[setname] or function(_, i, sub, v) set(i, id, v, sub or 0) end
        elseif text then
            obj[getname] = obj[getname] or function(_, i) return cstr(get(i, id)) end
            obj[setname] = obj[setname] or function(_, i, v) set(i, id, v) end
        else
            obj[getname] = obj[getname] or function(_, i) return num(get(i, id)) end
            obj[setname] = obj[setname] or function(_, i, v) set(i, id, v) end
        end
    else
        if array then
            obj[getname] = obj[getname] or function(_, sub) return num(get(id, sub or 0)) end
            obj[setname] = obj[setname] or function(_, sub, v) set(id, v, sub or 0) end
        elseif text then
            obj[getname] = obj[getname] or function() return cstr(get(id)) end
            obj[setname] = obj[setname] or function(_, v) set(id, v) end
        else
            obj[getname] = obj[getname] or function() return num(get(id)) end
            obj[setname] = obj[setname] or function(_, v) set(id, v) end
        end
    end
end

local function table_of(v, n)
    if type(v) == 'table' then return v end
    local t = {}
    for i = 1, n do t[i] = 0 end
    return t
end

------------------------------------------------------------------------------------------------
-- IEntity
------------------------------------------------------------------------------------------------

local Entity = {}

do
    local function eget(i, id, sub) return game.entity(i, id, sub) end
    local function eset(i, id, v, sub) return game.set_entity(i, id, v, sub) end
    local function eaddr(i, path)
        local base = game.base('entity', i)
        if not base then return 0 end
        local f = field('entity_t', path)
        return game.host_addr(base + f.offset)
    end
    each_path('entity_t', function(path, f)
        -- Movement.LocalPosition.X -> LocalPositionX, Movement.Move.DeltaX -> MoveDeltaX,
        -- Look.Hair -> LookHair, Render.Flags0 -> RenderFlags0; arrays drop their plural 's'
        local m = path:gsub('^Movement%.', ''):gsub('%.', '')
        if f.count > 1 and f.type ~= 'chars' and f.type ~= 'bytes' then m = singular(m) end
        add_accessors(Entity, 'entity_t', m, path, f, true, eget, eset, { addr = eaddr })
    end)
end

function Entity:GetRawEntity(index)
    return proxy('entity_t', game.base('entity', index))
end

function Entity:GetEntityMapSize()
    return game.entity_count()
end

-- C++ takes a pointer to 44 / 36 bytes; from Lua take a string or a table of bytes
function Entity:SetCustomProperties(index, v) game.set_entity(index, 'CustomProperties', v) end
function Entity:SetBallistaInfo(index, v) game.set_entity(index, 'BallistaInfo', v) end

local function GetEntity(index)
    index = tonumber(index)
    if index == nil then return nil end
    return proxy('entity_t', game.base('entity', index))
end

local function GetPlayerEntity()
    local i = game.player_index() or game.party_member(0, 'TargetIndex')
    if i == nil or i == 0 then return nil end
    return GetEntity(i)
end

------------------------------------------------------------------------------------------------
-- IParty
------------------------------------------------------------------------------------------------

local Party = {}

do
    local function mget(i, id, sub) return game.party_member(i, id, sub) end
    local function mset(i, id, v, sub) return game.set_party_member(i, id, v, sub) end
    each_path('partymember_t', function(path, f)
        if path == 'AllianceInfo' then return end
        local m = path
        if m:sub(1, 6) ~= 'Member' then m = 'Member' .. m end
        if f.count > 1 and f.type ~= 'chars' and f.type ~= 'bytes' then m = singular(m) end
        add_accessors(Party, 'partymember_t', m, path, f, true, mget, mset, { text = f.type == 'bytes' })
    end)
    local function aget(id) return game.alliance(id) end
    local function aset(id, v) return game.set_alliance(id, v) end
    each_path('allianceinfo_t', function(path, f)
        local m = path
        if m:sub(1, 8) ~= 'Alliance' then m = 'Alliance' .. m end
        -- PartyLeaderServerId1 -> AlliancePartyLeaderServerId1
        add_accessors(Party, 'allianceinfo_t', m, path, f, false, aget, aset)
    end)
end

function Party:GetRawStructure() return proxy('party_t', game.base('party')) end
function Party:GetRawStructureStatusIcons() return proxy('partystatusicons_t', game.base('party_icons')) end
function Party:GetStatusIconsServerId(i) return num(game.party_icons_field(i, 'ServerId')) end
function Party:GetStatusIconsTargetIndex(i) return num(game.party_icons_field(i, 'TargetIndex')) end
function Party:GetStatusIconsBitMask(i) return num(game.party_icons_field(i, 'BitMask')) end -- u64 as a double

function Party:GetStatusIcons(i)
    return game.party_icons(i) or table_of(nil, 32)
end

------------------------------------------------------------------------------------------------
-- IPlayer
------------------------------------------------------------------------------------------------

local Player = {}

do
    local function pget(id, sub) return game.player(id, sub) end
    local function pset(id, v, sub) return game.set_player(id, v, sub) end
    for _, f in ipairs(info('player_t').fields) do
        if is_scalar(f) and not f.name:find('^unknown') and not f.name:find('^padding') then
            add_accessors(Player, 'player_t', f.name, f.name, f, false, pget, pset)
        end
    end
end

-- bitfield flags are booleans in Ashita
for _, name in ipairs({ 'IsLimitBreaker', 'IsExperiencePointsLocked', 'IsLimitModeEnabled' }) do
    local id = pid('player_t', name)
    Player['Get' .. name] = function() return num(game.player(id)) ~= 0 end
end

local function sub_names(sname)
    local t = {}
    for i, f in ipairs(info(sname).fields) do t[i] = f.name end
    return t
end

local stat_names, resist_names = sub_names('playerstats_t'), sub_names('playerresists_t')
local combat_names, craft_names = sub_names('combatskills_t'), sub_names('craftskills_t')

local function named_getter(prefix, names)
    return function(_, i)
        local n = names[(tonumber(i) or -1) + 1]
        if n == nil then return 0 end
        return num(game.player(pid('player_t', prefix .. n)))
    end
end

Player.GetStat = named_getter('Stats.', stat_names)
Player.GetStatModifier = named_getter('StatsModifiers.', stat_names)
Player.GetResist = named_getter('Resists.', resist_names)

local combatskill = {}
combatskill.__index = combatskill
function combatskill:GetSkill() return bit.band(self.Raw, 0x7FFF) end
function combatskill:IsCapped() return bit.band(self.Raw, 0x8000) ~= 0 end

local craftskill = {}
craftskill.__index = craftskill
function craftskill:GetSkill() return bit.rshift(bit.band(self.Raw, 0x1FE0), 5) end
function craftskill:GetRank() return bit.band(self.Raw, 0x1F) end
function craftskill:IsCapped() return bit.band(self.Raw, 0x8000) ~= 0 end

function Player:GetCombatSkill(i)
    local n = combat_names[(tonumber(i) or -1) + 1]
    local raw = n and num(game.player(pid('player_t', 'CombatSkills.' .. n .. '.Raw'))) or 0
    return setmetatable({ Raw = raw }, combatskill)
end

function Player:GetCraftSkill(i)
    local n = craft_names[(tonumber(i) or -1) + 1]
    local raw = n and num(game.player(pid('player_t', 'CraftSkills.' .. n .. '.Raw'))) or 0
    return setmetatable({ Raw = raw }, craftskill)
end

local function ability_info(i, name)
    local a = elem('player_t', game.base('player'), 'AbilityInfo', i)
    if not a then return 0 end
    return num(game.read('abilityrecast_t', a, pid('abilityrecast_t', name)))
end

function Player:GetAbilityRecast(i) return ability_info(i, 'Recast') end
function Player:GetAbilityRecastCalc1(i) return ability_info(i, 'RecastCalc1') end
function Player:GetAbilityRecastTimerId(i) return ability_info(i, 'TimerId') end
function Player:GetAbilityRecastCalc2(i) return ability_info(i, 'RecastCalc2') end
function Player:GetMountRecast() return num(game.player(pid('player_t', 'MountRecast.Recast'))) end
function Player:GetMountRecastTimerId() return num(game.player(pid('player_t', 'MountRecast.TimerId'))) end
function Player:GetUnityFaction() return num(game.player(pid('player_t', 'UnityInfo.Bits.Faction'))) end
function Player:GetUnityPoints() return num(game.player(pid('player_t', 'UnityInfo.Bits.Points'))) end
function Player:GetHomepointMasks() return str(game.player('HomepointMasks')) or '' end

for n = 1, 4 do
    for _, axis in ipairs({ 'X', 'Z', 'Y', 'W' }) do
        local id = pid('player_t', ('StatusOffset%d.%s'):format(n, axis))
        Player[('GetStatusOffset%d%s'):format(n, axis)] = function() return num(game.player(id)) end
    end
end

local function job_point(i, name)
    local a = elem('jobpointsinfo_t', game.base('player') and (game.base('player') + field('player_t', 'JobPoints').offset), 'Jobs', i)
    if not a then return 0 end
    return num(game.read('jobpointentry_t', a, pid('jobpointentry_t', name)))
end

function Player:GetCapacityPoints(id) return job_point(id, 'CapacityPoints') end
function Player:GetJobPoints(id) return job_point(id, 'Points') end
function Player:GetJobPointsSpent(id) return job_point(id, 'PointsSpent') end

function Player:GetStatusIcons() return table_of(game.player('StatusIcons'), 32) end
function Player:GetStatusTimers() return table_of(game.player('StatusTimers'), 32) end
function Player:GetBuffs() return table_of(game.player('Buffs'), 32) end

function Player:GetPetMPPercent()
    local mpp = game.pet()
    return mpp or 0
end

function Player:GetPetTP()
    local _, tp = game.pet()
    return tp or 0
end

function Player:GetJobLevel(id) return game.job_level(id) or 0 end
function Player:GetJobMasterLevel(id) return game.master_level(id) or 0 end
function Player:GetJobMasterFlags() return game.master_flags() or 0 end
function Player:GetLoginStatus() return game.login_status() or 0 end
function Player:HasAbilityData() return game.has_ability_data() end
function Player:HasSpellData() return game.has_spell_data() end
function Player:HasSpell(id) return game.spell_known(id) end
function Player:HasKeyItem(id) return game.key_item(id) == true end

-- The known-ability field (0xB00 bits, packet 0x0AC plus a list-set range) is indexed by
-- Ashita's ability ids: weapon skills 0x000-0x1FF, job abilities 0x200+ (the game itself tests
-- job ability n at bit n + 0x200). Traits and pet commands: see game.c's notes (layout inferred).
function Player:HasAbility(id) return game.ability_known(id) end
function Player:HasWeaponSkill(id)
    id = tonumber(id) or -1
    return id >= 0 and id < 0x200 and game.ability_known(id)
end
function Player:HasTrait(id)
    id = tonumber(id) or -1
    return id >= 0 and id < 0x100 and game.ability_known(0x600 + id)
end
function Player:HasPetCommand(id)
    id = tonumber(id) or -1
    if id >= 0x700 then return game.ability_known(id) end
    return id >= 0 and id < 0x400 and game.ability_known(0x700 + id)
end

function Player:GetRawStructure() return proxy('player_t', game.base('player')) end

------------------------------------------------------------------------------------------------
-- ITarget
------------------------------------------------------------------------------------------------

local Target = {}

do
    local function tget(id, sub) return game.target(id, sub) end
    local function tset(id, v, sub) return game.set_target(id, v, sub) end
    each_path('target_t', function(path, f)
        if path:find('^Targets') then return end
        add_accessors(Target, 'target_t', path, path, f, false, tget, tset)
    end)
    -- Targets[index].<field>: GetTargetIndex(i) (Index), GetServerId(i), GetArrowPositionX(i)...
    each_path('targetentry_t', function(path, f)
        local m = path:gsub('%.', '')
        if m == 'Index' then m = 'TargetIndex' end
        local id = pid('targetentry_t', path)
        Target['Get' .. m] = function(_, i)
            local a = elem('target_t', game.base('target'), 'Targets', i)
            return a and num(game.read('targetentry_t', a, id)) or 0
        end
        Target['Set' .. m] = function(_, i, v)
            local a = elem('target_t', game.base('target'), 'Targets', i)
            if a then game.write('targetentry_t', a, id, v) end
        end
    end)
    local function wget(id, sub) return game.target_window(id, sub) end
    local function wset(id, v, sub) return game.set_target_window(id, v, sub) end
    each_path('targetwindow_t', function(path, f)
        -- m_pAnkShape -> WindowAnkShape, m_Sub -> WindowSub, Name -> WindowName
        local m = path:gsub('^m_p', ''):gsub('^m_', '')
        m = 'Window' .. m
        add_accessors(Target, 'targetwindow_t', m, path, f, false, wget, wset)
    end)
end

-- GetTargetPosF(i): one value; without an index, all four as a table
function Target:GetTargetPosF(i)
    if i == nil then return table_of(game.target('TargetPosF'), 4) end
    return num(game.target(pid('target_t', 'TargetPosF'), i))
end

function Target:GetLastTargetName() return cstr(game.target('LastTargetName')) end
function Target:GetRawStructure() return proxy('target_t', game.base('target')) end
function Target:GetRawStructureWindow() return proxy('targetwindow_t', game.base('target_window')) end

-- Ashita's force flag has no counterpart in the game's call (SetTarget(entity, 1, 0)).
function Target:SetTarget(index, force)
    return game.set_target_index(index)
end

------------------------------------------------------------------------------------------------
-- IInventory
------------------------------------------------------------------------------------------------

local Inventory = {}

do
    local function iget(id, sub) return game.inventory(id, sub) end
    local function iset(id, v, sub) return game.set_inventory(id, v, sub) end
    for _, f in ipairs(info('inventory_t').fields) do
        local text = f.type == 'chars'
        if (is_scalar(f) or text) and not f.name:find('^unknown') and not f.name:find('^padding') then
            add_accessors(Inventory, 'inventory_t', f.name, f.name, f, false, iget, iset)
        end
    end
end

function Inventory:GetRawStructure() return proxy('inventory_t', game.base('inventory')) end

function Inventory:GetContainerItem(container, index)
    return proxy('item_t', game.base('item', container, index))
end

-- occupied slots 1..80 (slot 0 is reserved: gil in the main inventory)
function Inventory:GetContainerCount(container)
    return game.container_count(container) or 0
end

-- the game stores the slot count including slot 0 (81 for a full inventory)
function Inventory:GetContainerCountMax(container)
    local m = game.container_max(container) or 0
    return m > 0 and m - 1 or 0
end

function Inventory:GetTreasurePoolItem(index)
    return proxy('treasureitem_t', game.base('treasure', index))
end

function Inventory:GetEquippedItem(index)
    return proxy('equipmententry_t', elem('inventory_t', game.base('inventory'), 'Equipment', index))
end

function Inventory:GetCheckEquippedItem(index)
    return proxy('itemcheck_t', elem('inventory_t', game.base('inventory'), 'CheckEquipment', index))
end

function Inventory:GetCheckLinkshellName() return str(game.inventory('CheckLinkshellName')) or '' end
function Inventory:GetSearchComment() return cstr(game.inventory('SearchComment')) or '' end

-- The selected-item pointer (Ashita's inventory.selecteditem, a menu object) has no SDK layout.
function Inventory:GetSelectedItemName() return '' end
function Inventory:GetSelectedItemId() return 0 end
function Inventory:GetSelectedItemIndex() return 0 end

------------------------------------------------------------------------------------------------
-- IAutoFollow, ICastBar
------------------------------------------------------------------------------------------------

local AutoFollow, CastBar = {}, {}

do
    local function aget(id, sub) return game.autofollow(id, sub) end
    local function aset(id, v, sub) return game.set_autofollow(id, v, sub) end
    for _, f in ipairs(info('autofollow_t').fields) do
        if is_scalar(f) and not f.name:find('^unknown') and not f.name:find('^padding') then
            add_accessors(AutoFollow, 'autofollow_t', f.name, f.name, f, false, aget, aset)
        end
    end
    local function cget(id, sub) return game.castbar(id, sub) end
    local function cset(id, v, sub) return game.set_castbar(id, v, sub) end
    for _, f in ipairs(info('castbar_t').fields) do
        if is_scalar(f) and not f.name:find('^unknown') and not f.name:find('^m_') and f.name ~= 'VTablePointer' then
            add_accessors(CastBar, 'castbar_t', f.name, f.name, f, false, cget, cset)
        end
    end
end

function AutoFollow:GetRawStructure() return proxy('autofollow_t', game.base('autofollow')) end
function CastBar:GetRawStructure() return proxy('castbar_t', game.base('castbar')) end

------------------------------------------------------------------------------------------------
-- IRecast
------------------------------------------------------------------------------------------------

local Recast = {}

-- the game's ability recast object: abilityrecast_t[31] then the live timers (1/60 s)
local function ability_recast(i, which)
    local v = { game.ability_recast(i) } -- timer, timer_id, recast, calc1, calc2
    return v[which] or 0
end

function Recast:GetAbilityTimer(i) return ability_recast(i, 1) end
function Recast:GetAbilityTimerId(i) return ability_recast(i, 2) end
function Recast:GetAbilityRecast(i) return ability_recast(i, 3) end
function Recast:GetAbilityCalc1(i) return ability_recast(i, 4) end
function Recast:GetAbilityCalc2(i) return ability_recast(i, 5) end
function Recast:GetSpellTimer(i) return game.spell_recast(i) or 0 end

------------------------------------------------------------------------------------------------
-- IMemoryManager
------------------------------------------------------------------------------------------------

local MemoryManager = {}

function MemoryManager:GetAutoFollow() return AutoFollow end
function MemoryManager:GetCastBar() return CastBar end
function MemoryManager:GetEntity() return Entity end
function MemoryManager:GetInventory() return Inventory end
function MemoryManager:GetParty() return Party end
function MemoryManager:GetPlayer() return Player end
function MemoryManager:GetRecast() return Recast end
function MemoryManager:GetTarget() return Target end

-- for the Ashita layer: its globals GetEntity(index) / GetPlayerEntity(), and struct proxies
MemoryManager.globals = { GetEntity = GetEntity, GetPlayerEntity = GetPlayerEntity }
MemoryManager.proxy = proxy
MemoryManager.interfaces = {
    IAutoFollow = AutoFollow, ICastBar = CastBar, IEntity = Entity, IInventory = Inventory,
    IParty = Party, IPlayer = Player, IRecast = Recast, ITarget = Target,
}

return MemoryManager
