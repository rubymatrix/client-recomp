# flag functions that are probably data decoded as code. Signals:
#   - share of `add byte ptr [eax],al` (bytes 00 00) instructions
#   - instructions a 2002 MSVC build never emits in user code (privileged, BCD, segment loads)
#   - no references to the entry at all
# Arg 2 = REMOVE=<addr,addr,...> clears those functions and their code units (manual verdicts).
# Jython headless post-script.
# @category FFXIRecomp
args = getScriptArgs()
out_path = args[0]
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
text = prog.getMemory().getBlock('.text')
sp = prog.getAddressFactory().getDefaultAddressSpace()

RARE = set(['IN', 'OUT', 'INSB', 'INSD', 'OUTSB', 'OUTSD', 'HLT', 'ARPL', 'BOUND', 'LES', 'LDS', 'INTO',
            'AAA', 'AAS', 'AAM', 'AAD', 'DAA', 'DAS', 'SALC', 'CLI', 'STI', 'LOCK', 'IRET', 'IRETD',
            'RETF', 'CALLF', 'JMPF', 'ICEBP', 'INT1', 'SIDT', 'SGDT', 'LGDT', 'LIDT'])  # not WAIT: CRT x87 code uses fwait

from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd

# Manual verdicts, per module and build: verdicts.py.
import os, sys
sys.path.insert(0, os.path.dirname(getSourceFile().getAbsolutePath()))
import verdicts as manual
NOT_CODE = manual.verdicts(prog)['NOT_CODE']
REDECODE = manual.verdicts(prog)['REDECODE']

if len(args) > 1 and args[1] == 'APPLY':
    for v in NOT_CODE:
        a = sp.getAddress(v)
        f = fm.getFunctionAt(a)
        if f is not None:
            body = f.getBody()
            fm.removeFunction(a)
            listing.clearCodeUnits(body.getMinAddress(), body.getMaxAddress(), False)
            print('removed %s' % a)
    for v in REDECODE:
        a = sp.getAddress(v)
        DisassembleCommand(a, None, True).applyTo(prog, monitor)
        f = fm.getFunctionContaining(a)
        if f is not None:
            CreateFunctionCmd.fixupFunctionBody(prog, f, monitor)
        else:
            createFunction(a, None)
        print('redecoded %s' % a)

rows = []
for f in fm.getFunctions(True):
    if not text.contains(f.getEntryPoint()):
        continue
    n = zeros = rare = 0
    for ins in listing.getInstructions(f.getBody(), True):
        n += 1
        b = ins.getBytes()
        if len(b) == 2 and b[0] == 0 and b[1] == 0:
            zeros += 1
        if ins.getMnemonicString() in RARE:
            rare += 1
    refd = rm.hasReferencesTo(f.getEntryPoint())
    score = (zeros * 3 + rare * 5) / float(max(n, 1))
    if score >= 0.3 or (rare >= 2 and not refd):
        rows.append((score, f.getEntryPoint(), n, zeros, rare, refd))

rows.sort(reverse=True)
lines = ['suspect_functions %d' % len(rows)]
for score, a, n, z, r, refd in rows:
    lines.append('  %s score=%.2f insns=%d zero_adds=%d rare=%d referenced=%s' % (a, score, n, z, r, refd))
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines[:40]))
