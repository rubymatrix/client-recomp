--[==[
windower.text and windower.prim over xi.ui's text objects and primitives: what Windower's texts
and images libraries drive.

    require('xi.windower_ui')(windower, helpers)

Objects are named by the addon (Windower's API takes names, ours ids). Font sizes are points in
Windower (96 dpi: 4/3 of a pixel), pixels in xi.ui. Positions are the game's pixels, which is
what get_windower_settings reports as the UI resolution. A right-justified text's x is its right
edge, as in Windower.
--]==]

local ui = xi.ui

return function(windower, h)
    local partial = h.partial

    local function argb(a, r, g, b)
        local function c(v) v = tonumber(v) or 0; return math.max(0, math.min(255, math.floor(v))) end
        return c(a) * 16777216 + c(r) * 65536 + c(g) * 256 + c(b)
    end

    ------------------------------------------------------------------------------ text

    local texts = {} -- name -> id

    local function tid(name)
        -- Windower ignores calls on names it doesn't have
        return texts[name] or 0
    end

    local text = { saved_texts = {} }

    function text.create(name)
        if texts[name] then ui.text_delete(texts[name]) end
        local id = ui.text_new()
        texts[name] = id
        ui.text_set(id, 'font', 'Arial')
        ui.text_set(id, 'size', 12 * 4 / 3)
        ui.text_set(id, 'color', argb(255, 255, 255, 255))
        ui.text_set(id, 'bg_visible', 0)
        ui.text_set(id, 'bg_color', argb(255, 0, 0, 0))
        ui.text_set(id, 'visible', 1)
    end

    function text.delete(name)
        local id = texts[name]
        if id then
            ui.text_delete(id)
            texts[name] = nil
        end
    end

    function text.set_text(name, s) ui.text_set(tid(name), 'text', tostring(s or '')) end
    function text.set_location(name, x, y)
        local id = tid(name)
        ui.text_set(id, 'x', tonumber(x) or 0)
        ui.text_set(id, 'y', tonumber(y) or 0)
    end
    function text.get_location(name)
        local id = tid(name)
        return ui.text_get(id, 'x'), ui.text_get(id, 'y')
    end
    function text.get_extents(name)
        local id = tid(name)
        return ui.text_get(id, 'extent_w'), ui.text_get(id, 'extent_h')
    end
    function text.set_visibility(name, v) ui.text_set(tid(name), 'visible', v and 1 or 0) end
    function text.set_color(name, a, r, g, b) ui.text_set(tid(name), 'color', argb(a, r, g, b)) end
    function text.set_font(name, font, ...) ui.text_set(tid(name), 'font', tostring(font or 'Arial')) end
    function text.set_font_size(name, pt) ui.text_set(tid(name), 'size', (tonumber(pt) or 12) * 4 / 3) end
    function text.set_bold(name, v) ui.text_set(tid(name), 'bold', v and 1 or 0) end
    function text.set_italic(name, v) ui.text_set(tid(name), 'italic', v and 1 or 0) end
    function text.set_bg_color(name, a, r, g, b) ui.text_set(tid(name), 'bg_color', argb(a, r, g, b)) end
    function text.set_bg_visibility(name, v) ui.text_set(tid(name), 'bg_visible', v and 1 or 0) end
    function text.set_bg_border_size(name, px) ui.text_set(tid(name), 'pad', tonumber(px) or 0) end
    function text.set_stroke_color(name, a, r, g, b) ui.text_set(tid(name), 'stroke_color', argb(a, r, g, b)) end
    function text.set_stroke_width(name, w) ui.text_set(tid(name), 'stroke', tonumber(w) or 0) end
    function text.set_right_justified(name, v) ui.text_set(tid(name), 'right', v and 1 or 0) end
    function text.set_bottom_justified(name, v) ui.text_set(tid(name), 'bottom', v and 1 or 0) end

    windower.text = partial(text, 'windower.text')

    ------------------------------------------------------------------------------ prim

    -- Windower primitives: a coloured rectangle, optionally textured. fit_to_texture makes the
    -- primitive the texture's size; repeat(x, y) tiles the texture x by y times over it.
    local prims = {} -- name -> { id, w, h, fit, tex, tw, th, rx, ry }

    local function pr(name)
        return prims[name] or { id = 0, w = 0, h = 0, tw = 0, th = 0, rx = 1, ry = 1 }
    end

    -- the xi primitive's size and texture mapping from the Windower state
    local function apply(p)
        local w, hgt = p.w, p.h
        if p.fit and p.tex then w, hgt = p.tw, p.th end
        local id = p.id
        if p.tex and (p.rx ~= 1 or p.ry ~= 1) and p.tw > 0 and p.th > 0 then
            -- uv 0..rx: the texture's pixels times the factor, scaled down to the size shown
            local vw, vh = p.tw * p.rx, p.th * p.ry
            ui.prim_set(id, 'fit', 0)
            ui.prim_set(id, 'width', vw)
            ui.prim_set(id, 'height', vh)
            ui.prim_set(id, 'scale_x', vw ~= 0 and w / vw or 1)
            ui.prim_set(id, 'scale_y', vh ~= 0 and hgt / vh or 1)
        else
            ui.prim_set(id, 'fit', 1)
            ui.prim_set(id, 'width', w)
            ui.prim_set(id, 'height', hgt)
            ui.prim_set(id, 'scale_x', 1)
            ui.prim_set(id, 'scale_y', 1)
        end
    end

    local prim = { saved_prims = {} }

    function prim.create(name)
        if prims[name] then prim.delete(name) end
        local id = ui.prim_new()
        prims[name] = { id = id, w = 0, h = 0, fit = false, tw = 0, th = 0, rx = 1, ry = 1 }
        ui.prim_set(id, 'visible', 1)
        ui.prim_set(id, 'color', argb(255, 255, 255, 255))
    end

    function prim.delete(name)
        local p = prims[name]
        if not p then return end
        ui.prim_delete(p.id)
        if p.tex then ui.texture_free(p.tex) end
        prims[name] = nil
    end

    function prim.set_color(name, a, r, g, b) ui.prim_set(pr(name).id, 'color', argb(a, r, g, b)) end
    function prim.set_visibility(name, v) ui.prim_set(pr(name).id, 'visible', v and 1 or 0) end
    function prim.set_position(name, x, y)
        local p = pr(name)
        ui.prim_set(p.id, 'x', tonumber(x) or 0)
        ui.prim_set(p.id, 'y', tonumber(y) or 0)
    end
    function prim.set_size(name, w, hgt)
        local p = pr(name)
        p.w, p.h = tonumber(w) or 0, tonumber(hgt) or 0
        apply(p)
    end
    function prim.set_fit_to_texture(name, fit)
        local p = pr(name)
        p.fit = fit and true or false
        apply(p)
    end
    function prim.set_repeat(name, x, y)
        local p = pr(name)
        p.rx, p.ry = tonumber(x) or 1, tonumber(y) or 1
        apply(p)
    end
    function prim.set_texture(name, path)
        local p = pr(name)
        if p.tex then
            ui.texture_free(p.tex)
            p.tex = nil
        end
        if path and path ~= '' then
            local id, w, hgt = ui.texture_file(xi.fs.path(tostring(path)))
            if id then
                p.tex, p.tw, p.th = id, w, hgt
            else
                xi.log(('windower.prim.set_texture: cannot load %s'):format(tostring(path)))
            end
        end
        ui.prim_set(p.id, 'texture', p.tex or 0)
        apply(p)
    end

    windower.prim = partial(prim, 'windower.prim')
end
