# diagnostics: where is the code Ghidra did not attach to a function, and which computed jumps
# are unresolved. Jython, run as a headless post-script against an analysed FFXiMain.
# @category FFXIRecomp
import collections

out_path = getScriptArgs()[0]
prog = currentProgram
listing = prog.getListing()
mem = prog.getMemory()
fm = prog.getFunctionManager()
text = mem.getBlock('.text')
af = prog.getAddressFactory()

def in_text(a):
    return a is not None and text.contains(a)

# Data -> code pointer targets, from the relocation table.
ptr_targets = set()
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
    a = af.getDefaultAddressSpace().getAddress(v)
    if in_text(a):
        ptr_targets.add(a)

lines = []
def w(s):
    lines.append(s)

w('pointer_targets %d' % len(ptr_targets))
at_fn_entry = sum(1 for a in ptr_targets if fm.getFunctionAt(a) is not None)
inside_fn = sum(1 for a in ptr_targets if fm.getFunctionAt(a) is None and fm.getFunctionContaining(a) is not None)
no_insn = sum(1 for a in ptr_targets if listing.getInstructionAt(a) is None)
w('  at_function_entry %d  inside_function %d  no_instruction %d  orphan_instruction %d' % (
    at_fn_entry, inside_fn, no_insn, len(ptr_targets) - at_fn_entry - inside_fn - no_insn))

# Orphan runs: maximal runs of consecutive instructions not inside any function.
runs = []
cur = None
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    a = ins.getAddress()
    if not text.contains(a):
        break
    if fm.getFunctionContaining(a) is None:
        if cur is not None and cur[1] == a:
            cur[1] = ins.getMaxAddress().add(1)
            cur[2] += 1
        else:
            cur = [a, ins.getMaxAddress().add(1), 1]
            runs.append(cur)
    else:
        cur = None

w('orphan_runs %d instructions %d' % (len(runs), sum(r[2] for r in runs)))
start_is_ptr = sum(1 for r in runs if r[0] in ptr_targets)
w('  run_start_is_pointer_target %d' % start_is_ptr)

# How is each run reached? Look at references to its first instruction.
reach = collections.Counter()
examples = collections.defaultdict(list)
for r in runs:
    refs = list(prog.getReferenceManager().getReferencesTo(r[0]))
    if r[0] in ptr_targets:
        k = 'data pointer'
    elif not refs:
        k = 'no references'
    else:
        types = set(str(x.getReferenceType()) for x in refs)
        k = 'refs: ' + ','.join(sorted(types))
    reach[k] += 1
    if len(examples[k]) < 5:
        examples[k].append('%s len=%d' % (r[0], r[2]))
for k, v in reach.most_common():
    w('  reached_by %-40s %d   e.g. %s' % (k, v, '; '.join(examples[k])))

sizes = sorted(r[2] for r in runs)
if sizes:
    w('  run_len_median %d p90 %d max %d' % (sizes[len(sizes) // 2], sizes[int(len(sizes) * 0.9)], sizes[-1]))

# First instruction of each run: what does it look like? (push ebp / sub esp = prologue)
first = collections.Counter()
for r in runs:
    first[listing.getInstructionAt(r[0]).toString().split(' ')[0]] += 1
w('  first_mnemonic %s' % first.most_common(10))

# Unresolved computed jumps.
forms = collections.Counter()
unres = []
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    ft = ins.getFlowType()
    if ft.isJump() and ft.isComputed() and len(ins.getFlows()) <= 1:
        s = ins.toString()
        fn = fm.getFunctionContaining(ins.getAddress())
        form = s.split(' ', 1)[1] if ' ' in s else s
        import re
        form = re.sub(r'0x[0-9a-fA-F]+', 'IMM', form)
        forms[form] += 1
        unres.append((ins.getAddress(), s, fn.getName() if fn else '-'))
w('unresolved_computed_jumps %d' % len(unres))
for k, v in forms.most_common(15):
    w('  form %-40s %d' % (k, v))
for a, s, f in unres[:20]:
    w('  e.g. %s  %-36s in %s' % (a, s, f))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines))
