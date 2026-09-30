--[[
Shared helpers for the Ashita v4 layer (ashita.lua and its ashita_* modules).

    local util = require('xi.ashita_util')

  util.unsupported(name)         logs "unsupported: <name>" (once per name) and raises
                                 "<name> is not supported yet" at the caller
  util.class(name, methods, nils) a metatable holding `methods` as raw fields (Ashita's libs look
                                 methods up with rawget(getmetatable(obj), k)); any other string key
                                 raises through unsupported(), except keys in `nils` (feature tests)
  util.object(mt[, state])        setmetatable(state or {}, mt)
  util.partial(tbl, name, nils)   the same for a plain namespace table (ashita.memory, ...)
  util.win_path(host_path)        '/a/b/' -> '\a\b\' (Ashita hands out Windows-shaped paths)
  util.u32(v)                     a colour or flag number as an unsigned 32-bit value
  util.bytes(tbl_or_str)          a byte table (1-based) or string as a string
  util.ptr(v)                     an ffi uint8_t* for cdata, lightuserdata or an address
  util.report(err, where)         an error from an addon's callback, to the chat log and the log
  util.dik / util.vk              DirectInput and virtual-key tables (names, conversions)
--]]

local ffi = require('ffi')
local bit = require('bit')

local util = {}

local seen = {}

function util.unsupported(name, level)
    if not seen[name] then
        seen[name] = true
        xi.log('unsupported: ' .. name)
    end
    error(name .. ' is not supported yet', (level or 2) + 1)
end

-- keys the Lua runtime itself probes on any value (never an addon's call)
local quiet = { __index = true, __newindex = true, __call = true, __tostring = true, __len = true, __gc = true,
    __metatable = true, __name = true, __type = true, __eq = true, __mode = true, __close = true, __pairs = true,
    __ipairs = true, __concat = true }

function util.class(name, methods, nils)
    -- the methods table itself: methods defined after this call are found too
    local mt = methods or {}
    mt.__index = function(_, k)
        local v = rawget(mt, k)
        if v ~= nil then return v end
        if type(k) ~= 'string' or quiet[k] or (nils and nils[k]) then return nil end
        return util.unsupported(name .. '.' .. k, 2)
    end
    mt.__tostring = function(t) return name end
    mt.__name = name
    return mt
end

function util.object(mt, state)
    return setmetatable(state or {}, mt)
end

function util.partial(tbl, name, nils)
    return setmetatable(tbl, {
        __index = function(_, k)
            if type(k) ~= 'string' or quiet[k] or (nils and nils[k]) then return nil end
            return util.unsupported(name .. '.' .. k, 2)
        end,
    })
end

-- a stub that exists (so feature tests pass) but fails loudly when called
function util.stub(name)
    return function() return util.unsupported(name, 2) end
end

function util.win_path(p)
    return (tostring(p):gsub('/', '\\'))
end

function util.u32(v)
    v = tonumber(v) or 0
    if v < 0 then v = v % 4294967296 end
    if v >= 4294967296 then v = v % 4294967296 end
    return v
end

function util.bytes(v)
    if type(v) == 'string' then return v end
    if type(v) ~= 'table' then return '' end
    local n = #v
    local parts = {}
    for i = 1, n do parts[i] = string.char(bit.band(tonumber(v[i]) or 0, 0xFF)) end
    return table.concat(parts)
end

local u8p = ffi.typeof('uint8_t*')

function util.ptr(v)
    local t = type(v)
    if t == 'cdata' or t == 'userdata' then return ffi.cast(u8p, v) end
    if t == 'number' then return ffi.cast(u8p, v) end -- guest addresses get the base (xi.lua)
    return nil
end

function util.report(err, where)
    xi.error(tostring(err), where)
end

------------------------------------------------------------------------------------------------
-- keys: DirectInput codes, their names (DIK_ without the prefix) and Windows virtual keys
------------------------------------------------------------------------------------------------

local DIK = {
    [0x01] = { 'ESCAPE', 0x1B }, [0x02] = { '1', 0x31 }, [0x03] = { '2', 0x32 }, [0x04] = { '3', 0x33 },
    [0x05] = { '4', 0x34 }, [0x06] = { '5', 0x35 }, [0x07] = { '6', 0x36 }, [0x08] = { '7', 0x37 },
    [0x09] = { '8', 0x38 }, [0x0A] = { '9', 0x39 }, [0x0B] = { '0', 0x30 }, [0x0C] = { 'MINUS', 0xBD },
    [0x0D] = { 'EQUALS', 0xBB }, [0x0E] = { 'BACK', 0x08 }, [0x0F] = { 'TAB', 0x09 }, [0x10] = { 'Q', 0x51 },
    [0x11] = { 'W', 0x57 }, [0x12] = { 'E', 0x45 }, [0x13] = { 'R', 0x52 }, [0x14] = { 'T', 0x54 },
    [0x15] = { 'Y', 0x59 }, [0x16] = { 'U', 0x55 }, [0x17] = { 'I', 0x49 }, [0x18] = { 'O', 0x4F },
    [0x19] = { 'P', 0x50 }, [0x1A] = { 'LBRACKET', 0xDB }, [0x1B] = { 'RBRACKET', 0xDD }, [0x1C] = { 'RETURN', 0x0D },
    [0x1D] = { 'LCONTROL', 0x11 }, [0x1E] = { 'A', 0x41 }, [0x1F] = { 'S', 0x53 }, [0x20] = { 'D', 0x44 },
    [0x21] = { 'F', 0x46 }, [0x22] = { 'G', 0x47 }, [0x23] = { 'H', 0x48 }, [0x24] = { 'J', 0x4A },
    [0x25] = { 'K', 0x4B }, [0x26] = { 'L', 0x4C }, [0x27] = { 'SEMICOLON', 0xBA }, [0x28] = { 'APOSTROPHE', 0xDE },
    [0x29] = { 'GRAVE', 0xC0 }, [0x2A] = { 'LSHIFT', 0x10 }, [0x2B] = { 'BACKSLASH', 0xDC }, [0x2C] = { 'Z', 0x5A },
    [0x2D] = { 'X', 0x58 }, [0x2E] = { 'C', 0x43 }, [0x2F] = { 'V', 0x56 }, [0x30] = { 'B', 0x42 },
    [0x31] = { 'N', 0x4E }, [0x32] = { 'M', 0x4D }, [0x33] = { 'COMMA', 0xBC }, [0x34] = { 'PERIOD', 0xBE },
    [0x35] = { 'SLASH', 0xBF }, [0x36] = { 'RSHIFT', 0x10 }, [0x37] = { 'MULTIPLY', 0x6A }, [0x38] = { 'LMENU', 0x12 },
    [0x39] = { 'SPACE', 0x20 }, [0x3A] = { 'CAPITAL', 0x14 }, [0x3B] = { 'F1', 0x70 }, [0x3C] = { 'F2', 0x71 },
    [0x3D] = { 'F3', 0x72 }, [0x3E] = { 'F4', 0x73 }, [0x3F] = { 'F5', 0x74 }, [0x40] = { 'F6', 0x75 },
    [0x41] = { 'F7', 0x76 }, [0x42] = { 'F8', 0x77 }, [0x43] = { 'F9', 0x78 }, [0x44] = { 'F10', 0x79 },
    [0x45] = { 'NUMLOCK', 0x90 }, [0x46] = { 'SCROLL', 0x91 }, [0x47] = { 'NUMPAD7', 0x67 }, [0x48] = { 'NUMPAD8', 0x68 },
    [0x49] = { 'NUMPAD9', 0x69 }, [0x4A] = { 'SUBTRACT', 0x6D }, [0x4B] = { 'NUMPAD4', 0x64 }, [0x4C] = { 'NUMPAD5', 0x65 },
    [0x4D] = { 'NUMPAD6', 0x66 }, [0x4E] = { 'ADD', 0x6B }, [0x4F] = { 'NUMPAD1', 0x61 }, [0x50] = { 'NUMPAD2', 0x62 },
    [0x51] = { 'NUMPAD3', 0x63 }, [0x52] = { 'NUMPAD0', 0x60 }, [0x53] = { 'DECIMAL', 0x6E }, [0x57] = { 'F11', 0x7A },
    [0x58] = { 'F12', 0x7B }, [0x9C] = { 'NUMPADENTER', 0x0D }, [0x9D] = { 'RCONTROL', 0x11 }, [0xB5] = { 'DIVIDE', 0x6F },
    [0xB7] = { 'SYSRQ', 0x2C }, [0xB8] = { 'RMENU', 0x12 }, [0xC5] = { 'PAUSE', 0x13 }, [0xC7] = { 'HOME', 0x24 },
    [0xC8] = { 'UP', 0x26 }, [0xC9] = { 'PRIOR', 0x21 }, [0xCB] = { 'LEFT', 0x25 }, [0xCD] = { 'RIGHT', 0x27 },
    [0xCF] = { 'END', 0x23 }, [0xD0] = { 'DOWN', 0x28 }, [0xD1] = { 'NEXT', 0x22 }, [0xD2] = { 'INSERT', 0x2D },
    [0xD3] = { 'DELETE', 0x2E }, [0xDB] = { 'LWIN', 0x5B }, [0xDC] = { 'RWIN', 0x5C }, [0xDD] = { 'APPS', 0x5D },
}

-- other spellings people bind with
local ALIASES = {
    ESC = 0x01, BACKSPACE = 0x0E, ENTER = 0x1C, LCTRL = 0x1D, RCTRL = 0x9D, LALT = 0x38, RALT = 0xB8,
    CAPSLOCK = 0x3A, SCROLLLOCK = 0x46, PAGEUP = 0xC9, PAGEDOWN = 0xD1, ['NUMPAD*'] = 0x37, ['NUMPAD-'] = 0x4A,
    ['NUMPAD+'] = 0x4E, ['NUMPAD.'] = 0x53, ['NUMPAD/'] = 0xB5, ['-'] = 0x0C, ['='] = 0x0D, ['['] = 0x1A,
    [']'] = 0x1B, [';'] = 0x27, ["'"] = 0x28, ['`'] = 0x29, ['\\'] = 0x2B, [','] = 0x33, ['.'] = 0x34, ['/'] = 0x35,
}

local by_name, by_vk = {}, {}
for code, v in pairs(DIK) do
    by_name[v[1]] = code
    if not by_vk[v[2]] or code < by_vk[v[2]] then by_vk[v[2]] = code end
end
for k, code in pairs(ALIASES) do by_name[k] = code end
-- the left/right virtual keys map to their left scan code
by_vk[0xA0], by_vk[0xA1], by_vk[0xA2], by_vk[0xA3], by_vk[0xA4], by_vk[0xA5] = 0x2A, 0x36, 0x1D, 0x9D, 0x38, 0xB8

util.dik = {
    name = function(code) local v = DIK[tonumber(code) or -1] return v and v[1] or nil end,
    code = function(name) return by_name[tostring(name):upper()] end,
    vk = function(code) local v = DIK[tonumber(code) or -1] return v and v[2] or 0 end,
    from_vk = function(vk) return by_vk[tonumber(vk) or -1] or 0 end,
}

return util
