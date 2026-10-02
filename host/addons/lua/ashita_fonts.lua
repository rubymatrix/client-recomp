--[[
Ashita v4's IFontManager / IPrimitiveManager (and their IFontObject / IPrimitiveObject) over the
host's overlay objects (xi.ui.text_* / prim_* / texture_*, gui.cpp).

    local fonts = require('xi.ashita_fonts')   -- { FontManager, PrimitiveManager }

Ashita's libs/fonts.lua and libs/primitives.lua run on top unchanged: objects keep their methods
as raw fields of their metatable (the libs call rawget(getmetatable(obj), name)).

Mapping:
  font_family / font_height (points; 96 dpi pixels on screen), bold / italic (also from
  create_flags), color, color_outline (an outline is drawn when its alpha is non-zero or the
  Outlined draw flag is set), padding (pixels), position (drag with Shift like Ashita's),
  anchor (which corner of the box the position is: TopRight / BottomLeft / BottomRight),
  anchor_parent + parent (the corner of the parent's box the position is from), right_justified
  (the position is the right edge), auto_resize off + window_width / window_height (a fixed box),
  visible, locked, can_focus, text (Ashita's |cAARRGGBB| colour codes work).
  The background primitive: visible and color draw behind the text; its texture, borders and
  scale are kept but not drawn (the box is sized to the text).
Primitives: position, width, height, scale, color, texture (file, memory), texture offsets,
  visible, locked, can_focus. Borders are kept but not drawn.
--]]

local ffi = require('ffi')
local bit = require('bit')
local util = require('xi.ashita_util')
local ui = xi.ui

local M = {}

local function b(v) return v and true or false end

------------------------------------------------------------------------------------------------
-- RECT / SIZE (Ashita's userdata; plain tables here with the same fields)
------------------------------------------------------------------------------------------------

local RECT_mt = { __name = 'RECT' }
RECT_mt.__index = RECT_mt
local SIZE_mt = { __name = 'SIZE' }
SIZE_mt.__index = function(t, k)
    -- Ashita's SIZE is cx/cy; its annotations say x/y: both work
    if k == 'x' then return rawget(t, 'cx') end
    if k == 'y' then return rawget(t, 'cy') end
    return nil
end
SIZE_mt.__newindex = function(t, k, v)
    if k == 'x' then k = 'cx' elseif k == 'y' then k = 'cy' end
    rawset(t, k, v)
end

RECT = {
    new = function(l, t, r, bt) return setmetatable({ left = l or 0, top = t or 0, right = r or 0, bottom = bt or 0 }, RECT_mt) end,
    __type = { is = function(v) return type(v) == 'table' and getmetatable(v) == RECT_mt end },
}
SIZE = {
    new = function(cx, cy) return setmetatable({ cx = cx or 0, cy = cy or 0 }, SIZE_mt) end,
    __type = { is = function(v) return type(v) == 'table' and getmetatable(v) == SIZE_mt end },
}

------------------------------------------------------------------------------------------------
-- fonts
------------------------------------------------------------------------------------------------

local fonts = {}       -- alias (lower) -> object
local fonts_visible = true

local FONT = {}        -- IFontObject methods
local BG = {}          -- IPrimitiveObject methods of a font's background

local function tset(o, k, v) ui.text_set(o.id, k, v) end
local function tget(o, k) return ui.text_get(o.id, k) end

local function px(points) return (tonumber(points) or 10) * 96 / 72 end

local function font_style(o)
    local cf = o.create_flags
    tset(o, 'bold', (o.bold or bit.band(cf, 0x01) ~= 0) and 1 or 0)
    tset(o, 'italic', (o.italic or bit.band(cf, 0x02) ~= 0) and 1 or 0)
    local outline = bit.band(o.draw_flags, 0x10) ~= 0 or bit.rshift(util.u32(o.color_outline), 24) ~= 0
    tset(o, 'stroke', outline and 1 or 0)
    tset(o, 'stroke_color', util.u32(o.color_outline))
end

local function font_layout(o)
    local a = tonumber(o.anchor) or 0
    tset(o, 'right', (bit.band(a, 1) ~= 0 or o.right_justified) and 1 or 0)
    tset(o, 'bottom', bit.band(a, 2) ~= 0 and 1 or 0)
    tset(o, 'anchor', tonumber(o.anchor_parent) or 0)
    tset(o, 'parent', o.parent and o.parent.id or 0)
    if o.auto_resize then
        tset(o, 'width', 0)
        tset(o, 'height', 0)
    else
        tset(o, 'width', tonumber(o.window_width) or 0)
        tset(o, 'height', tonumber(o.window_height) or 0)
    end
end

local function font_visible(o)
    tset(o, 'visible', (o.visible and fonts_visible) and 1 or 0)
    tset(o, 'bg_visible', o.bg.visible and 1 or 0)
end

local IFontObject = util.class('IFontObject', FONT, { parent = true })
local IPrimitiveObjectBG = util.class('IPrimitiveObject', BG, { texture = true })

local function new_font(alias)
    local o = util.object(IFontObject, {
        id = ui.text_new(), alias = alias, visible = true, can_focus = true, locked = false, lockedz = false,
        is_dirty = true, window_width = 0, window_height = 0, font_file = '', font_family = 'Arial', font_height = 10,
        create_flags = 0, draw_flags = 0, bold = false, italic = false, right_justified = false,
        strike_through = false, underlined = false, color = 0xFFFFFFFF, color_outline = 0, padding = 0,
        auto_resize = true, anchor = 0, anchor_parent = 0, parent = nil,
    })
    o.bg = util.object(IPrimitiveObjectBG, {
        font = o, alias = alias .. '_bg', visible = false, color = 0x80000000, texture_offset_x = 0,
        texture_offset_y = 0, border_visible = false, border_color = 0, border_flags = 0,
        border_sizes = RECT.new(), can_focus = true, locked = false, lockedz = false, scale_x = 1, scale_y = 1,
        width = 0, height = 0, draw_flags = 0,
    })
    tset(o, 'font', o.font_family)
    tset(o, 'size', px(o.font_height))
    tset(o, 'color', util.u32(o.color))
    tset(o, 'bg_color', util.u32(o.bg.color))
    font_style(o)
    font_layout(o)
    font_visible(o)
    return o
end

function FONT:Render() end -- drawn by the overlay every frame
function FONT:GetTextSize(size)
    local w, h = tget(self, 'extent_w') or 0, tget(self, 'extent_h') or 0
    if type(size) == 'table' then
        size.cx, size.cy = w, h
    end
    return w, h
end
function FONT:GetRealPositionX()
    local x = tget(self, 'x') or 0
    if self.parent then
        local pw = tget(self.parent, 'extent_w') or 0
        x = x + self.parent:GetRealPositionX() + (bit.band(self.anchor_parent, 1) ~= 0 and pw or 0)
    end
    if bit.band(self.anchor, 1) ~= 0 or self.right_justified then x = x - (tget(self, 'extent_w') or 0) end
    return x
end
function FONT:GetRealPositionY()
    local y = tget(self, 'y') or 0
    if self.parent then
        local ph = tget(self.parent, 'extent_h') or 0
        y = y + self.parent:GetRealPositionY() + (bit.band(self.anchor_parent, 2) ~= 0 and ph or 0)
    end
    if bit.band(self.anchor, 2) ~= 0 then y = y - (tget(self, 'extent_h') or 0) end
    return y
end
function FONT:HitTest(x, y)
    if not self.visible then return false end
    local rx, ry = self:GetRealPositionX(), self:GetRealPositionY()
    local w, h = tget(self, 'extent_w') or 0, tget(self, 'extent_h') or 0
    return x >= rx and y >= ry and x < rx + w and y < ry + h
end
function FONT:GetParent() return self.parent end
function FONT:GetBackground() return self.bg end
function FONT:GetAlias() return self.alias end
function FONT:GetVisible() return self.visible end
function FONT:GetCanFocus() return self.can_focus end
function FONT:GetLocked() return self.locked end
function FONT:GetLockedZ() return self.lockedz end
function FONT:GetIsDirty() return self.is_dirty end
function FONT:GetWindowWidth() return self.window_width end
function FONT:GetWindowHeight() return self.window_height end
function FONT:GetFontFile() return self.font_file end
function FONT:GetFontFamily() return self.font_family end
function FONT:GetFontHeight() return self.font_height end
function FONT:GetCreateFlags() return self.create_flags end
function FONT:GetDrawFlags() return self.draw_flags end
function FONT:GetBold() return self.bold end
function FONT:GetItalic() return self.italic end
function FONT:GetRightJustified() return self.right_justified end
function FONT:GetStrikeThrough() return self.strike_through end
function FONT:GetUnderlined() return self.underlined end
function FONT:GetColor() return self.color end
function FONT:GetColorOutline() return self.color_outline end
function FONT:GetPadding() return self.padding end
function FONT:GetPositionX() return tget(self, 'x') or 0 end
function FONT:GetPositionY() return tget(self, 'y') or 0 end
function FONT:GetAutoResize() return self.auto_resize end
function FONT:GetAnchor() return self.anchor end
function FONT:GetAnchorParent() return self.anchor_parent end
function FONT:GetText() return tget(self, 'text') or '' end

function FONT:SetAlias(v)
    fonts[self.alias:lower()] = nil
    self.alias = tostring(v)
    fonts[self.alias:lower()] = self
end
function FONT:SetVisible(v) self.visible = b(v) font_visible(self) end
function FONT:SetCanFocus(v) self.can_focus = b(v) tset(self, 'can_focus', v and 1 or 0) end
function FONT:SetLocked(v) self.locked = b(v) tset(self, 'locked', v and 1 or 0) end
function FONT:SetLockedZ(v) self.lockedz = b(v) end
function FONT:SetIsDirty(v) self.is_dirty = b(v) end
function FONT:SetWindowWidth(v) self.window_width = tonumber(v) or 0 font_layout(self) end
function FONT:SetWindowHeight(v) self.window_height = tonumber(v) or 0 font_layout(self) end
function FONT:SetFontFile(v)
    self.font_file = v and tostring(v) or ''
    if self.font_file ~= '' then tset(self, 'font', self.font_file) end
end
function FONT:SetFontFamily(v)
    self.font_family = tostring(v or 'Arial')
    tset(self, 'font', self.font_family)
end
function FONT:SetFontHeight(v)
    self.font_height = tonumber(v) or 10
    tset(self, 'size', px(self.font_height))
end
function FONT:SetCreateFlags(v) self.create_flags = tonumber(v) or 0 font_style(self) end
function FONT:SetDrawFlags(v)
    self.draw_flags = tonumber(v) or 0
    if bit.band(self.draw_flags, 0x08) ~= 0 then self.right_justified = true end
    font_style(self)
    font_layout(self)
end
function FONT:SetBold(v) self.bold = b(v) font_style(self) end
function FONT:SetItalic(v) self.italic = b(v) font_style(self) end
function FONT:SetRightJustified(v) self.right_justified = b(v) font_layout(self) end
function FONT:SetStrikeThrough(v) self.strike_through = b(v) end
function FONT:SetUnderlined(v) self.underlined = b(v) end
function FONT:SetColor(v) self.color = util.u32(v) tset(self, 'color', self.color) end
function FONT:SetColorOutline(v) self.color_outline = util.u32(v) font_style(self) end
function FONT:SetPadding(v)
    self.padding = tonumber(v) or 0
    tset(self, 'pad', self.padding)
end
function FONT:SetPositionX(v) tset(self, 'x', tonumber(v) or 0) end
function FONT:SetPositionY(v) tset(self, 'y', tonumber(v) or 0) end
function FONT:SetAutoResize(v) self.auto_resize = b(v) font_layout(self) end
function FONT:SetAnchor(v) self.anchor = tonumber(v) or 0 font_layout(self) end
function FONT:SetAnchorParent(v) self.anchor_parent = tonumber(v) or 0 font_layout(self) end
function FONT:SetText(v) tset(self, 'text', v ~= nil and tostring(v) or '') end
function FONT:SetParent(p)
    if p ~= nil and getmetatable(p) ~= IFontObject then p = nil end
    self.parent = p
    font_layout(self)
end

-- the background primitive of a font
function BG:Render() end
function BG:SetTextureFromFile(file)
    self.texture = file
    return file ~= nil and xi.fs.exists(file)
end
function BG:SetTextureFromMemory() return false end
function BG:SetTextureFromResource() return false end
function BG:SetTextureFromResourceCache() return false end
function BG:SetTextureFromTexture() return false end
function BG:HitTest(x, y) return self.font:HitTest(x, y) end
function BG:GetAlias() return self.alias end
function BG:GetTextureOffsetX() return self.texture_offset_x end
function BG:GetTextureOffsetY() return self.texture_offset_y end
function BG:GetBorderVisible() return self.border_visible end
function BG:GetBorderColor() return self.border_color end
function BG:GetBorderFlags() return self.border_flags end
function BG:GetBorderSizes() return self.border_sizes end
function BG:GetVisible() return self.visible end
function BG:GetPositionX() return self.font:GetRealPositionX() end
function BG:GetPositionY() return self.font:GetRealPositionY() end
function BG:GetCanFocus() return self.can_focus end
function BG:GetLocked() return self.locked end
function BG:GetLockedZ() return self.lockedz end
function BG:GetScaleX() return self.scale_x end
function BG:GetScaleY() return self.scale_y end
function BG:GetWidth() return tget(self.font, 'extent_w') or self.width end
function BG:GetHeight() return tget(self.font, 'extent_h') or self.height end
function BG:GetDrawFlags() return self.draw_flags end
function BG:GetColor() return self.color end
function BG:SetAlias(v) self.alias = tostring(v) end
function BG:SetTextureOffsetX(v) self.texture_offset_x = tonumber(v) or 0 end
function BG:SetTextureOffsetY(v) self.texture_offset_y = tonumber(v) or 0 end
function BG:SetBorderVisible(v) self.border_visible = b(v) end
function BG:SetBorderColor(v) self.border_color = util.u32(v) end
function BG:SetBorderFlags(v) self.border_flags = tonumber(v) or 0 end
function BG:SetBorderSizes(r) if type(r) == 'table' then self.border_sizes = r end end
function BG:SetVisible(v) self.visible = b(v) font_visible(self.font) end
function BG:SetPositionX(v) end -- the background follows its font
function BG:SetPositionY(v) end
function BG:SetCanFocus(v) self.can_focus = b(v) end
function BG:SetLocked(v) self.locked = b(v) end
function BG:SetLockedZ(v) self.lockedz = b(v) end
function BG:SetScaleX(v) self.scale_x = tonumber(v) or 1 end
function BG:SetScaleY(v) self.scale_y = tonumber(v) or 1 end
function BG:SetWidth(v) self.width = tonumber(v) or 0 end
function BG:SetHeight(v) self.height = tonumber(v) or 0 end
function BG:SetDrawFlags(v) self.draw_flags = tonumber(v) or 0 end
function BG:SetColor(v) self.color = util.u32(v) tset(self.font, 'bg_color', self.color) end

local FM = {}
function FM:Create(alias)
    alias = tostring(alias)
    local k = alias:lower()
    if fonts[k] then return nil end -- Ashita: the alias is taken
    local o = new_font(alias)
    fonts[k] = o
    return o
end
function FM:Get(alias) return fonts[tostring(alias):lower()] end
function FM:Delete(alias)
    local k = tostring(alias):lower()
    local o = fonts[k]
    if not o then return end
    fonts[k] = nil
    for _, f in pairs(fonts) do
        if f.parent == o then f:SetParent(nil) end
    end
    ui.text_delete(o.id)
end
function FM:GetFocusedObject() return nil end
function FM:SetFocusedObject(alias) return fonts[tostring(alias):lower()] ~= nil end
function FM:GetVisible() return fonts_visible end
function FM:SetVisible(v)
    fonts_visible = b(v)
    for _, o in pairs(fonts) do font_visible(o) end
end

M.FontManager = util.object(util.class('IFontManager', FM))

------------------------------------------------------------------------------------------------
-- primitives
------------------------------------------------------------------------------------------------

local prims = {}
local prims_visible = true
local PRIM = {}
local IPrimitiveObject = util.class('IPrimitiveObject', PRIM, { tex = true })

local function pset(o, k, v) ui.prim_set(o.id, k, v) end
local function pget(o, k) return ui.prim_get(o.id, k) end

local function prim_visible(o) pset(o, 'visible', (o.visible and prims_visible) and 1 or 0) end

local function set_texture(o, id, w, h)
    if o.tex then ui.texture_free(o.tex) end
    o.tex = id
    pset(o, 'texture', id or 0)
    if id and (tonumber(pget(o, 'width')) or 0) == 0 and (tonumber(pget(o, 'height')) or 0) == 0 then
        pset(o, 'width', w or 0)
        pset(o, 'height', h or 0)
    end
    return id ~= nil
end

function PRIM:Render() end
function PRIM:SetTextureFromFile(file)
    if file == nil or file == '' then return set_texture(self, nil) end
    local id, w, h = ui.texture_file(tostring(file))
    return set_texture(self, id, w, h)
end
function PRIM:SetTextureFromMemory(data, size, color_key)
    if type(data) ~= 'string' then
        local p = util.ptr(data)
        if not p then return false end
        data = ffi.string(p, tonumber(size) or 0)
    elseif size then
        data = data:sub(1, tonumber(size))
    end
    local id, w, h = ui.texture_memory(data)
    return set_texture(self, id, w, h)
end
function PRIM:SetTextureFromResource(module_name, res_name)
    return util.unsupported('IPrimitiveObject.SetTextureFromResource', 2)
end
function PRIM:SetTextureFromResourceCache(name)
    -- Ashita's resource cache: files under <install>/resources/
    local root = xi.info.root .. 'resources/'
    for _, ext in ipairs({ '', '.png', '.bmp', '.jpg', '.tga' }) do
        local p = root .. tostring(name) .. ext
        if xi.fs.exists(p) and not xi.fs.is_dir(p) then return self:SetTextureFromFile(p) end
    end
    return false
end
function PRIM:SetTextureFromTexture(texture, width, height)
    return util.unsupported('IPrimitiveObject.SetTextureFromTexture', 2)
end
function PRIM:HitTest(x, y)
    if not self.visible then return false end
    local px_, py_ = pget(self, 'x') or 0, pget(self, 'y') or 0
    local w = (pget(self, 'width') or 0) * (pget(self, 'scale_x') or 1)
    local h = (pget(self, 'height') or 0) * (pget(self, 'scale_y') or 1)
    return x >= px_ and y >= py_ and x < px_ + w and y < py_ + h
end
function PRIM:GetAlias() return self.alias end
function PRIM:GetTextureOffsetX() return pget(self, 'tex_x') or 0 end
function PRIM:GetTextureOffsetY() return pget(self, 'tex_y') or 0 end
function PRIM:GetBorderVisible() return self.border_visible end
function PRIM:GetBorderColor() return self.border_color end
function PRIM:GetBorderFlags() return self.border_flags end
function PRIM:GetBorderSizes() return self.border_sizes end
function PRIM:GetVisible() return self.visible end
function PRIM:GetPositionX() return pget(self, 'x') or 0 end
function PRIM:GetPositionY() return pget(self, 'y') or 0 end
function PRIM:GetCanFocus() return self.can_focus end
function PRIM:GetLocked() return self.locked end
function PRIM:GetLockedZ() return self.lockedz end
function PRIM:GetScaleX() return pget(self, 'scale_x') or 1 end
function PRIM:GetScaleY() return pget(self, 'scale_y') or 1 end
function PRIM:GetWidth() return pget(self, 'width') or 0 end
function PRIM:GetHeight() return pget(self, 'height') or 0 end
function PRIM:GetDrawFlags() return self.draw_flags end
function PRIM:GetColor() return self.color end
function PRIM:SetAlias(v)
    prims[self.alias:lower()] = nil
    self.alias = tostring(v)
    prims[self.alias:lower()] = self
end
function PRIM:SetTextureOffsetX(v) pset(self, 'tex_x', tonumber(v) or 0) end
function PRIM:SetTextureOffsetY(v) pset(self, 'tex_y', tonumber(v) or 0) end
function PRIM:SetBorderVisible(v) self.border_visible = b(v) end
function PRIM:SetBorderColor(v) self.border_color = util.u32(v) end
function PRIM:SetBorderFlags(v) self.border_flags = tonumber(v) or 0 end
function PRIM:SetBorderSizes(r) if type(r) == 'table' then self.border_sizes = r end end
function PRIM:SetVisible(v) self.visible = b(v) prim_visible(self) end
function PRIM:SetPositionX(v) pset(self, 'x', tonumber(v) or 0) end
function PRIM:SetPositionY(v) pset(self, 'y', tonumber(v) or 0) end
function PRIM:SetCanFocus(v) self.can_focus = b(v) pset(self, 'can_focus', v and 1 or 0) end
function PRIM:SetLocked(v) self.locked = b(v) pset(self, 'locked', v and 1 or 0) end
function PRIM:SetLockedZ(v) self.lockedz = b(v) end
function PRIM:SetScaleX(v) pset(self, 'scale_x', tonumber(v) or 1) end
function PRIM:SetScaleY(v) pset(self, 'scale_y', tonumber(v) or 1) end
function PRIM:SetWidth(v) pset(self, 'width', tonumber(v) or 0) end
function PRIM:SetHeight(v) pset(self, 'height', tonumber(v) or 0) end
function PRIM:SetDrawFlags(v) self.draw_flags = tonumber(v) or 0 end
function PRIM:SetColor(v) self.color = util.u32(v) pset(self, 'color', self.color) end

local PM = {}
function PM:Create(alias)
    alias = tostring(alias)
    local k = alias:lower()
    if prims[k] then return nil end
    local o = util.object(IPrimitiveObject, {
        id = ui.prim_new(), alias = alias, visible = true, can_focus = true, locked = false, lockedz = false,
        color = 0xFFFFFFFF, border_visible = false, border_color = 0, border_flags = 0, border_sizes = RECT.new(),
        draw_flags = 0,
    })
    pset(o, 'color', o.color)
    pset(o, 'fit', 1)
    prim_visible(o)
    prims[k] = o
    return o
end
function PM:Get(alias) return prims[tostring(alias):lower()] end
function PM:Delete(alias)
    local k = tostring(alias):lower()
    local o = prims[k]
    if not o then return end
    prims[k] = nil
    if o.tex then ui.texture_free(o.tex) end
    ui.prim_delete(o.id)
end
function PM:GetFocusedObject() return nil end
function PM:SetFocusedObject(alias) return prims[tostring(alias):lower()] ~= nil end
function PM:GetVisible() return prims_visible end
function PM:SetVisible(v)
    prims_visible = b(v)
    for _, o in pairs(prims) do prim_visible(o) end
end

M.PrimitiveManager = util.object(util.class('IPrimitiveManager', PM))

return M
