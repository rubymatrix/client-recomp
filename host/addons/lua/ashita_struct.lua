--[[
The `struct` library Ashita builds into its Lua (global `struct`, and require('struct')): binary
packing in the manner of Roberto Ierusalimschy's struct module, written here in Lua over ffi.

    struct.pack(fmt, ...) -> string
    struct.unpack(fmt, s[, pos]) -> values..., next position
    struct.size(fmt) -> bytes

Format: '<' little endian (the default), '>' big endian, '=' native (little), '!n' align to n
(default: none), ' ' ignored, 'x' a pad byte, 'b'/'B' int8/uint8, 'h'/'H' 16 bits, 'l'/'L' and
'T' 32 bits (Ashita is a 32-bit Windows program), 'i'/'I'[n] n-byte integers (default 4), 'f'
float, 'd' double, 's' a zero-terminated string, 'c'[n] n characters (c0: as long as the string
given / the previous number read).
--]]

local ffi = require('ffi')

local struct = {}

local fbuf = ffi.new('float[1]')
local dbuf = ffi.new('double[1]')

local function parse_num(fmt, i)
    local n = fmt:match('^%d+', i)
    if n then return tonumber(n), i + #n end
    return nil, i
end

local function sizeof(c, n)
    if c == 'b' or c == 'B' or c == 'x' then return 1 end
    if c == 'h' or c == 'H' then return 2 end
    if c == 'l' or c == 'L' or c == 'T' or c == 'f' then return 4 end
    if c == 'd' then return 8 end
    if c == 'i' or c == 'I' then return n or 4 end
    return 0
end

local function int_bytes(v, size, big)
    v = math.floor(tonumber(v) or 0)
    if v < 0 then v = v + 2 ^ (size * 8) end
    local t = {}
    for i = 1, size do
        t[i] = string.char(v % 256)
        v = math.floor(v / 256)
    end
    local s = table.concat(t)
    if big then s = s:reverse() end
    return s
end

local function bytes_int(s, pos, size, big, signed)
    local chunk = s:sub(pos, pos + size - 1)
    if #chunk < size then error('struct.unpack: data string too short', 3) end
    if big then chunk = chunk:reverse() end
    local v = 0
    for i = size, 1, -1 do v = v * 256 + chunk:byte(i) end
    if signed and v >= 2 ^ (size * 8 - 1) then v = v - 2 ^ (size * 8) end
    return v
end

local function align(pos, size, maxalign)
    if maxalign <= 1 or size <= 1 then return pos end
    local a = math.min(size, maxalign)
    local off = (pos - 1) % a
    if off ~= 0 then return pos + (a - off) end
    return pos
end

-- walks the format: fn(code, n, size) for each item
local function each(fmt, fn)
    local big, maxalign = false, 1
    local i = 1
    while i <= #fmt do
        local c = fmt:sub(i, i)
        i = i + 1
        local n
        if c == '<' or c == '=' then big = false
        elseif c == '>' then big = true
        elseif c == '!' then
            n, i = parse_num(fmt, i)
            maxalign = n or 8
        elseif c == ' ' then
            -- nothing
        else
            if c == 'i' or c == 'I' or c == 'c' then n, i = parse_num(fmt, i) end
            if not ('bBhHlLTiIfdsxc'):find(c, 1, true) then error(('struct: invalid format option \'%s\''):format(c), 3) end
            fn(c, n, big, maxalign)
        end
    end
end

function struct.pack(fmt, ...)
    local args = { n = select('#', ...), ... }
    local out, ai, pos = {}, 1, 1
    local function put(s) out[#out + 1] = s pos = pos + #s end
    each(fmt, function(c, n, big, maxalign)
        local size = sizeof(c, n)
        local aligned = align(pos, size, maxalign)
        if aligned > pos then put(('\0'):rep(aligned - pos)) end
        if c == 'x' then put('\0') return end
        local v = args[ai]
        ai = ai + 1
        if c == 'f' then
            fbuf[0] = tonumber(v) or 0
            local s = ffi.string(fbuf, 4)
            put(big and s:reverse() or s)
        elseif c == 'd' then
            dbuf[0] = tonumber(v) or 0
            local s = ffi.string(dbuf, 8)
            put(big and s:reverse() or s)
        elseif c == 's' then
            put(tostring(v) .. '\0')
        elseif c == 'c' then
            local s = tostring(v)
            if n == nil then n = 1 end
            if n == 0 then n = #s end
            put(s:sub(1, n) .. ('\0'):rep(math.max(0, n - #s)))
        else
            put(int_bytes(v, size, big))
        end
    end)
    return table.concat(out)
end

function struct.unpack(fmt, s, pos)
    s = tostring(s)
    pos = tonumber(pos) or 1
    local res, last = {}, nil
    each(fmt, function(c, n, big, maxalign)
        local size = sizeof(c, n)
        pos = align(pos, size, maxalign)
        if c == 'x' then pos = pos + 1 return end
        local v
        if c == 'f' then
            local chunk = s:sub(pos, pos + 3)
            if #chunk < 4 then error('struct.unpack: data string too short', 3) end
            if big then chunk = chunk:reverse() end
            ffi.copy(fbuf, chunk, 4)
            v = tonumber(fbuf[0])
            pos = pos + 4
        elseif c == 'd' then
            local chunk = s:sub(pos, pos + 7)
            if #chunk < 8 then error('struct.unpack: data string too short', 3) end
            if big then chunk = chunk:reverse() end
            ffi.copy(dbuf, chunk, 8)
            v = tonumber(dbuf[0])
            pos = pos + 8
        elseif c == 's' then
            local e = s:find('\0', pos, true)
            if not e then error('struct.unpack: unfinished string in data', 3) end
            v = s:sub(pos, e - 1)
            pos = e + 1
        elseif c == 'c' then
            if n == nil then n = 1 end
            if n == 0 then n = tonumber(last) or 0 end
            if pos + n - 1 > #s then error('struct.unpack: data string too short', 3) end
            v = s:sub(pos, pos + n - 1)
            pos = pos + n
        else
            local signed = c == 'b' or c == 'h' or c == 'l' or c == 'i'
            v = bytes_int(s, pos, size, big, signed)
            pos = pos + size
        end
        last = v
        res[#res + 1] = v
    end)
    res[#res + 1] = pos
    return unpack(res, 1, #res)
end

function struct.size(fmt)
    local pos = 1
    each(fmt, function(c, n, big, maxalign)
        if c == 's' or (c == 'c' and n == 0) then error('struct.size: options \'s\' and \'c0\' have no fixed size', 3) end
        local size = c == 'c' and (n or 1) or sizeof(c, n)
        pos = align(pos, size, maxalign) + size
    end)
    return pos - 1
end

return struct
