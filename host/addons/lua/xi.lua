--[==[
The addon runtime, run in every addon's state before anything else (host/addons/core.c).

  local hooks = xi.lua(native, info)

native is the C table (lua_xi.c), info the addon's name, path, kind and roots. It returns the hooks
the host calls: raise(event) for every event, frame() once a frame, load() and unload(). What it
sets up:

  xi.events.on(name, fn[, key[, opts]]) / off(name, key): handlers, in registration order.
      opts.coroutine: run the handler as a coroutine (Windower's handlers may coroutine.sleep).
      A handler returning true, or setting e.blocked, blocks what the event is about.
  xi.tasks: once(delay, fn, ...), oncef(frames, fn, ...), repeating(delay, reps, delay2, fn, ...),
      spawn(fn, ...), cancel(task); coroutine.sleep(seconds), coroutine.sleepf(frames) inside any
      task or coroutine handler.
  print: to the chat log (the layers may change it).
  Paths: io.open, io.lines, os.remove, os.rename, loadfile, dofile, require and lfs take Windows-
      shaped paths (backslashes, the Ashita and Windower install paths) and find the host file.
  ffi.cast of a guest address (a number below 2^32, as read from the game's memory) to a pointer
      type points at the guest's memory.
--]==]

local native, info = ...
local xi = native
xi.info = info

local traceback = debug.traceback
local co_create, co_resume, co_status, co_yield, co_running =
    coroutine.create, coroutine.resume, coroutine.status, coroutine.yield, coroutine.running

-------------------------------------------------------------------------------- paths

local host_path = native.fs.path

do
    local open, lines, remove, rename = io.open, io.lines, os.remove, os.rename
    local lf, df = loadfile, dofile
    io.open = function(p, mode) return open(host_path(p), mode) end
    io.lines = function(p, ...)
        if p == nil then return lines() end
        return lines(host_path(p), ...)
    end
    os.remove = function(p) return remove(host_path(p)) end
    os.rename = function(a, b) return rename(host_path(a), host_path(b)) end
    loadfile = function(p, ...)
        if p == nil then return lf() end
        return lf(host_path(p), ...)
    end
    dofile = function(p)
        if p == nil then return df() end
        return df(host_path(p))
    end
    -- os.execute and io.popen would start programs on the player's machine: not from addons
    os.execute = function() return nil, 'not supported' end
    io.popen = function() return nil, 'not supported' end
end

-- require: the addon's folder, then its kind's libs; Windows paths in package.path still resolve
package.path = table.concat({
    info.path .. '?.lua',
    info.path .. '?/init.lua',
    info.path .. 'libs/?.lua',
    info.root .. 'addons/libs/?.lua',
    info.root .. 'addons/libs/?/init.lua',
    info.root .. 'addons/libs/?/?.lua',
}, ';')
package.cpath = ''
do
    -- a searcher that maps whatever the addon put in package.path (backslashes, drive letters)
    local searchers = package.loaders or package.searchers
    local function mapped(name)
        local tried = {}
        local file = name:gsub('%.', '/')
        for pattern in package.path:gmatch('[^;]+') do
            local p = host_path((pattern:gsub('%?', file)))
            local f = io.open(p, 'rb')
            if f then
                f:close()
                local chunk, err = loadfile(p)
                if not chunk then error(err, 3) end
                return chunk, p
            end
            tried[#tried + 1] = '\n\tno file \'' .. p .. '\''
        end
        return table.concat(tried)
    end
    table.insert(searchers, 2, mapped)
end

-- lfs with the same paths
package.preload['lfs.mapped'] = nil
do
    local ok, lfs = pcall(require, 'lfs')
    if ok and lfs then
        for _, k in ipairs({ 'attributes', 'symlinkattributes', 'dir', 'mkdir', 'rmdir', 'touch', 'chdir' }) do
            local f = lfs[k]
            if f then
                lfs[k] = function(p, ...) return f(host_path(p), ...) end
            end
        end
    end
end

-------------------------------------------------------------------------------- ffi

do
    local ffi = require('ffi')
    local cast = ffi.cast
    local base = native.memory.base()
    local pointer = setmetatable({}, { __mode = 'k' })
    local function is_pointer(ct)
        local known = pointer[ct]
        if known ~= nil then return known end
        local ok, t = pcall(ffi.typeof, ct)
        local r = ok and tostring(t):find('[%*&]%s*>$') ~= nil or false
        if type(ct) ~= 'number' then pointer[ct] = r end
        return r
    end
    ffi.cast = function(ct, v)
        if type(v) == 'number' and v > 0 and v < 4294967296 and is_pointer(ct) then
            v = v + base
        end
        return cast(ct, v)
    end
end

-------------------------------------------------------------------------------- events

local handlers = {} -- name -> list of { key, fn, co }
local tasks = {}    -- running coroutines: { co, wake (clock), wakef (frame), args }
local frame_no = 0

local function report(err, where)
    native.error(tostring(err), where)
end

local events = {}
xi.events = events

function events.on(name, fn, key, opts)
    assert(type(fn) == 'function', 'xi.events.on: a function')
    local list = handlers[name]
    if not list then
        list = {}
        handlers[name] = list
    end
    key = key or fn
    for i = #list, 1, -1 do
        if list[i].key == key then table.remove(list, i) end
    end
    list[#list + 1] = { key = key, fn = fn, co = opts and opts.coroutine }
    return key
end

function events.off(name, key)
    local list = handlers[name]
    if not list then return false end
    for i = #list, 1, -1 do
        if list[i].key == key or list[i].fn == key then
            table.remove(list, i)
            return true
        end
    end
    return false
end

function events.has(name)
    local list = handlers[name]
    return list ~= nil and #list > 0
end

function events.clear()
    handlers = {}
end

-- A coroutine's yield: what it waits for.
local SLEEP, SLEEPF = {}, {}

local function park(co, what, amount)
    if what == SLEEPF then
        tasks[#tasks + 1] = { co = co, wakef = frame_no + math.max(1, amount or 1) }
    else
        tasks[#tasks + 1] = { co = co, wake = native.clock() + (amount or 0) }
    end
end

-- Runs fn as a coroutine now: its result if it finishes, nil if it sleeps (it goes on later).
local function run_co(fn, where, ...)
    local co = co_create(fn)
    local ok, a, b = co_resume(co, ...)
    if not ok then
        report(traceback(co, tostring(a)), where)
        return nil
    end
    if co_status(co) == 'suspended' then
        park(co, a, b)
        return nil
    end
    return a
end

local function call(fn, where, ...)
    local ok, r = xpcall(fn, traceback, ...)
    if not ok then
        report(r, where)
        return nil
    end
    return r
end

-- Raises an event to this addon's handlers: the event table (e) is what the host passes.
local function raise(name, ...)
    local list = handlers[name]
    if not list or #list == 0 then return nil end
    local any
    -- a copy: handlers may register or remove handlers while running
    local copy = { unpack(list) }
    for i = 1, #copy do
        local h = copy[i]
        local r
        if h.co then
            r = run_co(h.fn, name, ...)
        else
            r = call(h.fn, name, ...)
        end
        if r == true then any = true end
    end
    return any
end
events.raise = raise

-------------------------------------------------------------------------------- tasks

function coroutine.sleep(s)
    local co, main = co_running()
    if co == nil or main then error('coroutine.sleep: not inside a coroutine', 2) end
    co_yield(SLEEP, tonumber(s) or 0)
end

function coroutine.sleepf(n)
    local co, main = co_running()
    if co == nil or main then error('coroutine.sleepf: not inside a coroutine', 2) end
    co_yield(SLEEPF, tonumber(n) or 1)
end

local task_api = {}
xi.tasks = task_api

function task_api.spawn(fn, ...)
    local co = co_create(fn)
    local t = { co = co, wake = 0, args = { n = select('#', ...), ... } }
    tasks[#tasks + 1] = t
    return t
end

function task_api.once(delay, fn, ...)
    local args = { n = select('#', ...), ... }
    return task_api.spawn(function()
        coroutine.sleep(delay or 0)
        return fn(unpack(args, 1, args.n))
    end)
end

function task_api.oncef(frames, fn, ...)
    local args = { n = select('#', ...), ... }
    return task_api.spawn(function()
        coroutine.sleepf(frames or 1)
        return fn(unpack(args, 1, args.n))
    end)
end

-- repeating(delay, repeats (0 or nil: forever), interval, fn, ...)
function task_api.repeating(delay, reps, interval, fn, ...)
    local args = { n = select('#', ...), ... }
    local t
    t = task_api.spawn(function()
        coroutine.sleep(delay or 0)
        local n = 0
        while not t.cancelled and (not reps or reps <= 0 or n < reps) do
            fn(unpack(args, 1, args.n))
            n = n + 1
            coroutine.sleep(interval or 0)
        end
    end)
    return t
end

function task_api.repeatingf(delay, reps, interval, fn, ...)
    local args = { n = select('#', ...), ... }
    local t
    t = task_api.spawn(function()
        coroutine.sleepf(delay or 1)
        local n = 0
        while not t.cancelled and (not reps or reps <= 0 or n < reps) do
            fn(unpack(args, 1, args.n))
            n = n + 1
            coroutine.sleepf(interval or 1)
        end
    end)
    return t
end

function task_api.cancel(t)
    if type(t) == 'table' then t.cancelled = true end
end

local function run_tasks()
    frame_no = frame_no + 1
    if #tasks == 0 then return end
    local now = native.clock()
    local list = tasks
    tasks = {}
    for i = 1, #list do
        local t = list[i]
        local due = not t.cancelled and ((t.wakef and frame_no >= t.wakef) or (t.wake and now >= t.wake))
        if t.cancelled then
            -- dropped
        elseif not due then
            tasks[#tasks + 1] = t
        else
            local ok, a, b
            if t.args then
                local args = t.args
                t.args = nil
                ok, a, b = co_resume(t.co, unpack(args, 1, args.n))
            else
                ok, a, b = co_resume(t.co)
            end
            if not ok then
                report(traceback(t.co, tostring(a)), 'task')
            elseif co_status(t.co) == 'suspended' then
                t.wake, t.wakef = nil, nil
                if a == SLEEPF then
                    t.wakef = frame_no + math.max(1, b or 1)
                else
                    t.wake = now + (b or 0)
                end
                tasks[#tasks + 1] = t
            end
        end
    end
end

-------------------------------------------------------------------------------- print

print = function(...)
    local n = select('#', ...)
    local parts = {}
    for i = 1, n do parts[i] = tostring((select(i, ...))) end
    local s = table.concat(parts, ' ')
    for line in (s .. '\n'):gmatch('([^\n]*)\n') do
        native.chat.write(line, 1)
    end
end

-------------------------------------------------------------------------------- hooks

local hooks = {}

-- e: the host's event table (core.c push_event). The layers listen on these names.
function hooks.raise(e)
    local r = raise(e.name, e)
    return r
end

function hooks.frame()
    run_tasks()
    raise('frame')
end

function hooks.load()
    raise('load')
end

function hooks.unload()
    raise('unload')
end

xi.hooks = hooks
return hooks
