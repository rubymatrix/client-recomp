"""Sign in to a Garlemald or Project Meteor server and start the recompiled FINAL FANTASY XIV client.

  python3 tools/xiv_login.py [--server http://127.0.0.1:54993] [--lobby 127.0.0.1] [--user NAME]
                             [--signup] [--meteor] [--game <FINAL FANTASY XIV folder>] [-- extra xivboot args]

--bahamut signs in through Bahamut's JSON auth API (POST /api/v1/sessions on the auth service,
e.g. --server http://127.0.0.1:8080); the session id it returns is the same 56 hex digits.

--meteor signs in through Project Meteor's PHP pages instead (--server is then the site root,
e.g. http://127.0.0.1:8000): POST login/ answers with a page holding <x-sqexauth sid="<56 hex>">,
and sign-up posts login_su/create_user.php.

The same sign-in Garlemald Client makes (client/src/login): POST the account to the server's
web front end, which answers with a redirect to ffxiv://login_success?sessionId=<56 hex>. That
session id becomes the game's launch argument (tools/xiv_args.py, keyed to the tick count the host
starts the game's clock at), and the lobby host name goes where the client patches it. The
password is asked for on the terminal (or XIV_PASSWORD) and sent only to --server.
"""
import argparse
import getpass
import json
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

# the workspace's retail install, beside this checkout
DEFAULT_GAME = os.path.join(os.path.dirname(ROOT), 'retail', 'FINAL FANTASY XIV')


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


def post_json(url, fields):
    req = urllib.request.Request(url, data=json.dumps(fields).encode(), method='POST',
                                 headers={'Content-Type': 'application/json'})
    try:
        r = urllib.request.urlopen(req, timeout=15)
        return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b'{}')
        except ValueError:
            return e.code, {}


def sign_in_bahamut(server, user, password, signup=False):
    base = server.rstrip('/') + '/api/v1'
    if signup:
        status, body = post_json(base + '/accounts', {'username': user, 'password': password})
        if status >= 300:
            raise SystemExit('sign-up refused: %s %s' % (status, body))
    status, body = post_json(base + '/sessions', {'username': user, 'password': password})
    sid = body.get('session_id', '')
    if status != 200 or not re.fullmatch(r'[0-9A-Fa-f]{56}', sid):
        raise SystemExit('sign-in refused: %s %s' % (status, body))
    return sid


def sign_in_meteor(server, user, password, signup=False):
    base = server.rstrip('/')
    if signup:
        status, loc, body = post(base + '/login_su/create_user.php', {
            'username': user, 'password': password, 'repeatPassword': password,
            'email': '%s@localhost.test' % user, 'createUser': '1'})
        if 'create_user_success' not in loc:
            err = re.search(r'class="errorMessage">([^<]*)<', body)
            raise SystemExit('sign-up refused: %s' % (err.group(1).strip() if err and err.group(1).strip() else status))
    status, _, body = post(base + '/login/', {'username': user, 'password': password, 'login': '1'})
    m = re.search(r'<x-sqexauth sid="([0-9A-Fa-f]{56})"', body)
    if not m:
        err = re.search(r'class=errorText>([^<]*)<', body)
        raise SystemExit('sign-in refused: %s' % (err.group(1).strip() if err and err.group(1).strip() else status))
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='http://127.0.0.1:54993', help="the server's web front end")
    ap.add_argument('--lobby', default='127.0.0.1', help="the lobby server's host name (19 characters at most)")
    ap.add_argument('--user')
    ap.add_argument('--signup', action='store_true', help='create the account first')
    ap.add_argument('--bahamut', action='store_true', help="sign in through Bahamut's auth API (--server is then its auth URL, e.g. http://127.0.0.1:8080)")
    ap.add_argument('--meteor', action='store_true', help="sign in through Project Meteor's PHP login pages")
    ap.add_argument('--game', default=DEFAULT_GAME, help='the FINAL FANTASY XIV folder')
    ap.add_argument('--tick', type=lambda s: int(s, 0), default=xiv_args.DEFAULT_TICK)
    ap.add_argument('--print', action='store_true', help='print the session id and argument; do not start the game')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    user = a.user or input('Username: ')
    password = os.environ.get('XIV_PASSWORD') or getpass.getpass('Password: ')
    session = (sign_in_bahamut if a.bahamut else sign_in_meteor if a.meteor else sign_in)(a.server, user, password, a.signup)
    arg = xiv_args.build(session, a.tick)
    print('signed in as %s (session %s...)' % (user, session[:8]))
    if a.print:
        print(arg)
        return
    env = dict(os.environ, XIV_ARGS=arg, XIV_TICK=hex(a.tick), XIV_LOBBY=a.lobby)
    exe = os.path.join(a.game, 'ffxivgame.exe')
    # the app bundle's copy when there is one (macOS Game Mode wants a bundled game)
    boot = os.path.join(ROOT, 'build', 'FINAL FANTASY XIV.app', 'Contents', 'MacOS', 'xivboot')
    if not os.path.exists(boot):
        boot = os.path.join(ROOT, 'build', 'xivboot')
    os.execve(boot, ['xivboot', exe, a.game] + a.rest, env)


if __name__ == '__main__':
    main()
