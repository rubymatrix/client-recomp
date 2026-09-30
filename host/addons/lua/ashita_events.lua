--[[
ashita.events: Ashita v4's event names and event tables over the host's xi events.

    local events = require('xi.ashita_events')   -- { register, unregister, raise_plugin_event }

Ashita event      xi event     what the callback gets
  load            load         ()
  unload          unload       ()
  command         command      e: mode, command, injected, blocked
  text_in         text_in      e: mode, indent, message, mode_modified, indent_modified,
                                  message_modified, injected, blocked
  text_out        text_out     e: mode, message, mode_modified, message_modified, injected, blocked
  packet_in/out   packet_*     e: id, size, data, data_raw, data_modified, data_modified_raw,
                                  chunk_size, chunk_data, chunk_data_raw, injected, blocked
                                  (the *_raw fields are uint8_t* buffers; writes through
                                  data_modified_raw change the packet like data_modified does)
  key             key          e: wparam (virtual key), lparam (WM_KEY* flags), blocked
  key_data        key          e: key (DIK code), down, blocked
  key_state       frame        e: data, data_raw (uint8_t[256], DIK states), size (read-only here)
  mouse           mouse        e: message, x, y, delta, blocked
  d3d_beginscene  frame        (true)            once a frame, before drawing
  d3d_present     present      (nil, nil, nil, nil)   inside the overlay's (ImGui's) frame
  d3d_endscene    postrender   (true)
  plugin_event    message      e: name, data, data_raw, size, blocked
  xinput_state    frame        e: state (XINPUT_STATE* of the first pad), injected, blocked
  xinput_button   frame        e: button (bit number in wButtons), state (1 down, 0 up), injected, blocked
                               (both polled from the pad the game reads; blocking doesn't hide
                               anything from the game here)
  d3d_dp, d3d_dip, dinput_button, dinput_state: accepted, never raised (no per-draw-call or
      DirectInput-pad hook here); the first registration logs "unsupported: event <name>".

Handlers of the input events run as coroutines, so a handler may coroutine.sleep (the rest runs
from the task list). Errors in one handler are reported and the next still runs.
--]]

local ffi = require('ffi')
local bit = require('bit')
local util = require('xi.ashita_util')

local co_create, co_resume, co_status, co_yield = coroutine.create, coroutine.resume, coroutine.status, coroutine.yield
local traceback = debug.traceback

local M = {}

local handlers = {} -- ashita name -> list of { alias, fn }

-- Runs fn(...) as a coroutine; if it sleeps, the rest continues from xi.tasks.
local function invoke_co(fn, where, ...)
    local co = co_create(fn)
    local ok, a, b = co_resume(co, ...)
    if not ok then
        util.report(traceback(co, tostring(a)), where)
        return
    end
    if co_status(co) == 'suspended' then
        xi.tasks.spawn(function()
            local x, y = a, b
            while true do
                co_yield(x, y)
                local ok2
                ok2, x, y = co_resume(co)
                if not ok2 then
                    util.report(traceback(co, tostring(x)), where)
                    return
                end
                if co_status(co) ~= 'suspended' then return end
            end
        end)
    end
end

local function invoke(fn, where, ...)
    local ok, err = xpcall(fn, traceback, ...)
    if not ok then util.report(err, where) end
end

-- Calls every handler of an Ashita event; after each, `sync` (if any) folds changes into state.
local function each(name, co, sync, ...)
    local list = handlers[name]
    if not list or #list == 0 then return end
    local copy = { unpack(list) }
    for i = 1, #copy do
        if co then invoke_co(copy[i].fn, name, ...) else invoke(copy[i].fn, name, ...) end
        if sync then sync() end
    end
end

------------------------------------------------------------------------------------------------
-- buffers for the *_raw fields
------------------------------------------------------------------------------------------------

local u8arr = ffi.typeof('uint8_t[?]')
local u8p = ffi.typeof('uint8_t*')

-- A buffer of at least n bytes holding s (kept alive by the event table).
local function buffer(s, n)
    local cap = math.max(n or 0, #s, 0x400)
    local b = u8arr(cap)
    ffi.copy(b, s, #s)
    return b
end

------------------------------------------------------------------------------------------------
-- event builders: xi event table -> Ashita event, and the changes back
------------------------------------------------------------------------------------------------

-- A string field with a raw twin (data_modified / data_modified_raw): the handler may change either.
local function raw_mt(fields)
    -- fields: raw key -> { string key, size? }
    return {
        __index = function(t, k)
            local f = fields[k]
            if not f then return nil end
            local s = rawget(t, f[1]) or ''
            local keep = buffer(s, 0)
            rawset(t, '__buf_' .. k, keep)
            rawset(t, '__was_' .. k, s)
            local p = ffi.cast(u8p, keep)
            rawset(t, k, p)
            return p
        end,
    }
end

local packet_mt = raw_mt({
    data_raw = { 'data' },
    data_modified_raw = { 'data_modified' },
    chunk_data_raw = { 'chunk_data' },
})

-- Folds a handler's change of a string field and its raw buffer: returns the current string.
local function fold(e, key, raw)
    local s = rawget(e, key)
    if type(s) == 'table' then s = util.bytes(s) end
    s = s ~= nil and tostring(s) or ''
    local buf = rawget(e, '__buf_' .. raw)
    local was = rawget(e, '__was_' .. raw)
    if buf ~= nil then
        if s ~= was then
            -- the string was replaced: the buffer follows it
            local n = math.min(#s, ffi.sizeof(buf))
            ffi.copy(buf, s, n)
        else
            -- writes through the pointer: the string follows them (same length)
            s = ffi.string(buf, #was)
        end
        rawset(e, '__was_' .. raw, s)
    end
    rawset(e, key, s)
    return s
end

local function packet(name)
    return {
        xi = name,
        co = true,
        run = function(xe)
            local e = setmetatable({
                id = xe.id,
                size = #(xe.data or ''),
                data = xe.data or '',
                data_modified = xe.modified or xe.data or '',
                chunk_size = #(xe.chunk or ''),
                chunk_data = xe.chunk or '',
                injected = xe.injected and true or false,
                blocked = xe.blocked and true or false,
            }, packet_mt)
            each(name, true, function() fold(e, 'data_modified', 'data_modified_raw') end, e)
            xe.modified = rawget(e, 'data_modified')
            if e.blocked then xe.blocked = true end
        end,
    }
end

local function text(name, indent)
    return {
        xi = name,
        co = true,
        run = function(xe)
            local e = {
                mode = xe.mode,
                message = xe.data or '',
                mode_modified = xe.mode_modified or xe.mode,
                message_modified = xe.modified or xe.data or '',
                injected = xe.injected and true or false,
                blocked = xe.blocked and true or false,
            }
            if indent then
                e.indent = false
                e.indent_modified = false
            end
            each(name, true, nil, e)
            local m = e.message_modified
            xe.modified = m ~= nil and tostring(m) or ''
            xe.mode_modified = tonumber(e.mode_modified) or xe.mode_modified
            if e.blocked then xe.blocked = true end
        end,
    }
end

local keystate = u8arr(256)

-- The first XInput pad as the game reads it (XINPUT_STATE), polled once a frame.
ffi.cdef[[ typedef struct { uint32_t dwPacketNumber; uint16_t wButtons; uint8_t bLeftTrigger, bRightTrigger;
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY; } xi_xinput_state_t; ]]
local pad_state = ffi.new('xi_xinput_state_t')
local pad_ptr = ffi.cast('uint8_t*', pad_state)
local pad_buttons, pad_packet = 0, 0
local function poll_pad()
    local buttons, lt, rt, lx, ly, rx, ry = xi.ashita_native.xpad(0)
    if not buttons then return nil end
    if buttons ~= pad_state.wButtons or lt ~= pad_state.bLeftTrigger or rt ~= pad_state.bRightTrigger or
        lx ~= pad_state.sThumbLX or ly ~= pad_state.sThumbLY or rx ~= pad_state.sThumbRX or ry ~= pad_state.sThumbRY then
        pad_packet = pad_packet + 1
    end
    pad_state.dwPacketNumber = pad_packet
    pad_state.wButtons, pad_state.bLeftTrigger, pad_state.bRightTrigger = buttons, lt, rt
    pad_state.sThumbLX, pad_state.sThumbLY, pad_state.sThumbRX, pad_state.sThumbRY = lx, ly, rx, ry
    return { ptr = pad_ptr, buttons = buttons }
end

local EVENTS = {
    load = { xi = 'load', co = true, run = function() each('load', true) end },
    unload = { xi = 'unload', run = function() each('unload', false) end },
    command = {
        xi = 'command',
        co = true,
        run = function(xe)
            local e = {
                mode = xe.mode,
                command = xe.data or '',
                injected = xe.injected and true or false,
                blocked = xe.blocked and true or false,
            }
            each('command', true, nil, e)
            if e.blocked then xe.blocked = true end
        end,
    },
    text_in = text('text_in', true),
    text_out = text('text_out', false),
    packet_in = packet('packet_in'),
    packet_out = packet('packet_out'),
    key = {
        xi = 'key',
        run = function(xe)
            local code = xe.key or 0
            local lparam = xe.flags
            if not lparam or lparam == 0 then
                -- repeat 1, scan code, extended, previous state / transition when released
                lparam = 1 + bit.lshift(bit.band(code, 0x7F), 16) + (bit.band(code, 0x80) ~= 0 and 0x1000000 or 0)
                if not xe.down then lparam = lparam + 0xC0000000 end
            end
            local e = { wparam = util.dik.vk(code), lparam = lparam, blocked = xe.blocked and true or false }
            each('key', true, nil, e)
            if e.blocked then xe.blocked = true end
        end,
    },
    key_data = {
        xi = 'key',
        run = function(xe)
            local e = { key = xe.key or 0, down = xe.down and true or false, blocked = xe.blocked and true or false }
            each('key_data', true, nil, e)
            if e.blocked then xe.blocked = true end
        end,
    },
    key_state = {
        xi = 'frame',
        run = function()
            local down = xi.input.key_down
            for i = 0, 255 do keystate[i] = down(i) and 0x80 or 0 end
            local p = ffi.cast(u8p, keystate)
            each('key_state', false, nil, { data = p, data_raw = p, size = 256 })
        end,
    },
    mouse = {
        xi = 'mouse',
        run = function(xe)
            local e = {
                message = xe.message or 0,
                x = xe.x or 0,
                y = xe.y or 0,
                delta = xe.delta or 0,
                blocked = xe.blocked and true or false,
            }
            each('mouse', true, nil, e)
            if e.blocked then xe.blocked = true end
        end,
    },
    d3d_beginscene = { xi = 'frame', run = function() each('d3d_beginscene', false, nil, true) end },
    d3d_present = { xi = 'present', run = function() each('d3d_present', false, nil, nil, nil, nil, nil) end },
    d3d_endscene = { xi = 'postrender', run = function() each('d3d_endscene', false, nil, true) end },
    plugin_event = {
        xi = 'message',
        run = function(xe)
            local to = xe.chunk or ''
            local name = to:match('^ashita%.plugin_event:(.*)$')
            if not name then return end
            local data = xe.data or ''
            local e = { name = name, data = data, size = #data, blocked = false }
            local keep = buffer(data, 0)
            e.data_raw = ffi.cast(u8p, keep)
            each('plugin_event', false, nil, e)
            if e.blocked then xe.blocked = true end
        end,
    },
    xinput_state = {
        xi = 'frame',
        run = function()
            local pad = poll_pad()
            if not pad then return end
            each('xinput_state', false, nil, { state = pad.ptr, state_raw = pad.ptr, injected = false, blocked = false })
        end,
    },
    xinput_button = {
        xi = 'frame',
        run = function()
            local pad = poll_pad()
            local now = pad and pad.buttons or 0
            local was = pad_buttons
            pad_buttons = now
            if now == was then return end
            for b = 0, 15 do
                local m = bit.lshift(1, b)
                if bit.band(now, m) ~= bit.band(was, m) then
                    each('xinput_button', true, nil,
                        { button = b, state = bit.band(now, m) ~= 0 and 1 or 0, injected = false, blocked = false })
                end
            end
        end,
    },
    d3d_dp = { never = true },
    d3d_dip = { never = true },
    dinput_button = { never = true },
    dinput_state = { never = true },
}

------------------------------------------------------------------------------------------------
-- register / unregister
------------------------------------------------------------------------------------------------

local logged = {}

function M.register(name, alias, fn)
    assert(type(name) == 'string', 'ashita.events.register: the event name must be a string')
    assert(type(fn) == 'function', 'ashita.events.register: the callback must be a function')
    name = name:lower()
    alias = tostring(alias)
    local ev = EVENTS[name]
    if not ev then
        error(('ashita.events.register: unknown event \'%s\''):format(name), 2)
    end
    if ev.never and not logged[name] then
        logged[name] = true
        xi.log('unsupported: event ' .. name .. ' (registered; never raised here)')
    end
    local list = handlers[name]
    if not list then
        list = {}
        handlers[name] = list
    end
    for i = 1, #list do
        if list[i].alias == alias then
            list[i].fn = fn
            return true
        end
    end
    list[#list + 1] = { alias = alias, fn = fn }
    if #list == 1 and ev.xi then
        xi.events.on(ev.xi, ev.run, 'ashita:' .. name)
    end
    return true
end

function M.unregister(name, alias)
    name = tostring(name):lower()
    alias = tostring(alias)
    local list = handlers[name]
    if not list then return false end
    for i = #list, 1, -1 do
        if list[i].alias == alias then
            table.remove(list, i)
            if #list == 0 and EVENTS[name] and EVENTS[name].xi then
                xi.events.off(EVENTS[name].xi, 'ashita:' .. name)
            end
            return true
        end
    end
    return false
end

-- IPluginManager:RaiseEvent: every addon's plugin_event (there are no native plugins here).
function M.raise_plugin_event(name, data)
    xi.addons.send('ashita.plugin_event:' .. tostring(name), util.bytes(data or ''))
end

M.handlers = handlers
return M
