"""Generate host/addons/gui_lua_gen.inc: the Lua bindings of Ashita v4's IGuiManager (imgui.* in addons).

  python3 tools/gen_imgui_lua.py [--ashita DIR] [--report]

Two inputs:
  - Ashita's LuaLS annotations (addons/libs/annotations/SDK/IGuiManager.lua, IGuiManagerTypes.lua): the
    Lua-facing spec. They list every function/method with its Lua parameter names and types, overloads
    as repeated declarations, and whether an ImVec2 comes back as two numbers or as an object.
  - Our vendored third_party/imgui/imgui.h (the exact commit Ashita ships): C++ parameter types and
    default values, and the data members of every struct (for the live objects: io, style, fonts ...).

For every annotated function the generator pairs each annotated overload with an imgui.h overload
(position by position; C++ parameters Lua can't express, like a trailing text_end, are filled in)
and writes a C++ binding that converts the Lua arguments, calls ImGui and pushes the results.
Functions whose shapes need more than that (buffers, item lists, scalar data types, callbacks, fonts,
ambiguous overloads) are hand-written in host/addons/gui_lua.cpp as hand_<Name> / hand_<Class>_<Method>;
the generator only references them, so a missing hand function is a compile error.

The .inc has two parts, selected by the includer:
  XI_GUI_GEN_TRAITS  class ids + ClassOf<T> (needed by the runtime templates and the hand code)
  XI_GUI_GEN_BODY    generated functions, field accessors, method/field/function tables
--report prints the coverage (bound / hand-written / not bound, with reasons).
"""
import argparse
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_ASHITA = os.path.join(os.path.dirname(ROOT), 'addon-deps', 'Ashita-v4beta')
IMGUI_H = os.path.join(ROOT, 'third_party', 'imgui', 'imgui.h')
OUT = os.path.join(ROOT, 'host', 'addons', 'gui_lua_gen.inc')

# ---------------------------------------------------------------------------------------------------
# Policy tables (the only hand-maintained part of the generator).

# IGuiManager functions implemented in gui_lua.cpp as hand_<Name>.
HAND = {
    'GetVisible', 'SetVisible', 'CreateContext', 'DestroyContext', 'SetCurrentContext',
    'NewFrame', 'EndFrame', 'Render', 'UpdatePlatformWindows', 'RenderPlatformWindowsDefault',
    'DestroyPlatformWindows', 'SetNextWindowSizeConstraints', 'PushFont', 'GetColorU32', 'PushID',
    'GetID', 'CheckboxFlags', 'Combo', 'DragScalar', 'DragScalarN', 'SliderScalar', 'SliderScalarN',
    'VSliderScalar', 'InputScalar', 'InputScalarN', 'InputText', 'InputTextMultiline',
    'InputTextWithHint', 'ColorPicker4', 'ListBox', 'PlotLines', 'PlotHistogram', 'Value',
    'SetDragDropPayload', 'ColorConvertRGBtoHSV', 'ColorConvertHSVtoRGB', 'SaveIniSettingsToMemory',
    'MemAlloc', 'MemFree', 'AddFontFromFileTTF', 'AddFontFromMemoryCompressedTTF',
    'AddFontFromMemoryCompressedBase85TTF', 'BeginMenuEx', 'MenuItemEx',
}
# Class methods implemented in gui_lua.cpp as hand_<Class>_<Method>.
HAND_METHODS = {
    ('ImDrawList', 'AddPolyline'), ('ImDrawList', 'AddConvexPolyFilled'),
    ('ImDrawList', 'AddConcavePolyFilled'), ('ImFontAtlas', 'AddFontFromMemoryTTF'),
    ('ImFontAtlas', 'AddFontFromMemoryCompressedTTF'), ('ImFont', 'RenderText'),
}
# Defined by Ashita's libs/imgui.lua itself on its own table, not by the manager.
LIB_HELPERS = {'col32', 'ShowHelp', 'DisplayPopup'}

# Functions usable outside an ImGui frame (between frames / from load or command events). Everything
# else returns its defaults and does nothing unless xi_gui_in_frame() says a frame is open.
NO_GUARD = {
    'GetIO', 'GetPlatformIO', 'GetStyle', 'GetVersion', 'GetDrawData', 'GetCurrentContext',
    'StyleColorsDark', 'StyleColorsLight', 'StyleColorsClassic', 'GetStyleColorVec4',
    'GetStyleColorName', 'ColorConvertU32ToFloat4', 'ColorConvertFloat4ToU32', 'GetTime',
    'GetFrameCount', 'GetKeyName', 'IsKeyDown', 'IsKeyPressed', 'IsKeyReleased', 'IsKeyChordPressed',
    'GetKeyPressedAmount', 'IsMouseDown', 'IsMouseClicked', 'IsMouseReleased', 'IsMouseDoubleClicked',
    'IsMouseReleasedWithDelay', 'GetMouseClickedCount', 'IsAnyMouseDown', 'GetMousePos',
    'IsMousePosValid', 'IsMouseDragging', 'GetMouseDragDelta', 'GetMouseCursor', 'GetClipboardText',
    'SetClipboardText', 'LoadIniSettingsFromDisk', 'LoadIniSettingsFromMemory', 'SaveIniSettingsToDisk',
    'GetMainViewport', 'FindViewportByID', 'FindViewportByPlatformHandle', 'DebugCheckVersionAndDataLayout',
    'SetNextFrameWantCaptureKeyboard', 'SetNextFrameWantCaptureMouse', 'GetDrawListSharedData',
}
# Preconditions ImGui asserts and then relies on (a NULL dereference after our logging IM_ASSERT): an
# addon misusing these must not take the game down, so the binding checks first and does nothing.
PRECOND = {
    'SetColumnOffset': 'cur_window() && cur_window()->DC.CurrentColumns',
    'SetColumnWidth': 'cur_window() && cur_window()->DC.CurrentColumns',
    'Begin': 'nonempty_str(L, 1)',  # Begin('') spins forever inside ImGui
    'BeginTable': '!is_none(L, 2) && range_ok(L, 2, 1, IMGUI_TABLE_MAX_COLUMNS)',  # 0 columns corrupts memory
    'Columns': 'range_ok(L, 1, 1, 64)',
}
for _n in ('TableNextRow', 'TableNextColumn', 'TableSetColumnIndex', 'TableSetupColumn', 'TableSetupScrollFreeze',
           'TableHeader', 'TableHeadersRow', 'TableAngledHeadersRow', 'TableGetSortSpecs', 'TableGetColumnCount',
           'TableGetColumnIndex', 'TableGetRowIndex', 'TableGetColumnName', 'TableGetColumnFlags',
           'TableSetColumnEnabled', 'TableGetHoveredColumn', 'TableSetBgColor'):
    PRECOND[_n] = 'GImGui->CurrentTable'
# Index-like first arguments ImGui uses to index arrays after an assert (legacy key codes from old
# addons, out-of-range enums): out of range, the call is skipped.
for _n in ('IsKeyDown', 'IsKeyPressed', 'IsKeyReleased', 'GetKeyPressedAmount', 'GetKeyName', 'SetItemKeyOwner'):
    PRECOND[_n] = 'key_ok(L, 1)'
for _n in ('IsKeyChordPressed', 'Shortcut', 'SetNextItemShortcut'):
    PRECOND[_n] = 'chord_ok(L, 1)'
for _n in ('IsMouseDown', 'IsMouseClicked', 'IsMouseReleased', 'IsMouseDoubleClicked', 'IsMouseReleasedWithDelay',
           'GetMouseClickedCount', 'IsMouseDragging', 'GetMouseDragDelta', 'ResetMouseDragDelta', 'IsItemClicked'):
    PRECOND[_n] = 'range_ok(L, 1, 0, ImGuiMouseButton_COUNT)'
for _n in ('PushStyleColor', 'GetStyleColorVec4', 'DebugFlashStyleColor'):
    PRECOND[_n] = 'range_ok(L, 1, 0, ImGuiCol_COUNT)'
for _n in ('PushStyleVar', 'PushStyleVarX', 'PushStyleVarY'):
    PRECOND[_n] = 'range_ok(L, 1, 0, ImGuiStyleVar_COUNT)'
# Classes whose methods draw: guarded like the widget functions.
GUARDED_CLASSES = {'ImDrawList'}

# Classes exposed as live objects. Types annotated by Ashita plus the ones their fields reach.
EXTRA_CLASSES = ['ImVec2', 'ImVec4', 'ImGuiContext', 'ImGuiTextRange', 'ImFontAtlasCustomRect']
# Opaque (internal, incomplete in imgui.h; we include imgui_internal.h so sizeof works).
INTERNAL_CLASSES = {'ImGuiContext', 'ImDrawListSharedData', 'ImFontAtlasBuilder', 'ImFontLoader'}

# Annotation type names that are Lua tables / strings etc. but spelled oddly.
ANN_TYPE_FIX = {'size': 'table', 'tables': 'table', 'buffer': 'table', 'item_index': 'number',
                'ImGuiCOnd': 'number', 'IGuiWindowClass': 'ImGuiWindowClass'}

# ---------------------------------------------------------------------------------------------------
# Annotation parsing.


class AnnFunc:
    def __init__(self, owner, name, colon):
        self.owner, self.name, self.colon = owner, name, colon
        self.params = []      # (name, type, optional)
        self.returns = []     # (type, name)
        self.deprecated = False
        self.not_impl = False


def parse_annotations(path):
    funcs, classes, aliases = [], {}, {}
    pend_params, pend_returns, deprecated, not_impl = [], [], False, False
    cur_class = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            m = re.match(r'---@alias\s+(\S+)\s+(\S+)', line)
            if m:
                aliases[m.group(1)] = m.group(2)
                continue
            m = re.match(r'---@class\s+(\w+)', line)
            if m:
                cur_class = m.group(1)
                classes.setdefault(cur_class, [])
                continue
            m = re.match(r'---@field\s+(\w+)\s+(\S+)', line)
            if m and cur_class:
                classes[cur_class].append((m.group(1), m.group(2)))
                continue
            m = re.match(r'---@param\s+(\w+)(\??)\s+(\S+)', line)
            if m:
                if m.group(1) != 'self':
                    pend_params.append((m.group(1), m.group(3), m.group(2) == '?'))
                continue
            m = re.match(r'---@return\s+(\S+)\s*(\w*)', line)
            if m:
                pend_returns.append((m.group(1), m.group(2)))
                continue
            if line.startswith('---@deprecated'):
                deprecated = True
                continue
            if line.startswith('---Not implemented'):
                not_impl = True
                continue
            m = re.match(r'function\s+(\w+)([.:])(\w+)\s*\(([^)]*)\)', line)
            if m:
                fn = AnnFunc(m.group(1), m.group(3), m.group(2) == ':')
                sig_names = [a.strip() for a in m.group(4).split(',') if a.strip()]
                by_name = {p[0]: p for p in pend_params}
                for i, n in enumerate(sig_names):
                    if n in by_name:
                        fn.params.append(by_name[n])
                    elif i < len(pend_params):
                        p = pend_params[i]
                        fn.params.append((n, p[1], p[2]))
                    else:
                        fn.params.append((n, 'any', True))
                fn.returns = pend_returns
                fn.deprecated, fn.not_impl = deprecated, not_impl
                funcs.append(fn)
                pend_params, pend_returns, deprecated, not_impl = [], [], False, False
                continue
            if not line.startswith('---'):
                pend_params, pend_returns, deprecated, not_impl = [], [], False, False
    return funcs, classes, aliases


# ---------------------------------------------------------------------------------------------------
# imgui.h parsing.


class Param:
    def __init__(self, ctype, name, dim, default):
        self.ctype, self.name, self.dim, self.default = ctype, name, dim, default


class CppFunc:
    def __init__(self, owner, ret, name, params, static=False):
        self.owner, self.ret, self.name, self.params, self.static = owner, ret, name, params, static


def split_top(s, sep=','):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def norm_type(t):
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*([*&])', r'\1', t)
    return t


def parse_params(s):
    s = s.strip()
    if s in ('', 'void'):
        return []
    out = []
    for p in split_top(s):
        p = p.strip()
        if p == '...':
            out.append(Param('...', '...', None, None))
            continue
        default = None
        parts = split_top(p, '=')
        if len(parts) > 1:
            p, default = parts[0].strip(), '='.join(parts[1:]).strip()
        if '(*' in p:
            out.append(Param('FNPTR', 'fnptr', None, default))
            continue
        m = re.match(r'^(.*?)(\w+)\s*((?:\[[^\]]*\])*)$', p)
        if not m:
            out.append(Param('UNKNOWN', 'x', None, default))
            continue
        ctype, name, dims = norm_type(m.group(1)), m.group(2), m.group(3)
        dim = None
        if dims:
            dm = re.match(r'\[(\w*)\]', dims)
            dim = dm.group(1) if dm else ''
        out.append(Param(ctype, name, dim, default))
    return out


HEAD_RE = re.compile(r'^\s*(?:IMGUI_API\s+)?(?:static\s+)?(?:inline\s+)?(?:IMGUI_API\s+)?'
                     r'([\w:<>\*&\s]+?)\s*\b(\w+)\s*\(')
TAIL_RE = re.compile(r'^\s*(?:const)?\s*(?:IM_FMT\w+\([^)]*\))?\s*[;{]')


class FuncMatch:
    def __init__(self, ret, name, params):
        self.ret, self.name, self.params = ret, name, params

    def group(self, i):
        return (None, self.ret, self.name, self.params)[i]


def match_func(l):
    """Match 'ret Name(params) [const] [IM_FMTARGS(n)] ;|{' with balanced parentheses."""
    m = HEAD_RE.match(l)
    if not m:
        return None
    j, depth = m.end(), 1
    while j < len(l) and depth:
        if l[j] == '(':
            depth += 1
        elif l[j] == ')':
            depth -= 1
        j += 1
    if depth or not TAIL_RE.match(l[j:]):
        return None
    return FuncMatch(m.group(1), m.group(2), l[m.end():j - 1])


def parse_imgui_h(path):
    text = open(path, encoding='utf-8').read()
    # Strip comments (keep line structure).
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    lines = [re.sub(r'//.*$', '', l) for l in text.split('\n')]
    funcs = {}        # (owner, name) -> [CppFunc]
    structs = {}      # name -> {'members': [(name, bitfield)], 'methods': ...}
    typedef_scalars = set()
    enums = set()
    for l in lines:
        m = re.match(r'\s*typedef\s+(signed |unsigned )?(int|short|char|long long|ImS64|ImU64|ImU8|ImS8|ImU16|ImS16|ImU32|ImS32|ImWchar16|ImWchar32)\s+(\w+)\s*;', l)
        if m:
            typedef_scalars.add(m.group(3))
        m = re.match(r'\s*enum\s+(\w+)', l)
        if m:
            enums.add(m.group(1))
    i = 0
    n = len(lines)
    while i < n:
        l = lines[i]
        if re.match(r'^namespace ImGui\s*$', l):
            i += 1
            depth = 0
            while i < n:
                l = lines[i]
                depth += l.count('{') - l.count('}')
                if depth <= 0 and '}' in l:
                    break
                if 'IMGUI_API' in l or re.match(r'^\s*(static\s+)?inline\b', l):
                    m = match_func(l)
                    if m:
                        f = CppFunc(None, norm_type(m.group(1)), m.group(2), parse_params(m.group(3)))
                        funcs.setdefault((None, f.name), []).append(f)
                i += 1
            i += 1
            continue
        m = re.match(r'^(?:IM_MSVC_RUNTIME_CHECKS_OFF\s+)?struct\s+(\w+)\s*$', l)
        if m and i + 1 < n and lines[i + 1].strip().startswith('{'):
            sname = m.group(1)
            members, depth = [], 0
            i += 1
            while i < n:
                l = re.sub(r'\[[^\]]*\]', '[]', lines[i])
                opens, closes = l.count('{'), l.count('}')
                if depth == 1 and '(' not in l and l.strip().endswith(';') and \
                        not re.match(r'^\s*(typedef|static|friend|enum|struct|union|using|return|IM_|template)\b', l):
                    decls = split_top(l.strip()[:-1])
                    dm = re.match(r'^(.*?[\s\*&])(\w+)\s*(\[[^\]]*\])?\s*(:\s*\d+)?\s*(=.*)?$', decls[0].strip()) if decls else None
                    if dm and dm.group(1).strip() and dm.group(1).strip() not in ('return', 'else'):
                        ctype = norm_type(dm.group(1))
                        members.append((dm.group(2), bool(dm.group(4)), ctype))
                        for d in decls[1:]:
                            dm2 = re.match(r'^\s*[\*&]*\s*(\w+)\s*(\[[^\]]*\])?\s*(:\s*\d+)?\s*$', d)
                            if dm2:
                                members.append((dm2.group(1), bool(dm2.group(3)), ctype))
                if depth == 1 and '(' in l:
                    m2 = match_func(l)
                    if m2 and m2.group(2) != sname and not m2.group(1).strip().endswith('operator') \
                            and 'operator' not in l.split('(')[0] and m2.group(1).strip() not in ('', 'return', 'else'):
                        f = CppFunc(sname, norm_type(m2.group(1)), m2.group(2), parse_params(m2.group(3)),
                                    static=bool(re.match(r'^\s*(IMGUI_API\s+)?static\b', l)))
                        funcs.setdefault((sname, f.name), []).append(f)
                depth += opens - closes
                if depth <= 0 and (opens or closes):
                    break
                i += 1
            structs[sname] = members
        i += 1
    return funcs, structs, typedef_scalars, enums


# ---------------------------------------------------------------------------------------------------
# Kinds: how a C++ parameter / return is marshalled.

FLOAT_T = {'float', 'double'}
U32_T = {'ImU32', 'ImGuiID', 'unsigned int', 'ImWchar32', 'ImWchar'}
I64_T = {'ImS64', 'ImU64', 'ImGuiSelectionUserData', 'size_t', 'ImTextureID', 'ImFontAtlasRectId'}


class Ctx:
    classes = set()
    scalars = set()


def base_class(ctype):
    t = ctype.replace('const ', '').strip()
    t = t.rstrip('*&').strip()
    return t


def is_scalar(t):
    t = t.replace('const ', '').strip()
    return (t in ('int', 'float', 'double', 'short', 'unsigned short', 'char', 'signed char', 'unsigned char')
            or t in Ctx.scalars or t in U32_T or t in I64_T)


def param_kind(p):
    t, n = p.ctype, p.name
    if t == '...':
        return 'varargs'
    if t in ('FNPTR', 'UNKNOWN') or t.endswith('Callback'):
        return None
    if p.dim is not None:
        bt = t.replace('const ', '')
        if bt in ('float', 'int', 'double') and p.dim.isdigit():
            return 'arr'
        return None
    if t == 'const char*':
        if n.endswith('_end'):
            return 'end'
        if n == 'fmt':
            return 'fmt'
        return 'str'
    if t == 'const char**':
        return 'null'
    if t == 'bool':
        return 'bool'
    if t == 'bool*':
        return 'pbool'
    if t in ('int*', 'float*', 'double*', 'unsigned int*', 'size_t*'):
        return 'pnum'
    if t in ('const ImVec2&', 'ImVec2'):
        return 'vec2'
    if t in ('const ImVec4&', 'ImVec4'):
        return 'vec4'
    if t == 'const ImVec2*':
        return 'pvec2'
    if t == 'const ImVec4*':
        return 'pvec4'
    if t in ('ImTextureRef', 'const ImTextureRef&'):
        return 'tex'
    if t in U32_T:
        return 'u32'
    if t in I64_T:
        return 'i64'
    if is_scalar(t):
        return 'num'
    if t in ('void*', 'const void*', 'const ImWchar*', 'ImWchar*'):
        return 'rawptr'
    if (t.endswith('*') or t.endswith('&')) and base_class(t) in Ctx.classes and t.count('*') <= 1:
        return 'obj'
    return None


def ret_kind(t):
    if t in ('void', 'IMGUI_API void'):
        return 'void'
    if t == 'bool':
        return 'bool'
    if t in U32_T:
        return 'u32'
    if t in I64_T:
        return 'i64'
    if is_scalar(t):
        return 'num'
    if t == 'const char*':
        return 'str'
    if t in ('ImVec2', 'const ImVec2&'):
        return 'vec2'
    if t in ('ImVec4', 'const ImVec4&'):
        return 'vec4'
    if t in ('void*', 'const ImWchar*', 'const void*'):
        return 'rawptr'
    if t in ('int*', 'bool*', 'float*'):
        return 'pderef'
    if (t.endswith('*') or t.endswith('&')) and base_class(t) in Ctx.classes and t.count('*') <= 1:
        return 'obj'
    if t in Ctx.classes:
        return 'objval'
    return None


def ann_sig(t, aliases):
    t = ANN_TYPE_FIX.get(t, t)
    t = t.split('|')[0]
    if t in ('string',):
        return 's'
    if t == 'number' or aliases.get(t) == 'number':
        return 'n'
    if t == 'boolean':
        return 'b'
    if t in ('table', 'ImVec2', 'ImVec4'):
        return 't'
    if t == 'function':
        return 'f'
    if t == 'userdata' or t in Ctx.classes:
        return 'u'
    return '?'


COMPAT = {
    'str': 's?', 'fmt': 's?', 'bool': 'b?', 'num': 'n?', 'u32': 'n?', 'i64': 'n?', 'vec2': 't?',
    'vec4': 't?', 'pvec2': 't?', 'pvec4': 't?', 'pbool': 't?', 'pnum': 't?', 'arr': 't?',
    'tex': 'nu?', 'obj': 'u?', 'rawptr': 'un?', 'end': 'snu?', 'null': 'snut?',
}

# ---------------------------------------------------------------------------------------------------
# Alignment of an annotated overload with a C++ overload.


def align(ann, cf, aliases):
    """Return [(Param, kind, ann_index or None)] or (None, reason)."""
    out, ai, A = [], 0, ann.params
    for p in cf.params:
        k = param_kind(p)
        if k == 'varargs':
            continue
        if k is None:
            if p.default is not None and (ai >= len(A) or A[ai][0] != p.name):
                out.append((p, 'default', None))
                continue
            return None, 'C++ parameter %s %s not expressible' % (p.ctype, p.name)
        if k in ('end', 'null') and (ai >= len(A) or A[ai][0] != p.name):
            out.append((p, k, None))
            continue
        if ai < len(A):
            sig = ann_sig(A[ai][1], aliases)
            if sig not in COMPAT[k] and '?' not in sig:
                return None, 'Lua %s (%s) vs C++ %s %s' % (A[ai][0], A[ai][1], p.ctype, p.name)
            out.append((p, k, ai))
            ai += 1
        else:
            if p.default is None:
                return None, 'C++ parameter %s has no Lua counterpart' % p.name
            out.append((p, 'default', None))
    if ai < len(A):
        return None, 'Lua parameter %s has no C++ counterpart' % A[ai][0]
    return out, None


# ---------------------------------------------------------------------------------------------------
# Code generation.


def cxx_default(p, k):
    d = p.default
    if k in ('str', 'fmt'):
        return d if d is not None else '""'
    if k == 'bool':
        return d if d is not None else 'false'
    if k in ('num', 'u32', 'i64'):
        return d if d is not None else '0'
    if k == 'vec2':
        return d if d is not None else 'ImVec2(0.0f, 0.0f)'
    if k == 'vec4':
        return d if d is not None else 'ImVec4(0.0f, 0.0f, 0.0f, 0.0f)'
    return d


def gen_overload(fid, ann, cf, mapping, base_expr, self_expr):
    """Emit static int fid(lua_State* L, int b) for one overload."""
    L = []
    pre, post, args = [], [], []
    last_str = None
    for j, (p, k, ai) in enumerate(mapping):
        v = 'a%d' % j
        idx = '(b + %d)' % ai if ai is not None else None
        t = p.ctype
        if k == 'default':
            args.append(p.default)
            continue
        if k in ('str', 'fmt'):
            pre.append('size_t %s_n = 0; const char* %s = opt_str(L, %s, %s, &%s_n);' % (v, v, idx, cxx_default(p, k), v))
            last_str = v
            args.append('"%s", ' + v if k == 'fmt' else v)
            continue
        if k == 'end':
            args.append('(%s ? %s + %s_n : nullptr)' % (last_str, last_str, last_str) if last_str else 'nullptr')
            continue
        if k == 'null':
            args.append('nullptr')
            continue
        if k == 'bool':
            pre.append('bool %s = opt_bool(L, %s, %s);' % (v, idx, cxx_default(p, k)))
        elif k == 'num':
            bt = t.replace('const ', '')
            conv = 'opt_num' if bt in FLOAT_T else 'opt_int'
            pre.append('%s %s = (%s)%s(L, %s, (double)(%s));' % (bt, v, bt, conv, idx, cxx_default(p, k)))
        elif k == 'u32':
            bt = t.replace('const ', '')
            pre.append('%s %s = (%s)opt_u32(L, %s, (uint32_t)(%s));' % (bt, v, bt, idx, cxx_default(p, k)))
        elif k == 'i64':
            bt = t.replace('const ', '')
            pre.append('%s %s = (%s)opt_i64(L, %s, (int64_t)(%s));' % (bt, v, bt, idx, cxx_default(p, k)))
        elif k == 'vec2':
            pre.append('ImVec2 %s = opt_vec2(L, %s, %s);' % (v, idx, cxx_default(p, k)))
        elif k == 'vec4':
            pre.append('ImVec4 %s = opt_vec4(L, %s, %s);' % (v, idx, cxx_default(p, k)))
        elif k == 'pvec2':
            pre.append('ImVec2 %s_v; const ImVec2* %s = opt_pvec2(L, %s, &%s_v);' % (v, v, idx, v))
        elif k == 'pvec4':
            pre.append('ImVec4 %s_v; const ImVec4* %s = opt_pvec4(L, %s, &%s_v);' % (v, v, idx, v))
        elif k == 'tex':
            pre.append('ImTextureRef %s = opt_tex(L, %s);' % (v, idx))
        elif k == 'obj':
            bc = base_class(t)
            pre.append('%s* %s = (%s*)opt_obj(L, %s, CLS_%s);' % (bc, v, bc, idx, bc))
            if p.default is None and p.name != 'font':
                # ImGui dereferences required object parameters: a nil/foreign one skips the call.
                pre.append('if (!%s) { %s }' % (v, guard_default(ret_kind(cf.ret), cf.ret, len(ann.returns) >= 2)))
            args.append(('*' + v) if t.endswith('&') else v)
            continue
        elif k == 'rawptr':
            pre.append('%s %s = (%s)opt_rawptr(L, %s);' % (t, v, t, idx))
        elif k == 'pbool':
            nullable = 'true' if (p.default is not None) else 'false'
            pre.append('bool %s_v = false; bool* %s = get_pbool(L, %s, &%s_v, %s); const bool %s_o = %s_v;' % (v, v, idx, v, nullable, v, v))
            post.append('if (%s && %s_v != %s_o) set_t1_bool(L, %s, %s_v);' % (v, v, v, idx, v))
        elif k == 'pnum':
            bt = t[:-1]
            nullable = 'true' if (p.default is not None) else 'false'
            pre.append('%s %s_v = 0; %s* %s = get_pnum<%s>(L, %s, &%s_v, %s); const %s %s_o = %s_v;' % (bt, v, bt, v, bt, idx, v, nullable, bt, v, v))
            post.append('if (%s && %s_v != %s_o) set_t1_num(L, %s, (double)%s_v);' % (v, v, v, idx, v))
        elif k == 'arr':
            bt = t.replace('const ', '')
            nd = int(p.dim)
            pre.append('%s %s[%d] = {}; read_arr(L, %s, %s, %d); %s %s_o[%d]; memcpy(%s_o, %s, sizeof(%s));' % (
                bt, v, nd, idx, v, nd, bt, v, nd, v, v, v))
            if not t.startswith('const'):
                post.append('if (memcmp(%s, %s_o, sizeof(%s)) != 0) write_arr(L, %s, %s, %d);' % (v, v, v, idx, v, nd))
        args.append(v)
    rk = ret_kind(cf.ret)
    call = '%s%s(%s)' % (self_expr, cf.name, ', '.join(args))
    multi = len(ann.returns) >= 2
    L.append('static int %s(lua_State* L, int b)' % fid)
    L.append('{')
    L.append('    (void)L; (void)b;')
    if cf.owner and not cf.static:
        L.append('    %s* self = (%s*)self_ptr(L, CLS_%s); if (!self) return 0;' % (cf.owner, cf.owner, cf.owner))
    for s in pre:
        L.append('    ' + s)
    if rk == 'void':
        L.append('    %s;' % call)
        push, nret = [], 0
    else:
        rt = cf.ret
        if rk in ('obj',):
            L.append('    auto&& r = %s;' % call)
        else:
            L.append('    auto r = %s;' % call)
        push, nret = push_ret(rk, cf.ret, multi)
    for s in post:
        L.append('    ' + s)
    for s in push:
        L.append('    ' + s)
    L.append('    return %d;' % nret)
    L.append('}')
    return L


def push_ret(rk, t, multi, var='r'):
    if rk == 'bool':
        return ['lua_pushboolean(L, %s ? 1 : 0);' % var], 1
    if rk in ('num',):
        return ['lua_pushnumber(L, (lua_Number)%s);' % var], 1
    if rk == 'u32':
        return ['lua_pushnumber(L, (lua_Number)(uint32_t)%s);' % var], 1
    if rk == 'i64':
        return ['lua_pushnumber(L, (lua_Number)%s);' % var], 1
    if rk == 'str':
        return ['if (%s) lua_pushstring(L, %s); else lua_pushnil(L);' % (var, var)], 1
    if rk == 'vec2':
        if multi:
            return ['lua_pushnumber(L, %s.x); lua_pushnumber(L, %s.y);' % (var, var)], 2
        return ['push_val(L, %s, CLS_ImVec2);' % var], 1
    if rk == 'vec4':
        if multi:
            return ['lua_pushnumber(L, %s.x); lua_pushnumber(L, %s.y); lua_pushnumber(L, %s.z); lua_pushnumber(L, %s.w);' % ((var,) * 4)], 4
        return ['push_val(L, %s, CLS_ImVec4);' % var], 1
    if rk == 'rawptr':
        return ['if (%s) lua_pushlightuserdata(L, (void*)%s); else lua_pushnil(L);' % (var, var)], 1
    if rk == 'pderef':
        return ['if (%s) push_ref(L, *%s); else lua_pushnil(L);' % (var, var)], 1
    if rk == 'obj':
        bc = base_class(t)
        if t.endswith('&'):
            return ['push_obj(L, (void*)&%s, CLS_%s);' % (var, bc)], 1
        return ['if (%s) push_obj(L, (void*)%s, CLS_%s); else lua_pushnil(L);' % (var, var, bc)], 1
    if rk == 'objval':
        return ['push_val(L, %s, CLS_%s);' % (var, t)], 1
    raise ValueError(rk)


def guard_default(rk, t, multi):
    if rk in (None, 'void'):
        return 'return 0;'
    if rk == 'bool':
        return 'lua_pushboolean(L, 0); return 1;'
    if rk in ('num', 'u32', 'i64'):
        return 'lua_pushnumber(L, 0); return 1;'
    if rk == 'vec2':
        return 'lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2;' if multi else 'push_val(L, ImVec2(0.0f, 0.0f), CLS_ImVec2); return 1;'
    if rk == 'vec4':
        return 'for (int i = 0; i < 4; i++) lua_pushnumber(L, 0); return 4;' if multi else 'push_val(L, ImVec4(0.0f, 0.0f, 0.0f, 0.0f), CLS_ImVec4); return 1;'
    if rk == 'obj' and base_class(t) == 'ImDrawList':
        return 'push_obj(L, nullptr, CLS_ImDrawList); return 1;'
    return 'lua_pushnil(L); return 1;'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ashita', default=DEFAULT_ASHITA)
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()
    sdk = os.path.join(args.ashita, 'addons', 'libs', 'annotations', 'SDK')
    mgr_funcs, _, aliases1 = parse_annotations(os.path.join(sdk, 'IGuiManager.lua'))
    type_funcs, ann_classes, aliases2 = parse_annotations(os.path.join(sdk, 'IGuiManagerTypes.lua'))
    aliases = dict(aliases1)
    aliases.update(aliases2)
    cfuncs, structs, typedef_scalars, enums = parse_imgui_h(IMGUI_H)

    Ctx.scalars = set(typedef_scalars) | set(enums) | {'ImDrawIdx', 'ImTextureFormat', 'ImTextureStatus',
                                                       'ImGuiSelectionRequestType', 'ImDrawTextFlags'}
    # Classes: annotated + extras that exist.
    class_names = []
    for c in list(ann_classes.keys()) + EXTRA_CLASSES:
        if c in class_names:
            continue
        if c in structs or c in INTERNAL_CLASSES or c in ('ImVec2', 'ImVec4'):
            class_names.append(c)
    Ctx.classes = set(class_names)
    # Also make all imgui.h structs reachable through fields known classes (ImGuiKeyData, etc.).
    for s in structs:
        if s not in Ctx.classes and not s.startswith('ImVector') and s not in ('ImNewWrapper',):
            class_names.append(s)
            Ctx.classes.add(s)

    report = {'bound': [], 'hand': [], 'unbound': [], 'lib': [], 'partial': []}
    body = []
    fn_table = []     # (lua name, c function)

    # ---- IGuiManager functions.
    groups = {}
    order = []
    for f in mgr_funcs:
        if f.owner != 'IGuiManager':
            continue
        if f.name not in groups:
            order.append(f.name)
        groups.setdefault(f.name, []).append(f)

    def emit_group(owner, name, anns, cands, lua_owner):
        """Returns (cfunc name or None, reason)."""
        fid_base = ('f_%s' % name) if owner is None else ('m_%s_%s' % (owner, name))
        ovls = []
        reasons = []
        for i, a in enumerate(anns):
            ok = None
            for cf in cands:
                if ret_kind(cf.ret) is None:
                    reasons.append('C++ return type %s not expressible' % cf.ret)
                    continue
                mp, why = align(a, cf, aliases)
                if mp is not None:
                    ok = (cf, mp)
                    break
                reasons.append(why)
            if ok:
                ovls.append((a, ok[0], ok[1]))
        if not ovls:
            return None, '; '.join(sorted(set(reasons))) or 'not in imgui.h'
        base = 1 if owner is None else 2
        self_expr = 'ImGui::' if owner is None else 'self->'
        if owner is not None and ovls[0][1].static:
            self_expr = owner + '::'
        fids = []
        for i, (a, cf, mp) in enumerate(ovls):
            fid = '%s_%d' % (fid_base, i)
            body.extend(gen_overload(fid, a, cf, mp, base, self_expr))
            fids.append(fid)
        guarded = (owner is None and name not in NO_GUARD) or (owner in GUARDED_CLASSES)
        a0, cf0, _ = ovls[0]
        rk = ret_kind(cf0.ret)
        multi = len(a0.returns) >= 2
        body.append('static int %s(lua_State* L)' % fid_base)
        body.append('{')
        if guarded:
            body.append('    if (!xi_gui_in_frame() || !ImGui::GetCurrentContext()) { %s }' % guard_default(rk, cf0.ret, multi))
        elif owner is None:
            body.append('    if (!ImGui::GetCurrentContext()) { %s }' % guard_default(rk, cf0.ret, multi))
        if owner is None and name in PRECOND:
            body.append('    if (!(%s)) { %s }' % (PRECOND[name], guard_default(rk, cf0.ret, multi)))
        if len(ovls) == 1:
            body.append('    return %s(L, %d);' % (fids[0], base))
        else:
            sigs = []
            for a, cf, mp in ovls:
                sigs.append(''.join(ann_sig(p[1], aliases) for p in a.params))
            body.append('    static const char* const sigs[] = { %s };' % ', '.join('"%s"' % s for s in sigs))
            body.append('    switch (pick_overload(L, %d, sigs, %d)) {' % (base, len(sigs)))
            for i, fid in enumerate(fids):
                body.append('    case %d: return %s(L, %d);' % (i, fid, base))
            body.append('    }')
            body.append('    return 0;')
        body.append('}')
        body.append('')
        if len(ovls) < len(anns):
            report['partial'].append('%s%s (%d of %d overloads: %s)' % (lua_owner, name, len(ovls), len(anns), '; '.join(sorted(set(reasons)))))
        return fid_base, None

    for name in order:
        anns = groups[name]
        if name in LIB_HELPERS:
            report['lib'].append(name)
            continue
        if name in HAND:
            fn_table.append((name, 'hand_' + name))
            report['hand'].append(name)
            continue
        if all(a.deprecated or a.not_impl for a in anns):
            report['unbound'].append((name, 'deprecated / "Not implemented" in the annotations (va_list variant)'))
            continue
        cands = cfuncs.get((None, name), [])
        if not cands:
            report['unbound'].append((name, 'not in imgui.h'))
            continue
        fid, why = emit_group(None, name, anns, cands, '')
        if fid:
            fn_table.append((name, fid))
            report['bound'].append(name)
        else:
            report['unbound'].append((name, why))

    # ---- Class methods.
    meth_groups, morder = {}, []
    for f in type_funcs:
        key = (f.owner, f.name)
        if key not in meth_groups:
            morder.append(key)
        meth_groups.setdefault(key, []).append(f)
    methods = {}   # class -> [(name, cfunc)]
    mreport = {'bound': [], 'hand': [], 'unbound': []}
    for key in morder:
        owner, name = key
        if owner not in Ctx.classes:
            mreport['unbound'].append(('%s:%s' % key, 'class not in imgui.h'))
            continue
        if key in HAND_METHODS:
            methods.setdefault(owner, []).append((name, 'hand_%s_%s' % key))
            mreport['hand'].append('%s:%s' % key)
            continue
        cands = cfuncs.get(key, [])
        if not cands:
            mreport['unbound'].append(('%s:%s' % key, 'not in imgui.h'))
            continue
        fid, why = emit_group(owner, name, meth_groups[key], cands, owner + ':')
        if fid:
            methods.setdefault(owner, []).append((name, fid))
            mreport['bound'].append('%s:%s' % key)
        else:
            mreport['unbound'].append(('%s:%s' % key, why))

    # ---- Fields.
    fields = {}
    freport = {'missing': []}
    for c in class_names:
        members = structs.get(c, [])
        if c == 'ImVec2':
            members = [('x', False, 'float'), ('y', False, 'float')]
        elif c == 'ImVec4':
            members = [('x', False, 'float'), ('y', False, 'float'), ('z', False, 'float'), ('w', False, 'float')]
        names = {m[0] for m in members}
        for fname, _ in ann_classes.get(c, []):
            if fname not in names:
                freport['missing'].append('%s.%s' % (c, fname))
        out = []
        for fname, bitfield, ctype in members:
            gid = 'fg_%s_%s' % (c, fname)
            sid = 'fs_%s_%s' % (c, fname)
            if bitfield:
                body.append('static void %s(lua_State* L, void* p) { lua_pushnumber(L, (lua_Number)((%s*)p)->%s); }' % (gid, c, fname))
                body.append('static void %s(lua_State* L, void* p, int i) { if (lua_isnumber(L, i)) ((%s*)p)->%s = (unsigned)(int64_t)lua_tonumber(L, i); }' % (sid, c, fname))
            else:
                body.append('static void %s(lua_State* L, void* p) { push_ref(L, ((%s*)p)->%s); }' % (gid, c, fname))
                body.append('static void %s(lua_State* L, void* p, int i) { set_ref(L, ((%s*)p)->%s, i); }' % (sid, c, fname))
            out.append((fname, gid, sid))
        fields[c] = out

    # ---- Tables.
    for c in class_names:
        body.append('static const FieldInfo fields_%s[] = {' % c)
        for fname, gid, sid in fields[c]:
            body.append('    { "%s", %s, %s },' % (fname, gid, sid))
        body.append('    { nullptr, nullptr, nullptr } };')
        body.append('static const luaL_Reg methods_%s[] = {' % c)
        for mname, fid in methods.get(c, []):
            body.append('    { "%s", %s },' % (mname, fid))
        body.append('    { nullptr, nullptr } };')
    body.append('static const ClassInfo g_classes[CLS_COUNT] = {')
    for c in class_names:
        size = 'sizeof(%s)' % c
        body.append('    { "%s", %s, fields_%s, methods_%s },' % (c, size, c, c))
    body.append('};')
    body.append('static const luaL_Reg g_manager_funcs[] = {')
    for name, fid in fn_table:
        body.append('    { "%s", %s },' % (name, fid))
    body.append('    { nullptr, nullptr } };')

    # ---- Traits part.
    traits = ['enum ClassId {']
    for c in class_names:
        traits.append('    CLS_%s,' % c)
    traits.append('    CLS_COUNT')
    traits.append('};')
    for c in class_names:
        traits.append('template<> struct ClassOf<%s> { static constexpr int id = CLS_%s; };' % (c, c))

    total_ann = len([n for n in order if n not in LIB_HELPERS])
    hdr = ['// Generated by tools/gen_imgui_lua.py from Ashita\'s IGuiManager annotations and third_party/imgui/imgui.h.',
           '// Do not edit. Coverage: %d IGuiManager functions annotated (+%d defined by libs/imgui.lua itself);' % (total_ann, len(report['lib'])),
           '//   %d generated, %d hand-written in gui_lua.cpp, %d not bound.' % (len(report['bound']), len(report['hand']), len(report['unbound'])),
           '//   Methods: %d generated, %d hand-written, %d not bound.' % (len(mreport['bound']), len(mreport['hand']), len(mreport['unbound']))]
    for n, why in report['unbound']:
        hdr.append('//   not bound: %s -- %s' % (n, why))
    for s in report['partial']:
        hdr.append('//   partial: %s' % s)
    for n, why in mreport['unbound']:
        hdr.append('//   method not bound: %s -- %s' % (n, why))
    for s in freport['missing']:
        hdr.append('//   annotated field not in imgui.h: %s' % s)
    text = '\n'.join(hdr) + '\n\n#if defined(XI_GUI_GEN_TRAITS)\n\n' + '\n'.join(traits) + \
        '\n\n#elif defined(XI_GUI_GEN_BODY)\n\n' + '\n'.join(body) + '\n\n#endif\n'
    with open(args.out, 'w') as f:
        f.write(text)
    if args.report:
        print('\n'.join(hdr))
    else:
        print('%s: %d functions generated, %d hand, %d unbound; %d methods generated, %d hand, %d unbound' % (
            os.path.relpath(args.out, ROOT), len(report['bound']), len(report['hand']), len(report['unbound']),
            len(mreport['bound']), len(mreport['hand']), len(mreport['unbound'])))


if __name__ == '__main__':
    main()
