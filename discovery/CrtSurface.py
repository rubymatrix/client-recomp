# the CRT's API surface as the game uses it: functions inside the CRT region that are called
# directly from outside it, with call counts, Ghidra's name if it has one, and the imports each
# one reaches (its own calls, one level). Args: <out> <crt_start hex> <crt_end hex>
# Jython headless post-script, read-only.
# @category FFXIRecomp
import collections

args = getScriptArgs()
out_path = args[0]
lo, hi = int(args[1], 16), int(args[2], 16)
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
text = prog.getMemory().getBlock('.text')

def in_crt(a):
    return lo <= a < hi

def imports_of(f):
    names = set()
    for ins in listing.getInstructions(f.getBody(), True):
        if not ins.getFlowType().isCall():
            continue
        for r in ins.getReferencesFrom():
            to = r.getToAddress()
            sym = prog.getSymbolTable().getPrimarySymbol(to)
            if sym is not None and sym.isExternalEntryPoint() is False and to.isExternalAddress():
                names.add(sym.getName())
            elif sym is not None and ('IAT' in str(sym.getSymbolType()) or sym.getParentNamespace().isExternal()):
                names.add(sym.getName())
        for t in ins.getFlows():
            if t.isExternalAddress():
                s = prog.getSymbolTable().getPrimarySymbol(t)
                if s is not None:
                    names.add(s.getName())
    return names

calls = collections.Counter()
callers = collections.defaultdict(set)
for f in fm.getFunctions(True):
    e = f.getEntryPoint().getOffset()
    if not text.contains(f.getEntryPoint()) or in_crt(e) or f.getName().startswith('Unwind@'):
        continue
    for ins in listing.getInstructions(f.getBody(), True):
        if not ins.getFlowType().isCall():
            continue
        for t in ins.getFlows():
            if not text.contains(t):
                continue
            g = fm.getFunctionContaining(t)
            if g is not None and in_crt(g.getEntryPoint().getOffset()):
                calls[g.getEntryPoint()] += 1
                callers[g.getEntryPoint()].add(e)

lines = ['crt_region %08x-%08x  surface_functions %d  call_sites %d' % (lo, hi, len(calls), sum(calls.values()))]
named = 0
for a, n in calls.most_common():
    g = fm.getFunctionAt(a)
    nm = g.getName()
    if not nm.startswith('FUN_'):
        named += 1
    size = g.getBody().getNumAddresses()
    lines.append('  %s %-28s sites=%5d callers=%5d size=%5d imports=%s' % (
        a, nm, n, len(callers[a]), size, ','.join(sorted(imports_of(g)))[:120]))
lines.insert(1, 'named %d of %d' % (named, len(calls)))
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines[:3]))
