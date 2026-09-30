--[[
The Ashita v4 addon API, run in an Ashita addon's state after xi.lua and before the addon's own
file (host/addons/core.c). Our own code, written against Ashita's LuaLS annotations
(addons/libs/annotations/) and how its addons and libs use the API; Ashita's libs (common,
settings, imgui, fonts, primitives, ...) run on top unchanged.

    ashita.lua(hooks)    -- `...` is the hooks table xi.lua returned

Globals it sets:
  addon                  name, path (the addon's folder, Windows-shaped, trailing '\'), instance
  ashita                 events, tasks, fs, memory, misc, regex, time, bits, addons_version,
                         interface_version                        (ashita_events.lua, ashita_ns.lua)
  AshitaCore             IAshitaCore and every manager            (ashita_core.lua, ashita_fonts.lua)
  AddonManager, LogManager, GetEntity, GetPlayerEntity, RECT, SIZE
  the enums: ChatInputOpenStatus, CommandMode, FontBorderFlags, FontCreateFlags, FontDrawFlags,
             FrameAnchor, LogLevel, KeyboardEvent, MouseEvent, PluginFlags, PrimitiveDrawFlags
  coroutine.kill, table.make_literal (coroutine.sleep / sleepf come from xi.lua)

Anything Ashita has that isn't here fails loudly: "<Class>.<Member> is not supported yet", and the
host log gets one "unsupported: <Class>.<Member>" line per member (tools/addon_survey.py counts them).
--]]

local hooks = ...

local util = require('xi.ashita_util')

------------------------------------------------------------------------------------------------
-- enums (constants.lua)
------------------------------------------------------------------------------------------------

ChatInputOpenStatus = { Closed = 0x00, Opened = 0x01, OpenedChat = 0x11, OpenedOther = 0x21 }
CommandMode = {
    AshitaForceHandle = -3, AshitaScript = -2, AshitaParse = -1, Menu = 0x00, Typed = 0x01, Macro = 0x02,
    SubTargetST = 0x03, SubTargetSTPC = 0x04, SubTargetSTNPC = 0x05, SubTargetSTPT = 0x06, SubTargetSTAL = 0x07,
}
FontBorderFlags = { None = 0x00, Top = 0x01, Bottom = 0x02, Left = 0x04, Right = 0x08, All = 0x0F }
FontCreateFlags = { None = 0x00, Bold = 0x01, Italic = 0x02, StrikeThrough = 0x04, Underlined = 0x08, CustomFile = 0x10,
    ClearType = 0x20 }
FontDrawFlags = { None = 0x00, Filtered = 0x01, CenterX = 0x02, CenterY = 0x04, RightJustified = 0x08, Outlined = 0x10,
    ManualRender = 0x20 }
FrameAnchor = { TopLeft = 0x00, TopRight = 0x01, BottomLeft = 0x02, BottomRight = 0x03, Right = 0x01, Bottom = 0x02 }
LogLevel = { None = 0x00, Critical = 0x01, Error = 0x02, Warn = 0x03, Info = 0x04, Debug = 0x05 }
KeyboardEvent = { Down = 0x00, Up = 0x01 }
MouseEvent = { ClickLeft = 0x00, ClickRight = 0x01, ClickMiddle = 0x02, ClickX1 = 0x03, ClickX2 = 0x04, WheelUp = 0x05,
    WheelDown = 0x06, Move = 0x07 }
PluginFlags = { None = 0x00, UseCommands = 0x01, UseText = 0x02, UsePackets = 0x04, UseDirect3D = 0x08,
    UsePluginEvents = 0x10, Legacy = 0x07, LegacyDirect3D = 0x15, All = 0x1F }
PrimitiveDrawFlags = { None = 0x00, ManualRender = 0x01 }

------------------------------------------------------------------------------------------------
-- ashita.*
------------------------------------------------------------------------------------------------

local ns = require('xi.ashita_ns')          -- also sets coroutine.kill, table.make_literal
local events = require('xi.ashita_events')
local fonts = require('xi.ashita_fonts')    -- also sets RECT, SIZE

ashita = util.partial({
    addons_version = 4.30,
    interface_version = 4.30,
    events = util.partial({ register = events.register, unregister = events.unregister }, 'ashita.events'),
    tasks = ns.tasks,
    fs = ns.fs,
    memory = ns.memory,
    misc = ns.misc,
    regex = ns.regex,
    time = ns.time,
    bits = ns.bits,
}, 'ashita')

------------------------------------------------------------------------------------------------
-- AshitaCore and friends
------------------------------------------------------------------------------------------------

-- Ashita's built-in struct library (global, and require('struct'))
struct = require('xi.ashita_struct')
package.loaded['struct'] = struct

local core = require('xi.ashita_core')
AshitaCore = core.AshitaCore
LogManager = core.LogManager
AddonManager = core.AddonManager
GetEntity = core.MemoryManager.globals.GetEntity
GetPlayerEntity = core.MemoryManager.globals.GetPlayerEntity

------------------------------------------------------------------------------------------------
-- addon
------------------------------------------------------------------------------------------------

-- Ashita 4.3's per-addon feature switches (addon.instance:enable_feature(addon_feature.x)).
-- use_packet_chunks: packet events carry the whole chunk (chunk_data / chunk_data_raw), which they
-- always do here.
addon_feature = { none = 0, use_packet_chunks = 1 }

local frame = 0
local features = {}
local instance = util.object(util.class('Addon', {
    get_memory_usage = function() return collectgarbage('count') * 1024 end,
    enable_feature = function(_, f) features[tonumber(f) or f] = true return true end,
    disable_feature = function(_, f) features[tonumber(f) or f] = nil return true end,
    has_feature = function(_, f) return features[tonumber(f) or f] == true end,
}, { current_frame = true, state = true }), { current_frame = 0, state = 1 })

addon = {
    name = xi.info.name,
    path = util.win_path(xi.info.path),
    author = '',
    version = '0.0',
    desc = '',
    link = '',
    instance = instance,
}

xi.events.on('frame', function()
    frame = frame + 1
    rawset(instance, 'current_frame', frame)
end, 'ashita:frame_counter')

-- print: Ashita writes it to the chat log (xi.lua already does); error text stays for the host
return hooks
