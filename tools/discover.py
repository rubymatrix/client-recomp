"""The discovery pass: Ghidra headless over an unpacked image, to the per-build metadata.

  python tools/discover.py --label <build> --module FFXiMain.dll [--reuse] [--out <meta.json>]

Reads generated/images/<label>/<Module>.unpacked.dll and .retail.dll (tools/newbuild.py unpack),
works in generated/discovery/<label>/<Module>/ (a scratch Ghidra project and every step's report:
Square Enix code, gitignored), and writes meta/<Module>.<label>.meta.json.

The pipeline (discovery/, Jython post-scripts; notes on past runs in discovery/notes/):

  import + auto-analysis                 RecompStats -> 00-auto.stats.txt
  round 1 and 2:
    Discover   to a fixpoint             functions at orphaned pointer targets and referenced runs
    Discover2  to a fixpoint             decode never-touched pointer targets and aligned starts
    Switches, SwitchAudit                every jmp [reg*4+table] bounded from its guard, audited
    Conflicts                            decodes that overlap a real instruction
    BogusCheck APPLY                     apply the manual verdicts, list data decoded as code
  gate: conflicts 0, suspect_functions 0, SwitchAudit review 0 -- else stop (exit 2) with the
        reports named: read each site, add NOT_CODE / REDECODE / SWITCHES to discovery/verdicts.py,
        and run again with --reuse (keeps the analysed project)
  Classify, Diagnose, RecompStats        coverage reports
  Export                                 the metadata: addresses and shapes only

The build must be in discovery/verdicts.py first (tools/newbuild.py carry --write): the scripts
pick their manual verdicts by the image's PE timestamp.

Ghidra: --ghidra, else GHIDRA_INSTALL_DIR, else the newest C:/Dev/Ghidra/ghidra_*_PUBLIC or
~/ghidra*_PUBLIC. Tested with 11.3.2 (Jython bundled) and JDK 21.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402
import newbuild  # noqa: E402

ROOT = buildinfo.ROOT
SCRIPTS = os.path.join(ROOT, 'discovery')
PROJECT = 'discovery'
MAX_ROUNDS = 12


def find_ghidra(arg):
    cands = [arg, os.environ.get('GHIDRA_INSTALL_DIR')]
    cands += sorted(glob.glob('C:/Dev/Ghidra/ghidra_*_PUBLIC'), reverse=True)
    cands += sorted(glob.glob(os.path.expanduser('~/ghidra*_PUBLIC')), reverse=True)
    cands += sorted(glob.glob('/Applications/ghidra*_PUBLIC'), reverse=True)
    for c in cands:
        if not c:
            continue
        exe = os.path.join(c, 'support', 'analyzeHeadless.bat' if os.name == 'nt' else 'analyzeHeadless')
        if os.path.exists(exe):
            return exe
    raise SystemExit('Ghidra not found: pass --ghidra <install dir> or set GHIDRA_INSTALL_DIR')


class Run:
    def __init__(self, headless, work, image):
        self.headless, self.work, self.image = headless, work, image
        self.name = os.path.basename(image)
        self.step = 0

    def report(self, name):
        return os.path.join(self.work, name)

    def ghidra(self, scripts, imported=False):
        """One headless invocation running scripts [(script, [args])] in order; returns the log."""
        self.step += 1
        tag = '%02d-%s' % (self.step, '+'.join(s[0].split('.')[0] for s in scripts))
        cmd = [self.headless, self.work, PROJECT]
        cmd += ['-import', self.image, '-overwrite'] if imported else ['-process', self.name, '-noanalysis']
        cmd += ['-scriptPath', SCRIPTS]
        for script, args in scripts:
            cmd += ['-postScript', script] + args
        log = os.path.join(self.work, tag + '.log')
        t = time.time()
        print('> %s' % tag, end='', flush=True)
        with open(log, 'w') as f:
            rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
        print('  (%ds)' % (time.time() - t))
        text = open(log, errors='replace').read()
        # A failed post-script does not fail analyzeHeadless: look for the traceback.
        if rc or 'Traceback (most recent call last)' in text or 'SCRIPT ERROR' in text.upper():
            raise SystemExit('ghidra step %s failed (rc %d); see %s' % (tag, rc, log))
        return text

    def fixpoint(self, script, created):
        """Run script until created(report text) is 0."""
        for i in range(MAX_ROUNDS):
            out = self.report('%s.%d.txt' % (script.split('.')[0], self.step + 1))
            self.ghidra([(script, [out])])
            text = open(out).read()
            n = created(text)
            print('    %s' % text.splitlines()[0])
            if n == 0:
                return
        raise SystemExit('%s did not reach a fixpoint in %d rounds' % (script, MAX_ROUNDS))

    def first_int(self, path, key):
        m = re.search(r'%s\s+(\d+)' % key, open(path).read())
        return int(m.group(1)) if m else None


def discover_created(text):
    return int(re.search(r'created (\d+)', text).group(1))


def discover2_created(text):
    m = re.search(r"created \{([^}]*)\}", text)
    return sum(int(x) for x in re.findall(r':\s*(\d+)', m.group(1)))


def check_verdicts(image):
    """Fail fast if discovery/verdicts.py does not know this image's build (the scripts would)."""
    ns = newbuild.load_verdicts()
    ts = newbuild.pe_timestamp(image)
    module = os.path.basename(image).split('.')[0].lower()
    if ts not in ns['BUILDS']:
        raise SystemExit('discovery/verdicts.py does not know PE timestamp 0x%08x (%s): '
                         'run tools/newbuild.py carry --from <previous> --to <label> --write first' % (ts, image))
    if (module, ns['BUILDS'][ts]) not in ns['VERDICTS']:
        raise SystemExit('discovery/verdicts.py has no verdicts for (%r, %r)' % (module, ns['BUILDS'][ts]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', required=True, help='the build (generated/images/<label>/)')
    ap.add_argument('--module', required=True, choices=newbuild.MODULES)
    ap.add_argument('--out', help='default: meta/<Module>.<label>.meta.json')
    ap.add_argument('--reuse', action='store_true', help='keep the analysed project; skip import + auto-analysis')
    ap.add_argument('--ghidra', help='the Ghidra install folder')
    ap.add_argument('--export-anyway', action='store_true', help='export even if the gate fails')
    args = ap.parse_args()

    stem = args.module.split('.')[0]
    image = newbuild.image(args.label, args.module)
    retail = newbuild.image(args.label, args.module, 'retail')
    for p in (image, retail):
        if not os.path.exists(p):
            raise SystemExit('%s missing: tools/newbuild.py unpack --game <folder>' % p)
    check_verdicts(image)
    out = os.path.abspath(args.out or os.path.join(buildinfo.METADIR, '%s.%s.meta.json' % (stem, args.label)))
    work = os.path.join(ROOT, 'generated', 'discovery', args.label, stem)
    os.makedirs(work, exist_ok=True)
    run = Run(find_ghidra(args.ghidra), work, image)
    have_project = os.path.exists(os.path.join(work, PROJECT + '.gpr'))

    if not (args.reuse and have_project):
        print('import + auto-analysis (the slow step: minutes for FFXiMain.dll)')
        run.ghidra([('RecompStats.py', [run.report('00-auto.stats.txt')])], imported=True)

    gate = {}
    for rnd in (1, 2):
        print('round %d' % rnd)
        run.fixpoint('Discover.py', discover_created)
        run.fixpoint('Discover2.py', discover2_created)
        r = {k: run.report('%s.r%d.txt' % (k, rnd)) for k in ('Switches', 'SwitchAudit', 'Conflicts', 'BogusCheck')}
        run.ghidra([('Switches.py', [r['Switches']]), ('SwitchAudit.py', [r['SwitchAudit']]),
                    ('Conflicts.py', [r['Conflicts']]), ('BogusCheck.py', [r['BogusCheck'], 'APPLY'])])
        gate = {
            'conflicts': (run.first_int(r['Conflicts'], 'conflicts'), r['Conflicts']),
            'suspect_functions': (run.first_int(r['BogusCheck'], 'suspect_functions'), r['BogusCheck']),
            'switch review': (run.first_int(r['SwitchAudit'], 'review'), r['SwitchAudit']),
        }
        print('    ' + ', '.join('%s %s' % (k, v[0]) for k, v in gate.items()))

    bad = {k: v for k, v in gate.items() if v[0] != 0}
    if bad and not args.export_anyway:
        print('\nGATE FAILED; nothing exported. Read these, record verdicts in discovery/verdicts.py, '
              'then rerun with --reuse:')
        for k, (n, path) in bad.items():
            print('  %s %s: %s' % (k, n, path))
        print('Switches.py lines marked UNBOUNDED / BAD_TABLE are expected for guard-less switches '
              '(e.g. memcpy); SwitchAudit REVIEW lines are the ones that need a verdict.')
        sys.exit(2)

    print('reports')
    run.ghidra([('Classify.py', [run.report('Classify.txt')]), ('Diagnose.py', [run.report('Diagnose.txt')]),
                ('RecompStats.py', [run.report('RecompStats.txt')])])
    print('export')
    log = run.ghidra([('Export.py', [out, buildinfo.sha256(retail), args.label, args.module])])
    m = re.search(r'functions \d+ switches \d+ tail_jumps \d+', log)
    print('wrote %s: %s' % (out, m.group(0) if m else '?'))
    print('reports in %s' % work)


if __name__ == '__main__':
    main()
