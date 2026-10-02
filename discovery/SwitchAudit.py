# for every scaled-index switch, compare the cases Ghidra attached with how far the table
# plausibly runs (consecutive entries that point into .text within 64 KB of the jump). A run longer
# than the attached set means Ghidra may have under-bounded it; review those by hand.
# Jython headless post-script, read-only.
# @category FFXIRecomp
import re

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
text = mem.getBlock('.text')
space = prog.getAddressFactory().getDefaultAddressSpace()
SCALED = re.compile(r'JMP dword ptr \[(E[A-Z]{2})\*0x4 \+ (0x[0-9a-f]+)\]')

rm = prog.getReferenceManager()

switches = []
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    m = SCALED.match(ins.toString())
    if m:
        switches.append((ins, int(m.group(2), 16)))
# MSVC packs a function's tables back to back; a table ends where the next one (or any other
# referenced object, e.g. a byte index table) begins.
table_starts = set(t for (_, t) in switches)

lines = []
total = 0
for ins, table in switches:
    total += 1
    flows = set(str(x) for x in ins.getFlows())
    run = []
    i = 0
    while i < 256:
        ea = table + 4 * i
        if i > 0 and (ea in table_starts or rm.hasReferencesTo(space.getAddress(ea))):
            break
        v = mem.getInt(space.getAddress(ea)) & 0xFFFFFFFF
        a = space.getAddress(v)
        if not text.contains(a) or abs(a.subtract(ins.getAddress())) > 0x10000:
            break
        run.append(str(a))
        i += 1
    extra = [a for a in run if a not in flows]
    if extra:
        lines.append('REVIEW %s table=0x%08x attached=%d plausible_run=%d not_attached=%d first=%s' % (
            ins.getAddress(), table, len(flows), len(run), len(extra), extra[0]))
lines.insert(0, 'scaled_switches %d  review %d' % (total, len(lines)))
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines))
