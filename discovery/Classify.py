# classify every byte of .text that is not an instruction, and every data->code pointer whose
# target is not an instruction. Jython headless post-script, read-only.
# @category FFXIRecomp
import collections

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')
af = prog.getAddressFactory()
rm = prog.getReferenceManager()
space = af.getDefaultAddressSpace()

lines = []
def w(s):
    lines.append(s)

def byte_at(a):
    return mem.getByte(a) & 0xFF

# --- Undefined / data ranges in .text ---------------------------------------------------------
kinds = collections.Counter()
kind_bytes = collections.Counter()
examples = collections.defaultdict(list)
aset = af.getAddressSet(text.getStart(), text.getEnd())
cu_it = listing.getCodeUnits(aset, True)
# Merge consecutive non-instruction code units into ranges.
ranges = []
cur = None
while cu_it.hasNext():
    cu = cu_it.next()
    if listing.getInstructionAt(cu.getAddress()) is not None:
        cur = None
        continue
    if cur is not None and cur[1].equals(cu.getAddress()):
        cur[1] = cu.getMaxAddress().add(1)
    else:
        cur = [cu.getAddress(), cu.getMaxAddress().add(1)]
        ranges.append(cur)

for (s, e) in ranges:
    n = e.subtract(s)
    bs = [byte_at(s.add(i)) for i in range(min(n, 4096))]
    refs_in = False
    a = s
    for i in range(min(n, 4096)):
        if rm.hasReferencesTo(s.add(i)):
            refs_in = True
            break
    prev_fn = fm.getFunctionContaining(s.subtract(1)) if s.compareTo(text.getStart()) > 0 else None
    if all(b == 0xCC for b in bs):
        k = 'int3 padding'
    elif all(b == 0x90 for b in bs):
        k = 'nop padding'
    elif all(b == 0x00 for b in bs):
        k = 'zero fill'
    elif refs_in:
        # jump tables / index tables MSVC emits after a function body
        d = listing.getDataAt(s)
        k = 'referenced data (%s)' % (d.getDataType().getName() if d is not None else 'undefined')
        if 'pointer' in k or 'addr' in k.lower():
            k = 'referenced data (pointer table)'
    elif n < 16 and all(b in (0xCC, 0x90, 0x8D, 0x49, 0x00, 0x64, 0x24, 0x9B, 0xA4) for b in bs):
        k = 'alignment filler (lea/nop forms)'
    else:
        k = 'unreferenced, non-padding'
    kinds[k] += 1
    kind_bytes[k] += n
    if len(examples[k]) < 6:
        examples[k].append('%s+%d' % (s, n))

w('non_instruction_ranges %d bytes %d' % (len(ranges), sum(kind_bytes.values())))
for k, v in kind_bytes.most_common():
    w('  %-40s ranges %6d bytes %8d  e.g. %s' % (k, kinds[k], v, ', '.join(examples[k])))

# --- Pointer targets that are not instructions -------------------------------------------------
tgt = collections.OrderedDict()
rit = prog.getRelocationTable().getRelocations()
while rit.hasNext():
    r = rit.next()
    src = r.getAddress()
    if text.contains(src):
        continue
    try:
        v = mem.getInt(src) & 0xFFFFFFFF
    except:
        continue
    a = space.getAddress(v)
    if text.contains(a) and listing.getInstructionAt(a) is None:
        tgt.setdefault(a, []).append(src)

pk = collections.Counter()
pex = collections.defaultdict(list)
for a, srcs in tgt.items():
    ins = listing.getInstructionContaining(a)
    if ins is not None:
        k = 'middle of an instruction'
    else:
        d = listing.getDataContaining(a)
        fn = fm.getFunctionContaining(a)
        b = byte_at(a)
        if b == 0xCC:
            k = 'int3 padding'
        elif d is not None and d.isDefined():
            k = 'defined data: %s' % d.getDataType().getName()
        else:
            k = 'undefined bytes'
        # Is the source a vtable-looking run (neighbours also point into .text)?
    src_blk = mem.getBlock(srcs[0]).getName()
    k = '%s  <- from %s' % (k, src_blk)
    pk[k] += 1
    if len(pex[k]) < 6:
        pex[k].append('%s<-%s' % (a, srcs[0]))
w('pointer_targets_not_instruction %d' % len(tgt))
for k, v in pk.most_common():
    w('  %-60s %d  e.g. %s' % (k, v, ', '.join(pex[k])))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines))
