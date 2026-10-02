# function discovery: create functions at every entry Ghidra's auto-analysis left orphaned.
# Jython headless post-script; modifies the program. Run repeatedly until it reports 0 created.
# Seeds, in order of confidence:
#   1. data -> code pointer targets (vtables, callback tables) that are orphan instructions
#   2. the first instruction of every orphan run that something references (DATA or jump)
# @category FFXIRecomp
out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')
af = prog.getAddressFactory()
rm = prog.getReferenceManager()

seeds = []

rit = prog.getRelocationTable().getRelocations()
while rit.hasNext():
    r = rit.next()
    if text.contains(r.getAddress()):
        continue
    try:
        v = mem.getInt(r.getAddress()) & 0xFFFFFFFF
    except:
        continue
    a = af.getDefaultAddressSpace().getAddress(v)
    if text.contains(a) and listing.getInstructionAt(a) is not None and fm.getFunctionContaining(a) is None:
        seeds.append(a)

# A run start is an orphan instruction that nothing falls through into: the previous instruction
# is not adjacent, belongs to a function, or ends its flow (ret / unconditional jmp).
prev = None
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    a = ins.getAddress()
    if not text.contains(a):
        break
    if fm.getFunctionContaining(a) is None and rm.hasReferencesTo(a):
        starts_run = (prev is None
                      or not prev.getMaxAddress().add(1).equals(a)
                      or fm.getFunctionContaining(prev.getAddress()) is not None
                      or not prev.getFlowType().hasFallthrough())
        if starts_run:
            seeds.append(a)
    prev = ins

created = 0
failed = []
seen = set()
for a in seeds:
    if a in seen or fm.getFunctionContaining(a) is not None:
        continue
    seen.add(a)
    f = createFunction(a, None)
    if f is None:
        failed.append(a)
    else:
        created += 1

msg = 'seeds %d created %d failed %d functions_now %d' % (len(seeds), created, len(failed), fm.getFunctionCount())
lines = [msg] + ['  failed %s' % a for a in failed[:30]]
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print(msg)
