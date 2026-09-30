--[[
ThornyFFXI's gdifonts library (tHotBar, tCrossBar...) on the host's renderer (host/addons/gdifont_ffi.c)
and Direct3D (d3d_ffi.c). tests/addons/d3d8_ffi.py copies the library (not in this repository)
into gdifonts/ next to this file. Prints PASS/FAIL lines and "DONE <passed>/<total>".
--]]

package.path = xi.paths.ashita .. 'addons/libs/?.lua;' .. package.path
require('common')
local ffi = require('ffi')
local C = ffi.C

-- what an Ashita addon has, as much as gdifonts uses
AshitaCore = AshitaCore or { GetDirect3DDevice = function() return xi.d3d8_device() end }
ashita = ashita or {
    events = {
        register = function(name, key, fn) if name == 'd3d_present' then xi.events.on('present', fn, key) end end,
        unregister = function(name, key) if name == 'd3d_present' then xi.events.off('present', key) end end,
    },
    fs = { exists = function() return false end },
}

local passed, total = 0, 0
local function check(name, ok, why)
    total = total + 1
    if ok then
        passed = passed + 1
        print('PASS ' .. name)
    else
        print('FAIL ' .. name .. ': ' .. tostring(why))
    end
end
local function try(name, fn)
    local ok, err = pcall(fn)
    if not ok then check(name, false, err) end
end

local function pixel(tex, x, y)
    local _, lr = tex:LockRect(0, nil, 0)
    local v = ffi.cast('uint32_t*', lr.pBits)[y * (lr.Pitch / 4) + x]
    tex:UnlockRect(0)
    return v
end

local gdi
try('load', function()
    gdi = require('gdifonts.include')
    check('require gdifonts', gdi ~= nil)
end)

local font, rect
try('font', function()
    check('GetFontAvailable Arial', gdi:get_font_available('Arial') == true)
    check('GetFontAvailable missing', gdi:get_font_available('No Such Font Anywhere') == false)
    font = gdi:create_object({ font_height = 24, text = 'Hello\nWorld', outline_width = 2, font_flags = 1,
        gradient_style = 3, gradient_color = 0xFFFF0000, z_order = 1 }, false)
    local tex, r = font:get_texture()
    check('CreateTexture', tex ~= nil and r.right > 40 and r.bottom > 50, r and r.right .. 'x' .. r.bottom)
    local lit, clear = 0, 0
    local _, lr = tex:LockRect(0, nil, 0)
    local p = ffi.cast('uint32_t*', lr.pBits)
    for i = 0, (lr.Pitch / 4) * r.bottom - 1 do
        if bit.rshift(p[i], 24) > 200 then lit = lit + 1 elseif p[i] == 0 then clear = clear + 1 end
    end
    tex:UnlockRect(0)
    check('text pixels', lit > 100 and clear > 100, lit .. ' lit, ' .. clear .. ' clear')
end)

try('rect', function()
    rect = gdi:create_rect({ width = 50, height = 20, corner_rounding = 10, fill_color = 0xFF00FF00,
        outline_color = 0xFF000000, outline_width = 2 }, false)
    local tex, r = rect:get_texture()
    check('CreateRectTexture', tex ~= nil and r.right == 50 and r.bottom == 20, r and r.right)
    check('rect corner', pixel(tex, 0, 0) == 0, string.format('%08x', pixel(tex, 0, 0)))
    check('rect fill', pixel(tex, 25, 10) == 0xFF00FF00, string.format('%08x', pixel(tex, 25, 10)))
    check('rect outline', pixel(tex, 25, 0) == 0xFF000000, string.format('%08x', pixel(tex, 25, 0)))
end)

try('encoding', function()
    local enc = dofile(xi.info.path .. 'gdifonts/encoding.lua')
    local s = enc:UTF8_To_ShiftJIS('テスト abc')
    check('UTF8_To_ShiftJIS', s == '\131\101\131\88\131\103 abc', (s:gsub('.', function(c) return ('%02x'):format(c:byte()) end)))
    check('ShiftJIS_To_UTF8', enc:ShiftJIS_To_UTF8(s) == 'テスト abc')
end)

local frames = 0
xi.events.on('present', function()
    frames = frames + 1
    if frames == 2 then
        local _, v = xi.d3d8_stats()
        check('auto render', v >= 12, v) -- the text and the rect through gdifonts' own sprite
    end
end)

xi.tasks.oncef(4, function()
    try('destroy', function()
        gdi:destroy_interface()
        font, rect = nil, nil
        collectgarbage()
        check('destroy_interface', true)
    end)
    print(('DONE %d/%d'):format(passed, total))
end)
