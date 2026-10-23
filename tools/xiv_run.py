"""Start one of FINAL FANTASY XIV's programs as its native build, rebuilding it first when the install's
exe is not the build it was made from.
  python3 tools/xiv_run.py <ffxivboot|ffxivlogin|ffxivupdater|ffxivgame> <the install's exe> [argument]

FINAL FANTASY XIV.app starts ffxivboot through this, and the programs start each other through it
(tests/xivexe.c, XIV_RUNNER). The updater replaces ffxivboot.exe and ffxivupdater.exe, and its
patches bring ffxivlogin.exe and ffxivgame.exe: each build has its own address map in meta/, and a
program whose exe changed is translated and compiled again here (minutes, once per build).
ffxivgame is xivboot (tools/build_xiv.py xivboot), given its argument through XIV_ARGS.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_xiv  # noqa: E402

LOG = os.path.join(ROOT, 'build', 'run.log')


def tick():
    """GetTickCount as every program here reads it: the system's monotonic milliseconds"""
    return int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000) & 0xFFFFFFFF


def restamp(arg, made):
    """A sqex argument made at tick `made`, stamped with now: the programs take one only within a
    minute of its T (ffxivboot, ffxivlogin), and a rebuild takes longer"""
    import xiv_args
    if not arg.startswith('sqex000'):
        return arg
    version = int(arg[7])
    fields = xiv_args.decode(arg, made)
    fields.pop('T', None)
    return xiv_args.encode(fields, version, tick())


def notify(text):
    subprocess.call(['osascript', '-e', 'display notification "%s" with title "FINAL FANTASY XIV"' % text])


def rebuild(cmd, what):
    notify('Preparing %s for this Mac. This takes a few minutes, once.' % what)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as log:
        rc = subprocess.call([sys.executable, os.path.join(HERE, 'build_xiv.py')] + cmd, cwd=ROOT, stdout=log,
                             stderr=subprocess.STDOUT)
    if rc:
        notify('Could not prepare %s (see build/run.log).' % what)
        raise SystemExit(rc)


BUILDING = os.path.join(ROOT, 'build', 'xivboot.building')  # a game build in progress (its pid)


def game_current(exe):
    stamp = os.path.join(ROOT, 'build', 'xivboot.sha256')
    boot = os.path.join(ROOT, build_xiv.XIV_APP, 'Contents', 'MacOS', 'xivboot')
    return os.path.exists(boot) and os.path.exists(stamp) and open(stamp).read().strip() == build_xiv.sha256(exe)


def building():
    """the pid of a game build in progress, or None"""
    try:
        pid = int(open(BUILDING).read())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return None


def prebuild_game(install):
    """Patching has brought ffxivgame.exe: build the game now, while the player signs in, so it is
    ready when login starts it (main waits for this build rather than starting another)"""
    exe = os.path.join(install, 'ffxivgame.exe')
    if not os.path.exists(exe) or game_current(exe) or building():
        return
    with open(os.path.join(ROOT, build_xiv.META)) as f:
        if build_xiv.sha256(exe) not in f.read(4096):
            return  # not 1.23b yet: login comes after the patches, so this is rare
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    log = open(LOG, 'a')
    p = subprocess.Popen([sys.executable, '-c',
                          'import os,subprocess,sys; open(%r,"w").write(str(os.getpid())); '
                          'rc=subprocess.call([sys.executable,%r,"xivboot","--exe",%r],cwd=%r); os.remove(%r); sys.exit(rc)'
                          % (BUILDING, os.path.join(HERE, 'build_xiv.py'), exe, ROOT, BUILDING)],
                         stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    notify('Preparing the game for this Mac while you sign in.')
    return p


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    name, exe, rest = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3:]
    started = tick()
    if name == 'ffxivgame':
        boot = os.path.join(ROOT, build_xiv.XIV_APP, 'Contents', 'MacOS', 'xivboot')
        while building():  # started when login opened (prebuild_game)
            time.sleep(1)
        if not game_current(exe):
            rebuild(['xivboot', '--exe', exe], 'the game')
            rest = [restamp(rest[0], started)] + rest[1:] if rest else rest
        env = dict(os.environ)
        if rest:
            env['XIV_ARGS'] = rest[0]
        env['XIV_TICK'] = hex(tick())  # the game's clock starts at the system's, the argument's window
        os.execve(boot, ['xivboot', exe], env)
    if name == 'ffxivlogin':
        prebuild_game(os.path.dirname(exe))
    if not build_xiv.program_current(exe, name):
        build_xiv.program_meta(exe, name)  # stops here, saying so, when this build has no address map
        rebuild(['exe', '--exe', exe, '--name', name], name)
        rest = [restamp(rest[0], started)] + rest[1:] if rest else rest
    binary = os.path.join(ROOT, 'build', 'bin', name)
    os.execv(binary, [name, exe] + rest)


if __name__ == '__main__':
    main()
