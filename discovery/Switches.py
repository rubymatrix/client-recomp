# bound the `jmp [reg*4 + table]` switches Ghidra left unresolved, from the MSVC 6 guard
#   cmp reg, N ; ja default ; [mov/movzx reg, byte ptr [reg + index_table]] ; jmp [reg*4 + table]
# Adds computed-jump references to every case, disassembles them and extends the function body.
# Prints one line per switch; this is the switch section of the per-build metadata.
# Jython headless post-script; modifies the program.
# @category FFXIRecomp
import re
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.symbol import RefType, SourceType

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')
space = prog.getAddressFactory().getDefaultAddressSpace()
rm = prog.getReferenceManager()

SCALED = re.compile(r'JMP dword ptr \[(E[A-Z]{2})\*0x4 \+ (0x[0-9a-f]+)\]')
CMP = re.compile(r'CMP (E[A-Z]{2}),(0x[0-9a-f]+|\d+)$')
IDX = re.compile(r'(?:MOV|MOVZX) (?:[A-Z]{2,3}),byte ptr \[(E[A-Z]{2}) \+ (0x[0-9a-f]+)\]')

# Switches with no guard in their own function (the caller bounds the index), and signed
# two-sided guards: case counts read by hand, per module and build (verdicts.py).
import os, sys
sys.path.insert(0, os.path.dirname(getSourceFile().getAbsolutePath()))
import verdicts as manual
OVERRIDES = manual.verdicts(prog)['SWITCHES']

lines = []
it = listing.getInstructions(text.getStart(), True)
todo = []
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    ft = ins.getFlowType()
    # Every scaled-index switch, including ones Ghidra "resolved": its own bounds miss cases
    # (e.g. 0x10002226 had 2 of 4), and a switch it resolved to one case may no longer carry a
    # jump flow type at all (0x102bad09 became a computed-call terminator), so match on the
    # mnemonic and operand form alone.
    if ins.getMnemonicString() == 'JMP':
        m = SCALED.match(ins.toString())
        if m:
            todo.append((ins, m.group(1), int(m.group(2), 16)))

for ins, reg, table in todo:
    bound = None
    idx_table = None
    p = ins
    for _ in range(12):
        p = p.getPrevious()
        if p is None:
            break
        s = p.toString()
        mi = IDX.match(s)
        if mi and idx_table is None:
            idx_table = int(mi.group(2), 16)
        mc = CMP.match(s)
        if mc:
            bound = int(mc.group(2), 0)
            break
    if ins.getAddress().getOffset() in OVERRIDES:
        n = OVERRIDES[ins.getAddress().getOffset()]
        idx_table = None
    elif bound is None:
        lines.append('UNBOUNDED %s %s' % (ins.getAddress(), ins))
        continue
    elif idx_table is not None:
        idx = [mem.getByte(space.getAddress(idx_table + i)) & 0xFF for i in range(bound + 1)]
        n = max(idx) + 1
    else:
        n = bound + 1
    targets = []
    ok = True
    for i in range(n):
        v = mem.getInt(space.getAddress(table + 4 * i)) & 0xFFFFFFFF
        a = space.getAddress(v)
        # Cases live near their jump; anything far away means the guard we found is not this
        # switch's guard.
        if not text.contains(a) or abs(a.subtract(ins.getAddress())) > 0x10000:
            ok = False
            break
        targets.append(a)
    if not ok:
        lines.append('BAD_TABLE %s %s entry %d' % (ins.getAddress(), ins, len(targets)))
        continue
    known = set(str(x) for x in ins.getFlows())
    missing = [a for a in set(targets) if str(a) not in known]
    if not missing:
        continue
    for a in set(targets):
        rm.addMemoryReference(ins.getAddress(), a, RefType.COMPUTED_JUMP, SourceType.ANALYSIS, 0)
        if listing.getInstructionAt(a) is None:
            DisassembleCommand(a, None, True).applyTo(prog, monitor)
    fn = fm.getFunctionContaining(ins.getAddress())
    if fn is not None:
        CreateFunctionCmd.fixupFunctionBody(prog, fn, monitor)
    lines.append('SWITCH %s table=0x%08x cases=%d index_table=%s targets=%d added=%d fn=%s' % (
        ins.getAddress(), table, n, ('0x%08x' % idx_table) if idx_table else '-', len(set(targets)),
        len(missing), fn.getName() if fn else '-'))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines))
