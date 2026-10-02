# find decode conflicts: an instruction that falls through to an address where no instruction
# starts. Each one means some other decode (usually a false function start) overlaps real code.
# With FIX=1 as the second script argument, clears the overlapping code units and functions and
# re-disassembles from the fall-through, so the real decode wins.
# Jython headless post-script.
# @category FFXIRecomp
from ghidra.app.cmd.disassemble import DisassembleCommand

args = getScriptArgs()
out_path = args[0]
fix = len(args) > 1 and args[1] == 'FIX=1'
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
text = prog.getMemory().getBlock('.text')

conflicts = []
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    ft = ins.getFallThrough()
    if ft is None or not text.contains(ft):
        continue
    if listing.getInstructionAt(ft) is None:
        conflicts.append((ins.getAddress(), ft))

lines = ['conflicts %d' % len(conflicts)]
for a, ft in conflicts[:40]:
    over = listing.getInstructionContaining(ft)
    nxt = listing.getInstructionAfter(ft)
    fn = fm.getFunctionAt(nxt.getAddress()) if nxt is not None else None
    lines.append('  %s falls into %s  overlapping=%s  next_insn=%s%s' % (
        a, ft, over.getAddress() if over else '-', nxt.getAddress() if nxt else '-',
        ' (function entry)' if fn else ''))

if fix:
    fixed = 0
    for a, ft in conflicts:
        if listing.getInstructionAt(ft) is not None:
            continue
        # Remove bogus decodes in the next 16 bytes (the false start is at most one alignment slot away).
        end = ft.add(15)
        for f in list(fm.getFunctionsOverlapping(prog.getAddressFactory().getAddressSet(ft, end))):
            if f.getEntryPoint().compareTo(ft) > 0:
                fm.removeFunction(f.getEntryPoint())
        over = listing.getInstructionContaining(ft)
        clear_from = over.getAddress() if over is not None else ft
        listing.clearCodeUnits(clear_from, end, False)
        DisassembleCommand(ft, None, True).applyTo(prog, monitor)
        fixed += 1
    lines.append('fixed %d' % fixed)

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines[:5]))
