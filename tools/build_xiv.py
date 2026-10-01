"""Build driver for FINAL FANTASY XIV 1.23b ffxivgame.exe on arm64 macOS (clang).

  python3 tools/build_xiv.py meta --exe <ffxivgame.exe> --ghidra <meteor-decomp/build/ghidra>
        export meta/ffxivgame.1.23b.meta.json from meteor-decomp's Ghidra project
        (discovery/ExportFFXIV.java, headless, read-only)
  python3 tools/build_xiv.py translate --exe <ffxivgame.exe>
        recomp/recomp.py --all into generated/ffxivgame (Square Enix code: gitignored)
  python3 tools/build_xiv.py xivboot --exe <ffxivgame.exe> [--run] [-- args...]
        the boot test (tests/xivboot.c): the translation plus the portable runtime

Objects are rebuilt when missing or older than their source or than the newest runtime header
(the generated code's too: guest.h holds the Guest layout every translation compiles against).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thirdparty  # noqa: E402  (vendored SDL3, as tools/build_posix.py builds it)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META = 'meta/ffxivgame.1.23b.meta.json'
GEN = 'generated/ffxivgame'
CFLAGS = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I',
          'runtime/portable', '-I', GEN]
GEN_FLAGS = ['-O1', '-w']  # 115k translated functions: -O1 builds several times faster than -O2
PORTABLE = ['runtime/runtime.c', 'runtime/portable/plat_posix.c', 'runtime/portable/gwin.c',
            'runtime/portable/gthread.c', 'runtime/portable/thunk.c', 'runtime/portable/pe.c',
            'runtime/portable/k32.c', 'runtime/portable/vfs.c',
            'runtime/portable/kobj.c', 'runtime/portable/k32_io.c',
            'runtime/portable/reg.c', 'runtime/portable/ole.c',
            'runtime/portable/k32_misc.c', 'runtime/portable/k32_xiv.c',
            'runtime/portable/xiv_misc.c']


# Win32 shims on SDL3 (windowing, input, timers, sockets, audio) shared with FFXI.
HOST = ['runtime/portable/user32.c', 'runtime/portable/input.c', 'runtime/portable/ws2.c',
        'runtime/portable/dinput.c', 'runtime/portable/dsound.c']


def run(cmd):
    line = ' '.join(shlex.quote(c) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


def newest_header():
    newest = 0
    for d in ('runtime', 'runtime/portable'):
        for f in glob.glob(os.path.join(ROOT, d, '*.h')):
            newest = max(newest, os.path.getmtime(f))
    return newest


def compile_stale(sources, objdir, extra=()):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    headers = newest_header()
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full = os.path.join(ROOT, obj)
        src_time = max(os.path.getmtime(os.path.join(ROOT, s)), headers)
        if not os.path.exists(full) or os.path.getmtime(full) < src_time:
            flags = CFLAGS + list(extra) + (GEN_FLAGS if s.startswith('generated/') else [])
            jobs.append(['clang', '-c'] + flags + [s, '-o', obj])
    if jobs:
        print('compiling %d of %d' % (len(jobs), len(sources)), flush=True)
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            for cmd, rc in zip(jobs, ex.map(lambda c: subprocess.call(c, cwd=ROOT), jobs)):
                if rc:
                    failed.append(cmd[-3])
        if failed:
            raise SystemExit('compile failed: ' + ', '.join(failed))
    return objs


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def meta(exe, ghidra):
    head = glob.glob('/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless')
    if not head:
        raise SystemExit('Ghidra not found (brew install ghidra)')
    run([head[0], os.path.abspath(ghidra), 'ffxivgame', '-process', 'ffxivgame.exe', '-noanalysis', '-readOnly',
         '-scriptPath', 'discovery', '-postScript', 'ExportFFXIV.java', os.path.join(ROOT, META), sha256(exe), '1.23b'])


def translate(exe):
    run([sys.executable, 'recomp/recomp.py', '--meta', META, '--image', exe, '--retail', exe, '--out', GEN, '--all'])


def xivboot(exe, run_it, extra):
    if not os.path.exists(os.path.join(ROOT, GEN, 'table.c')):
        translate(exe)
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, GEN, '*.c')))
    objs = compile_stale(gen, 'build/xiv/gen')
    thirdparty.build('sdl3')
    objs += compile_stale(PORTABLE + HOST + ['tests/xivboot.c'], 'build/xiv/obj', thirdparty.flags('sdl3'))
    run(['clang', '-o', 'build/xivboot'] + objs + thirdparty.libs('sdl3') + ['-lm', '-lpthread'])
    if run_it:
        run(['build/xivboot', exe] + extra)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=('meta', 'translate', 'xivboot'))
    ap.add_argument('--exe', required=True, help='ffxivgame.exe (1.23b, 2012.09.19.0001)')
    ap.add_argument('--ghidra', help='meteor-decomp/build/ghidra (meta)')
    ap.add_argument('--run', action='store_true')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if a.target == 'meta':
        meta(exe, a.ghidra or os.path.join(ROOT, '..', 'decomp', 'build', 'ghidra'))
    elif a.target == 'translate':
        translate(exe)
    else:
        xivboot(exe, a.run, a.rest)


if __name__ == '__main__':
    main()
