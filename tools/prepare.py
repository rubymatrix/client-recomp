"""Produce generated/FFXiMain.unpacked.dll from the player's retail install.

  python tools/prepare.py [--game "<FINAL FANTASY XI folder>"]

Finds FFXiMain.dll in --game (default: the install folder in the game's registry key), identifies its build by
SHA-256 in meta/builds.json, and unpacks it statically with tools/unpack.py - FFXiMain.dll,
and FFXi.dll from the same folder, which must be the same build. The choice is recorded in
generated/build.json for build.py and install.py. Output lands in generated/, which is gitignored:
it is Square Enix code and never committed.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402


def registry_game():
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\PlayOnlineUS\InstallFolder', 0, winreg.KEY_READ | view)
            path, _ = winreg.QueryValueEx(k, '0001')
            return os.path.normpath(path)
        except OSError:
            continue
    raise SystemExit('FINAL FANTASY XI install not found in the registry; pass --game')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', help='the FINAL FANTASY XI folder (default: from the registry)')
    ap.add_argument('--out', default=os.path.join(buildinfo.ROOT, 'generated'))
    args = ap.parse_args()

    game = os.path.normpath(args.game) if args.game else registry_game()
    dll = os.path.join(game, 'FFXiMain.dll')
    ffxi = os.path.join(game, 'FFXi.dll')
    digest = buildinfo.sha256(dll)
    label = buildinfo.match(digest)
    if label is None:
        raise SystemExit('%s is build %s, which meta/builds.json does not know (a stand-in? run install.py restore)'
                         % (dll, digest))
    want = buildinfo.known()[label]['FFXi.dll']['sha256']
    if buildinfo.sha256(ffxi) != want:
        raise SystemExit('%s does not match FFXiMain.dll: build %s needs FFXi.dll %s' % (ffxi, label, want))

    os.makedirs(args.out, exist_ok=True)
    # Keep the verified retail files too: the build and the loader use these copies, so nothing
    # depends on the state of the install afterwards.
    shutil.copyfile(dll, os.path.join(args.out, 'FFXiMain.retail.dll'))
    shutil.copyfile(ffxi, os.path.join(args.out, 'FFXi.retail.dll'))
    unpack = os.path.join(HERE, 'unpack.py')
    out = os.path.join(args.out, 'FFXiMain.unpacked.dll')
    subprocess.check_call([sys.executable, unpack, dll, out])
    subprocess.check_call([sys.executable, unpack, ffxi, os.path.join(args.out, 'FFXi.unpacked.dll')])
    # One copy per build too: the next game update maps its addresses from this one
    # (tools/newbuild.py carry).
    keep = os.path.join(args.out, 'images', label)
    os.makedirs(keep, exist_ok=True)
    for name in ('FFXiMain.retail.dll', 'FFXiMain.unpacked.dll', 'FFXi.retail.dll', 'FFXi.unpacked.dll'):
        shutil.copyfile(os.path.join(args.out, name), os.path.join(keep, name))
    buildinfo.record(label, game)
    print('ok: %s (build %s, from %s)' % (out, label, game))


if __name__ == '__main__':
    main()
