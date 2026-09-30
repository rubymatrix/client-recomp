"""Which retail build this checkout is working on.

meta/builds.json lists every build the recompiler knows. tools/prepare.py matches the install's
FFXiMain.dll against it and records the choice in generated/build.json; everything after that
(build.py, install.py) reads the choice from there, so nothing depends on the install again.
"""
import hashlib
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METADIR = os.path.join(ROOT, 'meta')
BUILDS = os.path.join(METADIR, 'builds.json')
CHOSEN = os.path.join(ROOT, 'generated', 'build.json')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def known():
    with open(BUILDS) as f:
        return json.load(f)['builds']


def match(ffximain_sha):
    """The build label whose FFXiMain.dll has this hash, or None."""
    for label, b in known().items():
        if b['FFXiMain.dll']['sha256'] == ffximain_sha:
            return label
    return None


def record(label, game):
    with open(CHOSEN, 'w') as f:
        json.dump({'build': label, 'game': game}, f, indent=2)
        f.write('\n')


def current(required=True):
    """{'build', 'game', 'ffximain_meta', 'ffxi_meta', 'ffximain_sha', 'ffxi_sha', 'addresses', 'hooks', 'crt',
    'modern', 'modern_keys'}, ('modern_keys': every name a build's modern section has, so build.h
    names them all whichever this build is)
    or None if tools/prepare.py has not run and not required."""
    try:
        with open(CHOSEN) as f:
            chosen = json.load(f)
    except OSError:
        if not required:
            return None
        raise SystemExit('%s missing: run tools/prepare.py first' % CHOSEN)
    b = known()[chosen['build']]
    return {
        'build': chosen['build'],
        'game': chosen['game'],
        'ffximain_meta': os.path.join(METADIR, b['FFXiMain.dll']['meta']),
        'ffxi_meta': os.path.join(METADIR, b['FFXi.dll']['meta']),
        'ffximain_sha': b['FFXiMain.dll']['sha256'],
        'ffxi_sha': b['FFXi.dll']['sha256'],
        'version': b['version'],
        'addresses': b['addresses'],
        'hooks': b.get('hooks', {}),
        'wraps': b.get('wraps', {}),
        'patches': b.get('patches', {}),
        'crt': b['crt'],
        'modern': b.get('modern', {}),
        'modern_keys': sorted({k for other in known().values() for k in other.get('modern', {})}),
    }
