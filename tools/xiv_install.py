"""Install FINAL FANTASY XIV on this Mac from the retail disc, start to finish.
  python3 tools/xiv_install.py [--disc <mounted disc>] [--install-dir <folder>] [--no-unpack] [--open]
                               [--server <host>] [--patch-cert <cert.der>] [--account-url <url>]

1. Finds the disc (a mounted volume with ffxivsetup.exe and data/data1.hdr) or takes --disc.
2. Unpacks it with tools/xiv_unpack.c (InstallShield cabinets, read natively with the vendored
   libunshield; none of the disc's code runs) into the install folder, laid out as the disc's
   setup lays it out: the launcher programs at the top, client\\ and data\\ beneath. Default
   folder: ~/Library/Application Support/FFXIV1_0/FINAL FANTASY XIV. A second run resumes.
3. Translates and compiles the launcher programs it finds there (ffxivboot, ffxivupdater; the
   patches bring ffxivlogin and ffxivgame), each with the address map for its build.
4. Puts "FINAL FANTASY XIV.app" in /Applications (~/Applications if that is not writable; or
   --app-dir), a launcher that starts the native ffxivboot through tools/xiv_run.py: from there boot checks the version with the server, the updater patches the
   install up to 1.23b, login signs in and the game starts, each rebuilt here when a patch
   replaces it.

--server is the host Square Enix's names go to (default 127.0.0.1); --patch-cert the server's patch
signing certificate (meteor-web PatchTool's patch_cert.der), which ffxivboot checks patch torrents
against; --account-url the server's login page (meteor-web: http://<host>:8090). The app's launcher
keeps them.

--test-env KEY=VALUE (repeatable) adds an environment setting to the launcher: tools/xiv_e2e.sh
runs an install in folders of its own (XIV_DOCUMENTS) and signs in by script (XIV_WEB_SCRIPT).

It makes .venv with the recompiler's Python packages on its first run. Clang comes from Xcode's
command line tools; install.command gets them first when missing. The install is the player's own disc; nothing from it
is copied into this checkout.
"""
import argparse
import glob
import os
import plistlib
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_xiv  # noqa: E402
import thirdparty  # noqa: E402

DEFAULT_INSTALL = os.path.expanduser('~/Library/Application Support/FFXIV1_0/FINAL FANTASY XIV')
UNPACK = os.path.join(ROOT, 'build', 'bin', 'xiv_unpack')
VENV = os.path.join(ROOT, '.venv')
PACKAGES = ['capstone>=5,<6', 'pefile>=2023', 'pycryptodome>=3.20']


def in_venv():
    """Run inside .venv (made on the first run): the recompiler needs capstone and pefile, the
    launch arguments pycryptodome"""
    py = os.path.join(VENV, 'bin', 'python3')
    if os.path.realpath(sys.prefix) == os.path.realpath(VENV):
        return
    ok = os.path.exists(py) and subprocess.run([py, '-c', 'import capstone, pefile, Crypto'], capture_output=True).returncode == 0
    if not ok:
        print('Setting up Python packages in %s' % VENV, flush=True)
        if not os.path.exists(py):
            subprocess.check_call(['/usr/bin/python3' if os.path.exists('/usr/bin/python3') else sys.executable, '-m', 'venv', VENV])
        subprocess.check_call([py, '-m', 'pip', 'install', '--disable-pip-version-check', '-q'] + PACKAGES)
    os.execv(py, [py] + sys.argv)


def default_app_dir():
    return '/Applications' if os.access('/Applications', os.W_OK) else os.path.expanduser('~/Applications')
PROGRAMS = ('ffxivboot', 'ffxivupdater', 'ffxivlogin')


def find_disc():
    """The mounted retail disc: ffxivsetup.exe beside data/data1.hdr."""
    for v in sorted(glob.glob('/Volumes/*')):
        if os.path.exists(os.path.join(v, 'ffxivsetup.exe')) and os.path.exists(os.path.join(v, 'data', 'data1.hdr')):
            return v
    return None


def build_unpack():
    thirdparty.build('unshield')
    src = os.path.join(HERE, 'xiv_unpack.c')
    if os.path.exists(UNPACK) and os.path.getmtime(UNPACK) >= os.path.getmtime(src):
        return
    os.makedirs(os.path.dirname(UNPACK), exist_ok=True)
    subprocess.check_call(['clang', '-O2', '-o', UNPACK, src] + thirdparty.flags('unshield')
                          + [thirdparty.archive('unshield'), '-lz', '-liconv'])


def unpack(disc, install):
    """xiv_unpack with a progress bar from its progress lines"""
    p = subprocess.Popen([UNPACK, os.path.join(disc, 'data', 'data1.hdr'), install], stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        parts = line.split()
        if parts and parts[0] == 'progress':
            done, total, nf, tf = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            frac = done / total if total else 1
            bar = '#' * int(frac * 40)
            sys.stdout.write('\r  [%-40s] %5.1f%%  %.1f / %.1f GB  %d / %d files' % (
                bar, 100 * frac, done / 1e9, total / 1e9, nf, tf))
            sys.stdout.flush()
    print()
    if p.wait():
        raise SystemExit('unpacking the disc failed')


APP_PLIST = {
    'CFBundleExecutable': 'FINAL FANTASY XIV',
    'CFBundleIdentifier': 'local.ffxiv10.launcher',  # the game's own bundle is local.ffxiv10.recomp
    'CFBundleName': 'FINAL FANTASY XIV',
    'CFBundlePackageType': 'APPL',
    'CFBundleInfoDictionaryVersion': '6.0',
    'CFBundleShortVersionString': '1.23b',
    'CFBundleVersion': '1',
    'LSApplicationCategoryType': 'public.app-category.role-playing-games',
    'LSSupportsGameMode': True,
    'NSHighResolutionCapable': True,
    # the launcher is a script, which macOS may otherwise start under Rosetta: the Python it runs
    # (and the recompiler's arm64-only libraries) must be native
    'LSArchitecturePriority': ['arm64'],
    'LSRequiresNativeExecution': True,
}


def make_app(app_dir, install, server, cert, account_url, test_env):
    """The app is a launcher: it starts ffxivboot (xiv_run.py) with the server's settings. The game
    runs from its own bundle in build/ (build_xiv.py), so macOS sees a game when it runs."""
    APP = os.path.join(app_dir, 'FINAL FANTASY XIV.app')
    macos = os.path.join(APP, 'Contents', 'MacOS')
    os.makedirs(macos, exist_ok=True)
    plist = dict(APP_PLIST)
    # ffxivboot.exe's icon, as the retail shortcut showed it
    if build_xiv.exe_icns(os.path.join(install, 'ffxivboot.exe'), os.path.join(APP, 'Contents', 'Resources', 'AppIcon.icns')):
        plist['CFBundleIconFile'] = 'AppIcon'
    with open(os.path.join(APP, 'Contents', 'Info.plist'), 'wb') as f:
        plistlib.dump(plist, f)
    launcher = os.path.join(macos, 'FINAL FANTASY XIV')
    env = 'export XIV_SERVER_HOST="%s"\n' % server
    if cert:
        env += 'export XIV_PATCH_CERT="%s"\n' % os.path.abspath(cert)
    if account_url:
        env += 'export XIV_ACCOUNT_URL="%s"\n' % account_url
    for kv in test_env:
        k, v = kv.split('=', 1)
        env += 'export %s="%s"\n' % (k, v)
    with open(launcher, 'w') as f:
        f.write('#!/bin/sh\n# FINAL FANTASY XIV: the native ffxivboot of this install (tools/xiv_install.py)\n' + env +
                'exec /usr/bin/arch -arm64 "%s" "%s" ffxivboot "%s" >> "%s" 2>&1\n' % (
                    os.path.join(ROOT, '.venv', 'bin', 'python'), os.path.join(HERE, 'xiv_run.py'),
                    os.path.join(install, 'ffxivboot.exe'), os.path.join(ROOT, 'build', 'run.log')))
    os.chmod(launcher, 0o755)
    return APP


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disc', help='the mounted retail disc (default: found under /Volumes)')
    ap.add_argument('--install-dir', default=DEFAULT_INSTALL)
    ap.add_argument('--no-unpack', action='store_true', help='the install folder is already unpacked')
    ap.add_argument('--open', action='store_true', help='start the app when done')
    ap.add_argument('--server', default='127.0.0.1', help="the server's host (Square Enix's names go there)")
    ap.add_argument('--patch-cert', help="the server's patch signing certificate (patch_cert.der)")
    ap.add_argument('--account-url', help="the server's login page, e.g. http://127.0.0.1:8090")
    ap.add_argument('--app-dir', help='where the app goes (default /Applications, or ~/Applications)')
    ap.add_argument('--test-env', action='append', default=[], metavar='KEY=VALUE', help=argparse.SUPPRESS)
    a = ap.parse_args()
    in_venv()
    caller = os.environ.get('XIV_CALLER_DIR', os.getcwd())  # install.command runs this from the sources
    here = lambda p: p and os.path.join(caller, os.path.expanduser(p))
    a.install_dir, a.patch_cert, a.disc = here(a.install_dir), here(a.patch_cert), here(a.disc)
    install = os.path.abspath(a.install_dir)

    if not a.no_unpack:
        disc = a.disc or find_disc()
        if not disc:
            raise SystemExit('Insert or mount the FINAL FANTASY XIV disc (or give --disc).')
        print('Unpacking %s into %s' % (disc, install))
        build_unpack()
        unpack(disc, install)
    if not os.path.exists(os.path.join(install, 'ffxivboot.exe')):
        raise SystemExit('%s has no ffxivboot.exe' % install)

    for name in PROGRAMS:
        exe = os.path.join(install, name + '.exe')
        if not os.path.exists(exe):
            continue  # the patches bring it
        if build_xiv.program_current(exe, name):
            print('%s: ready' % name)
            continue
        print('%s: translating and compiling (a few minutes)' % name)
        build_xiv.program(exe, name, False, [])
    game = os.path.join(install, 'ffxivgame.exe')
    if os.path.exists(game):
        print('ffxivgame: built when it is first started (after patching to 1.23b)')

    app = make_app(os.path.expanduser(a.app_dir or default_app_dir()), install, a.server, a.patch_cert, a.account_url,
                   a.test_env)
    print('Ready: %s' % app)
    if a.open:
        subprocess.call(['open', app])


if __name__ == '__main__':
    main()
