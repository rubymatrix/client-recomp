-- The Ashita v4 layer (host/addons/lua/ashita*.lua), exercised the way Ashita addons use it
-- (tests/ashita_api_test.sh). Self-contained: Ashita's own libs are not needed.
addon.name    = 'ashtest';
addon.author  = 'tests';
addon.version = '1.0';

local ffi = require('ffi');

local function say(fmt, ...) print(('ashtest: ' .. fmt):format(...)); end

ashita.events.register('load', 'load_cb', function ()
    say('load: name=%s version=%s path ends with backslash=%s', addon.name, addon.version,
        tostring(addon.path:sub(-1) == '\\'));
    say('install path ends with backslash=%s', tostring(AshitaCore:GetInstallPath():sub(-1) == '\\'));

    -- configuration: the boot config, an ini of our own
    local cfg = AshitaCore:GetConfigurationManager();
    say('boot registry 0003=%d missing=%d language=%d', cfg:GetUInt32('boot', 'ffxi.registry', '0003', 0),
        cfg:GetUInt32('boot', 'ffxi.registry', 'nope', 7), cfg:GetInt32('boot', 'ashita.language', 'ashita', 0));
    cfg:SetValue('ashtest', 'settings', 'value', '42');
    cfg:SetValue('ashtest', 'settings', 'flag', 'true');
    say('saved=%s', tostring(cfg:Save('ashtest', 'ashtest\\ashtest.ini')));
    cfg:Delete('ashtest');
    say('loaded=%s value=%d flag=%s sections=%s', tostring(cfg:Load('again', 'ashtest\\ashtest.ini')),
        cfg:GetInt32('again', 'settings', 'value', 0), tostring(cfg:GetBool('again', 'settings', 'flag', false)),
        cfg:GetSections('again'));

    -- offsets and pointers from the ini files
    say('offset=0x%X', AshitaCore:GetOffsetManager():Get('test.section', 'value'));
    local p = AshitaCore:GetPointerManager():Get('test.pointer');
    say('pointer found=%s guest=%s', tostring(p ~= 0), tostring(p ~= 0 and ashita.memory.read_uint32(p) ~= 0));

    -- resources
    local res = AshitaCore:GetResourceManager();
    local item = res:GetItemById(4096);
    say('item 4096=%s stack=%d', item.Name[1], item.StackSize);
    local spell = res:GetSpellById(1);
    say('spell 1=%s mp=%d whm level=%d', spell.Name[1], spell.ManaCost, spell.LevelRequired[3 + 1]);
    say('ability 1=%s', res:GetAbilityById(1).Name[1]);
    say('string zones.names 230=%s', tostring(res:GetString('zones.names', 230)));
    say('string index of Cure=%d', res:GetString('spells.names', 'Cure'));

    -- memory: the game's code is there to scan
    local found = ashita.memory.find(0, 0, 'A1????????85C05E74????80', 0, 0);
    say('find=%s base=%s', tostring(found ~= 0), tostring(ashita.memory.get_base() ~= 0));
    local buf = ashita.memory.alloc(16);
    ashita.memory.write_uint32(buf, 0x11223344);
    ashita.memory.write_string(buf + 4, 'hi');
    say('memory rw=%08X %s bytes=%s', ashita.memory.read_uint32(buf), ashita.memory.read_string(buf + 4, 8),
        table.concat(ashita.memory.read_array(buf, 4), ','));
    ashita.memory.dealloc(buf);

    -- struct, bits, regex
    local s = struct.pack('<HIf', 0x1234, 0xDEADBEEF, 1.5);
    local a, b, c, nxt = struct.unpack('<HIf', s);
    say('struct %X %X %.1f next=%d size=%d', a, b, c, nxt, struct.size('<HIf'));
    local bytes = { 0x28, 0x08, 0, 0, 0x01, 0x02, 0x03, 0x04 };
    say('bits id=%X size=%d', ashita.bits.unpack_be(bytes, 0, 0, 9), ashita.bits.unpack_be(bytes, 0, 9, 7) * 4);
    local m = ashita.regex.search('call <call12> and <ncall3>', '<((c|nc)all([0-9]+))>');
    say('regex %d matches, first=%s group3=%s', #m, m[1][1], m[1][4]);
    say('regex replace=%s', ashita.regex.replace('a1b22c', '[0-9]+', '#'));
    say('regex split=%s', table.concat(ashita.regex.split('a, b,c', ',\\s*'), '|'));
    say('literal=%s', table.make_literal({ 1, 'two', x = true }));

    -- fonts and primitives
    local f = AshitaCore:GetFontManager():Create('ashtest_font');
    f:SetText('hello');
    f:SetPositionX(10);
    f:SetFontHeight(12);
    f:GetBackground():SetVisible(true);
    say('font text=%s x=%d bg=%s dup=%s', f:GetText(), f:GetPositionX(), tostring(f:GetBackground():GetVisible()),
        tostring(AshitaCore:GetFontManager():Create('ashtest_font')));
    local size = SIZE.new();
    f:GetTextSize(size);
    say('font size known=%s', tostring(size.cx ~= nil));
    local pr = AshitaCore:GetPrimitiveManager():Create('ashtest_prim');
    pr:SetWidth(30);
    pr:SetHeight(20);
    pr:SetPositionX(5);
    say('prim hit=%s miss=%s', tostring(pr:HitTest(10, 10)), tostring(pr:HitTest(50, 50)));

    -- input
    local kb = AshitaCore:GetInputManager():GetKeyboard();
    say('keys F1=%X name=%s vk=%X', kb:S2D('F1'), kb:D2S(0x3B), kb:D2V(0x2A));
    kb:Bind(0x3B, true, false, false, true, false, false, false, false, '/echo bound');
    say('bound=%s', tostring(kb:IsBound(0x3B, true, false, false, true, false, false, false, false)));

    -- plugins
    local pm = AshitaCore:GetPluginManager();
    say('plugins addons=%s other=%s count=%d', tostring(pm:IsLoaded('addons')), tostring(pm:IsLoaded('thing')), pm:Count());

    -- a missing member fails loudly
    local ok, err = pcall(function () return AshitaCore:GetChatManager():NoSuchThing(); end);
    say('loud=%s %s', tostring(not ok), tostring(err):match('IChatManager%.NoSuchThing is not supported yet') or err);

    ashita.tasks.once(0.05, function () say('task ran'); coroutine.sleep(0.05); say('task slept'); end);
end);

ashita.events.register('command', 'command_cb', function (e)
    if (e.command:sub(1, 8) ~= '/ashtest') then return; end
    e.blocked = true;
    say('command %s mode=%d injected=%s', e.command, e.mode, tostring(e.injected));
    if (e.command == '/ashtest sleep') then
        coroutine.sleep(0.05);
        say('command slept');
    end
end);

ashita.events.register('text_in', 'text_in_cb', function (e)
    if (e.message:find('secret')) then e.message_modified = e.message_modified:gsub('secret', 'xxxxxx'); end
    if (e.message:find('block me')) then e.blocked = true; end
end);

ashita.events.register('packet_in', 'packet_in_cb', function (e)
    say('packet_in %03X size=%d chunk=%d injected=%s', e.id, e.size, e.chunk_size, tostring(e.injected));
    if (e.id == 0x0AB) then e.blocked = true; end
    if (e.id == 0x0AC) then
        local p = ffi.cast('uint8_t*', e.data_modified_raw);
        p[4] = 0x5A;
    end
    if (e.id == 0x0AD) then e.data_modified = e.data:sub(1, 5) .. 'Y' .. e.data:sub(7); end
end);

ashita.events.register('key', 'key_cb', function (e)
    say('key wparam=%X up=%s', e.wparam, tostring(bit.band(e.lparam, 0x80000000) ~= 0));
end);

ashita.events.register('key_data', 'key_data_cb', function (e)
    say('key_data %X down=%s', e.key, tostring(e.down));
    if (e.key == 0x3C) then e.blocked = true; end
end);

ashita.events.register('mouse', 'mouse_cb', function (e)
    say('mouse %X at %d,%d', e.message, e.x, e.y);
end);

local presents = 0;
ashita.events.register('d3d_present', 'present_cb', function ()
    presents = presents + 1;
end);

ashita.events.register('d3d_beginscene', 'beginscene_cb', function (isRenderingBackBuffer)
    if (presents == 1) then say('beginscene backbuffer=%s', tostring(isRenderingBackBuffer)); end
end);

ashita.events.register('plugin_event', 'plugin_event_cb', function (e)
    say('plugin_event %s size=%d data=%s', e.name, e.size, e.data);
end);

ashita.events.register('unload', 'unload_cb', function ()
    say('unload after %s presents', presents > 0 and 'some' or 'no');
    AshitaCore:GetFontManager():Delete('ashtest_font');
    AshitaCore:GetPrimitiveManager():Delete('ashtest_prim');
end);
