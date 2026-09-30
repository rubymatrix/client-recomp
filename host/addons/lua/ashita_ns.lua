--[[
The ashita.* namespaces other than events (Ashita v4's annotations: addons/libs/annotations/ashita/):
fs, memory (+ assembler), misc, regex, tasks, time, bits; plus coroutine.kill and
table.make_literal.

    local ns = require('xi.ashita_ns')   -- { fs, memory, misc, regex, tasks, time, bits }

Addresses are the host's (xi.memory: numbers below 2^32 are guest addresses). Paths are
Windows-shaped on the way out and anything on the way in (xi.fs maps them).
--]]

local ffi = require('ffi')
local bit = require('bit')
local util = require('xi.ashita_util')
local native = xi.ashita_native

local M = {}

------------------------------------------------------------------------------------------------
-- ashita.fs
------------------------------------------------------------------------------------------------

local fs = { preferred_separator = '\\' }

local function host(p) return xi.fs.path(tostring(p or '')) end

function fs.create_directory(path)
    if xi.fs.is_dir(path) then return true end
    return xi.fs.mkdir(path) and xi.fs.is_dir(path) or false
end

local cwd

function fs.current_directory(path)
    if path ~= nil then
        if xi.fs.is_dir(path) then cwd = host(path) end
        return nil
    end
    return util.win_path(cwd or xi.info.root)
end

function fs.equivalent(a, b)
    local x, y = host(a):gsub('/+$', ''), host(b):gsub('/+$', '')
    return x:lower() == y:lower() and xi.fs.exists(a)
end

function fs.exists(path) return xi.fs.exists(path) end

local function regex_full(s, pattern)
    local ok, a, b = pcall(native.regex_find, s, '^(' .. pattern .. ')$')
    return ok and a ~= nil
end

function fs.get_directory(path, mask, subfolders)
    if not xi.fs.is_dir(path) then return nil end
    local dir = host(path):gsub('/*$', '/')
    local out = {}
    for _, name in ipairs(xi.fs.list(path)) do
        local is_dir = xi.fs.is_dir(dir .. name)
        if (not is_dir or subfolders) and (mask == nil or mask == '' or regex_full(name, mask)) then
            out[#out + 1] = name
        end
    end
    table.sort(out)
    return out
end

function fs.get_install_directory(lang, game_id)
    game_id = tonumber(game_id) or 1
    if game_id ~= 1 then return nil end -- only Final Fantasy XI is installed here
    -- the host path itself (no trailing slash): libs open files under it with the C library too
    return (xi.paths.game:gsub('/+$', ''))
end

function fs.normalize(path)
    local p = tostring(path):gsub('/', '\\'):gsub('\\\\+', '\\')
    local parts = {}
    for piece in p:gmatch('[^\\]+') do
        if piece == '..' then
            if #parts > 0 then parts[#parts] = nil end
        elseif piece ~= '.' then
            parts[#parts + 1] = piece
        end
    end
    local r = table.concat(parts, '\\')
    if p:sub(1, 1) == '\\' then r = '\\' .. r end
    if p:sub(-1) == '\\' then r = r .. '\\' end
    return r
end

function fs.remove(path)
    return os.remove(path) and true or false
end

function fs.rename(a, b)
    return os.rename(a, b) and true or false
end

function fs.size(path)
    local f = io.open(path, 'rb')
    if not f then return 0 end
    local n = f:seek('end') or 0
    f:close()
    return n
end

function fs.space(path)
    if not xi.fs.exists(path) then return nil end
    -- no statvfs from Lua: a large, plausible answer
    local big = 256 * 1024 * 1024 * 1024
    return { available = big, capacity = big * 2, free = big }
end

function fs.status(path)
    local exists = xi.fs.exists(path)
    if not exists then
        return { permissions = 0, exists = false, is_regular_file = false, is_directory = false, is_block_file = false,
            is_character_file = false, is_fifo = false, is_socket = false, is_symlink = false }
    end
    local dir = xi.fs.is_dir(path)
    return { permissions = 0x1FF, exists = true, is_regular_file = not dir, is_directory = dir, is_block_file = false,
        is_character_file = false, is_fifo = false, is_socket = false, is_symlink = false }
end

fs.create_dir = fs.create_directory
fs.current_dir = fs.current_directory
fs.equals = fs.equivalent
fs.get_dir = fs.get_directory
fs.get_install_dir = fs.get_install_directory

M.fs = util.partial(fs, 'ashita.fs')

------------------------------------------------------------------------------------------------
-- ashita.memory
------------------------------------------------------------------------------------------------

local mem = {}
local xm = xi.memory

local function module_name(name)
    if name == nil or name == 0 or name == '' then return 'FFXiMain.dll' end
    return tostring(name)
end

function mem.get_base(name) return (xm.module(module_name(name))) end
function mem.get_size(name)
    local _, size = xm.module(module_name(name))
    return size
end

function mem.protect(addr, size, protect) return xm.protect(addr, size, protect) end
function mem.unprotect(addr, size) return xm.unprotect(addr, size) end
function mem.alloc(size)
    local a = xm.alloc(tonumber(size) or 0)
    if not a or a == 0 then return nil end
    return a
end
function mem.dealloc(addr)
    xm.free(addr)
    return true
end

-- find(name|base, size, pattern, offset, usage): base 0 (or nil) is FFXiMain.dll, like a name
function mem.find(where, size, pattern, offset, usage)
    if where == nil or where == 0 then where = 'FFXiMain.dll' end
    if type(where) == 'string' then
        -- Ashita's "a.dll;b.dll" (or comma) lists: the first that has it
        for m in where:gmatch('[^;,]+') do
            local r = xm.find(m, size or 0, pattern, offset or 0, usage or 0)
            if r and r ~= 0 then return r end
        end
        return 0
    end
    return xm.find(where, size or 0, pattern, offset or 0, usage or 0)
end

for _, t in ipairs({ 'int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64', 'float', 'double' }) do
    local r, w = xm['read_' .. t], xm['write_' .. t]
    mem['read_' .. t] = function(addr) return r(addr) end
    mem['write_' .. t] = function(addr, v) w(addr, v) end
end

function mem.read_array(addr, size)
    size = tonumber(size) or 0
    if size <= 0 then return {} end
    local s = xm.read_bytes(addr, size)
    if not s then return nil end
    return { s:byte(1, -1) }
end

function mem.read_string(addr, size)
    return xm.read_string(addr, tonumber(size) or 0x10000)
end

function mem.read_literal(addr, size)
    return xm.read_bytes(addr, tonumber(size) or 0)
end

function mem.write_array(addr, val)
    if type(val) == 'string' then return xm.write_array(addr, val) end
    return xm.write_array(addr, util.bytes(val or {}))
end

function mem.write_string(addr, s, size)
    s = tostring(s or '')
    size = tonumber(size)
    if size then
        if #s >= size then
            xm.write_array(addr, s:sub(1, size))
            return
        end
        xm.write_string(addr, s, true)
        return
    end
    xm.write_string(addr, s, true)
end

-- The assembler writes and runs x86 code: the recompiled game never executes guest code bytes.
local asm = {}
for _, k in ipairs({ 'call', 'jmp', 'nop', 'assemble', 'cave', 'release' }) do
    asm[k] = util.stub('ashita.memory.assembler.' .. k)
end
mem.assembler = util.partial(asm, 'ashita.memory.assembler')

M.memory = util.partial(mem, 'ashita.memory')

------------------------------------------------------------------------------------------------
-- ashita.misc
------------------------------------------------------------------------------------------------

local misc = {}

function misc.get_clipboard() return native.clipboard_get() end
function misc.set_clipboard(s) return native.clipboard_set(tostring(s or '')) end
function misc.hide_console() end -- no console window here
function misc.show_console() end
misc.execute = util.stub('ashita.misc.execute') -- starting programs from addons: not here
function misc.open_url(url) xi.open_url(tostring(url)) end
function misc.play_sound(path) return native.play_sound(tostring(path)) end

M.misc = util.partial(misc, 'ashita.misc')

------------------------------------------------------------------------------------------------
-- ashita.regex (ECMAScript patterns, via ashita_native.c)
------------------------------------------------------------------------------------------------

local regex = {}
local find = native.regex_find

local function icase(flags) return flags ~= nil and bit.band(tonumber(flags) or 0, 1) ~= 0 end

-- every match: { { whole, group1, ... , start=, stop= }, ... }
local function all(s, pattern, flags)
    s = tostring(s)
    local out, init = {}, 1
    while init <= #s + 1 do
        local a, b, groups = find(s, pattern, init, icase(flags))
        if not a then break end
        groups.start, groups.stop = a, b
        out[#out + 1] = groups
        init = b >= a and b + 1 or a + 1
    end
    return out
end

function regex.search(s, pattern, flags)
    if s == nil then return nil end
    local m = all(s, pattern, flags)
    if #m == 0 then return nil end
    for _, g in ipairs(m) do g.start, g.stop = nil, nil end
    return m
end

function regex.match(s, pattern, flags)
    if s == nil then return nil end
    s = tostring(s)
    local a, b, groups = find(s, '^(' .. pattern .. ')$', 1, icase(flags))
    if not a then return nil end
    table.remove(groups, 2) -- the wrapper group
    return { groups }
end

local function expand(fmt, g, s)
    return (fmt:gsub('%$([%d&`\'%$]%d?)', function(k)
        if k == '$' then return '$' end
        if k == '&' then return g[1] end
        if k == '`' then return s:sub(1, g.start - 1) end
        if k == "'" then return s:sub(g.stop + 1) end
        local n = tonumber(k)
        if n and g[n + 1] then return g[n + 1] end
        -- $12 when there are fewer groups: $1 then '2'
        local one = tonumber(k:sub(1, 1))
        if one and g[one + 1] then return g[one + 1] .. k:sub(2) end
        return ''
    end))
end

function regex.replace(s, pattern, rep, flags)
    if s == nil then return nil end
    s = tostring(s)
    local m = all(s, pattern, flags)
    if #m == 0 then return s end
    local parts, pos = {}, 1
    for _, g in ipairs(m) do
        parts[#parts + 1] = s:sub(pos, g.start - 1)
        if type(rep) == 'function' then
            local r = rep({ unpack(g) })
            parts[#parts + 1] = r ~= nil and tostring(r) or ''
        else
            parts[#parts + 1] = expand(tostring(rep), g, s)
        end
        pos = g.stop + 1
    end
    parts[#parts + 1] = s:sub(pos)
    return table.concat(parts)
end

function regex.split(s, pattern, flags)
    if s == nil then return nil end
    s = tostring(s)
    local out, pos = {}, 1
    for _, g in ipairs(all(s, pattern, flags)) do
        if g.stop >= g.start then
            out[#out + 1] = s:sub(pos, g.start - 1)
            pos = g.stop + 1
        end
    end
    out[#out + 1] = s:sub(pos)
    return out
end

M.regex = util.partial(regex, 'ashita.regex')

------------------------------------------------------------------------------------------------
-- ashita.tasks, coroutine.kill
------------------------------------------------------------------------------------------------

local tasks = {}
local by_co = setmetatable({}, { __mode = 'k' })

local function track(t)
    by_co[t.co] = t
    return t.co
end

function tasks.once(delay, fn, ...)
    if type(delay) == 'function' then return track(xi.tasks.once(0, delay, fn, ...)) end
    return track(xi.tasks.once(tonumber(delay) or 0, fn, ...))
end

function tasks.oncef(delay, fn, ...)
    if type(delay) == 'function' then return track(xi.tasks.oncef(1, delay, fn, ...)) end
    return track(xi.tasks.oncef(math.max(1, tonumber(delay) or 1), fn, ...))
end

function tasks.repeating(delay, repeats, repeat_delay, fn, ...)
    return track(xi.tasks.repeating(tonumber(delay) or 0, tonumber(repeats) or 0, tonumber(repeat_delay) or 0, fn, ...))
end

function tasks.repeatingf(delay, repeats, repeat_delay, fn, ...)
    return track(xi.tasks.repeatingf(math.max(1, tonumber(delay) or 1), tonumber(repeats) or 0,
        math.max(1, tonumber(repeat_delay) or 1), fn, ...))
end

M.tasks = tasks -- Ashita documents this as its internal registry: left open

function coroutine.kill(co)
    local running = coroutine.running()
    co = co or running
    local t = by_co[co]
    if t then xi.tasks.cancel(t) end
    if co == running then
        -- stop here: a cancelled task is never resumed again
        if t == nil then error('coroutine.kill: not inside a task', 2) end
        coroutine.yield()
    end
end

------------------------------------------------------------------------------------------------
-- ashita.time
------------------------------------------------------------------------------------------------

local time = {}
local epoch_offset = os.time() - xi.clock() -- wall seconds at clock 0

function time.clock()
    local now = epoch_offset + xi.clock()
    return { s = math.floor(now), ms = math.floor(now * 1e3), micro = math.floor(now * 1e6), nano = math.floor(now * 1e9) }
end

local function systime(utc)
    local now = epoch_offset + xi.clock()
    local d = os.date(utc and '!*t' or '*t', math.floor(now))
    local ms = math.floor((now % 1) * 1000)
    return {
        day = d.day, dayofweek = d.wday - 1, hour = d.hour, minute = d.min, month = d.month, ms = ms, second = d.sec,
        year = d.year, d = d.day, wd = d.wday - 1, hh = d.hour, mm = d.min, m = d.month, ss = d.sec, y = d.year,
    }
end

function time.get_localtime() return systime(false) end
function time.get_systemtime() return systime(true) end
function time.get_tick() return math.floor(xi.clock() * 1000) % 4294967296 end
function time.get_tick64() return math.floor(xi.clock() * 1000) end

local function large(v)
    local hi = math.floor(v / 4294967296)
    local lo = v - hi * 4294967296
    return { quad_part = v, high_part = hi, low_part = lo, q = v, h = hi, l = lo }
end

function time.query_performance_counter() return large(math.floor(xi.clock() * 1e7)) end
function time.query_performance_frequency() return large(1e7) end

time.glt = time.get_localtime
time.gst = time.get_systemtime
time.qpc = time.query_performance_counter
time.qpf = time.query_performance_frequency
time.tick = time.get_tick

M.time = util.partial(time, 'ashita.time')

------------------------------------------------------------------------------------------------
-- ashita.bits
------------------------------------------------------------------------------------------------
-- unpack_be / pack_be: bits in the game's packet order (least significant bit of each byte first,
-- values little-endian across bytes); unpack_le / pack_le: most significant bit first.
-- (data, byte_offset, bit_offset, len) or (data, bit_offset, len); data: a uint8_t pointer (cdata,
-- userdata, an address), a string (unpack only) or a byte table (1-based, as :totable() makes).

local function accessor(data)
    local t = type(data)
    if t == 'table' then
        return function(i) return tonumber(data[i + 1]) or 0 end, function(i, v) data[i + 1] = v end
    elseif t == 'string' then
        return function(i) return data:byte(i + 1) or 0 end, function() error('ashita.bits: cannot pack into a string', 3) end
    end
    local p = util.ptr(data)
    if p == nil then error('ashita.bits: bad data', 3) end
    return function(i) return p[i] end, function(i, v) p[i] = v end
end

local function position(a, b, c)
    -- (byte, bit, len) or (bit, len)
    if c ~= nil then return (tonumber(a) or 0) * 8 + (tonumber(b) or 0), tonumber(c) or 0 end
    return tonumber(a) or 0, tonumber(b) or 0
end

local function unpack_bits(msb, data, a, b, c)
    local get = accessor(data)
    local pos, len = position(a, b, c)
    local v, mul = 0, 1
    for i = 0, len - 1 do
        local p = pos + i
        local byte = get(math.floor(p / 8))
        if msb then
            v = v * 2 + bit.band(bit.rshift(byte, 7 - p % 8), 1)
        else
            v = v + bit.band(bit.rshift(byte, p % 8), 1) * mul
            mul = mul * 2
        end
    end
    return v
end

local function pack_bits(msb, data, value, a, b, c)
    local get, set = accessor(data)
    local pos, len = position(a, b, c)
    value = tonumber(value) or 0
    for i = 0, len - 1 do
        local bitv
        if msb then
            bitv = math.floor(value / 2 ^ (len - 1 - i)) % 2
        else
            bitv = math.floor(value / 2 ^ i) % 2
        end
        local p = pos + i
        local idx = math.floor(p / 8)
        local shift = msb and (7 - p % 8) or (p % 8)
        local byte = get(idx)
        if bitv == 1 then byte = bit.bor(byte, bit.lshift(1, shift)) else byte = bit.band(byte, bit.bnot(bit.lshift(1, shift))) end
        set(idx, bit.band(byte, 0xFF))
    end
    return value
end

local bits = {
    unpack_be = function(data, a, b, c) return unpack_bits(false, data, a, b, c) end,
    unpack_le = function(data, a, b, c) return unpack_bits(true, data, a, b, c) end,
    pack_be = function(data, value, a, b, c) return pack_bits(false, data, value, a, b, c) end,
    pack_le = function(data, value, a, b, c) return pack_bits(true, data, value, a, b, c) end,
}

M.bits = util.partial(bits, 'ashita.bits')

------------------------------------------------------------------------------------------------
-- table.make_literal
------------------------------------------------------------------------------------------------

local function literal(v, seen)
    local t = type(v)
    if t == 'string' then return ('%q'):format(v) end
    if t == 'number' then
        if v ~= v then return '0/0' end
        if v == math.huge then return 'math.huge' end
        if v == -math.huge then return '-math.huge' end
        if v == math.floor(v) and math.abs(v) < 2 ^ 53 then return ('%d'):format(v) end
        return ('%.17g'):format(v)
    end
    if t == 'boolean' or t == 'nil' then return tostring(v) end
    if t ~= 'table' then return 'nil' end
    if seen[v] then error('table.make_literal: a table that contains itself', 3) end
    seen[v] = true
    local parts, n = {}, #v
    for i = 1, n do parts[#parts + 1] = literal(v[i], seen) end
    local keys = {}
    for k in pairs(v) do
        if not (type(k) == 'number' and k >= 1 and k <= n and k == math.floor(k)) then keys[#keys + 1] = k end
    end
    table.sort(keys, function(x, y) return tostring(x) < tostring(y) end)
    for _, k in ipairs(keys) do
        local key
        if type(k) == 'string' and k:match('^[%a_][%w_]*$') then key = k else key = '[' .. literal(k, seen) .. ']' end
        parts[#parts + 1] = key .. ' = ' .. literal(v[k], seen)
    end
    seen[v] = nil
    return '{ ' .. table.concat(parts, ', ') .. ' }'
end

function table.make_literal(v) return literal(v, {}) end

return M
