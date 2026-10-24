"""Start one of FINAL FANTASY XIV's programs as its native build, and build the others in the background
as the patches bring them.
  python3 tools/xiv_run.py <ffxivboot|ffxivlogin|ffxivupdater|ffxivgame> <the install's exe> [argument]
  python3 tools/xiv_run.py --watch <install folder>

FINAL FANTASY XIV.app starts ffxivboot through this, and the programs start each other through it
(tests/xivexe.c). Each build of a program has its own address map in meta/; a program is translated
and compiled for the exe the install has now (minutes, once per build). ffxivgame is xivboot
(tools/build_xiv.py xivboot), given its argument through XIV_ARGS.

The first start also starts a watcher (--watch) for the install: while the programs run, it builds
each exe the patches bring or replace as soon as it has settled - the patched ffxivupdater while
ffxivboot downloads the game's patches, ffxivlogin and ffxivgame while the updater applies them - so
each is ready by the time it is started. Builds run one at a time (build/build.lock); a program
whose build is under way waits for it.
"""
import fcntl
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_xiv  # noqa: E402

LOG = os.path.join(ROOT, 'build', 'run.log')
BUILD_LOCK = os.path.join(ROOT, 'build', 'build.lock')
WATCH_LOCK = os.path.join(ROOT, 'build', 'watch.lock')
PROGRAMS = ('ffxivboot', 'ffxivupdater', 'ffxivlogin', 'ffxivgame')
WHAT = {'ffxivboot': 'the launcher', 'ffxivupdater': 'the updater', 'ffxivlogin': 'the login window',
        'ffxivgame': 'the game'}


def tick():
    """GetTickCount as every program here reads it: the system's monotonic milliseconds"""
    return int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000) & 0xFFFFFFFF


def restamp(arg, made):
    """A sqex argument made at tick `made`, stamped with now: the programs take one only within a
    minute of its T (ffxivboot, ffxivlogin), and a build takes longer"""
    import xiv_args
    if not arg.startswith('sqex000'):
        return arg
    version = int(arg[7])
    fields = xiv_args.decode(arg, made)
    fields.pop('T', None)
    return xiv_args.encode(fields, version, tick())


def notify(text):
    subprocess.call(['osascript', '-e', 'display notification "%s" with title "FINAL FANTASY XIV"' % text])


def log(text):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('[xiv_run] %s %s\n' % (time.strftime('%H:%M:%S'), text))


def current(name, exe):
    """whether the native build of this program is the one for this exe"""
    if name != 'ffxivgame':
        return build_xiv.program_current(exe, name)
    stamp = os.path.join(ROOT, 'build', 'xivboot.sha256')
    boot = os.path.join(ROOT, build_xiv.XIV_APP, 'Contents', 'MacOS', 'xivboot')
    return os.path.exists(boot) and os.path.exists(stamp) and open(stamp).read().strip() == build_xiv.sha256(exe)


def buildable(name, exe):
    """whether meta/ has an address map for this exe (the patches bring builds that are only steps
    on the way to 1.23b: those are not built)"""
    if name == 'ffxivgame':
        with open(os.path.join(ROOT, build_xiv.META)) as f:
            return build_xiv.sha256(exe) in f.read(4096)
    try:
        build_xiv.program_meta(exe, name)
        return True
    except SystemExit:
        return False


def progress_window(what):
    """tools/xiv_progress.m, built here the first time: a window with the build's steps and a bar,
    fed the build's progress lines (None when it cannot be shown)"""
    src = os.path.join(HERE, 'xiv_progress.m')
    binary = os.path.join(ROOT, 'build', 'bin', 'xiv_progress')
    if not os.path.exists(binary) or os.path.getmtime(binary) < os.path.getmtime(src):
        os.makedirs(os.path.dirname(binary), exist_ok=True)
        if subprocess.call(['clang', '-fobjc-arc', '-O2', '-framework', 'AppKit', '-o', binary, src]):
            return None
    w = subprocess.Popen([binary], stdin=subprocess.PIPE, text=True, start_new_session=True)
    w.stdin.write('title %s\n' % what)
    w.stdin.flush()
    return w


def build(name, exe):
    """build the program for this exe, after any build under way (one at a time); nothing to do
    when that build was this one"""
    os.makedirs(os.path.dirname(BUILD_LOCK), exist_ok=True)
    with open(BUILD_LOCK, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if current(name, exe):
            return
        log('building %s for %s' % (name, exe))
        cmd = ['xivboot', '--exe', exe] if name == 'ffxivgame' else ['exe', '--exe', exe, '--name', name]
        window = progress_window(WHAT[name])
        p = subprocess.Popen([sys.executable, os.path.join(HERE, 'build_xiv.py')] + cmd, cwd=ROOT, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=dict(os.environ, XIV_PROGRESS='1'))
        with open(LOG, 'a') as out:
            for line in p.stdout:
                if line.startswith('@progress '):
                    if window:
                        try:
                            window.stdin.write(line)
                            window.stdin.flush()
                        except OSError:
                            window = None  # closed: the build carries on
                else:
                    out.write(line)
                    out.flush()
        rc = p.wait()
        if window:
            window.stdin.close()
        log('%s %s' % (name, 'built' if rc == 0 else 'failed (%d)' % rc))
        if rc:
            notify('Could not prepare %s (see build/run.log).' % WHAT[name])
            raise SystemExit(rc)


def chain_running(install):
    """whether one of the programs runs (the watcher's reason to keep going): by name, as the
    updater runs from the downloads folder, not the install; or a build or start for this install"""
    out = subprocess.run(['ps', '-axo', 'pid=,args='], capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, args = line.strip().partition(' ')
        first = args.split(' ', 1)[0]
        if int(pid) == os.getpid() or '--watch' in args:
            continue
        if first in ('ffxivboot', 'ffxivupdater', 'ffxivlogin', 'xivboot') or (install in args and 'xiv_run.py' in args):
            return True
    return False


def watch(install):
    """--watch: build each program the install has an exe for, once that exe has settled (the
    updater writes it in pieces), while the programs run; stop two minutes after they all end"""
    with open(WATCH_LOCK, 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return  # another watcher has this
        log('watching %s' % install)
        idle_since = time.time()
        while time.time() - idle_since < 120:
            for name in PROGRAMS:
                exe = os.path.join(install, name + '.exe')
                try:
                    settled = time.time() - os.path.getmtime(exe) > 10
                except OSError:
                    continue
                if settled and not current(name, exe) and buildable(name, exe):
                    try:
                        build(name, exe)
                    except SystemExit:
                        pass  # said so in the log; the program's own start tries again
            if chain_running(install):
                idle_since = time.time()
            time.sleep(5)
        log('watcher done')


def start_watcher(install):
    with open(WATCH_LOCK, 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(lock, fcntl.LOCK_UN)
        except OSError:
            return  # one runs already
    with open(LOG, 'a') as out:
        subprocess.Popen([sys.executable, os.path.abspath(__file__), '--watch', install], stdout=out,
                         stderr=subprocess.STDOUT, start_new_session=True, cwd=ROOT)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--watch':
        return watch(os.path.abspath(sys.argv[2]))
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    name, exe, rest = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3:]
    started = tick()
    # the updater may run from Documents\...\downloads: the install is the starting program's
    install = os.environ.get('XIV_INSTALL') or os.path.dirname(exe)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    start_watcher(install)
    if not current(name, exe):
        build_xiv.program_meta(exe, name) if name != 'ffxivgame' else None  # says so when no address map
        build(name, exe)
    if rest and (tick() - started) & 0xFFFFFFFF > 30000:
        rest = [restamp(rest[0], started)] + rest[1:]
    if name == 'ffxivgame':
        env = dict(os.environ)
        if rest:
            env['XIV_ARGS'] = rest[0]
        env['XIV_TICK'] = hex(tick())  # the game's clock starts at the system's, the argument's window
        os.execve(os.path.join(ROOT, build_xiv.XIV_APP, 'Contents', 'MacOS', 'xivboot'), ['xivboot', exe], env)
    os.execv(os.path.join(ROOT, 'build', 'bin', name), [name, exe] + rest)


if __name__ == '__main__':
    main()
