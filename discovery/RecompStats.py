# Feasibility stats for static recompilation of FFXiMain. Jython, run as a headless post-script.
# @category FFXIRecomp
import collections

from ghidra.program.model.symbol import FlowType

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')

lines = []
def w(s):
    lines.append(s)

text_size = text.getSize()
w('text_size %d' % text_size)

# Coverage: bytes of .text that are instructions, inside functions, or undefined.
insn_bytes = 0
insn_count = 0
insn_outside_fn = 0
computed_jumps = 0
computed_jumps_resolved = 0
computed_calls = 0
computed_calls_mem = 0
x87_fns = set()
mnem = collections.Counter()
it = listing.getInstructions(text.getStart(), True)
while it.hasNext() and not monitor.isCancelled():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    insn_count += 1
    insn_bytes += ins.getLength()
    fn = fm.getFunctionContaining(ins.getAddress())
    if fn is None:
        insn_outside_fn += 1
    ft = ins.getFlowType()
    if ft.isJump() and ft.isComputed():
        computed_jumps += 1
        if len(ins.getFlows()) > 1:
            computed_jumps_resolved += 1
    if ft.isCall() and ft.isComputed():
        computed_calls += 1
        if ins.getNumOperands() > 0 and '[' in ins.getDefaultOperandRepresentation(0):
            computed_calls_mem += 1
    m = ins.getMnemonicString()
    if m.startswith('F') and m not in ('FEMMS',):
        mnem['x87'] += 1
        if fn is not None:
            x87_fns.add(fn.getEntryPoint())

w('instructions %d' % insn_count)
w('instruction_bytes %d (%.1f%% of .text)' % (insn_bytes, 100.0 * insn_bytes / text_size))
w('instructions_outside_functions %d' % insn_outside_fn)
w('computed_jumps %d resolved_as_switch %d' % (computed_jumps, computed_jumps_resolved))
w('computed_calls %d (through memory %d)' % (computed_calls, computed_calls_mem))

fns = [f for f in fm.getFunctions(True) if text.contains(f.getEntryPoint())]
sizes = sorted(f.getBody().getNumAddresses() for f in fns)
w('functions %d' % len(fns))
w('function_bytes %d' % sum(sizes))
w('function_size_median %d p90 %d max %d' % (sizes[len(sizes) // 2], sizes[int(len(sizes) * 0.9)], sizes[-1]))
w('thunks %d' % sum(1 for f in fns if f.isThunk()))
w('functions_with_x87 %d' % len(x87_fns))
w('x87_instructions %d' % mnem['x87'])

# Bookmarks Ghidra left behind: errors and warnings are where analysis gave up.
bm = collections.Counter()
bit = prog.getBookmarkManager().getBookmarksIterator()
while bit.hasNext():
    b = bit.next()
    if b.getTypeString() in ('Error', 'Warning'):
        bm[(b.getTypeString(), b.getCategory(), b.getComment()[:60])] += 1
for (k, v) in bm.most_common(25):
    w('bookmark %s | %s | %s : %d' % (k[0], k[1], k[2], v))

# Undefined gaps in .text.
undef = 0
gaps = 0
ai = listing.getUndefinedRanges(prog.getAddressFactory().getAddressSet(text.getStart(), text.getEnd()), False, monitor)
for r in ai:
    undef += r.getLength()
    gaps += 1
w('undefined_bytes %d in %d ranges' % (undef, gaps))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines))
