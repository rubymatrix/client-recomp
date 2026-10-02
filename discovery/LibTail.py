# find where statically linked libraries begin. Library objects are linked after the game's
# own objects and never call back into them directly, so a library region is a suffix of .text
# that is closed under direct calls: nothing at or above the boundary calls below it.
# Prints every such boundary with the tail size, plus direct-call statistics per 64 KB.
# Jython headless post-script, read-only.
# @category FFXIRecomp
out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
text = prog.getMemory().getBlock('.text')

entries = []
min_callee = {}
callers_of = {}
for f in fm.getFunctions(True):
    e = f.getEntryPoint()
    if not text.contains(e):
        continue
    entries.append(e.getOffset())
    lo = None
    for ins in listing.getInstructions(f.getBody(), True):
        if not ins.getFlowType().isCall():
            continue
        for t in ins.getFlows():
            if not text.contains(t):
                continue
            g = fm.getFunctionContaining(t)
            if g is None:
                continue
            ga = g.getEntryPoint().getOffset()
            if lo is None or ga < lo:
                lo = ga
    min_callee[e.getOffset()] = lo

entries.sort()

# Down-edge profile: for candidate boundaries in the last 256 KB, how many direct call edges go
# from at/above the boundary to below it. Unwind funclets (compiler-generated, they call the
# game's destructors) are excluded. A library boundary shows as a sharp drop.
edges = []
for f in fm.getFunctions(True):
    e = f.getEntryPoint()
    if not text.contains(e) or f.getName().startswith('Unwind@'):
        continue
    for ins in listing.getInstructions(f.getBody(), True):
        if not ins.getFlowType().isCall():
            continue
        for t in ins.getFlows():
            g = fm.getFunctionContaining(t) if text.contains(t) else None
            if g is not None:
                edges.append((e.getOffset(), g.getEntryPoint().getOffset(), g.getName()))
tail_start = text.getEnd().getOffset() + 1 - 0x40000
profile = []
for b in [x for x in entries if x >= tail_start]:
    down = [(s, t, n) for (s, t, n) in edges if s >= b and t < b]
    profile.append((b, down))
prof_lines = []
prev = None
for b, down in profile:
    n = len(down)
    if prev is None or n != prev:
        prof_lines.append('  at %08x down_edges %d%s' % (b, n, ('  e.g. ' + ', '.join('%08x->%08x %s' % d for d in down[:3])) if n and n <= 6 else ''))
    prev = n

# suffix minimum of callee addresses
suffix_min = [None] * (len(entries) + 1)
running = None
for i in range(len(entries) - 1, -1, -1):
    v = min_callee[entries[i]]
    if v is not None and (running is None or v < running):
        running = v
    suffix_min[i] = running

end = text.getEnd().getOffset() + 1
lines = ['functions %d' % len(entries)]
closed = []
for i, e in enumerate(entries):
    m = suffix_min[i]
    if m is None or m >= e:
        closed.append((e, end - e, len(entries) - i))
lines.append('closed_suffix_boundaries %d' % len(closed))
# The interesting boundaries are the big ones.
for e, size, nfun in closed:
    if size >= 0x800:
        lines.append('  boundary %08x  tail %7d bytes  %5d functions' % (e, size, nfun))

lines.append('down_edge_profile (last 256 KB, changes only)')
lines += prof_lines
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines[:60]))
