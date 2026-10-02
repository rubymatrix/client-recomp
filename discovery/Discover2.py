# function discovery, pass 2: code Ghidra never disassembled.
#   A. data -> code pointer targets that are undefined bytes (callback tables in .data/.rdata)
#   B. 16-byte-aligned addresses right after nop/int3 padding (MSVC 6 function alignment)
# Every candidate is disassembled; if the disassembly hits a bad instruction the candidate is
# rolled back. Jython headless post-script; modifies the program. Run until it creates 0.
# @category FFXIRecomp
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.program.model.address import AddressSet

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')
space = prog.getAddressFactory().getDefaultAddressSpace()
bm = prog.getBookmarkManager()

def is_undefined(a):
    return listing.getInstructionContaining(a) is None and not (
        listing.getDataContaining(a) is not None and listing.getDataContaining(a).isDefined())

cands = []
rit = prog.getRelocationTable().getRelocations()
while rit.hasNext():
    r = rit.next()
    if text.contains(r.getAddress()):
        continue
    try:
        v = mem.getInt(r.getAddress()) & 0xFFFFFFFF
    except:
        continue
    a = space.getAddress(v)
    if text.contains(a) and is_undefined(a) and (mem.getByte(a) & 0xFF) not in (0xCC, 0x90, 0x00):
        cands.append(('ptr', a))

# B: scan undefined space for padding -> aligned non-padding byte.
start = text.getStart()
size = text.getSize()
jbuf = __import__('jarray').zeros(size, 'b')
mem.getBytes(start, jbuf)
i = 16
while i < size:
    b = jbuf[i] & 0xFF
    p = jbuf[i - 1] & 0xFF
    if (start.getOffset() + i) % 16 == 0 and b not in (0x90, 0xCC, 0x00):
        a = start.add(i)
        if is_undefined(a):
            if p in (0x90, 0xCC):
                cands.append(('aligned', a))
            else:
                # C: no padding, but the previous instruction ends the flow exactly here
                # (unreferenced accessors packed back to back, e.g. 0x10014e70).
                prev = listing.getInstructionContaining(a.subtract(1))
                if prev is not None and prev.getMaxAddress().add(1).equals(a) and not prev.getFlowType().hasFallthrough():
                    cands.append(('after_ret', a))
    i += 1

# Manual verdicts (verdicts.py, per module and build): never seed these.
import os, sys
sys.path.insert(0, os.path.dirname(getSourceFile().getAbsolutePath()))
import verdicts as manual
NOT_CODE = set(manual.verdicts(prog)['NOT_CODE'])
cands = [(k, a) for (k, a) in cands if a.getOffset() not in NOT_CODE]

created = {'ptr': 0, 'aligned': 0, 'after_ret': 0}
rolled_back = {'ptr': 0, 'aligned': 0, 'after_ret': 0}
rb_examples = []
for kind, a in cands:
    if not is_undefined(a):
        continue
    before = bm.getBookmarkCount('Error')
    cmd = DisassembleCommand(a, None, True)
    cmd.applyTo(prog, monitor)
    body = cmd.getDisassembledAddressSet()
    bad = bm.getBookmarkCount('Error') > before
    if not bad:
        # Also refuse candidates whose disassembly ran into non-text memory or is a single padding byte.
        if body is None or body.isEmpty():
            bad = True
    if bad:
        if body is not None and not body.isEmpty():
            listing.clearCodeUnits(body.getMinAddress(), body.getMaxAddress(), False)
            bm.removeBookmarks(body, 'Error', monitor)
        rolled_back[kind] += 1
        if len(rb_examples) < 15:
            rb_examples.append('%s %s' % (kind, a))
        continue
    if fm.getFunctionContaining(a) is None and createFunction(a, None) is not None:
        created[kind] += 1

lines = ['candidates %d created %s rolled_back %s functions_now %d' % (len(cands), created, rolled_back, fm.getFunctionCount())]
lines += ['  rolled back %s' % e for e in rb_examples]
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print(lines[0])
