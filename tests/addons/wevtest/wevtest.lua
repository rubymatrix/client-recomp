-- Test addon (kind xi) for host/addons/lua/windower_events.lua: prints every Windower event the
-- module raises as "EV <name> <args>" (tests/windower_events_test.sh compares them).
local wev = require('xi.windower_events')

local function ser(v, depth)
    depth = depth or 0
    local t = type(v)
    if t == 'string' then
        if v:find('[^\32-\126]') then return ('<%d bytes>'):format(#v) end
        return ('%q'):format(v)
    elseif t ~= 'table' then
        return tostring(v)
    end
    if depth > 4 then return '{...}' end
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b)
        if type(a) == type(b) then return a < b end
        return type(a) == 'number'
    end)
    local parts = {}
    for _, k in ipairs(keys) do
        parts[#parts + 1] = tostring(k) .. '=' .. ser(v[k], depth + 1)
    end
    return '{' .. table.concat(parts, ',') .. '}'
end
_G.ser = ser
_G.wev = wev

-- what the chunk handlers do: block, replace, or nothing
_G.block_in = {}
_G.replace_in = {}

local function raise(name, ...)
    local args = { n = select('#', ...), ... }
    local parts = {}
    for i = 1, args.n do
        local a = args[i]
        if name:find('chunk') and (i == 2 or i == 3) then
            parts[i] = ('<%d bytes>'):format(#a)
        else
            parts[i] = ser(a)
        end
    end
    io.write('EV ' .. name .. ' ' .. table.concat(parts, ' '), '\n')
    io.flush()
    if name == 'incoming chunk' then
        local id = args[1]
        if block_in[id] then return true end
        if replace_in[id] then return replace_in[id](args[3]) end
    end
end

local listening = {}
_G.listen = function(name, on) listening[name] = on ~= false end
for name in pairs(wev.provides) do listening[name] = true end

-- tests read the packet-fed state: the harness's game memory is there but empty
wev.memory = false
wev.install{ raise = raise, has = function(name) return listening[name] == true end }

-- a fixed clock for the time events
_G.fake_now = nil
local real_time = os.time
os.time = function(t)
    if t == nil and fake_now then return fake_now end
    return real_time(t)
end
