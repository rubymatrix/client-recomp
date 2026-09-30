-- The addon host's core, exercised through our own xi.* API (tests/addon_core_test.sh).
local ffi = require('ffi')

xi.events.on('load', function() print('coretest loaded, build ' .. xi.build) end)

-- commands: a host-style //command handled by the addon, others passed on
xi.events.on('command', function(e)
    if e.data:sub(1, 6) == '//core' then
        print('command: ' .. e.data .. ' injected=' .. tostring(e.injected))
        e.blocked = true
    elseif e.data == '//sleepy' then
        xi.tasks.spawn(function() print('sleeping'); coroutine.sleep(0.1); print('woke') end)
        return true
    end
end)

-- text_in: rewrite; text_out: append
xi.events.on('text_in', function(e)
    if e.data:find('secret') then e.modified = e.data:gsub('secret', '******') end
    if e.data:find('hide me') then e.blocked = true end
end)
xi.events.on('text_out', function(e)
    if e.data:sub(1, 1) ~= '/' then e.modified = e.data .. ' ~' end
end)

-- packets: log, block one id, rewrite another
xi.events.on('packet_in', function(e)
    print(('packet_in %03x size %d injected=%s'):format(e.id, #e.data, tostring(e.injected)))
    if e.id == 0x0AB then e.blocked = true end
    if e.id == 0x0AC then e.modified = e.data:sub(1, 4) .. 'ZZZZ' end
end)

-- memory: a pattern in the game's code, as a host address, read directly and through ffi
local at = xi.memory.find(0, 0, '81 EC 04 04 00 00 8B 8C 24 08 04 00 00', 0, 0)
print(('find: guest %08x, byte %02x, ffi %02x %02x'):format(xi.memory.guest(at), xi.memory.read_uint8(at),
    ffi.cast('uint8_t*', xi.memory.guest(at))[0], ffi.cast('uint8_t*', at)[1]))
print('ffi long is ' .. ffi.sizeof('long') .. ' bytes')

-- resources and text
print('item 4096: ' .. tostring(xi.res.item(4096).name_utf8.en))
print('sjis: ' .. xi.sjis_to_utf8(xi.utf8_to_sjis('こんにちは')))

-- the overlay's objects
local t = xi.ui.text_new()
xi.ui.text_set(t, 'text', 'hello')
xi.ui.text_set(t, 'size', 20)
print('text object: ' .. xi.ui.text_get(t, 'text') .. ' ' .. xi.ui.text_get(t, 'size'))

-- ImGui: an unbalanced window is closed by the host (logged against this addon)
local imgui = xi.ui.imgui()
local frames = 0
xi.events.on('present', function()
    frames = frames + 1
    imgui.Begin('coretest')
    imgui.Text('frame ' .. frames)
end)
xi.tasks.once(0.05, function() print('task ran, frames drawn > 0: ' .. tostring(frames > 0)) end)
ffi.cdef[[typedef struct FILE FILE; FILE* fopen(const char*, const char*); int fclose(FILE*);]]
-- a guest pointer chain: alloc a guest block A holding pointer to block B holding pointer to C (bytes)
local c = xi.memory.alloc(16); xi.memory.write_uint8(c + 0x3, 0xAB)
local b = xi.memory.alloc(8);  xi.memory.write_uint32(b, xi.memory.guest(c))
local a = xi.memory.alloc(8);  xi.memory.write_uint32(a, xi.memory.guest(b))
local pp = ffi.cast('uint8_t***', xi.memory.guest(a))
print('chain: ' .. string.format('%02x', pp[0][0][3]))
local p2 = ffi.cast('uint32_t**', a)
print('two: ' .. string.format('%08x', p2[0][0]) .. ' vs ' .. string.format('%08x', xi.memory.guest(c)))
local f = ffi.C.fopen(xi.paths.xi .. "addons\\coretest\\coretest.lua", 'rb')
print('fopen mapped: ' .. tostring(f ~= nil))
if f ~= nil then ffi.C.fclose(f) end
print('ffi.load winmm -> ' .. tostring(ffi.load('winmm.dll') == ffi.C))
