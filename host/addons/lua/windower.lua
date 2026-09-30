--[==[
The Windower 4 layer: the windower.* API over xi.*, so unmodified Windower addons and their own
libraries (addons/libs: config, texts, images, packets, resources, ...) run.

    hooks = ...           -- what xi.lua returned (run in the addon's state before the addon)

Split in modules (require('xi.<name>')):
    windower.lua          this file: the windower table, events, commands, chat, files, misc
    windower_ui.lua       windower.text / windower.prim (the texts and images libraries)
    windower_ffxi.lua     windower.ffxi (the game's state) and windower.packets
    windower_events.lua   (separate) the packet-derived events and state (action, zone change,
                          gain buff, ...): it gets `raise` / `has` from here, see install below

Events: windower.register_event(name, ..., fn) -> id, ...; handlers run as coroutines (they may
coroutine.sleep), raw_register_event's run plain. Raised here: load, unload, prerender,
postrender, addon command, unhandled command, keyboard, mouse, incoming text, outgoing text,
ipc message; everything else comes from windower_events.

Gaps are loud: an unimplemented member raises "windower.X is not supported yet"; every hit, and
every lookup of a name Windower doesn't have, is logged as "unsupported: ..." for the survey.
--]==]

local hooks = ...
local xi = xi
local native = xi
local info = xi.info
local wn = xi.windower_native

local traceback = debug.traceback
local co_create, co_resume, co_status, co_yield, co_running =
    coroutine.create, coroutine.resume, coroutine.status, coroutine.yield, coroutine.running
local select, type, tostring, tonumber, pairs, ipairs = select, type, tostring, tonumber, pairs, ipairs

-------------------------------------------------------------------------------- unsupported

local logged = {}
local function unsupported(what, loud)
    if not logged[what] then
        logged[what] = true
        xi.log('unsupported: ' .. what)
    end
    if loud ~= false then
        error(what .. ' is not supported yet', 3)
    end
end

-- A function that fails loudly when called.
local function stub(path)
    return function() unsupported(path) end
end

-- Tables whose unknown members are logged (and read as nil: addons feature-test them).
local function partial(t, path)
    return setmetatable(t, {
        __index = function(_, k)
            if type(k) == 'string' then unsupported(path .. '.' .. k, false) end
            return nil
        end,
    })
end

-------------------------------------------------------------------------------- the pack library

-- Windower's string.pack / string.unpack ('I':pack(n), data:unpack('H', 3)); require('pack')
string.pack = wn.pack
string.unpack = wn.unpack
package.loaded['pack'] = string
package.preload['pack'] = function() return string end

-- Windower's other native modules (DLLs in its install): not here, and loud about it
for _, name in ipairs({ 'sqlite3', 'ssl.core', 'ssl.context', 'ssl.x509', 'ssl.config' }) do
    package.preload[name] = function() unsupported("require('" .. name .. "') (Windower's native module)") end
end

-------------------------------------------------------------------------------- windower

windower = {}
local windower = windower

local function slash(p)
    if p:sub(-1) ~= '/' and p:sub(-1) ~= '\\' then return p .. '/' end
    return p
end

windower.addon_path = slash(info.path)
windower.windower_path = slash(info.root)
windower.ffxi_path = slash(xi.paths.game)
windower.pol_path = windower.ffxi_path

-- _addon: the addon fills it (name, version, author, command(s), language)
_addon = _addon or {}
_libs = _libs or {}

-------------------------------------------------------------------------------- coroutines

-- A coroutine that yielded (coroutine.sleep: xi.lua's scheduler token) goes on as a task: the
-- task re-yields the same token to xi.lua's scheduler and resumes the coroutine when it's due.
local scheduled = setmetatable({}, { __mode = 'k' }) -- coroutine -> task

local function report(err, where)
    native.error(tostring(err), where or 'handler')
end

local function continue_later(co, tok, amount, where)
    local t
    t = xi.tasks.spawn(function()
        local a, b = tok, amount
        while true do
            co_yield(a, b)
            local ok, x, y = co_resume(co)
            if not ok then
                report(traceback(co, tostring(x)), where)
                return
            end
            if co_status(co) ~= 'suspended' then return end
            a, b = x, y
        end
    end)
    scheduled[co] = t
    return t
end

-- Runs fn as a coroutine now; its results if it finishes, nothing if it sleeps.
local function run_co(fn, where, ...)
    local co = co_create(fn)
    local ok, a, b = co_resume(co, ...)
    if not ok then
        report(traceback(co, tostring(a)), where)
        return
    end
    if co_status(co) == 'suspended' then
        continue_later(co, a, b, where)
        return
    end
    return a, b
end

local function run_raw(fn, where, ...)
    local ok, a, b = xpcall(fn, traceback, ...)
    if not ok then
        report(a, where)
        return
    end
    return a, b
end

-- Windower's coroutine.schedule(fn, seconds) -> coroutine; coroutine.close cancels it.
function coroutine.schedule(fn, time)
    local co = co_create(fn)
    local t = xi.tasks.spawn(function()
        coroutine.sleep(tonumber(time) or 0)
        local ok, a, b = co_resume(co)
        while ok and co_status(co) == 'suspended' do
            co_yield(a, b)
            ok, a, b = co_resume(co)
        end
        if not ok then report(traceback(co, tostring(a)), 'coroutine.schedule') end
    end)
    scheduled[co] = t
    return co
end

function coroutine.close(co)
    local t = scheduled[co]
    if t then
        xi.tasks.cancel(t)
        scheduled[co] = nil
        return true
    end
    return co_status(co) == 'dead'
end

-------------------------------------------------------------------------------- events

local handlers = {} -- name -> { {id, fn, raw}, ... }
local by_id = {}    -- id -> name
local next_id = 1

local function register(raw, ...)
    local n = select('#', ...)
    local fn = select(n, ...)
    if type(fn) ~= 'function' then
        error('windower.register_event: the last argument must be a function', 3)
    end
    if n < 2 then error('windower.register_event: no event name', 3) end
    local ids = {}
    for i = 1, n - 1 do
        local name = select(i, ...)
        if type(name) ~= 'string' then error('windower.register_event: event names are strings', 3) end
        name = name:lower()
        local list = handlers[name]
        if not list then
            list = {}
            handlers[name] = list
        end
        local id = next_id
        next_id = next_id + 1
        list[#list + 1] = { id = id, fn = fn, raw = raw }
        by_id[id] = name
        ids[#ids + 1] = id
    end
    return unpack(ids)
end

function windower.register_event(...) return register(false, ...) end
function windower.raw_register_event(...) return register(true, ...) end

function windower.unregister_event(...)
    for i = 1, select('#', ...) do
        local id = select(i, ...)
        local name = by_id[id]
        if name then
            by_id[id] = nil
            local list = handlers[name]
            for k = #list, 1, -1 do
                if list[k].id == id then table.remove(list, k) end
            end
        end
    end
end

function windower.register_unhandled_command(fn)
    return windower.register_event('unhandled command', fn)
end

local function has(name)
    local list = handlers[name]
    return list ~= nil and #list > 0
end

local function call(h, name, ...)
    if h.raw then return run_raw(h.fn, name, ...) end
    return run_co(h.fn, name, ...)
end

-- Every handler of `name` with the same arguments: true if any returned true.
local function fire(name, ...)
    local list = handlers[name]
    if not list or #list == 0 then return nil end
    local any
    local copy = { unpack(list) }
    for i = 1, #copy do
        if call(copy[i], name, ...) == true then any = true end
    end
    return any
end

-- The chunk events: (id, original, modified, injected, blocked), threaded through the handlers
-- (each sees the one before's change). Returns true if blocked, else the new bytes if changed.
local function fire_chunk(name, id, original, modified, injected, blocked)
    local list = handlers[name]
    if not list or #list == 0 then return nil end
    local copy = { unpack(list) }
    local mod, block = modified or original, blocked and true or false
    for i = 1, #copy do
        local r = call(copy[i], name, id, original, mod, injected, block)
        if r == true then
            block = true
        elseif type(r) == 'string' then
            mod = r
        end
    end
    if block and not blocked then return true end
    if mod ~= (modified or original) then return mod end
    return nil
end

-- For windower_events: raise(name, ...) as its interface says.
local function raise(name, ...)
    name = name:lower()
    if name == 'incoming chunk' or name == 'outgoing chunk' then
        return fire_chunk(name, ...)
    end
    return fire(name, ...)
end

-------------------------------------------------------------------------------- xi events

local on = xi.events.on

on('load', function() fire('load') end)
on('unload', function() fire('unload') end)
on('present', function() fire('prerender') end)
on('postrender', function() fire('postrender') end)

-- incoming text(original, modified, original_mode, modified_mode, blocked): a string replaces the
-- text, true blocks; (string, mode) changes the mode too
on('text_in', function(e)
    local list = handlers['incoming text']
    if not list or #list == 0 then return end
    local copy = { unpack(list) }
    local text, mode = e.modified or e.data, e.mode_modified or e.mode
    for i = 1, #copy do
        local r, m = call(copy[i], 'incoming text', e.data, text, e.mode, mode, e.blocked or false)
        if r == true then
            e.blocked = true
        elseif type(r) == 'string' then
            text = r
            if type(m) == 'number' then mode = m end
        end
    end
    if e.modified ~= nil then
        e.modified = text
        e.mode_modified = mode
    end
end)

-- outgoing text(original, modified, blocked)
on('text_out', function(e)
    local list = handlers['outgoing text']
    if not list or #list == 0 then return end
    local copy = { unpack(list) }
    local text = e.modified or e.data
    for i = 1, #copy do
        local r = call(copy[i], 'outgoing text', e.data, text, e.blocked or false)
        if r == true then
            e.blocked = true
        elseif type(r) == 'string' then
            text = r
        end
    end
    if e.modified ~= nil then e.modified = text end
end)

-- keyboard(dik, pressed, flags, blocked)
on('key', function(e)
    if not has('keyboard') then return end
    if fire('keyboard', e.key, e.down and true or false, e.flags or 0, e.blocked or false) then
        e.blocked = true
    end
end)

-- mouse(type, x, y, delta, blocked): the type is the window message less WM_MOUSEMOVE (0 move,
-- 1/2 left down/up, 4/5 right, 7/8 middle, 10 wheel)
on('mouse', function(e)
    if not has('mouse') then return end
    local t = (e.message or 0x200) - 0x200
    if fire('mouse', t, e.x or 0, e.y or 0, e.delta or 0, e.blocked or false) then
        e.blocked = true
    end
end)

-------------------------------------------------------------------------------- commands

-- Windower's argument splitting: spaces separate, double quotes group (and are removed).
local function split_args(s)
    local args = {}
    local i, n = 1, #s
    while i <= n do
        while i <= n and s:sub(i, i):match('%s') do i = i + 1 end
        if i > n then break end
        local c = s:sub(i, i)
        if c == '"' then
            local j = s:find('"', i + 1, true)
            if not j then j = n + 1 end
            args[#args + 1] = s:sub(i + 1, j - 1)
            i = j + 1
        else
            local j = i
            while j <= n and not s:sub(j, j):match('%s') do j = j + 1 end
            args[#args + 1] = s:sub(i, j - 1)
            i = j
        end
    end
    return args
end

local function my_names()
    local names = {}
    if type(_addon.name) == 'string' then names[_addon.name:lower()] = true end
    names[info.name:lower()] = true
    if type(_addon.command) == 'string' then names[_addon.command:lower()] = true end
    if type(_addon.commands) == 'table' then
        for _, c in pairs(_addon.commands) do
            if type(c) == 'string' then names[c:lower()] = true end
        end
    elseif type(_addon.commands) == 'string' then
        names[_addon.commands:lower()] = true
    end
    return names
end

on('command', function(e)
    local line = e.data or ''
    local word, rest = line:match('^//(%S+)%s*(.-)%s*$')
    if not word then return end
    local lw = word:lower()
    if my_names()[lw] then
        fire('addon command', unpack(split_args(rest)))
        e.blocked = true
        return true
    end
    -- unhandled command: a console command no loaded addon is named for
    if has('unhandled command') then
        for _, a in ipairs(xi.addons.list()) do
            if a.name:lower() == lw then return end
        end
        if fire('unhandled command', word, unpack(split_args(rest))) then
            e.blocked = true
            return true
        end
    end
end)

-- ipc message: Windower sends to the other game instances running the same addon, never to the
-- sender. There is one instance here, so there is nobody to deliver to.
function windower.send_ipc_message(msg) end

-- send_command: Windower console syntax. ';' separates commands (not inside quotes), "wait n"
-- delays the rest, "input <line>" is a line for the game, anything else is a console command.
local function split_commands(s)
    local out, cur, q = {}, {}, false
    for i = 1, #s do
        local c = s:sub(i, i)
        if c == '"' then q = not q end
        if c == ';' and not q then
            out[#out + 1] = table.concat(cur)
            cur = {}
        else
            cur[#cur + 1] = c
        end
    end
    out[#out + 1] = table.concat(cur)
    return out
end

local console_unsupported = {
    setkey = true, keyboard_blockinput = true, wincontrol = true, game_forceclipping = true,
    console_position = true, console_color = true, hideconsole = true, showconsole = true,
    console_toggle = true, screenshot = true, drawdistance = true,
}

local run_commands
run_commands = function(list, from)
    for i = from, #list do
        -- a leading '@' only keeps the command out of the console's echo
        local cmd = list[i]:match('^%s*@?%s*(.-)%s*$')
        if cmd ~= '' then
            local word, rest = cmd:match('^(%S+)%s*(.*)$')
            local lw = word:lower():gsub('^//', '')
            if lw == 'wait' then
                xi.tasks.once(tonumber(rest) or 0, run_commands, list, i + 1)
                return
            elseif lw == 'input' then
                native.chat.run(rest, 1)
            elseif cmd:sub(1, 1) == '/' and cmd:sub(2, 2) ~= '/' then
                native.chat.run(cmd, 1)
            elseif lw == 'unalias' then
                native.chat.alias('//' .. rest:match('^(%S*)'), nil)
            elseif lw == 'load' or lw == 'unload' or lw == 'reload' then
                -- the console's load/unload are for Windower's plugins (DLLs): none here
                local plugin = rest:match('^(%S*)')
                unsupported('Windower plugin ' .. plugin, false)
                native.chat.write(('[%s] Windower plugins are not supported (%s %s)'):format(info.name, lw, plugin), 207)
            elseif console_unsupported[lw] then
                unsupported('console command ' .. lw, false)
                native.chat.write(('[%s] the console command "%s" is not supported yet'):format(info.name, lw), 207)
            else
                native.chat.run(cmd:sub(1, 2) == '//' and cmd or '//' .. cmd, 1)
            end
        end
    end
end

function windower.send_command(s)
    s = tostring(s or '')
    run_commands(split_commands(s), 1)
end

-------------------------------------------------------------------------------- chat

-- print: Windower's goes to its console, which reads \cs(r,g,b) / \cr colour codes; there is no
-- console here, so the lines go to the chat log without them.
do
    local xi_print = print
    print = function(...)
        local n = select('#', ...)
        local parts = {}
        for i = 1, n do parts[i] = tostring((select(i, ...))) end
        local s = table.concat(parts, ' '):gsub('\\cs%(%s*%d+%s*,%s*%d+%s*,%s*%d+%s*%)', ''):gsub('\\cr', '')
        return xi_print(s)
    end
end

function windower.add_to_chat(mode, msg)
    if msg == nil then msg, mode = mode, 1 end
    msg = tostring(msg)
    mode = tonumber(mode) or 1
    for line in (msg .. '\n'):gmatch('([^\n]*)\n') do
        native.chat.write(line, mode)
    end
end

windower.chat = partial({
    input = function(text) native.chat.run(tostring(text), 1) end,
    is_open = function() return select(2, native.chat.input()) and true or false end,
    get_input = function()
        local text = native.chat.input()
        return text, #text
    end,
    set_input = function(text) native.chat.set_input(tostring(text or '')) end,
    add_to_input = function(text, position)
        local cur = native.chat.input() or ''
        position = tonumber(position)
        if position and position >= 0 and position < #cur then
            cur = cur:sub(1, position) .. text .. cur:sub(position + 1)
        else
            cur = cur .. text
        end
        native.chat.set_input(cur)
    end,
    paste = function()
        local s = wn.clipboard_get and wn.clipboard_get()
        if s then windower.chat.add_to_input(windower.to_shift_jis(s)) end
    end,
}, 'windower.chat')

-- The Windower console (no console here): lines go to the log.
windower.console = partial({
    write = function(text) xi.log('console: ' .. tostring(text)) end,
    visible = function() return false end,
    clear = function() end,
    open = function() unsupported('windower.console.open', false) end,
    close = function() end,
    set_position = function() end,
}, 'windower.console')

-------------------------------------------------------------------------------- files

local function host(p) return native.fs.path(tostring(p)) end

function windower.file_exists(path)
    return native.fs.exists(path) and not native.fs.is_dir(path)
end

function windower.dir_exists(path)
    return native.fs.is_dir(path)
end

-- files and folders (lfs: xi.fs.list filters one or the other)
function windower.get_dir(path)
    if not native.fs.is_dir(path) then return nil end
    local ok, lfs = pcall(require, 'lfs')
    if not ok then return native.fs.list(path) end
    local t = {}
    for name in lfs.dir(tostring(path)) do
        if name ~= '.' and name ~= '..' then t[#t + 1] = name end
    end
    return t
end

function windower.create_dir(path)
    if native.fs.is_dir(path) then return true end
    local parent = tostring(path):gsub('[/\\]+$', ''):match('^(.*)[/\\][^/\\]+$')
    if parent and parent ~= '' and not native.fs.is_dir(parent) then
        return false, 'The parent directory does not exist: ' .. parent
    end
    if native.fs.mkdir(path) then return true end
    return false, 'Could not create ' .. tostring(path)
end

-------------------------------------------------------------------------------- misc

function windower.has_focus() return true end
function windower.take_focus() end

function windower.copy_to_clipboard(s)
    if wn.clipboard_set then return wn.clipboard_set(tostring(s)) end
    unsupported('windower.copy_to_clipboard')
end

function windower.get_from_clipboard()
    if wn.clipboard_get then return wn.clipboard_get() end
    unsupported('windower.get_from_clipboard')
end

function windower.open_url(url) native.open_url(tostring(url)) end

function windower.play_sound(path)
    if wn.play_sound and wn.play_sound(host(path)) then return end
    unsupported('windower.play_sound', false)
end

windower.execute = stub('windower.execute')
windower.set_mob_name = stub('windower.set_mob_name')
windower.get_camera = stub('windower.get_camera')
windower.get_chat_filters = stub('windower.get_chat_filters')

function windower.get_item_display() return nil end

function windower.debug(...)
    local parts = {}
    for i = 1, select('#', ...) do parts[i] = tostring((select(i, ...))) end
    xi.log('debug: ' .. table.concat(parts, ' '))
end

function windower.from_shift_jis(s) return native.sjis_to_utf8(tostring(s)) end
function windower.to_shift_jis(s) return native.utf8_to_sjis(tostring(s)) end

-- auto-translate phrases (FD kind lang b2 b3 FD) as the game's text
function windower.convert_auto_trans(s)
    s = tostring(s)
    return (s:gsub('\253(....)\253', function(code)
        local t = native.res.auto_translate(code:byte(1), code:byte(2), code:byte(3), code:byte(4))
        return t
    end))
end

-- wc_match(str, pattern): '*' any run, '?' one character, '|' alternatives; case-insensitive
local wc_cache = {}
function windower.wc_match(str, pattern)
    if str == nil or pattern == nil then return false end
    str, pattern = tostring(str):lower(), tostring(pattern):lower()
    local pats = wc_cache[pattern]
    if not pats then
        pats = {}
        for alt in (pattern .. '|'):gmatch('([^|]*)|') do
            local p = alt:gsub('[%^%$%(%)%%%.%[%]%+%-]', '%%%0'):gsub('%*', '.*'):gsub('%?', '.')
            pats[#pats + 1] = '^' .. p .. '$'
        end
        wc_cache[pattern] = pats
    end
    for i = 1, #pats do
        if str:find(pats[i]) then return true end
    end
    return false
end

-- Windower's regular expressions: ECMAScript (std::regex), windower_regex.cpp.
windower.regex = partial({
    -- match(str, pattern) -> { {[0] = whole, captures...}, ... } for every match, or nil
    match = function(str, pattern)
        str = tostring(str)
        local all = wn.regex_find_all(str, tostring(pattern))
        if #all == 0 then return nil end
        local res = {}
        for k, m in ipairs(all) do
            local t = { [0] = str:sub(m[1], m[2]) }
            for j = 3, #m do t[j - 2] = m[j] or nil end
            res[k] = t
        end
        return res
    end,
    replace = function(str, pattern, rep)
        str = tostring(str)
        local all = wn.regex_find_all(str, tostring(pattern))
        local out, last = {}, 1
        for _, m in ipairs(all) do
            out[#out + 1] = str:sub(last, m[1] - 1)
            local whole = str:sub(m[1], m[2])
            local r
            if type(rep) == 'function' then
                r = rep(whole, select(3, unpack(m)))
            elseif type(rep) == 'table' then
                r = rep[whole]
            else
                r = tostring(rep):gsub('%$(%d)', function(d)
                    d = tonumber(d)
                    if d == 0 then return whole end
                    return m[d + 2] or ''
                end)
            end
            out[#out + 1] = r ~= nil and tostring(r) or whole
            last = m[2] + 1
        end
        out[#out + 1] = str:sub(last)
        return table.concat(out)
    end,
    split = function(str, pattern)
        str = tostring(str)
        local out, last = {}, 1
        for _, m in ipairs(wn.regex_find_all(str, tostring(pattern))) do
            if m[2] >= m[1] then
                out[#out + 1] = str:sub(last, m[1] - 1)
                last = m[2] + 1
            end
        end
        out[#out + 1] = str:sub(last)
        return out
    end,
}, 'windower.regex')

-- Screen and UI sizes: the overlay draws in the game's pixels, so the UI resolution is the same.
function windower.get_windower_settings()
    local w, h = native.ui.screen()
    if not w or w == 0 then w, h = 1920, 1080 end
    return {
        x_res = w, y_res = h, ui_x_res = w, ui_y_res = h,
        window_x_pos = 0, window_y_pos = 0,
        launcher_version = '4.3.0.0', hook_version = '4.3.0.0', version = '4.3.0.0',
        branch = 'stable', profile_name = 'Default', ffxi_version = wn.ffxi_version or native.build,
    }
end

-------------------------------------------------------------------------------- modules

require('xi.windower_ui')(windower, { unsupported = unsupported, stub = stub, partial = partial })
require('xi.windower_ffxi')(windower, { unsupported = unsupported, stub = stub, partial = partial })

-- The packet-derived events and state: windower_events (its own module, may be absent).
local ok, wev = pcall(require, 'xi.windower_events')
if ok and type(wev) == 'table' then
    windower.__events = wev
    if wev.install then
        wev.install({ raise = raise, has = function(name) return has(name:lower()) end, windower = windower, hooks = hooks })
    end
else
    xi.log('windower_events is not available: no packet-derived events (' .. tostring(wev):match('[^\n]*') .. ')')
    -- the chunk events at least, straight from the packets
    local function chunk(dir)
        return function(e)
            local r = fire_chunk(dir, e.id, e.data, e.modified, e.injected, e.blocked)
            if r == true then
                e.blocked = true
            elseif type(r) == 'string' then
                e.modified = r
            end
        end
    end
    on('packet_in', chunk('incoming chunk'))
    on('packet_out', chunk('outgoing chunk'))
end

partial(windower, 'windower')
