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


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    name, exe, rest = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3:]
    started = tick()
    if name == 'ffxivgame':
        stamp = os.path.join(ROOT, 'build', 'xivboot.sha256')
        boot = os.path.join(ROOT, build_xiv.XIV_APP, 'Contents', 'MacOS', 'xivboot')
        if not (os.path.exists(boot) and os.path.exists(stamp) and open(stamp).read().strip() == build_xiv.sha256(exe)):
            rebuild(['xivboot', '--exe', exe], 'the game')
            rest = [restamp(rest[0], started)] + rest[1:] if rest else rest
        env = dict(os.environ)
        if rest:
            env['XIV_ARGS'] = rest[0]
        env['XIV_TICK'] = hex(tick())  # the game's clock starts at the system's, the argument's window
        os.execve(boot, ['xivboot', exe], env)
    if not build_xiv.program_current(exe, name):
        build_xiv.program_meta(exe, name)  # stops here, saying so, when this build has no address map
        rebuild(['exe', '--exe', exe, '--name', name], name)
        rest = [restamp(rest[0], started)] + rest[1:] if rest else rest
    binary = os.path.join(ROOT, 'build', 'bin', name)
    os.execv(binary, [name, exe] + rest)


if __name__ == '__main__':
    main()
