--[[
AshitaCore (IAshitaCore) and its managers, over the host's xi.* API.

    local core = require('xi.ashita_core')   -- { AshitaCore, LogManager, AddonManager }

Managers: Chat, Configuration (INI files; 'boot' is the boot config), Font / Primitive
(ashita_fonts.lua), Gui (xi.ui.imgui()), Input (keyboard binds through the host's bind table),
Memory (ashita_memory.lua), Offset / Pointer (<install>/config/ashita/ashita.offsets.ini,
ashita.pointers.ini and their custom.* overrides; patterns scanned in FFXiMain.dll's code),
Packet, Plugin (only the addons plugin exists here), PolPlugin (none), Properties, Resource
(xi.res, from the game's DATs).
--]]

local ffi = require('ffi')
local bit = require('bit')
local util = require('xi.ashita_util')
local events = require('xi.ashita_events')
local fonts = require('xi.ashita_fonts')

local class, object, u32 = util.class, util.object, util.u32

local M = {}

local ROOT = xi.info.root -- host path, trailing '/'
local function config_path(rel) return ROOT .. 'config/' .. rel end

------------------------------------------------------------------------------------------------
-- INI files
------------------------------------------------------------------------------------------------

-- { sections = { name(lower) -> { name=, keys = { key(lower) -> { key=, value= } }, order = {} } }, order = {} }
local function ini_new() return { sections = {}, order = {} } end

local function ini_section(ini, name, create)
    local k = name:lower()
    local s = ini.sections[k]
    if not s and create then
        s = { name = name, keys = {}, order = {} }
        ini.sections[k] = s
        ini.order[#ini.order + 1] = k
    end
    return s
end

local function ini_set(ini, section, key, value)
    local s = ini_section(ini, section, true)
    local k = key:lower()
    if not s.keys[k] then s.order[#s.order + 1] = k end
    s.keys[k] = { key = key, value = value }
end

local function ini_parse(text, ini)
    ini = ini or ini_new()
    local cur
    for line in (text .. '\n'):gmatch('([^\n]*)\n') do
        line = line:gsub('\r$', '')
        local trimmed = line:match('^%s*(.-)%s*$')
        if trimmed == '' or trimmed:sub(1, 1) == ';' or trimmed:sub(1, 1) == '#' then
            -- comment
        elseif trimmed:sub(1, 1) == '[' then
            local name = trimmed:match('^%[(.-)%]')
            if name then
                cur = name:match('^%s*(.-)%s*$')
                ini_section(ini, cur, true)
            end
        elseif cur then
            local key, value = trimmed:match('^(.-)%s*=%s*(.*)$')
            if key then
                -- a trailing "; comment" after whitespace
                value = value:gsub('%s+;.*$', '')
                if value:match('^".*"$') then value = value:sub(2, -2) end
                ini_set(ini, cur, key, value)
            end
        end
    end
    return ini
end

local function read_file(path)
    local f = io.open(path, 'rb')
    if not f then return nil end
    local s = f:read('*a')
    f:close()
    return s
end

local function ini_load(path, ini)
    local s = read_file(path)
    if not s then return nil end
    return ini_parse(s, ini)
end

local function ini_text(ini)
    local out = {}
    for _, sk in ipairs(ini.order) do
        local s = ini.sections[sk]
        out[#out + 1] = '[' .. s.name .. ']'
        for _, kk in ipairs(s.order) do
            local e = s.keys[kk]
            out[#out + 1] = e.key .. ' = ' .. tostring(e.value)
        end
        out[#out + 1] = ''
    end
    return table.concat(out, '\n')
end

------------------------------------------------------------------------------------------------
-- IConfigurationManager
------------------------------------------------------------------------------------------------

local configs = {} -- alias(lower) -> ini

local function boot_config()
    local ini = ini_new()
    local want = os.getenv('FFXI_ASHITA_BOOT')
    local tried = {}
    if want and want ~= '' then tried[#tried + 1] = want:gsub('%.ini$', '') .. '.ini' end
    for _, n in ipairs({ 'boot.ini', 'default.ini', 'example.ini' }) do tried[#tried + 1] = n end
    for _, n in ipairs(tried) do
        if ini_load(config_path('boot/' .. n), ini) then break end
    end
    return ini
end

local function config(alias)
    alias = tostring(alias):lower()
    local c = configs[alias]
    if not c and alias == 'boot' then
        c = boot_config()
        configs[alias] = c
    end
    return c
end

-- the game's real window size stands in for the boot config's registry resolution
local function live_value(alias, section, key)
    if tostring(alias):lower() ~= 'boot' or tostring(section):lower() ~= 'ffxi.registry' then return nil end
    local w, h = xi.ui.screen()
    if key == '0001' and w and w > 0 then return tostring(w) end
    if key == '0002' and h and h > 0 then return tostring(h) end
    return nil
end

local function get_raw(alias, section, key)
    local live = live_value(alias, section, key)
    if live then return live end
    local c = config(alias)
    if not c then return nil end
    local s = ini_section(c, tostring(section), false)
    if not s then return nil end
    local e = s.keys[tostring(key):lower()]
    return e and e.value or nil
end

local CFG = {}

local function resolve_config_file(file)
    file = tostring(file)
    if file:match('^%a:[\\/]') or file:match('^[\\/]') then return file end
    return config_path(file)
end

function CFG:Load(alias, file)
    local ini = ini_load(resolve_config_file(file))
    if not ini then return false end
    configs[tostring(alias):lower()] = ini
    return true
end

function CFG:Save(alias, file)
    local c = config(alias)
    if not c then return false end
    local path = resolve_config_file(file)
    local dir = xi.fs.path(path):match('^(.*)/[^/]*$')
    if dir then xi.fs.mkdir(dir) end
    local f = io.open(path, 'wb')
    if not f then return false end
    f:write(ini_text(c))
    f:close()
    return true
end

function CFG:Delete(alias) configs[tostring(alias):lower()] = nil end

function CFG:GetSections(alias)
    local c = config(alias)
    if not c then return '' end
    local out = {}
    for _, k in ipairs(c.order) do out[#out + 1] = c.sections[k].name end
    return table.concat(out, '\n')
end

function CFG:GetSectionKeys(alias, section)
    local c = config(alias)
    local s = c and ini_section(c, tostring(section), false)
    if not s then return '' end
    local out = {}
    for _, k in ipairs(s.order) do out[#out + 1] = s.keys[k].key end
    return table.concat(out, '\n')
end

function CFG:GetString(alias, section, key) return get_raw(alias, section, key) end

function CFG:SetValue(alias, section, key, value)
    local k = tostring(alias):lower()
    local c = config(alias)
    if not c then
        c = ini_new()
        configs[k] = c
    end
    ini_set(c, tostring(section), tostring(key), value == nil and '' or tostring(value))
end

function CFG:GetBool(alias, section, key, def)
    local v = get_raw(alias, section, key)
    if v == nil then return def end
    v = v:lower()
    return v == '1' or v == 'true' or v == 'yes' or v == 'on'
end

local function number_getter(integer)
    return function(_, alias, section, key, def)
        local v = get_raw(alias, section, key)
        local n = v and tonumber(v)
        if n == nil then return def end
        if integer then n = n < 0 and math.ceil(n) or math.floor(n) end
        return n
    end
end

for _, t in ipairs({ 'UInt8', 'UInt16', 'UInt32', 'UInt64', 'Int8', 'Int16', 'Int32', 'Int64' }) do
    CFG['Get' .. t] = number_getter(true)
end
CFG.GetFloat = number_getter(false)
CFG.GetDouble = number_getter(false)

local ConfigurationManager = object(class('IConfigurationManager', CFG))

------------------------------------------------------------------------------------------------
-- IOffsetManager / IPointerManager
------------------------------------------------------------------------------------------------

local function load_pair(base, custom)
    local ini = ini_new()
    ini_load(config_path('ashita/' .. base), ini)
    ini_load(config_path('ashita/' .. custom), ini)
    return ini
end

local offsets_ini
local function offsets()
    offsets_ini = offsets_ini or load_pair('ashita.offsets.ini', 'custom.offsets.ini')
    return offsets_ini
end

local OFF = {}
function OFF:Add(section, key, offset) ini_set(offsets(), tostring(section), tostring(key), tonumber(offset) or 0) end
function OFF:Get(section, key)
    local s = ini_section(offsets(), tostring(section), false)
    local e = s and s.keys[tostring(key):lower()]
    return e and (tonumber(e.value) or 0) or 0
end
function OFF:Delete(section, key)
    local ini = offsets()
    local k = tostring(section):lower()
    local s = ini.sections[k]
    if not s then return end
    if key == nil then
        ini.sections[k] = nil
        for i = #ini.order, 1, -1 do if ini.order[i] == k then table.remove(ini.order, i) end end
    else
        s.keys[tostring(key):lower()] = nil
    end
end
local OffsetManager = object(class('IOffsetManager', OFF))

local pointers_ini
local pointers = {} -- name(lower) -> address (host), resolved once
local function pointer_defs()
    pointers_ini = pointers_ini or load_pair('ashita.pointers.ini', 'custom.pointers.ini')
    return pointers_ini
end

local function scan(modules, pattern, offset, count)
    local ns = require('xi.ashita_ns')
    return ns.memory.find(tostring(modules or 'FFXiMain.dll'), 0, tostring(pattern), tonumber(offset) or 0, tonumber(count) or 0)
end

local PTR = {}
function PTR:Add(name, a, pattern, offset, count)
    local k = tostring(name):lower()
    local v
    if pattern == nil then
        v = tonumber(a) or 0
    else
        v = scan(a, pattern, offset, count)
    end
    pointers[k] = v
    return v
end
function PTR:Get(name)
    local k = tostring(name):lower()
    local v = pointers[k]
    if v ~= nil then return v end
    local s = ini_section(pointer_defs(), k, false)
    v = 0
    if s then
        local function f(key) local e = s.keys[key] return e and e.value end
        local pattern = f('pattern')
        if pattern and pattern ~= '' then v = scan(f('module'), pattern, tonumber(f('offset')), tonumber(f('count'))) end
    end
    if v == 0 then xi.log('pointer ' .. k .. ' not found in FFXiMain.dll') end
    pointers[k] = v
    return v
end
function PTR:Delete(name) pointers[tostring(name):lower()] = nil end
local PointerManager = object(class('IPointerManager', PTR))

------------------------------------------------------------------------------------------------
-- IChatManager
------------------------------------------------------------------------------------------------

local CHAT = {}
local silent_aliases = false

local function cmd_mode(mode)
    mode = tonumber(mode) or 1
    if mode < 0 then mode = 1 end -- Ashita's own parse/script/force modes: as typed
    return mode
end

function CHAT:QueueCommand(mode, command)
    xi.chat.run(tostring(command or ''), cmd_mode(mode))
end

function CHAT:Write(mode, indent, msg)
    if msg == nil and type(indent) == 'string' then msg, indent = indent, false end
    xi.chat.write(tostring(msg or ''), tonumber(mode) or 1)
end
CHAT.AddChatMessage = CHAT.Write

function CHAT:ParseAutoTranslate(msg, use_brackets)
    msg = tostring(msg or '')
    return (msg:gsub('\xFD(.)(.)(.)(.)\xFD', function(k, l, b2, b3)
        local s = xi.res.auto_translate(k:byte(), l:byte(), b2:byte(), b3:byte())
        if not s then return nil end
        if use_brackets then return '\xEF\x27' .. s .. '\xEF\x28' end
        return s
    end))
end

function CHAT:ExecuteScript(file, args, threaded)
    xi.chat.exec(tostring(file), 1)
end

function CHAT:ExecuteScriptString(str, args, threaded)
    for line in (tostring(str or '') .. '\n'):gmatch('([^\n]*)\n') do
        line = line:gsub('\r$', '')
        if line:match('%S') and not line:match('^%s*#') then xi.chat.run(line, 1) end
    end
end

local function input() return (xi.chat.input()) or '' end
function CHAT:GetInputTextRaw() return input() end
function CHAT:SetInputTextRaw(msg) xi.chat.set_input(tostring(msg or '')) end
function CHAT:GetInputTextRawLength() return #input() end
function CHAT:GetInputTextRawCaretPosition() return #input() end
function CHAT:GetInputTextParsed() return input() end
function CHAT:SetInputTextParsed(msg) xi.chat.set_input(tostring(msg or '')) end
function CHAT:GetInputTextParsedLength() return #input() end
function CHAT:GetInputTextParsedLengthMax() return 150 end
function CHAT:GetInputTextDisplay() return input() end
function CHAT:SetInputTextDisplay(msg) xi.chat.set_input(tostring(msg or '')) end
function CHAT:SetInputText(msg) xi.chat.set_input(tostring(msg or '')) end
function CHAT:IsInputOpen()
    local _, open = xi.chat.input()
    return open and 0x11 or 0
end
function CHAT:GetSilentAliases() return silent_aliases end
function CHAT:SetSilentAliases(v) silent_aliases = v and true or false end

local ChatManager = object(class('IChatManager', CHAT))

------------------------------------------------------------------------------------------------
-- IPacketManager
------------------------------------------------------------------------------------------------

local function packet_bytes(id, packet)
    local s = util.bytes(packet)
    if #s >= 2 and id then
        -- the header's id follows the id given (its size bits stay)
        local h = s:byte(1) + s:byte(2) * 256
        h = bit.bor(bit.band(h, 0xFE00), bit.band(tonumber(id) or 0, 0x1FF))
        s = string.char(bit.band(h, 0xFF), bit.rshift(h, 8)) .. s:sub(3)
    end
    return s
end

local PKT = {}
function PKT:AddIncomingPacket(id, packet) xi.packets.inject(false, packet_bytes(id, packet)) end
function PKT:AddOutgoingPacket(id, packet) xi.packets.inject(true, packet_bytes(id, packet)) end
function PKT:QueuePacket(id, len, align, p1, p2, callback)
    len = tonumber(len) or 4
    local buf = ffi.new('uint8_t[?]', math.max(len, 4))
    local size = math.floor((len + 3) / 4)
    local h = bit.bor(bit.band(tonumber(id) or 0, 0x1FF), bit.lshift(size, 9))
    buf[0], buf[1] = bit.band(h, 0xFF), bit.rshift(h, 8)
    if type(callback) == 'function' then
        local ok, err = xpcall(callback, debug.traceback, ffi.cast('uint8_t*', buf))
        if not ok then util.report(err, 'QueuePacket') return false end
    end
    buf[0], buf[1] = bit.band(h, 0xFF), bit.rshift(h, 8)
    xi.packets.inject(true, ffi.string(buf, len))
    return true
end
local PacketManager = object(class('IPacketManager', PKT))

------------------------------------------------------------------------------------------------
-- IPluginManager / IPolPluginManager
------------------------------------------------------------------------------------------------

local ADDONS_PLUGIN = {}
function ADDONS_PLUGIN:GetName() return 'Addons' end
function ADDONS_PLUGIN:GetAuthor() return 'Ashita Development Team' end
function ADDONS_PLUGIN:GetDescription() return 'Lua addons (hosted by FFXIRecompile).' end
function ADDONS_PLUGIN:GetLink() return 'https://www.ashitaxi.com/' end
function ADDONS_PLUGIN:GetVersion() return 4.30 end
function ADDONS_PLUGIN:GetInterfaceVersion() return 4.30 end
function ADDONS_PLUGIN:GetPriority() return 0 end
function ADDONS_PLUGIN:GetFlags() return 0x1F end
local addons_plugin = object(class('IPlugin', ADDONS_PLUGIN))

local PLG = {}
local silent_plugins = false
function PLG:IsLoaded(name) return tostring(name):lower() == 'addons' end
function PLG:Get(k)
    if type(k) == 'number' then return k == 0 and addons_plugin or nil end
    return tostring(k):lower() == 'addons' and addons_plugin or nil
end
function PLG:Count() return 1 end
function PLG:RaiseEvent(name, data, size)
    if type(data) == 'string' and size then data = data:sub(1, tonumber(size)) end
    events.raise_plugin_event(name, data)
end
function PLG:GetSilentPlugins() return silent_plugins end
function PLG:SetSilentPlugins(v) silent_plugins = v and true or false end
local PluginManager = object(class('IPluginManager', PLG))

local POL = {}
function POL:IsLoaded() return false end
function POL:Get() return nil end
function POL:Count() return 0 end
function POL:RaiseEvent() end
local PolPluginManager = object(class('IPolPluginManager', POL))

------------------------------------------------------------------------------------------------
-- IProperties
------------------------------------------------------------------------------------------------

local GAME_HWND = 0x00010010 -- a stand-in: there is one window and no Win32 behind it
local props = { ambient_enabled = false, ambient_color = 0xFFFFFFFF, fill_mode = 3, style = 0x16CF0000, style_ex = 0 }

local PROP = {}
function PROP:GetFinalFantasyHwnd() return GAME_HWND end
function PROP:GetFinalFantasyStyle() return props.style end
function PROP:GetFinalFantasyStyleEx() return props.style_ex end
function PROP:GetFinalFantasyRect()
    local w, h = xi.ui.screen()
    return RECT.new(0, 0, w or 0, h or 0)
end
function PROP:SetFinalFantasyHwnd() end -- the window is the host's
function PROP:SetFinalFantasyStyle(v) props.style = tonumber(v) or props.style end
function PROP:SetFinalFantasyStyleEx(v) props.style_ex = tonumber(v) or props.style_ex end
function PROP:SetFinalFantasyRect() end
function PROP:GetD3DAmbientEnabled() return props.ambient_enabled end
function PROP:GetD3DAmbientColor() return props.ambient_color end
function PROP:GetD3DFillMode() return props.fill_mode end
function PROP:SetD3DAmbientEnabled(v) props.ambient_enabled = v and true or false end
function PROP:SetD3DAmbientColor(v) props.ambient_color = u32(v) end
function PROP:SetD3DFillMode(v) props.fill_mode = tonumber(v) or 3 end
local Properties = object(class('IProperties', PROP))

------------------------------------------------------------------------------------------------
-- IInputManager
------------------------------------------------------------------------------------------------

local binds = {} -- key text -> command (binds made through IKeyboard)
local kb = { windows_key = false, block = false, block_binds_during_input = true, silent = false }
local mouse = { block = false }
local pads = { background = false, disable = false, controller_deadzone = true, xinput_deadzone = true }

local function bind_text(key, down, alt, apps, ctrl, shift, win, closed, open)
    local name = util.dik.name(key)
    if not name then return nil end
    return (ctrl and '^' or '') .. (alt and '!' or '') .. (win and '@' or '') .. (apps and '#' or '') ..
        (shift and '+' or '') .. (closed and '%' or '') .. (open and '$' or '') .. name, down
end

local KB = {}
function KB:Bind(key, down, alt, apps, ctrl, shift, win, closed, open, command)
    local text = bind_text(key, down, alt, apps, ctrl, shift, win, closed, open)
    if not text then return end
    binds[text .. (down == false and ':up' or '')] = tostring(command or '')
    xi.input.bind(text, tostring(command or ''), down == false)
end
function KB:Unbind(key, down, alt, apps, ctrl, shift, win, closed, open)
    local text = bind_text(key, down, alt, apps, ctrl, shift, win, closed, open)
    if not text then return end
    binds[text] = nil
    binds[text .. ':up'] = nil
    xi.input.unbind(text)
end
function KB:UnbindAll()
    binds = {}
    xi.input.unbind()
end
function KB:IsBound(key, down, alt, apps, ctrl, shift, win, closed, open)
    local text = bind_text(key, down, alt, apps, ctrl, shift, win, closed, open)
    return text ~= nil and binds[text .. (down == false and ':up' or '')] ~= nil
end
function KB:V2D(vk) return util.dik.from_vk(vk) end
function KB:D2V(dik) return util.dik.vk(dik) end
function KB:S2D(s) return util.dik.code(s) or 0 end
function KB:D2S(dik) return util.dik.name(dik) or '' end
function KB:GetWindowsKeyEnabled() return kb.windows_key end
function KB:SetWindowsKeyEnabled(v) kb.windows_key = v and true or false end
function KB:GetBlockInput() return kb.block end
function KB:SetBlockInput(v)
    kb.block = v and true or false
    if kb.block then
        xi.events.on('key', function(e) e.blocked = true end, 'ashita:block_keys')
    else
        xi.events.off('key', 'ashita:block_keys')
    end
end
function KB:GetBlockBindsDuringInput() return kb.block_binds_during_input end
function KB:SetBlockBindsDuringInput(v) kb.block_binds_during_input = v and true or false end
function KB:GetSilentBinds() return kb.silent end
function KB:SetSilentBinds(v) kb.silent = v and true or false end
local Keyboard = object(class('IKeyboard', KB))

local MS = {}
function MS:GetBlockInput() return mouse.block end
function MS:SetBlockInput(v)
    mouse.block = v and true or false
    if mouse.block then
        xi.events.on('mouse', function(e) e.blocked = true end, 'ashita:block_mouse')
    else
        xi.events.off('mouse', 'ashita:block_mouse')
    end
end
local Mouse = object(class('IMouse', MS))

local CTL = {}
CTL.QueueButtonData = util.stub('IController.QueueButtonData') -- no controller injection path
function CTL:GetTrackDeadZone() return pads.controller_deadzone end
function CTL:SetTrackDeadZone(v) pads.controller_deadzone = v and true or false end
local Controller = object(class('IController', CTL))

local XIN = {}
XIN.QueueButtonData = util.stub('IXInput.QueueButtonData')
function XIN:GetTrackDeadZone() return pads.xinput_deadzone end
function XIN:SetTrackDeadZone(v) pads.xinput_deadzone = v and true or false end
local XInput = object(class('IXInput', XIN))

local INP = {}
function INP:GetController() return Controller end
function INP:GetKeyboard() return Keyboard end
function INP:GetMouse() return Mouse end
function INP:GetXInput() return XInput end
function INP:GetAllowGamepadInBackground() return pads.background end
function INP:SetAllowGamepadInBackground(v) pads.background = v and true or false end
function INP:GetDisableGamepad() return pads.disable end
function INP:SetDisableGamepad(v) pads.disable = v and true or false end
local InputManager = object(class('IInputManager', INP))

------------------------------------------------------------------------------------------------
-- ILogManager
------------------------------------------------------------------------------------------------

local log_level = 5
local LOG = {}
local LEVELS = { [1] = 'critical', [2] = 'error', [3] = 'warn', [4] = 'info', [5] = 'debug' }
function LOG:Log(level, source, message)
    level = tonumber(level) or 4
    if level > log_level then return end
    xi.log(('[%s] %s: %s'):format(LEVELS[level] or tostring(level), tostring(source), tostring(message)))
end
function LOG:GetLogLevel() return log_level end
function LOG:SetLogLevel(v) log_level = tonumber(v) or log_level end
function LOG:GetPointer() return 0 end
local LogManager = object(class('ILogManager', LOG))

------------------------------------------------------------------------------------------------
-- IResourceManager
------------------------------------------------------------------------------------------------

local res = xi.res

-- Ashita's per-language text: [1] the default language, [2] Japanese, [3] English
local function names(t)
    t = t or {}
    local en, ja = t.en or '', t.ja or ''
    return { [0] = en, [1] = en, [2] = ja, [3] = en }
end

-- a BMP file (what D3DX's *FromFileInMemory wants) around a DIB (BITMAPINFOHEADER onward)
local function bmp_file(dib)
    if not dib or #dib < 40 then return nil end
    local function u32le(s, o) local a, b2, c, d = s:byte(o, o + 3) return a + b2 * 256 + c * 65536 + d * 16777216 end
    local header = u32le(dib, 1)
    local bpp = dib:byte(15) + dib:byte(16) * 256
    local used = u32le(dib, 33)
    local palette = 0
    if bpp <= 8 then palette = (used > 0 and used or 2 ^ bpp) * 4 end
    local total = 14 + #dib
    local off = 14 + header + palette
    local function le(v) return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256) end
    return 'BM' .. le(total) .. le(0) .. le(off) .. dib
end

local function icon_fields(t, fn, id)
    -- lazily: decoding the icon costs
    return setmetatable(t, {
        __index = function(tt, k)
            if k ~= 'Bitmap' and k ~= 'ImageSize' and k ~= 'ImageType' and k ~= 'ImageName' then return nil end
            local dib, _, _, _, name = fn(id)
            local file = bmp_file(dib)
            rawset(tt, 'Bitmap', file or '')
            rawset(tt, 'ImageSize', file and #file or 0)
            rawset(tt, 'ImageType', file and 1 or 0)
            rawset(tt, 'ImageName', name or '')
            return rawget(tt, k)
        end,
    })
end

local function item_of(r)
    if not r then return nil end
    local mon = {}
    for i, a in ipairs(r.monstrosity_abilities or {}) do
        mon[i] = { AbilityId = a.id or 0, Level = a.level or 0, Unknown0000 = a.unknown or 0 }
    end
    local t = {
        Id = r.id, Flags = r.flags or 0, StackSize = r.stack or 0, Type = r.type or 0, ResourceId = r.resource_id or 0,
        Targets = r.targets or 0, Level = r.level or 0, Slots = r.slots or 0, Races = r.races or 0, Jobs = r.jobs or 0,
        SuperiorLevel = r.superior_level or 0, ShieldSize = r.shield_size or 0, MaxCharges = r.max_charges or 0,
        CastTime = r.cast_time or 0, CastDelay = r.cast_delay or 0, RecastDelay = r.recast_delay or 0,
        BaseItemId = r.base_item_id or 0, ItemLevel = r.item_level or 0, Damage = r.damage or 0, Delay = r.delay or 0,
        DPS = r.dps or 0, Skill = r.skill or 0, JugSize = r.jug_size or 0, WeaponUnknown0000 = r.weapon_unknown or 0,
        Range = r.range or 0, AreaRange = r.area_range or 0, AreaShapeType = r.area_shape or 0,
        AreaCursorTargetType = r.area_cursor or 0, Element = r.element or 0, Storage = r.storage or 0,
        AttachmentFlags = r.attachment_flags or 0, InstinctCost = r.instinct_cost or 0,
        MonstrosityId = r.monstrosity_id or 0, MonstrosityName = r.monstrosity_name or '',
        MonstrosityData = r.monstrosity_data or '', MonstrosityAbilities = mon, PuppetSlotId = r.puppet_slot or 0,
        PuppetElements = r.puppet_elements or 0, SlipData = r.slip_data or '', UsableData0000 = r.usable0 or 0,
        UsableData0001 = r.usable1 or 0, UsableData0002 = r.usable2 or 0, Article = r.article or 0,
        Name = names(r.name), Description = names(r.description), LogNameSingular = names(r.log_singular),
        LogNamePlural = names(r.log_plural),
    }
    return icon_fields(t, res.item_icon, r.id)
end

local function levels_of(l)
    local out = {}
    for j = 0, 23 do out[j + 1] = (l and l[j]) or -1 end
    return out
end

local function spell_of(r)
    if not r then return nil end
    return {
        Index = r.index or 0, Type = r.type or 0, Element = r.element or 0, Targets = r.targets or 0, Skill = r.skill or 0,
        ManaCost = r.mp_cost or 0, CastTime = r.cast_time or 0, RecastDelay = r.recast_delay or 0,
        LevelRequired = levels_of(r.levels), Id = r.id or 0, ListIconNQ = r.icon_nq or 0, ListIconHQ = r.icon_hq or 0,
        Requirements = r.requirements or 0, Range = r.range or 0, AreaRange = r.area_range or 0,
        AreaShapeType = r.area_shape or 0, CursorTargetType = r.cursor_target or 0, Unknown0000 = {}, AreaFlags = r.area_flags or 0,
        Unknown0001 = 0, Unknown0002 = 0, Unknown0003 = 0, Unknown0004 = {}, JobPointMask = r.job_point_mask or 0,
        Unknown0005 = {}, EOE = 0xFF, Name = names(r.name), Description = names(r.description),
    }
end

local function ability_of(r)
    if not r then return nil end
    return {
        Id = r.id or 0, Type = r.type or 0, Element = r.element or 0, ListIconId = r.icon_id or 0, ManaCost = r.mp_cost or 0,
        RecastTimerId = r.recast_id or 0, Targets = r.targets or 0, TPCost = r.tp_cost or 0,
        MenuCategoryId = r.menu_category or 0, MonsterLevel = r.monster_level or 0, Range = r.range or 0,
        AreaRange = r.area_range or 0, AreaShapeType = r.area_shape or 0, CursorTargetType = r.cursor_target or 0,
        Unknown0000 = 0, Unknown0001 = 0, Unknown0002 = 0, Unknown0003 = 0, Unknown0004 = 0, Unknown0005 = 0,
        Unknown0006 = 0, Unknown0007 = 0, Unknown0008 = 0, Unknown0009 = {}, EOE = 0xFF,
        Name = names(r.name), Description = names(r.description),
    }
end

local function status_of(r)
    if not r then return nil end
    local t = { Index = r.index or 0, Id = r.id or 0, CanCancel = r.can_cancel or 0, HideTimer = r.hide_timer or 0,
        Description = names(r.description) }
    return icon_fields(t, res.status_icon, r.id)
end

-- small caches: addons look the same records up every frame
local function cached(fn)
    local cache = {}
    return function(k)
        if k == nil then return nil end
        local v = cache[k]
        if v == nil then
            v = fn(k) or false
            cache[k] = v
        end
        return v or nil
    end
end

local get_item = cached(function(id) return item_of(res.item(id)) end)
local get_spell = cached(function(id) return spell_of(res.spell(id)) end)
local get_ability = cached(function(id) return ability_of(res.ability(id)) end)
local get_status = cached(function(id) return status_of(res.status(id)) end)

local RES = {}
function RES:GetAbilityById(id) return get_ability(tonumber(id)) end
function RES:GetAbilityByName(name, lang)
    local r = res.ability_by_name(tostring(name), tonumber(lang) or 0)
    return r and get_ability(r.id) or nil
end
function RES:GetAbilityByTimerId(id)
    local r = res.ability_by_recast(tonumber(id) or -1)
    return r and get_ability(r.id) or nil
end
function RES:GetSpellById(id) return get_spell(tonumber(id)) end
function RES:GetSpellByName(name, lang)
    local r = res.spell_by_name(tostring(name), tonumber(lang) or 0)
    return r and get_spell(r.index or r.id) or nil
end
function RES:GetItemById(id) return get_item(tonumber(id)) end
function RES:GetItemByName(name, lang)
    local r = res.item_by_name(tostring(name), tonumber(lang) or 0)
    return r and get_item(r.id) or nil
end
function RES:GetStatusIconByIndex(index)
    local r = res.status_by_index(tonumber(index) or -1)
    return r and get_status(r.id) or nil
end
function RES:GetStatusIconById(id) return get_status(tonumber(id)) end
function RES:GetString(tbl, a, lang)
    if type(a) == 'string' then return res.string_find(tostring(tbl), a, tonumber(lang) or 0) or -1 end
    return res.string(tostring(tbl), tonumber(a) or -1, tonumber(lang) or 0)
end
function RES:GetStringLength(tbl, index, lang) return res.string_length(tostring(tbl), tonumber(index) or -1, tonumber(lang) or 0) or -1 end
RES.GetTexture = util.stub('IResourceManager.GetTexture') -- Direct3D textures from Ashita's cache
RES.GetTextureInfo = util.stub('IResourceManager.GetTextureInfo')
function RES:GetFilePath(id)
    local p = res.file_path(tonumber(id) or -1)
    return p and util.win_path(p) or ''
end
function RES:GetAbilityRange(id, area)
    local a = get_ability(tonumber(id))
    if not a then return 0 end
    return area and a.AreaRange or a.Range
end
function RES:GetAbilityType(tid)
    local a = RES.GetAbilityByTimerId(nil, tid)
    return a and a.Type or 0
end
function RES:GetSpellRange(id, area)
    local s = get_spell(tonumber(id))
    if not s then return 0 end
    return area and s.AreaRange or s.Range
end
local ResourceManager = object(class('IResourceManager', RES))

------------------------------------------------------------------------------------------------
-- IMemoryManager (ashita_memory.lua), IGuiManager (xi.ui.imgui())
------------------------------------------------------------------------------------------------

local memory_src = xi.embedded('ashita_memory')
local MemoryManager = assert(loadstring(memory_src, '=[xi ashita_memory]'))(xi.game)
-- a manager call on a missing member: loud like the rest
setmetatable(MemoryManager, class('IMemoryManager', {}))
for name, iface in pairs(MemoryManager.interfaces) do
    if getmetatable(iface) == nil then
        setmetatable(iface, { __index = function(_, k)
            if type(k) ~= 'string' or k:sub(1, 2) == '__' then return nil end
            return util.unsupported(name .. '.' .. k, 2)
        end })
    end
end

------------------------------------------------------------------------------------------------
-- IAshitaCore
------------------------------------------------------------------------------------------------

local CORE = {}
function CORE:GetHandle() return 0 end
function CORE:GetInstallPath() return util.win_path(ROOT) end
function CORE:GetDirect3DDevice()
    if xi.d3d8_device then return xi.d3d8_device() or 0 end
    return 0
end
function CORE:GetProperties() return Properties end
function CORE:GetChatManager() return ChatManager end
function CORE:GetConfigurationManager() return ConfigurationManager end
function CORE:GetFontManager() return fonts.FontManager end
function CORE:GetGuiManager() return xi.ui.imgui() end
function CORE:GetInputManager() return InputManager end
function CORE:GetMemoryManager() return MemoryManager end
function CORE:GetOffsetManager() return OffsetManager end
function CORE:GetPacketManager() return PacketManager end
function CORE:GetPluginManager() return PluginManager end
function CORE:GetPolPluginManager() return PolPluginManager end
function CORE:GetPointerManager() return PointerManager end
function CORE:GetPrimitiveManager() return fonts.PrimitiveManager end
function CORE:GetResourceManager() return ResourceManager end
function CORE:GetPointer() return 0 end
function CORE:GetSystemMetrics(index)
    local w, h = xi.ui.screen()
    index = tonumber(index) or -1
    if index == 0 or index == 16 or index == 78 then return w or 0 end -- SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    if index == 1 or index == 17 or index == 79 then return h or 0 end
    return 0
end
-- window management on the host's single window: nothing to do
function CORE:SendMessageA() return 0 end
function CORE:SetCursorPos() return true end
function CORE:SetFocus() return GAME_HWND end
function CORE:SetForegroundWindow() return true end
function CORE:SetPriorityClass() return true end

M.AshitaCore = object(class('IAshitaCore', CORE))
M.LogManager = LogManager
M.MemoryManager = MemoryManager

------------------------------------------------------------------------------------------------
-- AddonManager (the addons plugin's own table)
------------------------------------------------------------------------------------------------

local meta_cache = {}
local function addon_meta(name)
    local a
    for _, v in ipairs(xi.addons.list()) do
        if v.name:lower() == tostring(name):lower() then a = v break end
    end
    if not a then return nil end
    local m = meta_cache[a.name]
    if m then return m end
    m = { name = a.name, file = a.path .. a.name .. '.lua' }
    local src = read_file(m.file) or ''
    for _, k in ipairs({ 'author', 'desc', 'link', 'version' }) do
        m[k] = src:match('addon%.' .. k .. '%s*=%s*\'([^\']*)\'') or src:match('addon%.' .. k .. '%s*=%s*"([^"]*)"')
    end
    meta_cache[a.name] = m
    return m
end

local AM = {}
function AM:Count() return #xi.addons.list() end
function AM:Get(index)
    local l = xi.addons.list()
    local a = l[(tonumber(index) or -1) + 1]
    return a and a.name or nil
end
function AM:IsLoaded(name) return xi.addons.loaded(tostring(name)) end
function AM:GetState(name) return xi.addons.loaded(tostring(name)) and 1 or 0 end
function AM:GetAuthor(name) local m = addon_meta(name) return m and m.author end
function AM:GetDescription(name) local m = addon_meta(name) return m and m.desc end
function AM:GetFileName(name) local m = addon_meta(name) return m and util.win_path(m.file) end
function AM:GetLink(name) local m = addon_meta(name) return m and m.link end
function AM:GetVersion(name) local m = addon_meta(name) return m and m.version end
function AM:GetMemoryUsage(name)
    if tostring(name):lower() == xi.info.name:lower() then return collectgarbage('count') * 1024 end
    return 0
end
M.AddonManager = object(class('AddonManager', AM))

return M
