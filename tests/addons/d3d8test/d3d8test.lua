--[[
The addons' Direct3D 8, D3DX and Win32 through ffi (host/addons/d3d_ffi.c, win32_ffi.c), driven
through Ashita's own libs/d3d8 declarations. Run by tests/addons/d3d8_ffi.py in the harness
(host64 --addon-harness); prints "PASS <name>" / "FAIL <name>: why", then "DONE <passed>/<total>".
--]]

package.path = xi.paths.ashita .. 'addons/libs/?.lua;' .. package.path

local ffi = require('ffi')
require('d3d8')
local C = ffi.C

ffi.cdef[[
    short GetKeyState(int nVirtKey);
    short GetAsyncKeyState(int vKey);
    DWORD GetTickCount(void);
    DWORD timeGetTime(void);
    BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
    BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
    int GetSystemMetrics(int nIndex);
    HANDLE GetCurrentProcess(void);
    BOOL GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
    BOOL SetProcessWorkingSetSize(HANDLE hProcess, SIZE_T dwMinimumWorkingSetSize, SIZE_T dwMaximumWorkingSetSize);
    HWND GetForegroundWindow();
    HWND GetFocus();
    BOOL GetCursorPos(LPPOINT lpPoint);
    DWORD GetCurrentThreadId(void);
    HMODULE GetModuleHandleA(const char* lpModuleName);
    typedef struct FILE FILE;
    FILE* fopen(const char* path, const char* mode);
    int fclose(FILE* f);
]]

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

local dev = ffi.cast('IDirect3DDevice8*', xi.d3d8_device())

-- layouts: LONG/DWORD are 32 bits in the ffi (Windows'), as the host's structs
try('layout', function()
    check('layout RECT', ffi.sizeof('RECT') == 16, ffi.sizeof('RECT'))
    check('layout D3DSURFACE_DESC', ffi.sizeof('D3DSURFACE_DESC') == 32, ffi.sizeof('D3DSURFACE_DESC'))
    check('layout D3DCAPS8', ffi.sizeof('D3DCAPS8') == 212, ffi.sizeof('D3DCAPS8'))
end)

try('device', function()
    local res, vp = dev:GetViewport()
    check('GetViewport', res == C.S_OK and vp.Width == 1920 and vp.Height == 1080 and vp.MaxZ == 1, res)
    local r2, caps = dev:GetDeviceCaps()
    check('GetDeviceCaps', r2 == C.S_OK and caps.MaxTextureWidth == 16384 and caps.MaxSimultaneousTextures == 8, r2)
    local r3, mode = dev:GetDisplayMode()
    check('GetDisplayMode', r3 == C.S_OK and mode.Width == 1920 and mode.Format == C.D3DFMT_X8R8G8B8, r3)
    check('SetRenderState', dev:SetRenderState(C.D3DRS_ZENABLE, 1) == C.S_OK)
    local r4, v = dev:GetRenderState(C.D3DRS_ZENABLE)
    check('GetRenderState', r4 == C.S_OK and v == 1, v)
    local r5, d3d = dev:GetDirect3D()
    check('GetDirect3D', r5 == C.S_OK and d3d ~= nil and d3d:GetAdapterCount() == 1, r5)
    check('BeginScene', dev:BeginScene() == C.S_OK and dev:EndScene() == C.S_OK)
    -- a method nobody implements: E_NOTIMPL, never a crash
    local r6 = dev.lpVtbl.CreateCubeTexture(dev, 16, 1, 0, C.D3DFMT_A8R8G8B8, C.D3DPOOL_MANAGED, ffi.new('IDirect3DCubeTexture8*[1]'))
    check('unimplemented', r6 == C.E_NOTIMPL, string.format('%08x', r6))
end)

try('texture', function()
    local pp = ffi.new('IDirect3DTexture8*[1]')
    local res = dev.lpVtbl.CreateTexture(dev, 64, 32, 1, 0, C.D3DFMT_A8R8G8B8, C.D3DPOOL_MANAGED, pp)
    check('CreateTexture', res == C.S_OK and pp[0] ~= nil, res)
    local tex = pp[0]
    local r1, desc = tex:GetLevelDesc(0)
    check('GetLevelDesc', r1 == C.S_OK and desc.Width == 64 and desc.Height == 32 and desc.Format == C.D3DFMT_A8R8G8B8 and desc.Pool == C.D3DPOOL_MANAGED, r1)
    check('GetLevelCount', tex:GetLevelCount() == 1)
    local r2, lr = tex:LockRect(0, nil, 0)
    check('LockRect', r2 == C.S_OK and lr.Pitch == 256 and lr.pBits ~= nil, r2)
    local px = ffi.cast('uint32_t*', lr.pBits)
    for i = 0, 64 * 32 - 1 do px[i] = 0xFF00FF00 end
    check('UnlockRect', tex:UnlockRect(0) == C.S_OK)
    local r3, surf = tex:GetSurfaceLevel(0)
    check('GetSurfaceLevel', r3 == C.S_OK and surf ~= nil)
    local r4, sd = surf:GetDesc()
    check('Surface GetDesc', r4 == C.S_OK and sd.Width == 64 and sd.Height == 32, r4)
    local r5, slr = surf:LockRect(nil, 0)
    check('Surface LockRect', r5 == C.S_OK and ffi.cast('uint32_t*', slr.pBits)[5] == 0xFF00FF00, r5)
    surf:UnlockRect()
    surf:Release()
    -- the number addons hand ImGui is the texture
    local id = tonumber(ffi.cast('uint32_t', tex))
    check('texture id', id == xi.d3d8_texture_id(tex) and id >= 0x10000000, id)
    check('Release', tex:Release() == 0)
    -- 16-bit formats lock in their own layout
    res = dev.lpVtbl.CreateTexture(dev, 8, 8, 0, 0, C.D3DFMT_R5G6B5, C.D3DPOOL_MANAGED, pp)
    local _, lr2 = pp[0]:LockRect(0, nil, 0)
    check('R5G6B5 pitch', res == C.S_OK and lr2.Pitch == 16 and pp[0]:GetLevelCount() == 4, lr2 and lr2.Pitch)
    pp[0]:UnlockRect(0)
    pp[0]:Release()
end)

-- an 8 bpp DIB as FFXI's icons are: BITMAPINFOHEADER, palette (alpha 0..0x80 in the spare byte), bottom-up rows
local function dib()
    local w, h = 2, 2
    local s = ffi.new('uint8_t[?]', 40 + 256 * 4 + 4 * h)
    local hdr = ffi.cast('int32_t*', s)
    hdr[0] = 40; hdr[1] = w; hdr[2] = h
    ffi.cast('uint16_t*', s + 12)[0] = 1
    ffi.cast('uint16_t*', s + 14)[0] = 8
    local pal = s + 40
    pal[4 + 2] = 0xFF; pal[4 + 3] = 0x80             -- 1: red, opaque (FFXI's 0x80)
    pal[8 + 1] = 0xFF; pal[8 + 3] = 0x40             -- 2: green, half
    pal[12 + 3] = 0x80                                -- 3: black, opaque (the colour key takes it)
    local bits = s + 40 + 256 * 4
    bits[0], bits[1] = 3, 0                           -- bottom row: black, transparent
    bits[4], bits[5] = 1, 2                           -- top row: red, green
    return s, ffi.sizeof(s)
end

try('d3dx memory', function()
    local data, n = dib()
    local pp = ffi.new('IDirect3DTexture8*[1]')
    local info = ffi.new('D3DXIMAGE_INFO[1]')
    local res = C.D3DXCreateTextureFromFileInMemoryEx(dev, data, n, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0, C.D3DFMT_A8R8G8B8,
        C.D3DPOOL_MANAGED, C.D3DX_DEFAULT, C.D3DX_DEFAULT, 0xFF000000, info, nil, pp)
    check('D3DXCreateTextureFromFileInMemoryEx', res == C.S_OK and info[0].Width == 2 and info[0].ImageFileFormat == C.D3DXIFF_DIB, string.format('%08x', res))
    local _, lr = pp[0]:LockRect(0, nil, 0)
    local p = ffi.cast('uint32_t*', lr.pBits)
    local pitch = lr.Pitch / 4
    check('DIB pixels', p[0] == 0xFFFF0000 and p[1] == 0x8000FF00 and p[pitch] == 0 and p[pitch + 1] == 0,
        string.format('%08x %08x %08x %08x', p[0], p[1], p[pitch], p[pitch + 1]))
    pp[0]:UnlockRect(0)
    pp[0]:Release()
    -- DXT1 in a DDS: one 4x4 block, colour 0 white, all texels index 0
    local dds = ffi.new('uint8_t[?]', 128 + 8)
    ffi.copy(dds, 'DDS ', 4)
    local u = ffi.cast('uint32_t*', dds)
    u[1] = 124; u[2] = 0x1007; u[3] = 4; u[4] = 4; u[7] = 1
    u[19] = 32; u[20] = 4; u[21] = 0x31545844
    local blk = dds + 128
    blk[0], blk[1], blk[2], blk[3] = 0xFF, 0xFF, 0, 0
    res = C.D3DXCreateTextureFromFileInMemory(dev, dds, 136, pp)
    local _, lr3 = pp[0]:LockRect(0, nil, 0)
    check('DDS DXT1', res == C.S_OK and ffi.cast('uint32_t*', lr3.pBits)[5] == 0xFFFFFFFF, string.format('%08x', res))
    pp[0]:UnlockRect(0)
    pp[0]:Release()
    check('bad data', C.D3DXCreateTextureFromFileInMemory(dev, 'nonsense', 8, pp) ~= C.S_OK)
end)

try('d3dx file', function()
    -- a 24 bpp BMP written next to the addon, read back through a Windows-shaped path
    local w, h = 3, 2
    local stride = 12
    local size = 54 + stride * h
    local b = ffi.new('uint8_t[?]', size)
    b[0], b[1] = 66, 77
    ffi.cast('uint32_t*', b + 2)[0] = size
    ffi.cast('uint32_t*', b + 10)[0] = 54
    local ih = ffi.cast('int32_t*', b + 14)
    ih[0] = 40; ih[1] = w; ih[2] = h
    ffi.cast('uint16_t*', b + 26)[0] = 1
    ffi.cast('uint16_t*', b + 28)[0] = 24
    for i = 0, stride * h - 1 do b[54 + i] = 0x80 end
    local path = xi.info.path .. 'test.bmp'
    local f = io.open(path, 'wb')
    f:write(ffi.string(b, size))
    f:close()
    local winpath = path:gsub('/', '\\')
    local pp = ffi.new('IDirect3DTexture8*[1]')
    local res = C.D3DXCreateTextureFromFileA(dev, winpath, pp)
    check('D3DXCreateTextureFromFileA', res == C.S_OK, string.format('%08x', res))
    local _, desc = pp[0]:GetLevelDesc(0)
    check('BMP size', desc.Width == 3 and desc.Height == 2, desc.Width)
    pp[0]:Release()
    local info = ffi.new('D3DXIMAGE_INFO[1]')
    res = C.D3DXGetImageInfoFromFileA(path, info)
    check('D3DXGetImageInfoFromFileA', res == C.S_OK and info[0].Width == 3 and info[0].ImageFileFormat == C.D3DXIFF_BMP, res)
    check('missing file', C.D3DXCreateTextureFromFileA(dev, 'C:\\nowhere\\none.png', pp) ~= C.S_OK)
    -- explicit size: scaled
    res = C.D3DXCreateTextureFromFileExA(dev, path, 6, 4, 1, 0, C.D3DFMT_A8R8G8B8, C.D3DPOOL_MANAGED, C.D3DX_DEFAULT, C.D3DX_DEFAULT, 0, nil, nil, pp)
    local _, d2 = pp[0]:GetLevelDesc(0)
    check('D3DXCreateTextureFromFileExA size', res == C.S_OK and d2.Width == 6 and d2.Height == 4, d2.Width)
    pp[0]:Release()
    -- ffi.C.fopen takes the Windows-shaped path too
    local fp = C.fopen(winpath, 'rb')
    check('ffi.C.fopen path', fp ~= nil)
    if fp ~= nil then C.fclose(fp) end
    os.remove(path)
end)

try('state blocks', function()
    local tok = ffi.new('DWORD[1]')
    dev:SetRenderState(C.D3DRS_SRCBLEND, C.D3DBLEND_SRCALPHA)
    check('CreateStateBlock', dev.lpVtbl.CreateStateBlock(dev, C.D3DSBT_ALL, tok) == C.S_OK)
    dev:SetRenderState(C.D3DRS_SRCBLEND, C.D3DBLEND_ONE)
    dev.lpVtbl.ApplyStateBlock(dev, tok[0])
    local _, v = dev:GetRenderState(C.D3DRS_SRCBLEND)
    check('ApplyStateBlock', v == C.D3DBLEND_SRCALPHA, v)
    check('DeleteStateBlock', dev.lpVtbl.DeleteStateBlock(dev, tok[0]) == C.S_OK)
end)

try('win32', function()
    check('GetTickCount', C.GetTickCount() > 0 and C.timeGetTime() > 0)
    local li = ffi.new('LARGE_INTEGER[1]')
    check('QueryPerformanceCounter', C.QueryPerformanceCounter(li) == 1 and li[0].QuadPart > 0)
    check('QueryPerformanceFrequency', C.QueryPerformanceFrequency(li) == 1 and li[0].QuadPart == 1000000000)
    check('GetKeyState', C.GetKeyState(0x41) == 0 and C.GetAsyncKeyState(0x01) == 0)
    local m, s = ffi.new('DWORD_PTR[1]'), ffi.new('DWORD_PTR[1]')
    check('GetProcessAffinityMask', C.GetProcessAffinityMask(C.GetCurrentProcess(), m, s) == 1 and m[0] ~= 0, m[0])
    check('SetProcessWorkingSetSize', C.SetProcessWorkingSetSize(C.GetCurrentProcess(), -1, -1) == 1)
    check('GetForegroundWindow', C.GetForegroundWindow() == nil and C.GetFocus() == nil) -- no window in the harness
    local pt = ffi.new('POINT[1]')
    check('GetCursorPos', C.GetCursorPos(pt) == 1)
    check('GetCurrentThreadId', C.GetCurrentThreadId() ~= 0)
    check('GetSystemMetrics', C.GetSystemMetrics(0) >= 0)
    check('GetModuleHandleA', C.GetModuleHandleA(nil) ~= nil)
end)

-- drawing: into the overlay's list, in the frame (Ashita's d3d_present)
local sprite, font, tex
try('objects', function()
    local sp = ffi.new('ID3DXSprite*[1]')
    check('D3DXCreateSprite', C.D3DXCreateSprite(dev, sp) == C.S_OK)
    sprite = sp[0]
    local lf = ffi.new('LOGFONTA')
    lf.lfHeight = -14
    lf.lfWeight = 700
    ffi.copy(lf.lfFaceName, 'Arial')
    local fp = ffi.new('ID3DXFont*[1]')
    check('D3DXCreateFontIndirect', C.D3DXCreateFontIndirect(dev, lf, fp) == C.S_OK)
    font = fp[0]
    local tp = ffi.new('IDirect3DTexture8*[1]')
    dev.lpVtbl.CreateTexture(dev, 32, 32, 1, 0, C.D3DFMT_A8R8G8B8, C.D3DPOOL_MANAGED, tp)
    tex = tp[0]
end)

ffi.cdef[[
    typedef struct { float x, y, z, rhw; D3DCOLOR color; } TESTVTX_RHW;
    typedef struct { float x, y, z; D3DCOLOR color; } TESTVTX_XYZ;
]]

local frames = 0
xi.events.on('present', function()
    frames = frames + 1
    if frames ~= 2 then return end
    try('draw', function()
        local _, v0 = xi.d3d8_stats()
        sprite:Begin()
        local rect = ffi.new('RECT', { 0, 0, 32, 32 })
        local scale = ffi.new('D3DXVECTOR2', { 2, 2 })
        local pos = ffi.new('D3DXVECTOR2', { 100, 100 })
        check('Sprite Draw', sprite:Draw(tex, rect, scale, nil, 0, pos, 0xFFFFFFFF) == C.S_OK)
        sprite:End()
        local _, v1 = xi.d3d8_stats()
        check('sprite recorded', v1 - v0 == 6, v1 - v0)

        local q = ffi.new('TESTVTX_RHW[6]')
        local pts = { { 0, 0 }, { 50, 0 }, { 0, 50 }, { 50, 0 }, { 50, 50 }, { 0, 50 } }
        for i = 1, 6 do
            q[i - 1].x, q[i - 1].y, q[i - 1].z, q[i - 1].rhw, q[i - 1].color = pts[i][1], pts[i][2], 0, 1, 0x80FF0000
        end
        dev:SetVertexShader(bit.bor(C.D3DFVF_XYZRHW, C.D3DFVF_DIFFUSE))
        dev:SetTexture(0, nil)
        check('DrawPrimitiveUP', dev:DrawPrimitiveUP(C.D3DPT_TRIANGLELIST, 2, q, ffi.sizeof('TESTVTX_RHW')) == C.S_OK)
        local _, v2 = xi.d3d8_stats()
        check('primitives recorded', v2 - v1 == 6, v2 - v1)

        -- world space through set transforms: a line in front of the camera, and one behind it
        local ident = ffi.new('D3DMATRIX')
        ident._11, ident._22, ident._33, ident._44 = 1, 1, 1, 1
        dev:SetTransform(C.D3DTS_WORLD, ident)
        dev:SetTransform(C.D3DTS_VIEW, ident)
        dev:SetTransform(C.D3DTS_PROJECTION, ident)
        dev:SetVertexShader(bit.bor(C.D3DFVF_XYZ, C.D3DFVF_DIFFUSE))
        local l = ffi.new('TESTVTX_XYZ[4]')
        l[0].x, l[0].y, l[0].z, l[0].color = -0.5, 0, 0.5, 0xFFFFFFFF
        l[1].x, l[1].y, l[1].z, l[1].color = 0.5, 0, 0.5, 0xFFFFFFFF
        l[2].x, l[2].y, l[2].z, l[2].color = -0.5, 0, -1, 0xFFFFFFFF
        l[3].x, l[3].y, l[3].z, l[3].color = 0.5, 0, -1, 0xFFFFFFFF
        check('DrawPrimitiveUP lines', dev:DrawPrimitiveUP(C.D3DPT_LINELIST, 2, l, ffi.sizeof('TESTVTX_XYZ')) == C.S_OK)
        local _, v3 = xi.d3d8_stats()
        check('near plane clip', v3 - v2 == 6, v3 - v2)
        local _, m = dev:GetTransform(C.D3DTS_WORLD)
        check('GetTransform', m ~= nil and m._11 == 1 and m._41 == 0)

        local r = ffi.new('RECT', { 10, 10, 0, 0 })
        local hgt = font:DrawTextA('Hello\nworld', -1, r, 0x400, 0xFFFFFFFF) -- DT_CALCRECT
        check('DrawTextA calcrect', hgt > 20 and r.right > 10 and r.bottom > 10 + 20, hgt)
        local _, v4 = xi.d3d8_stats()
        font:DrawTextA('Hello', -1, r, 0, 0xFFFFFFFF)
        local _, v5 = xi.d3d8_stats()
        check('DrawTextA recorded', v5 > v4, v5 - v4)
    end)
end)

xi.events.on('unload', function()
    print(('DONE %d/%d'):format(passed, total))
end)

xi.tasks.oncef(4, function()
    sprite:Release()
    font:Release()
    tex:Release()
    collectgarbage()
    print(('DONE %d/%d'):format(passed, total))
end)
