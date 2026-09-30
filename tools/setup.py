"""Set up FINAL FANTASY XI on this Mac, start to finish. install.sh and setup.command run it.

  python3 tools/setup.py [--game <FINAL FANTASY XI folder>] [--install-dir <folder>] [--no-install] [--open]
                         [--server <name>] [--resolution WxH] [--window-mode 0-3] [--dats <folder>|none]
                         [--settings-only] [--no-open]
  python3 tools/setup.py --find            list the game folders found on this Mac, and stop

It finds the game folder the player got from their server (or takes --game), checks that its
version is one meta/builds.json knows, makes .venv with the recompiler's two Python packages,
translates the game's code and compiles it (the vendored SDL3 and mbedtls too), signs
"Final Fantasy XI.app" with a code-signing certificate of this Mac's own (made on the first run),
and copies the app to /Applications (~/Applications if that is not writable).

On a terminal it asks for the server (127.0.0.1 when left blank), the window's resolution and mode
(the menus' resolution and the interface's shape follow from them) and a folder of DAT overlays
(none when left blank); the options above answer instead. Each question starts from what the player
has now. The answers go into the app's Info.plist (host/appdefaults.h), and into the sign-in
screen's saved files when there are some, so they hold on a run after the first too.
--settings-only asks the questions, prints what the build would get, and stops: nothing is built or
saved. --no-open wins over --open (install.sh and setup.command give --open).

Run it again after the server hands out a new game version: it rebuilds what changed. Clang
comes from Xcode's command line tools; install.sh and setup.command get them first when missing.
The whole build's output goes to build/setup.log.
"""
import argparse
import math
import os
import platform
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402
import thirdparty  # noqa: E402

ROOT = os.path.dirname(HERE)
LOG = os.path.join(ROOT, 'build', 'setup.log')
VENV = os.path.join(ROOT, '.venv')
APP_NAME = 'Final Fantasy XI'
PACKAGES = ['capstone>=5,<6', 'pefile>=2023']
IDENTITY = 'FFXI Local Code Signing'
# where the app keeps its files (SDL_GetPrefPath("FFXIRecompile", "FFXI")); FFXI_DATA_DIR stands in
# for it in tests (tools/test_install.sh), so they leave the player's own settings alone
# relative folders the player gives are from where they ran install.sh, which runs this in the sources
CALLER_DIR = os.environ.get('FFXI_CALLER_DIR') or os.getcwd()
DATA_DIR = os.environ.get('FFXI_DATA_DIR') or os.path.expanduser('~/Library/Application Support/FFXIRecompile/FFXI')

STEPS = ['Checking this Mac', 'Finding the game', 'Choosing settings', 'Getting the recompiler ready', 'Building libraries',
         "Translating the game's code", 'Compiling', 'Making the app', 'Installing']
PHASES = {'translate': 5, 'compile': 6, 'app': 7}  # build_posix.py's @phase names -> STEPS


def step(n):
    print('\n[%d/%d] %s' % (n + 1, len(STEPS), STEPS[n]), flush=True)


def note(text):
    print('  ' + text, flush=True)


def progress(fraction, detail=''):
    width = 30
    fill = int(fraction * width)
    sys.stdout.write('\r  [%s%s] %3d%%  %s' % ('#' * fill, '.' * (width - fill), fraction * 100, detail[:40].ljust(40)))
    if fraction >= 1:
        sys.stdout.write('\n')
    sys.stdout.flush()


class Failure(Exception):
    pass


def user_path(p):
    """A folder the player named: ~ expanded, relative to where they ran setup."""
    return os.path.abspath(os.path.join(CALLER_DIR, os.path.expanduser(p)))


def log(text):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write(text if text.endswith('\n') else text + '\n')


def run(cmd, progress=None, **kw):
    """Runs cmd with its output in the log; progress(line) sees each line. Raises Failure."""
    log('> ' + ' '.join(cmd))
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors='replace', **kw)
    tail = []
    for line in p.stdout:
        log(line)
        tail = (tail + [line.rstrip()])[-15:]
        if progress:
            progress(line)
    if p.wait():
        raise Failure('%s failed. The last lines were:\n%s\n(everything is in %s)'
                      % (os.path.basename(cmd[0]) if cmd[0] != sys.executable else ' '.join(cmd[1:3]),
                         '\n'.join('    ' + t for t in tail), LOG))


# ---- the game folder ----------------------------------------------------------------------------

def is_game(folder):
    """A FINAL FANTASY XI folder: both DLLs and the ROM folder with the game's data."""
    return all(os.path.exists(os.path.join(folder, n)) for n in ('FFXiMain.dll', 'FFXi.dll', 'ROM'))


def identify(folder):
    """{'path', 'build' (a label, or None when unsupported), 'sha256'}."""
    digest = buildinfo.sha256(os.path.join(folder, 'FFXiMain.dll'))
    return {'path': folder, 'build': buildinfo.match(digest), 'sha256': digest}


def candidates():
    """Folders on this Mac that hold FFXiMain.dll: the usual places, then Spotlight."""
    home = os.path.expanduser('~')
    rel = [os.path.join('PlayOnline', 'SquareEnix', 'FINAL FANTASY XI'),
           os.path.join('SquareEnix', 'FINAL FANTASY XI'), 'FINAL FANTASY XI']
    places = [home, os.path.join(home, 'Games'), os.path.join(home, 'Downloads'), os.path.join(home, 'Desktop'),
              os.path.join(home, 'Documents'), '/Applications', os.path.join(home, 'Applications')]
    # Wine and CrossOver installs
    for pf in ('Program Files (x86)', 'Program Files'):
        places.append(os.path.join(home, '.wine', 'drive_c', pf))
        bottles = os.path.join(home, 'Library', 'Application Support', 'CrossOver', 'Bottles')
        if os.path.isdir(bottles):
            places += [os.path.join(bottles, b, 'drive_c', pf) for b in os.listdir(bottles)]
    found = []
    for p in places:
        for r in rel:
            found.append(os.path.join(p, r))
    try:
        out = subprocess.run(['mdfind', 'kMDItemFSName == "FFXiMain.dll"'], capture_output=True, text=True,
                             timeout=20).stdout
        found += [os.path.dirname(l) for l in out.splitlines() if l.strip()]
    except (OSError, subprocess.TimeoutExpired):
        pass
    seen, result = set(), []
    for f in found:
        f = os.path.realpath(f)
        # our own copies of the game's DLLs (generated/) are not a game folder
        if f in seen or f.startswith(os.path.realpath(ROOT) + os.sep) or '/.Trash/' in f:
            continue
        seen.add(f)
        if is_game(f):
            result.append(identify(f))
    return result


def ask_for_folder():
    """A Finder dialog, for a person running setup.command."""
    script = ('POSIX path of (choose folder with prompt "Choose the FINAL FANTASY XI folder '
              '(the one with FFXiMain.dll in it) from your server\'s game files:")')
    r = subprocess.run(['osascript', '-e', script], capture_output=True, text=True)
    return r.stdout.strip().rstrip('/') or None


def pick_game(given):
    if given:
        folder = user_path(given)
        if is_game(folder):
            return identify(folder)
        inner = os.path.join(folder, 'FINAL FANTASY XI')  # the SquareEnix folder, or the one above it
        for f in (inner, os.path.join(folder, 'SquareEnix', 'FINAL FANTASY XI'),
                  os.path.join(folder, 'PlayOnline', 'SquareEnix', 'FINAL FANTASY XI')):
            if is_game(f):
                return identify(f)
        raise Failure('%s is not a FINAL FANTASY XI folder (it needs FFXiMain.dll, FFXi.dll and ROM). Choose the FINAL FANTASY XI folder from your '
                      "server's game files." % folder)
    note('Looking for the game files on this Mac...')
    found = candidates()
    good = [c for c in found if c['build']]
    if len(good) == 1:
        return good[0]
    if len(good) > 1 and sys.stdin.isatty():
        print('  Found more than one copy of the game:')
        for i, c in enumerate(good, 1):
            print('    %d. %s' % (i, c['path']))
        while True:
            a = input('  Which one? [1-%d] ' % len(good)).strip()
            if a.isdigit() and 1 <= int(a) <= len(good):
                return good[int(a) - 1]
    if good:
        return good[0]
    if found:
        note('Found the game at %s, but not a version this build supports.' % found[0]['path'])
    note('Choose the FINAL FANTASY XI folder in the window that opens.')
    chosen = ask_for_folder()
    if not chosen:
        raise Failure('No game folder chosen.')
    return pick_game(chosen)


# ---- the settings -------------------------------------------------------------------------------

# settings.reg's 0034, in the order they are offered
WINDOW_MODES = [(1, 'windowed'), (2, 'borderless window'), (3, 'borderless full screen'), (0, 'full screen')]


def installed_defaults(where):
    """The FFXI* keys of the app a previous setup installed: {} if there is none."""
    for d in [where] if where else ['/Applications', os.path.expanduser('~/Applications')]:
        try:
            with open(os.path.join(d, APP_NAME + '.app', 'Contents', 'Info.plist'), 'rb') as f:
                return {k: v for k, v in plistlib.load(f).items() if k.startswith('FFXI')}
        except (OSError, plistlib.InvalidFileException):
            continue
    return {}


def saved_value(name, key):
    """key's value in one of the sign-in screen's saved files: signin.cfg and modern.cfg (key=value),
    settings.reg ("key"=dword:hex, as an int). None when it has none."""
    try:
        with open(os.path.join(DATA_DIR, name), newline='') as f:
            text = f.read()
    except OSError:
        return None
    if name.endswith('.reg'):
        m = re.search(r'^"%s"=dword:([0-9a-fA-F]+)' % key, text, re.M)
        return int(m.group(1), 16) if m else None
    m = re.search(r'^%s=(.*?)\r?$' % re.escape(key), text, re.M)
    return m.group(1) if m else None


def save_value(name, key, value):
    """Sets key in one of those files when the file is there (the app writes it first otherwise)."""
    path = os.path.join(DATA_DIR, name)
    try:
        with open(path, newline='') as f:
            text = f.read()
    except OSError:
        return
    if name.endswith('.reg'):
        line, pattern = '"%s"=dword:%08x' % (key, value), r'^"%s"=dword:[0-9a-fA-F]+' % key
        eol = '\r\n'
    else:
        line, pattern = '%s=%s' % (key, value), r'^%s=.*?(?=\r?$)' % re.escape(key)
        eol = '\n'
    new, n = re.subn(pattern, lambda _: line, text, count=1, flags=re.M)
    if not n:
        new = text + ('' if not text or text.endswith('\n') else eol) + line + eol
    with open(path, 'w', newline='') as f:
        f.write(new)


def parse_resolution(text):
    """'2560x1440' (or with * or spaces) as (w, h); None if it is not one the game takes."""
    m = re.fullmatch(r'\s*(\d+)\s*[xX*×]\s*(\d+)\s*', text or '')
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    return (w, h) if 640 <= w <= 16384 and 480 <= h <= 16384 else None


def aspect_name(w, h):
    g = math.gcd(w, h)
    return '%d:%d' % (w // g, h // g)


def derive(w, h):
    """The menus' resolution and the interface's shape (--ui-aspect) for a w x h window. Wider than
    16:9, the interface keeps 16:9 in the middle; otherwise it has the window's shape. The menus are
    half the window's height (384 at least, the game's smallest), at the interface's shape."""
    ui = 16 / 9 if w / h > 16 / 9 + 0.01 else w / h
    menu_h = min(h, max(384, h // 2))
    menu_w = max(512, int(round(menu_h * ui / 2)) * 2)
    return (menu_w, menu_h), ('16:9' if ui != w / h else 'off')


def has_dats(folder):
    """An overlay (ROM*/sound* folders in it) or a folder of overlays (host/host64.c --dats)."""
    def one(d):
        try:
            return any(re.fullmatch(r'(rom|sound)\d*', n, re.I) and os.path.isdir(os.path.join(d, n))
                       for n in os.listdir(d))
        except OSError:
            return False
    if one(folder):
        return True
    try:
        return any(one(os.path.join(folder, n)) for n in os.listdir(folder))
    except OSError:
        return False


def ask(question, default, parse):
    """A question on the terminal: Enter takes default; parse(answer) is the value, or None to ask again."""
    while True:
        a = input('  %s%s: ' % (question, ' [%s]' % default if default else '')).strip()
        v = parse(a if a else default)
        if v is not None:
            return v
        note('Try again.')


def choose_settings(a):
    """{server, resolution (w, h), window_mode, dats ('' for none), menu_resolution, ui_aspect}: from
    the options, else asked on a terminal, starting from what the player has now."""
    prev = installed_defaults(a.install_dir and user_path(a.install_dir))
    tty = sys.stdin.isatty()

    server = a.server or saved_value('signin.cfg', 'server') or prev.get('FFXIServer') or '127.0.0.1'
    if tty and not a.server:
        server = ask('Server (name or address; blank for 127.0.0.1)', server,
                     lambda t: t if re.fullmatch(r'[A-Za-z0-9.:\-\[\]]+', t) else None)

    w, h = saved_value('settings.reg', '0001'), saved_value('settings.reg', '0002')
    res = parse_resolution(a.resolution) if a.resolution else None
    if a.resolution and not res:
        raise Failure('--resolution %s: a size like 1920x1080 (640x480 at least).' % a.resolution)
    was = (w, h) if w and h and w >= 640 and h >= 480 else parse_resolution(prev.get('FFXIResolution')) or (1920, 1080)
    if not res:
        res = ask('Resolution', '%dx%d' % was, parse_resolution) if tty else was

    mode = a.window_mode
    if mode is None:
        mode = saved_value('settings.reg', '0034')
        if mode not in (0, 1, 2, 3):
            mode = int(prev.get('FFXIWindowMode', 1))
        if tty:
            print('  Window mode:')
            for i, (m, name) in enumerate(WINDOW_MODES, 1):
                print('    %d. %s' % (i, name))
            now = [m for m, _ in WINDOW_MODES].index(mode) + 1
            mode = ask('Which one?', str(now), lambda t: WINDOW_MODES[int(t) - 1][0]
                       if t.isdigit() and 1 <= int(t) <= len(WINDOW_MODES) else None)

    def dats_folder(t):
        if t.lower() in ('', 'none', '-'):
            return ''
        # quoted, or with its spaces escaped as a folder dragged into Terminal is
        d = user_path(re.sub(r'\\(.)', r'\1', t.strip().strip('\'"')))
        if not os.path.isdir(d):
            note('%s is not a folder.' % d)
            return None
        if not has_dats(d):
            note('%s has no ROM or sound folders, in it or in a folder in it.' % d)
            return None
        return d
    if a.dats is not None:
        dats = dats_folder(a.dats)
        if dats is None:
            raise Failure('--dats %s: a folder of DATs laid out like the game\'s (ROM, ROM2, ..., sound), '
                          'or a folder of those; none for no overlay.' % a.dats)
    else:
        dats = prev.get('FFXIDats', '')
        if tty:
            dats = ask('DAT overlay folder (%s)' % ('"none" for no overlay' if dats else 'blank for none'), dats,
                       dats_folder)

    menu, ui = derive(*res)
    note('Server %s; %dx%d (%s), %s; menus %dx%d%s; %s.'
         % (server, res[0], res[1], aspect_name(*res), dict(WINDOW_MODES)[mode], menu[0], menu[1],
            ', interface kept 16:9 in the middle' if ui != 'off' else '',
            'DATs from ' + dats if dats else 'no DAT overlay'))
    return {'server': server, 'resolution': res, 'window_mode': mode, 'dats': dats,
            'menu_resolution': menu, 'ui_aspect': ui, 'resized': res != was}


def save_settings(s):
    """The answers into the saved files a previous run left (Info.plist values count only on the first)."""
    save_value('signin.cfg', 'server', s['server'])
    save_value('settings.reg', '0034', s['window_mode'])
    if s['resized']:
        # the menus and the interface's shape follow the window; kept when it stays the same size
        for key, v in zip(('0001', '0002', '0037', '0038'), s['resolution'] + s['menu_resolution']):
            save_value('settings.reg', key, v)
        save_value('modern.cfg', 'ui_aspect', '%g' % (16 / 9 if s['ui_aspect'] != 'off' else 0))


# ---- the steps ----------------------------------------------------------------------------------

def check_mac():
    if sys.platform != 'darwin':
        raise Failure('This setup is for macOS. On Windows, see the README.')
    if platform.machine() != 'arm64':
        raise Failure('This needs a Mac with Apple silicon (M1 or later).')
    major = int(platform.mac_ver()[0].split('.')[0] or 0)
    if major and major < 12:
        raise Failure('This needs macOS 12 (Monterey) or later.')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise Failure("Xcode's command line tools are missing. Run: xcode-select --install, then setup again.")
    free = shutil.disk_usage(ROOT).free
    if free < 3 << 30:
        raise Failure('Setup needs about 3 GB free space; this disk has %.1f GB.' % (free / (1 << 30)))


def venv_python():
    return os.path.join(VENV, 'bin', 'python3')


def ensure_python():
    py = venv_python()
    ok = os.path.exists(py) and subprocess.run([py, '-c', 'import capstone, pefile'], capture_output=True).returncode == 0
    if ok:
        return py
    if not os.path.exists(py):
        run(['/usr/bin/python3' if os.path.exists('/usr/bin/python3') else sys.executable, '-m', 'venv', VENV])
    run([py, '-m', 'pip', 'install', '--disable-pip-version-check', '-q'] + PACKAGES)
    return py


def build_libraries():
    total = sum(sum(len(g['sources']) for g in thirdparty.manifest(n)['groups']) for n in thirdparty.NAMES)
    done = [0]

    def one(name, n, of):
        done[0] += 1
        progress(done[0] / total, name)
    for name in thirdparty.NAMES:
        before = done[0]
        thirdparty.build(name, one)
        if done[0] == before:  # already built
            done[0] += sum(len(g['sources']) for g in thirdparty.manifest(name)['groups'])
            progress(done[0] / total, name)


def ensure_identity():
    """This Mac's own code-signing certificate, so macOS knows each rebuild as the same app and
    keeps the keychain's "Always Allow" for the saved password (README)."""
    if subprocess.run(['security', 'find-certificate', '-c', IDENTITY], capture_output=True).returncode == 0:
        return
    work = os.path.join(ROOT, 'build', 'signing')
    os.makedirs(work, exist_ok=True)
    cnf = os.path.join(work, 'cs.cnf')
    with open(cnf, 'w') as f:
        f.write('[req]\ndistinguished_name = dn\nx509_extensions = ext\nprompt = no\n[dn]\nCN = %s\n'
                '[ext]\nbasicConstraints = critical, CA:false\nkeyUsage = critical, digitalSignature\n'
                'extendedKeyUsage = critical, codeSigning\n' % IDENTITY)
    key, cert, p12 = (os.path.join(work, n) for n in ('key.pem', 'cert.pem', 'cs.p12'))
    try:
        run(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
             '-days', '3650', '-config', cnf])
        run(['/usr/bin/openssl', 'pkcs12', '-export', '-inkey', key, '-in', cert, '-name', IDENTITY, '-out', p12,
             '-passout', 'pass:x'])
        run(['security', 'import', p12, '-k', os.path.expanduser('~/Library/Keychains/login.keychain-db'),
             '-P', 'x', '-T', '/usr/bin/codesign'])
    except Failure as e:
        log('signing certificate: %s' % e)
        note('Could not make a signing certificate; the app is signed ad hoc instead.')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def app_options(s):
    """build_posix.py app's options for the settings (the app's Info.plist defaults)."""
    return (['--server', s['server'], '--resolution', '%dx%d' % s['resolution'],
             '--menu-resolution', '%dx%d' % s['menu_resolution'], '--window-mode', str(s['window_mode']),
             '--ui-aspect', s['ui_aspect']] + (['--dats', s['dats']] if s['dats'] else []))


def build_app(py, game, s):
    """build_posix.py prepare and app; with FFXI_PROGRESS set it marks its phases (@phase name) and
    the compile's progress (@progress done total), shown here as steps."""
    def watch(line):
        if line.startswith('@phase ') and line.split()[1] in PHASES:
            step(PHASES[line.split()[1]])
        elif line.startswith('@progress '):
            done, total = map(int, line.split()[1:3])
            progress(done / total if total else 1.0, '%d of %d files' % (done, total))
    env = dict(os.environ, FFXI_PROGRESS='1')
    step(PHASES['translate'])
    run([py, os.path.join('tools', 'build_posix.py'), 'prepare', '--game', game], env=env)
    run([py, os.path.join('tools', 'build_posix.py'), 'app', '--game', game] + app_options(s), progress=watch,
        env=env)
    return os.path.join(ROOT, 'build', APP_NAME + '.app')


def install(app, where):
    targets = [where] if where else ['/Applications', os.path.expanduser('~/Applications')]
    for d in targets:
        try:
            os.makedirs(d, exist_ok=True)
            dest = os.path.join(d, APP_NAME + '.app')
            if os.path.exists(dest):
                shutil.rmtree(dest)
            # ditto keeps the signature and extended attributes intact
            subprocess.run(['ditto', app, dest], check=True, capture_output=True)
            return dest
        except (OSError, subprocess.CalledProcessError) as e:
            log('install to %s: %s' % (d, e))
    raise Failure('Could not copy the app to %s.' % ' or '.join(targets))


def fetch_addons():
    """Ashita's and Windower's addons, libraries and resources into the data folder (the addon host
    runs them; they aren't ours to ship). Not fatal: the game runs without them."""
    note('Addons: fetching Ashita and Windower addons and libraries into %s' % user_path(DATA_DIR))
    try:
        run([sys.executable, os.path.join(HERE, 'addons_fetch.py'), '--data-dir', DATA_DIR])
    except Exception as e:  # noqa: BLE001 - offline, or GitHub unreachable
        note('Addons: not fetched (%s); run tools/addons_fetch.py later' % e)


def main():
    ap = argparse.ArgumentParser(description='Set up FINAL FANTASY XI on this Mac.')
    ap.add_argument('--game', help='the FINAL FANTASY XI folder (found automatically when left out)')
    ap.add_argument('--install-dir', help='where the app goes (default: /Applications, else ~/Applications)')
    ap.add_argument('--no-install', action='store_true', help='leave the app in build/')
    ap.add_argument('--find', action='store_true', help='list the game folders found on this Mac, and stop')
    ap.add_argument('--open', action='store_true', help='start the game when done')
    ap.add_argument('--server', help='the server to sign in to (asked when left out; 127.0.0.1 by default)')
    ap.add_argument('--resolution', help='the window, as WxH (asked when left out; 1920x1080 by default)')
    ap.add_argument('--window-mode', type=int, choices=[0, 1, 2, 3],
                    help='0 full screen, 1 windowed, 2 borderless, 3 borderless full screen (asked when left out)')
    ap.add_argument('--dats', help='a folder of DAT overlays, or none (asked when left out; none by default)')
    ap.add_argument('--settings-only', action='store_true',
                    help='ask the questions, print what the build would get, and stop (nothing built or saved)')
    ap.add_argument('--no-open', action='store_true', help='do not start the game, even with --open')
    ap.add_argument('--no-addons', action='store_true',
                    help="don't fetch Ashita's and Windower's addons and libraries (tools/addons_fetch.py)")
    a = ap.parse_args()
    if a.find:
        for c in candidates():
            print('%s  %s' % (c['build'] or 'unsupported', c['path']))
        return 0
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w') as f:
        f.write('FFXI setup, %s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
    print('Setting up FINAL FANTASY XI. The first time takes a few minutes.')
    try:
        step(0)
        check_mac()

        step(1)
        game = pick_game(a.game)
        if not game['build']:
            raise Failure('The game at %s is a version this setup does not support yet (FFXiMain.dll %s...). '
                          'Supported: %s. Run the install command again later for a newer setup, or ask your '
                          'server which client version it uses.'
                          % (game['path'], game['sha256'][:12], ', '.join(buildinfo.known())))
        note('%s (version %s)' % (game['path'], game['build']))

        step(2)
        settings = choose_settings(a)
        if a.settings_only:
            print('\n  build_posix.py app --game %s %s' % (shlex.quote(game['path']),
                                                         ' '.join(shlex.quote(o) for o in app_options(settings))))
            print('  saved files (%s): %s' % (DATA_DIR, 'would be updated' if os.path.isdir(DATA_DIR)
                                              else 'none yet; the app writes them on its first run'))
            return 0

        step(3)
        py = ensure_python()

        step(4)
        build_libraries()
        ensure_identity()

        app = build_app(py, game['path'], settings)

        step(8)
        dest = app if a.no_install else install(app, a.install_dir and user_path(a.install_dir))
        save_settings(settings)
        if not a.no_addons:
            fetch_addons()
    except Failure as e:
        print('\n  FAILED: %s' % e)
        return 1
    except KeyboardInterrupt:
        print('\n  Stopped.')
        return 130
    print('\nDone. %s is in %s. When it opens, sign in to %s (Settings changes the server).'
          % (APP_NAME, os.path.dirname(dest), settings['server']))
    if a.open and not a.no_open:
        subprocess.run(['open', dest])
    return 0


if __name__ == '__main__':
    sys.exit(main())
