/* Headless test of the IGuiManager Lua binding (host/addons/gui_lua.cpp).
 *
 * Build and run (from the repo root; needs build/third_party/{imgui,luajit}.a from tools/thirdparty.py):
 *
 *   clang++ -std=c++17 -O1 -g -Ithird_party/imgui -Ithird_party/luajit/src -Ihost/addons \
 *     '-DIMGUI_USER_CONFIG="../../host/addons/imconfig_xi.h"' \
 *     tests/imgui_lua_test.cpp host/addons/gui_lua.cpp build/third_party/imgui.a build/third_party/luajit.a \
 *     -framework ApplicationServices -o build/imgui_lua_test && build/imgui_lua_test [ASHITA_DIR]
 *
 * ASHITA_DIR defaults to ../addon-deps/Ashita-v4beta (next to the repo). The test:
 *   1. semantics: conventions checked from Lua (tables at [1], multiple returns, live objects, overloads,
 *      the frame guard, a simulated click on a Checkbox and typing into an InputText);
 *   2. every manager function: each IGuiManager function from Ashita's annotations called once inside a
 *      frame with default-ish arguments derived from its annotated parameter types (Begin/End and
 *      Push/Pop pairs kept balanced), under pcall;
 *   3. real addons: several of Ashita's own addons loaded unmodified (their d3d_present handlers run for a
 *      few frames with every is_open flag forced on), with Ashita's real libs/imgui.lua and minimal stubs
 *      for the other AshitaCore managers.
 * It fails on any Lua error. ImGui assertions are counted and printed (the host logs and continues). */
#include "gui_lua.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "imgui.h"
#include "imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static int g_in_frame;
static int g_asserts;

extern "C" int xi_gui_in_frame(void) { return g_in_frame; }

extern "C" void xi_imgui_assert(const char* expr, const char* file, int line)
{
    if (++g_asserts <= 40)
        fprintf(stderr, "  imgui assert: %s (%s:%d)\n", expr, strrchr(file, '/') ? strrchr(file, '/') + 1 : file, line);
}

static ImGuiErrorRecoveryState g_recover;

static void begin_frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::ErrorRecoveryStoreState(&g_recover);
    g_in_frame = 1;
}

static void end_frame()
{
    ImGui::ErrorRecoveryTryToRecoverState(&g_recover);
    g_in_frame = 0;
    ImGui::Render();
    // Play a renderer backend that has textures: accept every create/update/destroy request.
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->Textures)
        for (ImTextureData* td : *dd->Textures)
        {
            if (td->Status == ImTextureStatus_WantCreate)
                td->SetTexID((ImTextureID)(uintptr_t)td->UniqueID + 1), td->SetStatus(ImTextureStatus_OK);
            else if (td->Status == ImTextureStatus_WantUpdates)
                td->SetStatus(ImTextureStatus_OK);
            else if (td->Status == ImTextureStatus_WantDestroy)
                td->SetTexID(ImTextureID_Invalid), td->SetStatus(ImTextureStatus_Destroyed);
        }
}

static int l_manager(lua_State* L)
{
    xi_gui_lua_push_manager(L);
    return 1;
}

static int l_frame_begin(lua_State*)
{
    begin_frame();
    return 0;
}

static int l_frame_end(lua_State*)
{
    end_frame();
    return 0;
}

static int l_mouse(lua_State* L)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2));
    if (!lua_isnoneornil(L, 3))
        io.AddMouseButtonEvent(0, lua_toboolean(L, 3) != 0);
    return 0;
}

static int l_type(lua_State* L)
{
    ImGui::GetIO().AddInputCharactersUTF8(luaL_checkstring(L, 1));
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// Lua side: the fake Ashita environment every state gets.

static const char* kPrelude = R"LUA(
local libs, addon_dir, scratch = XI_LIBS, XI_ADDON_DIR, XI_SCRATCH
package.path = libs .. '/?.lua;' .. libs .. '/?/init.lua;' .. (addon_dir and (addon_dir .. '/?.lua;') or '') .. package.path

XI_ERRORS = {}
XI_TRACE = os.getenv('XI_TRACE')
local function record(where, err)
    XI_ERRORS[#XI_ERRORS + 1] = where .. ': ' .. tostring(err)
end
XI_RECORD = record

-- A value that answers anything: indexing/calling gives itself, arithmetic gives 0.
local stub = {}
local zero = function() return 0 end
setmetatable(stub, {
    __index = function() return stub end,
    __call = function() return stub end,
    __add = zero, __sub = zero, __mul = zero, __div = zero, __mod = zero, __unm = zero, __pow = zero,
    __lt = function() return false end, __le = function() return false end,
    __concat = function(a, b) return tostring(a == stub and '' or a) .. tostring(b == stub and '' or b) end,
    __len = zero, __tostring = function() return '' end,
})
XI_STUB = stub

-- Managers: every method returns the stub unless overridden below.
local function manager(overrides)
    return setmetatable(overrides or {}, { __index = function() return function() return stub end end })
end
local memory = manager({
    GetPlayer = function() return manager({
        GetPetMPPercent = function() return 55 end, GetPetTP = function() return 1200 end,
        GetMainJob = function() return 16 end, GetMainJobLevel = function() return 75 end,
        GetSubJob = function() return 0 end, GetSubJobLevel = function() return 0 end,
        GetIsZoning = function() return 0 end,
    }) end,
    GetParty = function() return manager({
        GetMemberIndex = function() return 0 end, GetMemberServerId = function() return 0 end,
        GetMemberZone = function() return 0 end, GetMemberIsActive = function() return 0 end,
        GetMemberTargetIndex = function() return 0 end, GetMemberName = function() return 'Player' end,
    }) end,
    GetEntity = function() return manager({
        GetName = function() return nil end, GetRawEntity = function() return nil end,
        GetServerId = function() return 0 end, GetRenderFlags0 = function() return 0 end,
    }) end,
    GetTarget = function() return manager({ GetTargetIndex = function() return 0 end, GetIsSubTargetActive = function() return 0 end }) end,
    GetInventory = function() return manager({
        GetContainerCount = function() return 0 end, GetContainerCountMax = function() return 80 end,
        GetContainerItem = function() return nil end,
    }) end,
})
local resources = manager({
    GetItemById = function() return nil end, GetSpellById = function() return nil end,
    GetString = function() return '' end, GetSpellRange = function() return 0 end,
})
AshitaCore = manager({
    GetGuiManager = function() return XI_MANAGER() end,
    GetInstallPath = function() return scratch .. '/' end,
    GetMemoryManager = function() return memory end,
    GetResourceManager = function() return resources end,
    GetFontManager = function() return manager({ GetVisible = function() return true end }) end,
})

-- Global helpers Ashita provides to addons.
GetPlayerEntity = function() return { PetTargetIndex = 1, Name = 'Player', Distance = 0, HPPercent = 100 } end
GetEntity = function() return { Name = 'Pet', Distance = 25, HPPercent = 80, TargetIndex = 1, SpawnFlags = 1,
                                 Render = { Flags0 = 0 }, ActorPointer = 0 } end

-- ashita.*: events are captured, the rest answer harmlessly.
local events = {}
XI_EVENTS = events
local function ns(t) return setmetatable(t, { __index = function() return function() return stub end end }) end
ashita = ns({
    events = ns({
        register = function(name, id, fn) events[name] = events[name] or {}; events[name][id] = fn end,
        unregister = function(name, id) if events[name] then events[name][id] = nil end end,
    }),
    tasks = ns({ once = function() end, oncef = function() end, repeating = function() end, repeatingf = function() end }),
    fs = ns({ exists = function() return false end, create_dir = function() return true end,
              get_dir = function() return {} end, remove = function() return true end }),
    memory = ns({ find = function() return 0x10000 end, read_uint32 = function() return 0 end, read_uint = function() return 0 end,
                  write_uint = function() end, write_uint32 = function() end }),
    misc = ns({ open_url = function() end, set_clipboard = function() end }),
    regex = ns({ search = function() return nil end, match = function() return nil end }),
    bits = ns({ unpack_be = function() return 0 end, pack_be = function() end }),
})

-- Libraries needing the game or the real Ashita host.
package.preload['settings'] = function()
    return { load = function(d) return d end, save = function() end, register = function() end,
             reload = function() end, reset = function() end }
end
package.preload['d3d8'] = function()
    local dev = { GetViewport = function() return 0, { X = 0, Y = 0, Width = 1920, Height = 1080 } end }
    return setmetatable({ get_device = function() return dev end }, { __index = function() return function() return stub end end })
end
package.preload['ffxi.dats'] = function()
    return setmetatable({}, { __index = function() return function() return nil end end })
end
for _, m in ipairs({ 'fonts', 'primitives', 'scaling' }) do
    package.preload[m] = function() return stub end
end

addon = { name = XI_ADDON_NAME or 'test', author = '', version = '1', desc = '', link = '',
          path = (addon_dir or scratch) .. '/' }

function XI_FIRE(name, ...)
    for id, fn in pairs(events[name] or {}) do
        local ok, err = pcall(fn, ...)
        if not ok then record(name .. '/' .. tostring(id), err) end
    end
end

-- Force every is_open-style flag reachable from the present handlers on (editors, overlays).
function XI_OPEN_ALL()
    local seen = {}
    local function walk(v, depth)
        if depth > 6 or seen[v] then return end
        seen[v] = true
        if type(v) == 'table' then
            for k, x in pairs(v) do
                if (k == 'is_open' or k == 'visible' or k == 'enabled') and type(x) == 'table' and type(x[1]) == 'boolean' then
                    x[1] = true
                elseif (k == 'enabled') and type(x) == 'boolean' then
                    v[k] = true
                end
                if type(x) == 'table' or type(x) == 'function' then walk(x, depth + 1) end
            end
        elseif type(v) == 'function' then
            local i = 1
            while true do
                local n, x = debug.getupvalue(v, i)
                if not n then break end
                if type(x) == 'table' or type(x) == 'function' then walk(x, depth + 1) end
                i = i + 1
            end
        end
    end
    for _, cbs in pairs(events) do
        for _, fn in pairs(cbs) do walk(fn, 0) end
    end
    if type(_G[addon.name]) == 'table' then walk(_G[addon.name], 0) end -- addons keeping their state global
end
)LUA";

// ---------------------------------------------------------------------------------------------------
// 1. Semantics.

static const char* kSemantics = R"LUA(
local imgui = require('imgui')
local bit = require('bit')
local fails = 0
local function check(cond, what)
    if not cond then fails = fails + 1; XI_RECORD('semantics', what) end
end
local function near(a, b) return type(a) == 'number' and math.abs(a - b) < 1e-4 end

-- Missing functions are nil (feature tests), va_list variants unbound, Ashita helpers from imgui.lua.
check(imgui.TextV == nil, 'TextV should be nil')
check(imgui.NotAFunction == nil, 'unknown name should be nil')
check(type(imgui.col32) == 'function' and type(imgui.ShowHelp) == 'function', 'imgui.lua helpers')
check(imgui.GetVisible(imgui) == true, 'GetVisible')

-- Outside a frame: widgets do nothing and return defaults; style/io/colours still work.
check(imgui.Begin('x') == false, 'Begin outside frame is false')
check(imgui.Button('x') == false, 'Button outside frame is false')
imgui.End()
local style = imgui.GetStyle()
local io = imgui.GetIO()
check(near(io.DisplaySize.x, 1920) and near(io.DisplaySize.y, 1080), 'io.DisplaySize live ImVec2')
local old = style.WindowBorderSize
style.WindowBorderSize = 3.5
check(near(imgui.GetStyle().WindowBorderSize, 3.5), 'style field write')
style.WindowBorderSize = old
local c = style.Colors[ImGuiCol_Text + 1]
check(c ~= nil and near(c.w, 1.0), 'style.Colors 1-based ImVec4')
local ox = c.x
c.x = 0.25
check(near(style.Colors[ImGuiCol_Text + 1].x, 0.25), 'style.Colors write-through')
c.x = ox
local fpx = style.FramePadding.x
style.FramePadding.x = fpx + 1
check(near(style.FramePadding.x, fpx + 1), 'ImVec2 field write-through')
style.FramePadding = { fpx, style.FramePadding.y }
check(near(style.FramePadding.x, fpx), 'ImVec2 field assign from table')
check(style.ImageRounding == nil, 'unknown style field is nil')
style.ImageRounding = 2 -- ignored, no error
local r, g, b, a = imgui.ColorConvertU32ToFloat4(0xFF0000FF)
check(near(r, 1) and near(g, 0) and near(b, 0) and near(a, 1), 'ColorConvertU32ToFloat4 multiple returns')
check(imgui.ColorConvertFloat4ToU32({ 1, 0, 0, 1 }) == 0xFF0000FF, 'ColorConvertFloat4ToU32')
local neg = bit.bor(0x80000000, 0x000000FF) -- negative number from bit ops
check(neg < 0, 'bit.bor gives negative')
local r2, _, _, a2 = imgui.ColorConvertU32ToFloat4(neg)
check(near(r2, 1) and near(a2, 128 / 255), 'negative ImU32 accepted')
check(imgui.GetColorU32({ 1, 1, 1, 1 }) == 0xFFFFFFFF, 'GetColorU32(table)')
local sr, sg, sb, sa = imgui.GetStyleColorVec4(ImGuiCol_Text)
check(near(sa, 1), 'GetStyleColorVec4 4 returns')
check(imgui.GetMainViewport():GetCenter().x == 960, 'viewport:GetCenter().x')
check(imgui.GetStyleColorName(ImGuiCol_Text) == 'Text', 'GetStyleColorName')
check(type(imgui.GetVersion()) == 'string', 'GetVersion')
check(io.Fonts ~= nil and io.Fonts.Fonts[1] ~= nil, 'io.Fonts.Fonts[1]')
check(io.Fonts.Fonts[1] == io.Fonts.Fonts[1], 'object __eq')

-- Fonts: load outside the frame, use in it with the 1.92 PushFont(font, size) and the old one-arg form.
local font = imgui.AddFontFromFileTTF('/System/Library/Fonts/Supplemental/Arial.ttf', 18)
if font == nil then font = io.Fonts.Fonts[1] end
check(imgui.AddFontFromFileTTF('/nonexistent.ttf', 18) == nil, 'missing font file is nil')

XI_FRAME_BEGIN()
-- Guard is open now.
imgui.SetNextWindowPos({ 0, 0 }, ImGuiCond_Always)
imgui.SetNextWindowSize({ 400, 300 }, ImGuiCond_Always)
local cb = { false }
check(imgui.Begin('Semantics', { true }, ImGuiWindowFlags_NoTitleBar) == true, 'Begin inside frame')
local w, h = imgui.GetWindowSize()
check(near(w, 400) and near(h, 300), 'GetWindowSize multiple returns')
local tw, th = imgui.CalcTextSize('Hello')
check(tw > 0 and th > 0, 'CalcTextSize')
imgui.PushFont(font, 20)
check(near(imgui.GetFontSize(), 20), 'PushFont(font, size)')
imgui.PopFont()
imgui.PushFont(font)
imgui.PopFont()
imgui.PushStyleColor(ImGuiCol_Text, { 1, 0, 0, 1 })
imgui.PushStyleColor(ImGuiCol_Text, 0xFF00FF00)
imgui.PopStyleColor(2)
imgui.PushStyleVar(ImGuiStyleVar_Alpha, 0.5)
imgui.PushStyleVar(ImGuiStyleVar_WindowPadding, { 2, 2 })
imgui.PopStyleVar(2)
check(imgui.GetID('abc') == imgui.GetID('abc') and imgui.GetID('abc') ~= imgui.GetID('abd'), 'GetID')
imgui.PushID('s'); imgui.PushID(5); imgui.PushID({}); imgui.PopID(); imgui.PopID(); imgui.PopID()
imgui.Text('100% literal %s %d') -- printf-style functions take the string alone
imgui.TextColored({ 1, 1, 0, 1 }, 'colored')
local cx, cy = imgui.GetCursorScreenPos()
imgui.Checkbox('Check', cb)
local ix, iy = imgui.GetItemRectMin()
local buf = { 'ab' }
imgui.SetKeyboardFocusHere()
imgui.InputText('Input', buf, 16)
local dl = imgui.GetWindowDrawList()
dl:AddLine({ 0, 0 }, { 10, 10 }, 0xFFFFFFFF, 2)
dl:AddRectFilled({ 0, 0 }, { 10, 10 }, bit.bor(0xFF000000, 0x20), 3)
dl:AddText({ 5, 5 }, 0xFFFFFFFF, 'dl text')
dl:AddText(font, 18, { 5, 25 }, 0xFFFFFFFF, 'dl font text', 0)
dl:AddPolyline({ { 0, 0 }, { 5, 5 }, { 10, 0 } }, 3, 0xFFFFFFFF, 0, 1)
dl:AddImage(io.Fonts.TexRef, { 0, 0 }, { 16, 16 })
local mn_x, mn_y = dl:GetClipRectMin()
check(type(mn_x) == 'number' and type(mn_y) == 'number', 'draw list GetClipRectMin 2 returns')
imgui.GetForegroundDrawList():AddCircle({ 50, 50 }, 10, 0xFFFFFFFF)
imgui.Image(1, { 16, 16 })
local cur = { 1 }
imgui.Combo('combo', cur, 'a\0b\0c\0\0')
imgui.Combo('combo2', cur, { 'a', 'b', 'c' }, 3)
local sel = imgui.Selectable('sel', false)
local st = { false }
imgui.Selectable('sel2', st)
imgui.MenuItem('item', nil, true)
check(imgui.CollapsingHeader('hdr') ~= nil, 'CollapsingHeader overload 1')
imgui.CollapsingHeader('hdr2', { true }, 0)
if imgui.TreeNode('tn') then imgui.TreePop() end
if imgui.TreeNode('tn2', 'text') then imgui.TreePop() end
local f1 = { 0.5 }
imgui.SliderFloat('slider', f1, 0, 1)
local i2 = { 1, 2 }
imgui.InputInt2('i2', i2)
local col = { 1, 0, 0, 1 }
imgui.ColorEdit4('col', col)
local sc = { 7 }
imgui.DragScalar('dscalar', ImGuiDataType_S32, sc, 1, { 0 }, { 10 })
imgui.PlotLines('plot', { 1, 2, 3 }, 3)
imgui.Value('v', 3)
imgui.Value('v', 0.5)
imgui.Value('v', true)
imgui.ProgressBar(0.5, { -1, 0 }, 'p')
if imgui.BeginTable('t', 2, ImGuiTableFlags_Sortable) then
    imgui.TableSetupColumn('A')
    imgui.TableSetupColumn('B')
    imgui.TableHeadersRow()
    local specs = imgui.TableGetSortSpecs()
    check(specs ~= nil and specs.SpecsCount >= 0, 'TableGetSortSpecs object')
    if specs and specs.SpecsCount > 0 then
        check(specs.Specs.ColumnIndex == 0 and specs.Specs[1].ColumnIndex == 0, 'sort specs access')
    end
    imgui.TableNextRow()
    imgui.TableNextColumn(); imgui.Text('a')
    imgui.EndTable()
end
imgui.End()
XI_FRAME_END()

-- Simulated input: click the checkbox, type into the focused InputText.
XI_MOUSE(ix + 4, iy + 4)
local function frame(fn)
    XI_FRAME_BEGIN()
    imgui.SetNextWindowPos({ 0, 0 }, ImGuiCond_Always)
    imgui.SetNextWindowSize({ 400, 300 }, ImGuiCond_Always)
    imgui.Begin('Semantics', { true }, ImGuiWindowFlags_NoTitleBar)
    imgui.Text('100% literal %s %d')
    imgui.TextColored({ 1, 1, 0, 1 }, 'colored')
    fn()
    imgui.End()
    XI_FRAME_END()
end
local changed = false
frame(function() imgui.Checkbox('Check', cb); imgui.InputText('Input', buf, 16) end)
XI_MOUSE(ix + 4, iy + 4, true)
frame(function() if imgui.Checkbox('Check', cb) then changed = true end; imgui.InputText('Input', buf, 16) end)
XI_MOUSE(ix + 4, iy + 4, false)
frame(function() if imgui.Checkbox('Check', cb) then changed = true end; imgui.InputText('Input', buf, 16) end)
check(changed and cb[1] == true, 'Checkbox click toggles t[1] (got ' .. tostring(cb[1]) .. ')')
-- Focus the input (until ImGui reports it active), then type.
local active = false
for _ = 1, 5 do
    XI_FRAME_BEGIN()
    imgui.Begin('Typing', { true })
    if not active then imgui.SetKeyboardFocusHere() end
    imgui.InputText('Type', buf, 16)
    active = imgui.IsItemActive()
    imgui.End()
    XI_FRAME_END()
    if active then break end
end
check(active, 'InputText focus')
XI_TYPE('cd')
for _ = 1, 2 do
    XI_FRAME_BEGIN()
    imgui.Begin('Typing', { true })
    imgui.InputText('Type', buf, 16)
    imgui.End()
    XI_FRAME_END()
end
check(buf[1] == 'cd', 'InputText writes t[1] (keyboard focus selects all, typing replaces; got ' .. tostring(buf[1]) .. ')')
return fails
)LUA";

// ---------------------------------------------------------------------------------------------------
// 2. Every IGuiManager function from the annotations, once, with default-ish arguments.

static const char* kEveryFunction = R"LUA(
require('common') -- Ashita loads it for every addon; imgui.lua's DisplayPopup uses string:fmt
local imgui = require('imgui')
local path = XI_ANNOTATIONS
local f = assert(io.open(path, 'r'))
local sigs, order = {}, {}
local params = {}
for line in f:lines() do
    local pn, opt, pt = line:match('^%-%-%-@param%s+([%w_]+)(%??)%s+(%S+)')
    if pn then
        if pn ~= 'self' then params[#params + 1] = { name = pn, type = pt, opt = opt == '?' } end
    else
        local fname = line:match('^function%s+IGuiManager[.:]([%w_]+)%s*%(')
        if fname then
            if not sigs[fname] then sigs[fname] = params; order[#order + 1] = fname end
            params = {}
        elseif not line:match('^%-%-%-') then
            params = {}
        end
    end
end
f:close()

local scratch = XI_SCRATCH
local special_num = {
    columns = 2, count = 1, components = 2, items_count = 2, values_count = 3, buffer_size = 32,
    column_n = 0, column_index = 0, data_type = ImGuiDataType_S32, key = ImGuiKey_Tab, key_chord = ImGuiKey_Tab,
    button = 0, mouse_button = 0, idx = ImGuiCol_Text, dir = ImGuiDir_Right, cursor_type = 0, target = 1,
    tex_ref = 1, size = 16, id = 0, int_id = 3, storage_id = 1, viewport_id = 0, dock_id = 0, dockspace_id = 0,
    selection_user_data = 0, sz_io = 0, repeat_delay = 0.5, rate = 0.1, delay = 0.1, alpha = 0.5,
    size_pixels = 16, v_button = 1, fraction = 0.5, item_width = 100, local_x = 1, local_y = 1,
    offset_x = 10, width = 50, flags = 0, cond = 0, popup_flags = 0, child_flags = 0, window_flags = 0,
}
local function arg_for(fname, p, n)
    local t = p.type
    local name = p.name
    if name == 'buffer' then return { 'text' } end
    if name == 'items' then return { 'a', 'b' } end
    if name == 'values' then return { 1, 2, 3 } end
    if name == 'current_item' then return { 0 } end
    if name == 'flags_and_data' then return { 0, 1 } end
    if name == 'filename' or name == 'ini_filename' then return scratch .. '/imgui_test.ini' end
    if name == 'items_separated_by_zeros' then return 'a\0b\0\0' end
    if t == 'string' or t == 'buffer' then return name .. '##' .. fname .. n end
    if t == 'boolean' then return true end
    if t == 'table' or t == 'size' or t == 'tables' then
        if name:find('size') or name:find('pos') then return { 50, 20 } end
        return { 0.5, 0.5, 0.5, 1 }
    end
    if special_num[name] then return special_num[name] end
    if t == 'number' then return 1 end
    if t:match('^ImGui%w*Flags$') or t == 'ImGuiCond' or t == 'ImDrawFlags' then return 0 end
    if t == 'ImGuiKey' or t == 'ImGuiKeyChord' then return ImGuiKey_Tab end
    if t == 'ImGuiCol' then return ImGuiCol_Text end
    if t == 'ImGuiStyleVar' then return ImGuiStyleVar_Alpha end
    if t == 'ImGuiDataType' then return ImGuiDataType_S32 end
    if t == 'ImGuiMouseButton' then return 0 end
    if t:match('^ImGui') or t == 'userdata' or t == 'function' or t:match('^Im') then
        if t == 'ImFont' then return imgui.GetFont() end
        if t == 'ImGuiStyle' then return nil end
        if t == 'ImGuiViewport' then return imgui.GetMainViewport() end
        if t == 'ImGuiStorage' then return imgui.GetStateStorage() end
        return nil
    end
    return 1
end

-- Pairs: opener -> closer (true: always close, false: only when the opener returned true).
local closers = {
    Begin = { 'End', true }, BeginChild = { 'EndChild', true }, BeginGroup = { 'EndGroup', true },
    BeginDisabled = { 'EndDisabled', true }, BeginMultiSelect = { 'EndMultiSelect', true },
    BeginCombo = { 'EndCombo', false }, BeginListBox = { 'EndListBox', false }, BeginMenuBar = { 'EndMenuBar', false },
    BeginMainMenuBar = { 'EndMainMenuBar', false }, BeginMenu = { 'EndMenu', false }, BeginMenuEx = { 'EndMenu', false },
    BeginTooltip = { 'EndTooltip', false }, BeginItemTooltip = { 'EndTooltip', false }, BeginPopup = { 'EndPopup', false },
    BeginPopupModal = { 'EndPopup', false }, BeginPopupContextItem = { 'EndPopup', false },
    BeginPopupContextWindow = { 'EndPopup', false }, BeginPopupContextVoid = { 'EndPopup', false },
    BeginTable = { 'EndTable', false }, BeginTabBar = { 'EndTabBar', false }, BeginTabItem = { 'EndTabItem', false },
    BeginDragDropSource = { 'EndDragDropSource', false }, BeginDragDropTarget = { 'EndDragDropTarget', false },
    TreeNode = { 'TreePop', false }, TreeNodeEx = { 'TreePop', false }, TreePush = { 'TreePop', true },
    PushFont = { 'PopFont', true }, PushStyleColor = { 'PopStyleColor', true }, PushStyleVar = { 'PopStyleVar', true },
    PushStyleVarX = { 'PopStyleVar', true }, PushStyleVarY = { 'PopStyleVar', true }, PushItemFlag = { 'PopItemFlag', true },
    PushItemWidth = { 'PopItemWidth', true }, PushTextWrapPos = { 'PopTextWrapPos', true }, PushID = { 'PopID', true },
    PushClipRect = { 'PopClipRect', true }, LogToTTY = { 'LogFinish', true }, LogToFile = { 'LogFinish', true },
    LogToClipboard = { 'LogFinish', true }, Columns = { 'Columns', true },
}
local is_closer = {}
for _, v in pairs(closers) do is_closer[v[1]] = true end
is_closer.Columns = nil
-- Functions needing a scope to be meaningful, and ones with special arguments.
local in_table = { TableNextRow = 1, TableNextColumn = 1, TableSetColumnIndex = 1, TableSetupColumn = 1,
    TableSetupScrollFreeze = 1, TableHeader = 1, TableHeadersRow = 1, TableAngledHeadersRow = 1, TableGetSortSpecs = 1,
    TableGetColumnCount = 1, TableGetColumnIndex = 1, TableGetRowIndex = 1, TableGetColumnName = 1,
    TableGetColumnFlags = 1, TableSetColumnEnabled = 1, TableGetHoveredColumn = 1, TableSetBgColor = 1 }
local in_tabbar = { BeginTabItem = 1, TabItemButton = 1, SetTabItemClosed = 1 }
local in_multiselect = { SetNextItemSelectionUserData = 1, IsItemToggledSelection = 1 }
local skip = { EndFrame = true, Render = true, NewFrame = true } -- no-ops for addons, but keep the harness' frame

local called, missing, errors = 0, {}, 0
local function call(fname)
    local fn = imgui[fname]
    if fn == nil then
        if not missing[fname] then missing[fname] = true; missing[#missing + 1] = fname end
        return
    end
    local args, n = {}, 0
    for i, p in ipairs(sigs[fname]) do args[i] = arg_for(fname, p, i); n = i end
    if fname == 'MemFree' then args[1] = imgui.MemAlloc(16) end
    if fname == 'SetStateStorage' then args[1] = imgui.GetStateStorage() end
    if fname == 'GetVisible' or fname == 'SetVisible' then
        local ok, err = pcall(fn, imgui, true)
        called = called + 1
        if not ok then errors = errors + 1; XI_RECORD(fname, err) end
        return
    end
    local ok, r = pcall(fn, unpack(args, 1, n))
    called = called + 1
    if not ok then errors = errors + 1; XI_RECORD(fname, r); return end
    local c = closers[fname]
    if c and (c[2] or r == true) then
        local ok2, err2 = pcall(imgui[c[1]], fname == 'Columns' and 1 or nil)
        if not ok2 then errors = errors + 1; XI_RECORD(c[1], err2) end
    end
end

for frame = 1, 2 do
    XI_FRAME_BEGIN()
    imgui.Begin('Every function', { true })
    for _, fname in ipairs(order) do
        if not is_closer[fname] and not skip[fname] and not in_table[fname] and not in_tabbar[fname] and not in_multiselect[fname] then
            call(fname)
        end
    end
    if imgui.BeginTable('every_table', 2) then
        for fname in pairs(in_table) do call(fname) end
        imgui.EndTable()
    end
    if imgui.BeginTabBar('every_tabs') then
        for fname in pairs(in_tabbar) do call(fname) end
        imgui.EndTabBar()
    end
    imgui.BeginMultiSelect(0)
    for fname in pairs(in_multiselect) do call(fname) end
    imgui.EndMultiSelect()
    -- Closers on their own (unbalanced: exercise ImGui's error recovery, must not crash).
    imgui.End()
    XI_FRAME_END()
end

-- Every function with no arguments at all (nil for everything: ImGui defaults, no crash).
XI_FRAME_BEGIN()
imgui.Begin('No arguments', { true })
for _, fname in ipairs(order) do
    if not is_closer[fname] and not skip[fname] and fname ~= 'MemFree' and fname ~= 'col32' and imgui[fname] then
        if XI_TRACE then io.stderr:write('no-args ', fname, '\n') end
        local ok, r = pcall(imgui[fname])
        called = called + 1
        if not ok then errors = errors + 1; XI_RECORD(fname .. '()', r)
        else
            local c = closers[fname]
            if c and (c[2] or r == true) then pcall(imgui[c[1]]) end
        end
    end
end
imgui.End()
XI_FRAME_END()

-- The same outside a frame: widgets must do nothing, the rest must not touch a missing window.
for _, fname in ipairs(order) do
    if not skip[fname] then call(fname) end
end

-- Methods of the live objects (draw list: all of them; others: the read-only ones).
XI_FRAME_BEGIN()
imgui.Begin('Methods', { true })
local dl = imgui.GetWindowDrawList()
local p1, p2, p3, p4 = { 10, 10 }, { 60, 10 }, { 60, 60 }, { 10, 60 }
local W = 0xFFFFFFFF
local dcalls = {
    { 'PushClipRect', p1, p3, true }, { 'PopClipRect' }, { 'PushClipRectFullScreen' }, { 'PopClipRect' },
    { 'PushTexture', 1 }, { 'PopTexture' }, { 'GetClipRectMin' }, { 'GetClipRectMax' },
    { 'AddLine', p1, p2, W, 1 }, { 'AddRect', p1, p3, W, 2, 0, 1 }, { 'AddRectFilled', p1, p3, W, 2 },
    { 'AddRectFilledMultiColor', p1, p3, W, W, W, W }, { 'AddQuad', p1, p2, p3, p4, W }, { 'AddQuadFilled', p1, p2, p3, p4, W },
    { 'AddTriangle', p1, p2, p3, W }, { 'AddTriangleFilled', p1, p2, p3, W }, { 'AddCircle', p1, 5, W },
    { 'AddCircleFilled', p1, 5, W }, { 'AddNgon', p1, 5, W, 6 }, { 'AddNgonFilled', p1, 5, W, 6 },
    { 'AddEllipse', p1, { 5, 3 }, W }, { 'AddEllipseFilled', p1, { 5, 3 }, W }, { 'AddText', p1, W, 'txt' },
    { 'AddText', imgui.GetFont(), 14, p1, W, 'txt', 0 }, { 'AddBezierCubic', p1, p2, p3, p4, W, 1 },
    { 'AddBezierQuadratic', p1, p2, p3, W, 1 }, { 'AddPolyline', { p1, p2, p3 }, 3, W, 0, 1 },
    { 'AddConvexPolyFilled', { p1, p2, p3 }, 3, W }, { 'AddConcavePolyFilled', { p1, p2, p3, p4 }, 4, W },
    { 'AddImage', 1, p1, p3 }, { 'AddImageQuad', 1, p1, p2, p3, p4 }, { 'AddImageRounded', 1, p1, p3, { 0, 0 }, { 1, 1 }, W, 3 },
    { 'PathClear' }, { 'PathLineTo', p1 }, { 'PathLineToMergeDuplicate', p2 }, { 'PathLineTo', p3 }, { 'PathFillConvex', W },
    { 'PathLineTo', p1 }, { 'PathLineTo', p2 }, { 'PathLineTo', p3 }, { 'PathFillConcave', W },
    { 'PathLineTo', p1 }, { 'PathLineTo', p2 }, { 'PathStroke', W, 0, 1 },
    { 'PathArcTo', p1, 5, 0, 3 }, { 'PathArcToFast', p1, 5, 0, 6 }, { 'PathEllipticalArcTo', p1, { 5, 3 }, 0, 0, 3 },
    { 'PathBezierCubicCurveTo', p2, p3, p4 }, { 'PathBezierQuadraticCurveTo', p2, p3 }, { 'PathRect', p1, p3 },
    { 'PathStroke', W }, { 'PrimReserve', 6, 4 }, { 'PrimRect', p1, p3, W }, { 'PrimReserve', 6, 4 },
    { 'PrimRectUV', p1, p3, { 0, 0 }, { 1, 1 }, W }, { 'PrimReserve', 6, 4 },
    { 'PrimQuadUV', p1, p2, p3, p4, { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, W },
}
for _, c in ipairs(dcalls) do
    local m = dl[c[1]]
    if m == nil then missing[#missing + 1] = 'ImDrawList:' .. c[1]
    else
        local ok, err = pcall(m, dl, unpack(c, 2))
        called = called + 1
        if not ok then errors = errors + 1; XI_RECORD('ImDrawList:' .. c[1], err) end
    end
end
local font = imgui.GetFont()
local baked = imgui.GetFontBaked()
local vp = imgui.GetMainViewport()
local st = imgui.GetStateStorage()
local ocalls = {
    { font, 'IsGlyphInFont', 65 }, { font, 'IsLoaded' }, { font, 'GetDebugName' }, { font, 'GetFontBaked', 13 },
    { font, 'CalcTextSizeA', 13, 1000, 0, 'hello' }, { font, 'CalcWordWrapPosition', 13, 'hello world', nil, 20 },
    { font, 'RenderChar', dl, 13, { 10, 10 }, W, 65 }, { font, 'RenderText', dl, 13, { 10, 10 }, W, { 0, 0, 500, 500 }, 'txt', nil, 0 },
    { font, 'IsGlyphRangeUnused', 0x4E00, 0x4E10 }, { baked, 'FindGlyph', 65 }, { baked, 'FindGlyphNoFallback', 65 },
    { baked, 'GetCharAdvance', 65 }, { baked, 'IsGlyphLoaded', 65 }, { vp, 'GetCenter' }, { vp, 'GetWorkCenter' },
    { st, 'SetInt', 123, 5 }, { st, 'GetInt', 123, 0 }, { st, 'SetBool', 124, true }, { st, 'GetBool', 124, false },
    { st, 'SetFloat', 125, 0.5 }, { st, 'GetFloat', 125, 0 }, { st, 'GetIntRef', 126, 3 }, { st, 'GetBoolRef', 127, true },
    { st, 'GetFloatRef', 128, 1.5 }, { imgui.GetStyle(), 'ScaleAllSizes', 1.0 }, { imgui.GetIO(), 'AddFocusEvent', true },
    { imgui.GetIO().Fonts, 'GetGlyphRangesDefault' },
}
for _, c in ipairs(ocalls) do
    local o, name = c[1], c[2]
    local m = o and o[name]
    if m == nil then missing[#missing + 1] = tostring(o) .. ':' .. name
    else
        local ok, err = pcall(m, o, unpack(c, 3, 10))
        called = called + 1
        if not ok then errors = errors + 1; XI_RECORD(name, err) end
    end
end
local sx = select(1, font:CalcTextSizeA(13, 1000, 0, 'hello')).x
if not (sx and sx > 0) then errors = errors + 1; XI_RECORD('CalcTextSizeA', 'expected ImVec2 object with x > 0') end
if st:GetInt(123, 0) ~= 5 then errors = errors + 1; XI_RECORD('ImGuiStorage', 'GetInt after SetInt') end
imgui.End()
XI_FRAME_END()
XI_EVERY = { called = called, missing = missing }
return errors
)LUA";

// ---------------------------------------------------------------------------------------------------

static std::string g_ashita, g_scratch;

static lua_State* new_state(const char* addon_dir, const char* addon_name)
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, l_manager);
    lua_setglobal(L, "XI_MANAGER");
    lua_pushcfunction(L, l_frame_begin);
    lua_setglobal(L, "XI_FRAME_BEGIN");
    lua_pushcfunction(L, l_frame_end);
    lua_setglobal(L, "XI_FRAME_END");
    lua_pushcfunction(L, l_mouse);
    lua_setglobal(L, "XI_MOUSE");
    lua_pushcfunction(L, l_type);
    lua_setglobal(L, "XI_TYPE");
    lua_pushstring(L, (g_ashita + "/addons/libs").c_str());
    lua_setglobal(L, "XI_LIBS");
    lua_pushstring(L, (g_ashita + "/addons/libs/annotations/SDK/IGuiManager.lua").c_str());
    lua_setglobal(L, "XI_ANNOTATIONS");
    lua_pushstring(L, g_scratch.c_str());
    lua_setglobal(L, "XI_SCRATCH");
    if (addon_dir)
    {
        lua_pushstring(L, addon_dir);
        lua_setglobal(L, "XI_ADDON_DIR");
        lua_pushstring(L, addon_name);
        lua_setglobal(L, "XI_ADDON_NAME");
    }
    if (luaL_dostring(L, kPrelude) != 0)
    {
        fprintf(stderr, "prelude: %s\n", lua_tostring(L, -1));
        exit(2);
    }
    return L;
}

static int report_errors(lua_State* L, const char* what)
{
    lua_getglobal(L, "XI_ERRORS");
    int n = (int)lua_objlen(L, -1);
    for (int i = 1; i <= n && i <= 30; i++)
    {
        lua_rawgeti(L, -1, i);
        fprintf(stderr, "  %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return n;
}

static int run_chunk(lua_State* L, const char* chunk, const char* name)
{
    if (luaL_loadbuffer(L, chunk, strlen(chunk), name) != 0 || lua_pcall(L, 0, 1, 0) != 0)
    {
        fprintf(stderr, "  %s: %s\n", name, lua_tostring(L, -1));
        if (g_in_frame)
            end_frame();
        return 1;
    }
    lua_pop(L, 1);
    return report_errors(L, name);
}

static int run_addon(const char* name)
{
    std::string dir = g_ashita + "/addons/" + name;
    std::string file = dir + "/" + name + ".lua";
    lua_State* L = new_state(dir.c_str(), name);
    int errs = 0;
    if (luaL_dofile(L, file.c_str()) != 0)
    {
        fprintf(stderr, "  %s: load: %s\n", name, lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    luaL_dostring(L, "XI_FIRE('load')");
    luaL_dostring(L, "XI_OPEN_ALL()");
    lua_getglobal(L, "XI_EVENTS");
    lua_getfield(L, -1, "d3d_present");
    int handlers = 0;
    if (lua_istable(L, -1))
        for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
            handlers++;
    lua_pop(L, 2);
    int vtx = 0, windows = 0;
    for (int f = 0; f < 4; f++)
    {
        begin_frame();
        luaL_dostring(L, "XI_FIRE('d3d_present')");
        windows = 0;
        for (ImGuiWindow* w : GImGui->Windows)
            if (w->Active && !(w->Flags & ImGuiWindowFlags_ChildWindow) && strncmp(w->Name, "Debug##", 7) != 0) windows++, (getenv("XI_NAMES") ? fprintf(stderr, "    [%s]\n", w->Name) : 0);
        end_frame();
        vtx = ImGui::GetDrawData()->TotalVtxCount;
    }
    luaL_dostring(L, "XI_FIRE('unload')");
    errs = report_errors(L, name);
    printf("  addon %-12s %d present handler(s), %d window(s), %d vertices, %s\n", name, handlers, windows, vtx, errs ? "ERRORS" : "ok");
    xi_gui_lua_forget_state(L);
    lua_close(L);
    return errs;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_ashita = argc > 1 ? argv[1] : "../addon-deps/Ashita-v4beta";
    const char* tmp = getenv("TMPDIR");
    g_scratch = std::string(tmp ? tmp : "/tmp") + "/imgui_lua_test";
    mkdir(g_scratch.c_str(), 0755);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    io.DisplaySize = ImVec2(1920, 1080);
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    begin_frame(); // settle the atlas once
    end_frame();

    int fails = 0;

    printf("semantics\n");
    {
        lua_State* L = new_state(nullptr, nullptr);
        int e = run_chunk(L, kSemantics, "semantics");
        printf("  %s\n", e ? "FAILED" : "ok");
        fails += e;
        lua_close(L);
    }

    printf("every IGuiManager function\n");
    {
        lua_State* L = new_state(nullptr, nullptr);
        int asserts_before = g_asserts;
        int e = run_chunk(L, kEveryFunction, "every");
        lua_getglobal(L, "XI_EVERY");
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "called");
            printf("  %d calls, %d ImGui assertions (logged, recovered)\n", (int)lua_tonumber(L, -1), g_asserts - asserts_before);
            lua_pop(L, 1);
            lua_getfield(L, -1, "missing");
            int n = (int)lua_objlen(L, -1);
            printf("  not bound (nil): ");
            for (int i = 1; i <= n; i++)
            {
                lua_rawgeti(L, -1, i);
                printf("%s%s", i > 1 ? ", " : "", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            printf("%s\n", n ? "" : "none");
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        printf("  %s\n", e ? "FAILED" : "ok");
        fails += e;
        lua_close(L);
    }

    printf("Ashita addons (d3d_present)\n");
    const char* addons[] = { "imguistyle", "links", "petinfo", "crosshair", "chatmon", "itemwatch", "actionparse",
                             "blucheck", "clearcolor", "chamcham", "renamer" };
    for (const char* a : addons)
        fails += run_addon(a);

    ImGui::DestroyContext();
    printf("%s (%d ImGui assertions logged in total)\n", fails ? "FAIL" : "PASS", g_asserts);
    return fails ? 1 : 0;
}
