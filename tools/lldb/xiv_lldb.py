"""lldb helpers for the recompiled ffxivgame.exe (build/xivboot, build/xivboot-dbg).

    command script import tools/lldb/xiv_lldb.py      (tools/xivdbg.sh does this)

Guest memory is mapped at host 0x20000000000 + VA. Every translated function is f_XXXXXXXX (its
original VA) taking Guest* g; in the -g debug build each generated source line sits under a
/* XXXXXXXX: x86 */ comment, which is how guest instruction addresses are recovered.

  gx <va> [count] [size]     hex dump guest memory (size 1/2/4/8, default 4 x 16)
  gs <va>                    guest C string
  gaddr <va>                 host address for a guest VA
  gb <va> [lldb bp args]     breakpoint at a guest address: function entry or any instruction
  gsym <va|host-pc>          guest function containing an address + decomp name/size
  gwhere                     guest function + instruction for the selected frame
  gbt [n]                    guest backtrace (translated frames only)
  gregs                      guest registers for the selected frame
  gstack [n]                 n dwords at guest esp
"""

import bisect
import json
import os
import re
import shlex
import subprocess

import lldb

GUEST_BASE = 0x20000000000
IMAGE_BASE = 0x00400000
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, 'generated', 'ffxivgame')
DECOMP_SYMBOLS = os.path.join(ROOT, '..', 'decomp', 'config', 'ffxivgame.symbols.json')
# Names given by docs/ffxivDecomp/legacy sidecars for functions Ghidra still calls FUN_ (build_name_overrides.py).
NAME_OVERRIDES = os.path.join(ROOT, '..', 'decomp', 'config', 'ffxivgame.name_overrides.json')
COMMENT = re.compile(r'/\* ([0-9a-f]{8}): (.*?) \*/')
REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')

_fn_addrs = None      # sorted guest VAs that have an f_ symbol
_decomp = None        # sorted [(va, name, size)] from meteor-decomp's Ghidra export
_lines = {}           # generated file -> list of lines


def _num(s, frame=None):
    s = s.strip()
    try:
        return int(s, 0)
    except ValueError:
        pass
    if frame is not None:  # allow expressions: register names, locals, $x0 ...
        v = frame.EvaluateExpression(s)
        if v.IsValid() and v.GetError().Success():
            return v.GetValueAsUnsigned()
    try:
        return int(s, 16)
    except ValueError:
        raise ValueError('not a number: %s' % s)


def _ctx(debugger):
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    frame = process.GetSelectedThread().GetSelectedFrame() if process.IsValid() else None
    return target, process, frame


def _guest_fns(target):
    global _fn_addrs
    if _fn_addrs is None:
        addrs = set()
        for m in target.module_iter():
            for sym in m:
                n = sym.GetName() or ''
                if len(n) == 10 and n.startswith('f_'):
                    try:
                        addrs.add(int(n[2:], 16))
                    except ValueError:
                        pass
            if addrs:
                break
        _fn_addrs = sorted(addrs)
    return _fn_addrs


def _decomp_syms():
    global _decomp
    if _decomp is None:
        _decomp = []
        names = {}
        try:
            with open(NAME_OVERRIDES) as f:
                names = {e['rva']: e['name'] for e in json.load(f)}
        except (OSError, ValueError, KeyError):
            pass
        try:
            with open(DECOMP_SYMBOLS) as f:
                for e in json.load(f):
                    _decomp.append((IMAGE_BASE + e['rva'], names.get(e['rva'], e['name']), e.get('size', 0)))
            _decomp.sort()
        except (OSError, ValueError, KeyError):
            pass
    return _decomp


def _decomp_name(va):
    syms = _decomp_syms()
    i = bisect.bisect_right(syms, (va, '￿', 0)) - 1
    if i >= 0:
        start, name, size = syms[i]
        if va < start + max(size, 1):
            return name, start, size
    return None


def _containing_fn(target, va):
    addrs = _guest_fns(target)
    i = bisect.bisect_right(addrs, va) - 1
    return addrs[i] if i >= 0 else None


def _file_lines(path):
    if path not in _lines:
        try:
            with open(path, errors='replace') as f:
                _lines[path] = f.read().split('\n')
        except OSError:
            _lines[path] = []
    return _lines[path]


def _guest_insn(frame):
    """(va, asm) of the guest instruction the frame is executing, from the source line comments."""
    le = frame.GetLineEntry()
    if not le.IsValid():
        return None
    fs = le.GetFileSpec()
    path = os.path.join(fs.GetDirectory() or '', fs.GetFilename() or '')
    if not os.path.exists(path):
        path = os.path.join(GEN, fs.GetFilename() or '')
    lines = _file_lines(path)
    for i in range(min(le.GetLine(), len(lines)) - 1, max(-1, le.GetLine() - 400), -1):
        m = COMMENT.search(lines[i])
        if m:
            return int(m.group(1), 16), m.group(2)
        if lines[i].startswith('void f_'):
            break
    return None


def _frame_fn(frame):
    name = frame.GetFunctionName() or frame.GetSymbol().GetName() or ''
    m = re.match(r'f_([0-9a-f]{8})', name)
    return (int(m.group(1), 16), name) if m else (None, name)


def _guest_regs(frame):
    """Register values: the function's locals in the debug build, else the Guest struct via g/x0."""
    vals = {}
    for r in REGS:
        v = frame.FindVariable(r)
        if v.IsValid() and v.GetError().Success() and v.GetValue() is not None:
            vals[r] = v.GetValueAsUnsigned()
    if len(vals) == len(REGS):
        return vals, 'locals'
    g = frame.FindVariable('g')
    ptr = g.GetValueAsUnsigned() if g.IsValid() and g.GetValue() is not None else None
    src = 'Guest* g'
    if not ptr:
        ptr = frame.FindRegister('x0').GetValueAsUnsigned()
        src = 'Guest* x0 (only valid at function entry)'
    err = lldb.SBError()
    data = frame.GetThread().GetProcess().ReadMemory(ptr, 32, err)
    if err.Fail():
        return vals, 'unavailable'
    for i, r in enumerate(REGS):
        vals.setdefault(r, int.from_bytes(data[i * 4:i * 4 + 4], 'little'))
    return vals, src


def _read32(process, va):
    err = lldb.SBError()
    v = process.ReadUnsignedFromMemory(GUEST_BASE + va, 4, err)
    return None if err.Fail() else v


# ------------------------------------------------------------------ commands

def gx(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    a = shlex.split(command)
    if not a:
        result.SetError('usage: gx <va> [count] [size]')
        return
    va = _num(a[0], frame)
    count = int(a[1], 0) if len(a) > 1 else 16
    size = int(a[2], 0) if len(a) > 2 else 4
    debugger.GetCommandInterpreter().HandleCommand(
        'memory read --size %d --format x --count %d 0x%x' % (size, count, GUEST_BASE + va), result)


def gs(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    va = _num(command, frame)
    err = lldb.SBError()
    s = process.ReadCStringFromMemory(GUEST_BASE + va, 4096, err)
    result.AppendMessage('0x%08x: %r' % (va, s) if err.Success() else 'read failed: %s' % err)


def gaddr(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    va = _num(command, frame)
    result.AppendMessage('guest 0x%08x = host 0x%x' % (va, GUEST_BASE + va))


def gb(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    a = shlex.split(command)
    if not a:
        result.SetError('usage: gb <va> [extra breakpoint set args]')
        return
    va = _num(a[0])
    extra = ' '.join(shlex.quote(x) for x in a[1:])
    ci = debugger.GetCommandInterpreter()
    name = 'f_%08x' % va
    if target.FindSymbols(name).GetSize() or target.FindSymbols(name + '_body').GetSize():
        ci.HandleCommand('breakpoint set -n %s -n %s_body %s' % (name, name, extra), result)
        return
    # mid-function: find the instruction's comment line in the generated source (debug build)
    try:
        out = subprocess.run(['grep', '-n', '-F', '/* %08x:' % va, '-r', GEN, '--include=*.c'],
                             capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        out = ''
    hit = out.splitlines()[0] if out else None
    if not hit:
        fn = _containing_fn(target, va)
        result.SetError('no f_%08x and no instruction comment for it%s' %
                        (va, '; containing function f_%08x' % fn if fn is not None else ''))
        return
    path, line = hit.split(':')[:2]
    ci.HandleCommand('breakpoint set -f %s -l %d %s' % (os.path.basename(path), int(line) + 1, extra), result)
    if not result.Succeeded() or 'no locations' in (result.GetOutput() or ''):
        result.AppendMessage('note: line breakpoints need the -g build (tools/build_xiv.py xivboot --debug)')


def gsym(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    v = _num(command, frame)
    if v >= GUEST_BASE and v < GUEST_BASE + 0x100000000:
        v -= GUEST_BASE
    if v > 0xFFFFFFFF:  # host pc
        addr = target.ResolveLoadAddress(v)
        sym = addr.GetSymbol().GetName()
        result.AppendMessage('host 0x%x = %s+0x%x' % (v, sym, v - addr.GetSymbol().GetStartAddress().GetLoadAddress(target)))
        m = re.match(r'f_([0-9a-f]{8})', sym or '')
        if not m:
            return
        v = int(m.group(1), 16)
    fn = _containing_fn(target, v)
    d = _decomp_name(v)
    msg = 'guest 0x%08x' % v
    if fn is not None:
        msg += ' in f_%08x+0x%x' % (fn, v - fn)
    if d:
        msg += '  [decomp %s @0x%08x size 0x%x]' % (d[0], d[1], d[2])
    result.AppendMessage(msg)


def _describe(frame):
    va, name = _frame_fn(frame)
    if va is None:
        return '%s (host)' % name
    s = name
    insn = _guest_insn(frame)
    if insn:
        s += '  @%08x: %s' % insn
    d = _decomp_name(va)
    if d and d[0] != 'FUN_%08x' % va:
        s += '  [%s]' % d[0]
    return s


def gwhere(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    if frame is None:
        result.SetError('no process')
        return
    result.AppendMessage(_describe(frame))


def gbt(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    if frame is None:
        result.SetError('no process')
        return
    n = int(command, 0) if command.strip() else 32
    shown = 0
    for f in process.GetSelectedThread():
        va, _ = _frame_fn(f)
        if va is None:
            continue
        result.AppendMessage('#%-3d %s' % (f.GetFrameID(), _describe(f)))
        shown += 1
        if shown >= n:
            break
    if not shown:
        result.AppendMessage('no translated (f_) frames on this thread')


def gregs(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    if frame is None:
        result.SetError('no process')
        return
    vals, src = _guest_regs(frame)
    result.AppendMessage('(%s)' % src)
    result.AppendMessage('  '.join('%s=%08x' % (r, vals[r]) for r in REGS if r in vals))


def gstack(debugger, command, result, internal_dict):
    target, process, frame = _ctx(debugger)
    if frame is None:
        result.SetError('no process')
        return
    n = int(command, 0) if command.strip() else 16
    vals, src = _guest_regs(frame)
    esp = vals.get('esp')
    if esp is None:
        result.SetError('esp unavailable')
        return
    for i in range(n):
        v = _read32(process, esp + 4 * i)
        note = ''
        if v is not None and IMAGE_BASE <= v < 0x01400000:
            fn = _containing_fn(target, v)
            if fn is not None:
                note = '  f_%08x+0x%x' % (fn, v - fn)
        result.AppendMessage('esp+%02x %08x: %s%s' % (4 * i, esp + 4 * i, '????????' if v is None else '%08x' % v, note))


def __lldb_init_module(debugger, internal_dict):
    for name in ('gx', 'gs', 'gaddr', 'gb', 'gsym', 'gwhere', 'gbt', 'gregs', 'gstack'):
        debugger.HandleCommand('command script add -o -f %s.%s %s' % (__name__, name, name))
    print('xiv_lldb: gx gs gaddr gb gsym gwhere gbt gregs gstack  (guest base 0x%x)' % GUEST_BASE)
