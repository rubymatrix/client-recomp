"""Put the stand-in FFXiMain.dll into the FINAL FANTASY XI folder, or take it out again.

  python tools/install.py status    what is installed now
  python tools/install.py install   FFXiMain.dll <- build/host/FFXiMain.dll,
                                    FFXiMain.retail.dll <- the verified retail file
  python tools/install.py restore   FFXiMain.dll <- the verified retail file; remove the rest

The game folder and build are the ones tools/prepare.py chose (generated/build.json). The retail
copy used is generated/FFXiMain.retail.dll, checked against that build's SHA-256 every time. It
never overwrites a file it does not recognise (retail, or a stand-in it built). If the game folder
is under Program Files, run from an elevated shell.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import buildinfo  # noqa: E402
from buildinfo import sha256  # noqa: E402

RETAIL_COPY = os.path.join(ROOT, 'generated', 'FFXiMain.retail.dll')
STAND_IN = os.path.join(ROOT, 'build', 'host', 'FFXiMain.dll')
MARKER = b'FFXIRecompile'  # appears in every stand-in build (its dialog title)


def classify(path, want):
    if not os.path.exists(path):
        return 'missing'
    if sha256(path) == want:
        return 'retail'
    with open(path, 'rb') as f:
        if MARKER in f.read():
            return 'stand-in'
    return 'unknown'


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'status'
    build = buildinfo.current()
    want = build['ffximain_sha']
    if sha256(RETAIL_COPY) != want:
        raise SystemExit('%s is not build %s; run tools/prepare.py' % (RETAIL_COPY, build['build']))
    game = build['game']
    print('build %s in %s' % (build['build'], game))
    main_dll = os.path.join(game, 'FFXiMain.dll')
    side = os.path.join(game, 'FFXiMain.retail.dll')
    print('FFXiMain.dll:        %s' % classify(main_dll, want))
    print('FFXiMain.retail.dll: %s' % classify(side, want))
    if what == 'status':
        return
    try:
        if what == 'install':
            if classify(main_dll, want) not in ('retail', 'stand-in', 'missing'):
                raise SystemExit('refusing: %s is neither retail nor a stand-in (restore it by hand first)' % main_dll)
            shutil.copyfile(RETAIL_COPY, side)
            shutil.copyfile(STAND_IN, main_dll)
        elif what == 'restore':
            if classify(main_dll, want) == 'unknown':
                print('note: replacing an unrecognised FFXiMain.dll with the verified retail file')
            shutil.copyfile(RETAIL_COPY, main_dll)
            if os.path.exists(side):
                os.remove(side)
        else:
            raise SystemExit(__doc__)
    except PermissionError:
        raise SystemExit('permission denied: %s (under Program Files? run this from an elevated shell)' % game)
    print('done:')
    print('FFXiMain.dll:        %s' % classify(main_dll, want))
    print('FFXiMain.retail.dll: %s' % classify(side, want))


if __name__ == '__main__':
    main()
