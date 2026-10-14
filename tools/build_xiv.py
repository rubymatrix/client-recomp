"""Build driver for FINAL FANTASY XIV 1.23b ffxivgame.exe on arm64 macOS (clang).

  python3 tools/build_xiv.py meta --exe <ffxivgame.exe> --ghidra <meteor-decomp/build/ghidra>
        export meta/ffxivgame.1.23b.meta.json from meteor-decomp's Ghidra project
        (discovery/ExportFFXIV.java, headless, read-only)
  python3 tools/build_xiv.py translate --exe <ffxivgame.exe>
        recomp/recomp.py --all into generated/ffxivgame (Square Enix code: gitignored)
  python3 tools/build_xiv.py xivboot --exe <ffxivgame.exe> [--run] [-- args...]
        the boot test (tests/xivboot.c): the translation plus the portable runtime
  python3 tools/build_xiv.py xivboot --exe <ffxivgame.exe> --debug [--opt -O0]
        build/xivboot-dbg for lldb (tools/xivdbg.sh): no LTO, -g on the translation so every
        source line is one /* guest-address: x86 */ instruction; objects in build/xiv-dbg

Objects are rebuilt when missing or older than their source or than the newest runtime header
(the generated code's too: guest.h holds the Guest layout every translation compiles against).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thirdparty  # noqa: E402  (vendored SDL3, as tools/build_posix.py builds it)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META = 'meta/ffxivgame.1.23b.meta.json'
APPLY = 'meta/ffxivgame.1.23b.apply.json'
# functions the host may replace or watch (recomp.py --wraps; runtime/portable/xiv_ui.c)
WRAPS = ','.join('ui_%s=%s' % m for m in re.findall(r'^ANIM\((\w+), (0x[0-9a-f]+)\)', open(
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime/portable/xiv_ui_wraps.h')).read(), re.M))  # Garlemald Client's code patches, translated in
WRAPS += ',key_repeat=0xd429b0'  # the keyboard's auto-repeat counter, paced in time (xiv_ui.c)
WRAPS += ',pad_repeat=0xd42940'  # the gamepad's auto-repeat counter, paced in time (xiv_ui.c)
# the client's Lua C++ bindings, watched by the probe (runtime/portable/xiv_probe.c, XIV_PROBE)
WRAPS += ''.join(',lb_%s=0x%s' % (a, a) for a in re.findall(r'^LB\((\w+),', open(
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime/portable/xiv_probe_binds.h')).read(), re.M))
GEN = 'generated/ffxivgame'
CFLAGS = ['-O2', '-mcpu=apple-m1', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-DRT_GUEST_FIXED=0x300000000ull', '-fno-strict-aliasing', '-I', 'runtime', '-I',
          'runtime/portable', '-I', GEN]
GEN_FLAGS = ['-O3', '-mcpu=apple-m1', '-flto=thin', '-w']  # speed over build time: these are the game's code
PORTABLE = ['runtime/runtime.c', 'runtime/portable/plat_posix.c', 'runtime/portable/gwin.c',
            'runtime/portable/gthread.c', 'runtime/portable/thunk.c', 'runtime/portable/pe.c',
            'runtime/portable/k32.c', 'runtime/portable/vfs.c',
            'runtime/portable/kobj.c', 'runtime/portable/k32_io.c',
            'runtime/portable/reg.c', 'runtime/portable/ole.c',
            'runtime/portable/k32_misc.c', 'runtime/portable/k32_xiv.c',
            'runtime/portable/xiv_misc.c', 'runtime/portable/fiber.c', 'runtime/portable/xiv_ui.c', 'runtime/portable/xiv_probe.c', 'runtime/portable/sampler_posix.c']


# Win32 shims on SDL3 (windowing, input, timers, sockets, audio) shared with FFXI.
HOST = ['runtime/portable/user32.c', 'runtime/portable/input.c', 'runtime/portable/autoinput.c', 'runtime/portable/ws2.c',
        'runtime/portable/dinput.c', 'runtime/portable/dsound.c', 'runtime/portable/d3d9.c', 'runtime/portable/d3dx9.c', 'runtime/portable/xaudio2.c',
        'runtime/portable/sm_msl.c', 'runtime/portable/gfx9_metal.m']
GFX_LIBS = ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation']


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


def compile_stale(sources, objdir, extra=(), gen_flags=None):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    headers = newest_header()
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full = os.path.join(ROOT, obj)
        src_time = max(os.path.getmtime(os.path.join(ROOT, s)), headers)
        if not os.path.exists(full) or os.path.getmtime(full) < src_time:
            flags = CFLAGS + list(extra) + ((gen_flags or GEN_FLAGS) if s.startswith('generated/') else [])
            if s.endswith('.m'):  # Objective-C, references counted by hand
                flags = [f for f in flags if f != '-std=c11'] + ['-fno-objc-arc']
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
    run([sys.executable, 'recomp/recomp.py', '--meta', META, '--apply', APPLY, '--image', exe, '--retail', exe, '--out',
         GEN, '--all', '--wraps', WRAPS])


def xivboot(exe, run_it, extra, debug=False, opt='-O1'):
    if not os.path.exists(os.path.join(ROOT, GEN, 'table.c')):
        translate(exe)
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, GEN, '*.c')))
    if debug:
        # lldb build: line tables + locals for the translation, no LTO so functions keep their
        # own frames (b f_XXXXXXXX stops where you expect). Objects stay in build/xiv-dbg; the
        # binary's debug map points at them, so no dsymutil step is needed.
        objs = compile_stale(gen, 'build/xiv-dbg/gen', gen_flags=[opt, '-g', '-mcpu=apple-m1', '-w'])
        thirdparty.build('sdl3')
        objs += compile_stale(PORTABLE + HOST + ['tests/xivboot.c'], 'build/xiv-dbg/obj', thirdparty.flags('sdl3'))
        run(['clang', '-o', 'build/xivboot-dbg', '-g', '-mcpu=apple-m1']
            + objs + thirdparty.libs('sdl3') + GFX_LIBS + ['-lm', '-lpthread'])
        if run_it:
            run(['build/xivboot-dbg', exe] + extra)
        return
    objs = compile_stale(gen, 'build/xiv/gen')
    thirdparty.build('sdl3')
    objs += compile_stale(PORTABLE + HOST + ['tests/xivboot.c'], 'build/xiv/obj', thirdparty.flags('sdl3'))
    # ThinLTO: the translated functions call each other across 291 files; inlining the small ones
    # into their callers needs the whole program (cache: relinks reuse unchanged modules)
    run(['clang', '-o', 'build/xivboot', '-flto=thin', '-O3', '-mcpu=apple-m1',
         '-Wl,-cache_path_lto,build/xiv/lto-cache', '-Wl,-mllvm,-threads=%d' % (os.cpu_count() or 4)]
        + objs + thirdparty.libs('sdl3') + GFX_LIBS + ['-lm', '-lpthread'])
    xiv_bundle()
    if run_it:
        run(['build/xivboot', exe] + extra)


XIV_APP = 'build/FINAL FANTASY XIV.app'
XIV_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>xivboot</string>
	<key>CFBundleIdentifier</key><string>local.ffxiv10.recomp</string>
	<key>CFBundleName</key><string>FINAL FANTASY XIV</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>1.23b</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
	<key>LSSupportsGameMode</key><true/>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
'''


def xiv_bundle():
    """build/xivboot inside an app bundle (a hard link), so macOS sees a game: Game Mode turns on
    when it runs full screen. xiv_login.py starts this copy."""
    macos = os.path.join(ROOT, XIV_APP, 'Contents', 'MacOS')
    os.makedirs(macos, exist_ok=True)
    exe = os.path.join(macos, 'xivboot')
    if os.path.lexists(exe):
        os.remove(exe)
    os.link(os.path.join(ROOT, 'build', 'xivboot'), exe)
    with open(os.path.join(ROOT, XIV_APP, 'Contents', 'Info.plist'), 'w') as f:
        f.write(XIV_PLIST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=('meta', 'translate', 'xivboot'))
    ap.add_argument('--exe', required=True, help='ffxivgame.exe (1.23b, 2012.09.19.0001)')
    ap.add_argument('--ghidra', help='meteor-decomp/build/ghidra (meta)')
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--debug', action='store_true', help='build/xivboot-dbg: -g translation, no LTO (for lldb)')
    ap.add_argument('--opt', default='-O1', help='optimization for the translation in --debug builds (-O0 keeps every register local visible but runs slowly)')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if a.target == 'meta':
        meta(exe, a.ghidra or os.path.join(ROOT, '..', 'decomp', 'build', 'ghidra'))
    elif a.target == 'translate':
        translate(exe)
    else:
        xivboot(exe, a.run, a.rest, a.debug, a.opt)


if __name__ == '__main__':
    main()
