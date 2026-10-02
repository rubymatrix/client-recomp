# export the per-build recompilation metadata. Addresses and shapes only: no bytes from the
# game image are written, so the file carries no Square Enix code (README, Rules).
# Args: <out.json> <sha256 of the retail, packed DLL> <build label> [module name]
# The module name defaults to the imported image's name: FFXiMain.unpacked.dll -> FFXiMain.dll.
# Jython headless post-script, read-only.
# @category FFXIRecomp
import json
import re

out_path, sha256, label = getScriptArgs()[0], getScriptArgs()[1], getScriptArgs()[2]
prog = currentProgram
module = getScriptArgs()[3] if len(getScriptArgs()) > 3 else prog.getName().split('.')[0] + '.dll'
listing = prog.getListing()
fm = prog.getFunctionManager()
text = prog.getMemory().getBlock('.text')
SCALED = re.compile(r'JMP dword ptr \[(E[A-Z]{2})\*0x4 \+ (0x[0-9a-f]+)\]')

functions = []
for f in fm.getFunctions(True):
    if not text.contains(f.getEntryPoint()):
        continue
    ranges = [[r.getMinAddress().getOffset(), r.getMaxAddress().getOffset() + 1] for r in f.getBody()]
    functions.append({'entry': f.getEntryPoint().getOffset(), 'ranges': ranges,
                      'thunk': f.isThunk()})

switches = []
tail_jumps = []
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    if ins.getMnemonicString() != 'JMP':
        continue
    ft = ins.getFlowType()
    m = SCALED.match(ins.toString())
    if m:
        targets = sorted(set(x.getOffset() for x in ins.getFlows()))
        switches.append({'at': ins.getAddress().getOffset(), 'table': int(m.group(2), 16),
                         'index_reg': m.group(1), 'targets': targets})
    elif ft.isComputed() or ft.isCall():
        # Indirect jump that is not a switch: a tail call through a vtable, the IAT or a register.
        tail_jumps.append(ins.getAddress().getOffset())

meta = {
    'format': 'ffxi-recomp-meta/1',
    'module': module,
    'build': label,
    'sha256': sha256,
    'image_base': prog.getImageBase().getOffset(),
    'text': [text.getStart().getOffset(), text.getEnd().getOffset() + 1],
    'functions': functions,
    'switches': switches,
    'tail_jumps': tail_jumps,
}
f = open(out_path, 'w')
json.dump(meta, f, separators=(',', ':'))
f.close()
print('functions %d switches %d tail_jumps %d' % (len(functions), len(switches), len(tail_jumps)))
