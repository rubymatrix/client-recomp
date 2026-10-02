# locate the statically linked MSVC CRT. Reports (1) functions Ghidra named (Function ID,
# symbols), (2) where calls to CRT-only imports come from, (3) where CRT error strings are used.
# Jython headless post-script, read-only.
# @category FFXIRecomp
import collections

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
st = prog.getSymbolTable()
text = prog.getMemory().getBlock('.text')

lines = []
def w(s):
    lines.append(s)

named = []
for f in fm.getFunctions(True):
    if text.contains(f.getEntryPoint()) and not f.getName().startswith('FUN_') and not f.getName().startswith('thunk_FUN_'):
        named.append((f.getEntryPoint(), f.getName(), str(f.getSymbol().getSource())))
w('named_functions %d' % len(named))
src = collections.Counter(s for (_, _, s) in named)
w('  by source %s' % dict(src))
for a, n, s in named[:400]:
    w('  %s %s (%s)' % (a, n, s))

# Imports only the CRT startup / stdio / locale code uses.
CRT_ONLY = ['GetEnvironmentStrings', 'GetEnvironmentStringsW', 'FreeEnvironmentStringsA',
            'FreeEnvironmentStringsW', 'GetStartupInfoA', 'SetHandleCount', 'GetStdHandle', 'GetFileType',
            'HeapCreate', 'HeapDestroy', 'GetCommandLineA', 'GetVersion', 'GetCPInfo', 'GetACP', 'GetOEMCP',
            'LCMapStringA', 'LCMapStringW', 'GetStringTypeA', 'GetStringTypeW', 'RtlUnwind',
            'UnhandledExceptionFilter', 'SetUnhandledExceptionFilter', 'IsBadWritePtr', 'IsBadReadPtr',
            'IsBadCodePtr', 'FatalAppExitA', 'TlsAlloc', 'TlsGetValue', 'TlsSetValue', 'TlsFree',
            'SetStdHandle', 'FlushFileBuffers', 'SetEndOfFile', 'HeapReAlloc', 'HeapSize', 'EnumSystemLocalesA',
            'IsValidLocale', 'IsValidCodePage', 'GetLocaleInfoA', 'GetLocaleInfoW', 'GetTimeZoneInformation',
            'SetEnvironmentVariableA', 'CompareStringA', 'CompareStringW']
callers = []
for name in CRT_ONLY:
    for sym in st.getExternalSymbols(name):
        for ref in rm.getReferencesTo(sym.getAddress()):
            fa = ref.getFromAddress()
            # IAT slot -> find code references to the slot
            for r2 in rm.getReferencesTo(fa):
                f = fm.getFunctionContaining(r2.getFromAddress())
                if f is not None:
                    callers.append((f.getEntryPoint().getOffset(), name))
        # also direct code refs
    for sym in st.getSymbols(name):
        for ref in rm.getReferencesTo(sym.getAddress()):
            f = fm.getFunctionContaining(ref.getFromAddress())
            if f is not None and text.contains(f.getEntryPoint()):
                callers.append((f.getEntryPoint().getOffset(), name))
callers = sorted(set(callers))
w('crt_only_import_callers %d' % len(callers))
if callers:
    w('  lowest %08x (%s)  highest %08x (%s)' % (callers[0][0], callers[0][1], callers[-1][0], callers[-1][1]))
    hist = collections.Counter((a >> 16) for (a, _) in callers)
    w('  by 64KB page %s' % sorted(('%04x' % k, v) for k, v in hist.items()))
    for a, n in callers[:12]:
        w('  e.g. %08x %s' % (a, n))
    for a, n in callers[-6:]:
        w('  e.g. %08x %s' % (a, n))

# CRT runtime error strings.
mem = prog.getMemory()
for s in ['R6002', 'runtime error ', 'Microsoft Visual C++ Runtime Library', '_CrtDbgReport', 'pure virtual function call']:
    addr = mem.findBytes(prog.getMinAddress(), s, None, True, monitor)
    if addr is None:
        w('string %r: not found' % s)
        continue
    users = [fm.getFunctionContaining(r.getFromAddress()) for r in rm.getReferencesTo(addr)]
    users = [u.getEntryPoint() for u in users if u is not None]
    w('string %r at %s used by %s' % (s, addr, users[:5]))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
