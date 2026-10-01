"""Sign in to a Garlemald server and start the recompiled FINAL FANTASY XIV client.

  python3 tools/xiv_login.py [--server http://127.0.0.1:54993] [--lobby 127.0.0.1] [--user NAME]
                             [--signup] [--game <FINAL FANTASY XIV folder>] [-- extra xivboot args]

The same sign-in Garlemald Client makes (client/src/login): POST the account to the server's
web front end, which answers with a redirect to ffxiv://login_success?sessionId=<56 hex>. That
session id becomes the game's launch argument (tools/xiv_args.py, keyed to the tick count the host
starts the game's clock at), and the lobby host name goes where the client patches it. The
password is asked for on the terminal (or XIV_PASSWORD) and sent only to --server.
"""
import argparse
import getpass
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
try:
    import xiv_args  # noqa: E402
except ImportError:
    # pycryptodome (Blowfish) lives in the project's .venv: run there instead
    venv = os.path.join(ROOT, '.venv', 'bin', 'python')
    if os.path.exists(venv) and os.path.realpath(sys.executable) != os.path.realpath(venv):
        os.execv(venv, [venv] + sys.argv)
    raise SystemExit('pycryptodome is missing: uv pip install --python .venv/bin/python pycryptodome')

DEFAULT_GAME = os.path.expanduser(
    '~/Projects/FFXIV1_0/installer/target/prefix/drive_c/Program Files (x86)/SquareEnix/FINAL FANTASY XIV')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # the redirect target (ffxiv://...) is the answer, not a page to fetch


def post(url, fields):
    data = urllib.parse.urlencode(fields).encode()
    opener = urllib.request.build_opener(NoRedirect)
    try:
        r = opener.open(urllib.request.Request(url, data=data, method='POST'), timeout=15)
        return r.status, r.headers.get('Location', ''), r.read().decode(errors='replace')
    except urllib.error.HTTPError as e:  # 3xx arrive here once redirects are not followed
        return e.code, e.headers.get('Location', ''), e.read().decode(errors='replace')


def sign_in(server, user, password, signup=False):
    if signup:
        status, loc, _ = post(server.rstrip('/') + '/signup', {'username': user, 'password': password, 'confirm': password})
        m = re.search(r'sessionId=([0-9A-Fa-f]{56})', loc)
        if m:
            return m.group(1)
        raise SystemExit('sign-up refused: %s' % (urllib.parse.unquote(loc) or status))
    status, loc, _ = post(server.rstrip('/') + '/login', {'username': user, 'password': password})
    m = re.search(r'sessionId=([0-9A-Fa-f]{56})', loc)
    if not m:
        err = re.search(r'[?&]error=([^&]*)', loc)
        raise SystemExit('sign-in refused: %s' % (urllib.parse.unquote_plus(err.group(1)) if err else loc or status))
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='http://127.0.0.1:54993', help="the server's web front end")
    ap.add_argument('--lobby', default='127.0.0.1', help="the lobby server's host name (19 characters at most)")
    ap.add_argument('--user')
    ap.add_argument('--signup', action='store_true', help='create the account first')
    ap.add_argument('--game', default=DEFAULT_GAME, help='the FINAL FANTASY XIV folder')
    ap.add_argument('--tick', type=lambda s: int(s, 0), default=xiv_args.DEFAULT_TICK)
    ap.add_argument('--print', action='store_true', help='print the session id and argument; do not start the game')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    user = a.user or input('Username: ')
    password = os.environ.get('XIV_PASSWORD') or getpass.getpass('Password: ')
    session = sign_in(a.server, user, password, a.signup)
    arg = xiv_args.build(session, a.tick)
    print('signed in as %s (session %s...)' % (user, session[:8]))
    if a.print:
        print(arg)
        return
    env = dict(os.environ, XIV_ARGS=arg, XIV_TICK=hex(a.tick), XIV_LOBBY=a.lobby)
    exe = os.path.join(a.game, 'ffxivgame.exe')
    os.execve(os.path.join(ROOT, 'build', 'xivboot'), ['xivboot', exe, a.game] + a.rest, env)


if __name__ == '__main__':
    main()
